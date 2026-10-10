// server_manager.h
#pragma once

#include <wx/wx.h>
#include <wx/thread.h>
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>

#include "lb_windows.h"
#ifndef __WXMSW__
#include "lb_process.h"
#endif

#include <Poco/Logger.h>

// ── Server launch configuration ─────────────────────────────────
struct ServerConfig
{
    int  port       = 8384;
    int  gpuLayers  = -1;      // -1 = offload all layers to GPU
    int  ctxSize    = 8192;
    int  threads    = 0;       // 0 = auto-detect
    bool flashAttn  = true;
    bool kvCacheQ8  = true;    // -ctk/-ctv q8_0; requires flashAttn
    bool mtpEnabled = true;    // auto-detect GGUF MTP metadata and try
                               // --spec-type draft-mtp with safe fallback

    bool operator==(const ServerConfig& other) const
    {
        return port == other.port &&
               gpuLayers == other.gpuLayers &&
               ctxSize == other.ctxSize &&
               threads == other.threads &&
               flashAttn == other.flashAttn &&
               kvCacheQ8 == other.kvCacheQ8 &&
               mtpEnabled == other.mtpEnabled;
    }
};

// ── Backend type ────────────────────────────────────────────────
enum class Backend { CPU, CUDA12 };

// ── Custom events ───────────────────────────────────────────────
wxDECLARE_EVENT(wxEVT_SERVER_READY, wxCommandEvent);
wxDECLARE_EVENT(wxEVT_SERVER_ERROR, wxCommandEvent);

// Every local-server launch attempt receives a monotonically increasing
// generation.  Ready/error events carry the generation in wxCommandEvent's
// ExtraLong payload so queued events from an older, already-stopped process
// can be rejected after a model switch, retry, remote activation, or shutdown.
// Generation 0 is reserved for unstamped/invalid events.
using ServerLaunchGeneration = std::uint32_t;
constexpr ServerLaunchGeneration kInvalidServerLaunchGeneration = 0;

inline void SetServerEventGeneration(wxCommandEvent& event,
                                     ServerLaunchGeneration generation)
{
    event.SetExtraLong(static_cast<long>(generation));
}

inline ServerLaunchGeneration GetServerEventGeneration(
    const wxCommandEvent& event)
{
    return static_cast<ServerLaunchGeneration>(event.GetExtraLong());
}

// ── Health-check thread ─────────────────────────────────────────
// Polls GET /health until 200 or timeout, then posts an event.
// Also watches the llama-server process handle — if the child dies
// before becoming ready (bad args, bad mmproj, missing DLL, port
// conflict, etc.) we surface the failure within ~500 ms with the
// tail of server.log attached, instead of sitting at "loading…"
// for the full timeout with no explanation.
class ServerHealthThread : public wxThread
{
public:
    ServerHealthThread(wxEvtHandler* handler,
                       const std::string& baseUrl,
                       std::shared_ptr<std::atomic<bool>> cancelFlag,
                       std::weak_ptr<std::atomic<bool>> aliveToken,
#ifdef __WXMSW__
                       HANDLE processHandle,
#else
                       std::shared_ptr<lb_process::Child> child,
#endif
                       const std::string& logPath,
                       ServerLaunchGeneration generation,
                       int timeoutMs = 60000,
                       Poco::Logger* logger = nullptr);

