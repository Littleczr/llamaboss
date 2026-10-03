// chat_client.cpp
//
// Communicates with an OpenAI-compatible chat-completions endpoint:
//   POST <baseUrl><chatPath>   (default: /v1/chat/completions, SSE streaming)
//
// The endpoint may be the local llama-server (plain http, no auth) or
// a remote provider (TLS + auth header). Both are described by an
// InferenceTarget; the SSE response handling is identical for both
// because remote providers used here speak the OpenAI streaming shape.
//
// Response format (Server-Sent Events):
//   data: {"choices":[{"delta":{"content":"hello"}}]}
//   data: {"choices":[{"delta":{"content":" world"},"finish_reason":"stop"}]}
//   data: [DONE]

#include "chat_client.h"
#include "openai_responses.h"

// Poco headers for HTTP communication
#include <Poco/URI.h>
#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPSClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/Net/NetException.h>
#include <Poco/StreamCopier.h>
#include <Poco/Timespan.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Stringifier.h>
#include <Poco/JSON/JSONException.h>
#include <Poco/Exception.h>

#include <memory>
#include <sstream>
#include <chrono>
#include <cstddef>
#include <string>
#include <algorithm>
#include <istream>
#include "ui_event_post.h"
#include "lb_ssl.h"
#include "repetition_guard.h"

// ═══════════════════════════════════════════════════════════════════
//  Stream limits
// ═══════════════════════════════════════════════════════════════════
// Everything below arrives from a remote endpoint we do not control.
// A malfunctioning or hostile server could previously hand us an
// unterminated SSE line, an endless tool-argument fragment, or a
// multi-gigabyte error body, all of which grew a std::string until
// the process died. These are deliberately generous - the point is
// that a ceiling EXISTS, not that it is tight.
namespace {

// One SSE line. Generous because a whole generated image arrives on
// a single line as a base64 data URL, and a provider may pack
// several into one chunk.
constexpr std::size_t kMaxSseLineBytes = 64ull * 1024 * 1024;

// Total bytes read off one response. Bounds fullReply transitively:
// a reply cannot exceed what we were willing to read.
constexpr std::size_t kMaxStreamBytes = 512ull * 1024 * 1024;

// Non-200 error body. Only ever shown in a message box.
constexpr std::size_t kMaxErrorBodyBytes = 64ull * 1024;

// Accumulated function-call arguments, per tool-call slot. Real tool
// arguments are hundreds of bytes; anything past this is a stuck
// generation, not a payload.
constexpr std::size_t kMaxToolArgBytes = lb_responses::kMaxToolArgumentBytes;

// Generated images: per data URL, and across the whole response.
constexpr std::size_t kMaxImageDataUrlBytes = 48ull * 1024 * 1024;
constexpr std::size_t kMaxTotalImageBytes   = 192ull * 1024 * 1024;

// Delta coalescing (see queueDelta in Entry). Flush on whichever
// comes first.
constexpr std::size_t kDeltaFlushBytes = 4096;
constexpr int         kDeltaFlushMs    = 12;

// ── Transport timeouts ───────────────────────────────────────────
// Poco's HTTPClientSession::setTimeout sets the connect, send AND
// receive timeouts to one value, so a single number cannot serve both
// ends of this request: short enough that a dead host fails fast, and
// long enough that a legitimate prompt-processing phase survives.
// These were one flat 120s, which lost the second half.
//
// llama-server emits NOTHING while it prefills a prompt — no progress
// event, no SSE keepalive — so the socket is legitimately silent for
// however long prefill takes. On a large context that can exceed two
// minutes, and the receive timeout then aborted a perfectly healthy
// generation as "stream error". The wait that needs the headroom is
// specifically the FIRST byte.
//
// Once deltas are flowing they arrive continuously, so the timeout is
// tightened after the first one: a mid-stream stall is a real fault
// and should surface promptly instead of parking a worker thread for
// the whole prefill window. That also bounds the drain phase after
// [DONE] against a server that holds the socket open.
//
// None of these replace cancellation — Stop aborts the socket through
// ChatRequestControl no matter which wait is pending. kFirstByte is
// the knob to raise for very large contexts on slow hardware.
constexpr long kConnectTimeoutSeconds    =  30;
constexpr long kFirstByteTimeoutSeconds  = 900;
constexpr long kStreamIdleTimeoutSeconds = 120;

// Set only the receive timeout on a session whose socket is already
// connected, leaving the connect/send values from setTimeout() alone.
// Poco throws if the socket does not exist yet; a failure here just
// means the stream keeps its previous timeout, which is never worth
// failing a request over.
void SetReceiveTimeoutSeconds(Poco::Net::HTTPClientSession& session,
                              long seconds)
{
    try {
        session.socket().setReceiveTimeout(Poco::Timespan(seconds, 0));
    } catch (...) {
        // Keep whatever the session was already using.
    }
}

// Bounded replacement for std::getline.
//
// Returns false only at a clean EOF with nothing buffered. Sets
// `overflow` when the line hit maxBytes without a newline - the
// caller treats that as a protocol error rather than growing the
// buffer without limit.
//
// Reads through the streambuf rather than istream::get() so an empty
// line (which SSE uses as its event boundary) does not trip failbit,
// and so the per-byte cost stays a pointer bump in the common case.
bool ReadLineBounded(std::istream& in, std::string& out,
                     std::size_t maxBytes, bool& overflow)
{
    out.clear();
    overflow = false;

    std::streambuf* sb = in.rdbuf();
    if (!sb) return false;

    bool any = false;
    for (;;) {
        const int c = sb->sbumpc();
        if (c == std::char_traits<char>::eof()) {
            in.setstate(std::ios::eofbit);
            return any;
        }
        any = true;
        if (c == '\n') return true;
        if (out.size() >= maxBytes) { overflow = true; return true; }
        out.push_back(static_cast<char>(c));
    }
}

// Bounded body read, for error responses we only intend to display.
std::string ReadBodyBounded(std::istream& in, std::size_t maxBytes)
{
    std::string out;
    char buf[4096];

    while (out.size() < maxBytes && in.good()) {
        const std::size_t want =
            (std::min)(sizeof(buf), maxBytes - out.size());
        in.read(buf, static_cast<std::streamsize>(want));
        const std::streamsize n = in.gcount();
        if (n <= 0) break;
        out.append(buf, static_cast<std::size_t>(n));
    }

    if (out.size() >= maxBytes)
        out += " ... (truncated)";
    return out;
}

// Length of the longest prefix of s that ends on a complete UTF-8
// sequence. Used so a coalesced flush never cuts a multi-byte
// character in half - llama-server happily splits a CJK glyph or an
// emoji across two deltas, and each half converted separately through
// wxString::FromUTF8 renders as a replacement character.
std::size_t Utf8SafeCut(const std::string& s)
{
    if (s.empty()) return 0;

    std::size_t i = s.size();
    int walked = 0;
    while (i > 0 && walked < 4) {
        const unsigned char c = static_cast<unsigned char>(s[i - 1]);
        if ((c & 0xC0) == 0x80) { --i; ++walked; continue; }  // continuation

        std::size_t need = 0;
        if      (c < 0x80)          need = 1;
        else if ((c & 0xE0) == 0xC0) need = 2;
        else if ((c & 0xF0) == 0xE0) need = 3;
        else if ((c & 0xF8) == 0xF0) need = 4;
        else return s.size();   // invalid lead byte; pass it through

        const std::size_t have = s.size() - (i - 1);
        return (have >= need) ? s.size() : (i - 1);
    }
    return s.size();
}

} // namespace

