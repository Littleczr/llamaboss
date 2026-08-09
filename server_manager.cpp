// server_manager.cpp
#include "server_manager.h"
#include "path_safety.h"
#include "gguf_metadata.h"

#include <wx/filename.h>
#include <wx/dir.h>
#include <wx/stdpaths.h>
#include <wx/fileconf.h>
#include <wx/utils.h>

#include <Poco/URI.h>
#include <Poco/Net/HTTPClientSession.h>
#include <thread>
#include <mutex>
#include <deque>
#include <algorithm>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/StreamCopier.h>
#include <Poco/Timespan.h>
#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>

#include <sstream>
#include <fstream>
#include <algorithm>
#include <chrono>
#include <set>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include "ui_event_post.h"

// ── Event definitions ────────────────────────────────────────────
wxDEFINE_EVENT(wxEVT_SERVER_READY, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_SERVER_ERROR, wxCommandEvent);

// ── Internal: read the last ~N bytes of a file, snapped to a line ────
// Used to attach llama-server log tail to error events so the user
// sees why the server died instead of a generic timeout.
static std::string ReadLogTail(const std::string& path, size_t maxBytes = 4096)
{
    try {
        std::ifstream f(path_safety::Utf8ToWide(path), std::ios::binary | std::ios::ate);
        if (!f) return "";
        std::streampos size = f.tellg();
        if (size <= 0) return "";

        size_t total   = static_cast<size_t>(size);
        size_t readLen = (total > maxBytes) ? maxBytes : total;
        f.seekg(-static_cast<std::streamoff>(readLen), std::ios::end);

        std::string buf(readLen, '\0');
        f.read(&buf[0], readLen);
        buf.resize(static_cast<size_t>(f.gcount()));

        // If we truncated from the middle of a line, advance to the
        // next newline so the tail starts cleanly.
        if (total > maxBytes) {
            size_t nl = buf.find('\n');
            if (nl != std::string::npos) buf.erase(0, nl + 1);
        }
        return buf;
    } catch (...) {
        return "";
    }
}

// ═══════════════════════════════════════════════════════════════════
//  ServerHealthThread — polls /health until 200 or timeout

