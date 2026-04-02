// File: openchat.cpp (redesigned UI layout + image attach + drag-drop)
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
    ModelPickerThread(wxEvtHandler* handler, const std::string& apiUrl)
        : wxThread(wxTHREAD_DETACHED), m_handler(handler), m_apiUrl(apiUrl) {}
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
                wxCommandEvent* evt = new wxCommandEvent(wxEVT_MODEL_LIST_READY);
                evt->SetString("");
                wxQueueEvent(m_handler, evt);
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

            wxCommandEvent* evt = new wxCommandEvent(wxEVT_MODEL_LIST_READY);
            evt->SetString(modelList);
            wxQueueEvent(m_handler, evt);
        }
        catch (...) {
            wxCommandEvent* evt = new wxCommandEvent(wxEVT_MODEL_LIST_READY);
            evt->SetString("");
            wxQueueEvent(m_handler, evt);
        }
        return (ExitCode)0;
    }
private:
    wxEvtHandler* m_handler;
    std::string m_apiUrl;
};

// ═══════════════════════════════════════════════════════════════════
class MyFrame : public wxFrame {
public:
    MyFrame()
        : wxFrame(nullptr, wxID_ANY, "Ollama Chat",
            wxDefaultPosition, wxSize(1100, 700),
            wxDEFAULT_FRAME_STYLE)
        , m_appState(new AppState())
        , m_chatClient(new ChatClient(this))
        , m_chatDisplay(nullptr)
        , m_chatHistory(new ChatHistory())
        , m_sidebarVisible(false)
        , m_isClosing(false)
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

        // ── Sidebar panel (collapsible) ──
        _sidebarPanel = new wxPanel(this, wxID_ANY);
        _sidebarPanel->SetBackgroundColour(m_appState->GetTheme().bgSidebar);
        _sidebarPanel->SetMinSize(wxSize(260, -1));

        auto* sidebarOuterSizer = new wxBoxSizer(wxHORIZONTAL);

        // ── Sidebar content area ──
        _sidebarContent = new wxPanel(_sidebarPanel, wxID_ANY);
        _sidebarContent->SetBackgroundColour(m_appState->GetTheme().bgSidebar);
        auto* sidebarContentSizer = new wxBoxSizer(wxVERTICAL);

        // "New Chat" button at top
        _sidebarNewChat = new wxButton(_sidebarContent, wxID_ANY, "+ New Chat",
            wxDefaultPosition, wxSize(-1, 42), wxBORDER_NONE);
        _sidebarNewChat->SetBackgroundColour(m_appState->GetTheme().modelPillBg);
        _sidebarNewChat->SetForegroundColour(m_appState->GetTheme().textPrimary);
        wxFont ncFont = _sidebarNewChat->GetFont();
        ncFont.SetPointSize(11);
        ncFont.SetWeight(wxFONTWEIGHT_MEDIUM);
        _sidebarNewChat->SetFont(ncFont);
        sidebarContentSizer->Add(_sidebarNewChat, 0, wxEXPAND | wxALL, 8);

        // Scrollable conversation list
        _conversationList = new wxScrolledWindow(_sidebarContent, wxID_ANY,
            wxDefaultPosition, wxDefaultSize, wxVSCROLL);
        _conversationList->SetBackgroundColour(m_appState->GetTheme().bgSidebar);
        _conversationList->SetScrollRate(0, 8);
        _conversationListSizer = new wxBoxSizer(wxVERTICAL);
        _conversationList->SetSizer(_conversationListSizer);

        sidebarContentSizer->Add(_conversationList, 1, wxEXPAND);
        _sidebarContent->SetSizer(sidebarContentSizer);
        sidebarOuterSizer->Add(_sidebarContent, 1, wxEXPAND);

        // Vertical border on the right edge
        _sidebarBorder = new wxPanel(_sidebarPanel, wxID_ANY, wxDefaultPosition, wxSize(1, -1));
        _sidebarBorder->SetBackgroundColour(m_appState->GetTheme().borderSubtle);
        sidebarOuterSizer->Add(_sidebarBorder, 0, wxEXPAND);

        _sidebarPanel->SetSizer(sidebarOuterSizer);
        _sidebarPanel->Hide(); // Start collapsed
        _contentSizer->Add(_sidebarPanel, 0, wxEXPAND);

