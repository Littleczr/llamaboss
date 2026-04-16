// conversation_controller.cpp
#include "conversation_controller.h"
#include "app_state.h"
#include "chat_history.h"
#include "chat_display.h"
#include "attachment_manager.h"
#include "conversation_sidebar.h"
#include "server_manager.h"
#include "model_switcher.h"
#include "widgets.h"
#include "theme.h"

#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/dir.h>

ConversationController::ConversationController(
    wxFrame& frame,
    AppState& appState,
    std::unique_ptr<ChatHistory>& chatHistory,
    ChatDisplay* chatDisplay,
    AttachmentManager& attachments,
    ConversationSidebar& sidebar,
    ServerManager& serverManager,
    ModelSwitcher& modelSwitcher,
    StatusDot* statusDot)
    : m_frame(frame)
    , m_appState(appState)
    , m_chatHistory(chatHistory)
    , m_chatDisplay(chatDisplay)
    , m_attachments(attachments)
    , m_sidebar(sidebar)
    , m_serverManager(serverManager)
    , m_modelSwitcher(modelSwitcher)
    , m_statusDot(statusDot)
{
}

// ═════════════════════════════════════════════════════════════════
//  SAVE
// ═════════════════════════════════════════════════════════════════

void ConversationController::OnSaveConversation()
{
    if (m_chatHistory->IsEmpty()) return;

    if (m_chatHistory->HasFilePath()) {
        if (m_chatHistory->SaveToFile("", m_appState.GetModel())) {
            m_chatDisplay->DisplaySystemMessage("Conversation saved.");
        }
    }
    else {
        wxString defaultDir = wxString::FromUTF8(ChatHistory::GetConversationsDir());
        wxString defaultName = wxString::FromUTF8(
            m_chatHistory->GenerateTitle() + ".json");

        // Clean filename — remove chars invalid on Windows
        defaultName.Replace("/", "_");
        defaultName.Replace("\\", "_");
        defaultName.Replace(":", "_");
        defaultName.Replace("?", "_");
        defaultName.Replace("\"", "_");
        defaultName.Replace("<", "_");
        defaultName.Replace(">", "_");
        defaultName.Replace("|", "_");

        wxFileDialog dlg(&m_frame, "Save Conversation", defaultDir, defaultName,
            "JSON files (*.json)|*.json",
            wxFD_SAVE | wxFD_OVERWRITE_PROMPT);

        if (dlg.ShowModal() == wxID_CANCEL) return;

        std::string path = dlg.GetPath().ToUTF8().data();
        if (m_chatHistory->SaveToFile(path, m_appState.GetModel())) {
            UpdateWindowTitle();
            m_chatDisplay->DisplaySystemMessage("Conversation saved.");
        }
        else {
            wxMessageBox("Failed to save conversation", "Error", wxOK | wxICON_ERROR);
        }
    }
}

// ═════════════════════════════════════════════════════════════════
//  LOAD (file dialog)
// ═════════════════════════════════════════════════════════════════

void ConversationController::OnLoadConversation()
{
    if (m_cb.isBusy && m_cb.isBusy()) return;

    wxString defaultDir = wxString::FromUTF8(ChatHistory::GetConversationsDir());

    wxFileDialog dlg(&m_frame, "Open Conversation", defaultDir, "",
        "JSON files (*.json)|*.json|All files (*.*)|*.*",
        wxFD_OPEN | wxFD_FILE_MUST_EXIST);

    if (dlg.ShowModal() == wxID_CANCEL) return;

    if (!LoadConversationFromPath(dlg.GetPath().ToUTF8().data())) {
        wxMessageBox("Failed to load conversation file", "Error", wxOK | wxICON_ERROR);
    }
}

// ═════════════════════════════════════════════════════════════════
//  AUTO-SAVE
// ═════════════════════════════════════════════════════════════════

