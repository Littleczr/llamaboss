// conversation_controller.cpp
#include "conversation_controller.h"

#include "app.h"
#include "app_state.h"
#include "chat_history.h"
#include "chat_display.h"
#include "attachment_manager.h"
#include "conversation_sidebar.h"
#include "server_manager.h"
#include "model_service.h"
#include "model_switcher.h"
#include "widgets.h"
#include "theme.h"
#include "ui_event_post.h"
#include "lb_string_utils.h"   // LbUtf8SafeTruncate
#include "export_metrics.h"    // "Export with metrics..." section
#include "path_safety.h"       // Utf8ToWide for the chat-folder logs

#include <wx/dirdlg.h>
#include <wx/msgdlg.h>
#include <wx/file.h>
#include <wx/datetime.h>

#include <exception>
#include <condition_variable>
#include <deque>

#include <Poco/Types.h>

wxDEFINE_EVENT(wxEVT_LB_CONVERSATION_SAVE_COMPLETE, wxCommandEvent);

namespace {

class ConversationSaveResultData : public wxClientData
{
public:
    ConversationSaveResultData(ChatHistory::SaveSnapshot snapshot,
                               bool success,
                               bool refreshSidebar)
        : m_snapshot(std::move(snapshot))
        , m_success(success)
        , m_refreshSidebar(refreshSidebar)
    {}

    const ChatHistory::SaveSnapshot& Snapshot() const { return m_snapshot; }
    bool Success() const { return m_success; }
    bool RefreshSidebar() const { return m_refreshSidebar; }

private:
    ChatHistory::SaveSnapshot m_snapshot;
    bool m_success = false;
    bool m_refreshSidebar = false;
};


class ChatReplayBatchGuard
{
public:
    explicit ChatReplayBatchGuard(ChatDisplay* display)
        : m_display(display)
    {
        if (m_display) m_display->BeginReplayBatch();
    }

    ~ChatReplayBatchGuard()
    {
        if (m_display) m_display->EndReplayBatch();
    }

    ChatReplayBatchGuard(const ChatReplayBatchGuard&) = delete;
    ChatReplayBatchGuard& operator=(const ChatReplayBatchGuard&) = delete;

private:
    ChatDisplay* m_display = nullptr;
};

static bool StartsWith(const std::string& s, const std::string& prefix)
{
    return s.size() >= prefix.size() &&
           s.compare(0, prefix.size(), prefix) == 0;
}

static bool IsHiddenGoalReplaySystemMessage(const std::string& content)
{
    // Legacy: the retired Goals feature persisted its continuation
    // instructions as system messages.  They are internal orchestration
    // prompts, so keep hiding them when older saved conversations replay.
    return StartsWith(content, "Goal continuation instruction:");
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
    if (t == "pdf_extract_text")  return "PDF Extract Text";
    if (t == "pdf_inspect_form")  return "PDF Inspect Form";
    if (t == "pdf_fill_form")     return "PDF Fill Form";
    if (t == "docx_extract_text") return "DOCX Extract Text";
    if (t == "docx_inspect")      return "DOCX Inspect";
    if (t == "python_run_script")      return "Python Run";
    if (t == "python_install_package") return "Python Install Package";
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
    if (t == "pdf_extract_text")  return "\xF0\x9F\x93\x84"; // 📄
    if (t == "pdf_inspect_form")  return "\xF0\x9F\x93\x8B"; // 📋
    if (t == "pdf_fill_form")     return "\xF0\x9F\x96\x8A"; // 🖊
    if (t == "docx_extract_text") return "\xF0\x9F\x93\x84"; // 📄
    if (t == "docx_inspect")      return "\xF0\x9F\x93\x8B"; // 📋
    if (t == "python_run_script")      return "\xF0\x9F\x90\x8D"; // 🐍
    if (t == "python_install_package") return "\xF0\x9F\x90\x8D"; // 🐍
    if (t == "ls")         return "\xF0\x9F\x93\x81";       // 📁
    if (t == "open")       return "\xF0\x9F\x93\x82";       // 📂
    if (t == "write")      return "\xE2\x9C\x8D";           // ✍
    if (t == "edit")       return "\xE2\x9C\x8E";           // ✎
    if (t == "mkdir")      return "\xF0\x9F\x93\x81";       // 📁
    if (t == "delete")     return "\xF0\x9F\x97\x91";       // 🗑
    return "\xF0\x9F\x9B\xA0";                              // 🛠
}


static bool ParsePersistedArtifactLine(const std::string& line,
                                       PresentedFile& out)
{
    try {
        Poco::JSON::Parser parser;
        auto parsed = parser.parse(line);
        auto obj = parsed.extract<Poco::JSON::Object::Ptr>();
        if (!obj) return false;

        PresentedFile file;
        if (obj->has("display_name"))
            file.displayName = obj->getValue<std::string>("display_name");
        if (obj->has("language"))
            file.language = obj->getValue<std::string>("language");
        if (obj->has("disk_path"))
            file.diskPath = obj->getValue<std::string>("disk_path");
        if (obj->has("size_bytes"))
            file.sizeBytes = static_cast<std::size_t>(
                obj->getValue<Poco::UInt64>("size_bytes"));
        if (obj->has("line_count"))
            file.lineCount = obj->getValue<int>("line_count");

        if (file.diskPath.empty()) return false;
        if (file.displayName.empty()) {
            size_t slash = file.diskPath.find_last_of("\\/");
            file.displayName = (slash == std::string::npos)
                ? file.diskPath
                : file.diskPath.substr(slash + 1);
        }

        out = std::move(file);
        return true;
    } catch (...) {
        return false;
    }
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

    bool inArtifacts = false;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (StartsWith(line, "[status:")) {
            out.statusChips = ParseStatusChips(line);
            inArtifacts = false;
            continue;
        }

        if (line == "[artifacts]") {
            inArtifacts = true;
            continue;
        }

        if (inArtifacts) {
            const std::string artifactLine = TrimCopy(line);
            if (!artifactLine.empty()) {
                PresentedFile file;
                if (ParsePersistedArtifactLine(artifactLine, file))
                    out.presentedFiles.push_back(std::move(file));
            }
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

class ConversationController::AsyncSaveState
{
public:
    AsyncSaveState(wxEvtHandler* target,
                   std::weak_ptr<std::atomic<bool>> aliveToken)
        : m_target(target)
        , m_aliveToken(std::move(aliveToken))
        , m_thread(&AsyncSaveState::Run, this)
    {}

    ~AsyncSaveState()
    {
        WaitIdle();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
        }
        m_cv.notify_all();
        if (m_thread.joinable()) m_thread.join();
    }

    void Queue(ChatHistory::SaveSnapshot snapshot, bool refreshSidebar)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            // Coalesce the newest not-yet-started snapshot for this path, but
            // never discard a queued save for a different conversation.  Most
            // path changes drain the worker first; the deque is a correctness
            // backstop for metadata-only clears or future call sites.
            auto match = m_pending.rend();
            for (auto it = m_pending.rbegin(); it != m_pending.rend(); ++it) {
                if (it->snapshot.filePath == snapshot.filePath) {
                    match = it;
                    break;
                }
            }

            if (match != m_pending.rend()) {
                match->refreshSidebar = match->refreshSidebar || refreshSidebar;
                if (snapshot.revision >= match->snapshot.revision)
                    match->snapshot = std::move(snapshot);
            } else {
                Job job;
                job.snapshot = std::move(snapshot);
                job.refreshSidebar = refreshSidebar;
                m_pending.push_back(std::move(job));
            }
        }
        m_cv.notify_one();
    }

    void WaitIdle()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_idleCv.wait(lock, [this] {
            return !m_working && m_pending.empty();
        });
    }

