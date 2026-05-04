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

#include <algorithm>
#include <cctype>
#include <sstream>


namespace {

static bool StartsWith(const std::string& s, const std::string& prefix)
{
    return s.size() >= prefix.size() &&
           s.compare(0, prefix.size(), prefix) == 0;
}

static std::string TrimCopy(std::string s)
{
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

static std::string LowerCopy(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

static void AppendLine(std::string& dst, const std::string& line)
{
    dst += line;
    dst += '\n';
}

static bool IsFenceOpenLine(const std::string& line,
                            std::string& fence,
                            std::string& lang)
{
    size_t ticks = 0;
    while (ticks < line.size() && line[ticks] == '`') ++ticks;
    if (ticks < 3) return false;

    fence.assign(ticks, '`');
    lang = TrimCopy(line.substr(ticks));
    return true;
}

static std::vector<std::string> ParseStatusChips(const std::string& line)
{
    std::vector<std::string> chips;
    const std::string prefix = "[status:";
    if (!StartsWith(line, prefix)) return chips;

    size_t start = prefix.size();
    size_t end = line.rfind(']');
    if (end == std::string::npos || end < start) end = line.size();

    std::string body = TrimCopy(line.substr(start, end - start));
    std::stringstream ss(body);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item = TrimCopy(item);
        if (!item.empty()) chips.push_back(item);
    }
    return chips;
}

static std::string ToolNameFromTag(const std::string& tag)
{
    const std::string t = LowerCopy(tag);
    if (t == "powershell") return "PowerShell";
    if (t == "read")       return "Read";
    if (t == "grep")       return "Grep";
    if (t == "ls")         return "List";
    if (t == "open")       return "Open";
    if (t == "write")      return "Write";
    if (t == "edit")       return "Edit";
    if (t == "mkdir")      return "Mkdir";
    if (t == "delete")     return "Delete";

    if (tag.empty()) return "Tool";
    std::string out = tag;
    out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

static std::string ToolIconFromTag(const std::string& tag)
{
    const std::string t = LowerCopy(tag);
    if (t == "powershell") return "\xE2\x9A\x99";           // ⚙
    if (t == "read")       return "\xF0\x9F\x93\x84";       // 📄
    if (t == "grep")       return "\xF0\x9F\x94\x8D";       // 🔍
    if (t == "ls")         return "\xF0\x9F\x93\x81";       // 📁
    if (t == "open")       return "\xF0\x9F\x93\x82";       // 📂
    if (t == "write")      return "\xE2\x9C\x8D";           // ✍
    if (t == "edit")       return "\xE2\x9C\x8E";           // ✎
    if (t == "mkdir")      return "\xF0\x9F\x93\x81";       // 📁
    if (t == "delete")     return "\xF0\x9F\x97\x91";       // 🗑
    return "\xF0\x9F\x9B\xA0";                              // 🛠
}

static bool ParseSavedToolBlock(const std::string& content,
                                ChatDisplay::ToolBlock& out)
{
    if (!StartsWith(content, "[tool: ")) return false;

    std::istringstream input(content);
    std::string header;
    std::string echo;
    if (!std::getline(input, header)) return false;
    if (!std::getline(input, echo)) return false;
    if (!header.empty() && header.back() == '\r') header.pop_back();
    if (!echo.empty() && echo.back() == '\r') echo.pop_back();

    if (!StartsWith(header, "[tool: ") || header.back() != ']') return false;
    std::string tag = header.substr(7, header.size() - 8);

    out = ChatDisplay::ToolBlock{};
    out.iconUtf8 = ToolIconFromTag(tag);
    out.toolName = ToolNameFromTag(tag);
    out.bodyLang.clear();

    if (StartsWith(echo, "> ")) out.commandEcho = echo.substr(2);
    else                        out.commandEcho = echo;

    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (StartsWith(line, "[status:")) {
            out.statusChips = ParseStatusChips(line);
            continue;
        }

        if (line == "[error]") {
            std::string fenceLine, fence, lang;
            if (!std::getline(input, fenceLine)) break;
            if (!fenceLine.empty() && fenceLine.back() == '\r') fenceLine.pop_back();
            if (!IsFenceOpenLine(fenceLine, fence, lang)) continue;

            while (std::getline(input, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line == fence) break;
                AppendLine(out.errorBody, line);
            }
            continue;
        }

        std::string fence, lang;
        if (IsFenceOpenLine(line, fence, lang)) {
            if (out.bodyLang.empty()) out.bodyLang = lang;
            while (std::getline(input, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line == fence) break;
                AppendLine(out.body, line);
            }
        }
    }

    return true;
}

} // anonymous namespace

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
    if (!m_chatHistory->HasPersistableContent()) return;

