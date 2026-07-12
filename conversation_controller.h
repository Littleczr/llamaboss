// conversation_controller.h
// Owns conversation save / load / delete / replay and window-title
// updates.  Extracted from MyFrame to keep the frame thin.
#pragma once

#include <wx/wx.h>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <atomic>

// Forward declarations
class AppState;
class ChatHistory;
class ChatDisplay;
class AttachmentManager;
class ConversationSidebar;
class ServerManager;
class ModelSwitcher;
class StatusDot;

class ConversationController
{
public:
    struct Callbacks {
        std::function<bool()>  isBusy;

        // Fired at the end of UpdateWindowTitle().  The frame uses this
        // to refresh the project status strip alongside the title, so
        // every existing UpdateWindowTitle() call site automatically
        // keeps the strip in sync.
        std::function<void()>  onProjectStateChanged;

        // Fired at the top of LoadConversationFromPath(), before the new
        // conversation replaces the current one.  The frame uses this to
        // drop cross-chat transient state (queued sends behind deferred model
        // loads, pending Skill authoring sessions, etc.) so old-chat state
        // cannot leak into the newly-loaded chat.  Optional.
        std::function<void()>  cancelPendingSend;
    };

    ConversationController(wxFrame& frame,
                           AppState& appState,
                           std::unique_ptr<ChatHistory>& chatHistory,
                           ChatDisplay* chatDisplay,
                           AttachmentManager& attachments,
                           ConversationSidebar& sidebar,
                           ServerManager& serverManager,
                           ModelSwitcher& modelSwitcher,
                           StatusDot* statusDot,
                           std::weak_ptr<std::atomic<bool>> aliveToken);
    ~ConversationController();

    void SetCallbacks(Callbacks cb) { m_cb = std::move(cb); }

    // ── File-dialog save/load ────────────────────────────────────
    void OnSaveConversation();
    void OnLoadConversation();

    // ── Automatic save (no dialog) ───────────────────────────────
    // durable=false (the default) snapshots the conversation and queues JSON
    // construction + staged atomic replacement on the controller's serialized
    // persistence worker.  Bursty saves are coalesced by conversation path.
    // Pass durable=true when history is about to be cleared or replaced
    // (window close, New Chat, conversation switch, model/folder change): the
    // queue is drained first, then the final write is synchronously flushed.
    void AutoSaveConversation(bool refreshSidebar = true, bool durable = false);

    // ── Batch delete ─────────────────────────────────────────────
    // Paths open in another window are skipped (Phase 3b guard).
    void DeleteConversations(const std::vector<std::string>& requestedPaths);

    // ── Load a specific file (also used by sidebar click) ────────
    bool LoadConversationFromPath(const std::string& path);

    // ── Replay all messages to the display ───────────────────────
    void ReplayConversation();

    // ── Window title ─────────────────────────────────────────────
    void UpdateWindowTitle();

private:
    class AsyncSaveState;
    void OnAsyncSaveComplete(wxCommandEvent& evt);
    void WaitForPendingSaves();

    wxFrame&                        m_frame;
    AppState&                       m_appState;
    std::unique_ptr<ChatHistory>&   m_chatHistory;
    ChatDisplay*                    m_chatDisplay;
    AttachmentManager&              m_attachments;
    ConversationSidebar&            m_sidebar;
    ServerManager&                  m_serverManager;
    ModelSwitcher&                  m_modelSwitcher;
    StatusDot*                      m_statusDot;

    Callbacks                       m_cb;
    std::unique_ptr<AsyncSaveState> m_asyncSave;
};
