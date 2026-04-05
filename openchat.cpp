// File: openchat.cpp (redesigned UI layout + image attach + drag-drop)
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
#include "message_router.h"
#include "action_classifier.h"
#include "workspace_executor.h"
#include "llm_classifier.h"

// ─── Application version ─────────────────────────────────────
static const char* OPENCHAT_VERSION = "1.1.1";

// Custom event for model picker popup
wxDEFINE_EVENT(wxEVT_MODEL_LIST_READY, wxCommandEvent);

// ─── Color constants removed ──────────────────────────────────────
// All colors now come from ThemeData via m_appState->GetTheme().
// See theme.h / theme.cpp for dark and light theme definitions.

// ─── Custom status dot panel (green/gray circle) ─────────────────
class StatusDot : public wxPanel {
public:
    StatusDot(wxWindow* parent, int size = 8)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(size, size))
        , m_connected(true), m_size(size)
        , m_connectedColor(94, 181, 247)
        , m_disconnectedColor(109, 127, 142)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &StatusDot::OnPaint, this);
    }
    void SetConnected(bool connected) {
        m_connected = connected;
        Refresh();
    }
    void SetColors(const wxColour& connected, const wxColour& disconnected) {
        m_connectedColor = connected;
        m_disconnectedColor = disconnected;
        Refresh();
    }
private:
    bool m_connected;
    int m_size;
    wxColour m_connectedColor;
    wxColour m_disconnectedColor;
    void OnPaint(wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
        dc.Clear();
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(m_connected ? m_connectedColor : m_disconnectedColor));
        dc.DrawCircle(m_size / 2, m_size / 2, m_size / 2 - 1);
    }
};

// ─── Image file extension check ──────────────────────────────────
static bool IsImageFile(const wxString& path)
{
    wxString ext = wxFileName(path).GetExt().Lower();
    return ext == "png" || ext == "jpg" || ext == "jpeg" ||
        ext == "gif" || ext == "bmp" || ext == "webp";
}

static bool IsTextFile(const wxString& path)
{
    wxString ext = wxFileName(path).GetExt().Lower();
    return ext == "txt" || ext == "md" || ext == "json" ||
        ext == "cpp" || ext == "h" || ext == "hpp" ||
        ext == "py" || ext == "js" || ext == "ts" ||
        ext == "css" || ext == "html" || ext == "xml" ||
        ext == "yaml" || ext == "yml" || ext == "toml" ||
        ext == "csv" || ext == "log" || ext == "ini" ||
        ext == "cfg" || ext == "sh" || ext == "bat";
}

static constexpr size_t kMaxTextFileBytes = 100 * 1024; // 100 KB cap

// ─── Custom input control with clipboard image paste support ─────
// On Windows, a wxTE_MULTILINE text control handles Ctrl+V natively
// at the WM_PASTE message level, before wxEVT_CHAR_HOOK can fire.
// This subclass intercepts WM_PASTE to check for clipboard images first.
class ChatInputCtrl : public wxTextCtrl {
public:
    ChatInputCtrl(wxWindow* parent, wxWindowID id,
        const wxString& value = wxEmptyString,
        const wxPoint& pos = wxDefaultPosition,
        const wxSize& size = wxDefaultSize,
        long style = 0)
        : wxTextCtrl(parent, id, value, pos, size, style)
    {}

    void SetImagePasteHandler(std::function<bool()> handler) {
        m_imagePasteHandler = handler;
    }

#ifdef __WXMSW__
protected:
    WXLRESULT MSWWindowProc(WXUINT nMsg, WXWPARAM wParam, WXLPARAM lParam) override {
        // Intercept WM_PASTE (0x0302) before the native edit control handles it.
        // If the clipboard contains an image, handle it and suppress text paste.
        if (nMsg == 0x0302 /* WM_PASTE */) {
            if (m_imagePasteHandler && m_imagePasteHandler())
                return 0;
        }
        return wxTextCtrl::MSWWindowProc(nMsg, wParam, lParam);
    }
#endif

private:
    std::function<bool()> m_imagePasteHandler;
};

// ─── Custom display control with fast drag-scroll ─────────────────
// wxRichTextCtrl's built-in auto-scroll during drag-select is extremely
// slow.  This subclass uses a timer to scroll faster when the mouse is
// dragged above or below the visible area, scaling speed with distance.
class ChatDisplayCtrl : public wxRichTextCtrl {
public:
    ChatDisplayCtrl(wxWindow* parent, wxWindowID id,
        const wxString& value = wxEmptyString,
        const wxPoint& pos = wxDefaultPosition,
        const wxSize& size = wxDefaultSize,
        long style = 0)
        : wxRichTextCtrl(parent, id, value, pos, size, style)
        , m_autoScrollTimer(this)
        , m_scrollDirection(0)
        , m_scrollIntensity(0)
        , m_inAutoScroll(false)
    {
        Bind(wxEVT_MOTION, &ChatDisplayCtrl::OnDragMotion, this);
        Bind(wxEVT_LEFT_UP, &ChatDisplayCtrl::OnDragEnd, this);
        Bind(wxEVT_MOUSE_CAPTURE_LOST, &ChatDisplayCtrl::OnCaptureLost, this);
        Bind(wxEVT_TIMER, &ChatDisplayCtrl::OnAutoScrollTimer, this,
            m_autoScrollTimer.GetId());
    }

private:
    wxTimer m_autoScrollTimer;
    int m_scrollDirection;    // -1 = up, +1 = down, 0 = idle
    int m_scrollIntensity;    // lines per tick, scales with distance from edge
    bool m_inAutoScroll;      // guard against re-entry from synthetic events

    void OnDragMotion(wxMouseEvent& evt) {
        evt.Skip();  // always let base class handle selection
        if (m_inAutoScroll) return;

        if (!evt.Dragging() || !evt.LeftIsDown()) {
            StopAutoScroll();
            return;
        }

        int y = evt.GetPosition().y;
        int h = GetClientSize().y;

        if (y < 0) {
            m_scrollDirection = -1;
            m_scrollIntensity = std::min(std::max((-y) / 15 + 1, 1), 12);
            if (!m_autoScrollTimer.IsRunning())
                m_autoScrollTimer.Start(30);
        }
        else if (y > h) {
            m_scrollDirection = 1;
            m_scrollIntensity = std::min(std::max((y - h) / 15 + 1, 1), 12);
            if (!m_autoScrollTimer.IsRunning())
                m_autoScrollTimer.Start(30);
        }
        else {
            StopAutoScroll();
        }
    }

    void OnDragEnd(wxMouseEvent& evt) {
        StopAutoScroll();
        evt.Skip();
    }

    void OnCaptureLost(wxMouseCaptureLostEvent&) {
        StopAutoScroll();
    }

    void OnAutoScrollTimer(wxTimerEvent&) {
        if (m_scrollDirection == 0) return;

        ScrollLines(m_scrollDirection * m_scrollIntensity);

        // Synthesize a mouse-move at the visible edge so the base class
        // extends the selection to match the new scroll position.
        m_inAutoScroll = true;
        wxMouseEvent fake(wxEVT_MOTION);
        fake.SetLeftDown(true);
        fake.SetX(GetClientSize().x / 2);
        fake.SetY(m_scrollDirection < 0 ? 0 : GetClientSize().y - 1);
        fake.SetEventObject(this);
        HandleWindowEvent(fake);
        m_inAutoScroll = false;
    }

    void StopAutoScroll() {
        if (m_autoScrollTimer.IsRunning())
            m_autoScrollTimer.Stop();
        m_scrollDirection = 0;
        m_scrollIntensity = 0;
    }
};


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

// ─── Thread for quick model list fetch (model pill picker) ───
class ModelPickerThread : public wxThread {
public:
    // [STEP 1] Now takes a weak liveness token from the frame
    ModelPickerThread(wxEvtHandler* handler, const std::string& apiUrl,
        std::weak_ptr<std::atomic<bool>> aliveToken)
        : wxThread(wxTHREAD_DETACHED)
        , m_handler(handler)
        , m_apiUrl(apiUrl)
        , m_aliveToken(aliveToken) {
    }
protected:
    ExitCode Entry() override {
        try {
            Poco::URI uri(m_apiUrl + "/api/tags");
            Poco::Net::HTTPClientSession sess(uri.getHost(), uri.getPort());
            sess.setTimeout(Poco::Timespan(5, 0));

            Poco::Net::HTTPRequest req(Poco::Net::HTTPRequest::HTTP_GET,
                uri.getPathAndQuery(), Poco::Net::HTTPMessage::HTTP_1_1);
            sess.sendRequest(req);

            Poco::Net::HTTPResponse resp;
            std::istream& in = sess.receiveResponse(resp);

            if (resp.getStatus() != Poco::Net::HTTPResponse::HTTP_OK) {
                SafePost(new wxCommandEvent(wxEVT_MODEL_LIST_READY));
                return (ExitCode)0;
            }

            std::string body;
            Poco::StreamCopier::copyToString(in, body);

            Poco::JSON::Parser parser;
            auto result = parser.parse(body);
            auto obj = result.extract<Poco::JSON::Object::Ptr>();

            wxString modelList;
            if (obj->has("models")) {
                auto arr = obj->getArray("models");
                for (size_t i = 0; i < arr->size(); ++i) {
                    auto m = arr->getObject(i);
                    if (m->has("name")) {
                        if (!modelList.empty()) modelList += "\n";
                        modelList += wxString::FromUTF8(
                            m->getValue<std::string>("name"));
                    }
                }
            }

            auto* evt = new wxCommandEvent(wxEVT_MODEL_LIST_READY);
            evt->SetString(modelList);
            SafePost(evt);
        }
        catch (...) {
            SafePost(new wxCommandEvent(wxEVT_MODEL_LIST_READY));
        }
        return (ExitCode)0;
    }
private:
    wxEvtHandler* m_handler;
    std::string m_apiUrl;
    std::weak_ptr<std::atomic<bool>> m_aliveToken;     // [STEP 1]

    // [STEP 1] Post event only if owner frame is still alive
    void SafePost(wxCommandEvent* evt) {
        auto alive = m_aliveToken.lock();
        if (alive && alive->load()) {
            wxQueueEvent(m_handler, evt);
        }
        else {
            delete evt;
        }
    }
};