// Define custom events
wxDEFINE_EVENT(wxEVT_ASSISTANT_DELTA, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_ASSISTANT_COMPLETE, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_ASSISTANT_ERROR, wxCommandEvent);

// ═══════════════════════════════════════════════════════════════════
// ChatRequestControl Implementation
// ═══════════════════════════════════════════════════════════════════

void ChatRequestControl::Cancel()
{
    // Flag first, abort second.  Every error path in
    // ChatWorkerThread::Entry() is guarded on IsCancelled(), so setting
    // the flag before tearing the socket down is what keeps a user Stop
    // from surfacing as an "API Error" / "Stream ended unexpectedly"
    // message.
    m_cancelled.store(true, std::memory_order_release);

    // Copy the pointer out under the lock, abort outside it.  Holding
    // our own shared_ptr means the session cannot be destroyed mid-abort
    // even if the worker reaches DetachSession() first.
    std::shared_ptr<Poco::Net::HTTPClientSession> session;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        session = m_session;
    }
    if (!session) return;

    // abort() shuts the socket down from this thread, which is what
    // unblocks the worker's std::getline().  It throws when the session
    // never actually connected (Stop pressed during DNS/connect, where
    // Poco's socket has no descriptor to shut down).  Nothing to clean up
    // in that case -- the worker unwinds through its own handlers and the
    // cancelled flag keeps it quiet -- so swallow it.
    try { session->abort(); } catch (...) {}
}

bool ChatRequestControl::AttachSession(
    const std::shared_ptr<Poco::Net::HTTPClientSession>& session)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    // Lost the race: Cancel() ran between this worker being started and
    // its session being built, so there was no session to abort and the
    // abort has already happened (to nothing).  Refuse to register;
    // the worker abandons the request instead of entering a blocking
    // read that no longer has an interrupter.
    if (m_cancelled.load(std::memory_order_acquire)) return false;

    m_session = session;
    return true;
}

void ChatRequestControl::DetachSession()
{
    // Release outside the lock so the session destructor (socket close)
    // never runs while m_mutex is held.
    std::shared_ptr<Poco::Net::HTTPClientSession> released;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        released.swap(m_session);
    }
}

// ═══════════════════════════════════════════════════════════════════
// ChatWorkerThread Implementation
// ═══════════════════════════════════════════════════════════════════

ChatWorkerThread::ChatWorkerThread(wxEvtHandler* eventHandler,
    InferenceTarget target,
    std::string requestBody,
    std::shared_ptr<ChatRequestControl> control,
    std::weak_ptr<std::atomic<bool>> aliveToken,
    unsigned long generationId)
    : wxThread(wxTHREAD_DETACHED)
    , m_eventHandler(eventHandler)
    , m_target(std::move(target))
    , m_requestBody(std::move(requestBody))
    , m_control(std::move(control))
    , m_aliveToken(aliveToken)
    , m_generationId(generationId)
{
}

bool ChatWorkerThread::SafeQueueEvent(wxCommandEvent* event)
{
    event->SetExtraLong(m_generationId);
    return LbQueueEventIfAlive(m_eventHandler, m_aliveToken, event);
}