static std::string ToLowerAscii(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

static bool ContainsAny(const std::string& haystack,
                        std::initializer_list<const char*> needles)
{
    for (const char* needle : needles) {
        if (needle && haystack.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

static std::string WxToUtf8String(const wxString& s)
{
    wxCharBuffer buf = s.ToUTF8();
    return buf.data() ? std::string(buf.data()) : std::string();
}

static wxString ModelFolderForGgufPath(const std::string& ggufPath)
{
    wxFileName modelFn(wxString::FromUTF8(ggufPath.c_str()));
    return modelFn.GetPath();
}

static bool IsBundledModelFolder(const wxString& modelFolder)
{
    if (modelFolder.IsEmpty())
        return false;

    wxString modelsRoot = wxString::FromUTF8(ServerManager::GetModelsDir().c_str());
    if (modelsRoot.IsEmpty())
        return false;

    const std::string folderUtf8 = WxToUtf8String(modelFolder);
    const std::string rootUtf8   = WxToUtf8String(modelsRoot);

    // Bundled means: the GGUF lives in a direct child of the models root,
    // not in the root itself and not nested deeper. Use normalized path
    // comparison so Windows casing/separator differences from settings,
    // scans, or saved conversations do not silently disable mmproj pairing.
    if (path_safety::SameModelPath(folderUtf8, rootUtf8))
        return false;

    wxString parentFolder = wxFileName(modelFolder).GetPath();
    return path_safety::SameModelPath(WxToUtf8String(parentFolder), rootUtf8);
}

static bool IsBundledModelPath(const std::string& ggufPath)
{
    return IsBundledModelFolder(ModelFolderForGgufPath(ggufPath));
}

// Startup fallback classifiers are intentionally specific.  Earlier
// builds retried on every server-start failure, which could hide unrelated
// problems such as a bad model path, CUDA/DLL failure, or port conflict.
static bool LooksLikeMtpFailure(const std::string& error)
{
    const std::string e = ToLowerAscii(error);

    // Prefer direct llama.cpp startup diagnostics when present.
    if (ContainsAny(e, {
            "failed to create mtp context",
            "failed to create draft context",
            "failed to initialize mtp",
            "failed to initialize draft",
            "failed to load mtp",
            "failed to load draft model"})) {
        return true;
    }

    // Do not blame MTP for a clear Jinja/template diagnostic merely
    // because normal startup logging also mentioned draft-mtp.  Direct
    // MTP failures above still take precedence when both are present.
    if (ContainsAny(e, {
            "--jinja",
            "jinja",
            "chat template",
            "chat_template",
            "chat-template",
            "template compile",
            "template parsing"})) {
        return false;
    }

    // Some backend failures end in an assertion/allocation error after
    // the log has only announced creation of the MTP draft context.
    // Require both an MTP/speculative marker and a fatal-looking marker
    // so unrelated model-load errors do not trigger a second launch.
    const bool hasMtpMarker = ContainsAny(e, {
        "draft-mtp",
        "mtp context",
        "mtp draft",
        "nextn_predict_layers",
        "common_speculative_impl_draft_mtp",
        "creating speculative implementation"});

    const bool hasFailureMarker = ContainsAny(e, {
        "failed",
        "failure",
        "fatal",
        "error",
        "assert",
        "abort",
        "exception",
        "out of memory",
        "not enough memory",
        "cannot allocate",
        "could not allocate",
        "cuda error"});

    return hasMtpMarker && hasFailureMarker;
}

static bool LooksLikeJinjaOrTemplateFailure(const std::string& error)
{
    const std::string e = ToLowerAscii(error);

    if (ContainsAny(e, {"--jinja", "jinja"})) {
        return true;
    }

    if (ContainsAny(e, {
            "chat template",
            "chat_template",
            "chat-template",
            "failed to parse template",
            "failed to compile template",
            "template parsing",
            "template compile",
            "template error",
            "unsupported chat template",
            "invalid chat template"})) {
        return true;
    }

    return false;
}

// ── Single-slot verification (startup backstop) ─────────────────
// GET /slots and count the entries.  The launch command pins
// --parallel 1, but llama-server's default changed under us once
// already (1 → auto in late 2025, resolving to 4 slots + unified KV
// + automatic slot assignment), and every KV-persistence assumption
// in ServerManager is single-slot: ExecuteSlotAction hard-codes
// /slots/0, the m_slotOwner/m_slotDirty contract assumes requests
// serialize on one slot, and the multi-window queue notice promises
// FIFO waiting.  If a future bundled binary ignores or repurposes
// the flag, this logs an unmissable error at every launch instead of
// letting the conversation-switch fast path corrupt silently (its
// failures are swallowed by design).  Diagnostic only: runs after
// wxEVT_SERVER_READY is posted so readiness is never delayed, and
// every failure mode collapses to "unverified this launch".
static void VerifySingleSlot(const std::string& baseUrl,
                             Poco::Logger* logger)
{
    if (!logger) return;
    try {
        Poco::URI uri(baseUrl + "/slots");
        Poco::Net::HTTPClientSession sess(uri.getHost(), uri.getPort());
        sess.setTimeout(Poco::Timespan(2, 0));

        Poco::Net::HTTPRequest req(Poco::Net::HTTPRequest::HTTP_GET,
                                   uri.getPathAndQuery());
        sess.sendRequest(req);

        Poco::Net::HTTPResponse resp;
        std::istream& in = sess.receiveResponse(resp);
        std::string body;
        Poco::StreamCopier::copyToString(in, body);

        if (resp.getStatus() != Poco::Net::HTTPResponse::HTTP_OK) {
            logger->warning("slotpin: /slots probe returned HTTP " +
                            std::to_string((int)resp.getStatus()) +
                            " - slot count unverified this launch");
            return;
        }

        Poco::JSON::Parser p;
        auto var = p.parse(body);
        auto arr = var.extract<Poco::JSON::Array::Ptr>();
        const std::size_t n = arr ? arr->size() : 0;

        if (n == 1) {
            logger->information("slotpin: verified single slot (n_slots=1)");
        } else {
            logger->error(
                "slotpin: llama-server reports " + std::to_string(n) +
                " slots despite --parallel 1. KV save/restore targets "
                "/slots/0 and assumes serialized single-slot scheduling; "
                "the conversation-switch fast path and the multi-window "
                "busy notice are NOT safe under multi-slot assignment. "
                "Check the bundled llama-server build before shipping.");
        }
    } catch (...) {
        // Server stopping, endpoint refused, malformed body — all
        // equivalent to "unverified this launch".  Never fatal.
    }
}

// ═══════════════════════════════════════════════════════════════════

ServerHealthThread::ServerHealthThread(wxEvtHandler* handler,
                                       const std::string& baseUrl,
                                       std::shared_ptr<std::atomic<bool>> cancelFlag,
                                       std::weak_ptr<std::atomic<bool>> aliveToken,
#ifdef __WXMSW__
                                       HANDLE processHandle,
#endif
                                       const std::string& logPath,
                                       ServerLaunchGeneration generation,
                                       int timeoutMs,
                                       Poco::Logger* logger)
    : wxThread(wxTHREAD_DETACHED)
    , m_handler(handler)
    , m_baseUrl(baseUrl)
    , m_cancelFlag(cancelFlag)
    , m_aliveToken(aliveToken)
#ifdef __WXMSW__
    , m_processHandle(INVALID_HANDLE_VALUE)
#endif
    , m_logPath(logPath)
    , m_generation(generation)
    , m_timeoutMs(timeoutMs)
    , m_logger(logger)
{
#ifdef __WXMSW__
    // Duplicate the handle so we own our own copy. Required because
    // the ServerManager may close its handle (StopServer / dtor)
    // while this detached thread is still running — closing a handle
    // someone else is waiting on is undefined. With a duplicate,
    // both sides can close independently.
    //
    // The return value matters: on failure Windows may leave the target
    // as NULL rather than untouched, and NULL passes an
    // `!= INVALID_HANDLE_VALUE` guard.  That fed WaitForSingleObject(NULL)
    // every poll and CloseHandle(NULL) on the way out (which raises under
    // a debugger with handle verification on).  Normalize explicitly.
    if (processHandle != INVALID_HANDLE_VALUE) {
        if (!DuplicateHandle(GetCurrentProcess(), processHandle,
                             GetCurrentProcess(), &m_processHandle,
                             0, FALSE, DUPLICATE_SAME_ACCESS)) {
            m_processHandle = INVALID_HANDLE_VALUE;
        }
        else if (m_processHandle == nullptr) {
            m_processHandle = INVALID_HANDLE_VALUE;
        }
    }
#endif
}

#ifdef __WXMSW__
// Idempotent: nulls the member after closing so Entry() and the
// destructor can both call it without any double-close risk.
void ServerHealthThread::CloseProcessHandle()
{
    if (m_processHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(m_processHandle);
        m_processHandle = INVALID_HANDLE_VALUE;
    }
}
#endif

ServerHealthThread::~ServerHealthThread()
{
#ifdef __WXMSW__
    // Normally a no-op: Entry() already closed and nulled the handle.
    // This catches the paths where Entry() never ran at all -- a failed
    // wxThread::Create() or Run() in StartServer, which deletes this
    // object directly.
    CloseProcessHandle();
#endif
}

bool ServerHealthThread::SafePost(wxCommandEvent* ev)
{
    // Fast cancellation guard. StopServer() can still set the flag after this
    // check but before wx queues the event; the launch-generation stamp is the
    // authoritative guard for that remaining TOCTOU window.
    if (m_cancelFlag->load()) {
        delete ev;
        return false;
    }
    return LbQueueEventIfAlive(m_handler, m_aliveToken, ev);
}

wxThread::ExitCode ServerHealthThread::Entry()
{
    auto start = std::chrono::steady_clock::now();

    while (!m_cancelFlag->load()) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();

        if (elapsed > m_timeoutMs) {
            std::string msg = "Server did not become ready within "
                              + std::to_string(m_timeoutMs / 1000)
                              + " seconds.";
            std::string tail = ReadLogTail(m_logPath);
            if (!tail.empty()) msg += "\n\nLast log output:\n" + tail;
            else if (!m_logPath.empty())
                msg += "\n\nSee log: " + m_logPath;

            auto* ev = new wxCommandEvent(wxEVT_SERVER_ERROR);
            SetServerEventGeneration(*ev, m_generation);
            ev->SetString(wxString::FromUTF8(msg));
            SafePost(ev);
#ifdef __WXMSW__
            CloseProcessHandle();
#endif
            return (ExitCode)0;
        }

#ifdef __WXMSW__
        // Did the child process exit already? Non-blocking wait (timeout=0).
        // If so, llama-server crashed before answering /health — no point
        // polling for another ~120 seconds. Grab the log tail and bail.
        if (m_processHandle != INVALID_HANDLE_VALUE &&
            WaitForSingleObject(m_processHandle, 0) == WAIT_OBJECT_0) {
            DWORD exitCode = 0;
            GetExitCodeProcess(m_processHandle, &exitCode);
            std::string msg = "llama-server exited (code "
                              + std::to_string(exitCode)
                              + ") before becoming ready.";
            std::string tail = ReadLogTail(m_logPath);
            if (!tail.empty()) msg += "\n\nLast log output:\n" + tail;

            auto* ev = new wxCommandEvent(wxEVT_SERVER_ERROR);
            SetServerEventGeneration(*ev, m_generation);
            ev->SetString(wxString::FromUTF8(msg));
            SafePost(ev);
            CloseProcessHandle();
            return (ExitCode)0;
        }
#endif

        try {
            Poco::URI uri(m_baseUrl + "/health");
            Poco::Net::HTTPClientSession sess(uri.getHost(), uri.getPort());
            // 500ms is plenty on localhost — if the server can't answer
            // /health in half a second, it isn't "slow," the socket's hung.
            // The outer retry loop sleeps 500ms between probes anyway, so
            // worst case we waste 1s per failed attempt.
            sess.setTimeout(Poco::Timespan(0, 500 * 1000));

            Poco::Net::HTTPRequest req(Poco::Net::HTTPRequest::HTTP_GET,
                                       uri.getPathAndQuery());
            sess.sendRequest(req);

            Poco::Net::HTTPResponse resp;
            std::istream& in = sess.receiveResponse(resp);
            std::string body;
            Poco::StreamCopier::copyToString(in, body);

            if (resp.getStatus() == Poco::Net::HTTPResponse::HTTP_OK) {
                auto* ev = new wxCommandEvent(wxEVT_SERVER_READY);
                SetServerEventGeneration(*ev, m_generation);
                SafePost(ev);

                // Post-ready, pre-exit: cheap /slots probe confirming
                // the --parallel 1 pin actually took.  Ordered after
                // SafePost so readiness latency is untouched; skipped
                // when a StopServer already raced in.
                if (!m_cancelFlag->load())
                    VerifySingleSlot(m_baseUrl, m_logger);
#ifdef __WXMSW__
                CloseProcessHandle();
#endif
                return (ExitCode)0;
            }
            // 503 = still loading, keep polling
        }
        catch (...) {
            // Connection refused / not ready yet — keep polling
        }

        // Sleep 500ms in small increments (check cancel flag frequently)
        for (int i = 0; i < 10 && !m_cancelFlag->load(); ++i)
            wxMilliSleep(50);
    }

#ifdef __WXMSW__
    CloseProcessHandle();
#endif
    return (ExitCode)0;
}

// ═══════════════════════════════════════════════════════════════════
//  ServerManager
// ═══════════════════════════════════════════════════════════════════

// ── Serialized slot-action worker ────────────────────────────────
//
// Slot actions must reach llama-server in dispatch order.  The server
// serializes tasks once they are ENQUEUED on the slot, but it cannot
// order two clients racing to connect.  A conversation switch fires
// save-away(A) then restore-in(B) back-to-back; with one detached
// thread per action those two POSTs raced, and when the restore won,
// the slot held B's KV by the time the save executed — serializing
// B's state under A's filename.  Benign at restore time (the token
// prefix mismatches and llama-server falls back to full reprocess)
// but it silently defeated the fast path on exactly the rapid-switch
// pattern it was built for, at the cost of a multi-GB wasted write.
//
// Fix: one FIFO queue drained by at most one worker thread.  Callers
// keep fire-and-forget semantics — enqueue is a mutex push, never a
// network wait.  The queue block is shared_ptr-owned by ServerManager
// AND the worker, so app shutdown never joins: the destructor flips
// `stop` and the worker exits after its in-flight action instead of
// POSTing to a server StopServer just killed.  The worker exits when
// the queue drains (no idle thread); a later enqueue spawns a fresh
// one.  Overlap is impossible because `workerRunning` only flips
// under the same mutex that guards the queue.

struct SlotAction {
    // Launch identity of the server this action was enqueued
    // against.  Without it a queued action outlives the server it
    // was meant for: kill server A, start B on the same port, and
    // the stale action POSTs model A's KV filename at model B.  The
    // worker drops any action whose generation no longer matches the
    // queue's live generation.
    ServerLaunchGeneration generation = kInvalidServerLaunchGeneration;
    std::string baseUrl;
    std::string action;     // "save" | "restore"
    std::string filename;
    // Outcome logging only.  A raw pointer is safe on the detached
    // worker because Poco loggers live in the process-lifetime
    // registry — unlike ServerManager members, which the worker must
    // never touch (it can outlive the object).
    Poco::Logger* logger = nullptr;
};

struct SlotActionQueue {
    std::mutex             mutex;
    std::deque<SlotAction> items;
    bool                   workerRunning = false;
    bool                   stop          = false;   // set by ~ServerManager

    // The launch generation currently owning the port.  Set when a
    // server process starts; reset to invalid whenever one is
    // stopped.  Anything popped with a different generation is
    // dropped instead of executed.
    ServerLaunchGeneration liveGeneration = kInvalidServerLaunchGeneration;

    // The session the worker is blocked on right now, if any.
    // Published so the UI thread can abort() it at stop time rather
    // than letting a multi-GB save run out its 120s timeout against
    // a process that no longer exists.  Poco's abort() closes the
    // socket from another thread; the blocking call then throws and
    // lands in the worker's existing catch(...).
    std::shared_ptr<Poco::Net::HTTPClientSession> inFlight;
};

// Clear queued actions, invalidate the live generation, and abort
// whatever the worker is blocked on.  Safe to call with a null queue.
static void AbandonSlotQueue(const std::shared_ptr<SlotActionQueue>& q,
                             Poco::Logger* logger)
{
    if (!q) return;

    std::shared_ptr<Poco::Net::HTTPClientSession> inFlight;
    bool purged = false;
    {
        std::lock_guard<std::mutex> lock(q->mutex);
        purged = !q->items.empty();
        q->items.clear();
        q->liveGeneration = kInvalidServerLaunchGeneration;
        inFlight = q->inFlight;
    }

    if (inFlight) {
        try { inFlight->abort(); } catch (...) { /* already dead */ }
    }

    if (logger && (purged || inFlight))
        logger->information(
            "kvslot: queue abandoned (server stopping)");
}

ServerManager::ServerManager(wxEvtHandler* eventHandler,
                             std::weak_ptr<std::atomic<bool>> aliveToken,
                             Poco::Logger* logger)
    : m_eventHandler(eventHandler)
    , m_aliveToken(aliveToken)
    , m_logger(logger)
    // Lifecycle state, not optional work state.  Creating the queue here
    // guarantees StartServer() can stamp every successful launch with its
    // generation before the first save/restore is ever enqueued.  The old
    // lazy construction path created the queue after launch and left
    // liveGeneration invalid, so every first-launch KV action was dropped.
    , m_slotQueue(std::make_shared<SlotActionQueue>())
{
}

ServerManager::~ServerManager()
{
    // Tell a pending slot-action worker to exit after its in-flight
    // action instead of POSTing queued actions to the server that
    // StopServer() is about to kill.  No join — the worker owns a
    // shared_ptr to the queue block, so this never blocks the UI
    // thread on a network timeout.
    std::shared_ptr<Poco::Net::HTTPClientSession> inFlight;
    if (m_slotQueue) {
        std::lock_guard<std::mutex> lock(m_slotQueue->mutex);
        m_slotQueue->stop = true;
        m_slotQueue->liveGeneration = kInvalidServerLaunchGeneration;
        m_slotQueue->items.clear();
        inFlight = m_slotQueue->inFlight;
    }
    if (inFlight) {
        try { inFlight->abort(); } catch (...) { /* already dead */ }
    }

    StopServer();
}

std::string ServerManager::GetBaseUrl() const
{
    return "http://127.0.0.1:" + std::to_string(m_port);
}

// ═════════════════════════════════════════════════════════════════
//  KV SLOT STATE — instant conversation switching
// ═════════════════════════════════════════════════════════════════

// Defined below beside GetCacheDir(); forward-declared because the
// slot helpers here precede the directory-accessor cluster.
static std::string GetKvSlotCacheDir();

// ── KV cache key hashing ─────────────────────────────────────────
// FNV-1a/64 over a byte string.  Not cryptographic and doesn't need
// to be: the cache dir holds ~6 live files (see PruneKvSlotCacheDir),
// so collision probability is negligible, and the only property
// required is that it be stable across runs and across machines.
// Written inline rather than pulled from Poco so the value can never
// drift with a dependency version.
static uint64_t KvKeyHash(const std::string& s)
{
    uint64_t h = 1469598103934665603ULL;          // FNV offset basis
    for (unsigned char c : s) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ULL;                    // FNV prime
    }
    return h;
}

static std::string KvKeyHashHex(const std::string& s)
{
    static const char* kHex = "0123456789abcdef";
    uint64_t h = KvKeyHash(s);
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) { out[i] = kHex[h & 0xF]; h >>= 4; }
    return out;
}

// Cache filename for a (model, conversation) pair.  Keyed by BOTH so
// a conversation reopened under a different model simply finds no
// state file — restoring KV serialized by another model into the
// running one would be undefined behavior at best.  Sanitized to
// [A-Za-z0-9._-] because llama-server treats the filename as a path
// component under --slot-save-path.
//
// The key is a hash of the FULL normalized paths, not the file stems.
// Stems alone are not unique: D:\Models\A\model.gguf and
// D:\Models\B\model.gguf share a stem, as do same-named conversations
// in different folders, and the 60-char truncation merged long names
// that differed only past that point (quant variants of one base
// model are the realistic case).  Any of those collisions meant one
// conversation silently overwriting another's KV state, or restoring
// state serialized by a different model — the exact undefined
// behavior the (model, conversation) keying exists to prevent.
//
// The stems are retained purely so the cache dir stays readable when
// inspected by hand; uniqueness lives entirely in the hash, so they
// are truncated harder than before to keep the total path well clear
// of Windows' MAX_PATH.
static std::string SlotCacheFilenameFor(const std::string& modelGgufPath,
                                        const std::string& conversationPath)
{
    if (modelGgufPath.empty() || conversationPath.empty()) return "";

    auto stem = [](const std::string& path) -> std::string {
        wxFileName fn(wxString::FromUTF8(path));
        std::string out = fn.GetName().ToUTF8().data();
        for (char& c : out) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
            if (!ok) c = '-';
        }
        if (out.size() > 32) out.resize(32);
        return out;
    };

    // Normalize before hashing so the same file referenced with mixed
    // separators or mixed case yields the same key.  Deliberately does
    // NOT collapse duplicate slashes: merging "//server/share" with
    // "/server/share" would introduce a new collision source, and a
    // spurious cache MISS (slow but correct) is always preferable to a
    // spurious HIT (wrong KV state restored).
    auto normalizeForHash = [](const std::string& path) -> std::string {
        std::string out = path;
        for (char& c : out) {
            if (c == '\\') c = '/';
#ifdef __WXMSW__
            c = static_cast<char>(
                std::tolower(static_cast<unsigned char>(c)));
#endif
        }
        return out;
    };

    // Model identity beyond its path: replacing a .gguf in place —
    // re-downloading a different quant under the same filename — must
    // produce a different key, because llama.cpp's state header does
    // not carry model identity and will happily load mismatched KV.
    //
    // Deliberately NOT applied to the conversation file: that one is
    // rewritten on every message, so folding its mtime into the key
    // would miss the cache on literally every turn.  Both probes are
    // best-effort; if the stat fails the key simply falls back to
    // paths alone.
    std::string modelIdentity;
    {
        wxFileName mf(wxString::FromUTF8(modelGgufPath));
        const wxULongLong sz = mf.GetSize();
        if (sz != wxInvalidSize) {
            modelIdentity += "|sz=";
            modelIdentity += sz.ToString().ToUTF8().data();
        }
        wxDateTime mtime;
        if (mf.GetTimes(nullptr, &mtime, nullptr) && mtime.IsValid()) {
            modelIdentity += "|mt=";
            modelIdentity += std::to_string(
                static_cast<long long>(mtime.GetTicks()));
        }
    }

    // Schema tag is part of the hashed payload: any future change to
    // what goes into the key can bump this and every stale file ages
    // out of the prune window on its own, with no migration code.
    //
    // Separator is a char constant, not a "\x1f" string literal: a hex
    // escape in a string consumes every hex digit that follows it, so
    // appending a literal beginning with [0-9a-f] to one would silently
    // change the escape rather than concatenate. A path fragment can
    // easily start with a hex digit.
    const char kSep = '\x1f';
    const std::string payload =
        std::string("kvslot-v2") + kSep + normalizeForHash(modelGgufPath) +
        modelIdentity + kSep + normalizeForHash(conversationPath);

    std::string m = stem(modelGgufPath);
    std::string c = stem(conversationPath);
    if (m.empty() || c.empty()) return "";
    return m + "__" + c + "__" + KvKeyHashHex(payload) + ".kvbin";
}