// ═══════════════════════════════════════════════════════════════════
//  Chat State Machine for single-model and group chat flows
// ═══════════════════════════════════════════════════════════════════
enum class ChatState {
    Idle,              // Ready for user input
    SingleStreaming,   // Normal single-model streaming
    GroupModelA,       // Model A is streaming its response (will chain to B)
    GroupModelB,       // Model B is streaming its response (chained from A)
    GroupDirected,     // One specific model streaming in group mode (no chain)
};

enum class TurnRoute {
    GroupAll,
    DirectModelA,
    DirectModelB
};

// ═══════════════════════════════════════════════════════════════════
class MyFrame : public wxFrame {
public:
    MyFrame()
        : wxFrame(nullptr, wxID_ANY, "Ollama Chat",
            wxDefaultPosition, wxSize(1100, 700),
            wxDEFAULT_FRAME_STYLE)
        , m_appState(new AppState())
        , m_alive(std::make_shared<std::atomic<bool>>(true))  // [STEP 1]
        , m_chatClient(new ChatClient(this, m_alive))          // [STEP 1] pass token
        , m_chatDisplay(nullptr)
        , m_chatHistory(new ChatHistory())
        , m_sidebar(nullptr)
        , m_isClosing(false)
        , m_generationId(0)                                    // [STEP 2]
        , m_chatState(ChatState::Idle)
    {
        // Initialize application state first
        if (!m_appState->Initialize()) {
            wxMessageBox("Failed to initialize application state", "Startup Error",
                wxOK | wxICON_ERROR);
        }

        SetBackgroundColour(m_appState->GetTheme().bgMain);

        auto* mainSizer = new wxBoxSizer(wxVERTICAL);

        // ─── TOP BAR ─────────────────────────────────────────────────
        BuildTopBar(mainSizer);

        // ─── CONTENT AREA (sidebar + chat) ────────────────────────────
        _contentSizer = new wxBoxSizer(wxHORIZONTAL);

        // ── Sidebar (collapsible conversation list) ──
        ConversationSidebar::Callbacks sidebarCallbacks;
        sidebarCallbacks.onConversationClicked = [this](const std::string& path) {
            LoadConversationFromPath(path);
        };
        sidebarCallbacks.onNewChatClicked = [this]() {
            wxCommandEvent e;
            OnNewChat(e);
        };
        sidebarCallbacks.onDeleteRequested = [this](const std::vector<std::string>& paths) {
            DeleteConversations(paths);
        };
        m_sidebar = new ConversationSidebar(this, m_appState->GetTheme(),
                                            sidebarCallbacks);
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

        // ─── ATTACHMENT INDICATOR (hidden by default) ────────────────
        _attachLabel = new wxStaticText(_rightPanel, wxID_ANY, "");
        { wxFont f = _attachLabel->GetFont(); f.SetPointSize(11); _attachLabel->SetFont(f); }
        _attachLabel->SetForegroundColour(m_appState->GetTheme().attachIndicator);
        _attachLabel->SetBackgroundColour(m_appState->GetTheme().bgMain);
        _attachLabel->Hide();
        rightSizer->Add(_attachLabel, 0, wxLEFT | wxRIGHT | wxTOP, 12);

        // ─── INPUT AREA ──────────────────────────────────────────────
        BuildInputArea(rightSizer, _rightPanel);

        _rightPanel->SetSizer(rightSizer);
        _contentSizer->Add(_rightPanel, 1, wxEXPAND);

        mainSizer->Add(_contentSizer, 1, wxEXPAND);

        SetSizer(mainSizer);

        // ─── Setup fonts ─────────────────────────────────────────────
        wxFont codeFont = m_appState->CreateMonospaceFont(14);
        _chatDisplayCtrl->SetFont(codeFont);
        _userInputCtrl->SetFont(codeFont);

        m_chatDisplay = new ChatDisplay(_chatDisplayCtrl);
        m_chatDisplay->SetFont(codeFont);
        m_chatDisplay->ApplyTheme(m_appState->GetTheme());

        // ─── Apply theme colors to StatusDot ─────────────────────────
        _statusDot->SetColors(m_appState->GetTheme().accentButton,
                              m_appState->GetTheme().textMuted);

        // ─── Bind events ─────────────────────────────────────────────
        _sendButton->Bind(wxEVT_BUTTON, &MyFrame::OnSendMessage, this);
        _stopButton->Bind(wxEVT_BUTTON, &MyFrame::OnStopGeneration, this);
        _attachButton->Bind(wxEVT_BUTTON, &MyFrame::OnAttachImage, this);
        _userInputCtrl->Bind(wxEVT_TEXT_ENTER, &MyFrame::OnSendMessage, this);
        _userInputCtrl->Bind(wxEVT_TEXT, &MyFrame::OnUserInputChanged, this);
        _settingsButton->Bind(wxEVT_BUTTON, &MyFrame::OnOpenSettings, this);
        _newChatButton->Bind(wxEVT_BUTTON, &MyFrame::OnNewChat, this);
        _sidebarToggle->Bind(wxEVT_BUTTON, &MyFrame::OnToggleSidebar, this);
        Bind(wxEVT_ACTIVATE, &MyFrame::OnFrameActivate, this);

        Bind(wxEVT_ASSISTANT_DELTA, &MyFrame::OnAssistantDelta, this);
        Bind(wxEVT_ASSISTANT_COMPLETE, &MyFrame::OnAssistantComplete, this);
        Bind(wxEVT_ASSISTANT_ERROR, &MyFrame::OnAssistantError, this);

        // ─── Model pill click (quick model switch) ────────────────────
        _modelPill->Bind(wxEVT_LEFT_UP, &MyFrame::OnModelPillClick, this);
        _modelLabel->Bind(wxEVT_LEFT_UP, &MyFrame::OnModelPillClick, this);
        _statusDot->Bind(wxEVT_LEFT_UP, &MyFrame::OnModelPillClick, this);
        _groupToggleButton->Bind(wxEVT_BUTTON, &MyFrame::OnGroupToggleClick, this);
        Bind(wxEVT_MODEL_LIST_READY, &MyFrame::OnModelListReady, this);

        // ─── Drag-and-drop support ────────────────────────────────────
        SetDropTarget(new ImageDropTarget(this));

        // ─── Clipboard image paste (via WM_PASTE interception) ──────
        // On Windows, Ctrl+V is handled natively by the multiline edit
        // control at the WM_PASTE message level, before wxEVT_CHAR_HOOK
        // can fire. ChatInputCtrl::MSWWindowProc intercepts WM_PASTE and
        // checks for a clipboard image before the native paste runs.
        _userInputCtrl->SetImagePasteHandler([this]() -> bool {
            if (IsBusy()) return false;
            return TryPasteImageFromClipboard();
        });

        // ─── Keyboard shortcuts (Ctrl+N/S/O — frame level) ──────────
        Bind(wxEVT_CHAR_HOOK, &MyFrame::OnCharHook, this);

        // ─── Load icon and update model display ──────────────────────
        m_appState->LoadApplicationIcon(this);
        UpdateModelLabel();

        // ─── Restore window position/size ─────────────────────────────
        m_appState->RestoreWindowState(this);

        // ─── Save state on close ──────────────────────────────────────
        Bind(wxEVT_CLOSE_WINDOW, &MyFrame::OnClose, this);

        // ─── Final setup ─────────────────────────────────────────────
        CallAfter([this]() {
            _userInputCtrl->SetFocus();
            wxCommandEvent anEvent(wxEVT_TEXT, _userInputCtrl->GetId());
            OnUserInputChanged(anEvent);

            });
    }

    ~MyFrame() override
    {
        delete m_chatClient;
        delete m_chatDisplay;
        delete m_chatHistory;
        delete m_sidebar;
        delete m_appState;
    }

    void OnClose(wxCloseEvent& evt)
    {
        // [STEP 1] Kill the liveness token FIRST — this prevents any
        // detached thread (ChatWorkerThread, ModelPickerThread) from
        // posting events to this frame after this point.
        m_alive->store(false);

        m_isClosing = true;

        if (m_chatClient->IsStreaming()) {
            m_chatClient->StopGeneration();
        }

        if (!m_chatHistory->IsEmpty() && !m_chatHistory->HasFilePath()) {
            AutoSaveConversation();
        }

        m_appState->SaveWindowState(this);

        evt.Skip(); // Allow the default close behavior
    }

    // ── Public interface for image attachment (used by drop target) ──
    bool AttachImageFromFile(const std::string& filePath)
    {
        if (IsBusy()) return false;

        wxFileName fname(wxString::FromUTF8(filePath));
        if (!fname.FileExists()) return false;

        std::string base64 = FileToBase64(filePath);
        if (base64.empty()) return false;

        // Clear any pending text file (one attachment at a time)
        m_pendingFileText.clear();
        m_pendingFileName.clear();

        m_pendingImageBase64 = base64;
        m_pendingImageName = fname.GetFullName().ToStdString();

        _attachLabel->SetLabel(wxString::FromUTF8(
            "  \xF0\x9F\x96\xBC  " + m_pendingImageName));
        _attachLabel->Show();
        GetSizer()->Layout();
        _userInputCtrl->SetFocus();

        if (auto* logger = m_appState->GetLogger()) {
            logger->information("Image attached: " + m_pendingImageName +
                " (" + std::to_string(base64.size()) + " bytes base64)");
        }

        return true;
    }

