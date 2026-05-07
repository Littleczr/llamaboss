// model_switcher.cpp
#include "model_switcher.h"
#include "app_state.h"
#include "server_manager.h"
#include "chat_display.h"
#include "chat_history.h"
#include "attachment_manager.h"
#include "widgets.h"
#include "model_manager.h"
#include "model_downloader.h"   // first-run onboarding
#include "theme.h"

ModelSwitcher::ModelSwitcher(AppState& appState,
                             ServerManager& serverManager,
                             ChatDisplay* chatDisplay,
                             std::unique_ptr<ChatHistory>& chatHistory,
                             AttachmentManager& attachments,
                             StatusDot* statusDot,
                             wxStaticText* modelLabel,
                             wxWindow* parentFrame)
    : m_appState(appState)
    , m_serverManager(serverManager)
    , m_chatDisplay(chatDisplay)
    , m_chatHistory(chatHistory)
    , m_attachments(attachments)
    , m_statusDot(statusDot)
    , m_modelLabel(modelLabel)
    , m_parentFrame(parentFrame)
{
}

// ═════════════════════════════════════════════════════════════════
//  SERVER BOOTSTRAP
// ═════════════════════════════════════════════════════════════════

void ModelSwitcher::StartInitialServer()
{
    auto models = ServerManager::ScanModelPaths();

    if (models.empty()) {
        // ── First-run onboarding path ─────────────────────────────
        // A fresh install with no models AND the first-run flag still
        // set means this is someone's very first encounter with the
        // app. Open the downloader in first-run mode — it pre-highlights
        // the starter model and auto-closes on successful download so
        // the caller (here) can pick up and load it immediately.
        //
        // Existing users landing here (e.g., they deleted all their
        // models) have been migrated past first-run in AppState::
        // Initialize(), so they take the else branch below and see
        // the normal empty-state message instead.
        if (m_appState.IsFirstRun() && m_parentFrame) {
            std::string downloaded = LaunchFirstRunDownloader();
            if (!downloaded.empty()) {
                // Success — persist the choice and fall through to the
                // load-model block below. Rescanning picks up the new
                // bundle folder we just populated.
                bool mc, ac;
                m_appState.UpdateSettings(downloaded, m_appState.GetApiUrl(),
                                          mc, ac);
                m_completingFirstRun = true;
                models = ServerManager::ScanModelPaths();
                // Fall through — models is now non-empty.
            } else {
                // Dismissed without downloading. Leave the first-run
                // flag set so they get the onboarding again next launch.
                ShowFirstRunDismissedMessage();
                ServerManager::EnsureDataDirs();
                return;
            }
        } else {
            // Existing user with an empty folder, or first-run user on
            // a build without a parent frame wired up. Show the original
            // empty-state message unchanged.
            m_statusDot->SetConnected(false);
            m_chatDisplay->DisplaySystemMessage(
                "No models found.\n\n"
                "To get started:\n"
                "1. Download a model through Settings \xe2\x86\x92 Download Models\n"
                "2. Or drop a .gguf file into: " + ServerManager::GetModelsDir() + "\n"
                "3. Open Settings to select it\n\n"
                "The models folder has been created for you.");

            ServerManager::EnsureDataDirs();
            return;
        }
    }

    // Use saved model if it still exists, otherwise first available
    std::string savedModel = m_appState.GetModel();
    std::string modelToLoad;

    if (!savedModel.empty() && wxFileExists(savedModel)) {
        modelToLoad = savedModel;
    }
    else {
        modelToLoad = models.front();
        bool mc, ac;
        m_appState.UpdateSettings(modelToLoad, m_appState.GetApiUrl(), mc, ac);
    }

    UpdateModelLabel();
    m_chatDisplay->DisplaySystemMessage(
        "Loading " + ServerManager::ModelDisplayName(modelToLoad) + "...");
    m_statusDot->SetConnected(false);

    m_serverManager.StartServer(modelToLoad, m_appState.MakeServerConfig());
}

// ═════════════════════════════════════════════════════════════════
//  SERVER EVENT HANDLERS
// ═════════════════════════════════════════════════════════════════

