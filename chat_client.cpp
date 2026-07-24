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
#include "ui_event_post.h"
#include "lb_ssl.h"

// Define custom events
wxDEFINE_EVENT(wxEVT_ASSISTANT_DELTA, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_ASSISTANT_COMPLETE, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_ASSISTANT_ERROR, wxCommandEvent);

// ═══════════════════════════════════════════════════════════════════
// ChatWorkerThread Implementation
// ═══════════════════════════════════════════════════════════════════

ChatWorkerThread::ChatWorkerThread(wxEvtHandler* eventHandler,
    InferenceTarget target,
    std::string requestBody,
    std::shared_ptr<std::atomic<bool>> cancelFlag,
    std::weak_ptr<std::atomic<bool>> aliveToken,
    unsigned long generationId)
    : wxThread(wxTHREAD_DETACHED)
    , m_eventHandler(eventHandler)
    , m_target(std::move(target))
    , m_requestBody(std::move(requestBody))
    , m_cancelFlag(cancelFlag)
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

    auto collectImageUrl = [&](const std::string& url) {
        if (url.empty()) return;
        for (const auto& existing : imageDataUrls)
            if (existing == url) return;
        imageDataUrls.push_back(url);
    };

    // ── Context meter: exact token usage from the stream ─────────
    // Latest `usage` object seen on any SSE chunk.  llama-server puts
    // usage on the final chunk (the one carrying finish_reason);
    // OpenAI-style providers send it on a trailing usage-only chunk
    // (choices: []) after finish_reason.  -1 = not reported.
    long usagePromptTokens     = -1;
    long usageCompletionTokens = -1;

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

    auto isCancelled = [this]() { return m_cancelFlag->load(); };

    try {
        // ── Connect to the target's OpenAI-compatible endpoint ──
        // The path comes from the target so an Anthropic-native
        // adapter (future) can redirect to /v1/messages without
        // touching the transport. Port is resolved explicitly so an
        // authority-only URL ("https://host") still connects on 443.
        Poco::URI uri(m_target.baseUrl + m_target.chatPath);

        int port = uri.getPort();
        if (port == 0)
            port = m_target.useTls ? 443 : 80;

        std::unique_ptr<Poco::Net::HTTPClientSession> sess;
        if (m_target.useTls) {
            // SSL is initialized lazily and once per process; only the
            // TLS branch pays for it, so local-only users never load it.
            lb::EnsureSSLInitialized();
            sess.reset(new Poco::Net::HTTPSClientSession(uri.getHost(), port));
        } else {
            sess.reset(new Poco::Net::HTTPClientSession(uri.getHost(), port));
        }
        sess->setTimeout(Poco::Timespan(120, 0)); // 2min timeout for large models

        Poco::Net::HTTPRequest req(
            Poco::Net::HTTPRequest::HTTP_POST,
            uri.getPathAndQuery(),
            Poco::Net::HTTPMessage::HTTP_1_1
        );
        req.setContentType("application/json");
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

        std::ostream& out = sess->sendRequest(req);
        out << m_requestBody;
        out.flush();

        Poco::Net::HTTPResponse resp;
        std::istream& in = sess->receiveResponse(resp);

        if (resp.getStatus() != Poco::Net::HTTPResponse::HTTP_OK) {
            std::string err;
            Poco::StreamCopier::copyToString(in, err);

            wxCommandEvent* event = new wxCommandEvent(wxEVT_ASSISTANT_ERROR);
            event->SetString(wxString::FromUTF8(
                "API Error (" + std::to_string(resp.getStatus()) + "): "
                + resp.getReason() + " - " + err
            ));
            SafeQueueEvent(event);
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

        // The read loop gets its own guard: once sawTerminalEvent is
        // set, the only thing still being read is the optional trailing
        // usage chunk / [DONE] marker.  A transport exception in that
        // drain phase (e.g. a non-conforming server holding the socket
        // open until the receive timeout) must not demote an already
        // complete reply to a stream error.  Pre-terminal exceptions
        // are real failures and are rethrown to the handlers below.
        try {
        while (std::getline(in, line) && !isCancelled()) {
            // Strip trailing \r
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            // Skip empty lines (SSE event boundaries)
            if (line.empty()) continue;

            // Only process "data:" prefixed lines. The SSE spec allows
            // an optional single space after the colon — handle both.
            if (line.size() < 5 || line.compare(0, 5, "data:") != 0)
                continue;

            std::string data = line.substr(5);
            if (!data.empty() && data.front() == ' ')
                data.erase(0, 1);

            // End of stream marker
            if (data == "[DONE]") {
                sawTerminalEvent = true;
                break;
            }

            sawAnySseData = true;

            try {
                Poco::JSON::Parser parser;
                auto obj = parser.parse(data).extract<Poco::JSON::Object::Ptr>();

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

                    wxCommandEvent* event = new wxCommandEvent(wxEVT_ASSISTANT_ERROR);
                    event->SetString(wxString::FromUTF8("Stream error: " + errMsg));
                    SafeQueueEvent(event);
                    return (ExitCode)0;
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
                        std::string out;
                        if (!inReasoningBlock) {
                            inReasoningBlock = true;
                            out = "<think>";
                        }
                        out += reasoningDelta;
                        fullReply += out;

                        wxCommandEvent* event = new wxCommandEvent(wxEVT_ASSISTANT_DELTA);
                        event->SetString(wxString::FromUTF8(out));
                        if (!SafeQueueEvent(event))
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
                        if (inReasoningBlock && !content.empty()) {
                            inReasoningBlock = false;
                            content = "</think>\n" + content;
                        }

                        fullReply += content;

                        wxCommandEvent* event = new wxCommandEvent(wxEVT_ASSISTANT_DELTA);
                        event->SetString(wxString::FromUTF8(content));
                        if (!SafeQueueEvent(event))
                            return (ExitCode)0;
                    }

                    // ── Generated images ─────────────────────────
                    // Entries are image_url objects wrapping base64
                    // data URLs (see accumulator above).  A bare
                    // string entry or a direct "url" key are handled
                    // too — providers vary and a skipped image is
                    // worse than a lenient parse.
                    if (delta->has("images") && !delta->isNull("images")) {
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
                                                    try {
                                                        std::string s = fn->getValue<std::string>("arguments");
                                                        slot->arguments += s;
                                                    } catch (...) { /* skip */ }
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
                // Skip malformed JSON lines
                continue;
            }
            catch (const Poco::Exception&) {
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
            wxCommandEvent* event = new wxCommandEvent(wxEVT_ASSISTANT_ERROR);
            event->SetString(wxString::FromUTF8(
                sawAnySseData
                    ? "Stream ended unexpectedly before the server sent a terminal event."
                    : "Stream ended before the server sent any response data."
            ));
            SafeQueueEvent(event);
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
                fullReply += "</think>";

                wxCommandEvent* event = new wxCommandEvent(wxEVT_ASSISTANT_DELTA);
                event->SetString(wxString::FromUTF8("</think>"));
                if (!SafeQueueEvent(event))
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

            wxCommandEvent* event = new wxCommandEvent(wxEVT_ASSISTANT_COMPLETE);
            event->SetString(wxString::FromUTF8(fullReply));
            event->SetClientObject(new AssistantCompletePayload(
                toolCallsJson, usagePromptTokens, usageCompletionTokens,
                std::move(imageDataUrls)));
            SafeQueueEvent(event);
        }
    }
    catch (const Poco::Net::HTTPException& ex) {
        if (!isCancelled()) {
            wxCommandEvent* event = new wxCommandEvent(wxEVT_ASSISTANT_ERROR);
            event->SetString(wxString::FromUTF8("HTTP Error: " + ex.displayText()));
            SafeQueueEvent(event);
        }
    }
    catch (const Poco::Net::NetException& ex) {
        if (!isCancelled()) {
            wxCommandEvent* event = new wxCommandEvent(wxEVT_ASSISTANT_ERROR);
            event->SetString(wxString::FromUTF8("Network Error: " + ex.displayText()));
            SafeQueueEvent(event);
        }
    }
    catch (const Poco::Exception& ex) {
        if (!isCancelled()) {
            wxCommandEvent* event = new wxCommandEvent(wxEVT_ASSISTANT_ERROR);
            event->SetString(wxString::FromUTF8("Poco Error: " + ex.displayText()));
            SafeQueueEvent(event);
        }
    }
    catch (const std::exception& ex) {
        if (!isCancelled()) {
            wxCommandEvent* event = new wxCommandEvent(wxEVT_ASSISTANT_ERROR);
            event->SetString(wxString::FromUTF8(std::string("Error: ") + ex.what()));
            SafeQueueEvent(event);
        }
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
    m_cancelFlag = std::make_shared<std::atomic<bool>>(false);

    auto* thread = new ChatWorkerThread(
        m_eventHandler, target, requestBody,
        m_cancelFlag, m_aliveToken, generationId);

    if (thread->Run() != wxTHREAD_NO_ERROR) {
        delete thread;
        m_cancelFlag.reset();
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
    if (m_isStreaming && m_cancelFlag) {
        m_cancelFlag->store(true);
        m_cancelFlag.reset();
        m_isStreaming = false;
    }
}

void ChatClient::ResetStreamingState()
{
    m_isStreaming = false;
    m_cancelFlag.reset();
}