    if (m_chatHistory->HasFilePath()) {
        ChatHistory::EnsureWorkflowDir(m_chatHistory->GetFilePath());
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
        ChatHistory::EnsureWorkflowDir(path);
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
    if (!m_chatHistory->HasPersistableContent()) return;

    if (!m_chatHistory->IsDirty() && m_chatHistory->HasFilePath()) return;

    if (!m_chatHistory->HasFilePath()) {
        m_chatHistory->SetFilePath(ChatHistory::GenerateFilePath());
    }

    // Keep a user-visible workflow folder beside each saved conversation.
    // Tool-created files and imported files for this chat live there.
    ChatHistory::EnsureWorkflowDir(m_chatHistory->GetFilePath());

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
    if (filePaths.size() == 1) {
        msg = "Delete this conversation? This cannot be undone.\n\n"
              "Any files in this conversation's workflow folder will also be deleted.";
    } else {
        msg = wxString::Format(
            "Delete %zu conversations? This cannot be undone.\n\n"
            "Any files in those conversations' workflow folders will also be deleted.",
            filePaths.size());
    }

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
                if (!wxFileName::Rmdir(attachDirWx, wxPATH_RMDIR_RECURSIVE)) {
                    if (auto* logger = m_appState.GetLogger())
                        logger->warning("Could not fully remove attachment dir: " +
                            std::string(attachDirWx.ToUTF8().data()));
                }
            }

            // Clean up sidecar generated-files directory (Phase 3)
            wxString filesDirWx = wxString::FromUTF8(
                ChatHistory::GetConversationsDir() + "/files/" + stem);
            if (wxDirExists(filesDirWx)) {
                wxLogNull suppressErrors;
                if (!wxFileName::Rmdir(filesDirWx, wxPATH_RMDIR_RECURSIVE)) {
                    if (auto* logger = m_appState.GetLogger())
                        logger->warning("Could not fully remove generated files dir: " +
                            std::string(filesDirWx.ToUTF8().data()));
                }
            }

            // Clean up the per-conversation workflow folder.  This is the
            // user-visible folder that contains Workspace, attachments,
            // artifacts, scripts, reports, PDFs, etc. for this chat.
            wxString workflowDirWx = wxString::FromUTF8(
                ChatHistory::GetWorkflowDir(filePath));
            if (wxDirExists(workflowDirWx)) {
                wxLogNull suppressErrors;
                if (!wxFileName::Rmdir(workflowDirWx, wxPATH_RMDIR_RECURSIVE)) {
                    if (auto* logger = m_appState.GetLogger())
                        logger->warning("Could not fully remove workflow dir: " +
                            std::string(workflowDirWx.ToUTF8().data()));
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

    // Save current conversation before loading.  Project attachments can
    // be set before the first chat message, so check persistable metadata
    // rather than messages-only emptiness.
    if (m_chatHistory->HasPersistableContent()) {
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
        m_serverManager.StartServer(primaryModel, m_appState.MakeServerConfig());
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
            ChatDisplay::ToolBlock savedToolBlock;
            if (ParseSavedToolBlock(content, savedToolBlock)) {
                // Saved tool results are stored in history as user messages
                // so the model can see them. On replay, render them using
                // the same native tool-card UI instead of showing the raw
                // [tool: ...] transcript block as a normal user message.
                m_chatDisplay->DisplayToolBlock(savedToolBlock, /*startExpanded=*/false);
                continue;
            }

            std::string target = ChatHistory::GetMessageTarget(msg);

            std::string displayContent = content;
            std::vector<std::string> imagePaths;

            if (msg->has("attachments")) {
                auto arr = msg->getArray("attachments");
                if (arr && arr->size() > 0) {
                    std::string prefix;
                    std::string convDir = ChatHistory::GetConversationsDir();
                    std::string workflowDir = ChatHistory::GetWorkflowDir(m_chatHistory->GetFilePath());

                    for (unsigned ai = 0; ai < arr->size(); ++ai) {
                        auto att = arr->getObject(ai);
                        std::string kind = att->getValue<std::string>("kind");
                        std::string fname = att->getValue<std::string>("filename");
                        if (kind == "image") {
                            if (!prefix.empty()) prefix += ", ";
                            prefix += "\xF0\x9F\x96\xBC " + fname;  // 🖼

                            if (att->has("storage_path")) {
                                std::string sp = att->getValue<std::string>("storage_path");
                                if (!sp.empty()) {
                                    std::string workflowPath = workflowDir + "/" + sp;
                                    if (wxFileExists(wxString::FromUTF8(workflowPath)))
                                        imagePaths.push_back(workflowPath);
                                    else
                                        imagePaths.push_back(convDir + "/" + sp); // legacy sidecar path
                                }
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

    if (m_chatHistory->HasProject()) {
        std::string projectName = m_chatHistory->GetProjectName();
        if (projectName.size() > 28) {
            projectName = projectName.substr(0, 25) + "...";
        }
        title = "[" + projectName + "] " + title;
    }

    m_frame.SetTitle(wxString::FromUTF8(title));

    // Strip refresh piggybacks on every existing call site.
    if (m_cb.onProjectStateChanged) m_cb.onProjectStateChanged();
}