    // Entry() closes the duplicated process handle on every one of its
    // exit paths and nulls it out.  This destructor is the backstop for
    // the paths where Entry() NEVER RUNS -- specifically StartServer's
    // `delete healthThread;` after a failed Create() or Run().  Without
    // it the duplicate leaked, and because a live duplicate keeps the
    // process object alive it also weakened job-object teardown.
    ~ServerHealthThread() override;

protected:
    ExitCode Entry() override;
private:
    wxEvtHandler* m_handler;
    std::string   m_baseUrl;
    std::shared_ptr<std::atomic<bool>> m_cancelFlag;
    std::weak_ptr<std::atomic<bool>> m_aliveToken;
#ifdef __WXMSW__
    // Duplicated -- owned by this thread.  Closed by CloseProcessHandle(),
    // which nulls the member, so Entry() and ~ServerHealthThread() can both
    // call it without any risk of a double close.
    HANDLE        m_processHandle;
    void CloseProcessHandle();
#else
    std::shared_ptr<lb_process::Child> m_child;   // shared with ServerManager
#endif
    std::string   m_logPath;
    ServerLaunchGeneration m_generation;
    int m_timeoutMs;
    Poco::Logger* m_logger;   // app logger; nullable — verification is best-effort
    bool SafePost(wxCommandEvent* ev);
};

// ── Server manager ──────────────────────────────────────────────
class ServerManager
{
public:
    ServerManager(wxEvtHandler* eventHandler,
                  std::weak_ptr<std::atomic<bool>> aliveToken,
                  Poco::Logger* logger = nullptr);
    ~ServerManager();

    // Lifecycle
    bool StartServer(const std::string& ggufPath,
                     const ServerConfig& config = ServerConfig());
    void StopServer();
    bool IsProcessRunning() const;

    // Foreign-server guard: true if anything answers HTTP on the
    // given port right now.  StartServer refuses to spawn when this
    // is true and the answering process isn't ours — see the guard
    // block there for why silent attach is the failure mode.
    //
    // Takes the port explicitly: m_port is not assigned from
    // config.port until AFTER process creation, so reading it here
    // would probe the PREVIOUS port on any launch with a non-default
    // port and miss exactly the collision this guard exists to catch.
    bool IsPortAnswering(int port) const;

    // ── Startup fallback retries ────────────────────────────────
    // LlamaBoss normally tries the best available launch features:
    // embedded MTP self-drafting when advertised by GGUF metadata,
    // plus --jinja for native tool-aware templates.  Metadata and
    // templates are not guarantees, so startup failures are handled
    // by a bounded feature fallback chain:
    //
    //   1. If an MTP-looking failure occurs on an actual draft-mtp
    //      attempt, retry once with MTP disabled but keep --jinja.
    //   2. If a Jinja/template-looking failure remains, retry once
    //      more without --jinja (and keep MTP disabled if step 1 ran).
    //
    // ModelService calls this before publishing a permanent error.
    // Returns true iff a retry was started and the current error must
    // be consumed; false means the error should reach the UI.
    bool MaybeRetryAfterStartupFailure(const std::string& error);

    // Call this after wxEVT_SERVER_READY so per-load retry state is
    // cleared.  The actual running process feature flags remain
    // available for runtime protocol decisions.
    void NotifyServerReady();

    // True only for the currently running llama-server process when
    // that process was launched with --jinja.  Tool-call protocol
    // detection must check this because native function calling is a
    // server-runtime capability, not only a model/template capability.
    bool IsCurrentServerJinjaEnabled() const { return m_currentJinjaEnabled; }

    // Accessors
    std::string GetBaseUrl() const;
    ServerLaunchGeneration GetLaunchGeneration() const
    {
        return m_launchGeneration;
    }
    long long GetCurrentLaunchElapsedMs() const;
    std::string GetLoadedModel() const { return m_loadedModel; }
    std::string GetLoadedMmproj() const { return m_loadedMmproj; }

