#define _CRT_SECURE_NO_WARNINGS

#include <cctype>
#include <wx/wx.h>
#include <wx/artprov.h>
#include <wx/textdlg.h>
#include <wx/log.h>
#include <wx/richtext/richtextctrl.h>
#include <wx/utils.h>
#include <wx/thread.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/dcbuffer.h>
#include <wx/dnd.h>
#include <wx/clipbrd.h>
#include <wx/mstream.h>
#include <wx/dir.h>
#include <wx/scrolwin.h>
#include <wx/wrapsizer.h>

#include <vector>
#include <string>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <memory>
#include <functional>

// Poco headers for base64 and JSON
#include <Poco/Base64Encoder.h>
#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Stringifier.h>
#include <Poco/URI.h>
#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/StreamCopier.h>
#include <Poco/Timespan.h>

#include "settings.h"
#include "chat_client.h"
#include "chat_display.h"
#include "chat_history.h"
#include "app_state.h"
#include "conversation_sidebar.h"
#include "attachment_manager.h"
#include "model_manager.h"
#include "server_manager.h"

// ── Extracted widget & coordinator headers ────────────────────────
#include "widgets.h"
#include "chat_input_ctrl.h"
#include "chat_display_ctrl.h"
#include "ui_builder.h"
#include "model_switcher.h"
#include "conversation_controller.h"
#include "ascii_animation.h"

// ─── Application version ─────────────────────────────────────
static const char* LLAMABOSS_VERSION = "0.1.0";

// ─── Forward declaration ─────────────────────────────────────────
class MyFrame;

// ─── Drag-and-drop target for image files ────────────────────────
class ImageDropTarget : public wxFileDropTarget
{
public:
    ImageDropTarget(MyFrame* frame) : m_frame(frame) {}
    virtual bool OnDropFiles(wxCoord x, wxCoord y,
        const wxArrayString& filenames) override;
private:
    MyFrame* m_frame;
};

// ═══════════════════════════════════════════════════════════════════
//  Chat State Machine
// ═══════════════════════════════════════════════════════════════════
enum class ChatState {
    Idle,
    Streaming,
};

// ═══════════════════════════════════════════════════════════════════
class MyFrame : public wxFrame {
public:
    MyFrame()
        : wxFrame(nullptr, wxID_ANY, "LlamaBoss",
            wxDefaultPosition, wxSize(1100, 700),
            wxDEFAULT_FRAME_STYLE)
        , m_sidebar(nullptr)
        , m_isClosing(false)
        , m_alive(std::make_shared<std::atomic<bool>>(true))
        , m_generationId(0)
        , m_appState(std::make_unique<AppState>())
        , m_chatClient(std::make_unique<ChatClient>(this, m_alive))
        , m_chatDisplay(nullptr)
        , m_chatHistory(std::make_unique<ChatHistory>())
        , m_attachments(std::make_unique<AttachmentManager>())
        , m_chatState(ChatState::Idle)

