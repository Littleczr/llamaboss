#pragma once
// app_state.h

#include <wx/wx.h>
#include <wx/font.h>
#include <string>
#include <vector>

// Poco headers
#include <Poco/Logger.h>

// Theme system
#include "theme.h"
// Active inference target descriptor (transport URL/path/tls/auth/protocol).
// tool_protocol.h (pulled in transitively) is wx-only, so this stays light.
#include "inference_target.h"
// Forward declaration — full definition pulled in from server_manager.h
// where needed. Defining it here avoids a cycle with server_manager.h
// including us (via AppState pointer in some files).
struct ServerConfig;

// Forward declaration to avoid pulling Poco JSON into every TU that
// includes app_state.h.
class SecretsStore;
class EndpointStore;

#include <memory>

// Application state and configuration management
class AppState
{
public:
    AppState();
    ~AppState();

    // Initialization - call once at application startup
    bool Initialize();

    // Configuration management
    std::string GetModel() const { return m_currentModel; }
    std::string GetApiUrl() const { return m_currentApiUrl; }
    void SetModel(const std::string& model);
    void SetApiUrl(const std::string& apiUrl);

    // ── Active inference target ──────────────────────────────────
    // The resolved endpoint the next chat turn is sent to. For the
    // local lane this mirrors GetApiUrl()/GetModel() as a plain-http,
    // no-auth, OpenAI-compatible target — SetApiUrl()/SetModel() keep
    // it in sync, so existing callers need no changes. A remote
    // endpoint installs the full descriptor (TLS + auth) via
    // SetActiveTarget(), which also updates GetApiUrl() to match.
    const InferenceTarget& GetActiveTarget() const { return m_activeTarget; }
    void SetActiveTarget(const InferenceTarget& target);

    // ── Last model selection (persisted) ─────────────────────────
    // The user's most recent model choice, stored independently of the
    // session model state so it survives a relaunch. For a local model
    // this is the .gguf path; for a remote model it is the picker key
    // "remote:<endpoint>/<model>". StartInitialServer reads it at boot to
    // restore that choice — including re-activating a remote endpoint
    // without spawning a local server. Written immediately on its own
    // lifecycle (like the sidebar width / first-run flag), not bundled
    // with SaveSettings.
    std::string GetLastSelection() const;
    void SetLastSelection(const std::string& selection);

    // Context length (tokens) used when launching llama-server.
    // Persisted across sessions via wxFileConfig.
    int  GetCtxSize() const { return m_ctxSize; }
    void SetCtxSize(int n);

    // Chat font size in points. Applied to both the chat display and the
    // input control. Persisted across sessions.
    int  GetFontSize() const { return m_fontSize; }
    void SetFontSize(int pt);

    // Agent-mode default. When true, new chats (and app launches) seed
    // m_agentModeEnabled to ON. The robot button still toggles the live
    // flag per chat without touching this setting. Persisted across
    // sessions. Default: false — users opt into tool execution.
    bool GetAgentDefaultOn() const { return m_agentDefaultOn; }
    void SetAgentDefaultOn(bool on);

    // Context meter: show "ctx <used>/<window>" occupancy in the top
    // bar.  The setting controls WIDGET VISIBILITY ONLY — the token
    // accounting always runs (it is two integers parsed from a usage
    // object the client already deserializes), so toggling the meter
    // on mid-conversation shows an exact number immediately instead of
    // an estimate.  Persisted across sessions.  Default: true.
    bool GetContextMeterOn() const { return m_contextMeterOn; }
    void SetContextMeterOn(bool on);

    // 8-bit KV cache: launch the local llama-server with q8_0 K/V
    // cache types (halves KV memory — roughly doubles the context
    // that fits in VRAM).  Launch-argument setting: changes require
    // a server restart, handled by the Settings flow like ctx size.
    // Persisted across sessions.  Default: true.
    bool GetKvCacheQ8() const { return m_kvCacheQ8; }
    void SetKvCacheQ8(bool on);

    // Maximum tool steps per agent turn (the iteration safety cap).
    // Persisted across sessions; AgentController clamps and applies it.
    // Default 12 matches the historical AgentController::kMaxIterations.
    int  GetAgentMaxToolSteps() const { return m_agentMaxToolSteps; }
    void SetAgentMaxToolSteps(int steps);

    // Build a ServerConfig populated with the user's current settings.
    // Callers use this instead of the default-constructed ServerConfig
    // so ctx size / future options actually flow through to llama-server.
    // Defined in app_state.cpp (needs the full ServerConfig type).
    ServerConfig MakeServerConfig() const;

    // Save current settings to configuration file
    void SaveSettings();

    // Logger access
    Poco::Logger* GetLogger() { return m_logger; }

    // UI setup helpers
    wxFont CreateMonospaceFont(int size = 14) const;
    bool LoadApplicationIcon(wxFrame* frame, const std::string& iconPath = "app_icon.ico");

    // Settings update handling (simplified — no more Ollama model unloading)
    bool UpdateSettings(const std::string& newModel, const std::string& newApiUrl,
        bool& modelChanged, bool& apiUrlChanged);

