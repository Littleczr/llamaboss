// model_switcher.cpp
#include "model_switcher.h"
#include "app_state.h"
#include "server_manager.h"
#include "chat_display.h"
#include "chat_history.h"
#include "attachment_manager.h"
#include "widgets.h"
#include "model_manager.h"
#include "theme.h"

ModelSwitcher::ModelSwitcher(AppState& appState,
                             ServerManager& serverManager,
                             ChatDisplay* chatDisplay,
                             std::unique_ptr<ChatHistory>& chatHistory,
                             AttachmentManager& attachments,
                             StatusDot* statusDot,
                             wxStaticText* modelLabel)
    : m_appState(appState)
    , m_serverManager(serverManager)
    , m_chatDisplay(chatDisplay)
    , m_chatHistory(chatHistory)
    , m_attachments(attachments)
    , m_statusDot(statusDot)
    , m_modelLabel(modelLabel)
{
}

// ═════════════════════════════════════════════════════════════════
//  SERVER BOOTSTRAP
// ═════════════════════════════════════════════════════════════════

void ModelSwitcher::StartInitialServer()
{
    auto models = ServerManager::ScanModels();

    if (models.empty()) {
        m_statusDot->SetConnected(false);
        m_chatDisplay->DisplaySystemMessage(
            "No .gguf models found.\n\n"
            "To get started:\n"
            "1. Download a GGUF model (e.g. from huggingface.co)\n"
            "2. Place it in: " + ServerManager::GetModelsDir() + "\n"
            "   (Documents \\ LlamaBoss \\ models)\n"
            "3. Open Settings to select it\n\n"
            "The models folder has been created for you.");

        ServerManager::EnsureDataDirs();
        return;
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

    m_serverManager.StartServer(modelToLoad);
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

    auto models = ServerManager::ScanModels();
    if (models.empty()) {
        m_chatDisplay->DisplaySystemMessage(
            "No .gguf models found in " + ServerManager::GetModelsDir() +
            "\nPlace .gguf files there and try again.");
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
    m_serverManager.StartServer(newModel);

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

    m_modelLabel->SetLabel(wxString::FromUTF8(display) +
        wxString::FromUTF8(" \xE2\x96\xBE"));
    m_modelLabel->GetParent()->Layout();
}