    {
        // Ensure data directories exist
        ServerManager::EnsureDataDirs();

        // Initialize application state first
        if (!m_appState->Initialize()) {
            wxMessageBox("Failed to initialize application state", "Startup Error",
                wxOK | wxICON_ERROR);
        }

        // Create server manager (spawns llama-server process)
        m_serverManager = std::make_unique<ServerManager>(this, m_alive, m_appState->GetLogger());

        SetBackgroundColour(m_appState->GetTheme().bgMain);

        auto* mainSizer = new wxBoxSizer(wxVERTICAL);

        // ─── TOP BAR (via UIBuilder) ─────────────────────────────────
        auto tb = UIBuilder::BuildTopBar(this, mainSizer, m_appState->GetTheme());
        _toolbarPanel   = tb.toolbarPanel;
        _titleLabel     = tb.titleLabel;
        _modelPill      = tb.modelPill;
        _modelLabel     = tb.modelLabel;
        _statusDot      = tb.statusDot;
        _sidebarToggle  = tb.sidebarToggle;
        _newChatButton  = tb.newChatButton;
        _settingsButton = tb.settingsButton;
        _aboutButton    = tb.aboutButton;
        _topSeparator   = tb.topSeparator;

        // ─── CONTENT AREA (sidebar + chat) ────────────────────────────
        _contentSizer = new wxBoxSizer(wxHORIZONTAL);

        // ── Sidebar (collapsible conversation list) ──
        // Callbacks reference m_convController which is created below;
        // the lambdas capture `this` and dereference lazily, so this is safe.
        ConversationSidebar::Callbacks sidebarCallbacks;
        sidebarCallbacks.onConversationClicked = [this](const std::string& path) {
            m_convController->LoadConversationFromPath(path);
        };
        sidebarCallbacks.onNewChatClicked = [this]() {
            wxCommandEvent e;
            OnNewChat(e);
        };
        sidebarCallbacks.onDeleteRequested = [this](const std::vector<std::string>& paths) {
            m_convController->DeleteConversations(paths);
        };
        sidebarCallbacks.isBusy = [this]() {
            return IsBusy();
        };
        sidebarCallbacks.onResized = [this](int width) {
            m_appState->SetSidebarWidth(width);
        };
        m_sidebar = std::make_unique<ConversationSidebar>(this, m_appState->GetTheme(),
                                            sidebarCallbacks);
        m_sidebar->SetWidth(m_appState->GetSidebarWidth());
        _contentSizer->Add(m_sidebar->GetPanel(), 0, wxEXPAND);

        // ── Right panel (chat display + input) ──
        _rightPanel = new wxPanel(this, wxID_ANY);
        _rightPanel->SetBackgroundColour(m_appState->GetTheme().bgMain);
        auto* rightSizer = new wxBoxSizer(wxVERTICAL);

        _chatDisplayCtrl = new ChatDisplayCtrl(
            _rightPanel, wxID_ANY, wxEmptyString,
            wxDefaultPosition, wxDefaultSize,
            wxRE_MULTILINE | wxRE_READONLY | wxBORDER_NONE
        );
        _chatDisplayCtrl->SetBackgroundColour(m_appState->GetTheme().bgMain);
        _chatDisplayCtrl->SetForegroundColour(m_appState->GetTheme().textPrimary);
        rightSizer->Add(_chatDisplayCtrl, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);

        // ─── ATTACHMENT CHIP BAR (hidden by default) ─────────────────
        _attachChipBar = new wxPanel(_rightPanel, wxID_ANY);
        _attachChipBar->SetBackgroundColour(m_appState->GetTheme().bgMain);
        _attachChipSizer = new wxWrapSizer(wxHORIZONTAL);
        _attachChipBar->SetSizer(_attachChipSizer);
        _attachChipBar->Hide();
        rightSizer->Add(_attachChipBar, 0, wxLEFT | wxTOP, 12);

        m_attachments->SetLogger(m_appState->GetLogger());
        m_attachments->SetOnChanged([this]() { RebuildAttachmentChips(); });

        // ─── INPUT AREA (via UIBuilder) ──────────────────────────────
        auto ia = UIBuilder::BuildInputArea(_rightPanel, rightSizer, m_appState->GetTheme());
        _inputContainer = ia.inputContainer;
        _inputSeparator = ia.inputSeparator;
        _userInputCtrl  = ia.userInputCtrl;
        _sendButton     = ia.sendButton;
        _stopButton     = ia.stopButton;
        _attachButton   = ia.attachButton;
        _inputSizer     = ia.inputSizer;

        _rightPanel->SetSizer(rightSizer);
        _contentSizer->Add(_rightPanel, 1, wxEXPAND);
        mainSizer->Add(_contentSizer, 1, wxEXPAND);
        SetSizer(mainSizer);

        // ─── Setup fonts ─────────────────────────────────────────────
        wxFont codeFont = m_appState->CreateMonospaceFont(16);
        _chatDisplayCtrl->SetFont(codeFont);
        _userInputCtrl->SetFont(codeFont);

        m_chatDisplay = std::make_unique<ChatDisplay>(_chatDisplayCtrl);
        m_chatDisplay->SetFont(codeFont);
        m_chatDisplay->ApplyTheme(m_appState->GetTheme());

        _statusDot->SetColors(m_appState->GetTheme().accentButton,
                              m_appState->GetTheme().textMuted);

        // ─── Create coordinators ─────────────────────────────────────
        m_modelSwitcher = std::make_unique<ModelSwitcher>(
            *m_appState, *m_serverManager, m_chatDisplay.get(),
            m_chatHistory, *m_attachments, _statusDot, _modelLabel);

        m_convController = std::make_unique<ConversationController>(
            *this, *m_appState, m_chatHistory, m_chatDisplay.get(),
            *m_attachments, *m_sidebar, *m_serverManager,
            *m_modelSwitcher, _statusDot);

        m_modelSwitcher->SetCallbacks({
            /*isBusy*/            [this]() { return IsBusy(); },
            /*autoSave*/          [this]() { m_convController->AutoSaveConversation(); },
            /*updateWindowTitle*/ [this]() { m_convController->UpdateWindowTitle(); }
        });
        m_convController->SetCallbacks({
            /*isBusy*/ [this]() { return IsBusy(); }
        });

        // ─── Bind events ─────────────────────────────────────────────
        _sendButton->Bind(wxEVT_BUTTON, &MyFrame::OnSendMessage, this);
        _stopButton->Bind(wxEVT_BUTTON, &MyFrame::OnStopGeneration, this);

        // Animation timer
        Bind(wxEVT_TIMER, &MyFrame::OnAnimationTimer, this, m_animTimer.GetId());


        // Attach (📎) button hover
        _attachButton->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& e) {
            _attachButton->SetForegroundColour(m_appState->GetTheme().textPrimary);
            _attachButton->Refresh();
            e.Skip();
            });
        _attachButton->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) {
            _attachButton->SetForegroundColour(m_appState->GetTheme().textMuted);
            _attachButton->Refresh();
            e.Skip();
            });
        _attachButton->Bind(wxEVT_BUTTON, &MyFrame::OnAttachImage, this);
        
        
        
        _userInputCtrl->Bind(wxEVT_TEXT_ENTER, &MyFrame::OnSendMessage, this);
        _userInputCtrl->Bind(wxEVT_TEXT, &MyFrame::OnUserInputChanged, this);

        // Settings (⚙) button hover
        _settingsButton->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& e) {
            _settingsButton->SetForegroundColour(m_appState->GetTheme().textPrimary);
            _settingsButton->Refresh();
            e.Skip();
            });
        _settingsButton->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) {
            _settingsButton->SetForegroundColour(m_appState->GetTheme().textMuted);
            _settingsButton->Refresh();
            e.Skip();
            });
        _settingsButton->Bind(wxEVT_BUTTON, &MyFrame::OnOpenSettings, this);

        // New Chat (+) button hover — uses chatAssistant (mint green)
        _newChatButton->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& e) {
            _newChatButton->SetForegroundColour(m_appState->GetTheme().chatAssistant);
            _newChatButton->Refresh();
            e.Skip();
            });
        _newChatButton->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) {
            _newChatButton->SetForegroundColour(m_appState->GetTheme().textMuted);
            _newChatButton->Refresh();
            e.Skip();
            });
        _newChatButton->Bind(wxEVT_BUTTON, &MyFrame::OnNewChat, this);


        _sidebarToggle->Bind(wxEVT_BUTTON, &MyFrame::OnToggleSidebar, this);
        _aboutButton->Bind(wxEVT_BUTTON, &MyFrame::OnAbout, this);
        Bind(wxEVT_ACTIVATE, &MyFrame::OnFrameActivate, this);

        Bind(wxEVT_ASSISTANT_DELTA, &MyFrame::OnAssistantDelta, this);
        Bind(wxEVT_ASSISTANT_COMPLETE, &MyFrame::OnAssistantComplete, this);
        Bind(wxEVT_ASSISTANT_ERROR, &MyFrame::OnAssistantError, this);

        // Model pill click → delegate to ModelSwitcher
        auto pillClick = [this](wxMouseEvent&) {
            m_modelSwitcher->OnModelPillClick(this);
        };
        auto pillRightClick = [this](wxMouseEvent&) {
            m_modelSwitcher->OnModelPillRightClick(this);
        };
        _modelPill->Bind(wxEVT_LEFT_UP, pillClick);
        _modelLabel->Bind(wxEVT_LEFT_UP, pillClick);
        _statusDot->Bind(wxEVT_LEFT_UP, pillClick);
        _modelPill->Bind(wxEVT_RIGHT_UP, pillRightClick);
        _modelLabel->Bind(wxEVT_RIGHT_UP, pillRightClick);
        _statusDot->Bind(wxEVT_RIGHT_UP, pillRightClick);

        // Server lifecycle events
        Bind(wxEVT_SERVER_READY, &MyFrame::OnServerReady, this);
        Bind(wxEVT_SERVER_ERROR, &MyFrame::OnServerError, this);

        // Drag-and-drop + clipboard paste
        SetDropTarget(new ImageDropTarget(this));
        _userInputCtrl->SetImagePasteHandler([this]() -> bool {
            if (IsBusy()) return false;
            return TryPasteImageFromClipboard();
        });

        // Keyboard shortcuts
        Bind(wxEVT_CHAR_HOOK, &MyFrame::OnCharHook, this);

        // Load icon and update model display
        m_appState->LoadApplicationIcon(this);
        m_modelSwitcher->UpdateModelLabel();

        // Restore window state
        m_appState->RestoreWindowState(this);
        Bind(wxEVT_CLOSE_WINDOW, &MyFrame::OnClose, this);

        // Final setup
        CallAfter([this]() {
            _userInputCtrl->SetFocus();
            wxCommandEvent anEvent(wxEVT_TEXT, _userInputCtrl->GetId());
            OnUserInputChanged(anEvent);
            m_modelSwitcher->StartInitialServer();
        });
    }

    ~MyFrame() override = default;

    void OnClose(wxCloseEvent& evt)
    {
        m_alive->store(false);
        m_isClosing = true;

        StopAnimation();

        if (m_chatClient->IsStreaming())
            m_chatClient->StopGeneration();

        if (!m_chatHistory->IsEmpty())
            m_convController->AutoSaveConversation();

        m_appState->SaveWindowState(this);
        evt.Skip();
    }

    // ── Public interface for attachments (used by drop target) ─────
    bool AttachImageFromFile(const std::string& filePath)
    {
        if (IsBusy()) return false;
        bool ok = m_attachments->AttachImageFromFile(filePath);
        if (ok) _userInputCtrl->SetFocus();
        return ok;
    }

    bool AttachTextFile(const std::string& filePath)
    {
        if (IsBusy()) return false;

        wxFileName fname(wxString::FromUTF8(filePath));
        wxULongLong fileSize = fname.GetSize();
        if (fileSize == wxInvalidSize ||
            fileSize.GetValue() > AttachmentManager::kMaxTextFileBytes) {
            wxMessageBox("Text file too large (max 100 KB).",
                "Attachment Error", wxOK | wxICON_WARNING);
            return false;
        }

        bool ok = m_attachments->AttachTextFile(filePath);
        if (ok) _userInputCtrl->SetFocus();
        return ok;
    }