        // ── Right panel (chat display + input) ──
        _rightPanel = new wxPanel(this, wxID_ANY);
        _rightPanel->SetBackgroundColour(m_appState->GetTheme().bgMain);
        auto* rightSizer = new wxBoxSizer(wxVERTICAL);

        _chatDisplayCtrl = new wxRichTextCtrl(
            _rightPanel, wxID_ANY, wxEmptyString,
            wxDefaultPosition, wxDefaultSize,
            wxRE_MULTILINE | wxRE_READONLY | wxBORDER_NONE
        );
        _chatDisplayCtrl->SetBackgroundColour(m_appState->GetTheme().bgMain);
        _chatDisplayCtrl->SetForegroundColour(m_appState->GetTheme().textPrimary);
        rightSizer->Add(_chatDisplayCtrl, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);

        // ─── ATTACHMENT INDICATOR (hidden by default) ────────────────
        _attachLabel = new wxStaticText(_rightPanel, wxID_ANY, "");
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
        _sidebarNewChat->Bind(wxEVT_BUTTON, &MyFrame::OnNewChat, this);
        Bind(wxEVT_ACTIVATE, &MyFrame::OnFrameActivate, this);

        Bind(wxEVT_ASSISTANT_DELTA, &MyFrame::OnAssistantDelta, this);
        Bind(wxEVT_ASSISTANT_COMPLETE, &MyFrame::OnAssistantComplete, this);
        Bind(wxEVT_ASSISTANT_ERROR, &MyFrame::OnAssistantError, this);

        // ─── Model pill click (quick model switch) ────────────────────
        _modelPill->Bind(wxEVT_LEFT_UP, &MyFrame::OnModelPillClick, this);
        _modelLabel->Bind(wxEVT_LEFT_UP, &MyFrame::OnModelPillClick, this);
        _statusDot->Bind(wxEVT_LEFT_UP, &MyFrame::OnModelPillClick, this);
        Bind(wxEVT_MODEL_LIST_READY, &MyFrame::OnModelListReady, this);

        // ─── Drag-and-drop support ────────────────────────────────────
        SetDropTarget(new ImageDropTarget(this));

