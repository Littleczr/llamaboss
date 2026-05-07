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
// Forward declaration — full definition pulled in from server_manager.h
// where needed. Defining it here avoids a cycle with server_manager.h
// including us (via AppState pointer in some files).
struct ServerConfig;

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

private:
    // Configuration data
    std::string m_currentModel;
    std::string m_currentApiUrl;
    std::string m_defaultModel;
    std::string m_defaultApiUrl;
    int         m_ctxSize = 8192;  // tokens — default matches ServerConfig default
    int         m_fontSize = 16;   // points — default chat font size
    bool        m_agentDefaultOn = false;  // seed for new chats / app launches

    // Application components
    Poco::Logger* m_logger;
    ThemeManager m_themeManager;

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
    static const char* CONFIG_FIRST_RUN_KEY;
};