    // ── KV slot state (instant conversation switching) ───────────
    // llama-server can serialize a slot's KV cache to disk and load
    // it back (--slot-save-path + POST /slots/0?action=save|restore).
    // These wrap that API so switching back to a conversation skips
    // reprocessing its whole history.  All three are cheap no-ops
    // when no local server is running (remote lane) — StopServer()
    // clears m_loadedModel, which every path checks first.
    //
    // Ownership model: m_slotOwner tracks which conversation's KV
    // the single slot currently holds (as a cache filename), and
    // m_slotDirty tracks whether a generation ran since the last
    // save/restore.  Save-away only writes when the slot verifiably
    // holds the conversation being left AND something changed —
    // never a stale slot under a wrong name, never a redundant
    // multi-GB rewrite of state that a restore just loaded.
    //
    // Ordering: slot actions execute on ONE serialized worker (FIFO
    // queue drained by a single thread at a time), so a save-away
    // dispatched before a restore-in is guaranteed to reach
    // llama-server first.  The previous one-detached-thread-per-
    // action design had no arrival-order guarantee: on a fast
    // conversation switch the restore could win the race, overwrite
    // the slot, and the save would then serialize the WRONG
    // conversation's KV under the outgoing name.  Callers keep
    // fire-and-forget semantics; only execution is serialized.
    void NoteSlotOwner(const std::string& conversationPath);
    void SaveSlotStateForConversation(const std::string& conversationPath);
    void RestoreSlotStateForConversation(const std::string& conversationPath);

    // Out-of-conversation generations (e.g. the Skill draft builder) run against the same single
    // slot with throwaway histories.  They replace the slot's KV
    // with content that belongs to NO conversation — call this at
    // their dispatch so a later switch-away doesn't serialize that
    // state under the active conversation's filename.  The stale
    // save was benign (a mismatched restore just falls back to full
    // reprocess) but wasted a multi-GB write and a pointless
    // restore.  Cheap no-op when nothing is tracked.
    void InvalidateSlotOwner();

    // ── Prompt-cache pre-warm (prompt_prewarm.h) ─────────────────
    // Queue a "prewarm" slot action: render |chatRequestBody| with
    // /apply-template, cut it before "\n\n" + |marker|, and have
    // llama-server process that prefix so the next new chat's first
    // request only extends it.  Runs on the serialized slot-action
    // worker (after any queued save-away).  Clears slot ownership: the
    // slot will hold no conversation's KV.  |key| identifies the prefix;
    // re-priming is skipped while the same key is queued/running, or
    // after it SUCCEEDED until something else uses the slot (an exact
    // re-send is not free on recurrent models).  A failed or superseded
    // prime does not block a retry.  Returns false with |why| when
    // skipped.
    bool PrewarmPromptPrefix(const std::string& chatRequestBody,
                             const std::string& marker,
                             const std::string& key,
                             std::string& why);

    // Display name: "/path/to/model.gguf" -> "model"
    static std::string ModelDisplayName(const std::string& ggufPath);

    // ── Static utilities ────────────────────────────────────────
    static Backend     DetectBackend();
    static std::string FindServerBinary(Backend backend);

    // Token-match an mmproj .gguf to the loaded model; returns empty if none found.
    static std::string FindMatchingMmproj(const std::string& modelGgufPath,
                                          Poco::Logger* logger = nullptr);

    // ── Data directory helpers ───────────────────────────────────
    static std::string GetDataDir();                // %LOCALAPPDATA%\LlamaBoss

    // The hardcoded, never-changes models root. Used as the "home" folder
    // for casual users who haven't opted into a custom path. Always
    // retrievable so the UI can show it / reset to it.
    static std::string GetDefaultModelsDir();       // %LOCALAPPDATA%\LlamaBoss\models

    // The currently-active models root. Equals the override (if set) or
    // GetDefaultModelsDir() otherwise. This is what the scanner reads.
    static std::string GetModelsDir();

    // Set / clear the user's override. Empty string = use default.
    // Persisted to wxFileConfig so it survives restarts.
    static std::string GetModelsDirOverride();
    static void        SetModelsDirOverride(const std::string& path);

    // True when the active folder is the hardcoded default (casual mode).
    // False when the user has set a custom folder (power mode).
    // Casual mode uses bundle layout + deterministic mmproj pairing.
    // Power mode uses flat layout + filename-heuristic mmproj matching.
    static bool        IsCasualMode();

    static std::string GetLogsDir();
    static std::string GetConfigDir();
    static std::string GetConversationsDir();
    static std::string GetCacheDir();
    static void        EnsureDataDirs();

