// server_manager.h
#pragma once

#include <wx/wx.h>
#include <wx/thread.h>
#include <string>
#include <vector>
#include <memory>
#include <atomic>

#ifdef __WXMSW__
#include <windows.h>
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
};

// ── Backend type ────────────────────────────────────────────────
enum class Backend { CPU, CUDA12 };

// ── Custom events ───────────────────────────────────────────────
wxDECLARE_EVENT(wxEVT_SERVER_READY, wxCommandEvent);
wxDECLARE_EVENT(wxEVT_SERVER_ERROR, wxCommandEvent);

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
#endif
                       const std::string& logPath,
                       int timeoutMs = 60000);
protected:
    ExitCode Entry() override;
private:
    wxEvtHandler* m_handler;
    std::string   m_baseUrl;
    std::shared_ptr<std::atomic<bool>> m_cancelFlag;
    std::weak_ptr<std::atomic<bool>> m_aliveToken;
#ifdef __WXMSW__
    HANDLE        m_processHandle;   // duplicated — owned by this thread, closed in Entry()
#endif
    std::string   m_logPath;
    int m_timeoutMs;
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

    // ── Phase 3a: --jinja with fallback retry ────────────────────
    // llama-server's native function-calling support requires the
    // --jinja flag.  We add it by default (Phase 1's commitment to
    // "snappy + state-of-the-art") and fall back gracefully if the
    // active model's chat template doesn't compile under Jinja or
    // the server otherwise refuses to start.
    //
    // Call this from MyFrame::OnServerError BEFORE delegating to
    // ModelSwitcher.  Returns true iff a retry was kicked off — in
    // that case the caller suppresses the user-visible error and
    // waits for the next wxEVT_SERVER_READY / wxEVT_SERVER_ERROR.
    // Returns false if no retry is possible (already retried, jinja
    // wasn't on this attempt, no model loaded, etc.) and the caller
    // should display the error normally.
    bool MaybeRetryWithoutJinja(const std::string& error);

    // Call this from MyFrame::OnServerReady so the retry state is
    // cleared on success.  Without this, a successful retry leaves
    // m_jinjaForceOff = true and the next user-initiated load would
    // skip jinja for the wrong model.
    void NotifyServerReady();

    // True only for the currently running llama-server process when
    // that process was launched with --jinja. Phase 3 tool-call
    // detection must check this because native function calling is a
    // server-runtime capability, not only a model/template capability.
    bool IsCurrentServerJinjaEnabled() const { return m_currentJinjaEnabled; }

    // Accessors
    std::string GetBaseUrl() const;
    std::string GetLoadedModel() const { return m_loadedModel; }
    std::string GetLoadedMmproj() const { return m_loadedMmproj; }

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
    // default. Lives under %USERPROFILE%\LlamaBoss\Workspace so it
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
    static std::string GetDefaultWorkspaceDir();    // %USERPROFILE%\LlamaBoss\Workspace
    static std::string GetWorkspaceDir();
    static std::string GetWorkspaceDirOverride();
    static void        SetWorkspaceDirOverride(const std::string& path);
    static void        EnsureWorkspaceDir();

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

    // Legacy path-only scan — kept for settings-combo backward compat
    // where callers only want a flat list of .gguf paths to load.
    // New code should use ScanModels() and consume ModelEntry directly.
    static std::vector<std::string> ScanModelPaths();

private:
#ifdef __WXMSW__
    HANDLE m_processHandle = INVALID_HANDLE_VALUE;
    HANDLE m_threadHandle  = INVALID_HANDLE_VALUE;
    DWORD  m_processId     = 0;
#endif

    wxEvtHandler* m_eventHandler;
    std::weak_ptr<std::atomic<bool>> m_aliveToken;
    Poco::Logger* m_logger;
    std::string   m_loadedModel;
    std::string   m_loadedMmproj;   // Phase 3b: paired mmproj (or empty)
    int           m_port = 8384;
    std::shared_ptr<std::atomic<bool>> m_healthCancelFlag;

    // ── Retry-without-jinja state (Phase 3a) ─────────────────────
    // Cached args for the in-flight load attempt, so MaybeRetryWith
    // outJinja can re-launch with the same model and config minus
    // the --jinja flag.  m_jinjaForceOff is consulted by StartServer
    // when building the command line; it's set during a retry and
    // cleared by ResetJinjaRetryState() at the start of a fresh
    // StartServer call (so a brand-new model load always tries
    // jinja first).
    std::string   m_lastGgufPath;
    ServerConfig  m_lastConfig;
    bool          m_jinjaForceOff       = false;
    bool          m_jinjaRetryAttempted = false;

    // Tracks the actual running server process. This intentionally
    // survives NotifyServerReady()/ResetJinjaRetryState(), because those
    // reset per-load retry flags while the process may still be the
    // no-jinja fallback server.
    bool          m_currentJinjaEnabled = false;

    void ResetJinjaRetryState();
    void KillProcess();
};