    // ── Public interface for text-file attachment (used by drop target) ──
    bool AttachTextFile(const std::string& filePath)
    {
        if (IsBusy()) return false;

        wxFileName fname(wxString::FromUTF8(filePath));
        if (!fname.FileExists()) return false;

        // Size guard
        wxULongLong fileSize = fname.GetSize();
        if (fileSize == wxInvalidSize || fileSize.GetValue() > kMaxTextFileBytes) {
            wxMessageBox("Text file too large (max 100 KB).",
                "Attachment Error", wxOK | wxICON_WARNING);
            return false;
        }

        std::ifstream ifs(filePath, std::ios::in);
        if (!ifs.is_open()) return false;

        std::ostringstream oss;
        oss << ifs.rdbuf();
        std::string content = oss.str();
        if (content.empty()) return false;

        // Clear any pending image (one attachment at a time)
        m_pendingImageBase64.clear();
        m_pendingImageName.clear();

        m_pendingFileText = std::move(content);
        m_pendingFileName = fname.GetFullName().ToStdString();

        // 📄 U+1F4C4
        _attachLabel->SetLabel(wxString::FromUTF8(
            "  \xF0\x9F\x93\x84  " + m_pendingFileName));
        _attachLabel->Show();
        GetSizer()->Layout();
        _userInputCtrl->SetFocus();

        if (auto* logger = m_appState->GetLogger()) {
            logger->information("Text file attached: " + m_pendingFileName +
                " (" + std::to_string(m_pendingFileText.size()) + " bytes)");
        }

        return true;
    }

private:
    // ─── UI Controls ──────────────────────────────────────────────
    ChatDisplayCtrl* _chatDisplayCtrl;
    ChatInputCtrl* _userInputCtrl;
    wxButton* _sendButton;
    wxButton* _stopButton;
    wxButton* _attachButton;
    wxButton* _settingsButton;
    wxButton* _newChatButton;
    wxButton* _sidebarToggle;
    wxButton* _aboutButton;
    wxStaticText* _attachLabel;
    wxBoxSizer* _inputSizer;
    wxBoxSizer* _contentSizer;

    // Themed panels (need pointers for live theme switching)
    wxPanel* _toolbarPanel;
    wxStaticText* _titleLabel;
    wxPanel* _modelPill;
    wxPanel* _topSeparator;
    wxPanel* _rightPanel;
    wxPanel* _inputContainer;
    wxPanel* _inputSeparator;

    // Sidebar component
    ConversationSidebar* m_sidebar;
    bool m_isClosing;

    // Top bar controls
    wxStaticText* _modelLabel;
    StatusDot* _statusDot;
    wxButton* _groupToggleButton;

    // ─── Thread safety ────────────────────────────────────────────
    // [STEP 1] Shared liveness token — threads hold a weak_ptr to this.
    // Set to false in OnClose() before anything is destroyed, so all
    // detached threads know to stop posting events to this frame.
    std::shared_ptr<std::atomic<bool>> m_alive;

    // [STEP 2] Monotonically increasing generation ID — incremented
    // every time a new request starts or the current one is stopped.
    // Stamped on every event by the worker thread; handlers discard
    // events whose ID doesn't match the current generation.
    uint64_t m_generationId;

    // ─── Application Components ───────────────────────────────────
    AppState* m_appState;
    ChatClient* m_chatClient;
    ChatDisplay* m_chatDisplay;
    ChatHistory* m_chatHistory;

    // ─── Pending image attachment ─────────────────────────────────
    std::string m_pendingImageBase64;
    std::string m_pendingImageName;

    // ─── Pending text-file attachment ─────────────────────────────
    std::string m_pendingFileText;
    std::string m_pendingFileName;

    // ─── Model picker state ──────────────────────────────────────
    std::vector<std::string> m_pickerModels;
    bool m_pickerIsForGroupB = false;  // True when picker is inviting Model B

    // ─── Chat state machine ──────────────────────────────────────
    ChatState m_chatState;
    TurnRoute m_currentTurnRoute = TurnRoute::GroupAll;

    // ─── Group chat state ────────────────────────────────────────
    std::string m_groupModelB;  // Empty = single mode, non-empty = group mode active
    std::string m_directedTarget;  // Model being addressed in a GroupDirected turn



    // ═════════════════════════════════════════════════════════════
    //  UI CONSTRUCTION
    // ═════════════════════════════════════════════════════════════