void ModelSwitcher::OnServerReady()
{
    m_serverReady = true;
    m_statusDot->SetConnected(true);

    m_appState.SetApiUrl(m_serverManager.GetBaseUrl());

    std::string displayName = ServerManager::ModelDisplayName(
        m_serverManager.GetLoadedModel());

    m_chatDisplay->DisplaySystemMessage(displayName + " ready.");
    UpdateModelLabel();

    if (auto* logger = m_appState.GetLogger())
        logger->information("Server ready: " + displayName);

    // ── First-run onboarding completion ──────────────────────────
    // The flag is only true after LaunchFirstRunDownloader returned
    // a path on the first-run path. Every other "server ready" event
    // (model switch, initial load for a returning user, restart after
    // error) leaves the flag false and this block is a no-op.
    if (m_completingFirstRun) {
        m_completingFirstRun = false;
        m_appState.MarkFirstRunComplete();
    }
}

void ModelSwitcher::OnServerError(const std::string& error)
{
    m_serverReady = false;
    m_statusDot->SetConnected(false);

    m_chatDisplay->DisplaySystemMessage("Server error: " + error);

    if (auto* logger = m_appState.GetLogger())
        logger->error("Server error: " + error);
}

// ═════════════════════════════════════════════════════════════════
//  MODEL PILL CLICKS
// ═════════════════════════════════════════════════════════════════

void ModelSwitcher::OnModelPillClick(wxWindow* popupParent)
{
    if (m_cb.isBusy && m_cb.isBusy()) return;

    auto models = ServerManager::ScanModelPaths();
    if (models.empty()) {
        m_chatDisplay->DisplaySystemMessage(
            "No models found in " + ServerManager::GetModelsDir() +
            "\nDownload one through Settings, or drop a .gguf file there.");
        return;
    }

    ShowModelPickerMenu(popupParent, models);
}

void ModelSwitcher::OnModelPillRightClick(wxWindow* parent)
{
    if (m_cb.isBusy && m_cb.isBusy()) return;
    ModelManagerDialog dlg(parent, &m_appState.GetTheme());
    dlg.ShowModal();
}

// ═════════════════════════════════════════════════════════════════
//  PICKER MENU
// ═════════════════════════════════════════════════════════════════

void ModelSwitcher::ShowModelPickerMenu(wxWindow* anchor,
                                        const std::vector<std::string>& ggufPaths)
{
    if (ggufPaths.empty()) return;

    m_pickerModels = ggufPaths;
    m_menuIdMap.clear();

    wxMenu menu;
    std::string currentModel = m_appState.GetModel();

    for (size_t i = 0; i < m_pickerModels.size(); ++i) {
        std::string display = ServerManager::ModelDisplayName(m_pickerModels[i]);
        int id = wxNewId();
        m_menuIdMap[id] = i;
        wxMenuItem* item = menu.AppendCheckItem(id, wxString::FromUTF8(display));
        if (m_pickerModels[i] == currentModel) {
            item->Check(true);
        }
    }

    menu.Bind(wxEVT_MENU, [this](wxCommandEvent& e) {
        auto it = m_menuIdMap.find(e.GetId());
        if (it != m_menuIdMap.end() && it->second < m_pickerModels.size()) {
            SwitchToModel(m_pickerModels[it->second]);
        }
    });

    // Show popup below the model pill
    wxPanel* pill = static_cast<wxPanel*>(m_modelLabel->GetParent());
    wxPoint pos = pill->GetScreenPosition();
    pos = anchor->ScreenToClient(pos);
    pos.y += pill->GetSize().y;
    anchor->PopupMenu(&menu, pos);
}

// ═════════════════════════════════════════════════════════════════
//  CORE SWITCH
// ═════════════════════════════════════════════════════════════════