// ── Slot-action response helpers ─────────────────────────────────
// llama-server's save/restore responses carry useful telemetry:
// {"n_saved":..,"n_written":..,"timings":{"save_ms":..}} and
// {"n_restored":..,"n_read":..,"timings":{"restore_ms":..}}.
// Field names have drifted across versions, so everything is parsed
// as optional — an empty string just means "log without details".
static std::string DescribeSlotActionResult(const std::string& action,
                                            const std::string& body)
{
    try {
        Poco::JSON::Parser p;
        auto var = p.parse(body);
        auto obj = var.extract<Poco::JSON::Object::Ptr>();
        if (!obj) return "";

        std::string out;
        const char* nKey = (action == "restore") ? "n_restored" : "n_saved";
        if (obj->has(nKey))
            out += std::string(nKey) + "=" + obj->get(nKey).toString();

        if (obj->has("timings")) {
            auto t = obj->getObject("timings");
            const char* msKey = (action == "restore") ? "restore_ms"
                                                      : "save_ms";
            if (t && t->has(msKey)) {
                if (!out.empty()) out += ", ";
                out += t->get(msKey).toString() + " ms";
            }
        }
        return out;
    } catch (...) {
        return "";
    }
}

// Control characters flattened, hard cap — error bodies go into a
// single log line, and llama-server error JSON can embed newlines.
static std::string SanitizeLogSnippet(std::string s)
{
    for (char& c : s)
        if (static_cast<unsigned char>(c) < 0x20) c = ' ';
    if (s.size() > 200) { s.resize(200); s += "..."; }
    return s;
}

// Blocking POST /slots/0?action=<save|restore>.  Runs on the worker
// thread only.  Value copies, no UI, no ServerManager member access —
// the worker can outlive the object.  Failures are non-fatal but no
// longer silent: server gone, action rejected, timeout are all
// equivalent to "no fast path this time" with full reprocess as the
// fallback, and each outcome is logged so the fast path's actual hit
// rate is measurable instead of an article of faith.
static void ExecuteSlotAction(const SlotAction& a,
                              const std::shared_ptr<SlotActionQueue>& q)
{
    // Publishes/retracts the session under the queue mutex so a
    // concurrent stop can abort it.  RAII so an exception anywhere
    // below cannot leave a dangling handle behind.
    struct InFlightGuard {
        std::shared_ptr<SlotActionQueue> q;
        ~InFlightGuard() {
            if (!q) return;
            std::lock_guard<std::mutex> lock(q->mutex);
            q->inFlight.reset();
        }
    } guard{ q };

    try {
        Poco::URI uri(a.baseUrl + "/slots/0?action=" + a.action);
        auto sessPtr = std::make_shared<Poco::Net::HTTPClientSession>(
            uri.getHost(), uri.getPort());
        Poco::Net::HTTPClientSession& sess = *sessPtr;
        sess.setTimeout(Poco::Timespan(120, 0));   // multi-GB states take seconds

        // Authoritative generation check and in-flight publication must be
        // one atomic operation under the queue mutex.  The worker already
        // checks when it pops the action, but StopServer() can invalidate the
        // launch in the gap between that check and session creation.  By
        // re-checking while publishing the session, either:
        //   * stop wins first and this stale action is never sent, or
        //   * this action wins first and stop sees/aborts this exact session
        //     before killing the old server and allowing a replacement launch.
        ServerLaunchGeneration live = kInvalidServerLaunchGeneration;
        bool queueStopped = false;
        bool claimed = false;
        if (q) {
            std::lock_guard<std::mutex> lock(q->mutex);
            live = q->liveGeneration;
            queueStopped = q->stop;
            if (!queueStopped && live == a.generation) {
                q->inFlight = sessPtr;
                claimed = true;
            }
        }

        if (!claimed) {
            if (a.logger) {
                std::string reason;
                if (!q)
                    reason = "queue unavailable";
                else if (queueStopped)
                    reason = "queue stopped";
                else
                    reason = "generation " + std::to_string(a.generation) +
                             " != live " + std::to_string(live);

                a.logger->information(
                    "kvslot: dropped stale " + a.action + " \"" +
                    a.filename + "\" before send (" + reason + ")");
            }
            return;
        }

        const std::string body = "{\"filename\":\"" + a.filename + "\"}";

        Poco::Net::HTTPRequest req(Poco::Net::HTTPRequest::HTTP_POST,
                                   uri.getPathAndQuery(),
                                   Poco::Net::HTTPMessage::HTTP_1_1);
        req.setContentType("application/json");
        req.setContentLength((int)body.size());
        sess.sendRequest(req) << body;

        Poco::Net::HTTPResponse resp;
        std::istream& in = sess.receiveResponse(resp);
        std::string respBody;
        Poco::StreamCopier::copyToString(in, respBody);

        const int status = static_cast<int>(resp.getStatus());
        if (status >= 200 && status < 300) {
            if (a.logger) {
                const std::string details =
                    DescribeSlotActionResult(a.action, respBody);
                a.logger->information(
                    "kvslot: " + a.action + " \"" + a.filename + "\" ok" +
                    (details.empty() ? "" : " (" + details + ")"));
            }
        } else {
            if (a.logger) {
                std::string msg = "kvslot: " + a.action + " \"" + a.filename +
                                  "\" failed (HTTP " + std::to_string(status) +
                                  ") - full reprocess fallback";
                const std::string snippet = SanitizeLogSnippet(respBody);
                if (!snippet.empty()) msg += " | " + snippet;
                a.logger->warning(msg);
            }
        }
    } catch (...) {
        // Connection refused / reset / timeout — the server is gone or
        // going.  Same fallback, logged for the hit-rate picture.
        if (a.logger)
            a.logger->warning("kvslot: " + a.action + " \"" + a.filename +
                              "\" failed (connection error/timeout) - "
                              "full reprocess fallback");
    }
}

void ServerManager::EnqueueSlotAction(const std::string& action,
                                      const std::string& filename)
{
    if (m_logger)
        m_logger->information("kvslot: dispatch " + action + " \"" + filename + "\"");

    // Defensive fallback for future code that may explicitly reset the
    // queue.  Normal construction is eager, but any replacement queue must
    // inherit the launch that currently owns the port before accepting work.
    if (!m_slotQueue) {
        m_slotQueue = std::make_shared<SlotActionQueue>();
        m_slotQueue->liveGeneration = m_launchGeneration;
    }

    std::shared_ptr<SlotActionQueue> q = m_slotQueue;

    bool spawnWorker = false;
    {
        std::lock_guard<std::mutex> lock(q->mutex);
        q->items.push_back({ m_launchGeneration, GetBaseUrl(),
                             action, filename, m_logger });
        if (!q->workerRunning) {
            q->workerRunning = true;
            spawnWorker = true;
        }
    }

    if (spawnWorker) {
        std::thread([q]() {
            for (;;) {
                SlotAction a;
                ServerLaunchGeneration live = kInvalidServerLaunchGeneration;
                {
                    std::lock_guard<std::mutex> lock(q->mutex);
                    if (q->stop || q->items.empty()) {
                        q->workerRunning = false;
                        return;
                    }
                    a = std::move(q->items.front());
                    q->items.pop_front();
                    live = q->liveGeneration;
                }

                // Stale: the server this was queued for is gone, and
                // whatever owns the port now is a different launch.
                // Executing would restore model A's KV into model B,
                // or overwrite model A's cache file with B's slot.
                if (a.generation != live) {
                    if (a.logger)
                        a.logger->information(
                            "kvslot: dropped stale " + a.action + " \"" +
                            a.filename + "\" (generation " +
                            std::to_string(a.generation) + " != live " +
                            std::to_string(live) + ")");
                    continue;
                }

                ExecuteSlotAction(a, q);
            }
        }).detach();
    }
}

// Keep the newest few state files; each can run to gigabytes.  Runs
// on the UI thread before a save dispatch — directory enumeration of
// a handful of files is instant, and doing it here (not in the
// detached thread) keeps filesystem mutation single-threaded.
static void PruneKvSlotCacheDir(Poco::Logger* logger)
{
    const size_t kKeep = 5;   // +1 about to be written = 6 on disk

    wxString root = wxString::FromUTF8(GetKvSlotCacheDir());
    wxDir dir(root);
    if (!dir.IsOpened()) return;

    std::vector<std::pair<time_t, wxString>> files;
    wxString name;
    bool found = dir.GetFirst(&name, "*.kvbin", wxDIR_FILES);
    while (found) {
        wxFileName fn(root, name);
        wxDateTime mtime;
        if (fn.GetTimes(nullptr, &mtime, nullptr))
            files.push_back({ mtime.GetTicks(), name });
        found = dir.GetNext(&name);
    }
    if (files.size() <= kKeep) return;

    std::sort(files.begin(), files.end());   // oldest first
    const size_t doomed = files.size() - kKeep;
    for (size_t i = 0; i < doomed; ++i) {
        wxFileName fn(root, files[i].second);
        if (wxRemoveFile(fn.GetFullPath()) && logger)
            logger->information("kvslot: pruned \"" +
                std::string(files[i].second.ToUTF8().data()) + "\"");
    }
}

void ServerManager::NoteSlotOwner(const std::string& conversationPath)
{
    if (m_loadedModel.empty()) return;   // remote lane / no server

    const std::string fname =
        SlotCacheFilenameFor(m_loadedModel, conversationPath);
    if (fname.empty()) return;

    // ── Purge stale queued slot actions ──────────────────────────
    // A generation for this conversation is dispatching on its own
    // HTTP connection.  Any action still waiting in the CLIENT-side
    // FIFO was enqueued under assumptions this generation is about
    // to invalidate:
    //
    //   restore (this conversation)  — worthless: the generation
    //     rebuilds the slot to a longer prefix than the file holds,
    //     and letting it land AFTER the generation truncates the
    //     slot back to a stale prefix and triggers a redundant
    //     multi-GB rewrite on the next switch-away.
    //   restore (other conversation) — worse: rapid multi-switch can
    //     leave one queued; landing post-generation it clobbers the
    //     slot with foreign KV while ownership says otherwise.
    //   save — the slot's KV at execution time will be THIS
    //     conversation's, not the state the save was enqueued to
    //     capture; writing it would corrupt that conversation's
    //     cache file.  Dropping it merely skips one incremental
    //     update — the older file on disk stays prefix-valid.
    //
    // Only queued items are touched.  The in-flight action (already
    // popped by the worker) is safe either way: once its POST reaches
    // the server, llama-server's per-slot task queue serializes the
    // generation behind it in arrival order.
    bool purged = false;
    if (m_slotQueue) {
        std::lock_guard<std::mutex> lock(m_slotQueue->mutex);
        purged = !m_slotQueue->items.empty();
        m_slotQueue->items.clear();
    }
    if (purged && m_logger)
        m_logger->information(
            "kvslot: purged queued action(s) superseded by generation "
            "dispatch for \"" + fname + "\"");

    m_slotOwner = fname;
    m_slotDirty = true;
}

void ServerManager::InvalidateSlotOwner()
{
    // Called at the dispatch of any generation that runs against the
    // local slot with a throwaway history (goal contract builder,
    // goal verifier, Skill draft builder).  The slot's KV is about to
    // hold content belonging to no conversation; forgetting the owner
    // makes the next switch-away skip its save instead of serializing
    // that state under the active conversation's filename.  Stamped
    // ownership returns naturally on the next real conversation
    // request via NoteSlotOwner.
    if (m_slotOwner.empty() && !m_slotDirty) return;

    m_slotOwner.clear();
    m_slotDirty = false;

    if (m_logger)
        m_logger->information(
            "kvslot: ownership invalidated (out-of-conversation generation)");
}

