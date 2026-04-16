// conversation_controller.h
// Owns conversation save / load / delete / replay and window-title
// updates.  Extracted from MyFrame to keep the frame thin.
#pragma once

#include <wx/wx.h>
#include <string>
#include <vector>
#include <functional>
#include <memory>

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
    };

    ConversationController(wxFrame& frame,
                           AppState& appState,
                           std::unique_ptr<ChatHistory>& chatHistory,
                           ChatDisplay* chatDisplay,
                           AttachmentManager& attachments,
                           ConversationSidebar& sidebar,
                           ServerManager& serverManager,
                           ModelSwitcher& modelSwitcher,
                           StatusDot* statusDot);

    void SetCallbacks(Callbacks cb) { m_cb = std::move(cb); }

    // ── File-dialog save/load ────────────────────────────────────
    void OnSaveConversation();
    void OnLoadConversation();

    // ── Automatic save (no dialog) ───────────────────────────────
    void AutoSaveConversation(bool refreshSidebar = true);

    // ── Batch delete ─────────────────────────────────────────────
    void DeleteConversations(const std::vector<std::string>& filePaths);

    // ── Load a specific file (also used by sidebar click) ────────
    bool LoadConversationFromPath(const std::string& path);

    // ── Replay all messages to the display ───────────────────────
    void ReplayConversation();

    // ── Window title ─────────────────────────────────────────────
    void UpdateWindowTitle();

private:
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
};
