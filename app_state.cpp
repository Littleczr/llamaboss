// app_state.cpp
#include "app_state.h"
#include "server_manager.h"   // for ServerConfig (MakeServerConfig)

// wxWidgets headers
#include <wx/fileconf.h>
#include <wx/log.h>
#include <wx/icon.h>
#include <wx/display.h>

// Poco headers
#include <Poco/FileChannel.h>
#include <Poco/PatternFormatter.h>
#include <Poco/FormattingChannel.h>
#include <Poco/AutoPtr.h>

// Configuration constants
const char* AppState::CONFIG_APP_NAME = "LlamaBoss";
const char* AppState::CONFIG_MODEL_KEY = "Model";
const char* AppState::CONFIG_API_URL_KEY = "ApiBaseUrl";
const char* AppState::CONFIG_THEME_KEY = "Theme";
const char* AppState::CONFIG_CTX_SIZE_KEY = "ContextLength";
const char* AppState::CONFIG_FONT_SIZE_KEY = "ChatFontSize";
const char* AppState::CONFIG_AGENT_DEFAULT_ON_KEY = "AgentDefaultOn";
const char* AppState::CONFIG_FIRST_RUN_KEY = "FirstRunCompleted";

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

        // ── First-run migration ──────────────────────────────────
        // Pre-existing installs have models on disk but no
        // FirstRunCompleted key (the feature didn't exist when they
        // installed). Without this block they'd hit the first-run
        // downloader if they ever emptied their models folder, which
        // would feel wrong for someone who's been using the app for
        // months. Mark them as past-first-run silently on their first
        // launch of this build. Static ScanModelPaths doesn't need a
        // ServerManager instance; wxApp::SetAppName has already been
        // called by the time we get here, so path resolution works.
        if (IsFirstRun() && !ServerManager::ScanModelPaths().empty()) {
            MarkFirstRunComplete();
        }

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

void AppState::SetCtxSize(int n)
{
    // Clamp to the slider's supported range so an out-of-band value from
    // a hand-edited config file can't propagate to llama-server.
    if (n < 4096)   n = 4096;
    if (n > 262144) n = 262144;

    if (m_ctxSize != n) {
        int previous = m_ctxSize;
        m_ctxSize = n;
        SaveSettings();
        if (m_logger)
            m_logger->information("Context length changed from " +
                std::to_string(previous) + " to " + std::to_string(n));
    }
}

void AppState::SetFontSize(int pt)
{
    // Clamp to the range we expose in Settings — defense against stale
    // values in a hand-edited config.
    if (pt < 10) pt = 10;
    if (pt > 24) pt = 24;

    if (m_fontSize != pt) {
        int previous = m_fontSize;
        m_fontSize = pt;
        SaveSettings();
        if (m_logger)
            m_logger->information("Chat font size changed from " +
                std::to_string(previous) + " to " + std::to_string(pt) + "pt");
    }
}

void AppState::SetAgentDefaultOn(bool on)
{
    if (m_agentDefaultOn != on) {
        m_agentDefaultOn = on;
        SaveSettings();
        if (m_logger)
            m_logger->information(std::string("Agent default changed to ") +
                (on ? "ON" : "OFF"));
    }
}

