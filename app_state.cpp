// app_state.cpp
#include "app_state.h"

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
const char* AppState::CONFIG_APP_NAME = "LlamaBoss";
const char* AppState::CONFIG_MODEL_KEY = "Model";
const char* AppState::CONFIG_API_URL_KEY = "ApiBaseUrl";
const char* AppState::CONFIG_THEME_KEY = "Theme";

AppState::AppState()
    : m_currentModel("")
    , m_currentApiUrl("")
    , m_defaultModel("")                           // No default — set from GGUF scan
    , m_defaultApiUrl("http://127.0.0.1:8384")     // llama-server default port
    , m_logger(nullptr)
{
    SetDefaults();
}

AppState::~AppState()
{
    LogShutdownMessage();
    // No more Ollama model unloading — ServerManager handles process cleanup
}

bool AppState::Initialize()
{
    try {
        InitializeLogger();
        LoadSettings();
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

bool AppState::LoadApplicationIcon(wxFrame* frame, const std::string& /*iconPath*/)
{
    if (!frame) {
        return false;
    }

#ifdef __WXMSW__
    wxIcon icon("APP_ICON", wxBITMAP_TYPE_ICO_RESOURCE);
    if (icon.IsOk()) {
        frame->SetIcon(icon);
        if (m_logger) {
            m_logger->information("Application icon loaded from embedded resource");
        }
        return true;
    }
#endif

    wxIcon fileIcon;
    if (fileIcon.LoadFile("app_icon.ico", wxBITMAP_TYPE_ICO)) {
        frame->SetIcon(fileIcon);
        if (m_logger) {
            m_logger->information("Application icon loaded from file");
        }
        return true;
    }

    if (m_logger) {
        m_logger->warning("Could not load application icon");
    }
    return false;
}

bool AppState::UpdateSettings(const std::string& newModel, const std::string& newApiUrl,
    bool& modelChanged, bool& apiUrlChanged)
{
    modelChanged = (m_currentModel != newModel);
    apiUrlChanged = (m_currentApiUrl != newApiUrl);

    bool anyChange = modelChanged || apiUrlChanged;

    if (anyChange) {
        SetModel(newModel);
        SetApiUrl(newApiUrl);
        SaveSettings();

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

int AppState::GetSidebarWidth() const
{
    wxFileConfig cfg(CONFIG_APP_NAME);
    int w = 260;
    cfg.Read("SidebarWidth", &w);
    return w;
}

void AppState::SetSidebarWidth(int w)
{
    wxFileConfig cfg(CONFIG_APP_NAME);
    cfg.Write("SidebarWidth", w);
    cfg.Flush();
}

// Private methods

void AppState::LoadSettings()
{
    wxFileConfig cfg(CONFIG_APP_NAME);

    wxString savedModel, savedApiUrl;

    if (cfg.Read(CONFIG_MODEL_KEY, &savedModel)) {
        m_currentModel = savedModel.ToUTF8().data();
    }

    if (cfg.Read(CONFIG_API_URL_KEY, &savedApiUrl)) {
        m_currentApiUrl = savedApiUrl.ToUTF8().data();
    }

    wxString savedTheme;
    if (cfg.Read(CONFIG_THEME_KEY, &savedTheme)) {
        m_themeManager.SetActiveTheme(savedTheme.ToUTF8().data());
    }

    // Keep defaults if nothing was saved
    if (m_currentModel.empty()) {
        m_currentModel = m_defaultModel;
    }
    if (m_currentApiUrl.empty()) {
        m_currentApiUrl = m_defaultApiUrl;
    }
}

void AppState::InitializeLogger()
{
    Poco::AutoPtr<Poco::ConsoleChannel> pCons(new Poco::ConsoleChannel);
    Poco::AutoPtr<Poco::PatternFormatter> pPF(new Poco::PatternFormatter);
    pPF->setProperty("pattern", "%Y-%m-%d %H:%M:%S.%F %s: %t");
    Poco::AutoPtr<Poco::FormattingChannel> pFC(
        new Poco::FormattingChannel(pPF, pCons)
    );

    Poco::Logger::root().setChannel(pFC);
    Poco::Logger::root().setLevel(Poco::Message::PRIO_INFORMATION);

    m_logger = &Poco::Logger::get("LlamaBoss");
}

void AppState::SetDefaults()
{
    m_currentModel = m_defaultModel;
    m_currentApiUrl = m_defaultApiUrl;
}