    void BuildTopBar(wxBoxSizer* mainSizer)
    {
        _toolbarPanel = new wxPanel(this, wxID_ANY);
        _toolbarPanel->SetBackgroundColour(m_appState->GetTheme().bgToolbar);
        auto* sizer = new wxBoxSizer(wxHORIZONTAL);

        // ── Left: Sidebar toggle + App title ──
        wxString hamburger = wxString::FromUTF8("\xE2\x98\xB0"); // ☰
        _sidebarToggle = new wxButton(_toolbarPanel, wxID_ANY, hamburger,
            wxDefaultPosition, wxSize(52, 44), wxBORDER_NONE);
        _sidebarToggle->SetBackgroundColour(m_appState->GetTheme().bgToolbar);
        _sidebarToggle->SetForegroundColour(m_appState->GetTheme().textMuted);
        _sidebarToggle->SetToolTip("Toggle sidebar");
        wxFont hamburgerFont = _sidebarToggle->GetFont();
        hamburgerFont.SetPointSize(18);
        _sidebarToggle->SetFont(hamburgerFont);
        sizer->Add(_sidebarToggle, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 4);

        _titleLabel = new wxStaticText(_toolbarPanel, wxID_ANY, "Ollama Chat");
        _titleLabel->SetForegroundColour(m_appState->GetTheme().textPrimary);
        wxFont titleFont = _titleLabel->GetFont();
        titleFont.SetPointSize(15);
        titleFont.SetWeight(wxFONTWEIGHT_BOLD);
        _titleLabel->SetFont(titleFont);
        sizer->Add(_titleLabel, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 4);

        sizer->AddStretchSpacer(1);

        // ── Center: Model pill [dot + model name] (clickable) ──
        _modelPill = new wxPanel(_toolbarPanel, wxID_ANY);
        _modelPill->SetBackgroundColour(m_appState->GetTheme().bgToolbar);
        _modelPill->SetCursor(wxCURSOR_HAND);
        auto* pillSizer = new wxBoxSizer(wxHORIZONTAL);

        _statusDot = new StatusDot(_modelPill, 10);
        _statusDot->SetCursor(wxCURSOR_HAND);
        pillSizer->Add(_statusDot, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 10);

        _modelLabel = new wxStaticText(_modelPill, wxID_ANY, "loading...");
        _modelLabel->SetForegroundColour(m_appState->GetTheme().textPrimary);
        _modelLabel->SetCursor(wxCURSOR_HAND);
        wxFont modelFont = _modelLabel->GetFont();
        modelFont.SetPointSize(13);
        modelFont.SetWeight(wxFONTWEIGHT_BOLD);
        _modelLabel->SetFont(modelFont);
        pillSizer->Add(_modelLabel, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 6);
        pillSizer->AddSpacer(10);

        _modelPill->SetSizer(pillSizer);
        sizer->Add(_modelPill, 0, wxALIGN_CENTER_VERTICAL);

        // ── Group chat toggle button (+ to invite, × to remove) ──
        _groupToggleButton = new wxButton(_toolbarPanel, wxID_ANY, "+",
            wxDefaultPosition, wxSize(36, 32), wxBORDER_NONE);
        _groupToggleButton->SetBackgroundColour(m_appState->GetTheme().bgToolbar);
        _groupToggleButton->SetForegroundColour(m_appState->GetTheme().accentButton);
        _groupToggleButton->SetToolTip("Invite a second model to the conversation");
        wxFont groupFont = _groupToggleButton->GetFont();
        groupFont.SetPointSize(16);
        groupFont.SetWeight(wxFONTWEIGHT_BOLD);
        _groupToggleButton->SetFont(groupFont);
        _groupToggleButton->SetCursor(wxCURSOR_HAND);
        sizer->Add(_groupToggleButton, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 2);

        sizer->AddStretchSpacer(1);

        // ── Right: New Chat button ──
        _newChatButton = new wxButton(_toolbarPanel, wxID_ANY, "+",
            wxDefaultPosition, wxSize(48, 44), wxBORDER_NONE);
        _newChatButton->SetBackgroundColour(m_appState->GetTheme().bgToolbar);
        _newChatButton->SetForegroundColour(m_appState->GetTheme().textMuted);
        _newChatButton->SetToolTip("New Chat (Ctrl+N)");
        wxFont newChatFont = _newChatButton->GetFont();
        newChatFont.SetPointSize(22);
        _newChatButton->SetFont(newChatFont);
        sizer->Add(_newChatButton, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);

        // ── Right: Settings gear ──
        wxString gear = wxString::FromUTF8("\xE2\x9A\x99\xEF\xB8\x8F");
        _settingsButton = new wxButton(_toolbarPanel, wxID_ANY, gear,
            wxDefaultPosition, wxSize(52, 44), wxBORDER_NONE);
        _settingsButton->SetBackgroundColour(m_appState->GetTheme().bgToolbar);
        _settingsButton->SetForegroundColour(m_appState->GetTheme().textMuted);
        _settingsButton->SetToolTip("Settings");
        wxFont gearFont = _settingsButton->GetFont();
        gearFont.SetPointSize(18);
        _settingsButton->SetFont(gearFont);
        sizer->Add(_settingsButton, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 2);

        // ── Right: About info ──
        wxString infoChar = wxString::FromUTF8("\xE2\x93\x98"); // ⓘ
        _aboutButton = new wxButton(_toolbarPanel, wxID_ANY, infoChar,
            wxDefaultPosition, wxSize(48, 44), wxBORDER_NONE);
        _aboutButton->SetBackgroundColour(m_appState->GetTheme().bgToolbar);
        _aboutButton->SetForegroundColour(m_appState->GetTheme().textMuted);
        _aboutButton->SetToolTip("About OpenChat");
        wxFont aboutFont = _aboutButton->GetFont();
        aboutFont.SetPointSize(18);
        _aboutButton->SetFont(aboutFont);
        _aboutButton->Bind(wxEVT_BUTTON, &MyFrame::OnAbout, this);
        sizer->Add(_aboutButton, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

        _toolbarPanel->SetSizer(sizer);
        mainSizer->Add(_toolbarPanel, 0, wxEXPAND);

        // Separator line
        _topSeparator = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
        _topSeparator->SetBackgroundColour(m_appState->GetTheme().borderSubtle);
        mainSizer->Add(_topSeparator, 0, wxEXPAND);
    }

    void BuildInputArea(wxBoxSizer* mainSizer, wxWindow* parent)
    {
        _inputContainer = new wxPanel(parent, wxID_ANY);
        _inputContainer->SetBackgroundColour(m_appState->GetTheme().bgInputArea);
        auto* outerSizer = new wxBoxSizer(wxVERTICAL);

        // Separator line above input
        _inputSeparator = new wxPanel(_inputContainer, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
        _inputSeparator->SetBackgroundColour(m_appState->GetTheme().borderSubtle);
        outerSizer->Add(_inputSeparator, 0, wxEXPAND);

        // Input row: [📎] [TextInput] [Send/Stop]
        _inputSizer = new wxBoxSizer(wxHORIZONTAL);

        // Attach button
        wxString clip = wxString::FromUTF8("\xF0\x9F\x93\x8E");
        _attachButton = new wxButton(_inputContainer, wxID_ANY, clip,
            wxDefaultPosition, wxSize(52, 42), wxBORDER_NONE);
        _attachButton->SetBackgroundColour(m_appState->GetTheme().bgInputArea);
        _attachButton->SetForegroundColour(m_appState->GetTheme().textMuted);
        _attachButton->SetToolTip("Attach an image");
        wxFont clipFont = _attachButton->GetFont();
        clipFont.SetPointSize(18);
        _attachButton->SetFont(clipFont);
        _inputSizer->Add(_attachButton, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 6);

        // Text input field (ChatInputCtrl intercepts WM_PASTE for image clipboard support)
        _userInputCtrl = new ChatInputCtrl(_inputContainer, wxID_ANY, wxEmptyString,
            wxDefaultPosition, wxDefaultSize,
            wxTE_PROCESS_ENTER | wxTE_MULTILINE | wxBORDER_NONE);
        _userInputCtrl->SetBackgroundColour(m_appState->GetTheme().bgInputField);
        _userInputCtrl->SetForegroundColour(m_appState->GetTheme().textPrimary);
        _userInputCtrl->SetHint("Message...");
        _inputSizer->Add(_userInputCtrl, 1, wxEXPAND | wxTOP | wxBOTTOM, 6);

        // Send button — the primary action
        _sendButton = new wxButton(_inputContainer, wxID_ANY, "Send",
            wxDefaultPosition, wxSize(76, 36), wxBORDER_NONE);
        _sendButton->SetBackgroundColour(m_appState->GetTheme().accentButton);
        _sendButton->SetForegroundColour(m_appState->GetTheme().accentButtonText);
        wxFont btnFont = _sendButton->GetFont();
        btnFont.SetPointSize(11);
        btnFont.SetWeight(wxFONTWEIGHT_MEDIUM);
        _sendButton->SetFont(btnFont);
        _inputSizer->Add(_sendButton, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 6);

        // Stop button — red, hidden by default, replaces Send
        _stopButton = new wxButton(_inputContainer, wxID_ANY, "Stop",
            wxDefaultPosition, wxSize(76, 36), wxBORDER_NONE);
        _stopButton->SetBackgroundColour(m_appState->GetTheme().stopButton);
        _stopButton->SetForegroundColour(m_appState->GetTheme().stopButtonText);
        _stopButton->SetFont(btnFont);
        _stopButton->Hide();
        _inputSizer->Add(_stopButton, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);

        outerSizer->Add(_inputSizer, 0, wxEXPAND | wxALL, 4);
        _inputContainer->SetSizer(outerSizer);
        mainSizer->Add(_inputContainer, 0, wxEXPAND);
    }

    // ═════════════════════════════════════════════════════════════
    //  HELPERS
    // ═════════════════════════════════════════════════════════════

    void ApplyThemeToUI()
    {
        const ThemeData& t = m_appState->GetTheme();

        // ── Frame ────────────────────────────────────────────────
        SetBackgroundColour(t.bgMain);

        // ── Toolbar ──────────────────────────────────────────────
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

        // Group toggle button
        _groupToggleButton->SetBackgroundColour(t.bgToolbar);
        UpdateGroupToggleButton();  // Sets foreground color based on state

        // ── Sidebar ──────────────────────────────────────────────
        if (m_sidebar) {
            m_sidebar->ApplyTheme(t);
        }

        // ── Chat area ────────────────────────────────────────────
        _rightPanel->SetBackgroundColour(t.bgMain);
        _chatDisplayCtrl->SetBackgroundColour(t.bgMain);
        _chatDisplayCtrl->SetForegroundColour(t.textPrimary);
        _attachLabel->SetForegroundColour(t.attachIndicator);
        _attachLabel->SetBackgroundColour(t.bgMain);

        // ── Input area ───────────────────────────────────────────
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

        // ── ChatDisplay + MarkdownRenderer ───────────────────────
        if (m_chatDisplay) {
            m_chatDisplay->ApplyTheme(t);
        }

        // ── Refresh sidebar if visible ───────────────────────────
        if (m_sidebar && m_sidebar->IsVisible()) {
            m_sidebar->Refresh(m_chatHistory->GetFilePath());
        }

        // ── Force repaint ────────────────────────────────────────
        Refresh();
        Update();
    }

    void UpdateModelLabel()
    {
        std::string model = m_appState->GetModel();

        // Shorten: "pidrilkin/gemma3_27b_abliterated:Q4_K_M" -> "gemma3_27b_abliterated:Q4_K_M"
        auto shortenModel = [](const std::string& m) -> std::string {
            size_t slash = m.rfind('/');
            if (slash != std::string::npos && slash + 1 < m.size())
                return m.substr(slash + 1);
            return m;
        };

        std::string display = shortenModel(model);

        // In group mode, show both models
        if (IsGroupMode()) {
            display += " + " + shortenModel(m_groupModelB);
        }

        _modelLabel->SetLabel(wxString::FromUTF8(display) +
            wxString::FromUTF8(" \xE2\x96\xBE"));  // dropdown indicator
        _modelLabel->GetParent()->Layout();
    }

    std::string FileToBase64(const std::string& filePath)
    {
        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open()) return "";

        std::ostringstream base64Stream;
        Poco::Base64Encoder encoder(base64Stream);
        encoder << file.rdbuf();
        encoder.close();

        std::string result = base64Stream.str();
        result.erase(std::remove_if(result.begin(), result.end(), ::isspace), result.end());
        return result;
    }

    std::string InjectImageIntoRequest(const std::string& requestJson,
        const std::string& base64Image)
    {
        try {
            Poco::JSON::Parser parser;
            auto root = parser.parse(requestJson).extract<Poco::JSON::Object::Ptr>();
            auto messages = root->getArray("messages");

            if (messages && messages->size() > 0) {
                for (int i = (int)messages->size() - 1; i >= 0; --i) {
                    auto msg = messages->getObject(i);
                    if (msg->getValue<std::string>("role") == "user") {
                        Poco::JSON::Array::Ptr images = new Poco::JSON::Array;
                        images->add(base64Image);
                        msg->set("images", images);
                        break;
                    }
                }
            }

            std::ostringstream oss;
            Poco::JSON::Stringifier::stringify(root, oss);
            return oss.str();
        }
        catch (const Poco::Exception& ex) {
            if (auto* logger = m_appState->GetLogger()) {
                logger->error("Failed to inject image: " + ex.displayText());
            }
            return requestJson;
        }
    }

    std::string InjectTextFileIntoRequest(const std::string& requestJson,
        const std::string& fileName, const std::string& fileText)
    {
        try {
            Poco::JSON::Parser parser;
            auto root = parser.parse(requestJson).extract<Poco::JSON::Object::Ptr>();
            auto messages = root->getArray("messages");

            if (messages && messages->size() > 0) {
                for (int i = (int)messages->size() - 1; i >= 0; --i) {
                    auto msg = messages->getObject(i);
                    if (msg->getValue<std::string>("role") == "user") {
                        std::string original = msg->getValue<std::string>("content");
                        std::string combined =
                            "[File: " + fileName + "]\n"
                            "```\n" + fileText + "\n```\n\n"
                            + original;
                        msg->set("content", combined);
                        break;
                    }
                }
            }

            std::ostringstream oss;
            Poco::JSON::Stringifier::stringify(root, oss);
            return oss.str();
        }
        catch (const Poco::Exception& ex) {
            if (auto* logger = m_appState->GetLogger()) {
                logger->error("Failed to inject text file: " + ex.displayText());
            }
            return requestJson;
        }
    }

    void ClearPendingAttachment()
    {
        m_pendingImageBase64.clear();
        m_pendingImageName.clear();
        m_pendingFileText.clear();
        m_pendingFileName.clear();
        _attachLabel->SetLabel("");
        _attachLabel->Hide();
        GetSizer()->Layout();
    }

    void SetStreamingState(bool streaming)
    {
        _sendButton->Show(!streaming);
        _stopButton->Show(streaming);
        _userInputCtrl->Enable(!streaming);
        _attachButton->Enable(!streaming);
        _settingsButton->Enable(!streaming);
        _newChatButton->Enable(!streaming);
        _groupToggleButton->Enable(!streaming);
        _inputSizer->Layout();

        if (!streaming) {
            m_chatState = ChatState::Idle;
            _userInputCtrl->SetFocus();
        }
    }

    // Convenience: is the chat state machine busy (any model streaming)?
    bool IsBusy() const { return m_chatState != ChatState::Idle; }

    // Is group chat mode active (Model B assigned)?
    bool IsGroupMode() const { return !m_groupModelB.empty(); }

    // Get the accent color for a given model name
    wxColour GetModelAccentColor(const std::string& modelName) const
    {
        if (IsGroupMode() && modelName == m_groupModelB) {
            return m_appState->GetTheme().chatAssistantB;
        }
        return m_appState->GetTheme().chatAssistant;
    }

    // Build the models vector for file persistence
    std::vector<std::string> GetActiveModels() const
    {
        std::vector<std::string> models = { m_appState->GetModel() };
        if (IsGroupMode()) models.push_back(m_groupModelB);
        return models;
    }

    static std::string ToLowerAscii(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    static std::string TrimLeftAscii(const std::string& s)
    {
        size_t i = 0;
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
            ++i;
        }
        return s.substr(i);
    }

    static std::vector<std::string> BuildAddressTokens(const std::string& modelName)
    {
        std::vector<std::string> tokens;

        auto addUnique = [&](std::string token) {
            token = ToLowerAscii(token);
            if (token.empty()) return;
            if (std::find(tokens.begin(), tokens.end(), token) == tokens.end()) {
                tokens.push_back(token);
            }
            };

        addUnique(modelName);

        size_t colon = modelName.find(':');
        if (colon != std::string::npos) {
            addUnique(modelName.substr(0, colon));
        }

        // Friendly alias: leading letters only (e.g. "gemma" from "gemma4:..."
        // and "qwen" from "qwen3.5:latest")
        std::string lettersOnly;
        for (char ch : modelName) {
            unsigned char c = static_cast<unsigned char>(ch);
            if (std::isalpha(c)) {
                lettersOnly.push_back(static_cast<char>(std::tolower(c)));
            }
            else if (!lettersOnly.empty()) {
                break;
            }
        }
        addUnique(lettersOnly);

        return tokens;
    }

    static bool StartsWithAddressToken(const std::string& rawText, const std::string& token)
    {
        if (token.empty()) return false;

        std::string s = ToLowerAscii(TrimLeftAscii(rawText));
        if (!s.empty() && s[0] == '@') {
            s.erase(0, 1);
        }

        if (s.size() < token.size()) return false;
        if (s.compare(0, token.size(), token) != 0) return false;

        if (s.size() == token.size()) return true;

        unsigned char next = static_cast<unsigned char>(s[token.size()]);
        return std::isspace(next) || next == ':' || next == ',' || next == '.' ||
            next == '?' || next == '!' || next == '-' || next == ';';
    }

    TurnRoute ParseTurnRoute(const std::string& rawInput) const
    {
        if (!IsGroupMode()) {
            return TurnRoute::GroupAll;
        }

        const std::string modelA = m_appState->GetModel();
        const std::string modelB = m_groupModelB;

        for (const auto& token : BuildAddressTokens(modelB)) {
            if (StartsWithAddressToken(rawInput, token)) {
                return TurnRoute::DirectModelB;
            }
        }

        for (const auto& token : BuildAddressTokens(modelA)) {
            if (StartsWithAddressToken(rawInput, token)) {
                return TurnRoute::DirectModelA;
            }
        }

        return TurnRoute::GroupAll;
    }

    // ═════════════════════════════════════════════════════════════
    //  EVENT HANDLERS
    // ═════════════════════════════════════════════════════════════

    void OnAttachImage(wxCommandEvent&)
    {
        wxFileDialog dlg(this, "Attach a file", "", "",
            "Image files (*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.webp)"
            "|*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.webp"
            "|Text files (*.txt;*.md;*.json;*.cpp;*.h;*.py;*.js;*.ts;*.css;*.html;*.xml;*.yaml;*.yml;*.csv;*.log)"
            "|*.txt;*.md;*.json;*.cpp;*.h;*.py;*.js;*.ts;*.css;*.html;*.xml;*.yaml;*.yml;*.csv;*.log"
            "|All files (*.*)|*.*",
            wxFD_OPEN | wxFD_FILE_MUST_EXIST);

        if (dlg.ShowModal() == wxID_CANCEL) return;

        wxString path = dlg.GetPath();
        if (IsImageFile(path)) {
            if (!AttachImageFromFile(path.ToStdString()))
                wxMessageBox("Failed to read image file.", "Error", wxOK | wxICON_ERROR);
        }
        else if (IsTextFile(path)) {
            if (!AttachTextFile(path.ToStdString()))
                wxMessageBox("Failed to read text file.", "Error", wxOK | wxICON_ERROR);
        }
        else {
            wxMessageBox("Unsupported file type.\n\n"
                "Supported: images (png, jpg, gif, bmp, webp)\n"
                "and text files (txt, md, json, cpp, h, py, js, etc.)",
                "Unsupported File", wxOK | wxICON_INFORMATION);
        }
    }

    void OnAssistantDelta(wxCommandEvent& event)
    {
        if (m_isClosing) return;

        // [STEP 2] Drop stale events from a cancelled/previous generation
        if (static_cast<uint64_t>(event.GetExtraLong()) != m_generationId) return;

        std::string delta = WxToUtf8(event.GetString());

        // Persist streamed content as it arrives so switching chats or autosave
        // does not lose the assistant response if the final completion event fails.
        m_chatHistory->AppendToLastAssistantMessage(delta);

        // Still render to the UI normally
        m_chatDisplay->DisplayAssistantDelta(delta);
    }


    void OnAssistantComplete(wxCommandEvent& event)
    {
        if (m_isClosing) return;

        if (static_cast<uint64_t>(event.GetExtraLong()) != m_generationId) return;

        std::string fullResponse = WxToUtf8(event.GetString());

        m_chatDisplay->DisplayAssistantComplete();

        if (!fullResponse.empty()) {
            m_chatHistory->UpdateLastAssistantMessage(fullResponse);
        }
        else if (auto* logger = m_appState->GetLogger()) {
            logger->warning("Assistant complete event arrived empty; keeping streamed assistant content");
        }

        m_chatClient->ResetStreamingState();

        const bool shouldStartModelB =
            (m_chatState == ChatState::GroupModelA) &&
            IsGroupMode() &&
            (m_currentTurnRoute == TurnRoute::GroupAll);

        if (shouldStartModelB) {
            std::string modelA = m_appState->GetModel();
            std::string modelB = m_groupModelB;

            if (auto* logger = m_appState->GetLogger())
                logger->information("Group chat: " + modelA + " done, starting " + modelB);

            std::string body = m_chatHistory->BuildGroupChatRequestJson(modelB, modelA, true);

            m_chatHistory->AddAssistantPlaceholder(modelB);
            m_chatDisplay->DisplayAssistantPrefix(modelB, GetModelAccentColor(modelB));

            ++m_generationId;
            m_chatState = ChatState::GroupModelB;

            if (!m_chatClient->SendMessage(modelB, m_appState->GetApiUrl(),
                body, m_generationId)) {
                m_chatDisplay->DisplaySystemMessage("Failed to start " + modelB + " request");
                m_chatHistory->RemoveLastAssistantMessage();
                SetStreamingState(false);
                AutoSaveConversation();
                m_currentTurnRoute = TurnRoute::GroupAll;
            }
        }
        else {
            SetStreamingState(false);
            AutoSaveConversation();
            m_currentTurnRoute = TurnRoute::GroupAll;

            m_chatDisplay->Clear();
            ReplayConversation();

            if (auto* logger = m_appState->GetLogger())
                logger->information("Chat response completed");
        }
    }


    void OnAssistantError(wxCommandEvent& event)
    {
        if (m_isClosing) return;

        // Drop stale events from a cancelled/previous generation
        if (static_cast<uint64_t>(event.GetExtraLong()) != m_generationId) return;

        std::string error = event.GetString().ToStdString();

        // Identify which model failed (for error messages)
        std::string failedModel = m_appState->GetModel();
        if (m_chatState == ChatState::GroupModelB) {
            failedModel = m_groupModelB;
        }
        else if (m_chatState == ChatState::GroupDirected) {
            failedModel = m_directedTarget;
        }

        std::string friendly;

        if (error.find("Connection refused") != std::string::npos ||
            error.find("Network Error") != std::string::npos ||
            error.find("No connection") != std::string::npos ||
            error.find("Connection reset") != std::string::npos ||
            error.find("Net Exception") != std::string::npos) {

            friendly = "Could not connect to Ollama at " + m_appState->GetApiUrl() +
                ".\nMake sure Ollama is running (\"ollama serve\") and the URL in Settings is correct.";
            _statusDot->SetConnected(false);
        }
        else if (error.find("Timeout") != std::string::npos ||
            error.find("timeout") != std::string::npos) {

            friendly = "Request timed out. Ollama may be busy loading a model \xe2\x80\x94 try again in a moment.";
        }
        else if (error.find("model") != std::string::npos &&
            error.find("not found") != std::string::npos) {

            friendly = "Model \"" + failedModel + "\" was not found. "
                "Open Settings to pick an available model, or run:\n ollama pull " + failedModel;
        }
        else {
            friendly = "Error: " + error;
        }

        // In group mode, prefix the error with the model name for clarity
        if (m_chatState == ChatState::GroupModelB ||
            m_chatState == ChatState::GroupDirected) {
            friendly = "[" + failedModel + "] " + friendly;
        }

        m_chatDisplay->DisplaySystemMessage(friendly);
        if (m_chatHistory->HasAssistantPlaceholder()) {
            m_chatHistory->RemoveLastAssistantMessage();
        }
        m_chatClient->ResetStreamingState();
        SetStreamingState(false);

        m_currentTurnRoute = TurnRoute::GroupAll;

        // If Model B failed, Model A's response is still in history — save it
        if (!m_chatHistory->IsEmpty()) AutoSaveConversation();

        if (auto* logger = m_appState->GetLogger())
            logger->error("Chat error (" + failedModel + "): " + error);
    }

    void OnToggleSidebar(wxCommandEvent&)
    {
        m_sidebar->Toggle();

        if (m_sidebar->IsVisible()) {
            m_sidebar->Refresh(m_chatHistory->GetFilePath());
        }

        _contentSizer->Layout();
        GetSizer()->Layout();
    }

    void OnStopGeneration(wxCommandEvent&)
    {
        if (IsBusy()) {
            // Bump generation ID so any queued delta/complete/error
            // events from this request are silently dropped by the handlers.
            ++m_generationId;

            m_chatClient->StopGeneration();
            m_chatDisplay->DisplayAssistantComplete();
            m_chatDisplay->DisplaySystemMessage("Generation stopped by user");
            if (m_chatHistory->HasAssistantPlaceholder()) {
                m_chatHistory->RemoveLastAssistantMessage();
            }
            SetStreamingState(false);

            m_currentTurnRoute = TurnRoute::GroupAll;

            // Auto-save: if Model A completed but Model B was stopped,
            // Model A's response is still in history and worth saving.
            if (!m_chatHistory->IsEmpty()) AutoSaveConversation();
        }
    }


    void OnOpenSettings(wxCommandEvent&)
    {
        if (IsBusy()) {
            wxMessageBox("Cannot change settings while generating response",
                "Settings", wxOK | wxICON_INFORMATION);
            return;
        }

        SettingsDialog dlg(this, m_appState->GetModel(), m_appState->GetApiUrl(),
                           m_appState->GetThemeName(),
                           m_appState->IsWorkspaceEnabled(),
                           m_appState->GetWorkspacePath());

        if (dlg.ShowModal() == wxID_OK)
        {
            bool modelChanged, apiUrlChanged;
            bool anyChange = m_appState->UpdateSettings(
                dlg.GetSelectedModel(),
                dlg.GetSelectedApiUrl(),
                modelChanged,
                apiUrlChanged
            );

            // Handle theme change separately (doesn't require chat clear)
            bool themeChanged = dlg.WasThemeChanged();
            if (themeChanged) {
                m_appState->SetTheme(dlg.GetSelectedTheme());
                ApplyThemeToUI();
            }

            // Handle workspace change (doesn't require chat clear)
            bool workspaceChanged = dlg.WasWorkspaceChanged();
            if (workspaceChanged) {
                m_appState->SetWorkspaceEnabled(dlg.GetWorkspaceEnabled());
                m_appState->SetWorkspacePath(dlg.GetWorkspacePath());
            }

            if (anyChange)
            {
                m_chatHistory->Clear();
                m_chatDisplay->Clear();
                ClearPendingAttachment();
                m_groupModelB.clear();
                UpdateModelLabel();
                UpdateGroupToggleButton();

                m_chatDisplay->DisplaySystemMessage("Settings updated. Chat cleared.");
                _userInputCtrl->SetFocus();
            }
            else if (themeChanged)
            {
                // Re-render existing conversation with new theme colors
                // (wxRichTextCtrl bakes colors into rendered text, so we must replay)
                if (!m_chatHistory->IsEmpty()) {
                    m_chatDisplay->Clear();
                    ReplayConversation();
                }
                m_chatDisplay->DisplaySystemMessage("Theme changed to " +
                    m_appState->GetThemeName() + ".");
            }
        }
    }

    void OnAbout(wxCommandEvent&)
    {
        wxString msg;
        msg << "OpenChat v" << OPENCHAT_VERSION << "\n\n"
            << "A native desktop chat client for Ollama.\n\n"
            << "Built with wxWidgets + Poco\n"
            << "License: MIT\n\n"
            << wxString::FromUTF8("Model: ") << wxString::FromUTF8(m_appState->GetModel()) << "\n"
            << wxString::FromUTF8("API: ") << wxString::FromUTF8(m_appState->GetApiUrl());
        wxMessageBox(msg, "About OpenChat", wxOK | wxICON_INFORMATION);
    }

    // ─── Model pill click → fetch model list and show picker ─────
    void OnModelPillClick(wxMouseEvent&)
    {
        if (IsBusy()) return;

        m_pickerIsForGroupB = false;
        auto* thread = new ModelPickerThread(
            this, m_appState->GetApiUrl(), m_alive);
        if (thread->Run() != wxTHREAD_NO_ERROR) {
            delete thread;
            m_chatDisplay->DisplaySystemMessage(
                "Failed to fetch model list. Is Ollama running?");
        }
    }

    // ─── Group toggle button click ──────────────────────────────
    void OnGroupToggleClick(wxCommandEvent&)
    {
        if (IsBusy()) return;

        if (IsGroupMode()) {
            // Remove Model B
            m_groupModelB.clear();
            UpdateModelLabel();
            UpdateGroupToggleButton();
            m_chatDisplay->DisplaySystemMessage("Group chat disabled. Single model mode.");
        }
        else {
            // Invite Model B — fetch model list
            m_pickerIsForGroupB = true;
            auto* thread = new ModelPickerThread(
                this, m_appState->GetApiUrl(), m_alive);
            if (thread->Run() != wxTHREAD_NO_ERROR) {
                delete thread;
                m_chatDisplay->DisplaySystemMessage(
                    "Failed to fetch model list. Is Ollama running?");
            }
        }
    }

    void OnModelListReady(wxCommandEvent& event)
    {
        if (m_isClosing) return;

        wxString modelStr = event.GetString();
        if (modelStr.empty()) {
            m_chatDisplay->DisplaySystemMessage(
                "Could not fetch models from " + m_appState->GetApiUrl() +
                ". Make sure Ollama is running.");
            return;
        }

        // Parse newline-separated model list
        m_pickerModels.clear();
        std::istringstream stream(modelStr.ToStdString());
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.empty()) m_pickerModels.push_back(line);
        }