    // Utility methods
    void LogStartupMessage() const;
    void LogShutdownMessage() const;
    bool HasValidConfiguration() const;

    // Window state persistence
    void SaveWindowState(wxFrame* frame);
    void RestoreWindowState(wxFrame* frame);

    // Sidebar width persistence
    int  GetSidebarWidth() const;
    void SetSidebarWidth(int w);

    // ── Collapsed project sections in the sidebar ────────────────
    // Persisted as a comma-separated list of project IDs.  An empty
    // list means every section is expanded.  The pseudo-id
    // "__unassigned__" represents the Unassigned section, which is
    // treated like any other group for collapse purposes.
    std::vector<std::string> GetCollapsedProjectIds() const;
    void SetCollapsedProjectIds(const std::vector<std::string>& ids);

    // ── First-run tracking ───────────────────────────────────────
    // IsFirstRun() returns true when the user has not yet completed
    // onboarding (downloaded their first model and seen it load).
    // Default on a fresh install: true — the config key is absent.
    //
    // MarkFirstRunComplete() is called exactly once per install,
    // when the server reaches ready state on the first-run path.
    // Persists immediately so a crash before next launch doesn't
    // erase the fact that onboarding succeeded.
    //
    // These intentionally live outside SaveSettings()/LoadSettings()
    // so a routine settings change (theme, font, ctx) doesn't touch
    // the flag — the flag has its own lifecycle bound to onboarding.
    bool IsFirstRun() const;
    void MarkFirstRunComplete();

    // Theme management
    const ThemeData& GetTheme() const { return m_themeManager.GetActiveTheme(); }
    ThemeManager& GetThemeManager() { return m_themeManager; }
    std::string GetThemeName() const { return m_themeManager.GetActiveThemeName(); }
    void SetTheme(const std::string& themeName);

    // ── Secrets / Connections ────────────────────────────────────
    // SecretsStore owns %LOCALAPPDATA%\LlamaBoss\secrets.json.
    // Lazily constructed on first access; Load() runs the first
    // time too.  Mutations happen via the Connections dialog;
    // callers are responsible for invoking Save() when the user
    // accepts changes.  PythonRunner reads it (via the same
    // pointer) when spawning python_run_script subprocesses.
    SecretsStore* GetSecretsStore();

    // ── Endpoints / remote inference ─────────────────────────────
    // EndpointStore owns %LOCALAPPDATA%\LlamaBoss\endpoints.json — the
    // non-secret config for remote OpenAI-compatible endpoints (URLs,
    // headers, model lists, and which SecretsStore key to use). Lazily
    // constructed and Load()ed on first access, exactly like the
    // secrets store. The model switcher reads it to populate the picker
    // and to resolve a selection into an InferenceTarget.
    EndpointStore* GetEndpointStore();

private:
    // Configuration data
    std::string m_currentModel;
    std::string m_currentApiUrl;

    // Resolved send endpoint. Kept in sync with m_currentModel /
    // m_currentApiUrl for the local lane; replaced wholesale for a
    // remote endpoint via SetActiveTarget(). m_currentApiUrl remains
    // the canonical persisted URL; this is the richer overlay the
    // transport reads.
    InferenceTarget m_activeTarget;

    std::string m_defaultModel;
    std::string m_defaultApiUrl;
    int         m_ctxSize = 8192;  // tokens — default matches ServerConfig default
    int         m_fontSize = 15;   // points — default chat font size
    bool        m_agentDefaultOn = false;  // seed for new chats / app launches
    bool        m_contextMeterOn = true;   // top-bar context occupancy readout
    bool        m_kvCacheQ8 = true;        // q8_0 KV cache on local launches
    int         m_agentMaxToolSteps = 12;  // agent tool-step safety cap

    // Application components
    Poco::Logger* m_logger;
    ThemeManager m_themeManager;

    // SecretsStore lifetime is managed via unique_ptr so the
    // forward-declared type works in this header.  Constructed on
    // first GetSecretsStore() call.
    std::unique_ptr<SecretsStore> m_secretsStore;

    // Lazily constructed on first GetEndpointStore() call (see above).
    std::unique_ptr<EndpointStore> m_endpointStore;

    // Configuration file handling
    void LoadSettings();
    void InitializeLogger();
    void SetDefaults();

    // Configuration keys
    static const char* CONFIG_APP_NAME;
    static const char* CONFIG_MODEL_KEY;
    static const char* CONFIG_API_URL_KEY;
    static const char* CONFIG_THEME_KEY;
    static const char* CONFIG_CTX_SIZE_KEY;
    static const char* CONFIG_FONT_SIZE_KEY;
    static const char* CONFIG_AGENT_DEFAULT_ON_KEY;
    static const char* CONFIG_CONTEXT_METER_KEY;
    static const char* CONFIG_KV_CACHE_Q8_KEY;
    static const char* CONFIG_AGENT_MAX_TOOL_STEPS_KEY;
    static const char* CONFIG_FIRST_RUN_KEY;
    static const char* CONFIG_LAST_SELECTION_KEY;
};