        // ─── Clipboard image paste (via WM_PASTE interception) ──────
        // On Windows, Ctrl+V is handled natively by the multiline edit
        // control at the WM_PASTE message level, before wxEVT_CHAR_HOOK
        // can fire. ChatInputCtrl::MSWWindowProc intercepts WM_PASTE and
        // checks for a clipboard image before the native paste runs.
        _userInputCtrl->SetImagePasteHandler([this]() -> bool {
            if (m_chatClient->IsStreaming()) return false;
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

            // Auto-load the most recent conversation
            AutoLoadLastConversation();
            });
    }

    ~MyFrame() override
    {
        delete m_chatClient;
        delete m_chatDisplay;
        delete m_chatHistory;
        delete m_appState;
    }

    void OnClose(wxCloseEvent& evt)
    {
        m_isClosing = true;

        if (m_chatClient->IsStreaming()) {
            m_chatClient->StopGeneration();
        }

        if (!m_chatHistory->IsEmpty()) {
            AutoSaveConversation();
        }

        m_appState->SaveWindowState(this);

        evt.Skip(); // Allow the default close behavior
    }

    // ── Public interface for image attachment (used by drop target) ──
    bool AttachImageFromFile(const std::string& filePath)
    {
        if (m_chatClient->IsStreaming()) return false;

        wxFileName fname(wxString::FromUTF8(filePath));
        if (!fname.FileExists()) return false;

        std::string base64 = FileToBase64(filePath);
        if (base64.empty()) return false;

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

private:
    // ─── UI Controls ──────────────────────────────────────────────
    wxRichTextCtrl* _chatDisplayCtrl;
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
    wxPanel* _sidebarContent;
    wxPanel* _sidebarBorder;
    wxPanel* _inputContainer;
    wxPanel* _inputSeparator;

    // Sidebar
    wxPanel* _sidebarPanel;
    wxButton* _sidebarNewChat;
    wxScrolledWindow* _conversationList;
    wxBoxSizer* _conversationListSizer;
    bool m_sidebarVisible;
    bool m_isClosing;

    // Top bar controls
    wxStaticText* _modelLabel;
    StatusDot* _statusDot;

    // ─── Application Components ───────────────────────────────────
    AppState* m_appState;
    ChatClient* m_chatClient;
    ChatDisplay* m_chatDisplay;
    ChatHistory* m_chatHistory;

    // ─── Pending image attachment ─────────────────────────────────
    std::string m_pendingImageBase64;
    std::string m_pendingImageName;

    // ─── Model picker state ──────────────────────────────────────
    std::vector<std::string> m_pickerModels;

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

        // ── Sidebar ──────────────────────────────────────────────
        _sidebarPanel->SetBackgroundColour(t.bgSidebar);
        _sidebarContent->SetBackgroundColour(t.bgSidebar);
        _sidebarNewChat->SetBackgroundColour(t.modelPillBg);
        _sidebarNewChat->SetForegroundColour(t.textPrimary);
        _conversationList->SetBackgroundColour(t.bgSidebar);
        _sidebarBorder->SetBackgroundColour(t.borderSubtle);

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
        if (m_sidebarVisible) {
            RefreshConversationList();
        }

        // ── Force repaint ────────────────────────────────────────
        Refresh();
        Update();
    }

    void UpdateModelLabel()
    {
        std::string model = m_appState->GetModel();

        // Shorten: "pidrilkin/gemma3_27b_abliterated:Q4_K_M" → "gemma3_27b_abliterated:Q4_K_M"
        std::string display = model;
        size_t slash = model.rfind('/');
        if (slash != std::string::npos && slash + 1 < model.size()) {
            display = model.substr(slash + 1);
        }

        _modelLabel->SetLabel(wxString::FromUTF8(display) +
            wxString::FromUTF8(" \xE2\x96\xBE"));  // ▾ dropdown indicator
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

    void ClearPendingImage()
    {
        m_pendingImageBase64.clear();
        m_pendingImageName.clear();
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
        _inputSizer->Layout();

        if (!streaming) {
            _userInputCtrl->SetFocus();
        }
    }

    // ═════════════════════════════════════════════════════════════
    //  EVENT HANDLERS
    // ═════════════════════════════════════════════════════════════

    void OnAttachImage(wxCommandEvent&)
    {
        wxFileDialog dlg(this, "Select an image", "", "",
            "Image files (*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.webp)"
            "|*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.webp"
            "|All files (*.*)|*.*",
            wxFD_OPEN | wxFD_FILE_MUST_EXIST);

        if (dlg.ShowModal() == wxID_CANCEL) return;

        if (!AttachImageFromFile(dlg.GetPath().ToStdString())) {
            wxMessageBox("Failed to read image file", "Error", wxOK | wxICON_ERROR);
        }
    }

    void OnAssistantDelta(wxCommandEvent& event)
    {
        if (m_isClosing) return;

        m_chatDisplay->DisplayAssistantDelta(event.GetString().ToStdString());
    }

    void OnAssistantComplete(wxCommandEvent& event)
    {
        if (m_isClosing) return;

        std::string fullResponse = event.GetString().ToStdString();

        m_chatDisplay->DisplayAssistantComplete();
        m_chatHistory->UpdateLastAssistantMessage(fullResponse);
        m_chatClient->ResetStreamingState();
        SetStreamingState(false);

        // Auto-save conversation after each complete response
        AutoSaveConversation();

        if (auto* logger = m_appState->GetLogger())
            logger->information("Chat response completed");
    }

    void OnAssistantError(wxCommandEvent& event)
    {
        if (m_isClosing) return;

        std::string error = event.GetString().ToStdString();

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

            friendly = "Request timed out. Ollama may be busy loading a model — try again in a moment.";
        }
        else if (error.find("model") != std::string::npos &&
            error.find("not found") != std::string::npos) {

            friendly = "Model \"" + m_appState->GetModel() + "\" was not found. "
                "Open Settings to pick an available model, or run:\n ollama pull " + m_appState->GetModel();
        }
        else {
            friendly = "Error: " + error;
        }

        m_chatDisplay->DisplaySystemMessage(friendly);
        m_chatHistory->RemoveLastAssistantMessage();
        m_chatClient->ResetStreamingState();
        SetStreamingState(false);

        if (auto* logger = m_appState->GetLogger())
            logger->error("Chat error: " + error);
    }

    void OnToggleSidebar(wxCommandEvent&)
    {
        m_sidebarVisible = !m_sidebarVisible;
        _sidebarPanel->Show(m_sidebarVisible);

        if (m_sidebarVisible) {
            RefreshConversationList();
        }

        _contentSizer->Layout();
        GetSizer()->Layout();
    }

    void OnStopGeneration(wxCommandEvent&)
    {
        if (m_chatClient->IsStreaming()) {
            m_chatClient->StopGeneration();
            m_chatDisplay->DisplaySystemMessage("Generation stopped by user");
            m_chatHistory->RemoveLastAssistantMessage();
            SetStreamingState(false);
        }
    }

    void OnOpenSettings(wxCommandEvent&)
    {
        if (m_chatClient->IsStreaming()) {
            wxMessageBox("Cannot change settings while generating response",
                "Settings", wxOK | wxICON_INFORMATION);
            return;
        }

        SettingsDialog dlg(this, m_appState->GetModel(), m_appState->GetApiUrl(),
                           m_appState->GetThemeName());

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

            if (anyChange)
            {
                m_chatHistory->Clear();
                m_chatDisplay->Clear();
                ClearPendingImage();
                UpdateModelLabel();

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
        if (m_chatClient->IsStreaming()) return;

        // Start async model list fetch
        auto* thread = new ModelPickerThread(this, m_appState->GetApiUrl());
        if (thread->Run() != wxTHREAD_NO_ERROR) {
            delete thread;
            m_chatDisplay->DisplaySystemMessage(
                "Failed to fetch model list. Is Ollama running?");
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

        // Build popup menu with checkmarks on the active model
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

    void SwitchToModel(const std::string& newModel)
    {
        if (newModel == m_appState->GetModel()) return;
        if (m_chatClient->IsStreaming()) return;

        // Save current conversation before switching
        if (!m_chatHistory->IsEmpty()) {
            AutoSaveConversation();
        }

        bool mc, ac;
        m_appState->UpdateSettings(newModel, m_appState->GetApiUrl(), mc, ac);

        m_chatHistory->Clear();
        m_chatDisplay->Clear();
        ClearPendingImage();
        UpdateModelLabel();
        UpdateWindowTitle();

        m_chatDisplay->DisplaySystemMessage("Switched to " + newModel);
        _statusDot->SetConnected(true);
        _userInputCtrl->SetFocus();

        if (auto* logger = m_appState->GetLogger())
            logger->information("Quick-switched to model: " + newModel);
    }

    void OnNewChat(wxCommandEvent&)
    {
        if (m_chatClient->IsStreaming()) return;

        // Save current conversation if it has content
        if (!m_chatHistory->IsEmpty()) {
            AutoSaveConversation();
        }

        m_chatHistory->Clear();
        m_chatDisplay->Clear();
        ClearPendingImage();
        UpdateWindowTitle();
        RefreshConversationList();
        _userInputCtrl->SetFocus();

        if (auto* logger = m_appState->GetLogger())
            logger->information("New chat started");
    }

    void OnSaveConversation(wxCommandEvent&)
    {
        if (m_chatHistory->IsEmpty()) return;

        if (m_chatHistory->HasFilePath()) {
            // Already has a file — just save in place
            if (m_chatHistory->SaveToFile("", m_appState->GetModel())) {
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
            if (m_chatHistory->SaveToFile(path, m_appState->GetModel())) {
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
        if (m_chatClient->IsStreaming()) return;

        // Save current conversation before loading a new one
        if (!m_chatHistory->IsEmpty()) {
            AutoSaveConversation();
        }

        wxString defaultDir = wxString::FromUTF8(ChatHistory::GetConversationsDir());

        wxFileDialog dlg(this, "Open Conversation", defaultDir, "",
            "JSON files (*.json)|*.json|All files (*.*)|*.*",
            wxFD_OPEN | wxFD_FILE_MUST_EXIST);

        if (dlg.ShowModal() == wxID_CANCEL) return;

        std::string path = dlg.GetPath().ToStdString();
        std::string loadedModel;

        ChatHistory* newHistory = new ChatHistory();
        if (!newHistory->LoadFromFile(path, loadedModel)) {
            delete newHistory;
            wxMessageBox("Failed to load conversation file", "Error", wxOK | wxICON_ERROR);
            return;
        }

        // Replace current history
        delete m_chatHistory;
        m_chatHistory = newHistory;

        // Update model if the loaded conversation used a different one
        if (!loadedModel.empty() && loadedModel != m_appState->GetModel()) {
            bool mc, ac;
            m_appState->UpdateSettings(loadedModel, m_appState->GetApiUrl(), mc, ac);
            UpdateModelLabel();
        }

        // Replay the conversation to the display
        m_chatDisplay->Clear();
        ClearPendingImage();
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

        if (m_chatHistory->SaveToFile("", m_appState->GetModel())) {
            UpdateWindowTitle();
            if (m_sidebarVisible) RefreshConversationList();
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
                m_chatDisplay->DisplayUserMessage(content);
            }
            else if (role == "assistant") {
                m_chatDisplay->DisplayAssistantPrefix(m_appState->GetModel());
                m_chatDisplay->DisplayAssistantDelta(content);
                m_chatDisplay->DisplayAssistantComplete();
            }
            else if (role == "system") {
                m_chatDisplay->DisplaySystemMessage(content);
            }
        }
    }

    // ═════════════════════════════════════════════════════════════
    //  CONVERSATION LIST
    // ═════════════════════════════════════════════════════════════

    struct ConversationEntry {
        std::string filePath;
        std::string title;
        wxDateTime modTime;
    };

    void RefreshConversationList()
    {
        if (!_conversationListSizer) return;

        // Clear existing entries
        _conversationListSizer->Clear(true);

        // Scan conversations directory
        std::string convDir = ChatHistory::GetConversationsDir();
        wxDir dir(wxString::FromUTF8(convDir));
        if (!dir.IsOpened()) return;

        // Collect all conversation files with metadata
        std::vector<ConversationEntry> entries;

        wxString filename;
        bool found = dir.GetFirst(&filename, "*.json", wxDIR_FILES);
        while (found) {
            wxString fullPath = wxString::FromUTF8(convDir) +
                wxFileName::GetPathSeparator() + filename;

            ConversationEntry entry;
            entry.filePath = fullPath.ToStdString();

            // Read the title from the JSON file
            try {
                std::ifstream file(entry.filePath);
                if (file.is_open()) {
                    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
                    file.close();

                    Poco::JSON::Parser parser;
                    auto result = parser.parse(content);
                    auto obj = result.extract<Poco::JSON::Object::Ptr>();

                    if (obj->has("title")) {
                        entry.title = obj->getValue<std::string>("title");
                    }
                }
            }
            catch (...) {
                // Skip files we can't parse
            }

            if (entry.title.empty()) {
                entry.title = filename.ToStdString();
            }

            // Get file modification time
            wxFileName fn(fullPath);
            fn.GetTimes(nullptr, &entry.modTime, nullptr);

            entries.push_back(entry);
            found = dir.GetNext(&filename);
        }

        // Sort by modification time, newest first
        std::sort(entries.begin(), entries.end(),
            [](const ConversationEntry& a, const ConversationEntry& b) {
                return a.modTime.IsLaterThan(b.modTime);
            });

        // Create UI entries
        for (const auto& entry : entries) {
            auto* panel = new wxPanel(_conversationList, wxID_ANY);

            // Highlight the active conversation
            bool isActive = (entry.filePath == m_chatHistory->GetFilePath());
            panel->SetBackgroundColour(isActive ?
                m_appState->GetTheme().modelPillBg : m_appState->GetTheme().bgSidebar);

            auto* panelSizer = new wxBoxSizer(wxVERTICAL);

            // Title (truncated)
            std::string displayTitle = entry.title;
            if (displayTitle.size() > 35) {
                displayTitle = displayTitle.substr(0, 32) + "...";
            }
            auto* titleLabel = new wxStaticText(panel, wxID_ANY,
                wxString::FromUTF8(displayTitle));
            titleLabel->SetForegroundColour(m_appState->GetTheme().textPrimary);
            wxFont titleFont = titleLabel->GetFont();
            titleFont.SetPointSize(10);
            titleLabel->SetFont(titleFont);
            panelSizer->Add(titleLabel, 0, wxLEFT | wxRIGHT | wxTOP, 8);

            // Relative time
            std::string timeStr = RelativeTimeString(entry.modTime);
            auto* timeLabel = new wxStaticText(panel, wxID_ANY,
                wxString::FromUTF8(timeStr));
            timeLabel->SetForegroundColour(m_appState->GetTheme().textMuted);
            wxFont timeFont = timeLabel->GetFont();
            timeFont.SetPointSize(9);
            timeLabel->SetFont(timeFont);
            panelSizer->Add(timeLabel, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);

            panel->SetSizer(panelSizer);

            // Store file path in the panel name for click handling
            panel->SetName(wxString::FromUTF8(entry.filePath));

            // Bind click events on the panel and its children
            auto clickHandler = [this](wxMouseEvent& evt) {
                wxWindow* win = dynamic_cast<wxWindow*>(evt.GetEventObject());
                // Walk up to find the panel with the path stored in name
                while (win && win != _conversationList) {
                    wxString name = win->GetName();
                    if (name.EndsWith(".json")) {
                        LoadConversationFromPath(name.ToStdString());
                        return;
                    }
                    win = win->GetParent();
                }
                };

            panel->Bind(wxEVT_LEFT_UP, clickHandler);
            titleLabel->Bind(wxEVT_LEFT_UP, clickHandler);
            timeLabel->Bind(wxEVT_LEFT_UP, clickHandler);

            // Right-click context menu for delete
            auto rightClickHandler = [this](wxMouseEvent& evt) {
                wxWindow* win = dynamic_cast<wxWindow*>(evt.GetEventObject());
                std::string path;
                while (win && win != _conversationList) {
                    wxString name = win->GetName();
                    if (name.EndsWith(".json")) {
                        path = name.ToStdString();
                        break;
                    }
                    win = win->GetParent();
                }
                if (!path.empty()) {
                    ShowConversationContextMenu(path);
                }
                };

            panel->Bind(wxEVT_RIGHT_UP, rightClickHandler);
            titleLabel->Bind(wxEVT_RIGHT_UP, rightClickHandler);
            timeLabel->Bind(wxEVT_RIGHT_UP, rightClickHandler);

            // Hover effect
            auto enterHandler = [panel, this](wxMouseEvent&) {
                if (panel->GetBackgroundColour() != m_appState->GetTheme().modelPillBg) {
                    panel->SetBackgroundColour(m_appState->GetTheme().sidebarHover);
                    panel->Refresh();
                }
                };
            auto leaveHandler = [panel, this](wxMouseEvent&) {
                if (panel->GetBackgroundColour() != m_appState->GetTheme().modelPillBg) {
                    panel->SetBackgroundColour(m_appState->GetTheme().bgSidebar);
                    panel->Refresh();
                }
                };
            panel->Bind(wxEVT_ENTER_WINDOW, enterHandler);
            panel->Bind(wxEVT_LEAVE_WINDOW, leaveHandler);

            _conversationListSizer->Add(panel, 0, wxEXPAND);
        }

        _conversationList->FitInside();
        _conversationList->Layout();
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
        if (m_chatClient->IsStreaming()) return false;

        // Save current conversation before loading
        if (!m_chatHistory->IsEmpty()) {
            AutoSaveConversation();
        }

        std::string loadedModel;
        ChatHistory* newHistory = new ChatHistory();
        if (!newHistory->LoadFromFile(path, loadedModel)) {
            delete newHistory;
            return false;
        }

        // Replace current history
        delete m_chatHistory;
        m_chatHistory = newHistory;

        // Update model if needed
        if (!loadedModel.empty() && loadedModel != m_appState->GetModel()) {
            bool mc, ac;
            m_appState->UpdateSettings(loadedModel, m_appState->GetApiUrl(), mc, ac);
            UpdateModelLabel();
        }

        // Replay to display
        m_chatDisplay->Clear();
        ClearPendingImage();
        ReplayConversation();
        UpdateWindowTitle();
        RefreshConversationList();
        _userInputCtrl->SetFocus();

        if (auto* logger = m_appState->GetLogger())
            logger->information("Loaded conversation: " + m_chatHistory->GetTitle());

        return true;
    }

    static std::string RelativeTimeString(const wxDateTime& dt)
    {
        if (!dt.IsValid()) return "";

        wxDateTime now = wxDateTime::Now();
        wxTimeSpan diff = now.Subtract(dt);

        int minutes = (int)diff.GetMinutes();
        if (minutes < 1) return "Just now";
        if (minutes < 60) return std::to_string(minutes) + " min ago";

        int hours = (int)diff.GetHours();
        if (hours < 24) return std::to_string(hours) + "h ago";

        int days = diff.GetDays();
        if (days == 1) return "Yesterday";
        if (days < 7) return std::to_string(days) + " days ago";
        if (days < 30) return std::to_string(days / 7) + "w ago";

        return dt.Format("%b %d").ToStdString();
    }

    void ShowConversationContextMenu(const std::string& filePath)
    {
        wxMenu menu;
        menu.Append(wxID_DELETE, "Delete conversation");

        menu.Bind(wxEVT_MENU, [this, filePath](wxCommandEvent&) {
            DeleteConversation(filePath);
            }, wxID_DELETE);

        PopupMenu(&menu);
    }

    void DeleteConversation(const std::string& filePath)
    {
        int result = wxMessageBox(
            "Delete this conversation? This cannot be undone.",
            "Delete Conversation",
            wxYES_NO | wxICON_WARNING);

        if (result != wxYES) return;

        // If deleting the currently active conversation, clear the display
        bool wasActive = (filePath == m_chatHistory->GetFilePath());

        // Delete the file
        if (wxRemoveFile(wxString::FromUTF8(filePath))) {
            if (wasActive) {
                m_chatHistory->Clear();
                m_chatDisplay->Clear();
                ClearPendingImage();
                UpdateWindowTitle();
            }

            RefreshConversationList();

            if (auto* logger = m_appState->GetLogger())
                logger->information("Deleted conversation: " + filePath);
        }
        else {
            wxMessageBox("Failed to delete conversation file", "Error",
                wxOK | wxICON_ERROR);
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
        if (evt.GetActive() && !m_chatClient->IsStreaming())
            _userInputCtrl->SetFocus();
        evt.Skip();
    }

    void OnSendMessage(wxCommandEvent&)
    {
        if (m_chatClient->IsStreaming()) return;

        std::string userInput = _userInputCtrl->GetValue().ToStdString();
        if (userInput.empty() && m_pendingImageBase64.empty()) return;

        if (userInput.empty() && !m_pendingImageBase64.empty())
            userInput = "What is in this image?";

        if (!m_pendingImageBase64.empty())
            m_chatDisplay->DisplayUserMessage("[" + m_pendingImageName + "] " + userInput);
        else
            m_chatDisplay->DisplayUserMessage(userInput);

        _userInputCtrl->Clear();
        { wxCommandEvent e(wxEVT_TEXT, _userInputCtrl->GetId()); OnUserInputChanged(e); }

        m_chatHistory->AddUserMessage(userInput);
        std::string body = m_chatHistory->BuildChatRequestJson(m_appState->GetModel(), true);

        if (!m_pendingImageBase64.empty()) {
            body = InjectImageIntoRequest(body, m_pendingImageBase64);
            ClearPendingImage();
        }

        if (auto* logger = m_appState->GetLogger())
            logger->debug("Request sent (" + std::to_string(body.size()) + " bytes)");

        m_chatHistory->AddAssistantPlaceholder();
        m_chatDisplay->DisplayAssistantPrefix(m_appState->GetModel());

        SetStreamingState(true);
        if (!m_chatClient->SendMessage(m_appState->GetModel(), m_appState->GetApiUrl(), body)) {
            SetStreamingState(false);
            m_chatDisplay->DisplaySystemMessage("Failed to start chat request");
            m_chatHistory->RemoveLastAssistantMessage();
        }
    }
};

// ═══════════════════════════════════════════════════════════════════
//  ImageDropTarget Implementation
// ═══════════════════════════════════════════════════════════════════

bool ImageDropTarget::OnDropFiles(wxCoord /*x*/, wxCoord /*y*/,
    const wxArrayString& filenames)
{
    // Accept the first valid image file from the drop
    for (const auto& file : filenames) {
        if (IsImageFile(file)) {
            return m_frame->AttachImageFromFile(file.ToStdString());
        }
    }
    return false; // No valid image found in drop
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