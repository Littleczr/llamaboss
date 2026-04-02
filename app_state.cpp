// app_state.cpp
#include "app_state.h"
#include "chat_client.h"  // For UnloadModel

// wxWidgets headers
#include <wx/fileconf.h>
#include <wx/log.h>
#include <wx/icon.h>
#include <wx/display.h>

// Poco headers
#include <Poco/ConsoleChannel.h>
#include <Poco/PatternFormatter.h>
#include <Poco/FormattingChannel.h>
#include <Poco/AutoPtr.h>

// Configuration constants
const char* AppState::CONFIG_APP_NAME = "OllamaChatApp";
const char* AppState::CONFIG_MODEL_KEY = "Model";
const char* AppState::CONFIG_API_URL_KEY = "ApiBaseUrl";
const char* AppState::CONFIG_THEME_KEY = "Theme";

AppState::AppState()
    : m_currentModel("")
    , m_currentApiUrl("")
    , m_defaultModel("llama3")
    , m_defaultApiUrl("http://127.0.0.1:11434")
    , m_logger(nullptr)
{
    SetDefaults();
}

AppState::~AppState()
{
    LogShutdownMessage();

    // Unload current model if we have valid configuration
    if (HasValidConfiguration()) {
        ChatClient::UnloadModel(m_currentModel, m_currentApiUrl, m_logger);
    }
}

bool AppState::Initialize()
{
    try {
        // Initialize logging first
        InitializeLogger();

        // Load configuration from file
        LoadSettings();

        // Log successful initialization
        LogStartupMessage();

        return true;
    }
    catch (const std::exception& ex) {
        if (m_logger) {
            m_logger->error("Failed to initialize application state: " + std::string(ex.what()));
        }
        return false;
    }
}

void AppState::SetModel(const std::string& model)
{
    if (m_currentModel != model) {
        std::string previousModel = m_currentModel;
        m_currentModel = model;

        if (m_logger) {
            m_logger->information("Model changed from '" + previousModel + "' to '" + model + "'");
        }
    }
}

void AppState::SetApiUrl(const std::string& apiUrl)
{
    if (m_currentApiUrl != apiUrl) {
        std::string previousUrl = m_currentApiUrl;
        m_currentApiUrl = apiUrl;

        if (m_logger) {
            m_logger->information("API URL changed from '" + previousUrl + "' to '" + apiUrl + "'");
        }
    }
}

void AppState::SetTheme(const std::string& themeName)
{
    std::string previous = m_themeManager.GetActiveThemeName();
    if (previous != themeName) {
        m_themeManager.SetActiveTheme(themeName);
        SaveSettings();

        if (m_logger) {
            m_logger->information("Theme changed from '" + previous + "' to '" + themeName + "'");
        }
    }
}

void AppState::SaveSettings()
{
    try {
        wxFileConfig cfg(CONFIG_APP_NAME);
        cfg.Write(CONFIG_MODEL_KEY, wxString::FromUTF8(m_currentModel));
        cfg.Write(CONFIG_API_URL_KEY, wxString::FromUTF8(m_currentApiUrl));
        cfg.Write(CONFIG_THEME_KEY, wxString::FromUTF8(m_themeManager.GetActiveThemeName()));
        cfg.Flush();

        if (m_logger) {
            m_logger->information("Settings saved - Model: " + m_currentModel +
                ", API: " + m_currentApiUrl + ", Theme: " + m_themeManager.GetActiveThemeName());
        }
    }
    catch (const std::exception& ex) {
        if (m_logger) {
            m_logger->error("Failed to save settings: " + std::string(ex.what()));
        }
    }
}

wxFont AppState::CreateMonospaceFont(int size) const
{
    return wxFont(size, wxFONTFAMILY_TELETYPE,
        wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL,
        false, "Cascadia Code");
}

bool AppState::LoadApplicationIcon(wxFrame* frame, const std::string& iconPath)
{
    if (!frame) {
        return false;
    }

    wxIcon icon;
    if (icon.LoadFile(iconPath, wxBITMAP_TYPE_ICO)) {
        frame->SetIcon(icon);
        if (m_logger) {
            m_logger->information("Application icon loaded: " + iconPath);
        }
        return true;
    }
    else {
        wxLogWarning("Could not load application icon: %s", iconPath.c_str());
        if (m_logger) {
            m_logger->warning("Could not load application icon: " + iconPath);
        }
        return false;
    }
}

bool AppState::UpdateSettings(const std::string& newModel, const std::string& newApiUrl,
    bool& modelChanged, bool& apiUrlChanged)
{
    modelChanged = (m_currentModel != newModel);
    apiUrlChanged = (m_currentApiUrl != newApiUrl);

    bool anyChange = modelChanged || apiUrlChanged;

    if (anyChange) {
        std::string previousModel = m_currentModel;

        // Update the settings
        SetModel(newModel);
        SetApiUrl(newApiUrl);

        // Save to configuration file
        SaveSettings();

        // Unload previous model if model changed
        if (modelChanged && !previousModel.empty() && previousModel != newModel) {
            ChatClient::UnloadModel(previousModel, m_currentApiUrl, m_logger);
        }

        if (m_logger) {
            m_logger->information("Settings updated - Model changed: " +
                std::string(modelChanged ? "Yes" : "No") +
                ", API changed: " + std::string(apiUrlChanged ? "Yes" : "No"));
        }
    }

    return anyChange;
}