void ServerManager::SaveSlotStateForConversation(const std::string& conversationPath)
{
    if (m_loadedModel.empty()) return;

    const std::string fname =
        SlotCacheFilenameFor(m_loadedModel, conversationPath);
    if (fname.empty()) return;

    // The slot must verifiably hold THIS conversation's KV, and a
    // generation must have run since the last save/restore.  Anything
    // else is either a stale slot (would save the wrong conversation's
    // state under this name) or a redundant rewrite of what a restore
    // just loaded.
    if (m_slotOwner != fname || !m_slotDirty) return;

    PruneKvSlotCacheDir(m_logger);
    EnqueueSlotAction("save", fname);
    m_slotDirty = false;
}

void ServerManager::RestoreSlotStateForConversation(const std::string& conversationPath)
{
    if (m_loadedModel.empty()) return;

    const std::string fname =
        SlotCacheFilenameFor(m_loadedModel, conversationPath);
    if (fname.empty()) return;

    // Client-side existence check — no state file means a first visit
    // (or a different model last time); skip the round-trip entirely.
    wxFileName fn(wxString::FromUTF8(GetKvSlotCacheDir()),
                  wxString::FromUTF8(fname));
    if (!fn.FileExists()) return;

    EnqueueSlotAction("restore", fname);
    m_slotOwner = fname;
    m_slotDirty = false;
}

std::string ServerManager::ModelDisplayName(const std::string& ggufPath)
{
    // Prefer the bundle folder name when the model is bundled — this
    // surfaces clean, user-chosen names ("gemma-3-27b-it-abliterated")
    // in the UI instead of noisy quantization-tagged filenames
    // ("gemma-3-27b-it-abliterated-q4_k_m"). For loose files (power
    // mode, or dropped into the default folder without a bundle),
    // fall back to the .gguf filename stem — matches legacy behavior.
    wxFileName fn(wxString::FromUTF8(ggufPath.c_str()));
    wxString modelFolder = fn.GetPath();

    if (IsBundledModelPath(ggufPath)) {
        // Return the last path component (the bundle folder name).
        wxFileName folderFn = wxFileName::DirName(modelFolder);
        const wxArrayString& dirs = folderFn.GetDirs();
        if (!dirs.IsEmpty())
            return std::string(dirs.Last().ToUTF8().data());
    }

    // Loose file — "gemma-2-9b-it-Q4_K_M.gguf" → "gemma-2-9b-it-Q4_K_M"
    return fn.GetName().ToUTF8().data();
}

// ── Backend detection ────────────────────────────────────────────

Backend ServerManager::DetectBackend()
{
#ifdef __WXMSW__
    // Check if NVIDIA CUDA runtime is available
    HMODULE hCuda = LoadLibraryA("nvcuda.dll");
    if (hCuda) {
        FreeLibrary(hCuda);
        return Backend::CUDA12;
    }
#endif
    return Backend::CPU;
}

// ── Find llama-server binary ─────────────────────────────────────

std::string ServerManager::FindServerBinary(Backend backend)
{
    wxString exePath = wxStandardPaths::Get().GetExecutablePath();
    wxFileName exeFn(exePath);
    wxString exeDir = exeFn.GetPath();

    wxString subdir = (backend == Backend::CUDA12) ? "cuda12" : "cpu";
    wxString serverExe = "llama-server.exe";

    // Helper: check candidate directories for the server binary
    auto tryDir = [&](const wxString& dir) -> std::string {
        // Try with backend subfolder: bin\cuda12\llama-server.exe
        wxString p1 = dir + wxFILE_SEP_PATH + "bin" + wxFILE_SEP_PATH
                     + subdir + wxFILE_SEP_PATH + serverExe;
        if (wxFileExists(p1)) return p1.ToUTF8().data();

        // Try flat: bin\llama-server.exe (simple dev setup)
        wxString p2 = dir + wxFILE_SEP_PATH + "bin" + wxFILE_SEP_PATH + serverExe;
        if (wxFileExists(p2)) return p2.ToUTF8().data();

        return "";
    };

    // Walk up from the exe directory, checking each level.
    // Covers both installed layout (bin\ next to exe) and
    // VS dev layouts (exe in x64\Debug\, x64\Release\, etc.)
    wxFileName walkDir = wxFileName::DirName(exeDir);
    for (int level = 0; level < 4; ++level) {
        std::string found = tryDir(walkDir.GetPath());
        if (!found.empty()) return found;

        // Move one directory up
        walkDir.RemoveLastDir();
    }

    return ""; // Not found
}

// ── Data directories ─────────────────────────────────────────────

std::string ServerManager::GetDataDir()
{
    // wxStandardPaths uses the app name set via wxApp::SetAppName().
    // Falls back to %LOCALAPPDATA%\LlamaBoss.
    wxString dir = wxStandardPaths::Get().GetUserLocalDataDir();
    return dir.ToUTF8().data();
}

std::string ServerManager::GetDefaultModelsDir()
{
    // The hardcoded "home" folder for LlamaBoss-managed models. Never
    // changes across runs or installs, so the UI can always display it
    // and "Reset to default" always points here. Casual users never
    // move away from this path.
    return GetDataDir() + std::string(1, wxFILE_SEP_PATH) + "models";
}

std::string ServerManager::GetModelsDirOverride()
{
    // Read the user's custom path from wxFileConfig. An empty value
    // (or missing key) means "no override — use the default".
    wxFileConfig cfg("LlamaBoss");
    wxString path;
    if (cfg.Read("ModelsFolderOverride", &path) && !path.IsEmpty())
        return std::string(path.ToUTF8().data());
    return "";
}

void ServerManager::SetModelsDirOverride(const std::string& path)
{
    wxFileConfig cfg("LlamaBoss");
    cfg.Write("ModelsFolderOverride", wxString::FromUTF8(path));
    cfg.Flush();
}

bool ServerManager::IsCasualMode()
{
    // Casual = no override set, scanning LlamaBoss's own folder.
    // Power  = override set, user is in control.
    return GetModelsDirOverride().empty();
}

std::string ServerManager::GetModelsDir()
{
    // The active scan root: override if set, default otherwise.
    // This is what the scanner, downloader, and UI all consume.
    std::string ov = GetModelsDirOverride();
    return ov.empty() ? GetDefaultModelsDir() : ov;
}
std::string ServerManager::GetLogsDir()           { return GetDataDir() + std::string(1, wxFILE_SEP_PATH) + "logs"; }
std::string ServerManager::GetConfigDir()         { return GetDataDir() + std::string(1, wxFILE_SEP_PATH) + "config"; }
std::string ServerManager::GetConversationsDir()  { return GetDataDir() + std::string(1, wxFILE_SEP_PATH) + "conversations"; }
std::string ServerManager::GetCacheDir()          { return GetDataDir() + std::string(1, wxFILE_SEP_PATH) + "cache"; }
static std::string GetKvSlotCacheDir()            { return ServerManager::GetCacheDir() + std::string(1, wxFILE_SEP_PATH) + "kvslots"; }

