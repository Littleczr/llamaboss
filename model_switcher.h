// model_switcher.h
// Thin coordinator for model scanning, picker menu, switching,
// and server lifecycle events.  Extracted from MyFrame.
#pragma once

#include <wx/wx.h>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <unordered_map>

// Forward declarations
class AppState;
class ServerManager;
class ChatDisplay;
class ChatHistory;
class AttachmentManager;
class StatusDot;
struct ThemeData;

class ModelSwitcher
{
public:
    // Callbacks wired by MyFrame after construction.
    struct Callbacks {
        std::function<bool()>  isBusy;
        std::function<void()>  autoSave;
        std::function<void()>  updateWindowTitle;
    };

    ModelSwitcher(AppState& appState,
                  ServerManager& serverManager,
                  ChatDisplay* chatDisplay,
                  std::unique_ptr<ChatHistory>& chatHistory,
                  AttachmentManager& attachments,
                  StatusDot* statusDot,
                  wxStaticText* modelLabel);

    void SetCallbacks(Callbacks cb) { m_cb = std::move(cb); }

    // ── Server bootstrap ──────────────────────────────────────────
    void StartInitialServer();

    // ── Quick switch (model pill menu) ────────────────────────────
    void OnModelPillClick(wxWindow* popupParent);
    void OnModelPillRightClick(wxWindow* parent);

    // ── Core switch ──────────────────────────────────────────────
    void SwitchToModel(const std::string& newModel);

    // ── Server event handlers (called from MyFrame) ──────────────
    void OnServerReady();
    void OnServerError(const std::string& error);

    // ── Shared helper ────────────────────────────────────────────
    void UpdateModelLabel();

    // Server readiness — read/written by MyFrame and ConversationController
    bool m_serverReady = false;

private:
    void ShowModelPickerMenu(wxWindow* anchor,
                             const std::vector<std::string>& ggufPaths);

    AppState&                       m_appState;
    ServerManager&                  m_serverManager;
    ChatDisplay*                    m_chatDisplay;
    std::unique_ptr<ChatHistory>&   m_chatHistory;
    AttachmentManager&              m_attachments;
    StatusDot*                      m_statusDot;
    wxStaticText*                   m_modelLabel;

    Callbacks                       m_cb;
    std::vector<std::string>        m_pickerModels;
    std::unordered_map<int,size_t>  m_menuIdMap;  // maps wxNewId() → m_pickerModels index
};