void ConversationController::AutoSaveConversation(bool refreshSidebar)
{
    if (m_chatHistory->IsEmpty()) return;

    if (!m_chatHistory->IsDirty() && m_chatHistory->HasFilePath()) return;

    if (!m_chatHistory->HasFilePath()) {
        m_chatHistory->SetFilePath(ChatHistory::GenerateFilePath());
    }

    if (m_chatHistory->SaveToFile("", m_appState.GetModel())) {
        UpdateWindowTitle();
        if (refreshSidebar && m_sidebar.IsVisible())
            m_sidebar.Refresh(m_chatHistory->GetFilePath());
        if (auto* logger = m_appState.GetLogger())
            logger->debug("Auto-saved conversation: " + m_chatHistory->GetFilePath());
    }
}

// ═════════════════════════════════════════════════════════════════
//  DELETE
// ═════════════════════════════════════════════════════════════════

void ConversationController::DeleteConversations(
    const std::vector<std::string>& filePaths)
{
    if (filePaths.empty()) return;

    if (m_cb.isBusy && m_cb.isBusy()) {
        wxMessageBox(
            "Stop the current response before deleting conversations.",
            "Response in Progress",
            wxOK | wxICON_INFORMATION,
            &m_frame);
        return;
    }

    wxString msg;
    if (filePaths.size() == 1)
        msg = "Delete this conversation? This cannot be undone.";
    else
        msg = wxString::Format("Delete %zu conversations? This cannot be undone.",
                               filePaths.size());

    int result = wxMessageBox(msg, "Delete Conversation",
        wxYES_NO | wxICON_WARNING);
    if (result != wxYES) return;

    bool clearedActive = false;
    int deleted = 0;

    for (const auto& filePath : filePaths) {
        if (wxRemoveFile(wxString::FromUTF8(filePath))) {
            ++deleted;

            // Clean up sidecar attachment directory
            wxFileName convFn(wxString::FromUTF8(filePath));
            std::string stem(convFn.GetName().ToUTF8().data());
            wxString attachDirWx = wxString::FromUTF8(
                ChatHistory::GetConversationsDir() + "/attachments/" + stem);
            if (wxDirExists(attachDirWx)) {
                wxLogNull suppressErrors;
                if (!wxFileName::Rmdir(attachDirWx, wxPATH_RMDIR_FULL)) {
                    if (auto* logger = m_appState.GetLogger())
                        logger->warning("Could not fully remove attachment dir: " +
                            std::string(attachDirWx.ToUTF8().data()));
                }
            }

            // If deleting the currently active conversation, clear the display
            if (!clearedActive && filePath == m_chatHistory->GetFilePath()) {
                m_chatHistory->Clear();
                m_chatDisplay->Clear();
                m_attachments.Clear();
                UpdateWindowTitle();
                clearedActive = true;
            }

            if (auto* logger = m_appState.GetLogger())
                logger->information("Deleted conversation: " + filePath);
        }
    }

    if (deleted > 0) {
        m_sidebar.ClearSelection();
        if (m_sidebar.IsVisible())
            m_sidebar.Refresh(m_chatHistory->GetFilePath());
    }

    if (deleted < (int)filePaths.size()) {
        wxMessageBox(
            wxString::Format("Failed to delete %d of %zu files.",
                             (int)filePaths.size() - deleted,
                             filePaths.size()),
            "Error", wxOK | wxICON_ERROR);
    }
}

// ═════════════════════════════════════════════════════════════════
//  LOAD FROM PATH
// ═════════════════════════════════════════════════════════════════