wxThread::ExitCode ChatWorkerThread::Entry()
{
    std::string fullReply;
    const bool responsesApi = m_target.responsesApi ||
        lb_responses::IsResponsesPath(m_target.chatPath);
    lb_responses::ChatStream responsesStream;
    // Phase 2: function calls come back on the completion event in the
    // Chat Completions shape the rest of the pipeline already consumes,
    // plus the verbatim output array for reasoning replay.
    std::string responsesToolCallsJson;
    std::string responsesOutputJson;

    // ── Reasoning surfacing state ────────────────────────────────
    // With --jinja, llama-server extracts model thinking into
    // delta.reasoning_content instead of inline <think> tags
    // (DeepSeek R1 distills, Qwen3, GPT-OSS templates); some
    // OpenAI-compatible remotes use delta.reasoning.  Those deltas
    // are re-wrapped below as inline <think>…</think> so the
    // existing ChatDisplay probe/collapse/replay machinery and the
    // conversation file format work unchanged.  Reasoning always
    // precedes visible content, so the open tag lands at byte 0 —
    // exactly what the display's probe phase detects.  True while
    // an open tag has been emitted without its close.
    bool inReasoningBlock = false;

    // ── Phase 3c-ii: structured tool_calls accumulator ──────────
    // OpenAI streaming format delivers tool_calls in fragments
    // across many SSE events.  Each delta.tool_calls entry carries
    // an `index` — fragments accumulate per-index until the stream
    // ends.  `id` and `function.name` typically arrive on the
    // first fragment for an index; `function.arguments` arrives
    // char-by-char across many.  We slot fragments into this
    // vector and serialize the result on stream end.
    //
    // Empty if the model emitted no tool_calls (common case for
    // chat-only turns or XML-protocol models).  When non-empty,
    // serialized as a JSON array string and attached to the
    // completion event as wxClientData.
    struct ToolCallAcc {
        std::string id;
        std::string type;     // "function" — kept for round-trip fidelity
        std::string name;
        std::string arguments;
        // Repetition-guard bookkeeping for `arguments` (see
        // checkToolArgRepetition below).
        std::size_t repetitionCheckedAt = 0;
    };
    std::vector<ToolCallAcc> toolCalls;

    // ── Generated image accumulator ──────────────────────────────
    // Image-output models (requested via "modalities": ["image",
    // "text"]) return generated images on the message's `images`
    // field.  On the streaming path OpenRouter delivers them as
    // delta.images — each entry {"type":"image_url","image_url":
    // {"url":"data:image/png;base64,..."}} — typically in one chunk
    // near the end of the stream, since there is no partial-image
    // streaming over chat completions.  Accumulated verbatim as data
    // URLs; decode + disk persistence happen on the UI thread (the
    // save location is keyed on the conversation file path, which
    // the worker doesn't know).  Exact-duplicate URLs are dropped in
    // case a provider repeats the array across chunks.
    std::vector<std::string> imageDataUrls;

    // Running total so a provider that streams image after image
    // cannot walk us out of memory one "valid" entry at a time.
    std::size_t imageBytes = 0;

    auto collectImageUrl = [&](const std::string& url) {
        if (url.empty()) return;
        if (url.size() > kMaxImageDataUrlBytes) return;
        if (imageBytes + url.size() > kMaxTotalImageBytes) return;
        for (const auto& existing : imageDataUrls)
            if (existing == url) return;
        imageBytes += url.size();
        imageDataUrls.push_back(url);
    };

    // ── Context meter: exact token usage from the stream ─────────
    // Latest `usage` object seen on any SSE chunk.  llama-server puts
    // usage on the final chunk (the one carrying finish_reason);
    // OpenAI-style providers send it on a trailing usage-only chunk
    // (choices: []) after finish_reason.  -1 = not reported.
    long usagePromptTokens     = -1;
    long usageCompletionTokens = -1;

    // ── Turn stats: timings + detailed usage (turn_stats.h) ──────
    // Wall-clock marks are milliseconds from the moment the request is
    // sent.  markToken() is called for every non-empty reasoning,
    // content, tool-call or image delta; `visible` distinguishes the
    // answer/tool output from thinking so hidden-reasoning providers
    // don't get an inflated tok/s.
    TurnStats stats;
    stats.modelId = m_target.modelId;
    stats.modelRemote = m_target.managed ? 0 : 1;
    std::chrono::steady_clock::time_point requestSentAt{};
    bool requestSent = false;
    auto msSinceSend = [&]() -> double {
        if (!requestSent) return -1;
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - requestSentAt).count();
    };
    auto markToken = [&](bool visible) {
        const double now = msSinceSend();
        if (now < 0) return;
        if (stats.firstTokenMs < 0) stats.firstTokenMs = now;
        if (visible && stats.firstContentMs < 0) stats.firstContentMs = now;
        stats.lastTokenMs = now;
    };
    // Numeric JSON field as double (llama-server mixes ints and floats).
    auto readNumber = [](const Poco::JSON::Object::Ptr& o, const char* key) -> double {
        if (!o || !o->has(key) || o->isNull(key)) return -1;
        try { return o->get(key).convert<double>(); } catch (...) { return -1; }
    };

    auto ensureToolCallSlot = [&](int idx) -> ToolCallAcc* {
        if (idx < 0) return nullptr;
        // The index comes off the wire.  Without a ceiling, a buggy or
        // hostile endpoint sending {"index": 2000000000} drives a
        // multi-GB resize below (each slot is four std::strings).
        // Real tool-call fan-out is single digits; anything past the
        // cap is a malformed fragment to drop, not a slot to allocate.
        constexpr int kMaxToolCallSlots = 128;
        if (idx >= kMaxToolCallSlots) return nullptr;
        if ((size_t)idx >= toolCalls.size()) {
            toolCalls.resize(idx + 1);
        }
        return &toolCalls[(size_t)idx];
    };

    auto isCancelled = [this]() { return m_control->IsCancelled(); };

    // ── Delta coalescing ─────────────────────────────────────────
    // The UI already batches on a 16 ms timer, but the worker used to
    // allocate one wxCommandEvent and run one UTF-8 -> UTF-16
    // conversion PER TOKEN, then the UI converted each one straight
    // back. A fast local model turns that into thousands of
    // allocations and conversions for a single answer.
    //
    // Buffer here instead and flush on 4 KiB or 12 ms, whichever
    // comes first - below the UI's 16 ms tick, so perceived streaming
    // smoothness is unchanged. The flush is held back to the last
    // complete UTF-8 boundary so a split multi-byte character is
    // rejoined rather than rendered as two replacement characters.
    std::string pendingDelta;
    auto lastFlush = std::chrono::steady_clock::now();

    auto flushDelta = [&](bool force) -> bool {
        if (pendingDelta.empty()) return true;

        const std::size_t cut =
            force ? pendingDelta.size() : Utf8SafeCut(pendingDelta);
        if (cut == 0) return true;   // nothing but a partial character yet

        wxCommandEvent* event = new wxCommandEvent(wxEVT_ASSISTANT_DELTA);
        event->SetString(wxString::FromUTF8(pendingDelta.data(), cut));
        pendingDelta.erase(0, cut);
        lastFlush = std::chrono::steady_clock::now();
        return SafeQueueEvent(event);
    };

    // ── Runaway-loop guard (repetition_guard.h) ──────────────────
    // Every ~512 bytes of new output, check whether the reply now ends in
    // the same block repeated back-to-back.  Callers append to fullReply
    // before queueDelta, so the check sees the latest text.
    //
    // Policy follows what is streaming right now: thinking is strict
    // (~1 KB of exact repetition), visible text lenient (32 KB) so
    // requested repetitive output -- identical CSV rows, fixtures -- is
    // not cut off.  When this trips, the read loop ends the reply as an
    // INTERRUPTED turn (stats.stoppedForRepetition): no tool calls are
    // serialized and the frame ends any agent loop instead of continuing.
    bool argOverflow = false;      // native tool arguments passed kMaxToolArgBytes
    // Set the payload flag at detection time: a terminal event can exit
    // the read loop before its next iteration checks for repetition.
    std::size_t repetitionCheckedAt = 0;
    // Append streamed text to fullReply in 512-byte steps, checking at
    // every boundary.  Checking only the end of a whole delta let one
    // large chunk hide a repeating body behind a short tail -- e.g. an
    // XML tool call's closing </args></tool_call>.  Callers pass
    // homogeneous text (all thinking or all visible), so the policy is
    // read from inReasoningBlock once per call.
    auto appendReply = [&](const std::string& text) {
        const repetition_guard::Policy pol = inReasoningBlock
            ? repetition_guard::ThinkingPolicy()
            : repetition_guard::OutputPolicy();
        std::size_t offset = 0;
        while (offset < text.size()) {
            if (stats.stoppedForRepetition) {
                fullReply.append(text, offset, std::string::npos);
                return;
            }
            const std::size_t sinceCheck = fullReply.size() - repetitionCheckedAt;
            const std::size_t untilCheck = sinceCheck >= 512 ? 0 : 512 - sinceCheck;
            const std::size_t count = (std::min)(untilCheck, text.size() - offset);
            fullReply.append(text, offset, count);
            offset += count;
            if (fullReply.size() - repetitionCheckedAt >= 512) {
                repetitionCheckedAt = fullReply.size();
                if (repetition_guard::EndsInRepetitionLoop(fullReply, pol))
                    stats.stoppedForRepetition = true;
            }
        }
    };
    // Native tool-call arguments stream separately from fullReply, so a
    // model looping while writing a file body through write/py would
    // otherwise never be seen.  Same lenient policy as visible text:
    // file contents may legitimately repeat.
    //
    // Append in 512-byte steps and check at every boundary, exactly like
    // the Responses path (openai_responses.h AppendArgumentSuffix).  A
    // provider that sends one large arguments chunk -- or the whole body
    // at once -- must not hide a repeating body behind the closing JSON
    // characters at the end of that chunk.
    auto appendToolArgs = [&](ToolCallAcc& slot, const std::string& text) {
        std::size_t offset = 0;
        while (offset < text.size()) {
            if (stats.stoppedForRepetition) {
                slot.arguments.append(text, offset, std::string::npos);
                return;
            }
            const std::size_t sinceCheck =
                slot.arguments.size() - slot.repetitionCheckedAt;
            const std::size_t untilCheck = sinceCheck >= 512 ? 0 : 512 - sinceCheck;
            const std::size_t count =
                (std::min)(untilCheck, text.size() - offset);
            slot.arguments.append(text, offset, count);
            offset += count;
            if (slot.arguments.size() - slot.repetitionCheckedAt >= 512) {
                slot.repetitionCheckedAt = slot.arguments.size();
                if (repetition_guard::EndsInRepetitionLoop(
                        slot.arguments, repetition_guard::OutputPolicy()))
                    stats.stoppedForRepetition = true;
            }
        }
    };

    auto queueDelta = [&](const std::string& text) -> bool {
        if (text.empty()) return true;
        pendingDelta += text;

        if (pendingDelta.size() >= kDeltaFlushBytes)
            return flushDelta(false);

        const auto age = std::chrono::steady_clock::now() - lastFlush;
        if (age >= std::chrono::milliseconds(kDeltaFlushMs))
            return flushDelta(false);

        return true;
    };

    // Every terminal event must be preceded by a forced flush, or the
    // tail of the reply is queued behind an event the UI treats as
    // final - or never queued at all.
    auto postError = [&](const std::string& message) {
        flushDelta(true);
        wxCommandEvent* event = new wxCommandEvent(wxEVT_ASSISTANT_ERROR);
        event->SetString(wxString::FromUTF8(message));
        SafeQueueEvent(event);
    };

    // Bytes pulled off this response, for the overall budget.
    std::size_t streamBytes = 0;

    try {
        // Convert after the UI has finished message/attachment projection.
        // The original conversation remains unchanged. Other endpoints keep
        // their request bytes exactly as built by ChatHistory.
        if (responsesApi)
            m_requestBody = lb_responses::BuildChatRequest(m_requestBody);

        // ── Connect to the target's OpenAI-compatible endpoint ──
        // The path comes from the target so an Anthropic-native
        // adapter (future) can redirect to /v1/messages without
        // touching the transport. Port is resolved explicitly so an
        // authority-only URL ("https://host") still connects on 443.
        Poco::URI uri(m_target.baseUrl + m_target.chatPath);

        int port = uri.getPort();
        if (port == 0)
            port = m_target.useTls ? 443 : 80;

        // Shared rather than unique: ChatRequestControl holds a reference
        // for as long as the request is registered, so a Stop arriving
        // mid-teardown aborts a session that is guaranteed to still exist.
        std::shared_ptr<Poco::Net::HTTPClientSession> sess;
        if (m_target.useTls) {
            // SSL is initialized lazily and once per process; only the
            // TLS branch pays for it, so local-only users never load it.
            lb::EnsureSSLInitialized();
            sess = std::make_shared<Poco::Net::HTTPSClientSession>(
                uri.getHost(), port);
        } else {
            sess = std::make_shared<Poco::Net::HTTPClientSession>(
                uri.getHost(), port);
        }
        // Governs connect and send. The receive timeout is raised
        // separately once the socket exists (see below) — prefill can
        // legitimately take far longer than we want to spend waiting
        // on an unreachable host.
        sess->setTimeout(Poco::Timespan(kConnectTimeoutSeconds, 0));

        // ── Register the transport for cancellation ──────────────
        // From here until the scope guard fires, StopGeneration() can
        // abort this socket and break the blocking read below.  Before
        // this point there is nothing to abort, which is why Attach
        // reports a cancellation that already happened.
        if (!m_control->AttachSession(sess))
            return (ExitCode)0;

        struct SessionScope {
            ChatRequestControl* control;
            ~SessionScope() { if (control) control->DetachSession(); }
        } sessionScope{ m_control.get() };

        Poco::Net::HTTPRequest req(
            Poco::Net::HTTPRequest::HTTP_POST,
            uri.getPathAndQuery(),
            Poco::Net::HTTPMessage::HTTP_1_1
        );
        req.setContentType("application/json");
        if (responsesApi) req.set("Accept", "text/event-stream");
        req.setContentLength((long)m_requestBody.size());

        // Auth + provider-fixed headers. Both are empty for local
        // lanes, so this is a no-op there and the request is
        // byte-for-byte what it was before the target refactor.
        if (!m_target.authHeaderName.empty() &&
            !m_target.authHeaderValue.empty()) {
            req.set(m_target.authHeaderName, m_target.authHeaderValue);
        }
        for (const auto& h : m_target.extraHeaders) {
            if (!h.first.empty())
                req.set(h.first, h.second);
        }

        requestSentAt = std::chrono::steady_clock::now();
        requestSent = true;
        std::ostream& out = sess->sendRequest(req);
        out << m_requestBody;
        out.flush();

        // The socket is connected now, so its receive timeout can be
        // set independently of the connect timeout above. Everything
        // from here to the first delta is prompt processing, during
        // which a healthy server sends no bytes at all.
        SetReceiveTimeoutSeconds(*sess, kFirstByteTimeoutSeconds);

        Poco::Net::HTTPResponse resp;
        std::istream& in = sess->receiveResponse(resp);

        if (resp.getStatus() != Poco::Net::HTTPResponse::HTTP_OK) {
            const std::string err = ReadBodyBounded(in, kMaxErrorBodyBytes);

            wxCommandEvent* event = new wxCommandEvent(wxEVT_ASSISTANT_ERROR);
            event->SetString(wxString::FromUTF8(
                "API Error (" + std::to_string(resp.getStatus()) + "): "
                + resp.getReason() + " - " + err
            ));
            SafeQueueEvent(event);
            return (ExitCode)0;
        }

        if (responsesApi && resp.getContentType().find("text/event-stream") != 0) {
            postError("The Responses endpoint did not return a streaming response.");
            return (ExitCode)0;
        }

        // ── Parse SSE stream ─────────────────────────────────────
        // Each event is: "data: <json>\n\n"
        // Final event is: "data: [DONE]\n\n"
        //
        // Track whether the server actually ended the stream.  A dropped
        // socket / crashed llama-server can otherwise look like a clean EOF,
        // causing LlamaBoss to render a partial assistant answer as complete.
        bool sawTerminalEvent = false;
        bool sawAnySseData = false;
        std::string line;
        std::string responseEventData;

        // One parser for the whole stream, reset between chunks.
        // Constructing a Poco::JSON::Parser allocates its handler and
        // internal state; doing that per SSE event meant building and
        // tearing one down 100+ times a second at MTP generation speeds,
        // for the entire duration of every reply.  reset() returns it to
        // a fresh state far more cheaply than reconstruction.
        Poco::JSON::Parser sseParser;

        // The read loop gets its own guard: once sawTerminalEvent is
        // set, the only thing still being read is the optional trailing
        // usage chunk / [DONE] marker.  A transport exception in that
        // drain phase (e.g. a non-conforming server holding the socket
        // open until the receive timeout) must not demote an already
        // complete reply to a stream error.  Pre-terminal exceptions
        // are real failures and are rethrown to the handlers below.
        try {
        for (;;) {
            // Model stuck repeating itself: end the reply here.  It is
            // delivered through COMPLETE (so usage-less stats and the
            // partial text survive) but flagged stoppedForRepetition,
            // which the frame treats as an interrupted turn: no tool
            // dispatch, agent loop ended.  Leaving the loop drops the
            // connection, which makes llama-server stop generating too.
            if (stats.stoppedForRepetition) {
                sawTerminalEvent = true;
                break;
            }
            bool lineOverflow = false;
            if (!ReadLineBounded(in, line, kMaxSseLineBytes, lineOverflow))
                break;
            if (isCancelled()) break;

            streamBytes += line.size() + 1;

            if (lineOverflow) {
                postError("Stream aborted: the server sent a single line "
                          "larger than this client will buffer.");
                return (ExitCode)0;
            }
            if (streamBytes > kMaxStreamBytes) {
                postError("Stream aborted: response exceeded the maximum "
                          "size this client will accept.");
                return (ExitCode)0;
            }

            // Strip trailing \r
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            std::string data;
            if (responsesApi) {
                // Dispatch complete SSE records. Multiple data: lines are
                // joined by newlines; event:/id:/comments carry no JSON.
                if (line.empty()) {
                    if (responseEventData.empty()) continue;
                    data.swap(responseEventData);
                } else {
                    if (line.compare(0, 5, "data:") != 0) continue;
                    std::string part = line.substr(5);
                    if (!part.empty() && part.front() == ' ') part.erase(0, 1);
                    if (responseEventData.size() + part.size() + 1 > kMaxSseLineBytes) {
                        postError("Responses event exceeded the maximum buffered size.");
                        return (ExitCode)0;
                    }
                    if (!responseEventData.empty()) responseEventData += '\n';
                    responseEventData += part;
                    continue;
                }
            } else {
                // Preserve existing Chat Completions streaming behavior.
                if (line.empty()) continue;
                if (line.size() < 5 || line.compare(0, 5, "data:") != 0) continue;
                data = line.substr(5);
                if (!data.empty() && data.front() == ' ') data.erase(0, 1);
            }

            // End of stream marker
            if (data == "[DONE]") {
                // Responses must emit response.completed. A Chat Completions
                // marker alone cannot certify that an answer is complete.
                if (!responsesApi) sawTerminalEvent = true;
                break;
            }

            // First real data event: prefill is over and deltas now
            // arrive continuously, so drop back to the tighter
            // between-deltas timeout. This also bounds the post-[DONE]
            // drain phase described above.
            if (!sawAnySseData && !responsesApi)
                SetReceiveTimeoutSeconds(*sess, kStreamIdleTimeoutSeconds);

            sawAnySseData = true;
            if (stats.firstByteMs < 0) stats.firstByteMs = msSinceSend();

            try {
                sseParser.reset();
                auto obj = sseParser.parse(data)
                               .extract<Poco::JSON::Object::Ptr>();

                if (responsesApi) {
                    lb_responses::StreamUpdate update;
                    try {
                        update = responsesStream.Consume(obj);
                    } catch (const lb_responses::ToolArgumentRepetition&) {
                        // This can be raised by a delta OR the terminal
                        // snapshot. Mark the interruption before leaving
                        // the loop so partial calls cannot be dispatched.
                        stats.stoppedForRepetition = true;
                        sawTerminalEvent = true;
                        break;
                    }
                    if (!update.error.empty()) {
                        postError(update.error);
                        return (ExitCode)0;
                    }
                    if (!update.summary.empty()) markToken(false);
                    if (!update.text.empty())    markToken(true);
                    // Thinking and visible text are appended separately so
                    // each is checked under its own repetition policy.
                    if (!update.summary.empty()) {
                        std::string thinking;
                        if (!inReasoningBlock) {
                            thinking = "<think>";
                            inReasoningBlock = true;
                        }
                        thinking += update.summary;
                        appendReply(thinking);
                        if (!queueDelta(thinking)) return (ExitCode)0;
                    }
                    if (!update.text.empty()) {
                        std::string visible;
                        if (inReasoningBlock) {
                            visible = "</think>";
                            inReasoningBlock = false;
                        }
                        visible += update.text;
                        appendReply(visible);
                        if (!queueDelta(visible)) return (ExitCode)0;
                    }
                    if (update.completed) {
                        usagePromptTokens = update.inputTokens;
                        usageCompletionTokens = update.outputTokens;
                        stats.cachedPromptTokens = update.cachedInputTokens;
                        stats.reasoningTokens    = update.reasoningTokens;
                        responsesToolCallsJson = update.toolCallsJson;
                        responsesOutputJson = update.outputJson;
                        sawTerminalEvent = true;
                        break;
                    }
                    // Responses can announce created/in_progress long before
                    // the first visible token. Keep the reasoning allowance
                    // for this lane instead of switching to the short idle
                    // timeout on a lifecycle/keepalive event. Stop still aborts
                    // the socket through the existing cancellation control.
                    continue;
                }

                // Mid-stream error (OOM, context overflow, model unload, etc.)
                // llama-server emits `{"error": {"message": "..."}}` as a
                // regular SSE event on these — without this check the UI
                // would just stop receiving deltas and stay stuck in the
                // streaming state.
                if (obj->has("error")) {
                    std::string errMsg;
                    try {
                        auto errObj = obj->getObject("error");
                        if (errObj && errObj->has("message"))
                            errMsg = errObj->getValue<std::string>("message");
                    } catch (...) {
                        // "error" may be a plain string on some builds
                        try { errMsg = obj->getValue<std::string>("error"); }
                        catch (...) { /* unknown shape */ }
                    }
                    if (errMsg.empty())
                        errMsg = "Server returned an error mid-stream";

                    postError("Stream error: " + errMsg);
                    return (ExitCode)0;
                }

                // ── llama-server timings (final chunk) ──────────────
                // Server-side prompt-processing and generation speeds,
                // measured without network time.  Read BEFORE the usage
                // block: when usage arrives on a trailing chunk after
                // finish_reason, that block breaks out of the loop, and
                // llama-server can put timings on that same chunk.
                if (obj->has("timings") && !obj->isNull("timings")) {
                    try {
                        const auto tm = obj->getObject("timings");
                        if (tm) {
                            stats.hasServerTimings      = true;
                            stats.serverPromptN         = (long)readNumber(tm, "prompt_n");
                            stats.serverCacheN          = (long)readNumber(tm, "cache_n");
                            stats.serverPredictedN      = (long)readNumber(tm, "predicted_n");
                            stats.serverPromptMs        = readNumber(tm, "prompt_ms");
                            stats.serverPredictedMs     = readNumber(tm, "predicted_ms");
                            stats.serverPromptPerSec    = readNumber(tm, "prompt_per_second");
                            stats.serverPredictedPerSec = readNumber(tm, "predicted_per_second");
                        }
                    } catch (...) { /* malformed timings — skip */ }
                }

                // ── Context meter: capture `usage` when present ──────
                // Must run BEFORE the choices gate below: OpenAI-style
                // trailing usage chunks carry "choices": [] and would be
                // skipped by it.  Keep the latest values seen.
                if (obj->has("usage") && !obj->isNull("usage")) {
                    try {
                        auto usage = obj->getObject("usage");
                        if (usage) {
                            if (usage->has("prompt_tokens")) {
                                try {
                                    usagePromptTokens =
                                        usage->getValue<long>("prompt_tokens");
                                } catch (...) { /* non-numeric — skip */ }
                            }
                            if (usage->has("completion_tokens")) {
                                try {
                                    usageCompletionTokens =
                                        usage->getValue<long>("completion_tokens");
                                } catch (...) { /* non-numeric — skip */ }
                            }
                            // OpenAI-style detail objects (optional).
                            if (usage->has("prompt_tokens_details") &&
                                !usage->isNull("prompt_tokens_details")) {
                                const double v = readNumber(
                                    usage->getObject("prompt_tokens_details"),
                                    "cached_tokens");
                                if (v >= 0) stats.cachedPromptTokens = (long)v;
                            }
                            if (usage->has("completion_tokens_details") &&
                                !usage->isNull("completion_tokens_details")) {
                                const double v = readNumber(
                                    usage->getObject("completion_tokens_details"),
                                    "reasoning_tokens");
                                if (v >= 0) stats.reasoningTokens = (long)v;
                            }
                        }
                    } catch (...) { /* malformed usage — skip */ }

                    // If the stream is already terminal, usage was the
                    // only thing we were still draining for.
                    if (sawTerminalEvent && usagePromptTokens >= 0) break;
                }

                if (!obj->has("choices")) continue;
                auto choices = obj->getArray("choices");
                if (!choices || choices->size() == 0) continue;

                auto choice = choices->getObject(0);
                // choices[0] can be a non-object on shape-variant
                // chunks; getObject then yields a null Ptr whose
                // operator-> throws Poco::NullPointerException — an
                // exception the JSONException-only catch below never
                // absorbed, so one odd chunk aborted the whole stream.
                if (!choice) continue;

                // Extract content delta.  Same null discipline for
                // delta: some providers emit "delta": null on finish
                // chunks.
                auto delta = choice->has("delta")
                                 ? choice->getObject("delta")
                                 : Poco::JSON::Object::Ptr();
                if (delta) {

                    // ── Reasoning deltas ─────────────────────────
                    // Re-wrapped as inline think tags; see the state
                    // declaration above.  Non-string or null values
                    // are skipped defensively — provider validators
                    // differ on how they encode an absent field.
                    std::string reasoningDelta;
                    if (delta->has("reasoning_content") &&
                        !delta->isNull("reasoning_content")) {
                        try {
                            reasoningDelta =
                                delta->getValue<std::string>("reasoning_content");
                        } catch (...) { /* non-string — skip */ }
                    }
                    else if (delta->has("reasoning") &&
                             !delta->isNull("reasoning")) {
                        try {
                            reasoningDelta =
                                delta->getValue<std::string>("reasoning");
                        } catch (...) { /* non-string — skip */ }
                    }
                    if (!reasoningDelta.empty()) {
                        markToken(false);
                        std::string out;
                        if (!inReasoningBlock) {
                            inReasoningBlock = true;
                            out = "<think>";
                        }
                        out += reasoningDelta;
                        appendReply(out);

                        if (!queueDelta(out))
                            return (ExitCode)0;
                    }

                    if (delta->has("content") && !delta->isNull("content")) {
                        std::string content;
                        try {
                            content = delta->getValue<std::string>("content");
                        } catch (...) { /* non-string content — treat as absent */ }

                        // First visible content byte after reasoning:
                        // close the think block so display and stored
                        // reply transition cleanly.  Empty content
                        // deltas (role-priming chunks) never close —
                        // reasoning may still be streaming.
                        if (!content.empty()) markToken(true);
                        if (inReasoningBlock && !content.empty()) {
                            inReasoningBlock = false;
                            content = "</think>\n" + content;
                        }

                        appendReply(content);

                        if (!queueDelta(content))
                            return (ExitCode)0;
                    }

                    // ── Generated images ─────────────────────────
                    // Entries are image_url objects wrapping base64
                    // data URLs (see accumulator above).  A bare
                    // string entry or a direct "url" key are handled
                    // too — providers vary and a skipped image is
                    // worse than a lenient parse.
                    if (delta->has("images") && !delta->isNull("images")) {
                        markToken(true);
                        try {
                            auto imgArr = delta->getArray("images");
                            if (imgArr) {
                                for (size_t k = 0; k < imgArr->size(); ++k) {
                                    Poco::JSON::Object::Ptr entry;
                                    try { entry = imgArr->getObject(k); }
                                    catch (...) { entry = nullptr; }

                                    if (!entry) {
                                        // Bare string entry.
                                        try {
                                            collectImageUrl(
                                                imgArr->get(k)
                                                    .convert<std::string>());
                                        } catch (...) { /* skip */ }
                                        continue;
                                    }

                                    std::string url;
                                    if (entry->has("image_url") &&
                                        !entry->isNull("image_url")) {
                                        try {
                                            auto iu = entry->getObject("image_url");
                                            if (iu && iu->has("url"))
                                                url = iu->getValue<std::string>("url");
                                        } catch (...) { /* skip */ }
                                    }
                                    if (url.empty() && entry->has("url")) {
                                        try {
                                            url = entry->getValue<std::string>("url");
                                        } catch (...) { /* skip */ }
                                    }
                                    collectImageUrl(url);
                                }
                            }
                        } catch (...) {
                            // Malformed images fragment — skip; the
                            // text portion of the reply still streams.
                        }
                    }

                    // Phase 3c-ii: tool_calls fragments.  OpenAI
                    // streaming format — each entry has an index
                    // and partial field updates that we accumulate.
                    if (delta->has("tool_calls") && !delta->isNull("tool_calls")) {
                        markToken(true);
                        try {
                            auto tcArr = delta->getArray("tool_calls");
                            if (tcArr) {
                                for (size_t k = 0; k < tcArr->size(); ++k) {
                                    auto entry = tcArr->getObject(k);
                                    if (!entry) continue;

                                    // index is required; without it
                                    // we can't slot the fragment.
                                    int idx = -1;
                                    if (entry->has("index")) {
                                        try {
                                            idx = entry->getValue<int>("index");
                                        } catch (...) { idx = -1; }
                                    }
                                    ToolCallAcc* slot = ensureToolCallSlot(idx);
                                    if (!slot) continue;

                                    if (entry->has("id")) {
                                        try {
                                            std::string s = entry->getValue<std::string>("id");
                                            if (!s.empty()) slot->id = s;
                                        } catch (...) { /* skip */ }
                                    }
                                    if (entry->has("type")) {
                                        try {
                                            std::string s = entry->getValue<std::string>("type");
                                            if (!s.empty()) slot->type = s;
                                        } catch (...) { /* skip */ }
                                    }
                                    if (entry->has("function") && !entry->isNull("function")) {
                                        try {
                                            auto fn = entry->getObject("function");
                                            if (fn) {
                                                if (fn->has("name")) {
                                                    try {
                                                        std::string s = fn->getValue<std::string>("name");
                                                        if (!s.empty()) slot->name = s;
                                                    } catch (...) { /* skip */ }
                                                }
                                                if (fn->has("arguments")) {
                                                    std::string s;
                                                    try {
                                                        s = fn->getValue<std::string>("arguments");
                                                    } catch (...) { /* skip */ }
                                                    // A model stuck in a
                                                    // generation loop emits
                                                    // arguments forever.  Only
                                                    // refusing to store more
                                                    // (the old behaviour) let
                                                    // generation run on to
                                                    // the context limit, so
                                                    // abort the stream: the
                                                    // error path ends the
                                                    // agent loop and the call
                                                    // is never dispatched.
                                                    // (postError/return sit
                                                    // outside the try so the
                                                    // catch(...) below cannot
                                                    // swallow them.)
                                                    if (slot->arguments.size() + s.size()
                                                            > kMaxToolArgBytes) {
                                                        argOverflow = true;
                                                    } else if (!s.empty()) {
                                                        appendToolArgs(*slot, s);
                                                    }
                                                }
                                            }
                                        } catch (...) { /* skip */ }
                                    }
                                }
                            }
                        } catch (...) {
                            // Malformed tool_calls fragment — skip.
                            // We continue parsing rather than abort
                            // the whole stream; a partial tool call
                            // accumulator becomes a partial result.
                        }
                        if (argOverflow) {
                            postError("Stopped this reply: the model's tool-call "
                                      "arguments passed " +
                                      std::to_string(kMaxToolArgBytes / (1024 * 1024)) +
                                      " MB, which usually means it is stuck in a "
                                      "loop. The tool call was not run.");
                            return (ExitCode)0;
                        }
                    }
                }

                // Check finish_reason for completion.  OpenAI-compatible
                // servers normally send this on the final chunk before [DONE].
                // Treat any non-empty finish_reason as terminal so uncommon
                // reasons (for example "content_filter") do not force us to
                // wait for a marker that some local servers may omit.
                if (choice->has("finish_reason") && !choice->isNull("finish_reason")) {
                    std::string reason;
                    try {
                        reason = choice->getValue<std::string>("finish_reason");
                    } catch (...) { /* non-string finish_reason — ignore */ }
                    if (!reason.empty()) {
                        sawTerminalEvent = true;
                        // llama-server reports usage on this same final
                        // chunk — captured above — so the common local
                        // case breaks here exactly as before.  When usage
                        // hasn't arrived yet (OpenAI-style providers send
                        // it on a trailing usage-only chunk), keep reading:
                        // the loop exits at that usage chunk, at [DONE],
                        // or at EOF, all of which arrive immediately after
                        // finish_reason on conforming servers.
                        if (usagePromptTokens >= 0) break;
                        continue;
                    }
                }
            }
            catch (const Poco::JSON::JSONException&) {
                if (responsesApi) {
                    postError("Invalid JSON in the Responses stream.");
                    return (ExitCode)0;
                }
                // Skip malformed JSON lines
                continue;
            }
            catch (const Poco::Exception&) {
                if (responsesApi) {
                    postError("Malformed event in the Responses stream.");
                    return (ExitCode)0;
                }
                // Belt-and-braces for the guards above: any other Poco
                // failure inside ONE chunk (null Ptr deref, bad cast on
                // an unexpected field type) skips that chunk instead of
                // escaping to the outer handlers and demoting a fine
                // in-flight reply to a stream error.  Transport errors
                // still surface — they throw from getline/receive,
                // outside this per-chunk try.
                continue;
            }
        }
        } catch (...) {
            // Post-terminal drain failure only — the reply is complete.
            if (!sawTerminalEvent) throw;
        }

        if (!isCancelled() && !sawTerminalEvent) {
            postError(
                sawAnySseData
                    ? "Stream ended unexpectedly before the server sent a terminal event."
                    : "Stream ended before the server sent any response data.");
            return (ExitCode)0;
        }

        if (!isCancelled()) {
            // ── Close a still-open reasoning block ──────────────
            // A reply can legitimately end while thinking is the
            // only text emitted: reasoning straight into tool_calls
            // (Qwen3 does this constantly) or a terminal event with
            // no visible content.  Emit the close tag so both the
            // stored message and the display block terminate
            // cleanly before completion is signalled.
            if (inReasoningBlock) {
                inReasoningBlock = false;
                appendReply("</think>");

                if (!queueDelta("</think>"))
                    return (ExitCode)0;
            }

            // ── Phase 3c-ii: serialize the tool_calls accumulator ──
            // Render the per-index slots into an OpenAI-shape JSON
            // array.  Empty slots (no id AND no name AND no
            // arguments — happens if a fragment carried just an
            // index with no payload) are skipped.  Auto-id any
            // slot the model didn't name (rare but per-spec OK; we
            // synthesize "call_<idx>" so downstream threading has
            // something stable).
            // An interrupted (repetition-stopped) reply carries no tool
            // calls: they are unfinished at best, and dispatching one
            // after telling the user the reply stopped is the bug this
            // guards against.  The frame also refuses them (belt and
            // braces for XML-protocol calls embedded in fullReply).
            if (stats.stoppedForRepetition) {
                toolCalls.clear();
                responsesToolCallsJson.clear();
                responsesOutputJson.clear();
            }

            std::string toolCallsJson;
            if (!toolCalls.empty()) {
                Poco::JSON::Array::Ptr arr = new Poco::JSON::Array;
                int slotIdx = 0;
                for (const auto& acc : toolCalls) {
                    if (acc.id.empty() && acc.name.empty() &&
                        acc.arguments.empty()) {
                        ++slotIdx;
                        continue;
                    }

                    Poco::JSON::Object::Ptr entry = new Poco::JSON::Object;
                    entry->set("id",
                        acc.id.empty() ? std::string("call_") + std::to_string(slotIdx)
                                       : acc.id);
                    entry->set("type",
                        acc.type.empty() ? std::string("function") : acc.type);

                    Poco::JSON::Object::Ptr fn = new Poco::JSON::Object;
                    fn->set("name", acc.name);
                    // arguments is a JSON-encoded string per OpenAI
                    // spec — we pass it through verbatim.  An empty
                    // arguments string is valid for tools that take
                    // no parameters.
                    fn->set("arguments", acc.arguments);
                    entry->set("function", fn);

                    arr->add(entry);
                    ++slotIdx;
                }
                if (arr->size() > 0) {
                    std::ostringstream oss;
                    Poco::JSON::Stringifier::stringify(arr, oss);
                    toolCallsJson = oss.str();
                }
            }

            // Responses lane: the adapter already produced the array in
            // the same shape (id = call_id, which is what the next
            // request threads function_call_output on).  The delta
            // accumulator above is never fed on this lane.
            if (responsesApi) toolCallsJson = responsesToolCallsJson;

            // Ordering is load-bearing: the UI accumulates deltas and
            // treats COMPLETE as "everything has arrived". Anything
            // still buffered here has to go out first.
            flushDelta(true);

            stats.totalMs          = msSinceSend();
            stats.promptTokens     = usagePromptTokens;
            stats.completionTokens = usageCompletionTokens;

            wxCommandEvent* event = new wxCommandEvent(wxEVT_ASSISTANT_COMPLETE);
            event->SetString(wxString::FromUTF8(fullReply));
            auto* payload = new AssistantCompletePayload(
                toolCallsJson, usagePromptTokens, usageCompletionTokens,
                std::move(imageDataUrls), std::move(responsesOutputJson));
            payload->SetStats(stats);
            event->SetClientObject(payload);
            SafeQueueEvent(event);
        }
    }
    catch (const Poco::Net::HTTPException& ex) {
        if (!isCancelled()) postError("HTTP Error: " + ex.displayText());
    }
    catch (const Poco::Net::NetException& ex) {
        if (!isCancelled()) postError("Network Error: " + ex.displayText());
    }
    catch (const Poco::Exception& ex) {
        if (!isCancelled()) postError("Poco Error: " + ex.displayText());
    }
    catch (const std::exception& ex) {
        if (!isCancelled()) postError(std::string("Error: ") + ex.what());
    }

    return (ExitCode)0;
}