private:
    // ─── UI Controls ──────────────────────────────────────────────
    ChatDisplayCtrl* _chatDisplayCtrl;
    ChatInputCtrl*   _userInputCtrl;
    wxButton*        _sendButton;
    wxButton*        _stopButton;
    wxButton*        _attachButton;
    wxButton*        _settingsButton;
    wxButton*        _newChatButton;
    wxButton*        _sidebarToggle;
    wxButton*        _aboutButton;
    wxPanel*         _attachChipBar;
    wxWrapSizer*     _attachChipSizer;
    wxBoxSizer*      _inputSizer;
    wxBoxSizer*      _contentSizer;

    wxPanel*       _toolbarPanel;
    wxStaticText*  _titleLabel;
    wxPanel*       _modelPill;
    wxPanel*       _topSeparator;
    wxPanel*       _rightPanel;
    wxPanel*       _inputContainer;
    wxPanel*       _inputSeparator;

    std::unique_ptr<ConversationSidebar> m_sidebar;
    bool m_isClosing;

    wxStaticText* _modelLabel;
    StatusDot*    _statusDot;

    // ─── Thread safety ────────────────────────────────────────────
    std::shared_ptr<std::atomic<bool>> m_alive;
    unsigned long m_generationId;

    // ─── Application Components ───────────────────────────────────
    std::unique_ptr<AppState>      m_appState;
    std::unique_ptr<ChatClient>    m_chatClient;
    std::unique_ptr<ChatDisplay>   m_chatDisplay;
    std::unique_ptr<ChatHistory>   m_chatHistory;
    std::unique_ptr<AttachmentManager> m_attachments;
    std::unique_ptr<ServerManager> m_serverManager;

    // ─── Coordinators ────────────────────────────────────────────
    std::unique_ptr<ModelSwitcher>          m_modelSwitcher;
    std::unique_ptr<ConversationController> m_convController;

    // ─── Chat state machine ──────────────────────────────────────
    ChatState m_chatState;

    // ── ASCII Animation ──────────────────────────────────────────
    wxTimer                          m_animTimer{this};
    std::unique_ptr<AsciiAnimation>  m_activeAnimation;

    // ═════════════════════════════════════════════════════════════
    //  HELPERS
    // ═════════════════════════════════════════════════════════════

    void ApplyThemeToUI()
    {
        const ThemeData& t = m_appState->GetTheme();

        SetBackgroundColour(t.bgMain);

        _toolbarPanel->SetBackgroundColour(t.bgToolbar);
        _sidebarToggle->SetBackgroundColour(t.bgToolbar);
        _sidebarToggle->SetForegroundColour(t.textMuted);
        _titleLabel->SetForegroundColour(t.textPrimary);
        _modelPill->SetBackgroundColour(t.bgToolbar);
        _modelLabel->SetForegroundColour(t.textPrimary);
        _newChatButton->SetBackgroundColour(t.bgToolbar);
        _newChatButton->SetForegroundColour(t.textMuted);
        _settingsButton->SetBackgroundColour(t.bgToolbar);
        _settingsButton->SetForegroundColour(t.textMuted);
        _aboutButton->SetBackgroundColour(t.bgToolbar);
        _aboutButton->SetForegroundColour(t.textMuted);
        _topSeparator->SetBackgroundColour(t.borderSubtle);
        _statusDot->SetColors(t.accentButton, t.textMuted);

        if (m_sidebar) m_sidebar->ApplyTheme(t);

        _rightPanel->SetBackgroundColour(t.bgMain);
        _chatDisplayCtrl->SetBackgroundColour(t.bgMain);
        _chatDisplayCtrl->SetForegroundColour(t.textPrimary);
        _attachChipBar->SetBackgroundColour(t.bgMain);
        RebuildAttachmentChips();

        _inputContainer->SetBackgroundColour(t.bgInputArea);
        _inputSeparator->SetBackgroundColour(t.borderSubtle);
        _attachButton->SetBackgroundColour(t.bgInputArea);
        _attachButton->SetForegroundColour(t.textMuted);
        _userInputCtrl->SetBackgroundColour(t.bgInputField);
        _userInputCtrl->SetForegroundColour(t.textPrimary);
        _sendButton->SetBackgroundColour(t.accentButton);
        _sendButton->SetForegroundColour(t.accentButtonText);
        _stopButton->SetBackgroundColour(t.stopButton);
        _stopButton->SetForegroundColour(t.stopButtonText);

        if (m_chatDisplay) m_chatDisplay->ApplyTheme(t);
        if (m_sidebar && m_sidebar->IsVisible())
            m_sidebar->Refresh(m_chatHistory->GetFilePath());

        Refresh();
        Update();
    }

    void RebuildAttachmentChips()
    {
        _attachChipSizer->Clear(true);

        if (!m_attachments->HasPending()) {
            _attachChipBar->Hide();
            _rightPanel->GetSizer()->Layout();
            return;
        }

        const ThemeData& t = m_appState->GetTheme();

        for (size_t i = 0; i < m_attachments->GetCount(); ++i) {
            const auto& item = m_attachments->GetAt(i);
            std::string icon = (item.type == PendingAttachment::Type::Image)
                ? "\xF0\x9F\x96\xBC" : "\xF0\x9F\x93\x84";

            auto* chip = new AttachmentChip(
                _attachChipBar, i, icon, item.name,
                t.attachChipBg, t.attachIndicator, t.textMuted,
                [this](size_t idx) { m_attachments->RemoveAt(idx); }
            );
            _attachChipSizer->Add(chip, 0, wxRIGHT | wxBOTTOM, 4);
        }

        _attachChipBar->Show();
        _attachChipBar->Layout();
        _rightPanel->GetSizer()->Layout();
    }

    void SetStreamingState(bool streaming)
    {
        _sendButton->Show(!streaming);
        _stopButton->Show(streaming);
        _userInputCtrl->Enable(!streaming);
        _attachButton->Enable(!streaming);
        _settingsButton->Enable(!streaming);
        _newChatButton->Enable(!streaming);
        _inputSizer->Layout();

        if (!streaming) {
            m_chatState = ChatState::Idle;
            _userInputCtrl->SetFocus();
        }
    }

    bool IsBusy() const { return m_chatState != ChatState::Idle; }

    // ═════════════════════════════════════════════════════════════
    //  EVENT HANDLERS
    // ═════════════════════════════════════════════════════════════

    void OnAttachImage(wxCommandEvent&)
    {
        if (m_attachments->GetCount() >= AttachmentManager::kMaxAttachments) {
            wxMessageBox(wxString::Format(
                "Maximum of %zu attachments reached.\nRemove some before adding more.",
                AttachmentManager::kMaxAttachments),
                "Attachment Limit", wxOK | wxICON_INFORMATION);
            return;
        }

        wxFileDialog dlg(this, "Attach files", "", "",
            "Image files (*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.webp)"
            "|*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.webp"
            "|Text files (*.txt;*.md;*.json;*.cpp;*.h;*.py;*.js;*.ts;*.css;*.html;*.xml;*.yaml;*.yml;*.csv;*.log)"
            "|*.txt;*.md;*.json;*.cpp;*.h;*.py;*.js;*.ts;*.css;*.html;*.xml;*.yaml;*.yml;*.csv;*.log"
            "|All files (*.*)|*.*",
            wxFD_OPEN | wxFD_FILE_MUST_EXIST | wxFD_MULTIPLE);

        if (dlg.ShowModal() == wxID_CANCEL) return;

        wxArrayString paths;
        dlg.GetPaths(paths);

        int attached = 0, unsupported = 0, failures = 0;
        bool hitCap = false;

        for (const auto& path : paths) {
            if (m_attachments->GetCount() >= AttachmentManager::kMaxAttachments) {
                hitCap = true;
                break;
            }

            std::string pathUtf8 = WxToUtf8(path);
            if (AttachmentManager::IsImageFile(pathUtf8)) {
                if (AttachImageFromFile(pathUtf8)) ++attached; else ++failures;
            }
            else if (AttachmentManager::IsTextFile(pathUtf8)) {
                if (AttachTextFile(pathUtf8)) ++attached; else ++failures;
            }
            else {
                ++unsupported;
            }
        }

        if (hitCap) {
            wxMessageBox(wxString::Format(
                "Attached %d file(s). Remaining skipped (max %zu attachments).",
                attached, AttachmentManager::kMaxAttachments),
                "Attachment Limit", wxOK | wxICON_INFORMATION);
        }
        else if (unsupported > 0 && attached == 0 && failures == 0) {
            wxMessageBox("Unsupported file type.\n\n"
                "Supported: images (png, jpg, gif, bmp, webp)\n"
                "and text files (txt, md, json, cpp, h, py, js, etc.)",
                "Unsupported File", wxOK | wxICON_INFORMATION);
        }
        else if (failures > 0 || unsupported > 0) {
            wxMessageBox(wxString::Format(
                "%d of %zu file(s) could not be attached.",
                failures + unsupported, paths.size()),
                "Attachment Warning", wxOK | wxICON_WARNING);
        }
    }

    void OnAssistantDelta(wxCommandEvent& event)
    {
        if (m_isClosing) return;
        if (static_cast<unsigned long>(event.GetExtraLong()) != m_generationId) return;

        std::string delta = WxToUtf8(event.GetString());
        m_chatHistory->AppendToLastAssistantMessage(delta);
        m_chatDisplay->DisplayAssistantDelta(delta);
    }

    void OnAssistantComplete(wxCommandEvent& event)
    {
        if (m_isClosing) return;
        if (static_cast<unsigned long>(event.GetExtraLong()) != m_generationId) return;

        std::string fullResponse = WxToUtf8(event.GetString());
        m_chatDisplay->DisplayAssistantComplete();

        if (!fullResponse.empty()) {
            m_chatHistory->UpdateLastAssistantMessage(fullResponse);
        }
        else if (auto* logger = m_appState->GetLogger()) {
            logger->warning("Assistant complete event arrived empty; keeping streamed content");
        }

        m_chatClient->ResetStreamingState();
        SetStreamingState(false);
        m_convController->AutoSaveConversation();

        if (auto* logger = m_appState->GetLogger())
            logger->information("Chat response completed");
    }

    void OnAssistantError(wxCommandEvent& event)
    {
        if (m_isClosing) return;
        if (static_cast<unsigned long>(event.GetExtraLong()) != m_generationId) return;

        std::string error = WxToUtf8(event.GetString());
        std::string modelName = ServerManager::ModelDisplayName(m_appState->GetModel());

        std::string friendly;
        if (error.find("Connection refused") != std::string::npos ||
            error.find("Network Error") != std::string::npos ||
            error.find("No connection") != std::string::npos ||
            error.find("Connection reset") != std::string::npos ||
            error.find("Net Exception") != std::string::npos) {
            friendly = "Could not connect to llama-server at " + m_appState->GetApiUrl() +
                ".\nThe server may still be loading the model \xe2\x80\x94 try again in a moment.";
            _statusDot->SetConnected(false);
        }
        else if (error.find("Timeout") != std::string::npos ||
            error.find("timeout") != std::string::npos) {
            friendly = "Request timed out. The model may still be loading \xe2\x80\x94 try again in a moment.";
        }
        else if (error.find("model") != std::string::npos &&
            error.find("not found") != std::string::npos) {
            friendly = "Model \"" + modelName + "\" was not found. "
                "Open Settings to pick an available model.";
        }
        else {
            friendly = "Error: " + error;
        }

        m_chatDisplay->DisplaySystemMessage(friendly);
        // Remove the assistant message that was being streamed (partial or empty).
        // RemoveLastAssistantMessage() is safe — it checks role internally.
        m_chatHistory->RemoveLastAssistantMessage();
        m_chatClient->ResetStreamingState();
        SetStreamingState(false);

        if (!m_chatHistory->IsEmpty()) m_convController->AutoSaveConversation();

        if (auto* logger = m_appState->GetLogger())
            logger->error("Chat error (" + modelName + "): " + error);
    }

    void OnToggleSidebar(wxCommandEvent&)
    {
        m_sidebar->Toggle();
        if (m_sidebar->IsVisible())
            m_sidebar->Refresh(m_chatHistory->GetFilePath());
        _contentSizer->Layout();
        GetSizer()->Layout();
    }

    void OnStopGeneration(wxCommandEvent&)
    {
        // Stop any running Easter egg animation
        if (m_activeAnimation) { StopAnimation(); return; }

        if (IsBusy()) {
            ++m_generationId;
            m_chatClient->StopGeneration();
            m_chatDisplay->DisplayAssistantComplete();
            m_chatDisplay->DisplaySystemMessage("Generation stopped by user");
            if (m_chatHistory->HasAssistantPlaceholder())
                m_chatHistory->RemoveLastAssistantMessage();
            SetStreamingState(false);
            if (!m_chatHistory->IsEmpty()) m_convController->AutoSaveConversation();
        }
    }

    // ── ASCII Animation engine ───────────────────────────────────
    void OnAnimationTimer(wxTimerEvent&)
    {
        if (!m_activeAnimation) { m_animTimer.Stop(); return; }

        if (m_activeAnimation->Tick()) {
            AnimationFrame frame = m_activeAnimation->GetFrame();
            m_chatDisplay->BeginAnimationFrame();
            for (const auto& line : frame)
                m_chatDisplay->WriteAnimationLine(line);
            m_chatDisplay->EndAnimationFrame();
        }
        else {
            // Animation finished — stop timer, leave final frame
            m_animTimer.Stop();
            m_activeAnimation.reset();
            m_chatDisplay->ClearAnimation();
        }
    }

    void StopAnimation()
    {
        if (m_animTimer.IsRunning()) m_animTimer.Stop();
        m_activeAnimation.reset();
        m_chatDisplay->ClearAnimation();
    }

    void OnOpenSettings(wxCommandEvent&)
    {
        if (IsBusy()) {
            wxMessageBox("Cannot change settings while generating response",
                "Settings", wxOK | wxICON_INFORMATION);
            return;
        }

        SettingsDialog dlg(this, m_appState->GetModel(),
                           m_appState->GetThemeName(),
                           m_appState->GetTheme());

        if (dlg.ShowModal() == wxID_OK)
        {
            bool modelChanged = dlg.WasModelChanged();
            if (modelChanged) {
                std::string newModel = dlg.GetSelectedModel();

                if (!m_chatHistory->IsEmpty())
                    m_convController->AutoSaveConversation();

                bool mc, ac;
                m_appState->UpdateSettings(newModel, m_appState->GetApiUrl(), mc, ac);

                m_modelSwitcher->m_serverReady = false;
                _statusDot->SetConnected(false);
                m_chatDisplay->DisplaySystemMessage(
                    "Loading " + ServerManager::ModelDisplayName(newModel) + "...");
                m_serverManager->StartServer(newModel);

                m_chatHistory->Clear();
                m_chatDisplay->Clear();
                m_attachments->Clear();
                m_modelSwitcher->UpdateModelLabel();
                m_convController->UpdateWindowTitle();
            }

            bool themeChanged = dlg.WasThemeChanged();
            if (themeChanged) {
                m_appState->SetTheme(dlg.GetSelectedTheme());
                ApplyThemeToUI();
            }

            if (!modelChanged && themeChanged) {
                if (!m_chatHistory->IsEmpty()) {
                    m_chatDisplay->Clear();
                    m_convController->ReplayConversation();
                }
                m_chatDisplay->DisplaySystemMessage("Theme changed to " +
                    m_appState->GetThemeName() + ".");
            }
        }
    }

    void OnAbout(wxCommandEvent&)
    {
        wxString msg;
        msg << "LlamaBoss v" << LLAMABOSS_VERSION << "\n\n"
            << "A lightweight desktop chat client for local LLMs.\n"
            << "Powered by llama.cpp\n\n"
            << "Built with wxWidgets + Poco\n"
            << "License: MIT\n\n"
            << wxString::FromUTF8("Model: ") << wxString::FromUTF8(
                ServerManager::ModelDisplayName(m_appState->GetModel())) << "\n"
            << wxString::FromUTF8("Server: ") << wxString::FromUTF8(m_appState->GetApiUrl()) << "\n"
            << wxString::FromUTF8("Models: ") << wxString::FromUTF8(ServerManager::GetModelsDir());
        wxMessageBox(msg, "About LlamaBoss", wxOK | wxICON_INFORMATION);
    }

    void OnNewChat(wxCommandEvent&)
    {
        if (IsBusy()) return;

        if (!m_chatHistory->IsEmpty())
            m_convController->AutoSaveConversation(false);

        m_chatHistory->Clear();
        m_chatDisplay->Clear();
        m_attachments->Clear();
        m_modelSwitcher->UpdateModelLabel();
        m_convController->UpdateWindowTitle();
        if (m_sidebar->IsVisible())
            m_sidebar->Refresh(m_chatHistory->GetFilePath());
        _userInputCtrl->SetFocus();

        if (auto* logger = m_appState->GetLogger())
            logger->information("New chat started");
    }

    // ── Server lifecycle → delegate to ModelSwitcher ──────────────
    void OnServerReady(wxCommandEvent&)
    {
        if (m_isClosing) return;
        m_modelSwitcher->OnServerReady();
    }

    void OnServerError(wxCommandEvent& event)
    {
        if (m_isClosing) return;
        m_modelSwitcher->OnServerError(WxToUtf8(event.GetString()));
    }

    void OnUserInputChanged(wxCommandEvent&)
    {
        if (!_userInputCtrl || !_inputSizer) return;

        const int DESIRED_BASE_HEIGHT = 30;
        const int MAX_LINES_TO_SHOW = 5;

        int charHeight = _userInputCtrl->GetCharHeight();
        int lineHeight = charHeight + 4;
        int lines = _userInputCtrl->GetNumberOfLines();
        wxString val = _userInputCtrl->GetValue();

        int newH;
        if (val.IsEmpty() || lines == 1)
            newH = std::max(DESIRED_BASE_HEIGHT, lineHeight);
        else
            newH = std::max(lineHeight * std::min(lines, MAX_LINES_TO_SHOW),
                            DESIRED_BASE_HEIGHT);

        if (_userInputCtrl->GetMinSize().y != newH) {
            _userInputCtrl->SetMinSize(wxSize(-1, newH));
            _inputSizer->Layout();
            if (GetSizer()) GetSizer()->Layout();
        }
    }

    void OnCharHook(wxKeyEvent& evt)
    {
        if (evt.ControlDown()) {
            switch (evt.GetKeyCode()) {
            case 'N': { wxCommandEvent e; OnNewChat(e); } return;
            case 'S': m_convController->OnSaveConversation(); return;
            case 'O': m_convController->OnLoadConversation(); return;
            }
        }
        evt.Skip();
    }

    bool TryPasteImageFromClipboard()
    {
        if (!wxTheClipboard->Open()) return false;

        bool hasImage = wxTheClipboard->IsSupported(wxDF_BITMAP);
        if (!hasImage) { wxTheClipboard->Close(); return false; }

        wxBitmapDataObject bmpData;
        bool gotData = wxTheClipboard->GetData(bmpData);
        wxTheClipboard->Close();

        if (!gotData || !bmpData.GetBitmap().IsOk()) return false;

        wxImage img = bmpData.GetBitmap().ConvertToImage();
        wxMemoryOutputStream memStream;
        if (!img.SaveFile(memStream, wxBITMAP_TYPE_PNG)) return false;

        size_t dataSize = memStream.GetSize();
        std::vector<unsigned char> rawData(dataSize);
        memStream.CopyTo(rawData.data(), dataSize);

        std::ostringstream base64Stream;
        Poco::Base64Encoder encoder(base64Stream);
        encoder.write(reinterpret_cast<const char*>(rawData.data()), dataSize);
        encoder.close();

        std::string base64 = base64Stream.str();
        base64.erase(std::remove_if(base64.begin(), base64.end(), ::isspace),
            base64.end());

        if (base64.empty()) return false;

        bool ok = m_attachments->AttachImageFromBase64(base64, "clipboard_image.png");
        if (ok) _userInputCtrl->SetFocus();
        return ok;
    }

    void OnFrameActivate(wxActivateEvent& evt)
    {
        if (evt.GetActive() && !IsBusy())
            _userInputCtrl->SetFocus();
        evt.Skip();
    }

    // ═════════════════════════════════════════════════════════════
    //  SEND MESSAGE
    // ═════════════════════════════════════════════════════════════

    void OnSendMessage(wxCommandEvent&)
    {
        if (IsBusy()) return;
        if (m_activeAnimation) return;  // animation playing

        if (!m_modelSwitcher->m_serverReady) {
            m_chatDisplay->DisplaySystemMessage(
                "Server is still loading the model. Please wait...");
            return;
        }

        std::string userInput = WxToUtf8(_userInputCtrl->GetValue());
        bool hasAttachments = m_attachments->HasPending();
        if (userInput.empty() && !hasAttachments) return;

        // ── Easter egg commands ───────────────────────────────────
        if (!hasAttachments && (userInput == "/yay!" || userInput == "/yay")) {
            _userInputCtrl->Clear();
            { wxCommandEvent e(wxEVT_TEXT, _userInputCtrl->GetId()); OnUserInputChanged(e); }
            m_chatDisplay->DisplaySystemMessage("* fireworks *");
            m_activeAnimation = std::make_unique<FireworksAnimation>();
            m_animTimer.Start(m_activeAnimation->GetIntervalMs());
            return;
        }

        if (userInput.empty() && hasAttachments) {
            bool onlyImages = m_attachments->HasImage() && !m_attachments->HasTextFile();
            if (onlyImages) {
                userInput = (m_attachments->GetCount() == 1)
                    ? "What is in this image?" : "What is in these images?";
            } else {
                userInput = (m_attachments->GetCount() == 1)
                    ? "Please review this file." : "Please review these files.";
            }
        }

        auto attachInfo = m_attachments->GetAttachmentInfo();

        if (m_attachments->HasImage()) {
            if (!m_chatHistory->HasFilePath())
                m_chatHistory->SetFilePath(ChatHistory::GenerateFilePath());

            std::string attachDir = ChatHistory::GetAttachmentDir(m_chatHistory->GetFilePath());
            std::string relDir = ChatHistory::GetAttachmentRelDir(m_chatHistory->GetFilePath());
            size_t msgIndex = m_chatHistory->GetMessageCount();
            m_attachments->SaveImagesToDisk(attachDir, relDir, msgIndex, attachInfo);
        }

        std::vector<std::string> imagePaths;
        if (m_attachments->HasImage()) {
            std::string convDir = ChatHistory::GetConversationsDir();
            for (const auto& info : attachInfo) {
                if (info.kind == AttachmentInfo::Kind::Image && !info.storagePath.empty())
                    imagePaths.push_back(convDir + "/" + info.storagePath);
            }
        }

        if (hasAttachments) {
            auto names = m_attachments->GetFileNames();
            std::string prefix;
            for (size_t i = 0; i < names.size(); ++i) {
                if (i > 0) prefix += ", ";
                prefix += names[i];
            }
            m_chatDisplay->DisplayUserMessage(
                "[" + prefix + "] " + userInput, "", imagePaths);
        } else {
            m_chatDisplay->DisplayUserMessage(userInput);
        }

        _userInputCtrl->Clear();
        { wxCommandEvent e(wxEVT_TEXT, _userInputCtrl->GetId()); OnUserInputChanged(e); }

        if (m_attachments->HasTextFile())
            userInput = m_attachments->BakeTextFilesIntoMessage(userInput);

        m_chatHistory->AddUserMessage(userInput, "", attachInfo);

        std::string model = m_appState->GetModel();
        std::string body = m_chatHistory->BuildChatRequestJson(model, true);

        if (m_attachments->HasImage())
            body = m_attachments->InjectImagesIntoRequest(body);

        m_attachments->Clear();

        if (auto* logger = m_appState->GetLogger())
            logger->debug("Request sent (" + std::to_string(body.size()) + " bytes)");

        m_chatHistory->AddAssistantPlaceholder(model);
        m_chatDisplay->DisplayAssistantPrefix(
            ServerManager::ModelDisplayName(model),
            m_appState->GetTheme().chatAssistant);

        ++m_generationId;
        m_chatState = ChatState::Streaming;
        SetStreamingState(true);

        if (!m_chatClient->SendMessage(model, m_appState->GetApiUrl(),
            body, m_generationId)) {
            SetStreamingState(false);
            m_chatDisplay->DisplaySystemMessage("Failed to start chat request");
            m_chatHistory->RemoveLastAssistantMessage();
        }
    }

    static std::string WxToUtf8(const wxString& s)
    {
        wxScopedCharBuffer buf = s.ToUTF8();
        if (!buf) return std::string();
        return std::string(buf.data());
    }
};

