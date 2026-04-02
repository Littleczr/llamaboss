#pragma once
// app_state.h
#ifndef APP_STATE_H
#define APP_STATE_H

#include <wx/wx.h>
#include <wx/font.h>
#include <string>

// Poco headers
#include <Poco/Logger.h>

// Theme system
#include "theme.h"

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

    // Save current settings to configuration file
    void SaveSettings();

    // Logger access
    Poco::Logger* GetLogger() { return m_logger; }

    // UI setup helpers
    wxFont CreateMonospaceFont(int size = 14) const;
    bool LoadApplicationIcon(wxFrame* frame, const std::string& iconPath = "app_icon.ico");

    // Settings update handling
    bool UpdateSettings(const std::string& newModel, const std::string& newApiUrl,
        bool& modelChanged, bool& apiUrlChanged);

    // Utility methods
    void LogStartupMessage() const;
    void LogShutdownMessage() const;
    bool HasValidConfiguration() const;

    // Window state persistence
    void SaveWindowState(wxFrame* frame);
    void RestoreWindowState(wxFrame* frame);

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
};

#endif // APP_STATE_H