    // ── Workspace ────────────────────────────────────────────────
    // The user-visible directory where the agent creates files by
    // default. Lives under %USERPROFILE%\LlamaBoss\Shared\Workspace so it
    // stays out of OneDrive-redirected Documents by default while still
    // being easy to find in File Explorer.
    //
    // The override pattern mirrors the models folder above: empty
    // override = default path; non-empty = user-chosen path. Persisted
    // to wxFileConfig under key "WorkspaceFolderOverride".
    //
    // GetWorkspaceDir() is the single source of truth consumed by
    // MyFrame::ResolveCurrentCwd() and BuildAgentSystemPrompt() as
    // the fallback when no per-conversation /cd override is set.
    static std::string GetDefaultWorkspaceDir();    // %USERPROFILE%\LlamaBoss\Shared\Workspace
    static std::string GetWorkspaceDir();
    static std::string GetWorkspaceDirOverride();
    static void        EnsureWorkspaceDir();
    // Startup tidy-up: LlamaBoss\<lane> -> LlamaBoss\Shared\<lane> for
    // installs that predate the Shared folder.  Called by EnsureWorkspaceDir.
    static void        MigrateRootLanesToShared();

    // ── Conversation lane layout ─────────────────────────────────
    // Recognizers for the per-conversation folder layout created by
    // ChatHistory::EnsureChatFolder():
    //   %USERPROFILE%\LlamaBoss\Chats\<date>_<slug>_<id>\Workspace
    //
    // The shape check itself is chat_folders::ChatFolderFromWorkspaceCwd
    // (chat_folders.h).  Every recognizer in the codebase — here,
    // python_runner, agent_controller, cmd_executor, tool_router,
    // tool_web_fetch, tool_path_safety — goes through that one function.
    // agent_controller's use guards the one-shot python_run_script
    // approval bypass against cross-lane shadowing, so a private copy that
    // drifted would weaken exactly the safety property it exists to
    // protect.  If the layout ever changes, chat_folders.h is the one
    // place to update.

    // %USERPROFILE%\LlamaBoss — holds Chats, Projects, Skills, System.
    static std::string GetLlamaBossRootDir();

    // %USERPROFILE%\LlamaBoss\Shared — the lanes (Workspace, Scripts,
    // Documents, ToolOutputs, ...) used when the cwd is not a chat
    // workspace.  Parent of GetDefaultWorkspaceDir().
    static std::string GetSharedLanesRootDir();

    // Returns the chat folder (...\Chats\<date>_<slug>_<id>) when `cwd`
    // matches the conversation-workspace shape above; empty otherwise.
    static std::string ChatFolderFromCwd(const std::string& cwd);

    // Lane folder ("Scripts", "Documents", "Spreadsheets", ...) for
    // the conversation owning `cwd`, falling back to the matching
    // LlamaBoss\Shared lane when `cwd` is not a chat workspace.
    static std::string ConversationLaneDirForCwd(const std::string& cwd,
                                                 const std::string& lane);

    // Convenience: ConversationLaneDirForCwd(cwd, "Scripts").
    static std::string ConversationScriptsDirForCwd(const std::string& cwd);

    // ── Model scanning ───────────────────────────────────────────
    // A scanned model — either a bundle (subfolder containing one .gguf
    // and optionally one mmproj .gguf) or a loose .gguf file at the
    // scan root. Callers use ggufPath to pass to llama-server and
    // displayName for UI; mmprojPath is empty when the model doesn't
    // have / need a projector.
    struct ModelEntry {
        std::string ggufPath;      // Full path to the .gguf file to load
        std::string mmprojPath;    // Full path to the paired mmproj, or "" if none
        std::string displayName;   // Human-readable name for UI
        std::string bundleDir;     // Subfolder path if bundled, "" for loose files
        bool        isBundle = false;
    };

    // Scan the active models directory for models. Returns bundles as
    // single entries (one per subfolder) in casual mode, loose .gguf
    // files in power mode. Sorted alphabetically by displayName.
    static std::vector<ModelEntry> ScanModels();