ServerConfig AppState::MakeServerConfig() const
{
    ServerConfig cfg;       // defaults for port / gpuLayers / threads / flashAttn
    cfg.ctxSize = m_ctxSize;
    return cfg;
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
        cfg.Write(CONFIG_CTX_SIZE_KEY, (long)m_ctxSize);
        cfg.Write(CONFIG_FONT_SIZE_KEY, (long)m_fontSize);
        cfg.Write(CONFIG_AGENT_DEFAULT_ON_KEY, m_agentDefaultOn);
        cfg.Flush();

        if (m_logger) {
            m_logger->information("Settings saved - Model: " + m_currentModel +
                ", API: " + m_currentApiUrl +
                ", Theme: " + m_themeManager.GetActiveThemeName() +
                ", CtxSize: " + std::to_string(m_ctxSize) +
                ", FontSize: " + std::to_string(m_fontSize));
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

// ── Collapsed project sections in the sidebar ──────────────────────
//
// Stored as a single comma-separated string under "CollapsedProjects".
// Project IDs are sanitized (lowercase alphanumeric + hyphen) so commas
// can never appear inside an ID and the round-trip is unambiguous.

std::vector<std::string> AppState::GetCollapsedProjectIds() const
{
    wxFileConfig cfg(CONFIG_APP_NAME);
    wxString joined;
    cfg.Read("CollapsedProjects", &joined);

    std::vector<std::string> out;
    if (joined.IsEmpty()) return out;

    std::string raw = joined.ToUTF8().data();
    size_t start = 0;
    while (start <= raw.size()) {
        size_t comma = raw.find(',', start);
        std::string token = (comma == std::string::npos)
            ? raw.substr(start)
            : raw.substr(start, comma - start);
        if (!token.empty())
            out.push_back(token);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

void AppState::SetCollapsedProjectIds(const std::vector<std::string>& ids)
{
    std::string joined;
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i > 0) joined += ',';
        joined += ids[i];
    }
    wxFileConfig cfg(CONFIG_APP_NAME);
    cfg.Write("CollapsedProjects", wxString::FromUTF8(joined.c_str()));
    cfg.Flush();
}

// ── First-run tracking ───────────────────────────────────────────
//
// Uses wxFileConfig directly rather than going through LoadSettings/
// SaveSettings. This keeps the flag on an independent lifecycle: a
// theme change shouldn't touch it, and the mark-complete call shouldn't
// rewrite every other setting on disk.
//
// On a fresh install the key is absent; cfg.Read returns false and the
// default-initialized `completed` stays false, so IsFirstRun() returns
// true. That's the correct starting state.

bool AppState::IsFirstRun() const
{
    wxFileConfig cfg(CONFIG_APP_NAME);
    bool completed = false;
    cfg.Read(CONFIG_FIRST_RUN_KEY, &completed);
    return !completed;
}

void AppState::MarkFirstRunComplete()
{
    wxFileConfig cfg(CONFIG_APP_NAME);
    cfg.Write(CONFIG_FIRST_RUN_KEY, true);
    cfg.Flush();

    if (m_logger)
        m_logger->information("First-run onboarding completed");
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

    long savedCtx = 0;
    if (cfg.Read(CONFIG_CTX_SIZE_KEY, &savedCtx) && savedCtx > 0) {
        // SetCtxSize clamps to the valid range and logs, but here we're
        // loading from disk — skip the log spam and set directly.
        if (savedCtx < 4096)   savedCtx = 4096;
        if (savedCtx > 262144) savedCtx = 262144;
        m_ctxSize = (int)savedCtx;
    }

    long savedFont = 0;
    if (cfg.Read(CONFIG_FONT_SIZE_KEY, &savedFont) && savedFont > 0) {
        if (savedFont < 10) savedFont = 10;
        if (savedFont > 24) savedFont = 24;
        m_fontSize = (int)savedFont;
    }

    // Agent-mode default: bool, absent on fresh installs (falls through
    // to the m_agentDefaultOn = false initializer from the header).
    bool savedAgentDefault = false;
    if (cfg.Read(CONFIG_AGENT_DEFAULT_ON_KEY, &savedAgentDefault)) {
        m_agentDefaultOn = savedAgentDefault;
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
    // ── Where we log ─────────────────────────────────────────────
    // LlamaBoss is a GUI app with no attached console, so the older
    // ConsoleChannel sink dropped every log line on the floor. We
    // log to a real file alongside server.log so jinja decisions,
    // retry warnings, model-switch traces, and anything else our
    // own code emits is actually visible after the fact.
    //
    // EnsureDataDirs() creates the logs/ folder — we re-call it
    // here so the very first launch has the directory before Poco
    // tries to open the channel.
    ServerManager::EnsureDataDirs();
    std::string logPath = ServerManager::GetLogsDir() +
                          std::string(1, wxFILE_SEP_PATH) +
                          "llamaboss.log";

    Poco::AutoPtr<Poco::FileChannel> pFile(new Poco::FileChannel);
    pFile->setProperty("path", logPath);
    // Keep a single rolling file capped at a sensible size so the
    // log doesn't grow unbounded on long-lived installs. 5 MB is
    // plenty for human-eyeball debugging; older content rolls into
    // .0/.1 archives next to the live file.
    pFile->setProperty("rotation",  "5 M");
    pFile->setProperty("archive",   "number");
    pFile->setProperty("purgeCount", "3");

    Poco::AutoPtr<Poco::PatternFormatter> pPF(new Poco::PatternFormatter);
    pPF->setProperty("pattern", "%Y-%m-%d %H:%M:%S.%F %s: %t");
    Poco::AutoPtr<Poco::FormattingChannel> pFC(
        new Poco::FormattingChannel(pPF, pFile)
    );

    Poco::Logger::root().setChannel(pFC);
    Poco::Logger::root().setLevel(Poco::Message::PRIO_INFORMATION);

    m_logger = &Poco::Logger::get("LlamaBoss");
    m_logger->information("LlamaBoss log opened: " + logPath);
}

void AppState::SetDefaults()
{
    m_currentModel = m_defaultModel;
    m_currentApiUrl = m_defaultApiUrl;
}