        if (m_pickerModels.empty()) return;

        if (m_pickerIsForGroupB) {
            // ── Group mode: invite a second model ────────────────
            wxMenu menu;
            std::string currentModel = m_appState->GetModel();
            std::string currentGroupB = m_groupModelB;

            for (size_t i = 0; i < m_pickerModels.size(); ++i) {
                const auto& name = m_pickerModels[i];
                // Skip the primary model — can't invite yourself
                if (name == currentModel) continue;
                wxMenuItem* item = menu.AppendCheckItem(
                    10000 + (int)i, wxString::FromUTF8(name));
                if (name == currentGroupB) {
                    item->Check(true);
                }
            }

            menu.Bind(wxEVT_MENU, [this](wxCommandEvent& e) {
                int idx = e.GetId() - 10000;
                if (idx >= 0 && idx < (int)m_pickerModels.size()) {
                    SetGroupModelB(m_pickerModels[idx]);
                }
            });

            // Show popup below the group toggle button
            wxPoint pos = _groupToggleButton->GetScreenPosition();
            pos = ScreenToClient(pos);
            pos.y += _groupToggleButton->GetSize().y;
            PopupMenu(&menu, pos);
        }
        else {
            // ── Normal mode: switch primary model ────────────────
            wxMenu menu;
            std::string currentModel = m_appState->GetModel();

            for (size_t i = 0; i < m_pickerModels.size(); ++i) {
                wxMenuItem* item = menu.AppendCheckItem(
                    10000 + (int)i, wxString::FromUTF8(m_pickerModels[i]));
                if (m_pickerModels[i] == currentModel) {
                    item->Check(true);
                }
            }

            menu.Bind(wxEVT_MENU, [this](wxCommandEvent& e) {
                int idx = e.GetId() - 10000;
                if (idx >= 0 && idx < (int)m_pickerModels.size()) {
                    SwitchToModel(m_pickerModels[idx]);
                }
            });

            // Show popup below the model pill
            wxPoint pos = _modelPill->GetScreenPosition();
            pos = ScreenToClient(pos);
            pos.y += _modelPill->GetSize().y;
            PopupMenu(&menu, pos);
        }
    }

    // ─── Set/activate group Model B ──────────────────────────────
    void SetGroupModelB(const std::string& model)
    {
        if (model == m_appState->GetModel()) return;  // Can't be same as Model A
        m_groupModelB = model;
        UpdateModelLabel();
        UpdateGroupToggleButton();
        m_chatDisplay->DisplaySystemMessage(
            "Group chat enabled: " + m_appState->GetModel() + " + " + model);
    }

    // ─── Update group toggle button appearance ───────────────────
    void UpdateGroupToggleButton()
    {
        if (IsGroupMode()) {
            _groupToggleButton->SetLabel(wxString::FromUTF8("\xC3\x97")); // ×
            _groupToggleButton->SetForegroundColour(m_appState->GetTheme().stopButton);
            _groupToggleButton->SetToolTip("Remove " + wxString::FromUTF8(m_groupModelB) +
                " from conversation");
        }
        else {
            _groupToggleButton->SetLabel("+");
            _groupToggleButton->SetForegroundColour(m_appState->GetTheme().accentButton);
            _groupToggleButton->SetToolTip("Invite a second model to the conversation");
        }
        _groupToggleButton->Refresh();
    }

    void SwitchToModel(const std::string& newModel)
    {
        if (newModel == m_appState->GetModel()) return;
        if (IsBusy()) return;

        // Save current conversation before switching
        if (!m_chatHistory->IsEmpty()) {
            AutoSaveConversation();
        }

        bool mc, ac;
        m_appState->UpdateSettings(newModel, m_appState->GetApiUrl(), mc, ac);

        m_chatHistory->Clear();
        m_chatDisplay->Clear();
        ClearPendingAttachment();
        m_groupModelB.clear();  // Clear group mode on model switch
        UpdateModelLabel();
        UpdateGroupToggleButton();
        UpdateWindowTitle();

        m_chatDisplay->DisplaySystemMessage("Switched to " + newModel);
        _statusDot->SetConnected(true);
        _userInputCtrl->SetFocus();

        if (auto* logger = m_appState->GetLogger())
            logger->information("Quick-switched to model: " + newModel);
    }

    void OnNewChat(wxCommandEvent&)
    {
        if (IsBusy()) return;

        // Save current conversation if it has content
        if (!m_chatHistory->IsEmpty()) {
            AutoSaveConversation();
        }

        m_chatHistory->Clear();
        m_chatDisplay->Clear();
        ClearPendingAttachment();
        m_groupModelB.clear();  // Clear group mode on new chat
        UpdateModelLabel();
        UpdateGroupToggleButton();
        UpdateWindowTitle();
        if (m_sidebar->IsVisible())
            m_sidebar->Refresh(m_chatHistory->GetFilePath());
        _userInputCtrl->SetFocus();

        if (auto* logger = m_appState->GetLogger())
            logger->information("New chat started");
    }

    void OnSaveConversation(wxCommandEvent&)
    {
        if (m_chatHistory->IsEmpty()) return;

        if (m_chatHistory->HasFilePath()) {
            // Already has a file — just save in place
            if (m_chatHistory->SaveToFile("", GetActiveModels())) {
                m_chatDisplay->DisplaySystemMessage("Conversation saved.");
            }
        }
        else {
            // No file yet — ask where to save
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

            wxFileDialog dlg(this, "Save Conversation", defaultDir, defaultName,
                "JSON files (*.json)|*.json",
                wxFD_SAVE | wxFD_OVERWRITE_PROMPT);

            if (dlg.ShowModal() == wxID_CANCEL) return;

            std::string path = dlg.GetPath().ToStdString();
            if (m_chatHistory->SaveToFile(path, GetActiveModels())) {
                UpdateWindowTitle();
                m_chatDisplay->DisplaySystemMessage("Conversation saved.");
            }
            else {
                wxMessageBox("Failed to save conversation", "Error", wxOK | wxICON_ERROR);
            }
        }
    }

    void OnLoadConversation(wxCommandEvent&)
    {
        if (IsBusy()) return;

        // Save current conversation before loading a new one
        if (!m_chatHistory->IsEmpty() && !m_chatHistory->HasFilePath()) {
            AutoSaveConversation();
        }

        wxString defaultDir = wxString::FromUTF8(ChatHistory::GetConversationsDir());

        wxFileDialog dlg(this, "Open Conversation", defaultDir, "",
            "JSON files (*.json)|*.json|All files (*.*)|*.*",
            wxFD_OPEN | wxFD_FILE_MUST_EXIST);

        if (dlg.ShowModal() == wxID_CANCEL) return;

        std::string path = dlg.GetPath().ToStdString();
        std::vector<std::string> loadedModels;

        ChatHistory* newHistory = new ChatHistory();
        if (!newHistory->LoadFromFile(path, loadedModels)) {
            delete newHistory;
            wxMessageBox("Failed to load conversation file", "Error", wxOK | wxICON_ERROR);
            return;
        }

        // Replace current history
        delete m_chatHistory;
        m_chatHistory = newHistory;

        // Restore model(s) from the loaded conversation
        std::string primaryModel = loadedModels.empty() ? "" : loadedModels.front();
        if (!primaryModel.empty() && primaryModel != m_appState->GetModel()) {
            bool mc, ac;
            m_appState->UpdateSettings(primaryModel, m_appState->GetApiUrl(), mc, ac);
        }

        // Restore group mode if conversation had two models
        m_groupModelB = (loadedModels.size() >= 2) ? loadedModels[1] : "";
        UpdateModelLabel();
        UpdateGroupToggleButton();

        // Replay the conversation to the display
        m_chatDisplay->Clear();
        ClearPendingAttachment();
        ReplayConversation();
        UpdateWindowTitle();
        _userInputCtrl->SetFocus();

        if (auto* logger = m_appState->GetLogger())
            logger->information("Loaded conversation: " + m_chatHistory->GetTitle());
    }

    void AutoSaveConversation()
    {
        if (m_chatHistory->IsEmpty()) return;

        // If no file path yet, generate one automatically
        if (!m_chatHistory->HasFilePath()) {
            m_chatHistory->SetFilePath(ChatHistory::GenerateFilePath());
        }

        if (m_chatHistory->SaveToFile("", GetActiveModels())) {
            UpdateWindowTitle();
            if (m_sidebar->IsVisible())
                m_sidebar->Refresh(m_chatHistory->GetFilePath());
            if (auto* logger = m_appState->GetLogger())
                logger->debug("Auto-saved conversation: " + m_chatHistory->GetFilePath());
        }
    }

    void UpdateWindowTitle()
    {
        std::string title = "Ollama Chat";
        if (!m_chatHistory->IsEmpty()) {
            std::string convTitle = m_chatHistory->GetTitle();
            if (convTitle.empty()) {
                convTitle = m_chatHistory->GenerateTitle();
            }
            if (!convTitle.empty() && convTitle != "Untitled conversation") {
                // Truncate for window title
                if (convTitle.size() > 40) {
                    convTitle = convTitle.substr(0, 37) + "...";
                }
                title = convTitle + " - Ollama Chat";
            }
        }
        SetTitle(wxString::FromUTF8(title));
    }

    void ReplayConversation()
    {
        const auto& messages = m_chatHistory->GetMessages();
        for (const auto& msg : messages) {
            std::string role = msg->getValue<std::string>("role");
            std::string content = msg->getValue<std::string>("content");

            if (content.empty()) continue;

            if (role == "user") {
                std::string target = ChatHistory::GetMessageTarget(msg);
                m_chatDisplay->DisplayUserMessage(content, target);
            }
            else if (role == "assistant") {
                // Read per-message model tag; fall back to primary model
                std::string msgModel = ChatHistory::GetMessageModel(msg);
                if (msgModel.empty()) msgModel = m_appState->GetModel();
                m_chatDisplay->DisplayAssistantMessage(
                    msgModel,
                    content,
                    GetModelAccentColor(msgModel)
                );
            }
            else if (role == "system") {
                m_chatDisplay->DisplaySystemMessage(content);
            }
        }
    }

    void AutoLoadLastConversation()
    {
        std::string convDir = ChatHistory::GetConversationsDir();
        wxDir dir(wxString::FromUTF8(convDir));
        if (!dir.IsOpened()) return;

        // Find the most recently modified JSON file
        wxString bestFile;
        wxDateTime bestTime;

        wxString filename;
        bool found = dir.GetFirst(&filename, "*.json", wxDIR_FILES);
        while (found) {
            wxString fullPath = wxString::FromUTF8(convDir) +
                wxFileName::GetPathSeparator() + filename;

            wxFileName fn(fullPath);
            wxDateTime modTime;
            fn.GetTimes(nullptr, &modTime, nullptr);

            if (!bestTime.IsValid() || modTime.IsLaterThan(bestTime)) {
                bestTime = modTime;
                bestFile = fullPath;
            }

            found = dir.GetNext(&filename);
        }

        if (!bestFile.IsEmpty()) {
            LoadConversationFromPath(bestFile.ToStdString());
        }
    }

    bool LoadConversationFromPath(const std::string& path)
    {
        if (IsBusy()) return false;

        // Save current conversation before loading
        if (!m_chatHistory->IsEmpty() && !m_chatHistory->HasFilePath()) {
            AutoSaveConversation();
        }

        std::vector<std::string> loadedModels;
        ChatHistory* newHistory = new ChatHistory();
        if (!newHistory->LoadFromFile(path, loadedModels)) {
            delete newHistory;
            return false;
        }

        // Replace current history
        delete m_chatHistory;
        m_chatHistory = newHistory;

        // Restore model(s) from the loaded conversation
        std::string primaryModel = loadedModels.empty() ? "" : loadedModels.front();
        if (!primaryModel.empty() && primaryModel != m_appState->GetModel()) {
            bool mc, ac;
            m_appState->UpdateSettings(primaryModel, m_appState->GetApiUrl(), mc, ac);
        }

        // Restore group mode if conversation had two models
        m_groupModelB = (loadedModels.size() >= 2) ? loadedModels[1] : "";
        UpdateModelLabel();
        UpdateGroupToggleButton();

        // Replay to display
        m_chatDisplay->Clear();
        ClearPendingAttachment();
        ReplayConversation();
        UpdateWindowTitle();
        if (m_sidebar->IsVisible())
            m_sidebar->Refresh(m_chatHistory->GetFilePath());
        _userInputCtrl->SetFocus();

        if (auto* logger = m_appState->GetLogger())
            logger->information("Loaded conversation: " + m_chatHistory->GetTitle());

        return true;
    }

    static std::string WxToUtf8(const wxString& s)
    {
        wxScopedCharBuffer buf = s.ToUTF8();
        if (!buf) return std::string();
        return std::string(buf.data());
    }

    void DeleteConversations(const std::vector<std::string>& filePaths)
    {
        if (filePaths.empty()) return;

        // Build confirmation message
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

                // If deleting the currently active conversation, clear the display
                if (!clearedActive && filePath == m_chatHistory->GetFilePath()) {
                    m_chatHistory->Clear();
                    m_chatDisplay->Clear();
                    ClearPendingAttachment();
                    UpdateWindowTitle();
                    clearedActive = true;
                }

                if (auto* logger = m_appState->GetLogger())
                    logger->information("Deleted conversation: " + filePath);
            }
        }

        if (deleted > 0) {
            m_sidebar->ClearSelection();
            if (m_sidebar->IsVisible())
                m_sidebar->Refresh(m_chatHistory->GetFilePath());
        }

        if (deleted < (int)filePaths.size()) {
            wxMessageBox(
                wxString::Format("Failed to delete %d of %zu files.",
                                 (int)filePaths.size() - deleted,
                                 filePaths.size()),
                "Error", wxOK | wxICON_ERROR);
        }
    }

    void OnUserInputChanged(wxCommandEvent&)
    {
        if (!_userInputCtrl || !_inputSizer) return;

        const int DESIRED_BASE_HEIGHT = 30;
        const int MAX_LINES_TO_SHOW = 5;

        int charHeight = _userInputCtrl->GetCharHeight();
        int padding = 4;
        int lineHeight = charHeight + padding;
        int lines = _userInputCtrl->GetNumberOfLines();
        wxString val = _userInputCtrl->GetValue();

        int newH;
        if (val.IsEmpty() || lines == 1) {
            newH = std::max(DESIRED_BASE_HEIGHT, lineHeight);
        }
        else {
            int show = std::min(lines, MAX_LINES_TO_SHOW);
            newH = std::max(lineHeight * show, DESIRED_BASE_HEIGHT);
        }

        if (_userInputCtrl->GetMinSize().y != newH) {
            _userInputCtrl->SetMinSize(wxSize(-1, newH));
            _inputSizer->Layout();
            if (GetSizer()) GetSizer()->Layout();
        }
    }

    void OnCharHook(wxKeyEvent& evt)
    {
        // Note: Ctrl+V image paste is handled by ChatInputCtrl::MSWWindowProc
        // at the WM_PASTE level, not here.
        if (evt.ControlDown()) {
            switch (evt.GetKeyCode()) {
            case 'N':
                // Ctrl+N — new chat
            { wxCommandEvent e; OnNewChat(e); }
            return;
            case 'S':
                // Ctrl+S — save conversation
            { wxCommandEvent e; OnSaveConversation(e); }
            return;
            case 'O':
                // Ctrl+O — open/load conversation
            { wxCommandEvent e; OnLoadConversation(e); }
            return;
            }
        }
        evt.Skip();
    }

    bool TryPasteImageFromClipboard()
    {
        if (!wxTheClipboard->Open()) return false;

        bool hasImage = wxTheClipboard->IsSupported(wxDF_BITMAP);

        if (!hasImage) {
            wxTheClipboard->Close();
            return false;
        }

        wxBitmapDataObject bmpData;
        bool gotData = wxTheClipboard->GetData(bmpData);
        wxTheClipboard->Close();

        if (!gotData || !bmpData.GetBitmap().IsOk()) return false;

        // Convert bitmap to PNG in memory, then base64-encode
        wxImage img = bmpData.GetBitmap().ConvertToImage();
        wxMemoryOutputStream memStream;
        if (!img.SaveFile(memStream, wxBITMAP_TYPE_PNG)) return false;

        // Get raw bytes from stream
        size_t dataSize = memStream.GetSize();
        std::vector<unsigned char> rawData(dataSize);
        memStream.CopyTo(rawData.data(), dataSize);

        // Base64-encode using Poco
        std::ostringstream base64Stream;
        Poco::Base64Encoder encoder(base64Stream);
        encoder.write(reinterpret_cast<const char*>(rawData.data()), dataSize);
        encoder.close();

        std::string base64 = base64Stream.str();
        base64.erase(std::remove_if(base64.begin(), base64.end(), ::isspace),
            base64.end());

        if (base64.empty()) return false;

        // Clear any pending text file (one attachment at a time)
        m_pendingFileText.clear();
        m_pendingFileName.clear();

        m_pendingImageBase64 = base64;
        m_pendingImageName = "clipboard_image.png";

        _attachLabel->SetLabel(wxString::FromUTF8(
            "  \xF0\x9F\x96\xBC  Pasted image"));
        _attachLabel->Show();
        GetSizer()->Layout();
        _userInputCtrl->SetFocus();

        if (auto* logger = m_appState->GetLogger()) {
            logger->information("Image pasted from clipboard (" +
                std::to_string(base64.size()) + " bytes base64)");
        }

        return true;
    }

    void OnFrameActivate(wxActivateEvent& evt)
    {
        if (evt.GetActive() && !IsBusy())
            _userInputCtrl->SetFocus();
        evt.Skip();
    }

    void OnSendMessage(wxCommandEvent&)
    {
        if (IsBusy()) return;

        std::string userInput = WxToUtf8(_userInputCtrl->GetValue());
        bool hasImage = !m_pendingImageBase64.empty();
        bool hasFile  = !m_pendingFileText.empty();
        if (userInput.empty() && !hasImage && !hasFile) return;

        if (userInput.empty() && hasImage)
            userInput = "What is in this image?";
        if (userInput.empty() && hasFile)
            userInput = "Please review this file.";

        // ── Stage 1: Action classification ───────────────────────
        // Runs before model routing.  If the message is a workspace
        // command, handle it here and skip the LLM entirely.
        ActionIntent intent = ClassifyAction(userInput,
            m_appState->IsWorkspaceEnabled());

        if (intent.action == ActionType::WorkspaceSearch)
        {
            m_chatDisplay->DisplayUserMessage(userInput);
            _userInputCtrl->Clear();
            { wxCommandEvent e(wxEVT_TEXT, _userInputCtrl->GetId()); OnUserInputChanged(e); }

            // Execute the search against the workspace folder
            WorkspaceSearchResult searchResult = ExecuteWorkspaceSearch(
                intent.rawQuery, m_appState->GetWorkspacePath());

            std::string sysMsg = FormatSearchResult(
                searchResult, intent.rawQuery, m_appState->GetWorkspacePath());

            m_chatDisplay->DisplaySystemMessage(sysMsg);
            _userInputCtrl->SetFocus();
            return;
        }

        if (intent.action == ActionType::WorkspaceSaveChat)
        {
            m_chatDisplay->DisplayUserMessage(userInput);
            _userInputCtrl->Clear();
            { wxCommandEvent e(wxEVT_TEXT, _userInputCtrl->GetId()); OnUserInputChanged(e); }

            // Convert ChatHistory messages to executor-friendly format
            std::vector<ChatMessage> exportMessages;
            for (const auto& msg : m_chatHistory->GetMessages()) {
                ChatMessage cm;
                cm.role = msg->getValue<std::string>("role");
                cm.content = msg->getValue<std::string>("content");
                cm.model = ChatHistory::GetMessageModel(msg);
                exportMessages.push_back(std::move(cm));
            }

            WorkspaceSaveChatResult saveResult = ExecuteWorkspaceSaveChat(
                exportMessages,
                m_appState->GetWorkspacePath(),
                intent.rawQuery,                // custom name (may be empty)
                m_appState->GetModel());         // primary model

            m_chatDisplay->DisplaySystemMessage(FormatSaveChatResult(saveResult));
            _userInputCtrl->SetFocus();
            return;
        }

        // ── Stage 1b: LLM-assisted classification ────────────────
        // If workspace is enabled and no slash command matched, ask
        // the model whether this is a natural language workspace request.
        // This is a short synchronous call — typically 200-500ms.
        if (m_appState->IsWorkspaceEnabled() && m_pendingImageBase64.empty()
            && m_pendingFileText.empty())
        {
            LLMClassifyResult llmResult = ClassifyWithLLM(
                userInput,
                m_appState->GetApiUrl(),
                m_appState->GetModel());

            if (llmResult.success && llmResult.action == "save_chat")
            {
                m_chatDisplay->DisplayUserMessage(userInput);
                _userInputCtrl->Clear();
                { wxCommandEvent e(wxEVT_TEXT, _userInputCtrl->GetId()); OnUserInputChanged(e); }

                // Convert ChatHistory messages to executor-friendly format
                std::vector<ChatMessage> exportMessages;
                for (const auto& msg : m_chatHistory->GetMessages()) {
                    ChatMessage cm;
                    cm.role = msg->getValue<std::string>("role");
                    cm.content = msg->getValue<std::string>("content");
                    cm.model = ChatHistory::GetMessageModel(msg);
                    exportMessages.push_back(std::move(cm));
                }

                WorkspaceSaveChatResult saveResult = ExecuteWorkspaceSaveChat(
                    exportMessages,
                    m_appState->GetWorkspacePath(),
                    llmResult.filenameHint,
                    m_appState->GetModel());

                m_chatDisplay->DisplaySystemMessage(FormatSaveChatResult(saveResult));
                _userInputCtrl->SetFocus();
                return;
            }

            if (llmResult.success && llmResult.action == "workspace_search")
            {
                m_chatDisplay->DisplayUserMessage(userInput);
                _userInputCtrl->Clear();
                { wxCommandEvent e(wxEVT_TEXT, _userInputCtrl->GetId()); OnUserInputChanged(e); }

                WorkspaceSearchResult searchResult = ExecuteWorkspaceSearch(
                    llmResult.topic, m_appState->GetWorkspacePath());

                std::string sysMsg = FormatSearchResult(
                    searchResult, llmResult.topic, m_appState->GetWorkspacePath());

                m_chatDisplay->DisplaySystemMessage(sysMsg);
                _userInputCtrl->SetFocus();
                return;
            }

            // LLM returned "chat" or failed — fall through to normal chat
        }

        // ── Stage 2: Model routing ───────────────────────────────
        // Decide routing before dispatch.
        m_currentTurnRoute = ParseTurnRoute(userInput);

        if (!m_pendingImageBase64.empty())
            m_chatDisplay->DisplayUserMessage("[" + m_pendingImageName + "] " + userInput);
        else if (!m_pendingFileText.empty())
            m_chatDisplay->DisplayUserMessage("[" + m_pendingFileName + "] " + userInput);
        else
            m_chatDisplay->DisplayUserMessage(userInput);

        _userInputCtrl->Clear();
        { wxCommandEvent e(wxEVT_TEXT, _userInputCtrl->GetId()); OnUserInputChanged(e); }

        // Keep the raw user text in history so the transcript stays faithful.
        m_chatHistory->AddUserMessage(userInput);

        std::string modelA = m_appState->GetModel();
        std::string targetModel = modelA;
        std::string body;

        if (IsGroupMode()) {
            switch (m_currentTurnRoute) {
            case TurnRoute::DirectModelB:
                targetModel = m_groupModelB;
                break;
            case TurnRoute::DirectModelA:
            case TurnRoute::GroupAll:
            default:
                targetModel = modelA;
                break;
            }

            // In multi-model mode, always build a participant-aware request so
            // the target model does not mistake the other model's messages as its own.
            body = m_chatHistory->BuildParticipantChatRequestJson(targetModel, true);
        }
        else {
            m_currentTurnRoute = TurnRoute::GroupAll;
            body = m_chatHistory->BuildChatRequestJson(targetModel, true);
        }

        if (!m_pendingImageBase64.empty()) {
            body = InjectImageIntoRequest(body, m_pendingImageBase64);
            ClearPendingAttachment();
        }
        else if (!m_pendingFileText.empty()) {
            body = InjectTextFileIntoRequest(body, m_pendingFileName, m_pendingFileText);
            ClearPendingAttachment();
        }

        if (auto* logger = m_appState->GetLogger())
            logger->debug("Request sent (" + std::to_string(body.size()) + " bytes)");

        m_chatHistory->AddAssistantPlaceholder(targetModel);
        m_chatDisplay->DisplayAssistantPrefix(targetModel, GetModelAccentColor(targetModel));

        ++m_generationId;

        if (!IsGroupMode()) {
            m_chatState = ChatState::SingleStreaming;
        }
        else if (m_currentTurnRoute == TurnRoute::DirectModelB) {
            m_chatState = ChatState::GroupModelB;
        }
        else if (m_currentTurnRoute == TurnRoute::DirectModelA) {
            m_chatState = ChatState::GroupDirected;
            m_directedTarget = targetModel;
        }
        else {
            // GroupAll: Model A streams first, then chains to Model B.
            m_chatState = ChatState::GroupModelA;
        }

        SetStreamingState(true);

        if (!m_chatClient->SendMessage(targetModel, m_appState->GetApiUrl(),
            body, m_generationId)) {
            SetStreamingState(false);
            m_chatDisplay->DisplaySystemMessage("Failed to start chat request");
            m_chatHistory->RemoveLastAssistantMessage();
            m_currentTurnRoute = TurnRoute::GroupAll;
        }
    }
};

// ═══════════════════════════════════════════════════════════════════
//  ImageDropTarget Implementation
// ═══════════════════════════════════════════════════════════════════

bool ImageDropTarget::OnDropFiles(wxCoord /*x*/, wxCoord /*y*/,
    const wxArrayString& filenames)
{
    // Accept the first valid image or text file from the drop
    for (const auto& file : filenames) {
        if (IsImageFile(file))
            return m_frame->AttachImageFromFile(file.ToStdString());
        if (IsTextFile(file))
            return m_frame->AttachTextFile(file.ToStdString());
    }
    return false; // No supported file found in drop
}

// ═══════════════════════════════════════════════════════════════════
class MyApp : public wxApp {
public:
    bool OnInit() override {
        if (!wxApp::OnInit()) return false;
        wxInitAllImageHandlers();  // Required for JPEG/GIF/WebP clipboard & file support
        auto* frame = new MyFrame();
        frame->Show();
        return true;
    }
};

wxIMPLEMENT_APP(MyApp);