void ModelSwitcher::SwitchToModel(const std::string& newModel)
{
    if (newModel == m_appState.GetModel()) return;
    if (m_cb.isBusy && m_cb.isBusy()) return;

    if (!m_chatHistory->IsEmpty() && m_cb.autoSave) {
        m_cb.autoSave();
    }

    bool mc, ac;
    m_appState.UpdateSettings(newModel, m_appState.GetApiUrl(), mc, ac);

    m_chatHistory->Clear();
    m_chatDisplay->Clear();
    m_attachments.Clear();
    UpdateModelLabel();
    if (m_cb.updateWindowTitle) m_cb.updateWindowTitle();

    m_serverReady = false;
    m_statusDot->SetConnected(false);
    m_chatDisplay->DisplaySystemMessage(
        "Loading " + ServerManager::ModelDisplayName(newModel) + "...");
    m_serverManager.StartServer(newModel, m_appState.MakeServerConfig());

    if (auto* logger = m_appState.GetLogger())
        logger->information("Quick-switched to model: " + newModel);
}

// ═════════════════════════════════════════════════════════════════
//  SHARED HELPER
// ═════════════════════════════════════════════════════════════════

void ModelSwitcher::UpdateModelLabel()
{
    std::string model = m_appState.GetModel();

    auto shortenModel = [](const std::string& m) -> std::string {
        if ((m.find('\\') != std::string::npos || m.find('/') != std::string::npos)
            || (m.size() > 5 && m.substr(m.size() - 5) == ".gguf")) {
            return ServerManager::ModelDisplayName(m);
        }
        size_t slash = m.rfind('/');
        if (slash != std::string::npos && slash + 1 < m.size())
            return m.substr(slash + 1);
        return m;
        };

    std::string display = shortenModel(model);

    wxString label = wxString::FromUTF8(display) +
        wxString::FromUTF8(" \xE2\x96\xBE"); // ▾

    m_modelLabel->SetLabel(label);

    // Full name stays available on hover even if the toolbar ellipsizes it.
    m_modelLabel->SetToolTip(wxString::FromUTF8(display));
    if (auto* parent = m_modelLabel->GetParent()) {
        parent->SetToolTip(wxString::FromUTF8(display));
        parent->Layout();

        if (auto* grandparent = parent->GetParent()) {
            grandparent->Layout();
        }
    }
}

// ═════════════════════════════════════════════════════════════════
//  FIRST-RUN ONBOARDING
// ═════════════════════════════════════════════════════════════════
//
// Two helpers used only from StartInitialServer's first-run branch.
// Kept here (rather than inline) so the branch reads as a sequence of
// high-level steps: "launch downloader → act on result."

std::string ModelSwitcher::LaunchFirstRunDownloader()
{
    if (!m_parentFrame) return "";

    // The dialog reorders Llama 3.2 3B to the top, renders a "Start here"
    // badge next to it, and auto-closes ~1s after a successful download.
    // ShowModal blocks until that happens (or the user dismisses).
    ModelDownloaderDialog dlg(m_parentFrame, &m_appState.GetTheme(),
                              /*firstRunMode=*/true);
    int result = dlg.ShowModal();

    // EndModal(wxID_OK) from the auto-close timer signals success.
    // Any other path (user clicked Close, pressed Escape, X-button)
    // returns wxID_CANCEL and we treat it as a dismissal. The explicit
    // path-nonempty check is belt-and-braces: if something upstream
    // ever returns wxID_OK without a completed download, we'd still
    // refuse to pretend onboarding succeeded.
    if (result == wxID_OK && !dlg.GetDownloadedModelPath().empty()) {
        if (auto* logger = m_appState.GetLogger())
            logger->information("First-run download succeeded: " +
                                dlg.GetDownloadedModelPath());
        return dlg.GetDownloadedModelPath();
    }

    if (auto* logger = m_appState.GetLogger())
        logger->information("First-run downloader dismissed without download");
    return "";
}

void ModelSwitcher::ShowFirstRunDismissedMessage()
{
    // Option B copy — the existing "No models found" body with one
    // added sentence pointing at the model pill. That pill is where
    // a dismissed user would otherwise have no idea to click, so
    // naming it explicitly gives them a direct way back to the
    // downloader without opening Settings.
    m_statusDot->SetConnected(false);
    m_chatDisplay->DisplaySystemMessage(
        "No model installed yet.\n\n"
        "LlamaBoss needs a local AI model before you can start chatting.\n"
        "You can return to the model downloader any time by clicking\n"
        "the model name at the top of the window, or by opening\n"
        "Settings \xe2\x86\x92 Download Models.\n\n"
        "Models are saved to:\n" + ServerManager::GetModelsDir());
}