// ═══════════════════════════════════════════════════════════════════
// ChatClient Implementation
// ═══════════════════════════════════════════════════════════════════

ChatClient::ChatClient(wxEvtHandler* eventHandler,
                       std::weak_ptr<std::atomic<bool>> aliveToken)
    : m_eventHandler(eventHandler)
    , m_aliveToken(aliveToken)
    , m_isStreaming(false)
{
}

ChatClient::~ChatClient()
{
    StopGeneration();
}

bool ChatClient::SendMessage(const InferenceTarget& target,
    const std::string& requestBody,
    unsigned long generationId)
{
    if (m_isStreaming) {
        return false;
    }

    m_isStreaming = true;
    m_activeRequest = std::make_shared<ChatRequestControl>();
    if (!target.resolutionError.empty() || target.baseUrl.empty()) {
        // Accepted for asynchronous error delivery, but no network worker is
        // started. Existing normal/agent/hidden-turn handlers all unwind via
        // this generation-tagged event, just as they do for transport errors.
        auto* event = new wxCommandEvent(wxEVT_ASSISTANT_ERROR);
        event->SetExtraLong(generationId);
        event->SetString(wxString::FromUTF8(target.resolutionError.empty()
            ? std::string("Selected AI connection has no server address. No request was sent.")
            : target.resolutionError));
        if (!LbQueueEventIfAlive(m_eventHandler, m_aliveToken, event)) {
            m_activeRequest.reset();
            m_isStreaming = false;
            return false;
        }
        return true;
    }
    auto* thread = new ChatWorkerThread(
        m_eventHandler, target, requestBody,
        m_activeRequest, m_aliveToken, generationId);

    if (thread->Run() != wxTHREAD_NO_ERROR) {
        delete thread;
        m_activeRequest.reset();
        m_isStreaming = false;
        return false;
    }

    return true;
}