// ═══════════════════════════════════════════════════════════════════
//  ImageDropTarget Implementation
// ═══════════════════════════════════════════════════════════════════

bool ImageDropTarget::OnDropFiles(wxCoord /*x*/, wxCoord /*y*/,
    const wxArrayString& filenames)
{
    bool anyAttached = false;
    for (const auto& file : filenames) {
        std::string path(file.ToUTF8().data());
        if (AttachmentManager::IsImageFile(path)) {
            if (m_frame->AttachImageFromFile(path))
                anyAttached = true;
        }
        else if (AttachmentManager::IsTextFile(path)) {
            if (m_frame->AttachTextFile(path))
                anyAttached = true;
        }
    }
    return anyAttached;
}

// ═══════════════════════════════════════════════════════════════════
class MyApp : public wxApp {
public:
    bool OnInit() override {
        if (!wxApp::OnInit()) return false;
        // Set app name explicitly so wxStandardPaths::GetUserLocalDataDir()
        // always returns %LOCALAPPDATA%\LlamaBoss regardless of exe filename.
        SetAppName("LlamaBoss");
        SetAppDisplayName("LlamaBoss");
        wxInitAllImageHandlers();
        auto* frame = new MyFrame();
        frame->Show();
        return true;
    }
};

wxIMPLEMENT_APP(MyApp);