void AppState::LogStartupMessage() const
{
    if (m_logger) {
        m_logger->information("Application started - Model: " + m_currentModel +
            ", API: " + m_currentApiUrl + ", Theme: " + m_themeManager.GetActiveThemeName());
    }
}

void AppState::LogShutdownMessage() const
{
    if (m_logger) {
        m_logger->information("Application shutting down");
    }
}

bool AppState::HasValidConfiguration() const
{
    return !m_currentModel.empty() && !m_currentApiUrl.empty();
}

void AppState::SaveWindowState(wxFrame* frame)
{
    if (!frame) return;

    try {
        wxFileConfig cfg(CONFIG_APP_NAME);

        bool maximized = frame->IsMaximized();
        cfg.Write("WindowMaximized", maximized);

        // Save normal (non-maximized) position and size
        // If maximized, restore first to get the normal geometry
        if (!maximized) {
            wxPoint pos = frame->GetPosition();
            wxSize size = frame->GetSize();
            cfg.Write("WindowX", pos.x);
            cfg.Write("WindowY", pos.y);
            cfg.Write("WindowW", size.x);
            cfg.Write("WindowH", size.y);
        }

        cfg.Flush();

        if (m_logger) {
            m_logger->debug("Window state saved");
        }
    }
    catch (const std::exception& ex) {
        if (m_logger) {
            m_logger->error("Failed to save window state: " + std::string(ex.what()));
        }
    }
}

void AppState::RestoreWindowState(wxFrame* frame)
{
    if (!frame) return;

    try {
        wxFileConfig cfg(CONFIG_APP_NAME);

        int x, y, w, h;
        bool hasPos = cfg.Read("WindowX", &x) && cfg.Read("WindowY", &y);
        bool hasSize = cfg.Read("WindowW", &w) && cfg.Read("WindowH", &h);

        if (hasSize && w > 200 && h > 150) {
            frame->SetSize(w, h);
        }

        if (hasPos) {
            // Sanity check: make sure the window is on a visible display
            wxRect windowRect(x, y, hasSize ? w : 1100, hasSize ? h : 700);
            bool onScreen = false;
            for (unsigned int i = 0; i < wxDisplay::GetCount(); ++i) {
                wxDisplay display(i);
                if (display.GetClientArea().Intersects(windowRect)) {
                    onScreen = true;
                    break;
                }
            }

            if (onScreen) {
                frame->SetPosition(wxPoint(x, y));
            }
            else {
                frame->Centre();
            }
        }

        bool maximized = false;
        if (cfg.Read("WindowMaximized", &maximized) && maximized) {
            frame->Maximize(true);
        }

        if (m_logger) {
            m_logger->debug("Window state restored");
        }
    }
    catch (const std::exception& ex) {
        if (m_logger) {
            m_logger->error("Failed to restore window state: " + std::string(ex.what()));
        }
    }
}

// Private methods

void AppState::LoadSettings()
{
    wxFileConfig cfg(CONFIG_APP_NAME);

    wxString savedModel, savedApiUrl;

    // Load model setting
    if (cfg.Read(CONFIG_MODEL_KEY, &savedModel)) {
        m_currentModel = savedModel.ToStdString();
    }

    // Load API URL setting
    if (cfg.Read(CONFIG_API_URL_KEY, &savedApiUrl)) {
        m_currentApiUrl = savedApiUrl.ToStdString();
    }

    // Load theme setting
    wxString savedTheme;
    if (cfg.Read(CONFIG_THEME_KEY, &savedTheme)) {
        m_themeManager.SetActiveTheme(savedTheme.ToStdString());
    }

    // Ensure we have valid defaults
    if (m_currentModel.empty()) {
        m_currentModel = m_defaultModel;
    }
    if (m_currentApiUrl.empty()) {
        m_currentApiUrl = m_defaultApiUrl;
    }
}

void AppState::InitializeLogger()
{
    // Set up Poco logger with console output
    Poco::AutoPtr<Poco::ConsoleChannel> pCons(new Poco::ConsoleChannel);
    Poco::AutoPtr<Poco::PatternFormatter> pPF(new Poco::PatternFormatter);
    pPF->setProperty("pattern", "%Y-%m-%d %H:%M:%S.%F %s: %t");
    Poco::AutoPtr<Poco::FormattingChannel> pFC(
        new Poco::FormattingChannel(pPF, pCons)
    );

    // Configure root logger
    Poco::Logger::root().setChannel(pFC);
    Poco::Logger::root().setLevel(Poco::Message::PRIO_INFORMATION);

    // Get our application logger
    m_logger = &Poco::Logger::get("OllamaChatApp");
}

void AppState::SetDefaults()
{
    m_currentModel = m_defaultModel;
    m_currentApiUrl = m_defaultApiUrl;
}