bool ConversationController::LoadConversationFromPath(const std::string& path)
{
    if (m_cb.isBusy && m_cb.isBusy()) return false;

    // Save current conversation before loading
    if (!m_chatHistory->IsEmpty()) {
        AutoSaveConversation(false);
    }

    std::vector<std::string> loadedModels;
    auto newHistory = std::make_unique<ChatHistory>();
    if (!newHistory->LoadFromFile(path, loadedModels)) {
        return false;
    }

    // Replace current history (through the unique_ptr reference)
    m_chatHistory = std::move(newHistory);

    // Restore model(s) from the loaded conversation
    std::string primaryModel = loadedModels.empty() ? "" : loadedModels.front();
    bool needsServerRestart = false;

    if (!primaryModel.empty() && primaryModel != m_appState.GetModel()) {
        if (wxFileExists(primaryModel)) {
            bool mc, ac;
            m_appState.UpdateSettings(primaryModel, m_appState.GetApiUrl(), mc, ac);
            needsServerRestart = true;
        }
        else {
            if (auto* logger = m_appState.GetLogger())
                logger->warning("Conversation model not found: " + primaryModel +
                    " — keeping current model");
        }
    }

    m_modelSwitcher.UpdateModelLabel();

    // Replay to display
    m_chatDisplay->Clear();
    m_attachments.Clear();
    ReplayConversation();
    UpdateWindowTitle();
    if (m_sidebar.IsVisible())
        m_sidebar.Refresh(m_chatHistory->GetFilePath());

    // Restart server if the conversation uses a different model
    if (needsServerRestart) {
        m_modelSwitcher.m_serverReady = false;
        m_statusDot->SetConnected(false);
        m_chatDisplay->DisplaySystemMessage(
            "Loading " + ServerManager::ModelDisplayName(primaryModel) + "...");
        m_serverManager.StartServer(primaryModel);
    }

    if (auto* logger = m_appState.GetLogger())
        logger->information("Loaded conversation: " + m_chatHistory->GetTitle());

    return true;
}

// ═════════════════════════════════════════════════════════════════
//  REPLAY
// ═════════════════════════════════════════════════════════════════

void ConversationController::ReplayConversation()
{
    const auto& messages = m_chatHistory->GetMessages();
    for (const auto& msg : messages) {
        std::string role = msg->getValue<std::string>("role");
        std::string content = msg->getValue<std::string>("content");

        if (content.empty()) continue;

        if (role == "user") {
            std::string target = ChatHistory::GetMessageTarget(msg);

            std::string displayContent = content;
            std::vector<std::string> imagePaths;

            if (msg->has("attachments")) {
                auto arr = msg->getArray("attachments");
                if (arr && arr->size() > 0) {
                    std::string prefix;
                    std::string convDir = ChatHistory::GetConversationsDir();

                    for (unsigned ai = 0; ai < arr->size(); ++ai) {
                        auto att = arr->getObject(ai);
                        std::string kind = att->getValue<std::string>("kind");
                        std::string fname = att->getValue<std::string>("filename");
                        if (kind == "image") {
                            if (!prefix.empty()) prefix += ", ";
                            prefix += "\xF0\x9F\x96\xBC " + fname;  // 🖼

                            if (att->has("storage_path")) {
                                std::string sp = att->getValue<std::string>("storage_path");
                                if (!sp.empty())
                                    imagePaths.push_back(convDir + "/" + sp);
                            }
                        }
                        else if (kind == "text_file") {
                            if (!prefix.empty()) prefix += ", ";
                            prefix += "\xF0\x9F\x93\x84 " + fname;  // 📄
                        }
                    }
                    if (!prefix.empty())
                        displayContent = "[" + prefix + "] " + content;
                }
            }

            m_chatDisplay->DisplayUserMessage(displayContent, target, imagePaths);
        }
        else if (role == "assistant") {
            std::string msgModel = ChatHistory::GetMessageModel(msg);
            if (msgModel.empty()) msgModel = m_appState.GetModel();
            m_chatDisplay->DisplayAssistantMessage(
                ServerManager::ModelDisplayName(msgModel),
                content,
                m_appState.GetTheme().chatAssistant
            );
        }
        else if (role == "system") {
            m_chatDisplay->DisplaySystemMessage(content);
        }
    }
}

// ═════════════════════════════════════════════════════════════════
//  WINDOW TITLE
// ═════════════════════════════════════════════════════════════════

void ConversationController::UpdateWindowTitle()
{
    std::string title = "LlamaBoss";
    if (!m_chatHistory->IsEmpty()) {
        std::string convTitle = m_chatHistory->GetTitle();
        if (convTitle.empty()) {
            convTitle = m_chatHistory->GenerateTitle();
        }
        if (!convTitle.empty() && convTitle != "Untitled conversation") {
            if (convTitle.size() > 40) {
                convTitle = convTitle.substr(0, 37) + "...";
            }
            title = convTitle + " - LlamaBoss";
        }
    }
    m_frame.SetTitle(wxString::FromUTF8(title));
}