bool ChatClient::SendMessage(const std::string& model,
    const std::string& apiUrl,
    const std::string& requestBody,
    unsigned long generationId)
{
    // Reproduce historical behavior exactly: a plain-http, no-auth,
    // OpenAI-compatible local target.
    return SendMessage(InferenceTarget::Local(apiUrl, model),
                       requestBody, generationId);
}

void ChatClient::StopGeneration()
{
    if (m_isStreaming && m_activeRequest) {
        // Cancel() closes the socket; it does not merely raise a flag.
        // The worker is almost always parked in std::getline() on the SSE
        // stream and only re-checks the flag between lines, so without the
        // abort a stopped generation kept running -- holding llama-server's
        // only slot (--parallel 1) and delaying the next send behind it.
        m_activeRequest->Cancel();

        // Drop our handle immediately so a new SendMessage() can start
        // while the old worker is still unwinding.  The worker keeps the
        // control object alive through its own shared_ptr.
        m_activeRequest.reset();
        m_isStreaming = false;
    }
}

void ChatClient::ResetStreamingState()
{
    // Normal-completion path -- release the handle WITHOUT cancelling.
    // The transport has already closed itself and the worker is done or
    // nearly so; calling Cancel() here would be harmless but misleading.
    m_isStreaming = false;
    m_activeRequest.reset();
}