    // Path-only scan for callers that only want a flat list of .gguf
    // paths to load.  New code should use ScanModels() and consume
    // ModelEntry directly.
    static std::vector<std::string> ScanModelPaths();

private:
#ifdef __WXMSW__
    HANDLE m_processHandle = INVALID_HANDLE_VALUE;
    HANDLE m_threadHandle  = INVALID_HANDLE_VALUE;
    HANDLE m_jobHandle     = NULL;   // Kills llama-server if LlamaBoss closes/crashes.
    DWORD  m_processId     = 0;
#else
    std::shared_ptr<lb_process::Child> m_child;   // own process group; see KillProcess
#endif

    wxEvtHandler* m_eventHandler;
    std::weak_ptr<std::atomic<bool>> m_aliveToken;
    Poco::Logger* m_logger;
    std::string   m_loadedModel;
    std::string   m_loadedMmproj;   // paired mmproj (or empty)
    std::string   m_slotOwner;      // cache filename the slot's KV belongs to
    bool          m_slotDirty = false; // generation ran since last save/restore

    // Serialized slot-action worker state (queue + flags), shared
    // with the worker thread so ServerManager can be destroyed while
    // an action is in flight without blocking the UI thread on a
    // join.  Defined in server_manager.cpp and created eagerly in the
    // constructor so StartServer() can stamp the launch generation
    // before the first KV action is enqueued.  The destructor sets its
    // stop flag so a pending worker exits instead of POSTing to a
    // server that StopServer just killed.
    std::shared_ptr<struct SlotActionQueue> m_slotQueue;

    // Append a slot action to the serialized queue and make sure a
    // worker is draining it.  Logs the dispatch on the caller thread
    // (worker does not touch m_logger — it can outlive this object).
    void EnqueueSlotAction(const std::string& action,
                           const std::string& filename,
                           const std::string& requestBody = std::string(),
                           const std::string& marker = std::string());

    // Remove queued prewarm actions; with |abortInFlight| also cut short
    // one that is running (its socket is closed).
    void DropQueuedPrewarm(bool abortInFlight);
    // Something other than a prewarm is taking the slot: bump the
    // queue's slot epoch (an in-progress prewarm that hasn't submitted
    // its prefix yet will not) and forget the primed key.  Primed /
    // pending prewarm state lives in SlotActionQueue, where the worker
    // records what actually happened.
    void SupersedePrewarm();
    int           m_port = 8384;
    std::shared_ptr<std::atomic<bool>> m_healthCancelFlag;

    // Lifecycle identity for the currently active launch attempt.  Only the
    // wx main thread mutates this state.  Explicit StopServer() invalidates
    // queued events by advancing the generation; StartServer() advances it
    // once more for the new attempt and stamps every event with that value.
    ServerLaunchGeneration m_launchGeneration = kInvalidServerLaunchGeneration;
    std::chrono::steady_clock::time_point m_launchStartedAt{};

    // ── Per-load startup fallback state ─────────────────────────
    // Cached args allow a failed launch to be repeated with one
    // optional feature removed.  Force-off flags survive only while
    // retrying the same (model, config); a fresh user-driven launch
    // resets them and tries the preferred feature set again.
    std::string   m_lastGgufPath;
    ServerConfig  m_lastConfig;
    bool          m_mtpForceOff          = false;
    bool          m_mtpRetryAttempted    = false;
    bool          m_jinjaForceOff        = false;
    bool          m_jinjaRetryAttempted  = false;

    // Actual feature set of the process most recently spawned.  These
    // intentionally survive NotifyServerReady()/ResetStartupRetryState(),
    // because they describe the running server rather than retry intent.
    bool          m_currentMtpEnabled   = false;
    bool          m_currentJinjaEnabled = false;

    ServerLaunchGeneration AdvanceLaunchGeneration();
    void StopServerInternal(bool invalidateGeneration);
    void ResetStartupRetryState();
    void KillProcess();
};