private:
    struct Job {
        ChatHistory::SaveSnapshot snapshot;
        bool refreshSidebar = false;
    };

    void Run()
    {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this] {
                    return m_stopping || !m_pending.empty();
                });
                if (m_stopping && m_pending.empty()) break;

                job = std::move(m_pending.front());
                m_pending.pop_front();
                m_working = true;
            }

            bool ok = false;
            try {
                ok = ChatHistory::WriteSaveSnapshot(
                    job.snapshot, /*durable=*/false);

                if (m_target) {
                    auto* event = new wxCommandEvent(
                        wxEVT_LB_CONVERSATION_SAVE_COMPLETE);
                    event->SetClientObject(new ConversationSaveResultData(
                        std::move(job.snapshot), ok, job.refreshSidebar));
                    LbQueueEventIfAlive(m_target, m_aliveToken, event);
                }
            }
            catch (...) {
                // WriteSaveSnapshot is already fail-closed, but keep the
                // worker alive and release WaitIdle even if event allocation
                // or a future completion-path change throws unexpectedly.
            }

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_working = false;
            }
            m_idleCv.notify_all();
        }
    }

    wxEvtHandler* m_target = nullptr;
    std::weak_ptr<std::atomic<bool>> m_aliveToken;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::condition_variable m_idleCv;
    std::deque<Job> m_pending;
    bool m_working = false;
    bool m_stopping = false;
    std::thread m_thread;
};

ConversationController::ConversationController(
    wxFrame& frame,
    AppState& appState,
    std::unique_ptr<ChatHistory>& chatHistory,
    ChatDisplay* chatDisplay,
    AttachmentManager& attachments,
    ConversationSidebar& sidebar,
    ServerManager& serverManager,
    ModelSwitcher& modelSwitcher,
    StatusDot* statusDot,
    std::weak_ptr<std::atomic<bool>> aliveToken)
    : m_frame(frame)
    , m_appState(appState)
    , m_chatHistory(chatHistory)
    , m_chatDisplay(chatDisplay)
    , m_attachments(attachments)
    , m_sidebar(sidebar)
    , m_serverManager(serverManager)
    , m_modelSwitcher(modelSwitcher)
    , m_statusDot(statusDot)
    , m_asyncSave(std::make_unique<AsyncSaveState>(
          &m_frame, std::move(aliveToken)))
{
    m_frame.Bind(wxEVT_LB_CONVERSATION_SAVE_COMPLETE,
                 &ConversationController::OnAsyncSaveComplete,
                 this);
}

ConversationController::~ConversationController()
{
    WaitForPendingSaves();
    m_asyncSave.reset();
    m_frame.Unbind(wxEVT_LB_CONVERSATION_SAVE_COMPLETE,
                   &ConversationController::OnAsyncSaveComplete,
                   this);
}

void ConversationController::WaitForPendingSaves()
{
    if (m_asyncSave) m_asyncSave->WaitIdle();
}

void ConversationController::OnAsyncSaveComplete(wxCommandEvent& evt)
{
    // Payload ownership.  wxCommandEvent does not delete its client
    // object (see the contract on CmdResultClientData in cmd_executor.h),
    // so this handler must.  This is the costliest payload in the app:
    // ConversationSaveResultData holds an entire SaveSnapshot -- every
    // message's content plus its tool_calls / images / attachments JSON --
    // and autosave fires after every message AND every agent tool step.
    // Leaking it grows roughly with (conversation size x step count).
    //
    // payloadOwner must outlive `snapshot`, which is a reference into it
    // and is used all the way to the end of the function.
    std::unique_ptr<wxClientData> payloadOwner(evt.GetClientObject());
    evt.SetClientObject(nullptr);

    auto* data = static_cast<ConversationSaveResultData*>(payloadOwner.get());
    if (!data) return;

    const ChatHistory::SaveSnapshot& snapshot = data->Snapshot();
    if (!data->Success()) {
        if (auto* logger = m_appState.GetLogger())
            logger->warning("Background autosave failed: " + snapshot.filePath);
        return;
    }

    // Commit only when this frame still owns the same conversation and no
    // newer mutation superseded the snapshot.  Avoid refreshing the title or
    // sidebar for an older revision while a newer coalesced write is still in
    // flight; that would briefly repaint metadata from stale on-disk JSON.
    const bool isCurrent =
        snapshot.filePath == m_chatHistory->GetFilePath() &&
        snapshot.revision == m_chatHistory->GetRevision();
    m_chatHistory->CommitSaveSnapshot(snapshot);

    if (isCurrent) {
        wxGetApp().GetConversationRegistry().SetCurrent(
            &m_frame, snapshot.filePath);
        UpdateWindowTitle();
        if (data->RefreshSidebar() && m_sidebar.IsVisible())
            m_sidebar.Refresh(snapshot.filePath);
    }

    if (auto* logger = m_appState.GetLogger()) {
        logger->debug(std::string(isCurrent ? "Auto-saved conversation: "
                                            : "Completed stale autosave: ") +
                      snapshot.filePath);
    }
}

// ═════════════════════════════════════════════════════════════════
//  SAVE
// ═════════════════════════════════════════════════════════════════