void ServerManager::EnsureDataDirs()
{
    wxFileName::Mkdir(GetDataDir(),          wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    wxFileName::Mkdir(GetModelsDir(),        wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    wxFileName::Mkdir(GetLogsDir(),          wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    wxFileName::Mkdir(GetConfigDir(),        wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    wxFileName::Mkdir(GetConversationsDir(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    wxFileName::Mkdir(GetCacheDir(),         wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    wxFileName::Mkdir(GetKvSlotCacheDir(),   wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);

    // Workspace lives outside %LOCALAPPDATA% (under Documents) so the
    // user can find it from File Explorer. Created here at startup
    // so the agent always has a writeable home, even on a clean
    // first run before any /cd has been issued.
    EnsureWorkspaceDir();
}

// ── Workspace ──────────────────────────────────────────────────────
// Mirrors the models-folder helpers above: a hardcoded default path,
// an optional user override stored in wxFileConfig, and a single
// "active" accessor that returns the override when set and the
// default otherwise.
//
// The default user-visible workspace intentionally lives directly
// under the user's profile folder instead of Documents. On many
// Windows machines, Documents is silently redirected to OneDrive;
// using %USERPROFILE%\LlamaBoss keeps LlamaBoss's local-first output
// from being cloud-synced by surprise while still being easy to find.

static std::string JoinPath(const std::string& a, const std::string& b)
{
    if (a.empty()) return b;
    const char sep = wxFILE_SEP_PATH;
    if (a.back() == sep || a.back() == '/' || a.back() == '\\') return a + b;
    return a + std::string(1, sep) + b;
}

static std::string GetDefaultWorkspaceRootDir()
{
#ifdef __WXMSW__
    wxString userProfile;
    if (wxGetEnv("USERPROFILE", &userProfile) && !userProfile.IsEmpty()) {
        return JoinPath(std::string(userProfile.ToUTF8().data()), "LlamaBoss");
    }
#endif

    wxString home = wxGetHomeDir();
    if (!home.IsEmpty()) {
        return JoinPath(std::string(home.ToUTF8().data()), "LlamaBoss");
    }

    // Last-resort fallback only. This should rarely be used, but keeps
    // the app functional if the profile directory cannot be resolved.
    wxString docs = wxStandardPaths::Get().GetDocumentsDir();
    return JoinPath(std::string(docs.ToUTF8().data()), "LlamaBoss");
}

std::string ServerManager::GetDefaultWorkspaceDir()
{
    // Active default working directory for tools. Future document lanes
    // are created beside it by EnsureWorkspaceDir().
    return JoinPath(GetDefaultWorkspaceRootDir(), "Workspace");
}

// ─── Conversation lane layout ─────────────────────────────────────
// Single source of truth for recognizing the per-conversation folder
// shape created by ChatHistory::EnsureWorkflowDir():
//   %USERPROFILE%\LlamaBoss\Workflows\chat_xxxxxxxx\Workspace
// python_runner.cpp and agent_controller.cpp both delegate here; see
// the header comment for why the duplication was a safety hazard.

namespace {

std::string LaneTrimTrailingSeparators(std::string s)
{
    while (!s.empty() && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    return s;
}

std::string LaneParentDirOf(const std::string& path)
{
    std::string s = LaneTrimTrailingSeparators(path);
    size_t pos = s.find_last_of("/\\");
    if (pos == std::string::npos) return std::string();
    return s.substr(0, pos);
}

std::string LaneBaseNameOf(const std::string& path)
{
    std::string s = LaneTrimTrailingSeparators(path);
    size_t pos = s.find_last_of("/\\");
    return (pos == std::string::npos) ? s : s.substr(pos + 1);
}

std::string LaneLowerAscii(std::string s)
{
    for (char& ch : s) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + 32);
    }
    return s;
}

} // namespace

std::string ServerManager::GetLlamaBossRootDir()
{
    // Identical by construction to ParentDirOf(GetDefaultWorkspaceDir())
    // — GetDefaultWorkspaceDir is GetDefaultWorkspaceRootDir() +
    // "\Workspace" — but skips the string round-trip.
    return GetDefaultWorkspaceRootDir();
}

std::string ServerManager::ConversationWorkflowRootFromCwd(const std::string& cwd)
{
    std::string clean = LaneTrimTrailingSeparators(cwd);
    if (clean.empty()) return std::string();

    if (LaneLowerAscii(LaneBaseNameOf(clean)) != "workspace") return std::string();

    std::string chatRoot      = LaneParentDirOf(clean);     // ...\chat_xxxxxxxx
    std::string workflowsRoot = LaneParentDirOf(chatRoot);  // ...\Workflows
    if (chatRoot.empty() || workflowsRoot.empty()) return std::string();

    std::string chatBase = LaneLowerAscii(LaneBaseNameOf(chatRoot));
    if (chatBase.rfind("chat_", 0) != 0) return std::string();
    if (LaneLowerAscii(LaneBaseNameOf(workflowsRoot)) != "workflows") return std::string();

    return chatRoot;
}

std::string ServerManager::ConversationLaneDirForCwd(const std::string& cwd,
                                                     const std::string& lane)
{
    std::string root = ConversationWorkflowRootFromCwd(cwd);
    if (root.empty()) root = GetLlamaBossRootDir();
    if (root.empty()) return std::string();
    return JoinPath(root, lane);
}

std::string ServerManager::ConversationScriptsDirForCwd(const std::string& cwd)
{
    return ConversationLaneDirForCwd(cwd, "Scripts");
}

std::string ServerManager::GetWorkspaceDirOverride()
{
    // Read the user's custom workspace path from wxFileConfig. Empty
    // (or missing key) means "use the default".
    wxFileConfig cfg("LlamaBoss");
    wxString path;
    if (cfg.Read("WorkspaceFolderOverride", &path) && !path.IsEmpty())
        return std::string(path.ToUTF8().data());
    return "";
}

void ServerManager::SetWorkspaceDirOverride(const std::string& path)
{
    wxFileConfig cfg("LlamaBoss");
    cfg.Write("WorkspaceFolderOverride", wxString::FromUTF8(path));
    cfg.Flush();
}

std::string ServerManager::GetWorkspaceDir()
{
    // Active workspace: override if set, default otherwise. This is
    // what ResolveCurrentCwd() in MyFrame consults as the fallback
    // when no /cd override is set on the current conversation.
    std::string ov = GetWorkspaceDirOverride();
    return ov.empty() ? GetDefaultWorkspaceDir() : ov;
}

void ServerManager::EnsureWorkspaceDir()
{
    // Idempotent: wxPATH_MKDIR_FULL is "mkdir -p" semantics, so this
    // is a cheap no-op when the directory already exists.
    wxFileName::Mkdir(GetWorkspaceDir(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);

    // For the built-in default only, create the full LlamaBoss folder
    // layout. If the user later picks a custom workspace folder, respect
    // that exact override and do not create sibling folders around it.
    if (GetWorkspaceDirOverride().empty()) {
        const std::string root = GetDefaultWorkspaceRootDir();
        wxFileName::Mkdir(root, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);

        const char* lanes[] = {
            "Workspace",
            "Documents",
            "Spreadsheets",
            "PDFs",
            "Scripts",
            "Downloads"
        };

        for (const char* lane : lanes) {
            wxFileName::Mkdir(JoinPath(root, lane), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Model scanning
// ═══════════════════════════════════════════════════════════════════
//
// Two layouts are supported, selected by whether the user has set a
// custom models folder:
//
//   CASUAL MODE (override not set — scanning the default folder):
//     models/
//     ├── gemma-3-27b-it-abliterated/         ← bundle subfolder
//     │   ├── gemma-3-27b-it-abliterated-q4_k_m.gguf
//     │   └── mmproj-f16.gguf                 ← paired automatically
//     └── gemma-4-e4b-it/                     ← text-only bundle
//         └── gemma-4-e4b-it-f16.gguf
//
//     Each subfolder is a "bundle" — one model + optional mmproj.
//     Pairing is deterministic (same folder = paired); no filename
//     scoring, no ambiguity.
//
//   POWER MODE (override set — user picked their own folder):
//     my-models/
//     ├── gemma-3-27b-it-abliterated-q4_k_m.gguf  ← loose files
//     ├── mmproj-gemma-3.gguf
//     └── qwen2.5-7b-instruct-q4_k_m.gguf
//
//     Flat layout; mmproj pairing falls back to filename heuristics.
//     User opted in to approximate matching by choosing this layout.
//
// The two modes share this function's signature so callers don't care.

// ── Internal: is this filename an mmproj projector? ──────────────
static bool IsMmprojFilename(const wxString& filename)
{
    return filename.Lower().Contains("mmproj");
}

// ── Internal: is this filename a speculative draft model? ────────
static bool IsDraftFilename(const wxString& filename)
{
    return filename.Lower().Contains("draft");
}

// ── Internal: collect bundle-local mmproj candidates ──────────────
// wxDir enumeration order is not a stable user-facing choice. Keep the
// collection sorted so diagnostics are repeatable, but do not use sorting
// to silently choose among multiple projector files in a bundle.
static std::vector<wxString> CollectBundleMmprojs(const wxString& bundleDir)
{
    std::vector<wxString> projs;

    wxDir dir(bundleDir);
    if (!dir.IsOpened()) return projs;

    wxString filename;
    bool found = dir.GetFirst(&filename, "*mmproj*.gguf", wxDIR_FILES);
    while (found) {
        projs.push_back(filename);
        found = dir.GetNext(&filename);
    }

    std::sort(projs.begin(), projs.end(),
        [](const wxString& a, const wxString& b) {
            return a.CmpNoCase(b) < 0;
        });

    return projs;
}

static std::string JoinBundleFileUtf8(const wxString& bundleDir,
                                      const wxString& filename)
{
    return (bundleDir + wxFILE_SEP_PATH + filename).ToUTF8().data();
}

// Resolve a projector for a bundle only when there is exactly one.
// Multiple projectors are safer to skip than to pair incorrectly.
static std::string ResolveBundleMmproj(const wxString& bundleDir,
                                       Poco::Logger* logger = nullptr)
{
    std::vector<wxString> projs = CollectBundleMmprojs(bundleDir);
    const std::string bundlePath = bundleDir.ToUTF8().data();

    if (projs.empty()) {
        if (logger) {
            logger->information(
                "mmproj: no projector in bundle \"" + bundlePath +
                "\" — launching text-only");
        }
        return "";
    }

    if (projs.size() == 1) {
        std::string result = JoinBundleFileUtf8(bundleDir, projs[0]);
        if (logger)
            logger->information("mmproj: bundled pair \"" + result + "\"");
        return result;
    }

    if (logger) {
        logger->warning(
            "mmproj: multiple projector files found in bundle \"" +
            bundlePath +
            "\"; skipping projector pairing until only one remains");

        for (const wxString& proj : projs) {
            logger->warning(
                "mmproj:   candidate \"" +
                JoinBundleFileUtf8(bundleDir, proj) + "\"");
        }
    }

    return "";
}

// ── Internal: resolve a bundle's speculative draft model ─────────
// Mirrors ResolveBundleMmproj exactly: a single *draft*.gguf in the
// bundle pairs automatically; zero means no speculative decoding;
// more than one is ambiguous and skipped with a warning.  Filesystem
// structure as configuration — no UI, same guarantee as mmproj.
static std::string ResolveBundleDraft(const wxString& bundleDir,
                                      Poco::Logger* logger = nullptr)
{
    std::vector<wxString> drafts;
    {
        wxDir dir(bundleDir);
        if (dir.IsOpened()) {
            wxString filename;
            bool found = dir.GetFirst(&filename, "*.gguf", wxDIR_FILES);
            while (found) {
                if (IsDraftFilename(filename) && !IsMmprojFilename(filename))
                    drafts.push_back(filename);
                found = dir.GetNext(&filename);
            }
            std::sort(drafts.begin(), drafts.end());
        }
    }

    const std::string bundlePath = bundleDir.ToUTF8().data();

    if (drafts.empty()) return "";

    if (drafts.size() == 1) {
        std::string result = JoinBundleFileUtf8(bundleDir, drafts[0]);
        if (logger)
            logger->information("draft: bundled pair \"" + result + "\"");
        return result;
    }

    if (logger) {
        logger->warning(
            "draft: multiple draft files found in bundle \"" + bundlePath +
            "\"; skipping speculative pairing until only one remains");
        for (const wxString& d : drafts) {
            logger->warning(
                "draft:   candidate \"" + JoinBundleFileUtf8(bundleDir, d) + "\"");
        }
    }
    return "";
}

// ── Internal: scan a single bundle subfolder ─────────────────────
// Returns a ModelEntry if the folder contains exactly one non-mmproj
// .gguf. Returns an empty entry (ggufPath empty) otherwise — skip it.
// Bundles with multiple .gguf weights are ambiguous and unsupported.
static ServerManager::ModelEntry ScanBundle(const wxString& bundleDir,
                                            const wxString& bundleName)
{
    ServerManager::ModelEntry entry;

    wxDir dir(bundleDir);
    if (!dir.IsOpened()) return entry;

    std::vector<wxString> weights;   // non-mmproj .gguf files

    wxString filename;
    bool found = dir.GetFirst(&filename, "*.gguf", wxDIR_FILES);
    while (found) {
        // mmproj and *draft* files are bundle companions, not weights —
        // without the draft exclusion, dropping a speculative draft into
        // a bundle would make the bundle read as "two weights, ambiguous"
        // and the model would vanish from the list.
        if (!IsMmprojFilename(filename) && !IsDraftFilename(filename))
            weights.push_back(filename);
        found = dir.GetNext(&filename);
    }

    // Valid bundle: exactly one weight file. Zero = empty folder, skip.
    // More than one = ambiguous; we'd have to guess and we don't.
    if (weights.size() != 1) return entry;

    entry.ggufPath    = (bundleDir + wxFILE_SEP_PATH + weights[0]).ToUTF8().data();
    entry.displayName = bundleName.ToUTF8().data();
    entry.bundleDir   = bundleDir.ToUTF8().data();
    entry.isBundle    = true;

    // Pair only when the bundle has exactly one projector. If multiple
    // mmproj files exist, leave the model usable as text-only rather than
    // silently attaching the wrong vision projector. StartServer() repeats
    // this check with logging because current callers pass only ggufPath.
    entry.mmprojPath = ResolveBundleMmproj(bundleDir);

    return entry;
}

std::vector<ServerManager::ModelEntry> ServerManager::ScanModels()
{
    std::vector<ModelEntry> entries;
    wxString root = GetModelsDir();
    if (!wxDir::Exists(root)) return entries;

    wxDir dir(root);
    if (!dir.IsOpened()) return entries;

    const bool casualMode = IsCasualMode();

    if (casualMode) {
        // ── Casual: enumerate subfolders as bundles ──────────────
        // Loose .gguf files at the root ARE also picked up — this
        // handles the case where a user drops a file into the default
        // folder directly instead of downloading through the app. It
        // keeps things working, but such files won't have mmproj
        // pairing; users get the benefits when they organize.
        wxString subfolder;
        bool foundDir = dir.GetFirst(&subfolder, wxEmptyString, wxDIR_DIRS);
        while (foundDir) {
            wxString bundlePath = root + wxFILE_SEP_PATH + subfolder;
            ModelEntry e = ScanBundle(bundlePath, subfolder);
            if (!e.ggufPath.empty())
                entries.push_back(std::move(e));
            foundDir = dir.GetNext(&subfolder);
        }

        // Fall-through: pick up loose .gguf files at the root.
        // These show up as unbundled entries — usable but without
        // automatic mmproj pairing.
        wxString filename;
        bool foundFile = dir.GetFirst(&filename, "*.gguf", wxDIR_FILES);
        while (foundFile) {
            if (!IsMmprojFilename(filename)) {
                wxString fullPath = root + wxFILE_SEP_PATH + filename;
                ModelEntry e;
                e.ggufPath = fullPath.ToUTF8().data();
                // Use filename stem as display name (matches legacy UX
                // so users who dropped files in don't see a regression).
                wxFileName fn(fullPath);
                e.displayName = fn.GetName().ToUTF8().data();
                e.isBundle = false;
                entries.push_back(std::move(e));
            }
            foundFile = dir.GetNext(&filename);
        }
    }
    else {
        // ── Power: flat scan (original behavior) ─────────────────
        wxString filename;
        bool foundFile = dir.GetFirst(&filename, "*.gguf", wxDIR_FILES);
        while (foundFile) {
            if (!IsMmprojFilename(filename)) {
                wxString fullPath = root + wxFILE_SEP_PATH + filename;
                ModelEntry e;
                e.ggufPath = fullPath.ToUTF8().data();
                wxFileName fn(fullPath);
                e.displayName = fn.GetName().ToUTF8().data();
                e.isBundle = false;
                entries.push_back(std::move(e));
            }
            foundFile = dir.GetNext(&filename);
        }
        // Power-mode mmproj pairing is resolved at server-start time
        // via FindMatchingMmproj (filename scoring). We don't populate
        // mmprojPath here because the pairing depends on which model
        // the user actually selects.
    }

    std::sort(entries.begin(), entries.end(),
        [](const ModelEntry& a, const ModelEntry& b) {
            return a.displayName < b.displayName;
        });

    return entries;
}

std::vector<std::string> ServerManager::ScanModelPaths()
{
    // Legacy path-only view — thin wrapper over ScanModels for callers
    // that just want .gguf paths. New code should use ScanModels().
    std::vector<std::string> paths;
    auto entries = ScanModels();
    paths.reserve(entries.size());
    for (const auto& e : entries)
        paths.push_back(e.ggufPath);
    return paths;
}

// ── Token-based mmproj matcher ──────────────────────────────────
//
// Tokenises a GGUF filename (split on - _ .), lowercases everything,
// strips quant/format noise tokens (q4, k_m, f16, bf16, gguf, mmproj),
// then scores each *mmproj*.gguf candidate by shared-token count.
// Returns the full path of the best match, or empty if nothing fits.

// Noise tokens that appear in filenames but carry no model-identity info.
// Split on '- _ .' so only the actual tokens produced by the tokenizer
// need to appear here (e.g. "k_m" never appears because it splits into
// "k" and "m").
static const std::set<std::string> kNoiseTokens = {
    "gguf", "mmproj",
    // Quant tier
    "q2", "q3", "q4", "q5", "q6", "q8",
    // K-quant suffix letters (from Q3_K_M, Q4_K_S, Q5_K_L etc.)
    "k", "m", "s", "l",
    // Imatrix quant tiers & suffixes
    "i1", "iq1", "iq2", "iq3", "iq4",
    "xxs", "xs", "nl",
    // Unsloth dynamic quants
    "ud", "xl",
    // Precision tags
    "f16", "f32", "bf16", "fp16", "fp32",
    // Finetune-type suffixes (shared mmproj across finetunes of same arch)
    "it", "instruct", "chat", "base",
};

static std::set<std::string> TokeniseFilename(const wxString& filename)
{
    // Work on the stem (no extension)
    wxFileName fn(filename);
    std::string stem = std::string(fn.GetName().Lower().ToUTF8().data());

    std::set<std::string> tokens;
    std::string tok;
    for (char c : stem) {
        if (c == '-' || c == '_' || c == '.') {
            if (!tok.empty()) {
                if (kNoiseTokens.find(tok) == kNoiseTokens.end())
                    tokens.insert(tok);
                tok.clear();
            }
        } else {
            tok += c;
        }
    }
    if (!tok.empty() && kNoiseTokens.find(tok) == kNoiseTokens.end())
        tokens.insert(tok);

    return tokens;
}

std::string ServerManager::FindMatchingMmproj(const std::string& modelGgufPath,
                                              Poco::Logger* logger)
{
    wxString modelsDir = GetModelsDir();
    if (!wxDir::Exists(modelsDir))
        return "";

    wxDir dir(modelsDir);
    if (!dir.IsOpened())
        return "";

    // ── Collect all *mmproj*.gguf candidates ──
    struct Candidate { wxString fullPath; int score; };
    std::vector<Candidate> candidates;

    wxString filename;
    bool found = dir.GetFirst(&filename, "*mmproj*.gguf", wxDIR_FILES);
    while (found) {
        wxString fullPath = modelsDir + wxFILE_SEP_PATH + filename;
        candidates.push_back({ fullPath, 0 });
        found = dir.GetNext(&filename);
    }

    if (candidates.empty())
        return "";

    // ── Tokenise the model name and score each candidate ──
    wxFileName modelFn(modelGgufPath);
    std::set<std::string> modelTokens = TokeniseFilename(modelFn.GetFullName());

    int bestScore = 0;
    int bestIdx   = -1;

    for (size_t i = 0; i < candidates.size(); ++i) {
        wxFileName candFn(candidates[i].fullPath);
        std::set<std::string> candTokens = TokeniseFilename(candFn.GetFullName());

        int score = 0;
        for (const auto& t : candTokens) {
            if (modelTokens.count(t))
                ++score;
        }
        candidates[i].score = score;

        if (score > bestScore) {
            bestScore = score;
            bestIdx   = static_cast<int>(i);
        }
        else if (score == bestScore && score > 0 && bestIdx >= 0) {
            // Tie — break deterministically by filename (case-insensitive,
            // lexicographic). Without this, the "winner" depends on whatever
            // order the OS returned entries, which is non-portable and can
            // flip between runs on the same machine.
            wxString a = candidates[i].fullPath.Lower();
            wxString b = candidates[bestIdx].fullPath.Lower();
            if (a < b) bestIdx = static_cast<int>(i);
        }
    }

    // Detect ambiguous ties among the top candidates so the user can see
    // *why* a particular projector was picked (or why none was).
    int tiedWithBest = 0;
    if (bestIdx >= 0) {
        for (size_t i = 0; i < candidates.size(); ++i) {
            if (static_cast<int>(i) != bestIdx &&
                candidates[i].score == bestScore)
                ++tiedWithBest;
        }
    }

    // ── Single candidate → use only when generic or actually matching ─
    // Common legitimate case: one projector named generically
    // (e.g. "mmproj-F16.gguf") with no model-identity tokens after
    // noise stripping. Pair that. But if the sole projector has identity
    // tokens and zero overlap, it likely belongs to a different model;
    // skipping text-only is safer than force-pairing the wrong CLIP.
    if (candidates.size() == 1) {
        wxFileName candFn(candidates[0].fullPath);
        std::set<std::string> candTokens =
            TokeniseFilename(candFn.GetFullName());

        std::string result = candidates[0].fullPath.ToUTF8().data();
        if (bestScore > 0 || candTokens.empty()) {
            if (logger) {
                if (bestScore > 0)
                    logger->information(
                        "mmproj: matched \"" + result +
                        "\" (score " + std::to_string(bestScore) + "/" +
                        std::to_string(modelTokens.size()) + ")");
                else
                    logger->information(
                        "mmproj: using generic sole projector \"" + result +
                        "\" (no model-identity tokens after noise stripping)");
            }
            return result;
        }

        if (logger) {
            logger->information(
                "mmproj: sole projector \"" + result +
                "\" has model-identity tokens but zero overlap with "
                "the selected model — skipping projector pairing");
        }
        return "";
    }

    // ── Multiple candidates but no token overlap → ambiguous, skip ──
    if (bestScore == 0) {
        if (logger) {
            logger->information(
                "mmproj: " + std::to_string(candidates.size()) +
                " projector file(s) found but none matched model tokens — skipping");
        }
        return "";
    }

    std::string result = candidates[bestIdx].fullPath.ToUTF8().data();
    if (logger) {
        std::string tieNote;
        if (tiedWithBest > 0) {
            tieNote = " [AMBIGUOUS: " + std::to_string(tiedWithBest + 1) +
                      " files tied at score " + std::to_string(bestScore) +
                      ", picked alphabetically — consider renaming for clarity]";
        }
        logger->information(
            "mmproj: matched \"" + result +
            "\" (score " + std::to_string(bestScore) + "/" +
            std::to_string(modelTokens.size()) + ")" + tieNote);

        for (size_t i = 0; i < candidates.size(); ++i) {
            if (static_cast<int>(i) == bestIdx) continue;
            logger->information(
                "mmproj:   skipped \"" +
                std::string(candidates[i].fullPath.ToUTF8().data()) +
                "\" (score " + std::to_string(candidates[i].score) + ")");
        }
    }

    return result;
}

// ── Start server ─────────────────────────────────────────────────

// True if anything HTTP answers GET /health on our port right now.
// 250ms timeout: this runs on the UI thread just before a spawn, and
// on localhost a live server answers in microseconds — if nothing is
// listening, connect fails instantly (ECONNREFUSED), so the timeout
// is only ever felt in pathological half-open states.
bool ServerManager::IsPortAnswering(int port) const
{
    try {
        Poco::Net::HTTPClientSession sess("127.0.0.1", port);
        sess.setTimeout(Poco::Timespan(0, 250 * 1000));
        Poco::Net::HTTPRequest req(Poco::Net::HTTPRequest::HTTP_GET, "/health");
        sess.sendRequest(req);
        Poco::Net::HTTPResponse resp;
        sess.receiveResponse(resp);
        // Headers are enough: something owns the port.  Reading the
        // body would add a blocking read on the UI thread for
        // information we discard.  The session destructor closes the
        // socket without draining it.
        return true;   // any HTTP response at all means the port is taken
    }
    catch (...) {
        return false;  // connection refused / timeout — port is free
    }
}

bool ServerManager::StartServer(const std::string& ggufPath, const ServerConfig& config)
{
#ifdef __WXMSW__
    // Force-off flags may survive only while retrying the exact same
    // (model, config).  Any user-driven change is a fresh launch and
    // restores the normal "try MTP and --jinja first" behavior.
    const bool sameRetryArgs =
        ggufPath == m_lastGgufPath && config == m_lastConfig;
    const bool retryingWithoutMtp =
        sameRetryArgs && m_mtpForceOff && m_mtpRetryAttempted;
    const bool retryingWithoutJinja =
        sameRetryArgs && m_jinjaForceOff && m_jinjaRetryAttempted;

    if (!retryingWithoutMtp && !retryingWithoutJinja) {
        ResetStartupRetryState();
    }

    // Invalidate every older queued lifecycle event immediately, before any
    // preflight work for this attempt.  The old process is stopped below after
    // the foreign-port guard, preserving the original guard behavior.
    const ServerLaunchGeneration launchGeneration = AdvanceLaunchGeneration();
    m_launchStartedAt = std::chrono::steady_clock::now();

    // ── Foreign-server guard ─────────────────────────────────────
    // If something is already answering on our port and WE did not
    // spawn it, refuse to start.  Without this check the spawn below
    // fails on the bind conflict, but the health thread's first
    // /health probe gets a 200 from the *other* process's server
    // before our dead child is noticed — so this instance would
    // declare ready and silently chat against whatever model the
    // other instance has loaded.  One LlamaBoss window talking to
    // another LlamaBoss's server (with no ownership of its lifetime
    // or its KV slots) is never a state we want to be in silently.
    //
    // IsProcessRunning() distinguishes "our own server from a prior
    // load" (normal switch path — StopServer below handles it) from
    // "a server we never launched" (foreign).
    if (!IsProcessRunning() && IsPortAnswering(config.port)) {
        const std::string msg =
            "Another LlamaBoss instance is already running a local "
            "server on port " + std::to_string(config.port) + ".\n\n"
            "Close the other LlamaBoss window (or use remote "
            "endpoints in this one) and try again.";
        if (m_logger)
            m_logger->error("StartServer refused: foreign server "
                            "detected on port " + std::to_string(config.port));
        auto* ev = new wxCommandEvent(wxEVT_SERVER_ERROR);
        SetServerEventGeneration(*ev, launchGeneration);
        ev->SetString(wxString::FromUTF8(msg));
        LbQueueEventIfAlive(m_eventHandler, m_aliveToken, ev);
        return false;
    }

    // Stop any existing server first.  The new generation was already
    // allocated above, so cleanup must not advance it again.
    StopServerInternal(false);

    // Cache the launch args so startup fallback can re-invoke us
    // with the same (model, config) and one optional feature removed.
    m_lastGgufPath = ggufPath;
    m_lastConfig   = config;

    // Capture the launch mode before any later retry-state cleanup.
    // These become the truth for the server process we are about to start.
    const bool launchJinjaEnabled = !m_jinjaForceOff;
    bool       launchMtpEnabled   = false;

    // Detect backend and find binary
    Backend backend = DetectBackend();
    std::string serverBin = FindServerBinary(backend);

    if (serverBin.empty()) {
        // Try the other backend as fallback
        Backend fallback = (backend == Backend::CUDA12) ? Backend::CPU : Backend::CUDA12;
        serverBin = FindServerBinary(fallback);
        if (!serverBin.empty()) {
            if (m_logger)
                m_logger->warning("Preferred backend not found, falling back to " +
                    std::string(fallback == Backend::CUDA12 ? "CUDA12" : "CPU"));
            backend = fallback;
        }
    }

    if (serverBin.empty()) {
        if (m_logger)
            m_logger->error("llama-server binary not found in any search path");

        auto* ev = new wxCommandEvent(wxEVT_SERVER_ERROR);
        SetServerEventGeneration(*ev, launchGeneration);
        ev->SetString("llama-server.exe not found.\n\n"
                      "Download llama.cpp release binaries from:\n"
                      "https://github.com/ggml-org/llama.cpp/releases\n\n"
                      "Place them in a 'bin\\cpu\\' or 'bin\\cuda12\\' folder "
                      "next to LlamaBoss.exe.\n\n"
                      "Models go in:\n" + GetModelsDir());
        LbQueueEventIfAlive(m_eventHandler, m_aliveToken, ev);
        return false;
    }

    // Verify the GGUF file exists
    if (!wxFileExists(ggufPath)) {
        if (m_logger)
            m_logger->error("GGUF model not found: " + ggufPath);

        auto* ev = new wxCommandEvent(wxEVT_SERVER_ERROR);
        SetServerEventGeneration(*ev, launchGeneration);
        ev->SetString("Model file not found:\n" + ggufPath);
        LbQueueEventIfAlive(m_eventHandler, m_aliveToken, ev);
        return false;
    }

    if (m_logger) {
        m_logger->information("Starting llama-server: backend=" +
            std::string(backend == Backend::CUDA12 ? "CUDA12" : "CPU") +
            " model=" + ggufPath +
            " port=" + std::to_string(config.port) +
            " generation=" + std::to_string(launchGeneration) +
            " jinja=" + std::string(launchJinjaEnabled ? "on" : "off"));
    }

    // Build command line
    std::ostringstream cmd;
    cmd << "\"" << serverBin << "\""
        << " -m \"" << ggufPath << "\""
        << " --port " << config.port
        << " -c " << config.ctxSize;

    // ── --jinja (Phase 3a) ───────────────────────────────────────
    // Required by llama-server for native /v1/chat/completions tool
    // calling on tool-aware chat templates (Hermes 2 Pro, Qwen 2.5,
    // Llama 3.x, etc.).  Cost on non-tool models is one Jinja render
    // per chat completion — negligible on a modern GPU.  If the
    // server fails to start with this flag (rare; some custom chat
    // templates don't compile under Jinja), startup fallback
    // re-launches without it and we fall back to the existing XML
    // tool-call protocol for that model.
    if (launchJinjaEnabled) {
        cmd << " --jinja";
    }

    if (config.gpuLayers != 0)
        cmd << " -ngl " << config.gpuLayers;

    if (config.flashAttn)
        cmd << " --flash-attn on";

    // ── KV cache quantization ────────────────────────────────────
    // q8_0 K/V halves KV-cache memory — roughly double the usable
    // context per GiB of VRAM — with no quality loss measurable in
    // practice.  Quantized V requires flash attention, hence the
    // double gate: flashAttn is always true today, but the pair must
    // stay coupled if that ever changes.
    if (config.flashAttn && config.kvCacheQ8)
        cmd << " -ctk q8_0 -ctv q8_0";

    if (config.threads > 0)
        cmd << " -t " << config.threads;

    std::string launchMmproj;

    // ── Auto-detect multimodal projector (mmproj) ────────────────
    // Bundle mode (casual): the model's own folder either contains
    // an mmproj or it doesn't. Zero ambiguity — no scoring needed.
    // This is the casual user's guarantee: pairing is determined by
    // filesystem structure, not filename heuristics.
    //
    // Flat mode (power): fall back to filename-token scoring against
    // *mmproj*.gguf files in the models folder. User opted in to
    // approximate matching by choosing a custom folder.
    {
        std::string mmproj;

        wxString modelFolder = ModelFolderForGgufPath(ggufPath);

        if (IsBundledModelFolder(modelFolder)) {
            // Bundle layout is intentionally strict: exactly one projector
            // in the model folder pairs automatically; zero launches
            // text-only; more than one is ambiguous and skipped.
            mmproj = ResolveBundleMmproj(modelFolder, m_logger);
        }
        else {
            // Flat layout — use filename scoring (power-mode fallback).
            mmproj = FindMatchingMmproj(ggufPath, m_logger);
        }

        if (!mmproj.empty())
            cmd << " --mmproj \"" << mmproj << "\"";

        // Cache after successful process launch below.  Keeping this local
        // prevents a failed CreateProcess/ResumeThread attempt from leaving
        // GetLoadedMmproj() pointing at a projector for a server that is not
        // actually running.
        launchMmproj = mmproj;
    }

    // ── --cache-reuse (KV chunk reuse) ───────────────────────────
    // Reuses cached prompt/KV chunks via KV shifting after mid-prompt
    // edits — tool-result elision and agent-step trailer stripping in
    // BuildChatRequestJson both rewrite earlier messages, which would
    // otherwise invalidate the prompt cache from the edit point onward.
    //
    // Gated on text-only launches: llama-server disables context
    // shifting when a multimodal projector is loaded, so the flag is
    // inert there at best and a startup error on some builds at worst.
    // Placed after mmproj resolution because the pairing decision is
    // what determines eligibility.
    if (launchMmproj.empty())
        cmd << " --cache-reuse 256";

    // ── Speculative-decoding draft (bundle convention) ────────────
    // A single *draft*.gguf in the model's bundle attaches as the
    // speculative draft, fully offloaded.  llama-server's own draft
    // defaults govern acceptance windows.  Keep this separate-draft
    // path text-only until it has been validated with multimodal
    // launches.  A vocab-incompatible draft fails the launch with a
    // clear server error (the health thread surfaces the log tail);
    // removing or renaming the draft file is the fix.  Bundle layout
    // only — flat/power folders have no ownership structure to pair by.
    bool bundleDraftAttached = false;
    if (launchMmproj.empty()) {
        wxString bundleFolder = ModelFolderForGgufPath(ggufPath);
        if (IsBundledModelFolder(bundleFolder)) {
            std::string draft = ResolveBundleDraft(bundleFolder, m_logger);
            if (!draft.empty()) {
                cmd << " -md \"" << draft << "\" -ngld 99";
                bundleDraftAttached = true;
            }
        }
    }

    // ── Multi-token prediction (self-drafting) ───────────────────
    // Models with embedded MTP heads advertise
    // {arch}.nextn_predict_layers > 0 in GGUF metadata.  That is a
    // launch candidate signal, not a compatibility guarantee: a bad
    // conversion, backend limitation, or extra draft-context memory
    // requirement can still make llama-server reject the attempt.
    // MaybeRetryAfterStartupFailure() handles MTP-looking startup
    // failures by retrying once without MTP while keeping --jinja.
    //
    // Embedded MTP may be combined with an mmproj.  Treat that path as
    // experimental because the target model, MTP draft context, image
    // embeddings, projector, and KV cache all compete for memory.
    // Keep the projector on the CPU initially to preserve dedicated
    // GPU memory.  Text generation remains GPU accelerated, while
    // image encoding may be slower.  An explicit bundle *draft*.gguf
    // still wins over embedded MTP so speculative modes are not stacked.
    //
    // Use a conservative draft length of 2 for the initial LlamaBoss
    // implementation.  This limits additional memory and verification
    // work while still allowing multi-token acceptance.
    if (!bundleDraftAttached &&
        config.mtpEnabled &&
        !m_mtpForceOff) {
        // ggufPath arrives as a UTF-8 std::string (same bytes the -m
        // argument above is built from), which is exactly the encoding
        // GgufNextnPredictLayers expects — it widens internally on
        // Windows for the actual file open.
        const int nextn = GgufNextnPredictLayers(ggufPath);
        if (nextn > 0) {
            const bool mtpVisionEnabled = !launchMmproj.empty();

            if (mtpVisionEnabled) {
                // llama.cpp offloads the multimodal projector to the GPU
                // by default.  CPU offload is the safer first setting for
                // MTP + vision on a 32 GiB card.
                cmd << " --no-mmproj-offload";

                if (m_logger) {
                    m_logger->warning(
                        "mtp: enabling experimental MTP + vision; "
                        "mmproj will remain on CPU");
                }
            }

            cmd << " --spec-type draft-mtp --spec-draft-n-max 2";
            launchMtpEnabled = true;

            if (m_logger) {
                m_logger->information("mtp: " + std::to_string(nextn) +
                    " nextn layer(s) detected, enabling draft-mtp");
            }
        }
    }
    else if (m_mtpForceOff && config.mtpEnabled && m_logger) {
        m_logger->information(
            "mtp: disabled for this retry attempt after startup failure");
    }

    // ── Single-slot pinning ──────────────────────────────────────
    // llama-server's --parallel default changed in late 2025 from 1
    // to auto, which resolves to 4 slots + unified KV + automatic
    // slot assignment (continuous batching is also on by default).
    // Everything downstream is written against single-slot
    // semantics: ExecuteSlotAction hard-codes /slots/0, the
    // m_slotOwner/m_slotDirty ownership contract assumes requests
    // serialize on one slot, and the multi-window queue notice in
    // LlamaBoss.cpp promises FIFO waiting.  Under auto, a request
    // can be scheduled onto slot 1-3 while save/restore silently
    // targets a stale slot 0 — and slot-action failures are
    // swallowed by design, so nothing would surface.  Pin it.
    // An explicit --parallel 1 also keeps unified KV off (its
    // default is "on when slot count is auto"), restoring the
    // classic per-slot KV layout.  VerifySingleSlot() backstops
    // this at every launch in case a future bundled binary changes
    // flag semantics again.
    cmd << " --parallel 1";

    // ── KV slot state persistence ────────────────────────────────
    // Always armed: --slot-save-path enables POST /slots/0?action=
    // save|restore (used by the conversation-switch fast path) and
    // --slots enables the read-only slot listing for diagnostics —
    // and, since the 2025 default change, the VerifySingleSlot probe.
    // Costs nothing when unused.
    cmd << " --slots --slot-save-path \"" << GetKvSlotCacheDir() << "\"";

    std::string cmdLine = cmd.str();

    if (m_logger)
        m_logger->information("Command: " + cmdLine);

    // Set up log file for server stdout/stderr
    EnsureDataDirs();
    std::string logPath = GetLogsDir() + std::string(1, wxFILE_SEP_PATH) + "server.log";

    // ── UTF-8 → UTF-16 conversion ────────────────────────────────
    // Every path and command-line string we built above came out of
    // wxString::ToUTF8(), so the bytes are UTF-8.  The ANSI Win32
    // APIs (CreateFileA / CreateProcessA) interpret their char* args
    // using the system code page — NOT UTF-8 — so a UTF-8 byte
    // sequence like 0xC3 0xBC for "ü" gets read as "Ã¼" under
    // CP1252 and the filesystem call silently fails to resolve.
    // Using the W APIs with explicit UTF-8 → UTF-16 conversion
    // avoids the whole class of "works on my ASCII machine, breaks
    // on a non-ASCII username / model filename / install path" bug.
    // Mirrors the pattern already used in cmd_executor.cpp.
    auto Utf8ToWide = [](const std::string& in) -> std::wstring {
        if (in.empty()) return std::wstring();
        int n = MultiByteToWideChar(CP_UTF8, 0, in.data(), (int)in.size(),
                                    nullptr, 0);
        if (n <= 0) return std::wstring();
        std::wstring out(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, in.data(), (int)in.size(),
                            out.data(), n);
        return out;
    };

    std::wstring wLogPath = Utf8ToWide(logPath);
    std::wstring wCmdLine = Utf8ToWide(cmdLine);

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hLogFile = CreateFileW(
        wLogPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,    // Allow reading while server runs
        &sa,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    // If log redirection cannot be opened, do not let the health thread
    // attach a stale server.log from a previous run to this run's failure.
    std::string healthLogPath = logPath;
    if (hLogFile == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        healthLogPath.clear();
        if (m_logger) {
            m_logger->warning(
                "Could not open server.log for redirection (Windows error " +
                std::to_string(err) + "); this launch will not include "
                "llama-server stdout/stderr in readiness errors");
        }
    }

    // Set up process creation
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    if (hLogFile != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = hLogFile;
        si.hStdError  = hLogFile;
        si.hStdInput  = NULL;
    }

    PROCESS_INFORMATION pi = {};

    // CreateProcessW needs a MUTABLE wide command line buffer.
    // Windows filenames cannot contain '"', so the simple double-
    // quote wrapping around serverBin / ggufPath / mmproj in the
    // command-line build above is safe — no embedded-quote escape
    // pass needed.
    std::vector<wchar_t> cmdBuf(wCmdLine.begin(), wCmdLine.end());
    cmdBuf.push_back(L'\0');

    // Working directory = folder containing the server binary
    // (so DLLs like ggml-cuda.dll are found)
    wxFileName binFn(serverBin);
    std::string workDir = binFn.GetPath().ToUTF8().data();
    std::wstring wWorkDir = Utf8ToWide(workDir);

    BOOL ok = CreateProcessW(
        NULL,               // lpApplicationName (embedded in cmdLine)
        cmdBuf.data(),      // lpCommandLine
        NULL, NULL,         // process/thread security
        TRUE,               // bInheritHandles (for log file)
        CREATE_NO_WINDOW | CREATE_SUSPENDED, // assign to job before CUDA/model load
        NULL,               // lpEnvironment
        wWorkDir.c_str(),   // lpCurrentDirectory
        &si,
        &pi
    );

    // Close the log file handle in this process — the child has its own copy
    if (hLogFile != INVALID_HANDLE_VALUE)
        CloseHandle(hLogFile);

    if (!ok) {
        DWORD err = GetLastError();
        if (m_logger)
            m_logger->error("CreateProcess failed, error=" + std::to_string(err));

        auto* ev = new wxCommandEvent(wxEVT_SERVER_ERROR);
        SetServerEventGeneration(*ev, launchGeneration);
        ev->SetString("Failed to start llama-server (Windows error " +
                      std::to_string(err) + ")");
        LbQueueEventIfAlive(m_eventHandler, m_aliveToken, ev);
        return false;
    }

    // Put llama-server in a Windows Job Object with KILL_ON_JOB_CLOSE.
    // This gives us a hard safety net: if LlamaBoss closes unexpectedly,
    // the job handle closes and Windows tears down the server process tree,
    // releasing the loaded model and CUDA VRAM instead of leaving an orphan.
    HANDLE jobHandle = CreateJobObjectW(NULL, NULL);
    if (jobHandle) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobInfo = {};
        jobInfo.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

        if (!SetInformationJobObject(jobHandle,
                                     JobObjectExtendedLimitInformation,
                                     &jobInfo,
                                     sizeof(jobInfo))) {
            DWORD err = GetLastError();
            if (m_logger)
                m_logger->warning("SetInformationJobObject failed, error=" +
                                  std::to_string(err));
            CloseHandle(jobHandle);
            jobHandle = NULL;
        }
        else if (!AssignProcessToJobObject(jobHandle, pi.hProcess)) {
            DWORD err = GetLastError();
            if (m_logger)
                m_logger->warning("AssignProcessToJobObject failed, error=" +
                                  std::to_string(err) +
                                  "; normal StopServer fallback will be used");
            CloseHandle(jobHandle);
            jobHandle = NULL;
        }
    }
    else if (m_logger) {
        DWORD err = GetLastError();
        m_logger->warning("CreateJobObject failed, error=" + std::to_string(err));
    }

    if (ResumeThread(pi.hThread) == (DWORD)-1) {
        DWORD err = GetLastError();
        if (m_logger)
            m_logger->error("ResumeThread failed after starting llama-server, error=" +
                            std::to_string(err));

        TerminateProcess(pi.hProcess, 1);
        if (jobHandle)
            CloseHandle(jobHandle);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        auto* ev = new wxCommandEvent(wxEVT_SERVER_ERROR);
        SetServerEventGeneration(*ev, launchGeneration);
        ev->SetString("Failed to resume llama-server (Windows error " +
                      std::to_string(err) + ")");
        LbQueueEventIfAlive(m_eventHandler, m_aliveToken, ev);
        return false;
    }

    m_processHandle       = pi.hProcess;
    m_threadHandle        = pi.hThread;
    m_jobHandle           = jobHandle;
    m_processId           = pi.dwProcessId;
    m_port                = config.port;

    // This launch now owns the port; slot actions enqueued from here
    // carry this generation and anything older is dropped by the
    // worker.
    if (m_slotQueue) {
        std::lock_guard<std::mutex> lock(m_slotQueue->mutex);
        m_slotQueue->liveGeneration = launchGeneration;
    }

    m_loadedModel         = ggufPath;
    m_slotOwner.clear();       // fresh process, fresh (empty) slot
    m_slotDirty = false;
    m_loadedMmproj        = launchMmproj;
    m_currentMtpEnabled   = launchMtpEnabled;
    m_currentJinjaEnabled = launchJinjaEnabled;

    if (m_logger)
        m_logger->information("llama-server started, PID=" +
                              std::to_string(m_processId) +
                              " generation=" + std::to_string(launchGeneration));

    // Start health-check thread (polls /health until 200 or timeout).
    // wxThread requires Create() before Run(); if either step fails, do not
    // leave llama-server running with the UI stuck at "Loading..." and no
    // wxEVT_SERVER_READY / wxEVT_SERVER_ERROR path back to the frame.
    m_healthCancelFlag = std::make_shared<std::atomic<bool>>(false);
    auto* healthThread = new ServerHealthThread(
        m_eventHandler, GetBaseUrl(), m_healthCancelFlag, m_aliveToken,
        m_processHandle, healthLogPath, launchGeneration, 120000, // 2min timeout
        m_logger);

    auto failHealthMonitorStart = [&](const std::string& detail) -> bool {
        if (m_logger)
            m_logger->error("Failed to start health-check thread: " + detail);

        // The server process is alive, but without the readiness monitor the
        // UI cannot reliably leave the loading state. Tear it down and surface
        // the real failure instead of silently returning success.
        StopServerInternal(false);

        auto* ev = new wxCommandEvent(wxEVT_SERVER_ERROR);
        SetServerEventGeneration(*ev, launchGeneration);
        ev->SetString("Failed to start llama-server readiness monitor.\n\n"
                      "The server was stopped so LlamaBoss does not stay "
                      "stuck loading.\n\n"
                      "Details: " + detail);
        LbQueueEventIfAlive(m_eventHandler, m_aliveToken, ev);
        return false;
    };

    wxThreadError createErr = healthThread->Create();
    if (createErr != wxTHREAD_NO_ERROR) {
        delete healthThread;
        return failHealthMonitorStart("wxThread::Create failed with code " +
                                      std::to_string(static_cast<int>(createErr)));
    }

    wxThreadError runErr = healthThread->Run();
    if (runErr != wxTHREAD_NO_ERROR) {
        delete healthThread;
        return failHealthMonitorStart("wxThread::Run failed with code " +
                                      std::to_string(static_cast<int>(runErr)));
    }

    return true;
#else
    // Non-Windows: not implemented
    (void)ggufPath; (void)config;
    return false;
#endif
}

// ── Stop server ──────────────────────────────────────────────────

ServerLaunchGeneration ServerManager::AdvanceLaunchGeneration()
{
    // wxCommandEvent::ExtraLong is a signed long (32-bit on Windows), so keep
    // generations in its positive range and reserve 0 for unstamped events.
    const auto maxEventGeneration = static_cast<ServerLaunchGeneration>(
        std::numeric_limits<long>::max());
    if (m_launchGeneration >= maxEventGeneration)
        m_launchGeneration = 1;
    else
        ++m_launchGeneration;
    return m_launchGeneration;
}

long long ServerManager::GetCurrentLaunchElapsedMs() const
{
    if (m_launchStartedAt == std::chrono::steady_clock::time_point{})
        return -1;

    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - m_launchStartedAt).count();
}

void ServerManager::StopServerInternal(bool invalidateGeneration)
{
    if (invalidateGeneration) {
        AdvanceLaunchGeneration();
        m_launchStartedAt = std::chrono::steady_clock::time_point{};
    }

    // Cancel health check thread first.  Any event that already crossed its
    // cancellation check is still harmless: the generation no longer matches.
    if (m_healthCancelFlag) {
        m_healthCancelFlag->store(true);
        m_healthCancelFlag.reset();
    }

    // Drop queued slot actions and cut short anything in flight
    // BEFORE the process dies.  The old code only set the stop flag
    // in ~ServerManager, so a normal model switch left queued
    // actions alive to be delivered to the next server.
    AbandonSlotQueue(m_slotQueue, m_logger);

    KillProcess();
    m_loadedModel.clear();
    m_loadedMmproj.clear();
    m_currentMtpEnabled = false;
    m_currentJinjaEnabled = false;
    m_slotOwner.clear();
    m_slotDirty = false;
}

void ServerManager::StopServer()
{
    StopServerInternal(true);

    // A user cancellation, model detach, or shutdown ends the current
    // load attempt.  Do not let a force-off retry flag leak into a later
    // user-driven reload of the same model and configuration.
    ResetStartupRetryState();
}

// ── Startup feature fallback retries ────────────────────────────
//
// ModelService calls this before publishing a permanent startup error.
// Retries are deliberately bounded and ordered:
//
//   1. An actual draft-mtp attempt with an MTP-looking failure is
//      relaunched once with MTP force-disabled and --jinja unchanged.
//   2. A Jinja/template-looking failure is relaunched once without
//      --jinja.  If step 1 already ran, MTP stays force-disabled.
//
// The persisted MTP preference is never changed.  Force-off state is
// scoped to this one (model, config) load attempt and is cleared after
// success or when the fallback chain is exhausted.
bool ServerManager::MaybeRetryAfterStartupFailure(const std::string& error)
{
    if (m_lastGgufPath.empty()) {
        ResetStartupRetryState();
        return false;
    }

    // First preference: preserve native Jinja/tool support and remove
    // only the feature implicated by the failure.
    if (m_currentMtpEnabled &&
        !m_mtpForceOff &&
        !m_mtpRetryAttempted &&
        LooksLikeMtpFailure(error)) {
        if (m_logger) {
            m_logger->warning(
                "mtp: startup failed with an MTP-related error; retrying "
                "normally without MTP. Original error: " + error);
        }

        m_mtpForceOff       = true;
        m_mtpRetryAttempted = true;

        const std::string retryPath = m_lastGgufPath;
        const ServerConfig retryConfig = m_lastConfig;
        const bool ok = StartServer(retryPath, retryConfig);
        if (!ok && m_logger) {
            m_logger->error(
                "mtp: synchronous retry without MTP also failed; awaiting "
                "the retry error event.");
        }

        return true;   // consume the original error
    }

    // A no-jinja attempt has already failed, so the bounded fallback
    // chain is exhausted.  Reset for the next user-driven load and
    // surface this final error.
    if (m_jinjaRetryAttempted || m_jinjaForceOff) {
        ResetStartupRetryState();
        return false;
    }

    // Only remove --jinja for errors that actually implicate Jinja or
    // the chat template.  Unrelated startup failures should not be
    // hidden behind a second, potentially more confusing attempt.
    if (!m_currentJinjaEnabled ||
        !LooksLikeJinjaOrTemplateFailure(error)) {
        if (m_logger && m_currentJinjaEnabled) {
            m_logger->warning(
                "Server startup failed while --jinja was enabled, but the "
                "error does not look Jinja/template-related; not retrying "
                "without --jinja. Error: " + error);
        }
        ResetStartupRetryState();
        return false;
    }

    if (m_logger) {
        m_logger->warning(
            "Server failed to start with a Jinja/template error; retrying "
            "without --jinja. Original error: " + error);
    }

    m_jinjaForceOff       = true;
    m_jinjaRetryAttempted = true;

    const std::string retryPath = m_lastGgufPath;
    const ServerConfig retryConfig = m_lastConfig;
    const bool ok = StartServer(retryPath, retryConfig);
    if (!ok && m_logger) {
        m_logger->error(
            "Synchronous retry without --jinja also failed; awaiting the "
            "retry error event.");
    }

    return true;   // consume the original error
}

void ServerManager::ResetStartupRetryState()
{
    m_mtpForceOff          = false;
    m_mtpRetryAttempted    = false;
    m_jinjaForceOff        = false;
    m_jinjaRetryAttempted  = false;
}

void ServerManager::NotifyServerReady()
{
    // Successful start clears only retry intent.  The current feature
    // flags describe the actual running process and remain intact.
    ResetStartupRetryState();
}

bool ServerManager::IsProcessRunning() const
{
#ifdef __WXMSW__
    if (m_processHandle == INVALID_HANDLE_VALUE)
        return false;

    DWORD exitCode = 0;
    if (GetExitCodeProcess(m_processHandle, &exitCode))
        return exitCode == STILL_ACTIVE;
#endif
    return false;
}

void ServerManager::KillProcess()
{
#ifdef __WXMSW__
    const DWORD pid = m_processId;

    if (m_processHandle != INVALID_HANDLE_VALUE) {
        if (m_logger)
            m_logger->information("Stopping llama-server PID=" +
                                  std::to_string(pid));

        // If the process is in our job, closing the job handle kills the
        // whole process tree. This is stronger than TerminateProcess alone
        // and protects against orphaned helper children.
        const bool hadJob = (m_jobHandle != NULL);
        if (hadJob) {
            CloseHandle(m_jobHandle);
            m_jobHandle = NULL;
        }
        else {
            if (!TerminateProcess(m_processHandle, 0)) {
                DWORD err = GetLastError();
                if (m_logger)
                    m_logger->warning("TerminateProcess failed for llama-server PID=" +
                                      std::to_string(pid) + ", error=" +
                                      std::to_string(err));
            }
        }

        DWORD waitResult = WaitForSingleObject(m_processHandle, 10000);
        if (waitResult == WAIT_TIMEOUT && pid != 0) {
            if (m_logger)
                m_logger->warning("llama-server PID=" + std::to_string(pid) +
                                  " did not exit after 10s; trying taskkill /T /F");

            // PID is numeric and comes from CreateProcess, so this command line
            // is not user-controlled. It is only a last-resort cleanup fallback.
            std::string cmd = "taskkill /PID " + std::to_string(pid) +
                              " /T /F >NUL 2>NUL";
            std::system(cmd.c_str());
            waitResult = WaitForSingleObject(m_processHandle, 5000);

            if (waitResult == WAIT_TIMEOUT && m_logger) {
                m_logger->error("llama-server PID=" + std::to_string(pid) +
                                " still appears to be running after taskkill");
            }
        }

        CloseHandle(m_processHandle);
        m_processHandle = INVALID_HANDLE_VALUE;
    }

    if (m_threadHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(m_threadHandle);
        m_threadHandle = INVALID_HANDLE_VALUE;
    }

    if (m_jobHandle != NULL) {
        CloseHandle(m_jobHandle);
        m_jobHandle = NULL;
    }

    m_processId = 0;
#endif
}