void ConversationController::OnSaveConversation()
{
    if (!m_chatHistory->HasPersistableContent()) return;

    // A manual durable save/Save-As must not race an older background
    // autosave that could otherwise rename its stale snapshot afterward.
    WaitForPendingSaves();

    if (m_chatHistory->HasFilePath()) {
        ChatHistory::EnsureChatFolder(m_chatHistory->GetFilePath(),
                                      m_chatHistory->GetChatFolderTitle());
        m_chatHistory->SetModelSelection(m_modelSwitcher.GetConversationSelectionKeyForSave());
        if (m_chatHistory->SaveToFile("", m_modelSwitcher.GetConversationModelForSave())) {
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
        defaultName.Replace("*", "_");
        defaultName.Replace("\"", "_");
        defaultName.Replace("<", "_");
        defaultName.Replace(">", "_");
        defaultName.Replace("|", "_");

        wxFileDialog dlg(&m_frame, "Save Conversation", defaultDir, defaultName,
            "JSON files (*.json)|*.json",
            wxFD_SAVE | wxFD_OVERWRITE_PROMPT);

        if (dlg.ShowModal() == wxID_CANCEL) return;

        std::string path = dlg.GetPath().ToUTF8().data();

        // ── Cross-window ownership guard ──────────────────────────
        // Save-As onto a conversation another window has open would
        // silently clobber that window's file — the overwrite prompt
        // above only checks the disk, not window ownership, and the
        // other window's next autosave would clobber ours right back.
        // Same policy as LoadConversationFromPath: raise the owner
        // and refuse.
        if (wxFrame* owner = wxGetApp().GetConversationRegistry()
                                 .OwnerOf(path, &m_frame)) {
            owner->Raise();
            owner->RequestUserAttention(wxUSER_ATTENTION_INFO);
            wxMessageBox(
                "That conversation is open in another window.\n"
                "Close it there first, or choose a different name.",
                "Conversation In Use", wxOK | wxICON_INFORMATION, &m_frame);
            return;
        }

        ChatHistory::EnsureChatFolder(path, m_chatHistory->GetChatFolderTitle());
        m_chatHistory->SetModelSelection(m_modelSwitcher.GetConversationSelectionKeyForSave());
        if (m_chatHistory->SaveToFile(path, m_modelSwitcher.GetConversationModelForSave())) {
            // Save-As just changed this window's current path — refresh
            // the registry claim so no other window can open the new
            // path underneath us.  SetCurrent is one-claim-per-frame,
            // so this also releases the stale claim on the old path
            // (the registry header lists Save-As as a claim point).
            wxGetApp().GetConversationRegistry().SetCurrent(
                &m_frame, m_chatHistory->GetFilePath());
            // Session trust follows the conversation to its new path.
            // (The old path keeps its session grant too; both names
            // refer to content the user trusted this session.)
            if (m_chatHistory->IsChatApprovalTrustEnabled()) {
                wxGetApp().GetConversationRegistry().RememberSessionTrust(
                    m_chatHistory->GetFilePath());
            }
            for (const std::string& root :
                 m_chatHistory->GetChatWriteRoots()) {
                wxGetApp().GetConversationRegistry()
                    .RememberSessionWriteRoot(
                        m_chatHistory->GetFilePath(), root);
            }
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

    LoadConversationFromPath(dlg.GetPath().ToUTF8().data());
}

// ═════════════════════════════════════════════════════════════════
//  AUTO-SAVE
// ═════════════════════════════════════════════════════════════════

bool ConversationController::AutoSaveConversation(bool refreshSidebar,
                                                    bool durable,
                                                    bool touchActivityTimestamp)
{
    if (durable && m_cb.beforeDurableSave) m_cb.beforeDurableSave();
    if (!m_chatHistory->HasPersistableContent()) return true;

    // A destructive transition must not leave an older background writer in
    // flight, even when an earlier same-revision completion has already
    // cleared the dirty flag.  Drain before the clean fast-path so New Chat,
    // conversation switches, model resets, and close all establish a hard
    // write-order boundary.
    if (durable)
        WaitForPendingSaves();

    // A background completion only proves a non-durable write succeeded.
    // Flush again before destroying the in-memory copy, even if it is clean.
    if (!durable && !m_chatHistory->IsDirty() && m_chatHistory->HasFilePath()) return true;

    if (!m_chatHistory->HasFilePath())
        m_chatHistory->SetFilePath(ChatHistory::GenerateFilePath());

    const std::string savePath = m_chatHistory->GetFilePath();
    ChatHistory::EnsureChatFolder(savePath, m_chatHistory->GetChatFolderTitle());

    // Session trust: an unsaved chat can be granted one-approval mode
    // before it has a file path (HandleApprovalCommand ignores empty
    // paths).  Now that a path exists, record the grant so trust
    // survives a later reload within this app session.
    if (m_chatHistory->IsChatApprovalTrustEnabled()) {
        wxGetApp().GetConversationRegistry().RememberSessionTrust(savePath);
    }
    for (const std::string& root : m_chatHistory->GetChatWriteRoots()) {
        wxGetApp().GetConversationRegistry()
            .RememberSessionWriteRoot(savePath, root);
    }

    const std::vector<std::string> models{
        m_modelSwitcher.GetConversationModelForSave()
    };
    // Persist the exact connection with the model id (see
    // ChatHistory::GetModelSelection).
    m_chatHistory->SetModelSelection(m_modelSwitcher.GetConversationSelectionKeyForSave());

    if (durable) {
        // Earlier queued snapshots were drained above before the clean
        // fast-path.  The final write is synchronous and durable because the
        // in-memory history may be cleared or replaced immediately afterward.
        if (m_chatHistory->SaveToFile("", models, /*durable=*/true,
                                      touchActivityTimestamp)) {
            wxGetApp().GetConversationRegistry().SetCurrent(&m_frame, savePath);
            UpdateWindowTitle();
            if (refreshSidebar && m_sidebar.IsVisible())
                m_sidebar.Refresh(savePath);
            if (auto* logger = m_appState.GetLogger())
                logger->debug("Durably saved conversation: " + savePath);
            return true;
        }
        if (auto* logger = m_appState.GetLogger())
            logger->warning("Durable conversation save failed: " + savePath);
        return false;
    }

    ChatHistory::SaveSnapshot snapshot;
    if (!m_chatHistory->CreateSaveSnapshot("", models, snapshot,
                                             touchActivityTimestamp)) return false;

    // Claim/update immediately: the in-memory conversation already owns this
    // generated path, while disk construction proceeds in the worker.
    wxGetApp().GetConversationRegistry().SetCurrent(&m_frame, savePath);
    UpdateWindowTitle();

    if (m_asyncSave) {
        m_asyncSave->Queue(std::move(snapshot), refreshSidebar);
        return true;
    }
    return false;
}
// Recovery before a destructive transition.
bool ConversationController::SaveBeforeLeaving(bool allowRecoveryDialog)
{
    // Recovery dialogs pump events. Prevent nested close/load actions from
    // clearing history underneath a recovery attempt.
    if (m_saveRecoveryActive) return false;
    struct RecoveryScope {
        bool& active;
        explicit RecoveryScope(bool& value) : active(value) { active = true; }
        ~RecoveryScope() { active = false; }
    } scope(m_saveRecoveryActive);

    for (;;) {
        if (AutoSaveConversation(false, /*durable=*/true)) return true;
        if (!allowRecoveryDialog) return false;

        wxMessageDialog prompt(&m_frame,
            "The conversation could not be saved. It is still open in memory.\n\n"
            "Retry after fixing the disk or file access problem, or save a recovery "
            "copy in another folder. Save Elsewhere keeps the original filename "
            "so the copy can still find this conversation's existing workspace and attachments.",
            "Conversation Not Saved", wxYES_NO | wxCANCEL | wxICON_WARNING);
        prompt.SetYesNoCancelLabels("Retry", "Save Elsewhere...", "Keep Chat Open");
        const int answer = prompt.ShowModal();
        if (answer == wxID_YES) continue;
        if (answer != wxID_NO) return false;

        wxDirDialog folder(&m_frame, "Choose a folder for the recovery conversation",
                           wxEmptyString, wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
        if (folder.ShowModal() != wxID_OK) return false;

        const wxFileName original(wxString::FromUTF8(m_chatHistory->GetFilePath()));
        const wxString destination = wxFileName(folder.GetPath(), original.GetFullName()).GetFullPath();
        const std::string path(destination.ToUTF8().data());
        if (wxFrame* owner = wxGetApp().GetConversationRegistry().OwnerOf(path, &m_frame)) {
            owner->RequestUserAttention(wxUSER_ATTENTION_INFO);
            wxMessageBox("That conversation is open in another window. Choose another folder.",
                         "Conversation In Use", wxOK | wxICON_WARNING, &m_frame);
            continue;
        }
        if (wxFileExists(destination) && wxMessageBox(
                "Replace the existing conversation in this folder?",
                "Replace Conversation", wxYES_NO | wxNO_DEFAULT | wxICON_WARNING,
                &m_frame) != wxYES) return false;

        // Dialogs may have delivered more stream/tool results. Drain old
        // writes and take a fresh snapshot. This recovery export deliberately
        // preserves the live conversation's identity and dirty state.
        WaitForPendingSaves();
        if (m_cb.beforeDurableSave) m_cb.beforeDurableSave();
        m_chatHistory->SetModelSelection(m_modelSwitcher.GetConversationSelectionKeyForSave());
        ChatHistory::SaveSnapshot snapshot;
        if (m_chatHistory->CreateSaveSnapshot("",
                std::vector<std::string>{m_modelSwitcher.GetConversationModelForSave()}, snapshot)) {
            snapshot.filePath = path;
            snapshot.titleMarkerPath.clear();
            if (ChatHistory::WriteSaveSnapshot(snapshot, /*durable=*/true)) return true;
        }
        wxMessageBox("The recovery copy could not be saved. Your conversation is still open.",
                     "Save Failed", wxOK | wxICON_ERROR, &m_frame);
    }
}

// ═════════════════════════════════════════════════════════════════
//  SIDEBAR MANAGEMENT: rename / pin / archive
// ═════════════════════════════════════════════════════════════════

// ═════════════════════════════════════════════════════════════════
//  EXPORT (Markdown transcript)
// ═════════════════════════════════════════════════════════════════

namespace {

// Remove <think>…</think> reasoning (also an unterminated open tag, and
// the "orphan close" shape where the template prefilled <think>).
std::string ExportStripThinking(std::string s)
{
    static const std::string kOpen = "<think>", kClose = "</think>";
    const size_t firstOpen = s.find(kOpen);
    const size_t firstClose = s.find(kClose);
    if (firstClose != std::string::npos &&
        (firstOpen == std::string::npos || firstClose < firstOpen)) {
        s.erase(0, firstClose + kClose.size());
    }
    for (;;) {
        const size_t open = s.find(kOpen);
        if (open == std::string::npos) break;
        const size_t close = s.find(kClose, open + kOpen.size());
        if (close == std::string::npos) { s.erase(open); break; }
        s.erase(open, close + kClose.size() - open);
    }
    return TrimCopy(s);
}

// A fence longer than any backtick run inside the body, so tool output
// that itself contains ``` cannot break out of its code block.
std::string ExportFenceFor(const std::string& body)
{
    size_t longest = 0, run = 0;
    for (char c : body) {
        run = (c == '`') ? run + 1 : 0;
        longest = std::max(longest, run);
    }
    return std::string(std::max<size_t>(3, longest + 1), '`');
}

// Inline code span that survives backticks inside the text.
std::string ExportInlineCode(const std::string& text)
{
    size_t longest = 0, run = 0;
    for (char c : text) {
        run = (c == '`') ? run + 1 : 0;
        longest = std::max(longest, run);
    }
    const std::string ticks(longest + 1, '`');
    const bool pad = !text.empty() && (text.front() == '`' || text.back() == '`');
    return ticks + (pad ? " " : "") + text + (pad ? " " : "") + ticks;
}

size_t ExportCountLines(const std::string& s)
{
    if (s.empty()) return 0;
    size_t n = static_cast<size_t>(std::count(s.begin(), s.end(), '\n'));
    return (s.back() == '\n') ? n : n + 1;
}

// Long tool output (file reads, directory listings) would dominate the
// transcript.  Keep the first part and say how much was left out; the
// full text stays in the conversation JSON.
constexpr size_t kExportMaxToolLines = 400;

void ExportAppendCollapsible(std::string& md, const std::string& summary,
                             const std::string& body, const std::string& lang)
{
    std::string text = body;
    const size_t total = ExportCountLines(text);
    std::string note;
    if (total > kExportMaxToolLines) {
        size_t pos = 0;
        for (size_t i = 0; i < kExportMaxToolLines && pos != std::string::npos; ++i) {
            pos = text.find('\n', pos);
            if (pos != std::string::npos) ++pos;
        }
        if (pos != std::string::npos) text.erase(pos);
        note = "\n_\xE2\x80\xA6 " + std::to_string(total - kExportMaxToolLines) +
               " more lines not included in this export._\n";
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();

    const std::string fence = ExportFenceFor(text);
    md += "<details><summary>" + summary + " (" + std::to_string(total) +
          (total == 1 ? " line" : " lines") + ")</summary>\n\n";
    md += fence + lang + "\n" + text + "\n" + fence + "\n" + note;
    md += "\n</details>\n\n";
}

// "notes_read" -> "Notes Read".  Known tools already arrive pretty
// ("PowerShell"); the generic fallback only capitalises the first letter.
std::string ExportToolName(std::string name)
{
    bool startWord = true;
    for (char& c : name) {
        if (c == '_') { c = ' '; startWord = true; continue; }
        if (startWord)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        startWord = (c == ' ');
    }
    return name;
}

// LlamaBoss appends a "[workspace changes]" note to tool output for the
// model's benefit.  It is noise in a human transcript.
std::string ExportStripToolFooter(std::string body)
{
    const size_t pos = body.rfind("\n[workspace changes]");
    if (pos != std::string::npos)
        body.erase(pos);
    else if (body.rfind("[workspace changes]", 0) == 0)
        body.clear();
    return body;
}

std::string ExportSafeFileName(std::string name)
{
    static const std::string kBad = "<>:\"/\\|?*";
    for (char& c : name) {
        if (static_cast<unsigned char>(c) < 32 || kBad.find(c) != std::string::npos)
            c = '_';
    }
    name = TrimCopy(name);
    while (!name.empty() && (name.back() == '.' || name.back() == ' '))
        name.pop_back();
    if (name.size() > 80) name = LbUtf8SafeTruncate(name, 80);
    return name.empty() ? std::string("conversation") : name;
}

std::string BuildConversationMarkdown(const ChatHistory& history,
                                      const std::string& fallbackModel)
{
    std::string title = history.GetTitle();
    if (title.empty()) title = history.GenerateTitle();
    if (title.empty()) title = "Conversation";

    std::string md;
    md += "# " + title + "\n\n";
    if (!fallbackModel.empty())
        md += "- **Model:** " + ServerManager::ModelDisplayName(fallbackModel) + "\n";
    if (!history.GetCreatedAt().empty())
        md += "- **Created:** " + history.GetCreatedAt() + "\n";
    md += "- **Exported:** " +
          std::string(wxDateTime::Now().Format("%Y-%m-%d %H:%M").ToUTF8().data()) +
          " from LlamaBoss\n";

    // Speaker headings.  Native tool-call turns store an assistant message
    // with empty content plus a tool_calls sidecar, then the results as
    // user-role messages; without tracking, every tool result would land
    // under "## You".  Tool results and the final answer are grouped under
    // the model that issued the calls, with one heading per run.
    std::string lastHeading;
    std::string pendingModel = fallbackModel;
    auto modelHeading = [&](const std::string& model) {
        return model.empty() ? std::string("Assistant")
                             : ServerManager::ModelDisplayName(model);
    };
    auto emitAssistantHeading = [&](const std::string& model) {
        const std::string heading = modelHeading(model);
        if (heading == lastHeading) return;
        md += "## " + heading + "\n\n";
        lastHeading = heading;
    };

    for (const auto& msg : history.GetMessages()) {
        try {
            if (!msg || !msg->has("role") || msg->isNull("role")) continue;
            const std::string role = msg->getValue<std::string>("role");
            std::string content;
            if (msg->has("content") && !msg->isNull("content"))
                content = msg->getValue<std::string>("content");

            if (role == "user") {
                ChatDisplay::ToolBlock tool;
                if (ParseSavedToolBlock(content, tool)) {
                    emitAssistantHeading(pendingModel);
                    tool.body = ExportStripToolFooter(tool.body);
                    tool.errorBody = ExportStripToolFooter(tool.errorBody);
                    // Long or multi-line commands (agent PowerShell one-liners
                    // run to thousands of characters) are shortened in the
                    // summary line; the full text goes in a Command block.
                    constexpr size_t kMaxInlineCommand = 120;
                    const std::string& echo = tool.commandEcho;
                    const bool longEcho = echo.size() > kMaxInlineCommand ||
                                          echo.find('\n') != std::string::npos;
                    md += "> **" + ExportToolName(tool.toolName) + "**";
                    if (!echo.empty()) {
                        std::string shown = echo.substr(0, echo.find('\n'));
                        if (longEcho) {
                            shown = LbUtf8SafeTruncate(shown, 100);
                            while (!shown.empty() && shown.back() == ' ')
                                shown.pop_back();
                            shown += " \xE2\x80\xA6";
                        }
                        md += " \xE2\x80\x94 " + ExportInlineCode(shown);
                    }
                    if (!tool.statusChips.empty()) {
                        md += " \xC2\xB7 ";
                        for (size_t i = 0; i < tool.statusChips.size(); ++i)
                            md += (i ? ", " : "") + tool.statusChips[i];
                    }
                    md += "\n\n";
                    if (longEcho) {
                        const std::string lang =
                            LowerCopy(tool.toolName) == "powershell"
                                ? "powershell" : "";
                        ExportAppendCollapsible(md, "Command", echo, lang);
                    }
                    if (!TrimCopy(tool.body).empty())
                        ExportAppendCollapsible(md, "Output", tool.body, tool.bodyLang);
                    if (!TrimCopy(tool.errorBody).empty())
                        ExportAppendCollapsible(md, "Error", tool.errorBody, "");
                    // Short file lists inline; long ones (test runs can
                    // attach dozens) folded so they don't flood the page.
                    const size_t nFiles = tool.presentedFiles.size();
                    if (nFiles > 3)
                        md += "<details><summary>Files (" +
                              std::to_string(nFiles) + ")</summary>\n\n";
                    for (const auto& f : tool.presentedFiles)
                        md += "- " + ExportInlineCode(f.diskPath) + "\n";
                    if (nFiles > 3) md += "\n</details>\n";
                    if (nFiles) md += "\n";
                    continue;
                }

                // Same cleanup as replay: drop the per-turn session header.
                if (content.rfind("[Session context:", 0) == 0) {
                    const size_t cut = content.find("\n\n");
                    content = (cut == std::string::npos) ? std::string()
                                                         : content.substr(cut + 2);
                }

                std::string attachments;
                if (msg->has("attachments") && !msg->isNull("attachments")) {
                    auto arr = msg->getArray("attachments");
                    for (unsigned i = 0; arr && i < arr->size(); ++i) {
                        auto att = arr->getObject(i);
                        if (!att) continue;
                        std::string name = att->optValue<std::string>("filename", "");
                        if (name.empty())
                            name = att->optValue<std::string>("kind", "") == "image"
                                ? "image" : "file";
                        if (!attachments.empty()) attachments += ", ";
                        attachments += ExportInlineCode(name);
                    }
                }

                content = TrimCopy(content);
                if (content.empty() && attachments.empty()) continue;

                const std::string target = ChatHistory::GetMessageTarget(msg);
                while (md.size() > 1 && md.back() == '\n' &&
                       md[md.size() - 2] == '\n')
                    md.pop_back();               // exactly one blank line
                md += "\n---\n\n## You";
                if (!target.empty())
                    md += " \xE2\x86\x92 " + ServerManager::ModelDisplayName(target);
                md += "\n\n";
                lastHeading = "You";
                if (!attachments.empty())
                    md += "_Attachments: " + attachments + "_\n\n";
                if (!content.empty())
                    md += content + "\n\n";
            }
            else if (role == "assistant") {
                content = ExportStripThinking(content);
                {
                    std::string m = ChatHistory::GetMessageModel(msg);
                    pendingModel = m.empty() ? fallbackModel : m;
                }

                std::vector<std::string> images;
                if (msg->has("images") && !msg->isNull("images")) {
                    auto arr = msg->getArray("images");
                    for (unsigned i = 0; arr && i < arr->size(); ++i) {
                        try { images.push_back(arr->get(i).convert<std::string>()); }
                        catch (...) {}
                    }
                }
                if (content.empty() && images.empty()) continue;

                emitAssistantHeading(pendingModel);
                if (!content.empty())
                    md += content + "\n\n";
                for (const auto& img : images)
                    md += "_Generated image: " + ExportInlineCode(img) + "_\n\n";
            }
            else if (role == "system") {
                content = TrimCopy(content);
                if (content.empty() || IsHiddenGoalReplaySystemMessage(content))
                    continue;
                md += "> _" + content + "_\n\n";
            }
        } catch (...) {
            continue;   // one malformed message must not sink the export
        }
    }

    while (!md.empty() && (md.back() == '\n' || md.back() == ' '))
        md.pop_back();
    return md;   // caller appends the single final newline
}

} // anonymous namespace

namespace {
// Whole file as bytes; empty when missing or unreadable.
std::string ReadChatLog(const std::string& folderUtf8, const wchar_t* name)
{
    try {
        const std::filesystem::path p =
            std::filesystem::path(path_safety::Utf8ToWide(folderUtf8)) / name;
        std::ifstream f(p, std::ios::binary);
        if (!f) return {};
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    } catch (...) {
        return {};
    }
}
} // anonymous namespace

void ConversationController::ExportConversation(const std::string& path, bool withMetrics)
{
    if (path.empty()) return;

    // Read-only.  The active chat exports from memory (includes anything
    // not yet autosaved) unless a response is streaming, in which case the
    // last saved file is the consistent snapshot.  Every other chat,
    // including ones open in another window, is read from disk.
    const std::string activePath = m_chatHistory->GetFilePath();
    const bool isActive = !activePath.empty() && path == activePath;
    const bool busy = m_cb.isBusy && m_cb.isBusy();

    std::string markdown;
    std::string title;
    if (isActive && !busy) {
        markdown = BuildConversationMarkdown(
            *m_chatHistory, m_modelSwitcher.GetConversationModelForSave());
        title = m_chatHistory->GetTitle();
        if (title.empty()) title = m_chatHistory->GenerateTitle();
    }
    else {
        ChatHistory tmp;
        std::vector<std::string> models;
        if (!tmp.LoadFromFile(path, models)) {
            wxMessageBox("The conversation could not be opened for export.",
                         "Export Conversation", wxOK | wxICON_WARNING, &m_frame);
            return;
        }
        markdown = BuildConversationMarkdown(
            tmp, models.empty() ? std::string() : models.front());
        title = tmp.GetTitle();
        if (title.empty()) title = tmp.GenerateTitle();
    }

    // Metrics come from the chat folder's logs, which every completed
    // reply has already appended to, so the active chat needs no flush.
    if (withMetrics) {
        const std::string folder = ChatHistory::GetChatFolder(path);
        markdown += "\n\n---\n\n";
        markdown += lb_export_metrics::BuildMetricsMarkdown(
            folder.empty() ? std::string() : ReadChatLog(folder, L"turn_stats.tsv"),
            folder.empty() ? std::string() : ReadChatLog(folder, L"ctx_calibration.tsv"));
        while (!markdown.empty() && markdown.back() == '\n') markdown.pop_back();
    }

    // Remember the folder for the rest of the session; start in Documents.
    static wxString s_lastExportDir;
    if (s_lastExportDir.empty() || !wxDirExists(s_lastExportDir))
        s_lastExportDir = wxStandardPaths::Get().GetDocumentsDir();

    wxFileDialog dlg(&m_frame,
        withMetrics ? "Export Conversation with Metrics" : "Export Conversation",
        s_lastExportDir,
        wxString::FromUTF8(ExportSafeFileName(title) +
                           (withMetrics ? " (metrics).md" : ".md")),
        "Markdown (*.md)|*.md|Text (*.txt)|*.txt",
        wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() == wxID_CANCEL) return;

    wxFileName out(dlg.GetPath());
    if (out.GetExt().empty())
        out.SetExt(dlg.GetFilterIndex() == 1 ? "txt" : "md");
    s_lastExportDir = out.GetPath();

    wxFile file;
    bool ok = file.Create(out.GetFullPath(), /*overwrite=*/true);
    if (ok) {
        markdown += "\n";
        ok = file.Write(markdown.data(), markdown.size()) == markdown.size();
        ok = file.Close() && ok;
    }
    if (!ok) {
        wxMessageBox("Could not write:\n" + out.GetFullPath(),
                     "Export Conversation", wxOK | wxICON_WARNING, &m_frame);
        return;
    }

    if (m_chatDisplay)
        m_chatDisplay->DisplaySystemMessage(
            std::string(withMetrics ? "Exported conversation with metrics to "
                                    : "Exported conversation to ") +
            out.GetFullPath().ToUTF8().data());
}

void ConversationController::RenameConversation(const std::string& path)
{
    if (path.empty()) return;

    if (m_cb.isBusy && m_cb.isBusy()) {
        wxMessageBox(
            "Stop the current response before renaming a conversation.",
            "Response in Progress",
            wxOK | wxICON_INFORMATION,
            &m_frame);
        return;
    }

    const std::string activePath = m_chatHistory->GetFilePath();
    const bool isActive = !activePath.empty() && path == activePath;

    // Another window owns the path and may autosave at any moment.
    if (!isActive &&
        wxGetApp().GetConversationRegistry().OwnerOf(path, &m_frame)) {
        wxMessageBox(
            "That conversation is open in another LlamaBoss window. "
            "Rename it from that window instead.",
            "Conversation In Use",
            wxOK | wxICON_INFORMATION,
            &m_frame);
        return;
    }

    std::string currentTitle;
    std::vector<std::string> models;
    ChatHistory tmp;

    if (isActive) {
        currentTitle = m_chatHistory->GetTitle();
    }
    else {
        if (!tmp.LoadFromFile(path, models)) {
            wxMessageBox(
                "The conversation could not be opened for renaming.",
                "Rename Conversation",
                wxOK | wxICON_WARNING,
                &m_frame);
            return;
        }
        currentTitle = tmp.GetTitle();
    }

    wxTextEntryDialog dlg(
        &m_frame,
        "Enter a new conversation title:",
        "Rename Conversation",
        wxString::FromUTF8(currentTitle.c_str()));
    if (dlg.ShowModal() != wxID_OK)
        return;

    wxString titleWx = dlg.GetValue();
    titleWx.Trim(true).Trim(false);
    if (titleWx.empty()) {
        wxMessageBox(
            "The conversation title cannot be empty.",
            "Rename Conversation",
            wxOK | wxICON_INFORMATION,
            &m_frame);
        return;
    }

    // Keep the stored title generous while protecting the title bar and
    // sidebar from accidentally pasted paragraphs.  The sidebar still
    // applies its own 64-character display shortening.
    if (titleWx.length() > 160)
        titleWx = titleWx.Left(160);

    const std::string newTitle = std::string(titleWx.ToUTF8().data());
    if (newTitle == currentTitle)
        return;

    bool saved = false;
    if (isActive) {
        m_chatHistory->SetTitle(newTitle);
        AutoSaveConversation(/*refreshSidebar=*/false, /*durable=*/true,
                             /*touchActivityTimestamp=*/false);
        UpdateWindowTitle();
        saved = true;
    }
    else {
        tmp.SetTitle(newTitle);
        saved = tmp.SaveToFile(path, models, /*durable=*/true,
                               /*touchActivityTimestamp=*/false);
    }

    if (!saved) {
        wxMessageBox(
            "The new title could not be saved.",
            "Rename Conversation",
            wxOK | wxICON_WARNING,
            &m_frame);
        return;
    }

    if (m_sidebar.IsVisible()) {
        m_sidebar.InvalidateMetadata({ path });
        m_sidebar.Refresh(m_chatHistory->GetFilePath());
    }
}

void ConversationController::SetConversationsPinned(
    const std::vector<std::string>& paths,
    bool pinned)
{
    if (paths.empty()) return;

    if (m_cb.isBusy && m_cb.isBusy()) {
        wxMessageBox(
            "Stop the current response before changing pinned conversations.",
            "Response in Progress",
            wxOK | wxICON_INFORMATION,
            &m_frame);
        return;
    }

    const std::string activePath = m_chatHistory->GetFilePath();
    size_t changed = 0;
    size_t skipped = 0;

    for (const auto& path : paths) {
        if (path.empty()) continue;

        const bool isActive = !activePath.empty() && path == activePath;
        if (!isActive &&
            wxGetApp().GetConversationRegistry().OwnerOf(path, &m_frame)) {
            ++skipped;
            continue;
        }

        if (isActive) {
            if (m_chatHistory->IsPinned() == pinned)
                continue;
            m_chatHistory->SetPinned(pinned);
            AutoSaveConversation(/*refreshSidebar=*/false, /*durable=*/true,
                             /*touchActivityTimestamp=*/false);
            ++changed;
            continue;
        }

        ChatHistory tmp;
        std::vector<std::string> models;
        if (!tmp.LoadFromFile(path, models)) {
            ++skipped;
            continue;
        }
        if (tmp.IsPinned() == pinned)
            continue;

        tmp.SetPinned(pinned);
        if (!tmp.SaveToFile(path, models, /*durable=*/true,
                               /*touchActivityTimestamp=*/false)) {
            ++skipped;
            continue;
        }
        ++changed;
    }

    if (m_sidebar.IsVisible()) {
        m_sidebar.InvalidateMetadata(paths);
        m_sidebar.Refresh(m_chatHistory->GetFilePath());
    }

    // The card moving between PINNED / date sections is the success
    // confirmation.  Do not write management acknowledgements into the chat
    // surface: DisplaySystemMessage is display-only, but it still leaves
    // distracting transcript-like lines until the conversation is replayed.
    // Only surface exceptional partial failures, using a normal dialog that
    // cannot be mistaken for conversation content.
    if (skipped > 0) {
        std::string message;
        if (changed > 0) {
            message = std::to_string(changed) +
                      (changed == 1 ? " conversation was updated. "
                                    : " conversations were updated. ");
        }
        else {
            message = "No conversations were changed. ";
        }
        message += std::to_string(skipped) +
                   (skipped == 1
                       ? " conversation was skipped because it is open elsewhere or unreadable."
                       : " conversations were skipped because they are open elsewhere or unreadable.");
        wxMessageBox(
            wxString::FromUTF8(message.c_str()),
            pinned ? "Pin Conversations" : "Unpin Conversations",
            wxOK | wxICON_WARNING,
            &m_frame);
    }
}

void ConversationController::SetConversationsArchived(
    const std::vector<std::string>& paths,
    bool archived)
{
    if (paths.empty()) return;

    if (m_cb.isBusy && m_cb.isBusy()) {
        wxMessageBox(
            "Stop the current response before archiving conversations.",
            "Response in Progress",
            wxOK | wxICON_INFORMATION,
            &m_frame);
        return;
    }

    const std::string activePath = m_chatHistory->GetFilePath();
    size_t changed = 0;
    size_t skipped = 0;

    for (const auto& path : paths) {
        if (path.empty()) continue;

        const bool isActive = !activePath.empty() && path == activePath;
        if (!isActive &&
            wxGetApp().GetConversationRegistry().OwnerOf(path, &m_frame)) {
            ++skipped;
            continue;
        }

        if (isActive) {
            if (m_chatHistory->IsArchived() == archived)
                continue;
            m_chatHistory->SetArchived(archived);
            AutoSaveConversation(/*refreshSidebar=*/false, /*durable=*/true,
                             /*touchActivityTimestamp=*/false);
            ++changed;
            continue;
        }

        ChatHistory tmp;
        std::vector<std::string> models;
        if (!tmp.LoadFromFile(path, models)) {
            ++skipped;
            continue;
        }
        if (tmp.IsArchived() == archived)
            continue;

        tmp.SetArchived(archived);
        if (!tmp.SaveToFile(path, models, /*durable=*/true,
                               /*touchActivityTimestamp=*/false)) {
            ++skipped;
            continue;
        }
        ++changed;
    }

    if (m_sidebar.IsVisible()) {
        m_sidebar.InvalidateMetadata(paths);
        m_sidebar.Refresh(m_chatHistory->GetFilePath());
    }

    // Archiving/restoring visibly moves the card and updates the archive
    // footer count, which is sufficient success feedback.  Keep the chat
    // surface reserved for conversation content and report only partial
    // failures with a conventional warning dialog.
    if (skipped > 0) {
        std::string message;
        if (changed > 0) {
            message = std::to_string(changed) +
                      (changed == 1 ? " conversation was updated. "
                                    : " conversations were updated. ");
        }
        else {
            message = "No conversations were changed. ";
        }
        message += std::to_string(skipped) +
                   (skipped == 1
                       ? " conversation was skipped because it is open elsewhere or unreadable."
                       : " conversations were skipped because they are open elsewhere or unreadable.");
        wxMessageBox(
            wxString::FromUTF8(message.c_str()),
            archived ? "Archive Conversations" : "Restore Conversations",
            wxOK | wxICON_WARNING,
            &m_frame);
    }
}

// ═════════════════════════════════════════════════════════════════
//  DELETE
// ═════════════════════════════════════════════════════════════════

void ConversationController::DeleteConversations(
    const std::vector<std::string>& requestedPaths)
{
    if (requestedPaths.empty()) return;

    if (m_cb.isBusy && m_cb.isBusy()) {
        wxMessageBox(
            "Stop the current response before deleting conversations.",
            "Response in Progress",
            wxOK | wxICON_INFORMATION,
            &m_frame);
        return;
    }

    // ── Cross-window ownership guard ─────────────────────────────
    // Deleting a conversation another window has open would pull the
    // file out from under it (its next autosave resurrects a ghost;
    // its chat folder vanishes mid-use).  Filter those out BEFORE
    // the confirmation so the dialog quotes an honest count.
    //
    // Defined as a lambda because the filter must run TWICE: once
    // here, and once more after the confirmation dialog below.  The
    // dialog is window-modal (it disables only this frame), so other
    // windows stay interactive while it is up and can claim one of
    // these paths in the meantime — deleting on the pre-modal result
    // alone is a TOCTOU that defeats the guard.
    auto filterOpenElsewhere =
        [this](const std::vector<std::string>& in,
               size_t& skipped) -> std::vector<std::string> {
            std::vector<std::string> out;
            out.reserve(in.size());
            skipped = 0;
            for (const auto& p : in) {
                if (wxGetApp().GetConversationRegistry().OwnerOf(p, &m_frame))
                    ++skipped;
                else
                    out.push_back(p);
            }
            return out;
        };

    size_t skippedOpenElsewhere = 0;
    std::vector<std::string> filePaths =
        filterOpenElsewhere(requestedPaths, skippedOpenElsewhere);
    if (skippedOpenElsewhere > 0 && m_chatDisplay) {
        m_chatDisplay->DisplaySystemMessage(
            skippedOpenElsewhere == 1
                ? "1 conversation was skipped because it is open in another window."
                : std::to_string(skippedOpenElsewhere) +
                  " conversations were skipped because they are open in another window.");
    }
    if (filePaths.empty()) return;

    wxString msg;
    if (filePaths.size() == 1) {
        msg = "Delete this conversation? This cannot be undone.\n\n"
              "Any files in this conversation's chat folder will also be deleted.";
    } else {
        msg = wxString::Format(
            "Delete %zu conversations? This cannot be undone.\n\n"
            "Any files in those conversations' chat folders will also be deleted.",
            filePaths.size());
    }

    int result = wxMessageBox(msg, "Delete Conversation",
        wxYES_NO | wxICON_WARNING,
        &m_frame);
    if (result != wxYES) return;

    // ── Re-run the ownership filter AFTER the confirmation modal ──
    // wxMessageBox above is window-modal: only this frame was
    // disabled while it sat open.  In that gap the user could click
    // one of these conversations in another window's sidebar (or a
    // second-launch handoff could open a new window onto one), and
    // that window is now live on the file — possibly mid-agent-run,
    // writing into its chat folder.  Deleting on the pre-modal
    // filter would recursively remove that folder out from under it.
    // Registry claims mutate on the main thread only and no modal
    // runs between here and the delete loops below, so this second
    // pass is authoritative.
    size_t claimedDuringConfirm = 0;
    filePaths = filterOpenElsewhere(filePaths, claimedDuringConfirm);
    if (claimedDuringConfirm > 0 && m_chatDisplay) {
        m_chatDisplay->DisplaySystemMessage(
            claimedDuringConfirm == 1
                ? "1 conversation was not deleted because it was opened "
                  "in another window while the confirmation was showing."
                : std::to_string(claimedDuringConfirm) +
                  " conversations were not deleted because they were opened "
                  "in another window while the confirmation was showing.");
    }
    if (filePaths.empty()) return;

    // Prevent an in-flight autosave of the active conversation from
    // resurrecting a file immediately after the user deletes it.
    WaitForPendingSaves();

    // ── Pass 1 (fast): remove .json files, collect sidecar dirs ──
    //
    // The .json is what ConversationSidebar::ScanConversations keys off
    // of, so once it's gone the chat is effectively "deleted" from the
    // user's POV.  We do all of those first, refresh the sidebar so the
    // rows visibly disappear, and only then do the slow recursive
    // directory cleanups (Pass 2 below).  Without this split the UI
    // thread is wedged inside wxFileName::Rmdir for every chat's
    // attachments/files/Chats tree before anything updates, which
    // is what makes bulk delete feel sluggish at higher chat counts.
    struct PendingCleanup {
        std::string filePath;
        wxString    attachDir;
        wxString    filesDir;
        wxString    chatDir;
    };
    std::vector<PendingCleanup> cleanups;
    cleanups.reserve(filePaths.size());

    bool clearedActive = false;
    int deleted = 0;

    for (const auto& filePath : filePaths) {
        if (!wxRemoveFile(wxString::FromUTF8(filePath)))
            continue;

        ++deleted;

        wxFileName convFn(wxString::FromUTF8(filePath));
        std::string stem(convFn.GetName().ToUTF8().data());

        PendingCleanup pc;
        pc.filePath    = filePath;
        pc.attachDir   = wxString::FromUTF8(
            ChatHistory::GetConversationsDir() + "/attachments/" + stem);
        pc.filesDir    = wxString::FromUTF8(
            ChatHistory::GetConversationsDir() + "/files/" + stem);
        pc.chatDir     = wxString::FromUTF8(
            ChatHistory::GetChatFolder(filePath));
        ChatHistory::ForgetChatFolder(filePath);
        cleanups.push_back(std::move(pc));

        // If deleting the currently active conversation, clear the
        // display now -- before the sidebar refresh -- so the refresh
        // reflects the cleared state.
        if (!clearedActive && filePath == m_chatHistory->GetFilePath()) {
            m_chatHistory->Clear();
            m_chatDisplay->Clear();
            m_attachments.Clear();
            if (m_cb.cancelPendingSend) m_cb.cancelPendingSend();
            UpdateWindowTitle();
            clearedActive = true;
        }
    }

    // ── Sidebar update: chats visibly disappear here ──
    if (deleted > 0) {
        m_sidebar.ClearSelection();
        if (m_sidebar.IsVisible())
            m_sidebar.Refresh(m_chatHistory->GetFilePath());
    }

    // ── Pass 2 (slow): recursive sidecar/chat-folder cleanup ──
    //
    // Wait cursor while these run so the user sees work is still in
    // progress even though the sidebar already updated.  This loop
    // remains on the UI thread by design -- moving it off-thread would
    // need coordination with anything else that touches the same
    // conversation paths, and the perceived hitch after the sidebar
    // updates is much smaller than the original "everything wedges
    // before anything visibly changes" feel.
    if (!cleanups.empty()) {
        wxBusyCursor wait;
        for (const auto& pc : cleanups) {
            if (wxDirExists(pc.attachDir)) {
                wxLogNull suppressErrors;
                if (!wxFileName::Rmdir(pc.attachDir, wxPATH_RMDIR_RECURSIVE)) {
                    if (auto* logger = m_appState.GetLogger())
                        logger->warning("Could not fully remove attachment dir: " +
                            std::string(pc.attachDir.ToUTF8().data()));
                }
            }
            if (wxDirExists(pc.filesDir)) {
                wxLogNull suppressErrors;
                if (!wxFileName::Rmdir(pc.filesDir, wxPATH_RMDIR_RECURSIVE)) {
                    if (auto* logger = m_appState.GetLogger())
                        logger->warning("Could not fully remove generated files dir: " +
                            std::string(pc.filesDir.ToUTF8().data()));
                }
            }
            if (wxDirExists(pc.chatDir)) {
                wxLogNull suppressErrors;
                if (!wxFileName::Rmdir(pc.chatDir, wxPATH_RMDIR_RECURSIVE)) {
                    if (auto* logger = m_appState.GetLogger())
                        logger->warning("Could not fully remove chat folder: " +
                            std::string(pc.chatDir.ToUTF8().data()));
                }
            }

            if (auto* logger = m_appState.GetLogger())
                logger->information("Deleted conversation: " + pc.filePath);
        }
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

    // ── Cross-window ownership guard ─────────────────────────────
    // The same conversation open in two windows is last-writer-wins
    // data loss on every autosave.  If another window already has
    // this one, raise that window instead of loading.
    if (wxFrame* owner = wxGetApp().GetConversationRegistry()
                             .OwnerOf(path, &m_frame)) {
        owner->Raise();
        owner->RequestUserAttention(wxUSER_ATTENTION_INFO);
        return false;
    }

    // Do not cancel queued work or replace history until persistence succeeds.
    if (!SaveBeforeLeaving()) return false;

    // KV fast path, save side: snapshot the outgoing conversation's
    // slot state before its history is replaced.  Fire-and-forget;
    // ServerManager's ownership guard makes this a no-op unless the
    // slot verifiably holds this conversation's KV (see
    // SaveSlotStateForConversation).  Must run while m_chatHistory
    // still points at the conversation being left.  Routed through
    // ModelService: skipped when another window is mid-generation on
    // the shared slot.
    wxGetApp().GetModelService().SaveSlotStateForConversation(
        &m_frame, m_chatHistory->GetFilePath());

    std::vector<std::string> loadedModels;
    auto newHistory = std::make_unique<ChatHistory>();
    if (!newHistory->LoadFromFile(path, loadedModels)) {
        wxMessageBox("Failed to load conversation file. The current conversation is still open.",
                     "Load Failed", wxOK | wxICON_ERROR, &m_frame);
        return false;
    }

    // The load succeeded and the outgoing history is safe on disk.
    if (m_cb.cancelPendingSend) m_cb.cancelPendingSend();

    // Replace current history (through the unique_ptr reference)
    m_chatHistory = std::move(newHistory);

    // This window now owns the conversation (releases any previous
    // claim implicitly — one claim per frame).
    wxGetApp().GetConversationRegistry().SetCurrent(
        &m_frame, m_chatHistory->GetFilePath());

    // Session trust: LoadFromFile always starts with a clean approval
    // state (chat_history.cpp commit block).  If this conversation was
    // granted one-approval mode earlier in this app session, re-arm it
    // so the user isn't re-prompted just for having switched chats.
    if (wxGetApp().GetConversationRegistry().HasSessionTrust(
            m_chatHistory->GetFilePath())) {
        m_chatHistory->RememberAllToolApprovalsForChat();
    }
    for (const std::string& root :
         wxGetApp().GetConversationRegistry().SessionWriteRoots(
             m_chatHistory->GetFilePath())) {
        m_chatHistory->GrantWriteRootForChat(root);
    }

    // ── Model handling: frame-owned preference, deferred service switch ──
    // Loading a conversation must not rewrite the app-global active target or
    // shared readiness.  This frame records the saved model as its preference;
    // if the shared service is already on that model it can send immediately,
    // otherwise the first Send requests the global switch.
    std::string primaryModel = loadedModels.empty() ? "" : loadedModels.front();
    bool needsModelPickNotice = false;   // shown after the transcript replays

    const bool savedModelIsLocal =
        !primaryModel.empty() && wxFileExists(wxString::FromUTF8(primaryModel));
    if (!primaryModel.empty() &&
        m_modelSwitcher.SetConversationPreferredSavedModel(
            primaryModel, m_chatHistory->GetModelSelection())) {
        if (m_modelSwitcher.IsConversationTargetActive() &&
            m_modelSwitcher.IsServerReady()) {
            m_modelSwitcher.ClearPendingDeferredModel();
            if (m_statusDot) m_statusDot->SetConnected(true);

            // Slot persistence applies only to a matching live local model.
            // Routed through ModelService: a restore against the slot
            // another window's stream is generating into would clobber
            // that stream's KV, so it is skipped under contention.
            if (savedModelIsLocal)
                wxGetApp().GetModelService().RestoreSlotStateForConversation(
                    &m_frame, path);
        }
        else {
            if (m_statusDot) m_statusDot->SetConnected(false);
        }
    }
    else if (!primaryModel.empty()) {
        // Missing local path, or a model id from an older file that no single
        // connection offers.  Never guess a provider: with a remote target
        // active the chat waits for an explicit model pick; with a local
        // target it adopts that (nothing leaves the machine).
        const bool needsPick =
            m_modelSwitcher.HandleUnresolvedSavedModel(primaryModel);
        if (m_statusDot)
            m_statusDot->SetConnected(m_modelSwitcher.IsServerReady());
        needsModelPickNotice = needsPick;
        if (auto* logger = m_appState.GetLogger())
            logger->warning("Conversation model could not be resolved: " +
                primaryModel + (needsPick ? " — waiting for an explicit model selection"
                                          : " — keeping current local model"));
    }
    else {
        m_modelSwitcher.AdoptActiveTargetForConversation();
        if (m_statusDot)
            m_statusDot->SetConnected(m_modelSwitcher.IsServerReady());
    }

    m_modelSwitcher.UpdateModelLabel();

    // Replay to display.  Rebuilding a long saved chat into wxRichTextCtrl can
    // be slow if every historical message scrolls/repaints individually, so
    // freeze the transcript for the full clear + replay and thaw/scroll once.
    {
        ChatReplayBatchGuard replayBatch(m_chatDisplay);
        m_chatDisplay->Clear();
        m_attachments.Clear();
        ReplayConversation();
    }
    if (needsModelPickNotice) {
        m_chatDisplay->DisplaySystemMessage(
            "This chat was saved with " + primaryModel + ", which doesn't "
            "match a single AI connection. Pick a model from the model menu "
            "before sending.");
    }
    UpdateWindowTitle();
    if (m_sidebar.IsVisible())
        m_sidebar.Refresh(m_chatHistory->GetFilePath());

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
        try {
            if (!msg) continue;

            if (!msg->has("role") || msg->isNull("role") ||
                !msg->has("content") || msg->isNull("content")) {
                if (auto* logger = m_appState.GetLogger())
                    logger->warning("Skipping malformed conversation message during replay: missing role/content");
                continue;
            }

            std::string role = msg->getValue<std::string>("role");
            std::string content = msg->getValue<std::string>("content");

            // Empty content is normally a skip, but an assistant turn
            // from an image-output model can be image-only: the
            // "images" sidecar is the visible payload.
            if (content.empty() &&
                !(role == "assistant" && msg->has("images"))) continue;

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

                // Strip the per-turn [Session context: ...] header the
                // frame prepends to the wire copy of user messages (see
                // MyFrame::BuildSessionContextHeader).  It exists for
                // the model, not the reader.
                if (displayContent.rfind("[Session context:", 0) == 0) {
                    const size_t cut = displayContent.find("\n\n");
                    displayContent = (cut == std::string::npos)
                        ? std::string()
                        : displayContent.substr(cut + 2);
                    if (displayContent.empty()) continue;
                }
                std::vector<std::string> imagePaths;

                if (msg->has("attachments") && !msg->isNull("attachments")) {
                    auto arr = msg->getArray("attachments");
                    if (arr && arr->size() > 0) {
                        std::string prefix;
                        std::string convDir = ChatHistory::GetConversationsDir();
                        std::string chatDir = ChatHistory::GetChatFolder(m_chatHistory->GetFilePath());

                        for (unsigned ai = 0; ai < arr->size(); ++ai) {
                            auto att = arr->getObject(ai);
                            if (!att) continue;

                            std::string kind;
                            std::string fname;
                            if (att->has("kind") && !att->isNull("kind"))
                                kind = att->getValue<std::string>("kind");
                            if (att->has("filename") && !att->isNull("filename"))
                                fname = att->getValue<std::string>("filename");

                            if (kind == "image") {
                                if (!prefix.empty()) prefix += ", ";
                                prefix += "\xF0\x9F\x96\xBC ";  // 🖼
                                prefix += fname.empty() ? "image" : fname;

                                if (att->has("storage_path") && !att->isNull("storage_path")) {
                                    std::string sp = att->getValue<std::string>("storage_path");
                                    if (!sp.empty()) {
                                        std::string chatFilePath = chatDir + "/" + sp;
                                        if (wxFileExists(wxString::FromUTF8(chatFilePath)))
                                            imagePaths.push_back(chatFilePath);
                                        else
                                            imagePaths.push_back(convDir + "/" + sp); // legacy sidecar path
                                    }
                                }
                            }
                            else if (kind == "text_file") {
                                if (!prefix.empty()) prefix += ", ";
                                prefix += "\xF0\x9F\x93\x84 ";  // 📄
                                prefix += fname.empty() ? "text file" : fname;
                            }
                        }
                        if (!prefix.empty())
                            displayContent = "[" + prefix + "] " + displayContent;
                    }
                }

                m_chatDisplay->DisplayUserMessage(displayContent, target, imagePaths);
            }
            else if (role == "assistant") {
                std::string msgModel = ChatHistory::GetMessageModel(msg);
                if (msgModel.empty()) msgModel = m_modelSwitcher.GetConversationModelForSave();
                m_chatDisplay->DisplayAssistantMessage(
                    ServerManager::ModelDisplayName(msgModel),
                    content,
                    m_appState.GetTheme().chatAssistant
                );

                // Generated-images sidecar: resolve the chat-folder-
                // relative paths and redisplay the thumbnails.
                // Missing files (deleted chat folder) skip
                // silently inside DisplayInlineImages.
                if (msg->has("images") && !msg->isNull("images")) {
                    try {
                        auto imgs = msg->getArray("images");
                        if (imgs && imgs->size() > 0) {
                            const std::string chatDir =
                                ChatHistory::GetChatFolder(
                                    m_chatHistory->GetFilePath());
                            std::vector<std::string> paths;
                            for (unsigned ii = 0; ii < imgs->size(); ++ii) {
                                std::string rel;
                                try {
                                    rel = imgs->get(ii)
                                              .convert<std::string>();
                                } catch (...) { continue; }
                                if (!rel.empty())
                                    paths.push_back(chatDir + "/" + rel);
                            }
                            if (!paths.empty())
                                m_chatDisplay->DisplayInlineImages(paths);
                        }
                    } catch (...) { /* malformed sidecar — skip */ }
                }
            }
            else if (role == "system") {
                if (IsHiddenGoalReplaySystemMessage(content)) {
                    continue;
                }
                m_chatDisplay->DisplaySystemMessage(content);
            }
            else {
                if (auto* logger = m_appState.GetLogger())
                    logger->warning("Skipping conversation message with unknown role during replay: " + role);
            }
        } catch (const std::exception& ex) {
            if (auto* logger = m_appState.GetLogger())
                logger->warning(std::string("Skipping malformed conversation message during replay: ") + ex.what());
            continue;
        } catch (...) {
            if (auto* logger = m_appState.GetLogger())
                logger->warning("Skipping malformed conversation message during replay: unknown error");
            continue;
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
                convTitle = LbUtf8SafeTruncate(convTitle, 37) + "...";
            }
            title = convTitle + " - LlamaBoss";
        }
    }

    if (m_chatHistory->HasProject()) {
        std::string projectName = m_chatHistory->GetProjectName();
        if (projectName.size() > 28) {
            projectName = LbUtf8SafeTruncate(projectName, 25) + "...";
        }
        title = "[" + projectName + "] " + title;
    }

    m_frame.SetTitle(wxString::FromUTF8(title));

    // Strip refresh piggybacks on every existing call site.
    if (m_cb.onProjectStateChanged) m_cb.onProjectStateChanged();
}
