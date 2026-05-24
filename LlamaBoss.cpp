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
#include <wx/filefn.h>
#include <wx/dcbuffer.h>
#include <wx/dnd.h>
#include <wx/clipbrd.h>
#include <wx/mstream.h>
#include <wx/dir.h>
#include <wx/scrolwin.h>
#include <wx/wrapsizer.h>
#include <wx/statline.h>

#ifdef __WXMSW__
#include <windows.h>
#endif

#include <vector>
#include <string>
#include <string_view>
#include <sstream>
#include "ui_event_post.h"
#include <fstream>
#include <algorithm>
#include <memory>
#include <functional>
#include <utility>
#include <filesystem>
#include <system_error>

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
#include "cmd_executor.h"
#include "python_runner.h"
#include "tool_path.h"
#include "tool_grep.h"
#include "tool_call_parser.h"  // ToolCallStreamDetector for hiding raw <tool_call> blocks
#include "agent_controller.h"
#include "tool_protocol.h"     // Phase 3b: tool-call protocol detection
#include "tool_router.h"       // Phase 3c-i: BuildToolsArrayJson for native requests
#include "tool_approval.h"     // Phase 6 approval cards
#include "project_manager.h"   // Projects Phase 1/2
#include "project_attach_dialog.h"
#include "project_status_strip.h"

// ── File-local support modules (extracted helpers) ───────────────
#include "lb_string_utils.h"
#include "skill_authoring_support.h"
#include "skill_prompt_builder.h"
#include "goal_prompt_builder.h"
#include "agent_prompt_builder.h"
#include "goal_verifier_support.h"
#include "python_package_recovery.h"
#include "artifact_presentation.h"
#include "drop_import_controller.h"

// ── Extracted widget & coordinator headers ────────────────────────
#include "widgets.h"
#include "chat_input_ctrl.h"
#include "chat_display_ctrl.h"
#include "ui_builder.h"
#include "model_switcher.h"
#include "conversation_controller.h"
#include "ascii_animation.h"

// ─── Application version ─────────────────────────────────────
static const char* LLAMABOSS_VERSION = "0.1.2";

// Native menu command ids. Keep above wxID_HIGHEST to avoid collisions
// with stock wxWidgets commands.
enum {
    ID_ANIMATION_TIMER = wxID_HIGHEST + 2000,
    ID_ASSISTANT_DELTA_FLUSH_TIMER,

    ID_PROJECT_NEW = wxID_HIGHEST + 2100,
    ID_PROJECT_ATTACH,
    ID_PROJECT_OPEN_FOLDER,
    ID_PROJECT_OPEN_INSTRUCTIONS,
    ID_PROJECT_ADD_SOURCES,
    ID_PROJECT_OPEN_SOURCES_FOLDER,
    ID_PROJECT_NEW_WORKFLOW,
    ID_PROJECT_NEW_WORKFLOW_WITH_SCRIPT,
    ID_PROJECT_OPEN_WORKFLOW,
    ID_PROJECT_OPEN_WORKFLOWS_FOLDER,
    ID_PROJECT_CLEAR,
    ID_PROJECT_DELETE,
    ID_SKILL_NEW,
    ID_SKILL_NEW_WITH_SCRIPT,
    ID_SKILL_OPEN,
    ID_SKILL_OPEN_FOLDER
};

namespace {

// Global Skills live as:
//   <user>\LlamaBoss\Skills\<Skill Folder>\SKILL.md
// ProjectManager keeps an internal legacy "<stem>.workflow.md" name for
// resolver compatibility, but user-facing Skill prompt/UI text should show
// the actual folder-based Skill identity instead of that synthetic filename.
std::string LbSkillDisplayNameFromContractPath(const SkillInfo& skill)
{
    std::string path = skill.path;
    std::replace(path.begin(), path.end(), '\\', '/');

    const size_t contractSlash = path.find_last_of('/');
    if (contractSlash != std::string::npos && contractSlash > 0) {
        const size_t folderSlash = path.find_last_of('/', contractSlash - 1);
        const size_t folderStart = folderSlash == std::string::npos ? 0 : folderSlash + 1;
        const std::string folderName = path.substr(folderStart, contractSlash - folderStart);
        if (!folderName.empty()) return folderName;
    }

    return skill.name;
}

long long LbPathMTimeTicks(const std::string& path)
{
    if (path.empty()) return 0;

    // Use std::filesystem with error_code so cache probing never triggers
    // wxWidgets log popups on Windows directories that cannot expose times.
    // A failed probe simply returns 0 and lets explicit cache invalidation
    // handle normal in-app project/source/workflow changes.
    std::error_code ec;

#ifdef _WIN32
    std::filesystem::path fsPath(wxString::FromUTF8(path).ToStdWstring());
#else
    std::filesystem::path fsPath = std::filesystem::u8path(path);
#endif

    const auto status = std::filesystem::status(fsPath, ec);
    if (ec || !std::filesystem::exists(status)) return 0;

    const auto mtime = std::filesystem::last_write_time(fsPath, ec);
    if (ec) return 0;

    return static_cast<long long>(mtime.time_since_epoch().count());
}

const wxColour& LbInteractiveAccentForTheme(const ThemeData& theme)
{
    // The original LlamaBoss Dark theme intentionally uses the mint assistant
    // color for small interactive highlights (paperclip, robot, + New Chat).
    // Other themes keep their author/palette-correct primary accent so assistant
    // body text can remain a true foreground color instead of driving UI chrome.
    return (theme.name == "dark") ? theme.chatAssistant : theme.accentButton;
}


wxButton* LbMakeThemedAccentButton(wxWindow* parent, wxWindowID id,
                                   const wxString& label, const ThemeData& theme,
                                   int height = 32)
{
    auto* button = new wxButton(parent, id, label,
                                wxDefaultPosition, wxSize(-1, height),
                                wxBORDER_NONE);
    wxFont font = button->GetFont();
    font.SetPointSize(10);
    font.SetWeight(wxFONTWEIGHT_SEMIBOLD);
    button->SetFont(font);
    button->SetBackgroundColour(theme.accentButton);
    button->SetForegroundColour(theme.accentButtonText);
    return button;
}

wxButton* LbMakeThemedFlatButton(wxWindow* parent, wxWindowID id,
                                 const wxString& label, const ThemeData& theme,
                                 int height = 32)
{
    auto* button = new wxButton(parent, id, label,
                                wxDefaultPosition, wxSize(-1, height),
                                wxBORDER_NONE);
    wxFont font = button->GetFont();
    font.SetPointSize(10);
    font.SetWeight(wxFONTWEIGHT_SEMIBOLD);
    button->SetFont(font);
    button->SetBackgroundColour(theme.bgDialogSurface);
    button->SetForegroundColour(theme.textMuted);
    return button;
}

// Small themed replacement for wxTextEntryDialog.  Native wxTextEntryDialog
// ignores the app palette on Windows, which makes the New Project prompt look
// detached from themed modal flows such as Attach / Manage Project.
class LbThemedTextEntryDialog final : public wxDialog
{
public:
    LbThemedTextEntryDialog(wxWindow* parent,
                            const ThemeData& theme,
                            const wxString& title,
                            const wxString& prompt,
                            const wxString& actionLabel)
        : wxDialog(parent, wxID_ANY, title,
                   wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
        , m_theme(theme)
    {
        SetBackgroundColour(m_theme.bgDialogSurface);

        wxFont base = GetFont();
        base.SetPointSize(11);
        SetFont(base);

        auto* top = new wxBoxSizer(wxVERTICAL);

        auto* label = new wxStaticText(this, wxID_ANY, prompt);
        label->SetForegroundColour(m_theme.textPrimary);
        top->Add(label, 0, wxALL, 12);

        m_input = new wxTextCtrl(this, wxID_ANY, wxEmptyString,
                                 wxDefaultPosition, wxSize(340, -1),
                                 wxTE_PROCESS_ENTER);
        m_input->SetBackgroundColour(m_theme.bgInputField);
        m_input->SetForegroundColour(m_theme.textPrimary);
        top->Add(m_input, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 12);

        auto* line = new wxPanel(this, wxID_ANY,
                                 wxDefaultPosition, wxSize(-1, 1));
        line->SetBackgroundColour(m_theme.borderSubtle);
        top->Add(line, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 12);

        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        buttons->AddStretchSpacer(1);

        m_okButton = LbMakeThemedAccentButton(this, wxID_OK, actionLabel, m_theme);
        m_okButton->SetMinSize(wxSize(96, 32));

        auto* cancelButton = LbMakeThemedFlatButton(this, wxID_CANCEL, "Cancel", m_theme);
        cancelButton->SetMinSize(wxSize(96, 32));

        buttons->Add(m_okButton, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
        buttons->Add(cancelButton, 0, wxALIGN_CENTER_VERTICAL);
        top->Add(buttons, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 12);

        SetSizerAndFit(top);
        SetMinSize(wxSize(420, 190));
        CentreOnParent();
        SetAffirmativeId(wxID_OK);
        SetEscapeId(wxID_CANCEL);
        m_okButton->SetDefault();
        UpdateOkButton();

        m_input->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
            UpdateOkButton();
        });
        m_input->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) {
            if (m_okButton && m_okButton->IsEnabled()) {
                EndModal(wxID_OK);
            }
        });

        ApplyDarkTitleBar(this, m_theme.name != "light");
        m_input->SetFocus();
    }

    wxString GetValue() const
    {
        return m_input ? m_input->GetValue() : wxString();
    }

private:
    void UpdateOkButton()
    {
        if (!m_okButton || !m_input) return;
        wxString value = m_input->GetValue();
        value.Trim(true);
        value.Trim(false);
        m_okButton->Enable(!value.empty());
    }

    ThemeData  m_theme;
    wxTextCtrl* m_input = nullptr;
    wxButton*   m_okButton = nullptr;
};


} // namespace

// ─── Forward declaration ─────────────────────────────────────────
class MyFrame;

// ─── Drag-and-drop target for files ─────────────────────────────
// Handles existing image/text attachments plus PDF input polish.
class ImageDropTarget : public wxFileDropTarget
{
public:
    ImageDropTarget(MyFrame* frame) : m_frame(frame) {}
    virtual bool OnDropFiles(wxCoord x, wxCoord y,
        const wxArrayString& filenames) override;
private:
    MyFrame* m_frame;
};

// ─── File-local helpers for project source summaries ─────────────
namespace {

std::string ProjectSource_HumanBytes(unsigned long long bytes)
{
    const double value = static_cast<double>(bytes);
    if (value < 1024.0) {
        return std::string(wxString::Format("%.0f B", value).ToUTF8().data());
    }
    if (value < 1024.0 * 1024.0) {
        return std::string(wxString::Format("%.1f KB", value / 1024.0).ToUTF8().data());
    }
    if (value < 1024.0 * 1024.0 * 1024.0) {
        return std::string(wxString::Format("%.1f MB", value / (1024.0 * 1024.0)).ToUTF8().data());
    }
    return std::string(wxString::Format("%.1f GB",
                       value / (1024.0 * 1024.0 * 1024.0)).ToUTF8().data());
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════
//  Chat State Machine
// ═══════════════════════════════════════════════════════════════════
enum class ChatState {
    Idle,
    Streaming,
    RunningCmd,
    RunningGrep,
    RunningPython,
    AwaitingApproval,
};

// ═══════════════════════════════════════════════════════════════════
class MyFrame : public wxFrame, public AgentEventSink {
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
        , m_cmdExecutor(std::make_unique<CmdExecutor>(this, m_alive))
        , m_pythonRunner(std::make_unique<PythonRunner>(this, m_alive))
        , m_grepExecutor(std::make_unique<GrepExecutor>(this, m_alive))
        , m_chatState(ChatState::Idle)
        , m_agentModeEnabled(false)

    {
        // Ensure data directories exist
        ServerManager::EnsureDataDirs();

        // Initialize application state first
        if (!m_appState->Initialize()) {
            wxMessageBox("Failed to initialize application state", "Startup Error",
                wxOK | wxICON_ERROR);
        }

        // Seed agent-mode flag from the persisted default. Happens here
        // (rather than in the init list) so m_appState->Initialize() has
        // already populated m_agentDefaultOn from wxFileConfig.
        m_agentModeEnabled = m_appState->GetAgentDefaultOn();

        // Wire the secrets store into the Python runner so
        // python_run_script subprocesses get the configured
        // Connections injected as environment variables.  Must come
        // after m_appState->Initialize() (the lazy ctor inside
        // GetSecretsStore touches the user-local data dir, which
        // wxStandardPaths only resolves correctly after SetAppName
        // has been called by the wxApp -- guaranteed here).
        m_pythonRunner->SetSecretsStore(m_appState->GetSecretsStore());

        // Create server manager (spawns llama-server process)
        m_serverManager = std::make_unique<ServerManager>(this, m_alive, m_appState->GetLogger());

        SetBackgroundColour(m_appState->GetTheme().bgMain);

        // Projects: actions live on the ProjectStatusStrip (built below),
        // not on a native Windows menu bar.  The strip's popup menu uses
        // these wxEVT_MENU bindings exactly as the old menu bar did, so
        // the OnProject* handlers stay unchanged.
        Bind(wxEVT_MENU, &MyFrame::OnProjectNew, this, ID_PROJECT_NEW);
        Bind(wxEVT_MENU, &MyFrame::OnProjectAttach, this, ID_PROJECT_ATTACH);
        Bind(wxEVT_MENU, &MyFrame::OnProjectOpenFolder, this, ID_PROJECT_OPEN_FOLDER);
        Bind(wxEVT_MENU, &MyFrame::OnProjectOpenInstructions, this, ID_PROJECT_OPEN_INSTRUCTIONS);
        Bind(wxEVT_MENU, &MyFrame::OnProjectAddSources, this, ID_PROJECT_ADD_SOURCES);
        Bind(wxEVT_MENU, &MyFrame::OnProjectOpenSourcesFolder, this, ID_PROJECT_OPEN_SOURCES_FOLDER);
        Bind(wxEVT_MENU, &MyFrame::OnProjectNewWorkflow, this, ID_PROJECT_NEW_WORKFLOW);
        Bind(wxEVT_MENU, &MyFrame::OnProjectNewWorkflowWithScript, this, ID_PROJECT_NEW_WORKFLOW_WITH_SCRIPT);
        Bind(wxEVT_MENU, &MyFrame::OnProjectOpenWorkflow, this, ID_PROJECT_OPEN_WORKFLOW);
        Bind(wxEVT_MENU, &MyFrame::OnProjectOpenWorkflowsFolder, this, ID_PROJECT_OPEN_WORKFLOWS_FOLDER);
        Bind(wxEVT_MENU, &MyFrame::OnSkillNew, this, ID_SKILL_NEW);
        Bind(wxEVT_MENU, &MyFrame::OnSkillNewWithScript, this, ID_SKILL_NEW_WITH_SCRIPT);
        Bind(wxEVT_MENU, &MyFrame::OnSkillOpen, this, ID_SKILL_OPEN);
        Bind(wxEVT_MENU, &MyFrame::OnSkillOpenFolder, this, ID_SKILL_OPEN_FOLDER);
        Bind(wxEVT_MENU, &MyFrame::OnProjectClear, this, ID_PROJECT_CLEAR);
        Bind(wxEVT_MENU, &MyFrame::OnProjectDelete, this, ID_PROJECT_DELETE);

        auto* mainSizer = new wxBoxSizer(wxVERTICAL);

        // ─── TOP BAR (via UIBuilder) ─────────────────────────────────
        auto tb = UIBuilder::BuildTopBar(this, mainSizer, m_appState->GetTheme());
        _toolbarPanel   = tb.toolbarPanel;
        _titleLabel     = tb.titleLabel;
        _modelPill      = tb.modelPill;
        _modelPillLeftBracket  = tb.modelPillLeftBracket;
        _modelLabel     = tb.modelLabel;
        _modelPillRightBracket = tb.modelPillRightBracket;
        _statusDot      = tb.statusDot;
        _protocolChip   = tb.protocolChip;
        _sidebarToggle  = tb.sidebarToggle;
        _newChatButton  = tb.newChatButton;
        _settingsButton = tb.settingsButton;
        _aboutButton    = tb.aboutButton;
        _topSeparator   = tb.topSeparator;

        // ─── PROJECT STATUS STRIP ────────────────────────────────────
        // Single-line strip showing the active project for the current
        // chat.  Replaces the native menu bar; the same OnProject*
        // handlers are reused via the strip's popup menu.
        ProjectStatusStrip::Callbacks stripCallbacks;
        stripCallbacks.onMenuRequested = [this](wxWindow* anchor) {
            ShowProjectPopupMenu(anchor);
        };
        stripCallbacks.onSkillMenuRequested = [this](wxWindow* anchor) {
            ShowProjectPopupMenu(anchor, true);
        };
        stripCallbacks.onAttachRequested = [this]() {
            wxCommandEvent e;
            OnProjectAttach(e);
        };
        // Goal action ([ /goal ] when empty, [ details ] when set)
        // routes to the same detail-card flow the old separate goal
        // strip used.  See ProjectStatusStrip merged layout (Goals
        // Phase 16 -> UI polish: project + goal on one row).
        stripCallbacks.onGoalActionClicked = [this]() {
            DisplayGoalStatus();
        };
        m_projectStrip = std::make_unique<ProjectStatusStrip>(
            this, m_appState->GetTheme(), stripCallbacks);
        mainSizer->Add(m_projectStrip->GetPanel(), 0, wxEXPAND);

        // NOTE: the old separate Goal status strip (BuildGoalStatusStrip)
        // has been merged into ProjectStatusStrip as the right-hand pair
        // on the same row.  RefreshGoalStatusStrip() is now a thin alias
        // for RefreshProjectStrip() so the existing ~17 call sites keep
        // working without churn.

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
        sidebarCallbacks.onCollapsedProjectsChanged =
            [this](const std::vector<std::string>& ids) {
                m_appState->SetCollapsedProjectIds(ids);
            };
        sidebarCallbacks.onChatContextMenuRequested =
            [this](const std::vector<std::string>& paths, wxWindow* anchor) {
                ShowSidebarChatContextMenu(paths, anchor);
            };
        sidebarCallbacks.onProjectHeaderContextMenuRequested =
            [this](const std::string& projectId, wxWindow* anchor) {
                ShowSidebarProjectHeaderContextMenu(projectId, anchor);
            };
        sidebarCallbacks.onChatsDroppedOnProject =
            [this](const std::vector<std::string>& paths,
                   const std::string& targetProjectId) {
                MoveChatsToProject(paths, targetProjectId);
            };
        m_sidebar = std::make_unique<ConversationSidebar>(
            this, m_appState->GetTheme(),
            sidebarCallbacks,
            m_appState->GetCollapsedProjectIds());
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

        // ─── Agent-mode toggle (Phase 4) ─────────────────────────────
        // Sits right after the attach button in _inputSizer.  Visual
        // state: muted when off, interactive-accent-colored when on.  Click flips
        // m_agentModeEnabled and re-tints.
        _agentToggleButton = new wxButton(
            _inputContainer, wxID_ANY,
            wxString::FromUTF8("\xF0\x9F\xA4\x96"),   // 🤖
            wxDefaultPosition, wxSize(36, 36),
            wxBORDER_NONE);
        _agentToggleButton->SetBackgroundColour(m_appState->GetTheme().bgInputArea);
        _agentToggleButton->SetForegroundColour(
            m_agentModeEnabled ? LbInteractiveAccentForTheme(m_appState->GetTheme())
                               : m_appState->GetTheme().textMuted);
        _agentToggleButton->SetFont(wxFont(wxFontInfo(14)));
        _agentToggleButton->SetToolTip(
            "Agent mode: when ON, the model can call tools (read, ls, open, grep, pwd, powershell) "
            "to answer your questions.  Click to toggle.");
        // Insert right after attach button.  _inputSizer was built
        // as: [attach][userInput][send/stop].  Find attach's index
        // via GetChildren() to avoid hardcoding a position in case
        // UIBuilder changes later.
        {
            size_t attachIdx = 0;
            const auto& children = _inputSizer->GetChildren();
            for (size_t i = 0; i < children.size(); ++i) {
                if (children[i]->GetWindow() == _attachButton) {
                    attachIdx = i;
                    break;
                }
            }
            _inputSizer->Insert(attachIdx + 1, _agentToggleButton, 0,
                                wxALIGN_CENTER_VERTICAL | wxLEFT, 4);
        }

        _rightPanel->SetSizer(rightSizer);
        _contentSizer->Add(_rightPanel, 1, wxEXPAND);
        mainSizer->Add(_contentSizer, 1, wxEXPAND);
        SetSizer(mainSizer);

        // ─── Setup fonts ─────────────────────────────────────────────
        // Font size is user-configurable via Settings; persisted in AppState.
        wxFont codeFont = m_appState->CreateMonospaceFont(m_appState->GetFontSize());
        _chatDisplayCtrl->SetFont(codeFont);
        _userInputCtrl->SetFont(codeFont);

        m_chatDisplay = std::make_unique<ChatDisplay>(_chatDisplayCtrl);
        m_chatDisplay->SetFont(codeFont);
        m_chatDisplay->ApplyTheme(m_appState->GetTheme());

        // Approval card buttons route back through HandleApprovalCommand
        // using the same chat-scoped semantics as the typed-command
        // fallback in TryHandlePendingApprovalInput.  Click and type both
        // converge on a single resolution path.
        m_chatDisplay->SetApprovalCallback(
            [this](ChatDisplay::ApprovalChoice choice) {
                switch (choice) {
                case ChatDisplay::ApprovalChoice::Once:
                    HandleApprovalCommand(true,  /*rememberForChat=*/false);
                    break;
                case ChatDisplay::ApprovalChoice::Always:
                    HandleApprovalCommand(true,  /*rememberForChat=*/true);
                    break;
                case ChatDisplay::ApprovalChoice::Deny:
                    HandleApprovalCommand(false);
                    break;
                }
            });


        // Drag-and-drop document imports keep their shared cwd-copy,
        // status-message, and chip-attach flow outside the frame.  The
        // frame still supplies the app-specific callbacks.
        DropImportControllerCallbacks dropImportCallbacks;
        dropImportCallbacks.isBusy = [this]() {
            return IsBusy();
        };
        dropImportCallbacks.displaySystemMessage = [this](const std::string& message) {
            if (m_chatDisplay) m_chatDisplay->DisplaySystemMessage(message);
        };
        dropImportCallbacks.resolveCurrentCwd = [this]() {
            return ResolveCurrentCwd();
        };
        dropImportCallbacks.restoreComposerFocusDeferred = [this]() {
            RestoreComposerFocusDeferred();
        };
        dropImportCallbacks.attachPdfFile =
            [this](const std::string& absPath, const std::string& relPath) {
                return m_attachments->AttachPdfFile(absPath, relPath);
            };
        dropImportCallbacks.attachSpreadsheetFile =
            [this](const std::string& absPath, const std::string& relPath) {
                return m_attachments->AttachSpreadsheetFile(absPath, relPath);
            };
        dropImportCallbacks.attachDocxFile =
            [this](const std::string& absPath, const std::string& relPath) {
                return m_attachments->AttachDocxFile(absPath, relPath);
            };
        m_dropImportController =
            std::make_unique<DropImportController>(std::move(dropImportCallbacks));

        _statusDot->SetColors(m_appState->GetTheme().accentButton,
                              m_appState->GetTheme().textMuted);

        // ─── Create agent controller (Phase 5) ───────────────────────
        // Phase 5: MyFrame is the AgentEventSink — it receives
        // structured loop-progress events and translates them to UI
        // operations.  The Phase-4 ChatDisplay* slot is gone; tool
        // blocks now arrive via OnAgentToolBlock and are forwarded
        // to the display from there.  Callbacks are wired below,
        // after all coordinators are in place.
        m_agentController = std::make_unique<AgentController>(
            m_chatHistory,
            this,
            m_appState.get(),
            m_grepExecutor.get(),
            m_cmdExecutor.get(),
            m_pythonRunner.get());

        // ─── Create coordinators ─────────────────────────────────────
        m_modelSwitcher = std::make_unique<ModelSwitcher>(
            *m_appState, *m_serverManager, m_chatDisplay.get(),
            m_chatHistory, *m_attachments, _statusDot, _modelLabel, this);

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
            /*isBusy*/                [this]() { return IsBusy(); },
            /*onProjectStateChanged*/ [this]() {
                RefreshProjectStrip();
                RefreshGoalStatusStrip();
            }
        });

        // Initial strip render now that the controller can drive refreshes.
        RefreshProjectStrip();
        RefreshGoalStatusStrip();

        // ─── AgentController callbacks ───────────────────────────────
        // Phase 5: Callbacks now contain only logic concerns
        // (sendRequest, buildToolContext, buildSystemPrompt,
        // bumpGenerationId, getActiveProtocol).  The Phase-4 UI-shaped
        // entries (beginNextIteration, onLoopEnd) moved into the
        // AgentEventSink methods further down (OnAgentIterationBegin,
        // OnAgentLoopEnd) — same body, cleaner separation.
        m_agentController->SetCallbacks({
            /*sendRequest*/ [this](const std::string& model,
                                   const std::string& body,
                                   unsigned long      genId) {
                return m_chatClient->SendMessage(
                    model, m_appState->GetApiUrl(), body, genId);
            },
            /*buildToolContext*/ [this]() { return BuildToolContext(); },
            /*buildSystemPrompt*/ [this]() { return BuildAgentSystemPrompt(); },
            /*bumpGenerationId*/ [this]() {
                ++m_generationId;
                return m_generationId;
            },
            /*getActiveProtocol*/ [this]() { return _activeProtocol; },
        });

        // ─── Bind events ─────────────────────────────────────────────
        _sendButton->Bind(wxEVT_BUTTON, &MyFrame::OnSendMessage, this);
        _stopButton->Bind(wxEVT_BUTTON, &MyFrame::OnStopGeneration, this);

        // Animation timer
        Bind(wxEVT_TIMER, &MyFrame::OnAnimationTimer, this, m_animTimer.GetId());

        // Streamed assistant chunks can arrive very quickly from local models.
        // Batch them into one UI update per frame-ish interval so wxRichTextCtrl
        // does far less Freeze/Thaw/scroll work while still feeling live.
        Bind(wxEVT_TIMER, &MyFrame::OnAssistantDeltaFlushTimer, this,
             m_assistantDeltaFlushTimer.GetId());


        // Attach (📎) button hover — use the theme's interactive accent.
        _attachButton->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& e) {
            _attachButton->SetForegroundColour(LbInteractiveAccentForTheme(m_appState->GetTheme()));
            _attachButton->Refresh();
            e.Skip();
            });
        _attachButton->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) {
            _attachButton->SetForegroundColour(m_appState->GetTheme().textMuted);
            _attachButton->Refresh();
            e.Skip();
            });
        _attachButton->Bind(wxEVT_BUTTON, &MyFrame::OnAttachImage, this);

        // Agent toggle — hover mirrors attach styling; click
        // flips m_agentModeEnabled and re-tints.
        _agentToggleButton->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& e) {
            if (!m_agentModeEnabled)
                _agentToggleButton->SetForegroundColour(m_appState->GetTheme().textPrimary);
            _agentToggleButton->Refresh();
            e.Skip();
        });
        _agentToggleButton->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) {
            _agentToggleButton->SetForegroundColour(
                m_agentModeEnabled ? LbInteractiveAccentForTheme(m_appState->GetTheme())
                                   : m_appState->GetTheme().textMuted);
            _agentToggleButton->Refresh();
            e.Skip();
        });
        _agentToggleButton->Bind(wxEVT_BUTTON, &MyFrame::OnToggleAgentMode, this);
        
        
        
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

        // New Chat (+) button hover — use the theme's interactive accent.
        _newChatButton->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& e) {
            _newChatButton->SetForegroundColour(LbInteractiveAccentForTheme(m_appState->GetTheme()));
            _newChatButton->Refresh();
            e.Skip();
            });
        _newChatButton->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) {
            _newChatButton->SetForegroundColour(m_appState->GetTheme().textMuted);
            _newChatButton->Refresh();
            e.Skip();
            });
        _newChatButton->Bind(wxEVT_BUTTON, &MyFrame::OnNewChat, this);


        // Sidebar/history toggle hover — match New Chat's interactive accent affordance.
        _sidebarToggle->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& e) {
            _sidebarToggle->SetForegroundColour(LbInteractiveAccentForTheme(m_appState->GetTheme()));
            _sidebarToggle->Refresh();
            e.Skip();
            });
        _sidebarToggle->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) {
            _sidebarToggle->SetForegroundColour(m_appState->GetTheme().textMuted);
            _sidebarToggle->Refresh();
            e.Skip();
            });
        _sidebarToggle->Bind(wxEVT_BUTTON, &MyFrame::OnToggleSidebar, this);
        _aboutButton->Bind(wxEVT_BUTTON, &MyFrame::OnAbout, this);
        Bind(wxEVT_ACTIVATE, &MyFrame::OnFrameActivate, this);

        Bind(wxEVT_ASSISTANT_DELTA, &MyFrame::OnAssistantDelta, this);
        Bind(wxEVT_ASSISTANT_COMPLETE, &MyFrame::OnAssistantComplete, this);
        Bind(wxEVT_ASSISTANT_ERROR, &MyFrame::OnAssistantError, this);

        // ─── /cmd (Phase 1 tool executor) ─────────────────────────
        Bind(wxEVT_CMD_COMPLETE, &MyFrame::OnCmdComplete, this);
        Bind(wxEVT_CMD_ERROR,    &MyFrame::OnCmdError,    this);

        // ─── controlled Python helper runner ─────────────────────
        Bind(wxEVT_PYTHON_COMPLETE, &MyFrame::OnPythonComplete, this);
        Bind(wxEVT_PYTHON_ERROR,    &MyFrame::OnPythonError,    this);

        // ─── /grep (Phase 3 threaded executor) ────────────────────
        Bind(wxEVT_GREP_COMPLETE, &MyFrame::OnGrepComplete, this);

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

        // Bracket widgets are part of the same affordance -- clicking
        // them opens the picker exactly like clicking the dot or
        // model label.
        if (_modelPillLeftBracket) {
            _modelPillLeftBracket->Bind(wxEVT_LEFT_UP,  pillClick);
            _modelPillLeftBracket->Bind(wxEVT_RIGHT_UP, pillRightClick);
        }
        if (_modelPillRightBracket) {
            _modelPillRightBracket->Bind(wxEVT_LEFT_UP,  pillClick);
            _modelPillRightBracket->Bind(wxEVT_RIGHT_UP, pillRightClick);
        }

        // Hover recoloring: brackets light up in mint when the pointer
        // is anywhere over the pill (any child widget), back to muted
        // on leave.  We bind on every child because wxWidgets does NOT
        // propagate enter/leave events from children up to the parent
        // panel on MSW -- the panel-level enter would fire only when
        // the cursor entered the bare panel space, which is barely
        // any pixels once the children are laid out.
        auto pillEnter = [this](wxMouseEvent& e) {
            const ThemeData& th = m_appState->GetTheme();
            if (_modelPillLeftBracket) {
                _modelPillLeftBracket->SetForegroundColour(LbInteractiveAccentForTheme(th));
                _modelPillLeftBracket->Refresh();
            }
            if (_modelPillRightBracket) {
                _modelPillRightBracket->SetForegroundColour(LbInteractiveAccentForTheme(th));
                _modelPillRightBracket->Refresh();
            }
            e.Skip();
        };
        auto pillLeave = [this](wxMouseEvent& e) {
            const ThemeData& th = m_appState->GetTheme();
            if (_modelPillLeftBracket) {
                _modelPillLeftBracket->SetForegroundColour(th.textMuted);
                _modelPillLeftBracket->Refresh();
            }
            if (_modelPillRightBracket) {
                _modelPillRightBracket->SetForegroundColour(th.textMuted);
                _modelPillRightBracket->Refresh();
            }
            e.Skip();
        };
        auto bindPillHover = [&](wxWindow* w) {
            if (!w) return;
            w->Bind(wxEVT_ENTER_WINDOW, pillEnter);
            w->Bind(wxEVT_LEAVE_WINDOW, pillLeave);
        };
        bindPillHover(_modelPill);
        bindPillHover(_modelLabel);
        bindPillHover(_statusDot);
        bindPillHover(_modelPillLeftBracket);
        bindPillHover(_modelPillRightBracket);

        // Server lifecycle events
        Bind(wxEVT_SERVER_READY, &MyFrame::OnServerReady, this);
        Bind(wxEVT_SERVER_ERROR, &MyFrame::OnServerError, this);

        // Phase 3b: tool protocol detection result
        Bind(wxEVT_TOOL_PROTOCOL_DETECTED,
             &MyFrame::OnToolProtocolDetected, this);

        // Drag-and-drop + clipboard paste
        // Install the file drop target on the frame and directly on the
        // input area.  On Windows, child controls can swallow file drops
        // before the frame sees them, so the text box needs its own target.
        SetDropTarget(new ImageDropTarget(this));
        _inputContainer->SetDropTarget(new ImageDropTarget(this));
        _userInputCtrl->SetDropTarget(new ImageDropTarget(this));

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
        LbMarkUiEventTargetDead(m_alive);
        m_isClosing = true;

        StopAnimation();

        if (m_chatClient->IsStreaming())
            m_chatClient->StopGeneration();

        if (!m_chatHistory->IsEmpty())
            m_convController->AutoSaveConversation();

        m_appState->SaveWindowState(this);

        // Do not rely only on MyFrame destruction to release VRAM.
        // llama-server owns the loaded model/CUDA context, so make shutdown
        // explicit while the frame and logger are still alive.
        if (m_serverManager)
            m_serverManager->StopServer();

        evt.Skip();
    }

    // ── Public interface for attachments (used by drop target) ─────
    bool AttachImageFromFile(const std::string& filePath)
    {
        if (IsBusy()) return false;
        bool ok = m_attachments->AttachImageFromFile(filePath);
        if (ok) RestoreComposerFocusDeferred();
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
        if (ok) RestoreComposerFocusDeferred();
        return ok;
    }

    // ─── Drag-and-drop document import routing ───────────────────
    //
    // The shared cwd-copy / safe-relative-path / status-message flow now
    // lives in DropImportController.  These public wrappers remain here
    // because ImageDropTarget and the file picker already route through
    // MyFrame.
    bool QueuePdfAttachmentFromDrop(const std::string& filePath)
    {
        return m_dropImportController &&
               m_dropImportController->QueuePdfAttachmentFromDrop(filePath);
    }

    bool QueueSpreadsheetAttachmentFromDrop(const std::string& filePath)
    {
        return m_dropImportController &&
               m_dropImportController->QueueSpreadsheetAttachmentFromDrop(filePath);
    }

    bool QueueDocxAttachmentFromDrop(const std::string& filePath)
    {
        return m_dropImportController &&
               m_dropImportController->QueueDocxAttachmentFromDrop(filePath);
    }

    void NotifyDocmDropRejected(const std::string& filePath)
    {
        if (m_dropImportController)
            m_dropImportController->NotifyDocmDropRejected(filePath);
    }

private:
    // Drag-and-drop on Windows can finish focus negotiation *after* the
    // attachment callback returns.  Calling SetFocus() immediately from the
    // drop/paste path can leave the composer drawing a caret while keyboard
    // focus has already moved elsewhere, which causes audible dings when the
    // user starts typing.  Defer the focus restore until the current event
    // unwinds so the visible caret and real keyboard focus stay in sync.
    void RestoreComposerFocusDeferred()
    {
        CallAfter([this]() {
            if (!m_isClosing && _userInputCtrl && _userInputCtrl->IsEnabled()) {
                _userInputCtrl->SetFocus();
            }
        });
    }

    // Telegram-style modal scrim: raised dialogs stay visually focused
    // while the parent client area dims beneath them.
    //
    // The first version used a translucent wxFrame. On wxMSW that can be
    // capability-dependent and, in practice here, it did not reliably appear
    // above the owning frame before the modal dialog opened. Use a small native
    // layered popup instead: it is owned by the LlamaBoss frame, paints solid
    // black, applies per-window alpha, and is shown before SettingsDialog enters
    // its modal loop. The Settings dialog is created/shown afterward, so it
    // remains above the scrim.
#ifdef __WXMSW__
    static LRESULT CALLBACK ModalScrimWndProc(HWND hwnd,
                                                       UINT msg,
                                                       WPARAM wParam,
                                                       LPARAM lParam)
    {
        switch (msg) {
        case WM_MOUSEACTIVATE:
            // The scrim is decorative modal chrome, not an interactive window.
            // Do not let an outside click activate it or leak into focus changes.
            return MA_NOACTIVATEANDEAT;

        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
        case WM_XBUTTONDBLCLK:
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
        case WM_CONTEXTMENU:
            // Outside clicks should simply be swallowed while Settings is modal.
            // This preserves the dimming effect without disturbing wxDialog::ShowModal().
            return 0;

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }

    HWND ShowModalScrim()
    {
        const wxSize clientSize = GetClientSize();
        if (clientSize.GetWidth() <= 0 || clientSize.GetHeight() <= 0)
            return nullptr;

        HWND owner = reinterpret_cast<HWND>(GetHandle());
        if (!owner) return nullptr;

        static const wchar_t* kScrimClassName = L"LlamaBossModalScrim";
        static ATOM s_scrimClassAtom = 0;
        static HBRUSH s_scrimBrush = nullptr;

        if (s_scrimClassAtom == 0) {
            if (!s_scrimBrush)
                s_scrimBrush = CreateSolidBrush(RGB(0, 0, 0));

            WNDCLASSW wc{};
            wc.lpfnWndProc   = ModalScrimWndProc;
            wc.hInstance     = GetModuleHandleW(nullptr);
            wc.lpszClassName = kScrimClassName;
            wc.hbrBackground = s_scrimBrush;

            s_scrimClassAtom = RegisterClassW(&wc);
            if (s_scrimClassAtom == 0 &&
                GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
                return nullptr;
            }
        }

        const wxPoint screenPos = ClientToScreen(wxPoint(0, 0));

        HWND scrim = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kScrimClassName,
            L"",
            WS_POPUP,
            screenPos.x,
            screenPos.y,
            clientSize.GetWidth(),
            clientSize.GetHeight(),
            owner,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);

        if (!scrim) return nullptr;

        // 128/255 ≈ 50% black. On the Telegram-like #0E1621 app surface,
        // this visually lands close to the sampled #070B11 backdrop.
        SetLayeredWindowAttributes(scrim, 0, 128, LWA_ALPHA);

        ShowWindow(scrim, SW_SHOWNOACTIVATE);
        SetWindowPos(
            scrim,
            HWND_TOP,
            screenPos.x,
            screenPos.y,
            clientSize.GetWidth(),
            clientSize.GetHeight(),
            SWP_NOACTIVATE | SWP_SHOWWINDOW);

        return scrim;
    }

    void HideModalScrim(HWND& scrim)
    {
        if (!scrim) return;
        DestroyWindow(scrim);
        scrim = nullptr;
    }
#else
    wxFrame* ShowModalScrim()
    {
        return nullptr;
    }

    void HideModalScrim(wxFrame*& scrim)
    {
        scrim = nullptr;
    }
#endif

    // ─── UI Controls ──────────────────────────────────────────────
    ChatDisplayCtrl* _chatDisplayCtrl;
    ChatInputCtrl*   _userInputCtrl;
    wxButton*        _sendButton;
    wxButton*        _stopButton;
    wxButton*        _attachButton;
    wxButton*        _agentToggleButton;
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

    // Goals Phase 16: lightweight first-class Goal status strip shown
    // directly below the existing Project strip.
    // Goal strip widgets were removed -- merged into ProjectStatusStrip
    // as the right-hand pair on the same row.  See TODO(rename) in
    // project_status_strip.h.

    wxPanel*       _rightPanel;
    wxPanel*       _inputContainer;
    wxPanel*       _inputSeparator;

    std::unique_ptr<ConversationSidebar> m_sidebar;
    bool m_isClosing;

    wxStaticText* _modelLabel;
    wxStaticText* _modelPillLeftBracket = nullptr;   // "[" — hover-recolored
    wxStaticText* _modelPillRightBracket = nullptr;  // "]" — hover-recolored
    StatusDot*    _statusDot;
    wxStaticText* _protocolChip;   // Phase 3b: native/xml chip beside model name
    ToolProtocol  _activeProtocol = ToolProtocol::Unknown;   // Phase 3c-i

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
    std::unique_ptr<CmdExecutor>   m_cmdExecutor;
    std::unique_ptr<PythonRunner>  m_pythonRunner;
    std::unique_ptr<GrepExecutor>  m_grepExecutor;

    // ─── Coordinators ────────────────────────────────────────────
    std::unique_ptr<ModelSwitcher>          m_modelSwitcher;
    std::unique_ptr<ConversationController> m_convController;
    std::unique_ptr<AgentController>        m_agentController;
    std::unique_ptr<DropImportController>   m_dropImportController;

    // Project status strip — replaces the native menu bar; renders
    // current project state in a single line under the top toolbar.
    std::unique_ptr<ProjectStatusStrip>     m_projectStrip;

    // Agent mode — when true, the next user message begins an
    // agent loop via m_agentController->Begin().  Toggled by the
    // agent button on the input area.
    bool m_agentModeEnabled;

    // Goals Phase 2: a small verifier + continuation loop layered outside
    // AgentController's inner tool loop.  The controller still owns a single
    // assistant/tool turn; MyFrame decides whether a completed goal-oriented
    // turn needs a fresh verifier check and an automatic follow-up turn.
    static constexpr int kGoalMaxAutoContinuations = 3;
    bool m_goalLoopSawToolOutput = false;
    bool m_goalAutoContinuationTurn = false;
    bool m_goalAutoStartAfterContractBuild = false;
    bool m_goalContractBuilderInFlight = false;
    bool m_goalVerifierInFlight = false;
    bool m_goalVerifierManualOnly = false;

    // Skill authoring Phase 2I: after the user creates a Skill from the
    // popup menu, ordinary chat remains conversational so the user and model
    // can design the Skill together first. Only an explicit draft request
    // hands the accumulated design conversation to the hidden Skill builder,
    // which writes the first SKILL.md draft and any complete same-folder helper.
    struct PendingSkillAuthoring {
        bool        active = false;
        // Whether the user picked "New Skill with Python Script" from
        // the menu at creation time. Determines whether the draft
        // builder is allowed to consider a same-folder .py helper:
        // when false, pythonHelperPath stays empty through draft
        // handoff and the builder system prompt takes the explicit
        // "no Python helper" branch.
        bool        requestedPythonScript = false;
        std::string skillName;
        std::string skillPath;
        std::string pythonHelperPath;
        std::string userDescription;
        size_t      conversationStartMessageIndex = 0;
    };
    PendingSkillAuthoring m_pendingSkillAuthoring;
    bool m_skillDraftBuilderInFlight = false;


    // Goals Phase 11 stores compact structured AgentEvent evidence inside
    // GoalState so it survives conversation save/load.  MyFrame only builds
    // and appends evidence; ChatHistory owns the durable data.

    // Cached prompt context for Skills + the active project.  This block is
    // otherwise rebuilt for every normal send and every agent-loop iteration,
    // which repeatedly scans Sources/Workflows and rereads PROJECT.md.
    struct ProjectContextCacheSignature {
        bool        hasProject = false;
        std::string projectRoot;
        std::string projectName;
        long long   skillsDirMTime = 0;
        unsigned long long skillsListHash = 0;
        unsigned long long skillScriptsListHash = 0;
        long long   projectJsonMTime = 0;
        long long   projectMdMTime = 0;
        long long   sourcesDirMTime = 0;
        unsigned long long sourcesListHash = 0;
        long long   workflowsDirMTime = 0;
        unsigned long long workflowsListHash = 0;
        unsigned long long workflowScriptsListHash = 0;

        bool operator==(const ProjectContextCacheSignature& other) const
        {
            return hasProject == other.hasProject &&
                   projectRoot == other.projectRoot &&
                   projectName == other.projectName &&
                   skillsDirMTime == other.skillsDirMTime &&
                   skillsListHash == other.skillsListHash &&
                   skillScriptsListHash == other.skillScriptsListHash &&
                   projectJsonMTime == other.projectJsonMTime &&
                   projectMdMTime == other.projectMdMTime &&
                   sourcesDirMTime == other.sourcesDirMTime &&
                   sourcesListHash == other.sourcesListHash &&
                   workflowsDirMTime == other.workflowsDirMTime &&
                   workflowsListHash == other.workflowsListHash &&
                   workflowScriptsListHash == other.workflowScriptsListHash;
        }
    };

    struct ProjectContextCache {
        bool valid = false;
        ProjectContextCacheSignature sig;
        std::string block;
    };

    mutable ProjectContextCache m_projectContextCache;

    // Phase 6: slash-command approval state.  Agent approvals live
    // inside AgentController because native tool_call_id threading
    // must remain with the loop.  Slash approvals live here because
    // MyFrame owns slash rendering, persistence, and async state.
    struct PendingSlashApproval {
        ToolInvocation invocation;
        ToolContext    context;
        bool           active = false;
    };
    PendingSlashApproval m_pendingSlashApproval;

    ToolCallStreamDetector m_agentToolStreamDetector;
    size_t m_agentToolVisibleProseLen = 0;

    void ResetAgentToolStreamFilter()
    {
        m_agentToolStreamDetector.Reset();
        m_agentToolVisibleProseLen = 0;
    }

    void DisplayNewAgentVisibleProse()
    {
        const std::string& prose = m_agentToolStreamDetector.GetProsePrefix();
        if (prose.size() > m_agentToolVisibleProseLen) {
            m_chatDisplay->DisplayAssistantDelta(prose.substr(m_agentToolVisibleProseLen));
            m_agentToolVisibleProseLen = prose.size();
        }
    }

    void FlushAgentHeldProseIfSafe()
    {
        if (!m_agentToolStreamDetector.Complete()) {
            const std::string& held = m_agentToolStreamDetector.GetHeldBuffer();
            if (!held.empty() && !ContainsToolCallOpenMarker(held)) {
                m_chatDisplay->DisplayAssistantDelta(held);
            }
        }
        ResetAgentToolStreamFilter();
    }

    // ═════════════════════════════════════════════════════════════
    //  AgentEventSink implementation (Phase 5)
    // ═════════════════════════════════════════════════════════════
    //
    // The agent loop reports progress through these four hooks
    // instead of reaching into ChatDisplay or invoking UI lambdas.
    // Bodies are the same work the Phase-4 callbacks did — just
    // moved here so the controller stays UI-free.

    // Goals Phase 10: capture the typed AgentEvent stream for the active
    // goal before the default sink bridge fans it back out to the existing
    // UI callbacks.  This gives the verifier structured proof of tool runs,
    // artifacts, edits, deletes, and tool errors without changing renderer
    // behavior or AgentController.
    void OnAgentEvent(const AgentEvent& event) override
    {
        RecordGoalStructuredAgentEvidence(event);
        AgentEventSink::OnAgentEvent(event);
    }

    // No loop-scoped UI state today.  The user's message is on
    // screen and the first chat request is in flight by the time
    // Begin() runs, so there's nothing to set up here.  Hook is
    // kept for future loop-scoped indicators (a "thinking…" status,
    // a Stop-button enable, etc.).
    void OnAgentLoopBegin() override
    {
        // Fresh per-turn observation. A goal continuation should only spin
        // the outer verifier loop when real work occurred, unless this very
        // loop was itself launched automatically by the goal verifier.
        m_goalLoopSawToolOutput = false;
    }

    // Between iterations: the previous streaming worker has exited
    // (that's what fired wxEVT_ASSISTANT_COMPLETE), but
    // ChatClient::m_isStreaming stays true until someone clears it
    // — the normal-completion path inside OnAssistantComplete that
    // we skipped.  Clear it here so the next SendMessage() doesn't
    // bounce off the is-streaming guard, render the assistant
    // prefix, and re-arm the streaming flag.
    void OnAgentIterationBegin() override
    {
        m_chatClient->ResetStreamingState();
        ResetAgentToolStreamFilter();
        m_chatDisplay->DisplayAssistantPrefix(
            ServerManager::ModelDisplayName(m_appState->GetModel()),
            m_appState->GetTheme().chatAssistant);
        m_chatState = ChatState::Streaming;
        SetStreamingState(true);
    }

    // The controller emits one of these for every tool result —
    // sync dispatches, async grep/cmd completions, malformed-call
    // errors.  Phase-5 plumbing forwards straight to ChatDisplay;
    // future P6 approval cards will intercept this seam to gate
    // dangerous results before they hit the chat.
    void OnAgentToolBlock(const ToolBlock& block,
                          bool startExpanded) override
    {
        if (m_chatHistory->HasActiveGoal())
            m_goalLoopSawToolOutput = true;

        m_chatDisplay->DisplayToolBlock(block, startExpanded);
    }

    // Phase 6: agent approval pauses the loop before the risky tool
    // runs.  The card is UI-only; the Allow Once / Allow Always / Deny
    // buttons (or the typed-command fallback) resolve the pending
    // invocation held by AgentController.
    void OnAgentApprovalRequired(const ToolBlock& block) override
    {
        // UX polish: approval cards should not show the full script/source
        // by default. Casual users get a calm, simple prompt; developers can
        // still click [show details] to review the exact tool/source before
        // approving.  Setting requiresApproval=true on a local copy tells
        // ChatDisplay to render the button row beneath [show details]; the
        // typed-command fallback in TryHandlePendingApprovalInput still
        // works for keyboard users without being visually advertised.
        ToolBlock card = block;
        card.requiresApproval = true;
        m_chatDisplay->DisplayToolBlock(card, false);
        SetApprovalState(true);
    }

    // Explicit non-streamed final answer for deterministic helper paths
    // that intentionally skip another model pass.  Render it exactly like
    // a normal assistant message so successful file-creation turns do not
    // end with a gray/italic system status line.
    void OnAgentTurnComplete(const std::string& message) override
    {
        if (message.empty()) return;

        const std::string model = m_appState->GetModel();
        m_chatDisplay->DisplayAssistantMessage(
            ServerManager::ModelDisplayName(model),
            message,
            m_appState->GetTheme().chatAssistant);

        if (m_chatHistory->HasAssistantPlaceholder())
            m_chatHistory->UpdateLastAssistantMessage(message);
        else
            m_chatHistory->AddAssistantMessage(message, model);
    }

    std::string GoalTurnInterruptionMessage(AgentEndReason reason) const
    {
        switch (reason) {
        case AgentEndReason::Cancelled:
            return "Goal turn stopped by user before verification. Goal remains active.";
        case AgentEndReason::IterationCap:
            return "Goal turn stopped at the agent tool-step safety cap before verification. Goal remains active.";
        case AgentEndReason::MalformedCap:
            return "Goal turn stopped because malformed tool calls hit the safety cap before verification. Goal remains active.";
        case AgentEndReason::StreamError:
            return "Goal turn stopped because the assistant stream failed before verification. Goal remains active.";
        case AgentEndReason::SendFailed:
            return "Goal turn stopped because the next agent request could not be sent before verification. Goal remains active.";
        case AgentEndReason::LoopGuard:
            return "Goal turn stopped because the loop guard blocked a repeated tool call before verification. Goal remains active.";
        case AgentEndReason::Normal:
            break;
        }
        return std::string();
    }

    // Loop ended for any reason.  If the controller supplied a
    // user-facing message (cancel/iter-cap/malformed-cap/send-fail
    // cases), surface it as a system message before we finalize.
    // Normal and StreamError both arrive with empty messages —
    // Normal because the model's final answer is the message, and
    // StreamError because OnAssistantError already showed friendly
    // error text before unwinding the loop.
    void OnAgentLoopEnd(AgentEndReason     reason,
                        const std::string& userFacingMessage) override
    {
        const bool shouldVerifyGoal =
            reason == AgentEndReason::Normal &&
            m_chatHistory->HasActiveGoal() &&
            (m_goalLoopSawToolOutput || m_goalAutoContinuationTurn);

        if (!userFacingMessage.empty()) {
            m_chatDisplay->DisplaySystemMessage(userFacingMessage);
        }

        if (reason != AgentEndReason::Normal &&
            m_chatHistory->HasActiveGoal()) {
            const std::string goalInterruption =
                GoalTurnInterruptionMessage(reason);
            if (!goalInterruption.empty()) {
                m_chatHistory->NoteGoalTurnInterrupted(goalInterruption);
                RefreshGoalStatusStrip();
                m_chatDisplay->DisplaySystemMessage(goalInterruption);
                m_convController->AutoSaveConversation();
            }
        }

        m_chatClient->ResetStreamingState();
        ResetAgentToolStreamFilter();
        SetStreamingState(false);
        m_chatDisplay->ClearFilePersistenceContext();
        if (!m_chatHistory->IsEmpty())
            m_convController->AutoSaveConversation();

        m_goalLoopSawToolOutput = false;
        m_goalAutoContinuationTurn = false;

        if (shouldVerifyGoal) {
            CallAfter([this]() {
                BeginGoalVerificationIfNeeded();
            });
        }
    }

    // ─── Chat state machine ──────────────────────────────────────
    ChatState m_chatState;

    // ── ASCII Animation ──────────────────────────────────────────
    wxTimer                          m_animTimer{this, ID_ANIMATION_TIMER};
    std::unique_ptr<AsciiAnimation>  m_activeAnimation;

    // Assistant streaming delta batcher.  The worker still posts deltas as
    // quickly as llama-server emits them, but the UI/history path receives
    // combined chunks at most about once per frame.
    wxTimer       m_assistantDeltaFlushTimer{this, ID_ASSISTANT_DELTA_FLUSH_TIMER};
    std::string   m_pendingAssistantDelta;
    unsigned long m_pendingAssistantDeltaGenerationId = 0;

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
        if (_modelPillLeftBracket)  _modelPillLeftBracket->SetForegroundColour(t.textMuted);
        if (_modelPillRightBracket) _modelPillRightBracket->SetForegroundColour(t.textMuted);
        _newChatButton->SetBackgroundColour(t.bgToolbar);
        _newChatButton->SetForegroundColour(t.textMuted);
        _settingsButton->SetBackgroundColour(t.bgToolbar);
        _settingsButton->SetForegroundColour(t.textMuted);
        _aboutButton->SetBackgroundColour(t.bgToolbar);
        _aboutButton->SetForegroundColour(t.textMuted);
        _topSeparator->SetBackgroundColour(t.borderSubtle);
        _statusDot->SetColors(t.accentButton, t.textMuted);

        // Merged Project + Goal strip.  ApplyTheme() repaints both
        // halves at once and also fixes the prior latent issue where
        // the project strip itself was never re-themed (only the old
        // separate goal strip was).
        if (m_projectStrip) m_projectStrip->ApplyTheme(t);
        RefreshGoalStatusStrip();   // alias for RefreshProjectStrip()

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
        _agentToggleButton->SetBackgroundColour(t.bgInputArea);
        _agentToggleButton->SetForegroundColour(
            m_agentModeEnabled ? LbInteractiveAccentForTheme(t) : t.textMuted);
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
        if (m_chatDisplay) {
            m_chatDisplay->SetToolBlockInteractionEnabled(!streaming);
        }

        _sendButton->Show(!streaming);
        _stopButton->Show(streaming);
        _userInputCtrl->Enable(!streaming);
        _attachButton->Enable(!streaming);
        _agentToggleButton->Enable(!streaming);
        _settingsButton->Enable(!streaming);
        _newChatButton->Enable(!streaming);
        _inputSizer->Layout();

        if (!streaming) {
            m_chatState = ChatState::Idle;
            _userInputCtrl->SetFocus();
        }
    }

    void SetApprovalState(bool waiting)
    {
        // Approval review is paused, not actively appending; keep [details]
        // usable so the user can inspect the proposed mutation before approving.
        if (m_chatDisplay) {
            m_chatDisplay->SetToolBlockInteractionEnabled(true);
        }

        if (waiting) {
            // Approval is intentionally a special busy state: the
            // chat turn is paused, but the input must stay enabled
            // so the user can type /approve or /deny.  Keep Stop
            // visible so pressing it cancels the pending approval.
            _sendButton->Show(false);
            _stopButton->Show(true);
            _userInputCtrl->Enable(true);
            _attachButton->Enable(false);
            _agentToggleButton->Enable(false);
            _settingsButton->Enable(false);
            _newChatButton->Enable(false);
            m_chatState = ChatState::AwaitingApproval;
        } else {
            _sendButton->Show(true);
            _stopButton->Show(false);
            _userInputCtrl->Enable(true);
            _attachButton->Enable(true);
            _agentToggleButton->Enable(true);
            _settingsButton->Enable(true);
            _newChatButton->Enable(true);
            m_chatState = ChatState::Idle;
        }
        _inputSizer->Layout();
        _userInputCtrl->SetFocus();
    }

    bool IsBusy() const { return m_chatState != ChatState::Idle; }

    static bool IsPythonAsyncToolName(const std::string& toolName)
    {
        return toolName == tool_names::kPythonHealth ||
               toolName == tool_names::kCsvInspect ||
               toolName == tool_names::kCsvReport ||
               toolName == tool_names::kCsvToXlsx ||
               toolName == tool_names::kXlsxInspect ||
               toolName == tool_names::kXlsxReport ||
               toolName == tool_names::kXlsxCreateWorkbook ||
               toolName == tool_names::kPdfExtractText ||
               toolName == tool_names::kPdfInspectForm ||
               toolName == tool_names::kPdfFillForm ||
               toolName == tool_names::kDocxExtractText ||
               toolName == tool_names::kDocxInspect ||
               toolName == tool_names::kPythonRunScript ||
               toolName == tool_names::kPythonInstallPackage;
    }

    // ═════════════════════════════════════════════════════════════
    //  EVENT HANDLERS
    // ═════════════════════════════════════════════════════════════

    void OnToggleAgentMode(wxCommandEvent&)
    {
        if (IsBusy()) return;  // ignore toggle while something's running
        m_agentModeEnabled = !m_agentModeEnabled;
        _agentToggleButton->SetForegroundColour(
            m_agentModeEnabled ? LbInteractiveAccentForTheme(m_appState->GetTheme())
                               : m_appState->GetTheme().textMuted);
        _agentToggleButton->Refresh();
        m_chatDisplay->DisplaySystemMessage(
            m_agentModeEnabled
              ? "\xF0\x9F\xA4\x96 Agent mode ON. The model can use read/ls/open/grep/pwd/powershell."
              : "\xF0\x9F\xA4\x96 Agent mode OFF.");
    }

    void OnAttachImage(wxCommandEvent&)
    {
        // Don't show the picker while a tool or stream is in flight.  The
        // Queue*/Attach* helpers refuse anyway, but opening the dialog just
        // to throw the selection away is bad UX.
        if (IsBusy()) return;

        if (m_attachments->GetCount() >= AttachmentManager::kMaxAttachments) {
            wxMessageBox(wxString::Format(
                "Maximum of %zu attachments reached.\nRemove some before adding more.",
                AttachmentManager::kMaxAttachments),
                "Attachment Limit", wxOK | wxICON_INFORMATION);
            return;
        }

        // Filter shape mirrors the drag-and-drop dispatch order.  The
        // first ("All supported") filter is what wxFileDialog selects by
        // default, so users don't have to hunt for a per-kind filter.
        // CSV intentionally lives under Text & code -- IsSpreadsheetFile
        // only matches .xlsx, and the drop target routes .csv through
        // AttachTextFile, so the click path matches.
        const wxString filter =
            "All supported files"
            "|*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.webp;"
             "*.pdf;*.xlsx;*.docx;"
             "*.txt;*.md;*.json;*.cpp;*.h;*.hpp;*.py;*.js;*.ts;*.jsx;*.tsx;"
             "*.css;*.html;*.xml;*.yaml;*.yml;*.toml;*.csv;*.log;*.ini;*.cfg;"
             "*.sh;*.bat;*.rs;*.go;*.java;*.kt;*.swift;*.rb;*.php;*.sql;"
             "*.dockerfile;.env;.gitignore"
            "|Image files (*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.webp)"
            "|*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.webp"
            "|PDF documents (*.pdf)|*.pdf"
            "|Word documents (*.docx)|*.docx"
            "|Spreadsheets (*.xlsx)|*.xlsx"
            "|Text & code files"
            "|*.txt;*.md;*.json;*.cpp;*.h;*.hpp;*.py;*.js;*.ts;*.jsx;*.tsx;"
             "*.css;*.html;*.xml;*.yaml;*.yml;*.toml;*.csv;*.log;*.ini;*.cfg;"
             "*.sh;*.bat;*.rs;*.go;*.java;*.kt;*.swift;*.rb;*.php;*.sql;"
             "*.dockerfile;.env;.gitignore"
            "|All files (*.*)|*.*";

        wxFileDialog dlg(this, "Attach files", "", "",
            filter,
            wxFD_OPEN | wxFD_FILE_MUST_EXIST | wxFD_MULTIPLE);

        if (dlg.ShowModal() == wxID_CANCEL) return;

        wxArrayString paths;
        dlg.GetPaths(paths);

        int attached = 0, unsupported = 0, failures = 0;
        bool hitCap = false;

        // Per-file dispatch matches ImageDropTarget::OnDropFiles exactly:
        // pdf -> IsSpreadsheetFile (xlsx) -> docx -> IsImageFile -> IsTextFile
        // -> unsupported.  PDF/XLSX/DOCX route through the same Queue*
        // helpers the drop path uses, so the click and drag paths share
        // the cwd-copy logic, the 100 MB cap, and the system-message
        // feedback.  .docm is intentionally not auto-routed here even
        // though IsDocxFile() accepts it -- matches the drop-target gate.
        for (const auto& path : paths) {
            if (m_attachments->GetCount() >= AttachmentManager::kMaxAttachments) {
                hitCap = true;
                break;
            }

            std::string pathUtf8 = WxToUtf8(path);
            wxFileName fn(path);
            std::string ext(fn.GetExt().Lower().ToUTF8().data());

            bool ok = false;
            bool routed = true;

            if (ext == "pdf") {
                ok = QueuePdfAttachmentFromDrop(pathUtf8);
            }
            else if (AttachmentManager::IsSpreadsheetFile(pathUtf8)) {
                ok = QueueSpreadsheetAttachmentFromDrop(pathUtf8);
            }
            else if (ext == "docx") {
                ok = QueueDocxAttachmentFromDrop(pathUtf8);
            }
            else if (AttachmentManager::IsImageFile(pathUtf8)) {
                ok = AttachImageFromFile(pathUtf8);
            }
            else if (AttachmentManager::IsTextFile(pathUtf8)) {
                ok = AttachTextFile(pathUtf8);
            }
            else {
                routed = false;
            }

            if (!routed)      ++unsupported;
            else if (ok)      ++attached;
            else              ++failures;
        }

        if (hitCap) {
            wxMessageBox(wxString::Format(
                "Attached %d file(s). Remaining skipped (max %zu attachments).",
                attached, AttachmentManager::kMaxAttachments),
                "Attachment Limit", wxOK | wxICON_INFORMATION);
        }
        else if (unsupported > 0 && attached == 0 && failures == 0) {
            wxMessageBox("Unsupported file type.\n\n"
                "Supported: images (png, jpg, gif, bmp, webp), PDF, XLSX, "
                "DOCX, and text/code files (txt, md, json, cpp, h, py, js, "
                "csv, etc.).",
                "Unsupported File", wxOK | wxICON_INFORMATION);
        }
        else if (failures > 0 || unsupported > 0) {
            wxMessageBox(wxString::Format(
                "%d of %zu file(s) could not be attached.",
                failures + unsupported, paths.size()),
                "Attachment Warning", wxOK | wxICON_WARNING);
        }
    }

    void DiscardPendingAssistantDelta()
    {
        if (m_assistantDeltaFlushTimer.IsRunning())
            m_assistantDeltaFlushTimer.Stop();
        m_pendingAssistantDelta.clear();
        m_pendingAssistantDeltaGenerationId = 0;
    }

    void FlushPendingAssistantDelta()
    {
        if (m_assistantDeltaFlushTimer.IsRunning())
            m_assistantDeltaFlushTimer.Stop();

        if (m_pendingAssistantDelta.empty()) return;

        const unsigned long pendingGen = m_pendingAssistantDeltaGenerationId;
        std::string delta;
        delta.swap(m_pendingAssistantDelta);
        m_pendingAssistantDeltaGenerationId = 0;

        if (m_isClosing || pendingGen != m_generationId) return;

        // Goal contract-builder and verifier requests are deliberately
        // invisible: they have no assistant placeholder in the user-visible
        // transcript, so streamed hidden-control deltas must not append onto
        // the previous assistant reply.
        if (m_goalContractBuilderInFlight || m_goalVerifierInFlight ||
            m_skillDraftBuilderInFlight) return;

        m_chatHistory->AppendToLastAssistantMessage(delta);

        if (m_agentController->IsActive()) {
            m_agentToolStreamDetector.Feed(delta);
            DisplayNewAgentVisibleProse();
            return;
        }

        m_chatDisplay->DisplayAssistantDelta(delta);
    }

    void OnAssistantDeltaFlushTimer(wxTimerEvent&)
    {
        FlushPendingAssistantDelta();
    }

    void OnAssistantDelta(wxCommandEvent& event)
    {
        if (m_isClosing) return;
        if (static_cast<unsigned long>(event.GetExtraLong()) != m_generationId) return;

        std::string delta = WxToUtf8(event.GetString());
        if (delta.empty()) return;

        if (m_pendingAssistantDelta.empty()) {
            m_pendingAssistantDeltaGenerationId = m_generationId;
        } else if (m_pendingAssistantDeltaGenerationId != m_generationId) {
            m_pendingAssistantDelta.clear();
            m_pendingAssistantDeltaGenerationId = m_generationId;
        }

        m_pendingAssistantDelta += delta;

        if (!m_assistantDeltaFlushTimer.IsRunning())
            m_assistantDeltaFlushTimer.Start(16, wxTIMER_ONE_SHOT);
    }

    void OnAssistantComplete(wxCommandEvent& event)
    {
        if (m_isClosing) return;
        if (static_cast<unsigned long>(event.GetExtraLong()) != m_generationId) return;

        // Make sure any final batched text is in history/UI before we
        // decide whether this was visible prose, XML tool-only, or native
        // tool-only completion.
        FlushPendingAssistantDelta();

        std::string fullResponse = WxToUtf8(event.GetString());

        // Goals Phase 3 contract-builder requests and Phase 2 verifier
        // requests are hidden control turns, not transcript replies. Their
        // streamed deltas were discarded above; consume their completed text
        // here before any normal chat UI finalization runs.
        if (m_skillDraftBuilderInFlight) {
            HandleSkillDraftBuilderComplete(fullResponse);
            return;
        }
        if (m_goalContractBuilderInFlight) {
            HandleGoalContractBuilderComplete(fullResponse);
            return;
        }
        if (m_goalVerifierInFlight) {
            HandleGoalVerifierComplete(fullResponse);
            return;
        }

        // Phase 3 bugfix #3: extract native tool_calls before deciding
        // whether this assistant turn has visible UI text. Native
        // function-calling turns often complete with content == "" and
        // tool_calls != []; if we call DisplayAssistantComplete() first,
        // the chat renders an empty "model:" row before the tool card.
        std::string toolCallsJson;
        if (auto* payload = dynamic_cast<AssistantCompletePayload*>(
                event.GetClientObject())) {
            toolCallsJson = payload->ToolCallsJson();
        }

        const auto hasVisibleText = [](const std::string& text) -> bool {
            return text.find_first_not_of(" \t\r\n") != std::string::npos;
        };

        const bool agentStreamActive = m_agentController->IsActive();
        const bool xmlToolOnlyCall =
            agentStreamActive &&
            m_agentToolStreamDetector.Complete() &&
            m_agentToolVisibleProseLen == 0;
        const bool nativeToolOnlyCall =
            agentStreamActive &&
            !toolCallsJson.empty() &&
            !hasVisibleText(fullResponse) &&
            m_agentToolVisibleProseLen == 0;
        const bool agentToolOnlyCall = xmlToolOnlyCall || nativeToolOnlyCall;

        if (agentStreamActive) {
            FlushAgentHeldProseIfSafe();
        }

        if (agentToolOnlyCall) {
            m_chatDisplay->CancelPendingAssistantDisplay();
        } else {
            m_chatDisplay->DisplayAssistantComplete();
        }

        if (hasVisibleText(fullResponse)) {
            m_chatHistory->UpdateLastAssistantMessage(fullResponse);
        }
        else if (auto* logger = m_appState->GetLogger();
                 logger && toolCallsJson.empty()) {
            logger->warning("Assistant complete event arrived empty; keeping streamed content");
        }

        // ── Agent mode routing ──────────────────────────────────
        // If a loop is active and the controller consumed this
        // event (tool call found, loop continuing), skip the
        // normal "finalize and stop streaming" path — the next
        // iteration is already in flight and SetStreamingState(true)
        // was re-applied by OnAgentIterationBegin (Phase 5).
        if (m_agentController->IsActive()) {
            // Phase 3c-ii: structured tool_calls were extracted above
            // before UI finalization so native tool-only turns can be
            // hidden cleanly instead of rendering blank assistant rows.
            if (auto* logger = m_appState->GetLogger();
                logger && !toolCallsJson.empty()) {
                logger->information(
                    "Structured tool_calls extracted: "
                    + std::to_string(toolCallsJson.size())
                    + " bytes JSON");
            }

            bool consumed = m_agentController->HandleAssistantComplete(
                fullResponse, toolCallsJson);
            if (consumed) return;
            // Not consumed = loop ended (no tool call in reply).
            // Fall through to normal finalization below.
        }

        m_chatClient->ResetStreamingState();
        SetStreamingState(false);
        m_chatDisplay->ClearFilePersistenceContext();
        m_convController->AutoSaveConversation();

        if (auto* logger = m_appState->GetLogger())
            logger->information("Chat response completed");
    }

    void OnAssistantError(wxCommandEvent& event)
    {
        if (m_isClosing) return;
        if (static_cast<unsigned long>(event.GetExtraLong()) != m_generationId) return;

        if (m_skillDraftBuilderInFlight) {
            HandleSkillDraftBuilderError(WxToUtf8(event.GetString()));
            return;
        }
        if (m_goalContractBuilderInFlight) {
            HandleGoalContractBuilderError(WxToUtf8(event.GetString()));
            return;
        }
        if (m_goalVerifierInFlight) {
            HandleGoalVerifierError(WxToUtf8(event.GetString()));
            return;
        }

        // The request failed; do not render any buffered tail chunks after
        // the error message removes the assistant placeholder.
        DiscardPendingAssistantDelta();

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

        // Tell the agent loop to unwind (if active).  Returns false
        // always; we still do normal finalization below regardless.
        if (m_agentController->IsActive()) {
            ResetAgentToolStreamFilter();
            m_agentController->HandleAssistantError(error);
        }

        m_chatClient->ResetStreamingState();
        SetStreamingState(false);
        m_chatDisplay->ClearFilePersistenceContext();

        if (!m_chatHistory->IsEmpty()) m_convController->AutoSaveConversation();

        if (auto* logger = m_appState->GetLogger())
            logger->error("Chat error (" + modelName + "): " + error);
    }

    // ── /cmd completion handlers (Phase 1) ───────────────────────
    void OnCmdComplete(wxCommandEvent& evt)
    {
        if (m_isClosing) return;

        auto* data = static_cast<CmdResultClientData*>(evt.GetClientObject());
        if (!data) {
            SetStreamingState(false);
            m_chatState = ChatState::Idle;
            return;
        }
        const CmdResult& r = data->GetResult();

        // ── Agent mode routing ──────────────────────────────────
        // When the loop is active and was awaiting THIS result,
        // the controller builds the ToolBlock itself and fires
        // the next iteration — skip the normal user-/cmd path.
        // The controller's HandleCmdComplete returns false if the
        // pending invocation isn't a powershell call (defensive)
        // so we fall through and treat the event as a user-/cmd
        // completion.
        if (m_agentController->IsActive()) {
            bool consumed = m_agentController->HandleCmdComplete(r);
            if (consumed) return;
        }

        // ── Slash arm (Phase 4 unified) ────────────────────────
        // Build the same ToolInvocationResult shape the agent's
        // HandleCmdComplete builds, then render + persist via the
        // shared helper.  Chip ordering matches the agent path and
        // the saved-history tool-card layout — pre-Phase
        // 4 the on-screen ordering was [elapsed, status]; it's now
        // [status, elapsed] consistently across display and history.
        ToolInvocationResult tir;
        tir.toolTag       = tool_names::kPowerShell;
        tir.invocationRaw.clear();
        tir.iconUtf8      = "\xE2\x9A\x99";       // ⚙
        tir.toolName      = "PowerShell";
        tir.commandEcho   = r.command;
        tir.body          = r.stdoutText;
        tir.errorBody     = r.stderrText;
        tir.bodyLang      = "powershell";
        tir.presentedFiles = r.presentedFiles;

        // Status chip (cancelled / timed out / exit N) — mutually
        // exclusive — followed by elapsed, then truncated if any.
        if (r.cancelled) {
            tir.chips.push_back("cancelled");
        } else if (r.timedOut) {
            tir.chips.push_back("timed out");
        } else {
            std::ostringstream ec;
            ec << "exit " << r.exitCode;
            tir.chips.push_back(ec.str());
        }
        {
            std::ostringstream ts;
            ts << std::fixed;
            ts.precision(r.elapsedSec < 10.0 ? 2 : 1);
            ts << r.elapsedSec << "s";
            tir.chips.push_back(ts.str());
        }
        if (r.truncated) tir.chips.push_back("truncated");

        RenderAndPersistSlashResult(tir);

        SetStreamingState(false);
        m_chatState = ChatState::Idle;

        if (!m_chatHistory->IsEmpty())
            m_convController->AutoSaveConversation();
    }

    void OnCmdError(wxCommandEvent& evt)
    {
        if (m_isClosing) return;
        m_chatDisplay->DisplaySystemMessage(
            "Command error: " + WxToUtf8(evt.GetString()));
        SetStreamingState(false);
        m_chatState = ChatState::Idle;
    }

    // ── controlled Python helper completion handler ───────────────
    void OnPythonComplete(wxCommandEvent& evt)
    {
        if (m_isClosing) return;

        auto* data = static_cast<PythonRunResultClientData*>(evt.GetClientObject());
        if (!data) {
            SetStreamingState(false);
            m_chatState = ChatState::Idle;
            return;
        }
        const PythonRunResult& r = data->GetResult();

        auto TryReadSmallTextFile = [](const std::string& path,
                                       size_t             maxBytes,
                                       std::string&       out,
                                       size_t&            sizeOut) -> bool {
            out.clear();
            sizeOut = 0;

            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file) return false;

            std::streampos end = file.tellg();
            if (end == std::streampos(-1)) return false;

            sizeOut = static_cast<size_t>(end);
            if (sizeOut > maxBytes) return false;

            file.seekg(0, std::ios::beg);
            std::ostringstream buffer;
            buffer << file.rdbuf();
            out = buffer.str();
            return true;
        };

        if (m_agentController->IsActive()) {
            bool consumed = m_agentController->HandlePythonComplete(r);
            if (consumed) return;
        }

        const bool isInspect = (r.toolName == tool_names::kCsvInspect ||
                                r.helperName == tool_names::kCsvInspect);
        const bool isReport  = (r.toolName == tool_names::kCsvReport ||
                                r.helperName == tool_names::kCsvReport);
        const bool isCsvToXlsx = (r.toolName == tool_names::kCsvToXlsx ||
                                  r.helperName == tool_names::kCsvToXlsx);
        const bool isXlsxIns = (r.toolName == tool_names::kXlsxInspect ||
                                r.helperName == tool_names::kXlsxInspect);
        const bool isXlsxRep = (r.toolName == tool_names::kXlsxReport ||
                                r.helperName == tool_names::kXlsxReport);
        const bool isXlsxCreate = (r.toolName == tool_names::kXlsxCreateWorkbook ||
                                   r.helperName == tool_names::kXlsxCreateWorkbook);
        const bool isPdf     = (r.toolName == tool_names::kPdfExtractText ||
                                r.helperName == tool_names::kPdfExtractText);
        const bool isPdfInspect = (r.toolName == tool_names::kPdfInspectForm ||
                                   r.helperName == tool_names::kPdfInspectForm);
        const bool isPdfFill = (r.toolName == tool_names::kPdfFillForm ||
                                r.helperName == tool_names::kPdfFillForm);
        const bool isDocxExtract = (r.toolName == tool_names::kDocxExtractText ||
                                    r.helperName == tool_names::kDocxExtractText);
        const bool isDocxInspect = (r.toolName == tool_names::kDocxInspect ||
                                    r.helperName == tool_names::kDocxInspect);
        const bool isRun     = (r.toolName == tool_names::kPythonRunScript ||
                                r.helperName == tool_names::kPythonRunScript);
        const bool isInstall = (r.toolName == tool_names::kPythonInstallPackage ||
                                r.helperName == tool_names::kPythonInstallPackage);
        ToolInvocationResult tir;
        tir.toolTag       = isInstall ? std::string(tool_names::kPythonInstallPackage)
                         : isRun ? std::string(tool_names::kPythonRunScript)
                         : isPdf ? std::string(tool_names::kPdfExtractText)
                         : isPdfInspect ? std::string(tool_names::kPdfInspectForm)
                         : isPdfFill ? std::string(tool_names::kPdfFillForm)
                         : isDocxExtract ? std::string(tool_names::kDocxExtractText)
                         : isDocxInspect ? std::string(tool_names::kDocxInspect)
                         : isXlsxCreate ? std::string(tool_names::kXlsxCreateWorkbook)
                         : isCsvToXlsx ? std::string(tool_names::kCsvToXlsx)
                         : isXlsxRep ? std::string(tool_names::kXlsxReport)
                         : isXlsxIns ? std::string(tool_names::kXlsxInspect)
                         : isReport ? std::string(tool_names::kCsvReport)
                         : isInspect ? std::string(tool_names::kCsvInspect)
                                     : std::string(tool_names::kPythonHealth);
        tir.invocationRaw.clear();
        tir.iconUtf8      = (isPdf || isPdfInspect || isPdfFill ||
                               isDocxExtract || isDocxInspect) ? std::string("\xF0\x9F\x93\x84")  // 📄
                         : (isXlsxRep || isXlsxCreate || isCsvToXlsx) ? std::string("\xF0\x9F\x93\x97")       // 📗
                         : isReport ? std::string("\xF0\x9F\x93\x9D")        // 📝
                         : (isInspect || isXlsxIns) ? std::string("\xF0\x9F\x93\x8A") // 📊
                                     : std::string("\xF0\x9F\x90\x8D");      // 🐍
        tir.toolName      = isInstall ? std::string("Install Python Package")
                         : isRun ? std::string("Python Run")
                         : isPdf ? std::string("PDF Extract Text")
                         : isPdfInspect ? std::string("PDF Inspect Form")
                         : isPdfFill ? std::string("PDF Fill Form")
                         : isDocxExtract ? std::string("DOCX Extract Text")
                         : isDocxInspect ? std::string("DOCX Inspect")
                         : isXlsxCreate ? std::string("Create Workbook")
                         : isCsvToXlsx ? std::string("CSV to XLSX")
                         : isXlsxRep ? std::string("XLSX Report")
                         : isXlsxIns ? std::string("XLSX Inspect")
                         : isReport ? std::string("CSV Report")
                         : isInspect ? std::string("CSV Inspect")
                                     : std::string("Python Health");
        tir.commandEcho   = r.commandEcho.empty()
                                ? (isInstall ? std::string("python_install_package")
                                  : isRun ? std::string("python_run_script")
                                  : isPdf ? std::string("pdf_extract_text")
                                  : isPdfInspect ? std::string("pdf_inspect_form")
                                  : isPdfFill ? std::string("pdf_fill_form")
                                  : isDocxExtract ? std::string("docx_extract_text")
                                  : isDocxInspect ? std::string("docx_inspect")
                                  : isXlsxCreate ? std::string("xlsx_create_workbook")
                                  : isCsvToXlsx ? std::string("csv_to_xlsx")
                                  : isXlsxRep ? std::string("xlsx_report")
                                  : isXlsxIns ? std::string("xlsx_inspect")
                                  : isReport ? std::string("csv_report")
                                  : isInspect ? std::string("csv_inspect")
                                              : std::string("python_health"))
                                : r.commandEcho;
        tir.body          = r.stdoutText;
        tir.errorBody     = r.stderrText;
        tir.bodyLang      = (isRun || isInstall) ? std::string() : std::string("json");
        tir.presentedFiles = r.presentedFiles;
        if (isRun && r.exitCode == 0 && !r.cancelled && !r.timedOut) {
            ApplyArtifactPresentation(tir);
        }
        ApplyMissingPythonPackageRecovery(tir, r);

        if (r.cancelled) {
            tir.chips.push_back("cancelled");
        } else if (r.timedOut) {
            tir.chips.push_back("timed out");
        } else {
            std::ostringstream ec;
            ec << "exit " << r.exitCode;
            tir.chips.push_back(ec.str());
        }
        {
            std::ostringstream ts;
            ts << std::fixed;
            ts.precision(r.elapsedSec < 10.0 ? 2 : 1);
            ts << r.elapsedSec << "s";
            tir.chips.push_back(ts.str());
        }
        if (!r.pythonCommand.empty()) tir.chips.push_back(r.pythonCommand);
        if (r.truncated) tir.chips.push_back("truncated");

        // PDF UX polish: card-first policy.  The artifact chip is always
        // the primary affordance for tool-produced files; for small
        // extractions we ALSO surface the Markdown body inline as a
        // courtesy preview, but the [Open] / [Save As] / [Open Folder]
        // card stays.  Large extractions get a short explanation in the
        // body and rely on the card alone.  This also generalizes
        // cleanly to binary-output tools (e.g. pdf_fill_form) where
        // there is no inline-able body at all.
        if (isPdf && r.exitCode == 0 && !r.presentedFiles.empty()) {
            constexpr size_t kMaxInlinePdfMarkdownBytes = 32 * 1024;

            std::string inlineMarkdown;
            size_t inlineBytes = 0;
            const std::string artifactPath = r.presentedFiles.front().diskPath;

            if (TryReadSmallTextFile(artifactPath,
                                     kMaxInlinePdfMarkdownBytes,
                                     inlineMarkdown,
                                     inlineBytes)) {
                tir.body = inlineMarkdown;
                tir.errorBody.clear();
                tir.bodyLang = "markdown";
                // presentedFiles intentionally retained -- card is the
                // primary deliverable.  No "inline" chip; card + inline
                // body is now the default success shape.
            } else if (inlineBytes > kMaxInlinePdfMarkdownBytes) {
                std::ostringstream msg;
                msg << "PDF text was extracted successfully, but the extracted "
                       "Markdown is too large to show inline.\n\n"
                    << "Extracted Markdown size: " << inlineBytes << " bytes\n"
                    << "Inline display limit: " << kMaxInlinePdfMarkdownBytes
                    << " bytes\n\n"
                    << "Use the file chip above to save or open the extracted Markdown.";
                tir.body = msg.str();
                tir.errorBody.clear();
                tir.bodyLang.clear();
            }
        }

        RenderAndPersistSlashResult(tir);

        SetStreamingState(false);
        m_chatState = ChatState::Idle;

        if (!m_chatHistory->IsEmpty())
            m_convController->AutoSaveConversation();
    }

    void OnPythonError(wxCommandEvent& evt)
    {
        if (m_isClosing) return;
        m_chatDisplay->DisplaySystemMessage(
            "Python runner error: " + WxToUtf8(evt.GetString()));
        SetStreamingState(false);
        m_chatState = ChatState::Idle;
    }

    // ── /grep completion handler (Phase 3) ───────────────────────
    // Worker posts this from the thread.  Pattern matches /cmd:
    // unpack the client data, render, persist, reset UI state,
    // auto-save if anything's in history.
    void OnGrepComplete(wxCommandEvent& evt)
    {
        if (m_isClosing) return;

        auto* data = static_cast<GrepResultClientData*>(evt.GetClientObject());
        if (!data) {
            SetStreamingState(false);
            m_chatState = ChatState::Idle;
            return;
        }
        const GrepResult& r = data->GetResult();

        // ── Agent mode routing ──────────────────────────────────
        // When a loop is active and awaiting this result, the
        // controller builds the ToolBlock itself and fires the
        // next iteration — skip the normal path entirely.
        if (m_agentController->IsActive()) {
            bool consumed = m_agentController->HandleGrepComplete(r);
            if (consumed) return;
            // Fall through only if controller declined (shouldn't
            // happen in practice since grep only runs via agent or
            // /grep, never both simultaneously — but defensive).
        }

        // ── Slash arm (Phase 4 unified) ────────────────────────
        // Build a ToolInvocationResult from GrepResult, then render
        // + persist via the shared helper.  Same shape the agent's
        // HandleGrepComplete uses (minus the toolCallId threading).
        ToolInvocationResult tir;
        tir.toolTag       = tool_names::kGrep;
        tir.invocationRaw.clear();
        tir.iconUtf8      = "\xF0\x9F\x94\x8D";   // 🔍
        tir.toolName      = "Grep";
        tir.commandEcho   = r.commandEcho;
        tir.chips         = r.chips;
        tir.body          = r.body;
        tir.errorBody     = r.errorBody;
        tir.bodyLang      = r.bodyLang;

        RenderAndPersistSlashResult(tir);

        SetStreamingState(false);
        m_chatState = ChatState::Idle;

        if (!m_chatHistory->IsEmpty())
            m_convController->AutoSaveConversation();
    }

    // ── Slash-command handlers ───────────────────────────────────
    // After Phase 4, tool-shaped slash commands all route through
    // HandleSlashCommand → DispatchInvocation, the same path the agent
    // uses.  Stateful conversation commands keep their own handlers:
    //   - /cd mutates the per-conversation tool cwd.
    //   - /goal manages Goals Phase 1 mission state.
    //
    // /cd resolution: per-conversation tool CWD if set, else the
    // conversation workspace.  Env-var expansion (%USERPROFILE% etc.)
    // is handled inside ResolveToolPath.  AutoSave fires only if
    // the conversation has content — empty-conversation /cd lives
    // in memory until the first real message pins it to disk.

    // Ensures the current conversation has a stable identity and a
    // user-visible workflow folder before tools or attachments need a
    // real path on disk.  Conversation JSON still saves under
    // %LOCALAPPDATA%\LlamaBoss\conversations, but files for this chat
    // live under %USERPROFILE%\LlamaBoss\Workflows\chat_xxxxxxxx.
    void EnsureConversationWorkflow()
    {
        if (!m_chatHistory->HasFilePath())
            m_chatHistory->SetFilePath(ChatHistory::GenerateFilePath());

        ChatHistory::EnsureWorkflowDir(m_chatHistory->GetFilePath());
    }

    // Resolves the effective working directory for a tool invocation:
    // per-conversation override first, falling back to this
    // conversation's own workflow Workspace folder.  Never returns
    // empty.  The Workspace folder is created on first use rather than
    // up-front in EnsureWorkflowDir, so chats that never invoke tools
    // don't grow an empty Workspace/ subfolder.
    std::string ResolveCurrentCwd()
    {
        std::string cwd = m_chatHistory->GetToolCwd();
        if (cwd.empty()) {
            EnsureConversationWorkflow();
            cwd = ChatHistory::GetConversationWorkspaceDir(m_chatHistory->GetFilePath());
            wxFileName::Mkdir(wxString::FromUTF8(cwd),
                              wxS_DIR_DEFAULT,
                              wxPATH_MKDIR_FULL);
        }
        return cwd;
    }

    void HandleSlashCd(const std::string& arg)
    {
        // Trim surrounding whitespace — users sometimes paste paths
        // with trailing newlines from the terminal.
        std::string path = arg;
        {
            size_t a = path.find_first_not_of(" \t\r\n");
            size_t b = path.find_last_not_of(" \t\r\n");
            path = (a == std::string::npos) ? std::string()
                                             : path.substr(a, b - a + 1);
        }

        if (path.empty()) {
            m_chatDisplay->DisplaySystemMessage(
                "Usage: /cd <path>   (use /pwd to show the current directory)");
            return;
        }

        std::string resolved = ResolveToolPath(path, ResolveCurrentCwd());
        if (resolved.empty()) {
            m_chatDisplay->DisplaySystemMessage(
                "Could not resolve path: " + path);
            return;
        }

        if (!IsDirectory(resolved)) {
            m_chatDisplay->DisplaySystemMessage(
                "Not a directory: " + resolved);
            return;
        }

        m_chatHistory->SetToolCwd(resolved);
        m_chatDisplay->DisplaySystemMessage(
            "Working directory: " + resolved);

        // Persist only if the conversation already has content.
        // Empty-conversation /cd stays in memory until a real message
        // triggers the first save (see AutoSaveConversation guards).
        if (!m_chatHistory->IsEmpty())
            m_convController->AutoSaveConversation();
    }


    void DisplayGoalStatus()
    {
        const GoalState& goal = m_chatHistory->GetGoalState();
        if (!goal.HasGoal()) {
            m_chatDisplay->DisplaySystemMessage(
                "No goal is set. Start one with 'Make this a goal: <objective>' or /goal <objective>.");
            return;
        }

        std::ostringstream body;
        body << "Goal status: " << GoalStatusLabel(goal.status) << "\n"
             << "Objective: " << goal.objective << "\n"
             << "Contract: " << GoalContractStatusLabel(goal.contract.status);

        if (goal.contract.IsReady()) {
            if (!goal.contract.successCriteria.empty()) {
                body << "\nSuccess criteria:";
                for (const auto& item : goal.contract.successCriteria)
                    body << "\n- " << item;
            }
            if (!goal.contract.constraints.empty()) {
                body << "\nConstraints:";
                for (const auto& item : goal.contract.constraints)
                    body << "\n- " << item;
            }
            if (!goal.contract.evidenceChecks.empty()) {
                body << "\nEvidence checks:";
                for (const auto& item : goal.contract.evidenceChecks)
                    body << "\n- " << item;
            }
        }
        else if (!goal.contract.lastBuilderReason.empty()) {
            body << "\nContract note: " << goal.contract.lastBuilderReason;
        }

        if (goal.turnsUsed > 0 || goal.IsBudgetReached()) {
            body << "\nAutomatic continuations used: "
                 << goal.turnsUsed << "/" << kGoalMaxAutoContinuations;
        }
        if (!goal.lastVerifierReason.empty()) {
            body << "\nLast verifier: " << goal.lastVerifierReason;
        }
        if (!goal.lastInterruptionReason.empty()) {
            body << "\nLast interruption: " << goal.lastInterruptionReason;
        }
        if (goal.IsAwaitingUser() && !goal.awaitingUserReason.empty()) {
            body << "\nAwaiting user: " << goal.awaitingUserReason;
        }
        if (!goal.structuredAgentEvidence.empty()) {
            body << "\nStructured evidence events: "
                 << goal.structuredAgentEvidence.size();
        }

        m_chatDisplay->DisplaySystemMessage(body.str());
    }

    void HandleSlashGoal(const std::string& arg)
    {
        std::string text = arg;
        {
            size_t a = text.find_first_not_of(" \t\r\n");
            size_t b = text.find_last_not_of(" \t\r\n");
            text = (a == std::string::npos) ? std::string()
                                             : text.substr(a, b - a + 1);
        }

        const std::string command = LbLowerAscii(text);

        // Goal control commands must be sent by themselves. Without this
        // guard, a pasted multi-line message such as:
        //
        //   /goal resume
        //   Begin working on the goal.
        //
        // falls through as a brand-new goal objective whose literal text is
        // "resume\nBegin working on the goal."  That silently corrupts the
        // existing goal state.  Reject that shape explicitly and ask the user
        // to send the follow-up instruction as a separate message.
        {
            const size_t newlinePos = text.find_first_of("\r\n");
            if (newlinePos != std::string::npos) {
                std::string firstLine = text.substr(0, newlinePos);
                const size_t firstA = firstLine.find_first_not_of(" \t\r\n");
                const size_t firstB = firstLine.find_last_not_of(" \t\r\n");
                firstLine = (firstA == std::string::npos)
                    ? std::string()
                    : firstLine.substr(firstA, firstB - firstA + 1);

                std::string trailing = text.substr(newlinePos + 1);
                const size_t trailingA = trailing.find_first_not_of(" \t\r\n");
                const bool hasTrailingContent = trailingA != std::string::npos;

                const std::string firstCommand = LbLowerAscii(firstLine);
                const bool isStandaloneGoalControl =
                    firstCommand == "status" ||
                    firstCommand == "pause" ||
                    firstCommand == "resume" ||
                    firstCommand == "clear" ||
                    firstCommand == "verify" ||
                    firstCommand == "continue" ||
                    firstCommand == "rebuild" ||
                    firstCommand == "rebuild contract" ||
                    firstCommand == "contract rebuild";

                if (isStandaloneGoalControl && hasTrailingContent) {
                    m_chatDisplay->DisplaySystemMessage(
                        "/goal " + firstLine +
                        " must be sent by itself. Send the next instruction as a separate message.");
                    return;
                }
            }
        }

        if (text.empty() || command == "status") {
            DisplayGoalStatus();
            return;
        }

        if (command == "pause") {
            if (!m_chatHistory->HasGoal()) {
                m_chatDisplay->DisplaySystemMessage(
                    "No goal is set. Start one with 'Make this a goal: <objective>' or /goal <objective>.");
            } else if (m_chatHistory->HasPausedGoal()) {
                m_chatDisplay->DisplaySystemMessage("Goal is already paused.");
            } else {
                m_chatHistory->PauseGoal();
                RefreshGoalStatusStrip();
                m_chatDisplay->DisplaySystemMessage("Goal paused.");
                m_convController->AutoSaveConversation();
            }
            return;
        }

        if (command == "resume") {
            if (!m_chatHistory->HasGoal()) {
                m_chatDisplay->DisplaySystemMessage(
                    "No goal is set. Start one with 'Make this a goal: <objective>' or /goal <objective>.");
            } else if (m_chatHistory->HasActiveGoal()) {
                m_chatDisplay->DisplaySystemMessage("Goal is already active.");
            } else {
                m_chatHistory->ResumeGoal();
                RefreshGoalStatusStrip();
                m_chatDisplay->DisplaySystemMessage("Goal resumed.");
                m_convController->AutoSaveConversation();
            }
            return;
        }

        if (command == "clear") {
            if (!m_chatHistory->HasGoal()) {
                m_chatDisplay->DisplaySystemMessage("No goal is set.");
            } else {
                m_chatHistory->ClearGoal();
                RefreshGoalStatusStrip();
                m_chatDisplay->DisplaySystemMessage("Goal cleared.");
                m_convController->AutoSaveConversation();
            }
            return;
        }

        if (command == "verify") {
            if (!m_chatHistory->HasActiveGoal()) {
                m_chatDisplay->DisplaySystemMessage(
                    "No active goal is available to verify. Start a goal or resume the existing one first.");
            } else if (IsBusy()) {
                m_chatDisplay->DisplaySystemMessage(
                    "Goal verification cannot start while LlamaBoss is busy.");
            } else {
                BeginGoalVerificationIfNeeded(true);
            }
            return;
        }

        if (command == "continue") {
            if (!m_chatHistory->HasActiveGoal() &&
                !m_chatHistory->HasAwaitingUserGoal()) {
                m_chatDisplay->DisplaySystemMessage(
                    "No active goal is available to continue. Start a goal or resume the existing one first.");
            } else if (!m_agentModeEnabled) {
                m_chatDisplay->DisplaySystemMessage(
                    "Goal continuation requires Agent mode.");
            } else if (IsBusy()) {
                m_chatDisplay->DisplaySystemMessage(
                    "Goal continuation cannot start while LlamaBoss is busy.");
            } else {
                const bool wasAwaitingUser = m_chatHistory->HasAwaitingUserGoal();
                if (wasAwaitingUser) {
                    m_chatHistory->ResumeGoal();
                    RefreshGoalStatusStrip();
                    m_convController->AutoSaveConversation();
                    m_chatDisplay->DisplaySystemMessage(
                        "Resuming the goal that was waiting for your input.");
                } else {
                    m_chatDisplay->DisplaySystemMessage(
                        "Continuing the active goal.");
                }

                BeginGoalContinuationTurn(
                    wasAwaitingUser
                        ? "The user provided input and resumed a waiting goal."
                        : "The user requested goal continuation.");
            }
            return;
        }

        if (command == "rebuild" || command == "rebuild contract" || command == "contract rebuild") {
            if (!m_chatHistory->HasActiveGoal()) {
                m_chatDisplay->DisplaySystemMessage(
                    "No active goal is available for contract rebuilding. Start a goal or resume the existing one first.");
            } else if (IsBusy()) {
                m_chatDisplay->DisplaySystemMessage(
                    "Goal contract rebuilding cannot start while LlamaBoss is busy.");
            } else {
                m_chatHistory->BeginGoalContractDrafting();
                m_goalAutoStartAfterContractBuild = false;
                m_chatDisplay->DisplaySystemMessage("Goal contract rebuild requested.");
                m_convController->AutoSaveConversation();
                CallAfter([this]() {
                    BeginGoalContractBuildIfNeeded();
                });
            }
            return;
        }

        m_chatHistory->StartGoal(text);
        RefreshGoalStatusStrip();
        m_goalAutoStartAfterContractBuild = true;
        m_convController->AutoSaveConversation();
        m_chatDisplay->DisplaySystemMessage(
            "Goal started. You can say 'show goal status', 'pause the goal', 'resume the goal', 'continue the goal', 'verify the goal', 'rebuild the goal contract', or 'clear the goal'.");

        CallAfter([this]() {
            BeginGoalContractBuildIfNeeded();
        });
    }

    const char* GoalStructuredEvidenceEventLabel(AgentEventType type) const
    {
        switch (type) {
        case AgentEventType::LoopBegin:        return "LoopBegin";
        case AgentEventType::IterationBegin:   return "IterationBegin";
        case AgentEventType::ToolCall:         return "ToolCall";
        case AgentEventType::ToolOutput:       return "ToolOutput";
        case AgentEventType::ApprovalRequired: return "ApprovalRequired";
        case AgentEventType::AgentStatus:      return "AgentStatus";
        case AgentEventType::Error:            return "Error";
        case AgentEventType::TurnComplete:     return "TurnComplete";
        case AgentEventType::FileCreated:      return "FileCreated";
        case AgentEventType::EditApplied:      return "EditApplied";
        case AgentEventType::DirectoryCreated: return "DirectoryCreated";
        case AgentEventType::FileDeleted:      return "FileDeleted";
        case AgentEventType::LoopEnd:          return "LoopEnd";
        }
        return "Unknown";
    }

    std::string GoalStructuredEvidenceChips(const ToolBlock& block) const
    {
        if (block.statusChips.empty()) return std::string();

        std::ostringstream chips;
        for (size_t i = 0; i < block.statusChips.size(); ++i) {
            if (i) chips << ", ";
            chips << block.statusChips[i];
        }
        return chips.str();
    }

    void AppendGoalStructuredAgentEvidence(std::string chunk)
    {
        chunk = LbTrimAscii(std::move(chunk));
        if (chunk.empty()) return;

        m_chatHistory->AppendGoalStructuredAgentEvidence(chunk);
    }

    void RecordGoalStructuredAgentEvidence(const AgentEvent& event)
    {
        if (!m_chatHistory->HasActiveGoal()) return;

        // Iteration bookkeeping is useful for orchestration but adds little
        // completion proof.  Keep the evidence packet focused on work facts.
        if (event.type == AgentEventType::LoopBegin ||
            event.type == AgentEventType::IterationBegin) {
            return;
        }

        std::ostringstream evidence;
        evidence << "- Event: " << GoalStructuredEvidenceEventLabel(event.type) << "\n";

        switch (event.type) {
        case AgentEventType::ToolCall:
            if (!event.toolName.empty())
                evidence << "  Tool: " << event.toolName << "\n";
            if (!event.commandEcho.empty())
                evidence << "  Command: " << event.commandEcho << "\n";
            if (!event.toolCallId.empty())
                evidence << "  Tool call id: " << event.toolCallId << "\n";
            break;

        case AgentEventType::ToolOutput:
        case AgentEventType::ApprovalRequired:
        case AgentEventType::AgentStatus:
        case AgentEventType::Error:
        case AgentEventType::FileCreated:
        case AgentEventType::EditApplied:
        case AgentEventType::DirectoryCreated:
        case AgentEventType::FileDeleted:
        {
            const ToolBlock& block = event.toolBlock;

            if (!block.toolName.empty())
                evidence << "  Tool: " << block.toolName << "\n";
            if (!block.commandEcho.empty())
                evidence << "  Command: " << block.commandEcho << "\n";

            const std::string chips = GoalStructuredEvidenceChips(block);
            if (!chips.empty())
                evidence << "  Tool result metadata (not file content): "
                         << chips << "\n";

            if (!block.presentedFiles.empty()) {
                for (const auto& file : block.presentedFiles) {
                    evidence << "  Presented artifact: "
                             << (file.displayName.empty()
                                     ? std::string("(unnamed)")
                                     : file.displayName);
                    if (!file.diskPath.empty())
                        evidence << " | disk path: " << file.diskPath;
                    if (file.sizeBytes > 0)
                        evidence << " | bytes: " << file.sizeBytes;
                    if (file.lineCount > 0)
                        evidence << " | lines: " << file.lineCount;
                    evidence << "\n";
                }
            }

            const std::string body = LbTrimAscii(block.body);
            if (!body.empty()) {
                evidence << "  Body excerpt:\n"
                         << LbClipForGoalVerifier(body, 1800) << "\n";
            }

            const std::string error = LbTrimAscii(block.errorBody);
            if (!error.empty()) {
                evidence << "  Error excerpt:\n"
                         << LbClipForGoalVerifier(error, 900) << "\n";
            }
            break;
        }

        case AgentEventType::TurnComplete:
            if (!event.userFacingMessage.empty()) {
                evidence << "  Final deterministic assistant message:\n"
                         << LbClipForGoalVerifier(
                                LbTrimAscii(event.userFacingMessage), 1200)
                         << "\n";
            }
            break;

        case AgentEventType::LoopEnd:
            evidence << "  End reason: ";
            switch (event.endReason) {
            case AgentEndReason::Normal:        evidence << "normal"; break;
            case AgentEndReason::Cancelled:     evidence << "cancelled"; break;
            case AgentEndReason::IterationCap:  evidence << "tool_step_cap"; break;
            case AgentEndReason::MalformedCap:  evidence << "malformed_tool_cap"; break;
            case AgentEndReason::StreamError:   evidence << "stream_error"; break;
            case AgentEndReason::SendFailed:    evidence << "send_failed"; break;
            case AgentEndReason::LoopGuard:     evidence << "loop_guard"; break;
            }
            evidence << "\n";
            if (!event.userFacingMessage.empty()) {
                evidence << "  Loop-end message: "
                         << LbClipForGoalVerifier(
                                LbTrimAscii(event.userFacingMessage), 800)
                         << "\n";
            }
            break;

        case AgentEventType::LoopBegin:
        case AgentEventType::IterationBegin:
            break;
        }

        AppendGoalStructuredAgentEvidence(evidence.str());
    }

    std::string BuildGoalStructuredAgentEvidence() const
    {
        const auto& structuredEvidence =
            m_chatHistory->GetGoalStructuredAgentEvidence();

        if (structuredEvidence.empty()) {
            return "(No structured AgentEvent evidence has been recorded for this saved goal.)";
        }

        constexpr size_t kMaxTotalBytes = 18000;
        std::ostringstream out;
        size_t totalBytes = 0;

        for (const auto& chunk : structuredEvidence) {
            std::string clipped = LbClipForGoalVerifier(chunk, 2600);
            if (totalBytes + clipped.size() + 2 > kMaxTotalBytes) {
                out << "[structured AgentEvent evidence truncated]\n";
                break;
            }
            out << clipped << "\n\n";
            totalBytes += clipped.size() + 2;
        }

        const std::string text = out.str();
        return text.empty()
            ? std::string("(No structured AgentEvent evidence has been recorded for this saved goal.)")
            : text;
    }

    std::string LatestAssistantGoalReplyForAwaitUserFallback() const
    {
        const auto& messages = m_chatHistory->GetMessages();
        for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
            const auto& msg = *it;
            if (!msg) continue;

            std::string role;
            std::string content;
            try { role = msg->getValue<std::string>("role"); }
            catch (...) { role.clear(); }
            if (role != "assistant") continue;

            try { content = msg->getValue<std::string>("content"); }
            catch (...) { content.clear(); }

            content = LbTrimAscii(content);
            if (!content.empty())
                return content;
        }
        return std::string();
    }

    bool LatestAssistantGoalReplyLooksLikeBlockingUserQuestion() const
    {
        const std::string latest =
            LbLowerAscii(LatestAssistantGoalReplyForAwaitUserFallback());
        if (latest.empty()) return false;

        const bool hasQuestionMark =
            latest.find('?') != std::string::npos;

        const bool asksChoiceOrDecision =
            latest.find("which option") != std::string::npos ||
            latest.find("which do you prefer") != std::string::npos ||
            latest.find("which would you prefer") != std::string::npos ||
            latest.find("what do you prefer") != std::string::npos ||
            latest.find("do you prefer") != std::string::npos ||
            latest.find("would you like") != std::string::npos ||
            latest.find("do you want") != std::string::npos ||
            latest.find("please choose") != std::string::npos ||
            latest.find("please let me know") != std::string::npos ||
            latest.find("please provide") != std::string::npos ||
            latest.find("could you please") != std::string::npos ||
            latest.find("can you confirm") != std::string::npos ||
            latest.find("need your choice") != std::string::npos ||
            latest.find("need your confirmation") != std::string::npos ||
            latest.find("need you to confirm") != std::string::npos;

        const bool explicitlyWaiting =
            latest.find("i am waiting for") != std::string::npos ||
            latest.find("i'll wait for") != std::string::npos ||
            latest.find("i will wait for") != std::string::npos ||
            latest.find("before i can continue") != std::string::npos ||
            latest.find("before creating") != std::string::npos ||
            latest.find("before proceeding") != std::string::npos ||
            latest.find("i am blocked until") != std::string::npos ||
            latest.find("i'm blocked until") != std::string::npos ||
            latest.find("i am still blocked") != std::string::npos ||
            latest.find("i'm still blocked") != std::string::npos ||
            latest.find("until the working directory is changed") != std::string::npos ||
            latest.find("once the directory is set correctly") != std::string::npos;

        const bool reportsWritableRootBlock =
            latest.find("outside of my allowed writable workspace") != std::string::npos ||
            latest.find("outside of my current writable workspace") != std::string::npos ||
            latest.find("outside the allowed write roots") != std::string::npos ||
            latest.find("outside of the allowed write roots") != std::string::npos ||
            latest.find("file system restriction") != std::string::npos ||
            latest.find("refuses to edit outside") != std::string::npos;

        const bool requestsUserUnblockAction =
            latest.find("please run /cd") != std::string::npos ||
            latest.find("run /cd") != std::string::npos ||
            latest.find("change the working directory") != std::string::npos ||
            latest.find("change the directory") != std::string::npos ||
            latest.find("ensure the source directory is within") != std::string::npos;

        // Keep this fallback deliberately conservative.  The verifier's model
        // verdict remains primary; this only rescues obvious cases where the
        // assistant is plainly waiting on the user, but the verifier returned
        // CONTINUE instead of AWAIT_USER.
        return (hasQuestionMark && asksChoiceOrDecision) ||
               (hasQuestionMark && explicitlyWaiting) ||
               (asksChoiceOrDecision && explicitlyWaiting) ||
               (reportsWritableRootBlock &&
                (explicitlyWaiting || requestsUserUnblockAction));
    }

    std::string BuildGoalVerifierRecentEvidence() const
    {
        std::ostringstream evidence;
        const auto& messages = m_chatHistory->GetMessages();

        if (messages.empty()) {
            return "(No recent transcript evidence.)";
        }

        constexpr size_t kMaxMessages = 8;
        constexpr size_t kMaxDefaultMessageBytes = 1400;
        constexpr size_t kMaxToolResultMessageBytes = 3600;
        constexpr size_t kMaxTotalBytes = 12000;

        const size_t start = messages.size() > kMaxMessages
            ? messages.size() - kMaxMessages
            : 0;

        size_t totalBytes = 0;
        for (size_t i = start; i < messages.size(); ++i) {
            const auto& msg = messages[i];
            if (!msg) continue;

            std::string role;
            std::string content;
            try { role = msg->getValue<std::string>("role"); }
            catch (...) { role = "unknown"; }
            try { content = msg->getValue<std::string>("content"); }
            catch (...) { content.clear(); }

            content = LbTrimAscii(content);
            if (content.empty()) continue;

            const bool looksLikeToolResult =
                content.compare(0, 7, "[tool: ") == 0;
            const size_t clipBudget = looksLikeToolResult
                ? kMaxToolResultMessageBytes
                : kMaxDefaultMessageBytes;
            content = LbClipForGoalVerifier(content, clipBudget);

            std::ostringstream block;
            block << "[" << role << "]\n"
                  << content << "\n\n";
            const std::string chunk = block.str();

            if (totalBytes + chunk.size() > kMaxTotalBytes) {
                evidence << "[evidence truncated]\n";
                break;
            }

            evidence << chunk;
            totalBytes += chunk.size();
        }

        const std::string out = evidence.str();
        return out.empty() ? std::string("(No recent transcript evidence.)") : out;
    }

    std::string BuildGoalProjectContextBlock(const char* purposeLabel) const
    {
        if (!m_chatHistory->HasProject()) return std::string();

        const std::string projectRoot = m_chatHistory->GetProjectRoot();
        const std::string projectMdPath =
            ProjectManager::ProjectInstructionsPath(projectRoot);

        std::string projectInstructions;
        std::string projectInstructionsStatus;
        const bool loadedProjectInstructions =
            ProjectManager::ReadProjectInstructions(
                projectRoot,
                projectInstructions,
                projectInstructionsStatus,
                6000);

        std::ostringstream p;
        p << "ACTIVE PROJECT CONTEXT";
        if (purposeLabel && *purposeLabel)
            p << " FOR " << purposeLabel;
        p << ":\n"
          << "Project name: " << m_chatHistory->GetProjectName() << "\n"
          << "Project root: " << projectRoot << "\n"
          << "PROJECT.md: " << projectMdPath << "\n"
          << "project.json: " << ProjectManager::ProjectJsonPath(projectRoot) << "\n"
          << "Standard project lanes: Inputs\\, Outputs\\, Workflows\\, Notes\\, Sources\\, Templates\\, PROJECT.md, project.json.\n"
          << "Use this project context only when the active Goal is project-related. Do not force unrelated Goals into project deliverables or invent project requirements.\n";

        const auto projectSources =
            ProjectManager::ListProjectSources(projectRoot, 12);
        if (!projectSources.empty()) {
            p << "Project source files available in Sources/:\n";
            for (const auto& src : projectSources) {
                p << "- " << src.name
                  << " (" << ProjectSource_HumanBytes(src.sizeBytes) << ")\n"
                  << "  " << src.path << "\n";
            }
        }

        const auto projectWorkflows =
            ProjectManager::ListProjectWorkflows(projectRoot, 12);
        if (!projectWorkflows.empty()) {
            p << "Project workflow files available in Workflows/:\n";
            for (const auto& wf : projectWorkflows) {
                p << "- " << wf.name
                  << " (" << ProjectSource_HumanBytes(wf.sizeBytes) << ")\n"
                  << "  " << wf.path << "\n";
            }
        }

        const auto projectWorkflowScripts =
            ProjectManager::ListProjectWorkflowScripts(projectRoot, 12);
        if (!projectWorkflowScripts.empty()) {
            p << "Project workflow helper scripts available in Workflows/:\n";
            for (const auto& script : projectWorkflowScripts) {
                p << "- " << script.name
                  << " (" << ProjectSource_HumanBytes(script.sizeBytes) << ")\n"
                  << "  " << script.path << "\n";
            }
        }

        if (loadedProjectInstructions) {
            p << "Project contract loaded from PROJECT.md:\n"
              << "--- PROJECT.md START ---\n"
              << projectInstructions;
            if (!projectInstructions.empty() &&
                projectInstructions.back() != '\n') {
                p << "\n";
            }
            p << "--- PROJECT.md END ---\n";

            if (!projectInstructionsStatus.empty()) {
                p << "Project contract note: "
                  << projectInstructionsStatus << "\n";
            }
        } else {
            p << "PROJECT.md was not loaded";
            if (!projectInstructionsStatus.empty()) {
                p << ": " << projectInstructionsStatus;
            }
            p << "\n";
        }

        return p.str();
    }



    void BeginSkillDraftBuildFromPendingDescription()
    {
        if (m_isClosing) return;
        if (!m_pendingSkillAuthoring.active) return;
        if (m_pendingSkillAuthoring.skillPath.empty()) return;
        if (m_pendingSkillAuthoring.userDescription.empty()) return;
        if (m_skillDraftBuilderInFlight) return;
        if (IsBusy()) return;

        if (!m_modelSwitcher->m_serverReady) {
            m_chatDisplay->DisplaySystemMessage(
                "Skill drafting was skipped because the model server is not ready. "
                "The design session is still active; say `draft this Skill` again once the model is ready.");
            return;
        }

        const std::string model = m_appState->GetModel();
        if (model.empty()) {
            m_chatDisplay->DisplaySystemMessage(
                "Skill drafting was skipped because no model is selected. "
                "Select a model, then say `draft this Skill` again.");
            return;
        }

        const SkillPromptBuilderInput skillPromptInput{
            m_pendingSkillAuthoring.skillName,
            m_pendingSkillAuthoring.skillPath,
            m_pendingSkillAuthoring.pythonHelperPath,
            m_pendingSkillAuthoring.userDescription
        };

        ChatHistory builderHistory;
        builderHistory.AddUserMessage(
            BuildSkillDraftBuilderUserPrompt(skillPromptInput));

        int ctxTokens = m_appState->GetCtxSize();
        if (ctxTokens <= 0) ctxTokens = 8192;

        std::string body = builderHistory.BuildChatRequestJson(
            model,
            true,
            BuildSkillDraftBuilderSystemPrompt(skillPromptInput),
            ctxTokens);

        DiscardPendingAssistantDelta();
        m_skillDraftBuilderInFlight = true;

        ++m_generationId;
        m_chatState = ChatState::Streaming;
        SetStreamingState(true);

        m_chatDisplay->DisplaySystemMessage(
            "Skill builder is drafting the initial Skill definition and deciding whether a reusable Python helper belongs with it.");

        if (!m_chatClient->SendMessage(
                model, m_appState->GetApiUrl(), body, m_generationId)) {
            m_skillDraftBuilderInFlight = false;
            SetStreamingState(false);
            m_chatDisplay->DisplaySystemMessage(
                "Failed to start Skill drafting. The design session is still active; say `draft this Skill` again and I will retry.");
        }
    }

    void HandleSkillDraftBuilderComplete(const std::string& builderResponse)
    {
        m_skillDraftBuilderInFlight = false;
        m_chatClient->ResetStreamingState();
        DiscardPendingAssistantDelta();
        SetStreamingState(false);

        if (!m_pendingSkillAuthoring.active ||
            m_pendingSkillAuthoring.skillPath.empty()) {
            return;
        }

        LbSkillDraftBuilderPayload payload =
            LbParseSkillDraftBuilderPayload(builderResponse);
        std::string markdown = std::move(payload.markdown);
        std::string pythonHelperSource = std::move(payload.pythonHelperSource);

        const bool draftRequestsPythonHelper =
            LbSkillMarkdownRequestsPythonHelper(markdown);
        const bool generatedPythonHelper = !pythonHelperSource.empty();

        if (payload.malformedHelperBlock ||
            !LbLooksLikeSkillMarkdown(markdown) ||
            draftRequestsPythonHelper != generatedPythonHelper ||
            (generatedPythonHelper && m_pendingSkillAuthoring.pythonHelperPath.empty())) {
            m_chatDisplay->DisplaySystemMessage(
                "The Skill builder did not return a complete, internally consistent Skill draft. "
                "The design session is still active; refine it if needed, then say `draft this Skill` again.");
            if (!m_chatHistory->IsEmpty())
                m_convController->AutoSaveConversation();
            return;
        }

        if (markdown.empty() || markdown.back() != '\n')
            markdown.push_back('\n');
        if (generatedPythonHelper &&
            (pythonHelperSource.empty() || pythonHelperSource.back() != '\n')) {
            pythonHelperSource.push_back('\n');
        }

        if (!LbWriteUtf8TextFile(m_pendingSkillAuthoring.skillPath, markdown)) {
            m_chatDisplay->DisplaySystemMessage(
                "The Skill draft was generated, but LlamaBoss could not save it to disk. "
                "The starter SKILL.md file was left as-is.");
            if (!m_chatHistory->IsEmpty())
                m_convController->AutoSaveConversation();
            return;
        }

        const std::string savedPath = m_pendingSkillAuthoring.skillPath;
        const std::string pythonHelperPath = generatedPythonHelper
            ? m_pendingSkillAuthoring.pythonHelperPath
            : std::string();

        bool pythonHelperCreated = false;
        bool pythonHelperWriteFailed = false;
        if (generatedPythonHelper) {
            pythonHelperCreated = LbWriteUtf8TextFile(
                pythonHelperPath,
                pythonHelperSource);
            pythonHelperWriteFailed = !pythonHelperCreated;
        }

        m_pendingSkillAuthoring = PendingSkillAuthoring{};
        InvalidateProjectContextCache();
        RefreshProjectStrip();

        std::ostringstream msg;
        msg << "Skill definition drafted and saved:\n"
            << savedPath;
        if (pythonHelperCreated) {
            msg << "\n\nThe Skill builder determined that reusable Python code would help here, so I created a Python helper script for this Skill:\n"
                << pythonHelperPath;
        } else if (pythonHelperWriteFailed) {
            msg << "\n\nThe Skill builder drafted a Python helper for this Skill, but LlamaBoss could not save the `.py` file. "
                << "The SKILL.md draft was still saved, so review the Skill before trying to run it.";
        } else {
            msg << "\n\nNo separate Python helper was created. This Skill draft relies on LlamaBoss's available tools or approved command execution when it runs.";
        }
        msg << "\n\nYou can now ask me to use this Skill whenever you are ready, or open it from the Skills menu to review it. I will not run it until you ask.";

        // Skills Phase 2E: close the setup flow as a real assistant turn,
        // not a system-status line.  Keeping this completion in chat history
        // gives the next ordinary user message a clean conversational handoff
        // instead of leaving the model parked on the original Skill request.
        const std::string completion = msg.str();
        const std::string model = m_appState->GetModel();
        m_chatDisplay->DisplayAssistantMessage(
            ServerManager::ModelDisplayName(model),
            completion,
            m_appState->GetTheme().chatAssistant);
        m_chatHistory->AddAssistantMessage(completion, model);

        if (!m_chatHistory->IsEmpty())
            m_convController->AutoSaveConversation();
    }

    void HandleSkillDraftBuilderError(const std::string& error)
    {
        m_skillDraftBuilderInFlight = false;
        DiscardPendingAssistantDelta();
        m_chatClient->ResetStreamingState();
        SetStreamingState(false);

        std::string msg =
            "Skill drafting failed. The design session is still active; refine it if needed, then say `draft this Skill` again.";
        const std::string trimmed = LbTrimAscii(error);
        if (!trimmed.empty())
            msg += " " + trimmed.substr(0, std::min<size_t>(trimmed.size(), 240));

        m_chatDisplay->DisplaySystemMessage(msg);
        if (!m_chatHistory->IsEmpty())
            m_convController->AutoSaveConversation();
    }

    std::string BuildGoalSkillContextBlock(const char* purposeLabel) const
    {
        std::ostringstream skillBody;
        AppendSkillsBlock(skillBody);

        const std::string listedSkills = skillBody.str();
        if (listedSkills.empty()) return std::string();

        std::ostringstream p;
        p << "AVAILABLE LLAMABOSS SKILL CONTEXT";
        if (purposeLabel && *purposeLabel)
            p << " FOR " << purposeLabel;
        p << ":\n"
          << "Skills are cross-project reusable abilities. Use this Skill context only when the active Goal explicitly names a Skill or clearly requires an outcome that matches a listed Skill. Do not force unrelated Goals into Skill use or invent Skill requirements.\n"
          << listedSkills;

        return p.str();
    }

    GoalContractPromptInput BuildGoalContractPromptInput() const
    {
        const GoalState& goal = m_chatHistory->GetGoalState();

        GoalContractPromptInput input;
        input.skillContext = BuildGoalSkillContextBlock("CONTRACT DRAFTING");
        input.projectContext = BuildGoalProjectContextBlock("CONTRACT DRAFTING");
        input.objective = goal.objective;
        return input;
    }

    void BeginGoalContractBuildIfNeeded()
    {
        if (m_isClosing) return;
        if (!m_chatHistory->HasActiveGoal()) return;
        if (m_goalContractBuilderInFlight) return;
        if (m_chatHistory->GoalContractReady()) return;
        if (IsBusy()) return;

        if (!m_modelSwitcher->m_serverReady) {
            m_goalAutoStartAfterContractBuild = false;
            m_chatHistory->MarkGoalContractFailed(
                "Contract drafting was skipped because the model server was not ready.");
            m_convController->AutoSaveConversation();
            m_chatDisplay->DisplaySystemMessage(
                "Goal contract drafting was skipped because the model server is not ready. "
                "The goal remains active with objective-only verification.");
            return;
        }

        const std::string model = m_appState->GetModel();
        if (model.empty()) {
            m_goalAutoStartAfterContractBuild = false;
            m_chatHistory->MarkGoalContractFailed(
                "Contract drafting was skipped because no model is selected.");
            m_convController->AutoSaveConversation();
            m_chatDisplay->DisplaySystemMessage(
                "Goal contract drafting was skipped because no model is selected. "
                "The goal remains active with objective-only verification.");
            return;
        }

        ChatHistory contractHistory;
        contractHistory.AddUserMessage(BuildGoalContractBuilderUserPrompt(BuildGoalContractPromptInput()));

        int ctxTokens = m_appState->GetCtxSize();
        if (ctxTokens <= 0) ctxTokens = 8192;

        std::string body = contractHistory.BuildChatRequestJson(
            model,
            true,
            BuildGoalContractBuilderSystemPrompt(),
            ctxTokens);

        DiscardPendingAssistantDelta();
        m_chatHistory->BeginGoalContractDrafting();
        m_goalContractBuilderInFlight = true;

        ++m_generationId;
        m_chatState = ChatState::Streaming;
        SetStreamingState(true);

        m_chatDisplay->DisplaySystemMessage(
            "Goal contract builder is drafting success criteria.");

        if (!m_chatClient->SendMessage(
                model, m_appState->GetApiUrl(), body, m_generationId)) {
            m_goalContractBuilderInFlight = false;
            m_goalAutoStartAfterContractBuild = false;
            SetStreamingState(false);
            m_chatHistory->MarkGoalContractFailed(
                "Failed to start the contract drafting request.");
            m_convController->AutoSaveConversation();
            m_chatDisplay->DisplaySystemMessage(
                "Failed to start goal contract drafting. "
                "The goal remains active with objective-only verification.");
        }
    }

    void HandleGoalContractBuilderComplete(const std::string& builderResponse)
    {
        const bool autoStartAfterContractBuild = m_goalAutoStartAfterContractBuild;
        m_goalAutoStartAfterContractBuild = false;
        m_goalContractBuilderInFlight = false;
        m_chatClient->ResetStreamingState();
        DiscardPendingAssistantDelta();
        SetStreamingState(false);

        if (!m_chatHistory->HasGoal()) {
            return;
        }

        GoalContractDraft draft = ParseGoalContractDraft(builderResponse);
        if (!draft.Parsed()) {
            m_chatHistory->MarkGoalContractFailed(
                "Contract builder did not return usable SUCCESS lines.");
            m_convController->AutoSaveConversation();
            const std::string statusText =
                "Goal contract builder could not draft a structured contract. "
                "The goal remains active with objective-only verification.";
            CallAfter([this, statusText]() {
                if (m_isClosing) return;
                m_chatDisplay->DisplaySystemMessage(statusText);
            });
            return;
        }

        std::string reason = LbTrimAscii(draft.reason);
        if (reason.empty())
            reason = "Structured contract drafted from the goal.";

        const size_t successCount = draft.successCriteria.size();
        const size_t constraintCount = draft.constraints.size();
        const size_t evidenceCount = draft.evidenceChecks.size();

        m_chatHistory->SetGoalContractReady(
            draft.successCriteria,
            draft.constraints,
            draft.evidenceChecks,
            reason);
        m_convController->AutoSaveConversation();

        std::ostringstream msg;
        msg << "Goal contract ready: "
            << successCount << " success "
            << (successCount == 1 ? "criterion" : "criteria")
            << ", " << constraintCount << " "
            << (constraintCount == 1 ? "constraint" : "constraints")
            << ", and " << evidenceCount << " evidence "
            << (evidenceCount == 1 ? "check" : "checks")
            << ". Say 'show goal status' to review it.";

        const std::string statusText = msg.str();
        CallAfter([this, statusText, autoStartAfterContractBuild]() {
            if (m_isClosing) return;
            m_chatDisplay->DisplaySystemMessage(statusText);

            if (!autoStartAfterContractBuild ||
                !m_chatHistory->HasActiveGoal()) {
                return;
            }

            if (!m_agentModeEnabled) {
                m_chatDisplay->DisplaySystemMessage(
                    "Goal contract is ready. Agent mode is off, so automatic goal start was skipped. "
                    "Turn on Agent mode and say 'continue the goal'.");
                return;
            }

            if (IsBusy()) {
                m_chatDisplay->DisplaySystemMessage(
                    "Goal contract is ready, but LlamaBoss is busy. Say 'continue the goal' when ready.");
                return;
            }

            m_chatDisplay->DisplaySystemMessage(
                "Goal contract is ready. Starting work on the active goal.");
            BeginGoalContinuationTurn(
                "The goal contract is ready. Begin work on the active goal.");
        });
    }

    void HandleGoalContractBuilderError(const std::string& error)
    {
        m_goalContractBuilderInFlight = false;
        m_goalAutoStartAfterContractBuild = false;
        DiscardPendingAssistantDelta();
        m_chatClient->ResetStreamingState();
        SetStreamingState(false);

        if (m_chatHistory->HasGoal()) {
            m_chatHistory->MarkGoalContractFailed(
                "Goal contract drafting request failed.");
            m_convController->AutoSaveConversation();
        }

        std::string msg =
            "Goal contract drafting failed. "
            "The goal remains active with objective-only verification.";
        const std::string trimmed = LbTrimAscii(error);
        if (!trimmed.empty())
            msg += " " + LbClipForGoalVerifier(trimmed, 240);

        m_chatDisplay->DisplaySystemMessage(msg);
    }

    GoalVerifierPromptInput BuildGoalVerifierPromptInput() const
    {
        const GoalState& goal = m_chatHistory->GetGoalState();

        GoalVerifierPromptInput input;
        input.skillContext = BuildGoalSkillContextBlock("VERIFICATION");
        input.projectContext = BuildGoalProjectContextBlock("VERIFICATION");
        input.objective = goal.objective;
        input.hasReadyContract = goal.contract.IsReady();
        input.successCriteria = goal.contract.successCriteria;
        input.constraints = goal.contract.constraints;
        input.evidenceChecks = goal.contract.evidenceChecks;
        input.awaitingUserPromptEvidence = goal.awaitingUserPromptEvidence;
        input.awaitingUserReplyEvidence = goal.awaitingUserReplyEvidence;
        input.structuredAgentEvidence = BuildGoalStructuredAgentEvidence();
        input.recentTranscriptEvidence = BuildGoalVerifierRecentEvidence();
        return input;
    }

    void BeginGoalVerificationIfNeeded(bool manualOnly = false)
    {
        if (m_isClosing || m_goalVerifierInFlight) return;
        if (!m_agentModeEnabled || !m_chatHistory->HasActiveGoal()) return;
        if (IsBusy()) return;

        if (!m_modelSwitcher->m_serverReady) {
            m_chatDisplay->DisplaySystemMessage(
                "Goal verifier could not run because the model server is not ready. "
                "The goal remains active.");
            return;
        }

        const std::string model = m_appState->GetModel();
        if (model.empty()) {
            m_chatDisplay->DisplaySystemMessage(
                "Goal verifier could not run because no model is selected. "
                "The goal remains active.");
            return;
        }

        ChatHistory verifierHistory;
        verifierHistory.AddUserMessage(BuildGoalVerifierUserPrompt(BuildGoalVerifierPromptInput()));

        int ctxTokens = m_appState->GetCtxSize();
        if (ctxTokens <= 0) ctxTokens = 8192;

        std::string body = verifierHistory.BuildChatRequestJson(
            model,
            true,
            BuildGoalVerifierSystemPrompt(),
            ctxTokens);

        DiscardPendingAssistantDelta();
        m_goalVerifierManualOnly = manualOnly;
        m_goalVerifierInFlight = true;

        ++m_generationId;
        m_chatState = ChatState::Streaming;
        SetStreamingState(true);

        m_chatDisplay->DisplaySystemMessage(
            "Goal verifier is checking whether the goal is complete.");

        if (!m_chatClient->SendMessage(
                model, m_appState->GetApiUrl(), body, m_generationId)) {
            m_goalVerifierInFlight = false;
            m_goalVerifierManualOnly = false;
            SetStreamingState(false);
            m_chatDisplay->DisplaySystemMessage(
                "Failed to start goal verification. The goal remains active.");
        }
    }

    void HandleGoalVerifierComplete(const std::string& verifierResponse)
    {
        const bool manualOnly = m_goalVerifierManualOnly;
        m_goalVerifierManualOnly = false;
        m_goalVerifierInFlight = false;
        m_chatClient->ResetStreamingState();
        DiscardPendingAssistantDelta();
        SetStreamingState(false);

        if (!m_chatHistory->HasActiveGoal()) {
            return;
        }

        GoalVerifierVerdict verdict = ParseGoalVerifierVerdict(verifierResponse);
        if (!verdict.Parsed()) {
            m_chatHistory->NoteGoalVerifierUnclear(
                "Verifier did not return a clear COMPLETE, CONTINUE, or AWAIT_USER verdict.");
            m_convController->AutoSaveConversation();

            // Defer the verifier-result status card until after the stream-
            // completion event fully unwinds. In testing, synchronous status
            // messages from this hidden verifier completion path could be
            // skipped visually even though goal state updated correctly.
            const std::string statusText =
                "Goal verifier could not return a clear COMPLETE, CONTINUE, or AWAIT_USER verdict. "
                "The goal remains active.";
            CallAfter([this, statusText]() {
                if (m_isClosing) return;
                m_chatDisplay->DisplaySystemMessage(statusText);
            });
            return;
        }

        std::string reason = LbTrimAscii(verdict.reason);
        if (reason.empty()) {
            reason = verdict.IsComplete()
                ? "The verifier judged the goal satisfied."
                : "The verifier judged the goal not yet complete.";
        }

        if (verdict.IsComplete()) {
            m_chatHistory->MarkGoalVerifiedComplete(reason);
            RefreshGoalStatusStrip();
            m_convController->AutoSaveConversation();

            const std::string statusText =
                "Goal verified complete. " + reason;
            CallAfter([this, statusText]() {
                if (m_isClosing) return;
                m_chatDisplay->DisplaySystemMessage(statusText);
            });
            return;
        }

        if (verdict.IsAwaitUser()) {
            m_chatHistory->MarkGoalAwaitingUser(
                reason,
                LbClipForGoalVerifier(
                    LatestAssistantGoalReplyForAwaitUserFallback(), 1800));
            RefreshGoalStatusStrip();
            m_convController->AutoSaveConversation();

            const std::string statusText =
                "Goal is waiting for your input. " + reason +
                " Reply in chat, then say 'continue the goal' to resume work.";
            CallAfter([this, statusText]() {
                if (m_isClosing) return;
                m_chatDisplay->DisplaySystemMessage(statusText);
            });
            return;
        }

        if (LatestAssistantGoalReplyLooksLikeBlockingUserQuestion()) {
            m_chatHistory->MarkGoalAwaitingUser(
                reason,
                LbClipForGoalVerifier(
                    LatestAssistantGoalReplyForAwaitUserFallback(), 1800));
            RefreshGoalStatusStrip();
            m_convController->AutoSaveConversation();

            const std::string statusText =
                "Goal is waiting for your input. " + reason +
                " Reply in chat, then say 'continue the goal' to resume work.";
            CallAfter([this, statusText]() {
                if (m_isClosing) return;
                m_chatDisplay->DisplaySystemMessage(statusText);
            });
            return;
        }

        m_chatHistory->NoteGoalVerifierContinue(reason);
        m_convController->AutoSaveConversation();

        if (manualOnly) {
            const std::string statusText =
                "Goal verifier: continue - " + reason;
            CallAfter([this, statusText]() {
                if (m_isClosing) return;
                m_chatDisplay->DisplaySystemMessage(statusText);
            });
            return;
        }

        if (!m_chatHistory->CanGoalAutoContinue(kGoalMaxAutoContinuations)) {
            m_chatHistory->MarkGoalBudgetReached(reason);
            RefreshGoalStatusStrip();
            m_convController->AutoSaveConversation();

            std::ostringstream msg;
            msg << "Goal verifier: continue - " << reason << "\n"
                << "Automatic continuation budget reached ("
                << kGoalMaxAutoContinuations << "/"
                << kGoalMaxAutoContinuations
                << "). Say 'resume the goal' to reopen this goal.";

            const std::string statusText = msg.str();
            CallAfter([this, statusText]() {
                if (m_isClosing) return;
                m_chatDisplay->DisplaySystemMessage(statusText);
            });
            return;
        }

        m_chatHistory->ConsumeGoalAutoContinuation();
        m_convController->AutoSaveConversation();
        const int used = m_chatHistory->GetGoalState().turnsUsed;

        std::ostringstream msg;
        msg << "Goal verifier: continue - " << reason << "\n"
            << "Continuing automatically ("
            << used << "/" << kGoalMaxAutoContinuations << ").";

        const std::string statusText = msg.str();
        CallAfter([this, statusText, reason]() {
            if (m_isClosing) return;
            m_chatDisplay->DisplaySystemMessage(statusText);
            BeginGoalContinuationTurn(reason);
        });
    }

    void HandleGoalVerifierError(const std::string& error)
    {
        m_goalVerifierInFlight = false;
        m_goalVerifierManualOnly = false;
        DiscardPendingAssistantDelta();
        m_chatClient->ResetStreamingState();
        SetStreamingState(false);

        if (m_chatHistory->HasActiveGoal()) {
            m_chatHistory->NoteGoalVerifierUnclear(
                "Goal verification request failed.");
            m_convController->AutoSaveConversation();
        }

        std::string msg =
            "Goal verification failed. The goal remains active.";
        const std::string trimmed = LbTrimAscii(error);
        if (!trimmed.empty())
            msg += " " + LbClipForGoalVerifier(trimmed, 240);

        m_chatDisplay->DisplaySystemMessage(msg);
    }

    void BeginGoalContinuationTurn(const std::string& verifierReason)
    {
        if (m_isClosing) return;
        if (!m_agentModeEnabled || !m_chatHistory->HasActiveGoal()) return;
        if (IsBusy()) return;

        if (!m_modelSwitcher->m_serverReady) {
            m_chatDisplay->DisplaySystemMessage(
                "Automatic goal continuation could not start because the model server is not ready. "
                "The goal remains active.");
            return;
        }

        std::ostringstream continuation;
        continuation
            << "Goal continuation instruction:\n"
            << "The active goal is not yet complete.\n"
            << "Verifier reason: "
            << (LbTrimAscii(verifierReason).empty()
                    ? std::string("The verifier requested more work.")
                    : LbTrimAscii(verifierReason))
            << "\n"
            << "Continue with the next concrete step toward the active goal. "
            << "Use tools when useful. Do not claim the goal is complete unless "
            << "the requested outcome is actually satisfied.";

        m_chatHistory->AddSystemMessage(continuation.str());

        const std::string model = m_appState->GetModel();
        int ctxTokens = m_appState->GetCtxSize();
        if (ctxTokens <= 0) ctxTokens = 8192;

        std::string tools;
        const bool native = (_activeProtocol == ToolProtocol::Native);
        if (native) {
            tools = GetCachedToolsArrayJson();
        }

        std::string body = m_chatHistory->BuildChatRequestJson(
            model,
            true,
            BuildAgentSystemPrompt(),
            ctxTokens,
            tools,
            native);

        m_chatHistory->AddAssistantPlaceholder(model);
        m_chatDisplay->DisplayAssistantPrefix(
            ServerManager::ModelDisplayName(model),
            m_appState->GetTheme().chatAssistant);

        if (!m_chatHistory->HasFilePath())
            m_chatHistory->SetFilePath(ChatHistory::GenerateFilePath());
        {
            std::string genDir = ChatHistory::GetGeneratedFilesDir(
                m_chatHistory->GetFilePath());

            size_t msgIdx = m_chatHistory->GetMessageCount() > 0
                ? m_chatHistory->GetMessageCount() - 1
                : 0;

            m_chatDisplay->SetFilePersistenceContext(genDir, msgIdx);
        }

        ResetAgentToolStreamFilter();
        m_goalAutoContinuationTurn = true;
        m_agentController->Begin();

        DiscardPendingAssistantDelta();
        ++m_generationId;
        m_chatState = ChatState::Streaming;
        SetStreamingState(true);

        if (!m_chatClient->SendMessage(
                model, m_appState->GetApiUrl(), body, m_generationId)) {
            if (m_agentController->IsActive()) {
                ResetAgentToolStreamFilter();
                m_agentController->HandleAssistantError(
                    "Failed to start automatic goal continuation");
            }

            SetStreamingState(false);
            m_chatDisplay->DisplaySystemMessage(
                "Failed to start automatic goal continuation. The goal remains active.");
            m_chatHistory->RemoveLastAssistantMessage();
        }
    }

    // ─── Unified slash-command dispatch (Phase 4 / 4.1) ──────────
    //
    // Every tool-shaped slash command — /read, /ls, /grep, /pwd,
    // /open, /cmd (Phase 4) plus /write, /mkdir, /edit, /delete
    // (Phase 4.1) — flows through HandleSlashCommand below.  The
    // method builds a ToolInvocation, calls DispatchInvocation, and
    // either renders the sync result or sets the chat-state so the
    // matching OnGrepComplete / OnCmdComplete picks up the async
    // continuation.
    //
    // /cd and /goal are NOT tools — they mutate per-conversation state
    // and keep dedicated handlers.  Everything else that used to live in
    // HandleSlashRead/Ls/Grep/Open/Pwd is gone: dispatch,
    // validation, rendering, and history are now identical to the
    // agent path.
    //
    // toolCallId is always empty for slash invocations: there is no
    // model-emitted call to thread.  AddUserMessage (rather than
    // AddToolResultMessage) is therefore the correct persistence
    // call for the result — see RenderAndPersistSlashResult.
    //
    // Behavioral deltas vs Phase 3 user-typed slash:
    //   - /pwd renders as a tool card now (Pwd icon + body) instead
    //     of a system message.  Same body text, different chrome.
    //   - /cmd is now classified by the PowerShell policy layer
    //     (EvaluatePowerShellCommand), matching the agent path.
    //     Clearly read-only commands run immediately, broader valid
    //     commands pause for approval, and malformed commands render
    //     with a "blocked" chip plus an explanation in errorBody.
    //   - /cmd chip ordering becomes [status, elapsed, truncated?]
    //     to match the saved-history order and the agent path
    //     (previously the on-screen order was [elapsed, status]).
    void HandleSlashCommand(const std::string& toolName,
                            const std::string& args)
    {
        // Pending indicators for async tools.  Slash UX wants
        // immediate feedback while the worker runs; the agent path
        // explicitly skips these because the eventual ToolBlock is
        // the user-facing surface and a duplicate would just be
        // noise.  For slash, the user's keystroke is the trigger and
        // a hint is welcome.
        if (toolName == tool_names::kGrep && !args.empty()) {
            // Re-extract pattern + path purely for the indicator.
            // The dispatcher re-extracts identically inside DoGrep.
            std::string s = args;
            size_t a = s.find_first_not_of(" \t\r\n");
            if (a == std::string::npos)      s.clear();
            else if (a > 0)                  s.erase(0, a);

            size_t sep = s.find_first_of(" \t");
            std::string pat = (sep == std::string::npos) ? s
                                                          : s.substr(0, sep);
            std::string rawPath = (sep == std::string::npos)
                ? std::string()
                : s.substr(sep + 1);
            {
                size_t pa = rawPath.find_first_not_of(" \t\r\n");
                size_t pb = rawPath.find_last_not_of(" \t\r\n");
                if (pa == std::string::npos) rawPath.clear();
                else                          rawPath = rawPath.substr(pa, pb - pa + 1);
            }

            std::string ctxCwd  = ResolveCurrentCwd();
            std::string target  = rawPath.empty() ? ctxCwd : rawPath;
            std::string resolved = ResolveToolPath(target, ctxCwd);
            if (!resolved.empty() && !pat.empty()) {
                m_chatDisplay->DisplaySystemMessage(
                    "\xF0\x9F\x94\x8D Grep: '" + pat + "' in " + resolved);
            }
        } else if (toolName == tool_names::kPowerShell && !args.empty()) {
            m_chatDisplay->DisplaySystemMessage(
                "\xE2\x9A\x99 PowerShell: " + args);
        } else if (toolName == tool_names::kPythonHealth) {
            m_chatDisplay->DisplaySystemMessage(
                "\xF0\x9F\x90\x8D Python Health");
        } else if (toolName == tool_names::kCsvInspect) {
            m_chatDisplay->DisplaySystemMessage(
                "\xF0\x9F\x93\x8A CSV Inspect");
        } else if (toolName == tool_names::kCsvReport) {
            m_chatDisplay->DisplaySystemMessage(
                "\xF0\x9F\x93\x9D CSV Report");
        } else if (toolName == tool_names::kCsvToXlsx) {
            m_chatDisplay->DisplaySystemMessage(
                "\xF0\x9F\x93\x97 CSV to XLSX");
        } else if (toolName == tool_names::kXlsxInspect) {
            m_chatDisplay->DisplaySystemMessage(
                "\xF0\x9F\x93\x8A XLSX Inspect");
        } else if (toolName == tool_names::kXlsxReport) {
            m_chatDisplay->DisplaySystemMessage(
                "\xF0\x9F\x93\x97 XLSX Report");
        } else if (toolName == tool_names::kXlsxCreateWorkbook) {
            m_chatDisplay->DisplaySystemMessage(
                "\xF0\x9F\x93\x97 Create Workbook");
        } else if (toolName == tool_names::kPdfExtractText) {
            m_chatDisplay->DisplaySystemMessage(
                "\xF0\x9F\x93\x84 PDF Extract Text");
        } else if (toolName == tool_names::kPythonRunScript) {
            m_chatDisplay->DisplaySystemMessage(
                "\xF0\x9F\x90\x8D Python Run");
        } else if (toolName == tool_names::kNotesRead ||
                   toolName == tool_names::kNotesAppend) {
            m_chatDisplay->DisplaySystemMessage(
                "\xF0\x9F\x93\x92 Notes");
        } else if (toolName == tool_names::kProjectNotesRead ||
                   toolName == tool_names::kProjectNotesAppend) {
            m_chatDisplay->DisplaySystemMessage(
                "\xF0\x9F\x93\x93 Project Notes");
        } else if (toolName == tool_names::kWebFetchUrl) {
            m_chatDisplay->DisplaySystemMessage(
                "\xF0\x9F\x8C\x90 Web Page Inspect");
        }

        // Build the protocol-neutral invocation.  Validation runs
        // here so DispatchInvocation can fold a shape-level rejection
        // into the same Invalid-outcome path the parser uses for
        // malformed agent <tool_call> blocks.
        ToolInvocation inv;
        inv.name     = toolName;
        inv.args     = args;
        inv.rawBlock.clear();    // no <tool_call> source for slash
        inv.toolCallId.clear();  // no native id threading for slash

        std::string reason;
        inv.valid         = ValidateToolArgs(toolName, args, reason);
        inv.invalidReason = reason;

        ToolContext ctx = BuildToolContext();

        // Approval polish: skip the approval card if this chat is
        // already trusted (plain approve / approve always) or this
        // tool was remembered by an older build.  Read-only tools
        // never enter the gate at all (RequiresApproval returns false).
        const bool alreadyApproved =
            m_chatHistory && m_chatHistory->IsToolChatApproved(toolName);

        tool_approval::ApprovalDecision approval;
        if (!alreadyApproved &&
            tool_approval::RequiresApproval(inv, ctx, approval)) {
            if (HasPendingApproval()) {
                m_chatDisplay->DisplaySystemMessage(
                    "Approval is already pending. Use the buttons above to respond.");
                return;
            }
            m_pendingSlashApproval.invocation = inv;
            m_pendingSlashApproval.context    = ctx;
            m_pendingSlashApproval.active     = true;
            // UX polish: keep the approval card calm by default. The full
            // preview/source remains one click away under [show details].
            // requiresApproval=true tells ChatDisplay to render the Allow
            // Once / Allow Always / Deny buttons inline beneath
            // [show details]; the typed-command fallback in
            // TryHandlePendingApprovalInput remains as a keyboard safety net.
            ToolBlock card = approval.block;
            card.requiresApproval = true;
            m_chatDisplay->DisplayToolBlock(card, false);
            SetApprovalState(true);
            return;
        }

        DispatchOutcome out = DispatchInvocation(
            inv, ctx, m_grepExecutor.get(), m_cmdExecutor.get(), m_pythonRunner.get());

        switch (out.status) {
        case DispatchStatus::Completed:
        case DispatchStatus::Invalid:
            RenderAndPersistSlashResult(out.result);
            if (!m_chatHistory->IsEmpty())
                m_convController->AutoSaveConversation();
            return;

        case DispatchStatus::Async:
            // The dispatcher already started the worker.  Set the
            // chat-state so OnGrepComplete / OnCmdComplete know which
            // path to take, and arm streaming so Stop is wired.
            if (toolName == tool_names::kGrep) {
                m_chatState = ChatState::RunningGrep;
            } else if (toolName == tool_names::kPowerShell) {
                m_chatState = ChatState::RunningCmd;
            } else if (IsPythonAsyncToolName(toolName)) {
                m_chatState = ChatState::RunningPython;
            }
            SetStreamingState(true);
            return;
        }
    }

    // Render + persist a completed ToolInvocationResult on the slash
    // path.  Mirrors the agent's FeedResultAndIterate but writes via
    // AddUserMessage (no tool_call_id) and does not iterate.  Shared
    // by sync slash dispatch and the slash arm of the async
    // completion handlers (OnCmdComplete, OnGrepComplete).
    void RenderAndPersistSlashResult(const ToolInvocationResult& r)
    {
        ChatDisplay::ToolBlock tb;
        tb.iconUtf8     = r.iconUtf8;
        tb.toolName     = r.toolName;
        tb.statusChips  = r.chips;
        tb.commandEcho  = r.commandEcho;
        tb.body         = r.body;
        tb.errorBody    = r.errorBody;
        tb.bodyLang     = r.bodyLang;
        tb.presentedFiles = r.presentedFiles;
        m_chatDisplay->DisplayToolBlock(tb, /*startExpanded=*/true);

        std::string formatted = ChatHistory::FormatToolBlockAsUserMessage(
            r.toolTag,
            r.commandEcho,
            r.body,
            r.errorBody,
            r.chips,
            r.bodyLang);
        m_chatHistory->AddUserMessage(formatted);
    }

    bool HasPendingApproval() const
    {
        return m_pendingSlashApproval.active ||
               (m_agentController && m_agentController->IsAwaitingApproval());
    }

    void ExecuteApprovedSlashTool(bool rememberForChat = false)
    {
        if (!m_pendingSlashApproval.active) return;

        ToolInvocation inv = m_pendingSlashApproval.invocation;
        ToolContext    ctx = m_pendingSlashApproval.context;
        m_pendingSlashApproval = PendingSlashApproval{};
        SetApprovalState(false);

        // Mark BEFORE dispatch so the per-chat approval state is
        // already in place if the model immediately requests another
        // approval-required tool.  "Approve always" now means
        // one-approval mode for this conversation, not just this
        // individual tool name.
        if (rememberForChat && m_chatHistory) {
            m_chatHistory->RememberAllToolApprovalsForChat();
        }

        DispatchOutcome out = DispatchInvocation(
            inv, ctx, m_grepExecutor.get(), m_cmdExecutor.get(), m_pythonRunner.get());

        switch (out.status) {
        case DispatchStatus::Completed:
        case DispatchStatus::Invalid:
            RenderAndPersistSlashResult(out.result);
            if (!m_chatHistory->IsEmpty())
                m_convController->AutoSaveConversation();
            return;

        case DispatchStatus::Async:
            if (inv.name == tool_names::kGrep) {
                m_chatState = ChatState::RunningGrep;
            } else if (inv.name == tool_names::kPowerShell) {
                m_chatState = ChatState::RunningCmd;
            } else if (IsPythonAsyncToolName(inv.name)) {
                m_chatState = ChatState::RunningPython;
            }
            SetStreamingState(true);
            return;
        }
    }

    void DenyPendingSlashTool(const std::string& message =
        "Denied by user. Tool was not executed.")
    {
        if (!m_pendingSlashApproval.active) return;

        ToolInvocation inv = m_pendingSlashApproval.invocation;
        m_pendingSlashApproval = PendingSlashApproval{};
        SetApprovalState(false);

        ToolInvocationResult r = tool_approval::DeniedResult(inv, message);
        RenderAndPersistSlashResult(r);
        if (!m_chatHistory->IsEmpty())
            m_convController->AutoSaveConversation();
    }

    void HandleApprovalCommand(bool approve, bool rememberForChat = false)
    {
        // Clear the visible button row regardless of resolution path.
        // The click path (HandleApprovalButtonClick) already cleared it
        // before invoking the callback, so this is a no-op there; the
        // typed-command fallback path hasn't, so this is where the row
        // actually vanishes for keyboard users.  Cheap either way.
        if (m_chatDisplay) m_chatDisplay->ClearApprovalButtons();

        if (m_pendingSlashApproval.active) {
            if (approve) ExecuteApprovedSlashTool(rememberForChat);
            else         DenyPendingSlashTool();
            return;
        }

        if (m_agentController && m_agentController->IsAwaitingApproval()) {
            SetApprovalState(false);
            if (approve) {
                bool ok = m_agentController->ApprovePendingTool(rememberForChat);
                if (ok && m_agentController->IsAwaitingAsyncResult())
                    SetStreamingState(true);
            } else {
                m_agentController->DenyPendingTool();
            }
            return;
        }

        SetApprovalState(false);
        m_chatDisplay->DisplaySystemMessage("No approval is pending.");
    }

    // Builds the execution context for a tool invocation at the
    // current instant: resolves CWD (per-conv override → app CWD),
    // resolves timeout (per-conv override → kDefaultToolTimeoutMs),
    // reads the active model's context size from AppState (so tools
    // can cap their bodies to fit), and packs in the alive-token +
    // event-handler hooks that threaded tools will need once we have
    // any.  /read uses ctxTokens today; /grep in a few turns will
    // use aliveToken and eventHandler.
    ToolContext BuildToolContext()
    {
        ToolContext ctx;
        ctx.cwd = ResolveCurrentCwd();

        unsigned long t = m_chatHistory->GetToolTimeoutMs();
        ctx.timeoutMs = (t == 0) ? kDefaultToolTimeoutMs : t;

        ctx.ctxTokens = m_appState->GetCtxSize();
        if (ctx.ctxTokens <= 0) ctx.ctxTokens = 8192;  // defensive

        ctx.eventHandler = this;
        ctx.aliveToken   = m_alive;

        // History-aware tools (/open's fuzzy-match against recent file listings,
        // and future view/edit/delete tools) walk through this.
        // Non-owning -- m_chatHistory outlives every tool invocation.
        ctx.history = m_chatHistory.get();

        // Projects: project metadata is passed to tools. File mutation
        // tools keep the chat cwd as their relative-path base, but the
        // active project root is also an allowed write root for absolute
        // project paths and project workflow creation.
        if (m_chatHistory->HasProject()) {
            ctx.activeProjectId = m_chatHistory->GetProjectId();
            ctx.activeProjectName = m_chatHistory->GetProjectName();
            ctx.activeProjectRoot = m_chatHistory->GetProjectRoot();
        }

        // Global reusable Skills are durable user-authored assets too.
        // Treat the Skills folder as an approval-gated mutation root so
        // the agent can finish, edit, and maintain SKILL.md contracts and
        // helper scripts in place instead of being forced to create stray
        // one-off workspace scripts.
        ctx.skillsRoot = ProjectManager::GetSkillsDir();
        return ctx;
    }

    static unsigned long long MixProjectContextHash(unsigned long long seed,
                                                    unsigned long long value)
    {
        // boost::hash_combine-style mixer. This signature is only for
        // in-process cache invalidation, not persistence or security.
        return seed ^ (value + 0x9e3779b97f4a7c15ull +
                       (seed << 6) + (seed >> 2));
    }

    static unsigned long long MixProjectContextString(unsigned long long seed,
                                                      const std::string& value)
    {
        return MixProjectContextHash(
            seed,
            static_cast<unsigned long long>(std::hash<std::string>{}(value)));
    }

    template <typename Items>
    static unsigned long long FingerprintProjectContextItems(const Items& items)
    {
        unsigned long long h = 1469598103934665603ull;
        h = MixProjectContextHash(h,
            static_cast<unsigned long long>(items.size()));

        for (const auto& item : items) {
            // The prompt lists name/path/size, but content-relevant edits
            // often keep the same directory timestamp. Include file mtime
            // so edited SKILL.md, workflow, source, and helper files
            // invalidate the cached system-prompt context.
            h = MixProjectContextString(h, item.name);
            h = MixProjectContextString(h, item.path);
            h = MixProjectContextHash(h,
                static_cast<unsigned long long>(item.sizeBytes));
            h = MixProjectContextHash(h,
                static_cast<unsigned long long>(LbPathMTimeTicks(item.path)));
        }

        return h;
    }

    ProjectContextCacheSignature BuildProjectContextCacheSignature() const
    {
        ProjectContextCacheSignature sig;
        sig.hasProject = m_chatHistory->HasProject();
        sig.skillsDirMTime =
            LbPathMTimeTicks(ProjectManager::GetSkillsDir());

        // Directory mtime catches add/remove/rename.  The listed-file
        // fingerprints catch edits to existing files that may not update
        // the parent directory timestamp. Keep the list cap aligned with
        // AppendSkillsBlock() so the cache only tracks prompt-visible items.
        sig.skillsListHash = FingerprintProjectContextItems(
            ProjectManager::ListSkills(30));
        sig.skillScriptsListHash = FingerprintProjectContextItems(
            ProjectManager::ListSkillScripts(30));

        if (sig.hasProject) {
            sig.projectRoot = m_chatHistory->GetProjectRoot();
            sig.projectName = m_chatHistory->GetProjectName();
            sig.projectJsonMTime =
                LbPathMTimeTicks(ProjectManager::ProjectJsonPath(sig.projectRoot));
            sig.projectMdMTime =
                LbPathMTimeTicks(ProjectManager::ProjectInstructionsPath(sig.projectRoot));
            sig.sourcesDirMTime =
                LbPathMTimeTicks(ProjectManager::ProjectSourcesPath(sig.projectRoot));
            sig.sourcesListHash = FingerprintProjectContextItems(
                ProjectManager::ListProjectSources(sig.projectRoot, 30));
            sig.workflowsDirMTime =
                LbPathMTimeTicks(ProjectManager::ProjectWorkflowsPath(sig.projectRoot));
            sig.workflowsListHash = FingerprintProjectContextItems(
                ProjectManager::ListProjectWorkflows(sig.projectRoot, 30));
            sig.workflowScriptsListHash = FingerprintProjectContextItems(
                ProjectManager::ListProjectWorkflowScripts(sig.projectRoot, 30));
        }

        return sig;
    }

    void InvalidateProjectContextCache() const
    {
        m_projectContextCache.valid = false;
    }

    // Emits the optional "LlamaBoss Skills" header section
    // when at least one Skill contract or helper script exists.  Kept
    // separate so it can run regardless of whether a project is attached --
    // Skills are cross-project reusable abilities by design.
    void AppendSkillsBlock(std::ostringstream& p) const
    {
        const auto skills = ProjectManager::ListSkills(30);
        const auto skillScripts = ProjectManager::ListSkillScripts(30);
        if (skills.empty() && skillScripts.empty()) return;

        p << "LlamaBoss Skills (cross-project; available even when no project is attached):\n"
          << "  Folder: " << ProjectManager::GetSkillsDir() << "\n"
          << "  Skill execution grounding rule: when the user asks to use, run, invoke, or continue a named Skill, the first tool call for that Skill request MUST read the listed SKILL.md contract path, even if that Skill was just drafted or used earlier in this same chat. Do not run a Skill helper, PowerShell step, or one-off replacement before reading the saved Skill contract.\n"
          << "  After the saved SKILL.md is read, treat that contract and the saved files in the Skill folder as the source of truth. Do not substitute temporary workspace scripts, design-time test scripts, or remembered helper names from an earlier Skill-design conversation unless the saved SKILL.md explicitly directs that exact file.\n"
          << "  Use a Skill by reading its SKILL.md contract first, then following its steps using normal tools and approval rules. python_run_script can run a same-folder .py helper by filename or in-lane path. If the helper needs runtime inputs, put the helper filename/path on the first args line and each optional command-line argument on its own later line; never call python_run_script with only data arguments and no .py script filename/path. Skill helper scripts are files, not tool names: never call the .py filename or Skill name directly as a tool. If an active project has a project workflow with the same name, the project workflow takes precedence.\n";

        if (!skills.empty()) {
            p << "  Skills (read the listed SKILL.md contract path before using one):\n";
            for (const auto& skill : skills) {
                p << "    - " << LbSkillDisplayNameFromContractPath(skill)
                  << " contract (" << ProjectSource_HumanBytes(skill.sizeBytes) << ")\n"
                  << "      SKILL.md: " << skill.path << "\n";
            }
        }

        if (!skillScripts.empty()) {
            p << "  Skill Python helper scripts (run only after reading the matching Skill and only when needed):\n";
            for (const auto& script : skillScripts) {
                p << "    - " << script.name
                  << " (" << ProjectSource_HumanBytes(script.sizeBytes) << ")\n"
                  << "      " << script.path << "\n";
            }
        }
    }

    std::string BuildActiveProjectContextBlockFresh() const
    {
        std::ostringstream p;
        AppendSkillsBlock(p);

        if (!m_chatHistory->HasProject()) return p.str();

        const std::string projectRoot = m_chatHistory->GetProjectRoot();
        const std::string projectMdPath = ProjectManager::ProjectInstructionsPath(projectRoot);

        std::string projectInstructions;
        std::string projectInstructionsStatus;
        const bool loadedProjectInstructions =
            ProjectManager::ReadProjectInstructions(projectRoot, projectInstructions, projectInstructionsStatus);

        p << "Active project:\n"
          << "  Name: " << m_chatHistory->GetProjectName() << "\n"
          << "  Root: " << projectRoot << "\n"
          << "  PROJECT.md: " << projectMdPath << "\n"
          << "  Sources folder: " << ProjectManager::ProjectSourcesPath(projectRoot) << "\n"
          << "  Notes folder: " << projectRoot << "\\Notes\n"
          << "  Project Workflows folder: " << ProjectManager::ProjectWorkflowsPath(projectRoot) << "\n"
          << "  This is a long-lived project folder attached to the current chat. Chat workspace files remain separate from project files.\n"
          << "  PROJECT.md is loaded into this system prompt on every request while the project is attached. Treat it as the project contract for project-related work.\n"
          << "  Project-relative paths are supported by tools for the standard project lanes: Inputs\\, Outputs\\, Workflows\\, Notes\\, Sources\\, Templates\\, PROJECT.md, and project.json resolve under the active project root. Use those short project-relative paths for durable project files instead of writing them to the chat workspace.\n"
          << "  The active project root is an allowed write root for write/mkdir/edit/delete. Other arbitrary relative paths still resolve against the chat workspace, so prefer the standard project lane prefixes above for project files.\n"
          << "  Follow PROJECT.md for project-related requests. For unrelated general questions or casual chat, answer normally and do not force the request into this project.\n"
          << "  Do not invent project sources, templates, workflows, or policies that are not provided. Do not modify PROJECT.md or other project files unless the user explicitly asks.\n"
          << "  Project-aware file use: when the user asks to inspect, summarize, open, extract, report on, or fill a file that appears in Project Sources, use the listed project source path or the source filename with the appropriate read/open/helper tool. Source files are read-only reference inputs; built-in helper artifacts still save to conversation workflow folders unless a workflow or user explicitly asks for a project output path.\n"
          << "  Project workflows: workflow files are reusable Markdown instruction plans in Workflows/. When the user asks to run or use a workflow, read the relevant workflow file first, then follow its steps using normal tools and approval rules. A workflow file is not automatic code execution by itself.\n"
          << "  Project workflow Python scripts: optional .py helper scripts may live in Workflows/. Do not run a project workflow script unless the workflow file or user request calls for it. python_run_script can run an active project's workflow script by filename or in-lane path; if the script needs runtime inputs, put the script filename/path on the first args line and each optional command-line argument on its own later line. read/open/ls/write/edit/delete/mkdir can use Workflows\\... project-relative paths; scripts run from the conversation workspace and can infer the project root from their own file path.\n"
          << "  Project notes: durable project-specific memory lives in Notes/NOTES.md. If the user says save this to my notes while this project is active, use notes_append so the full note is saved to project notes and a compact pointer is saved to global NOTES.md. If the user specifically says project notes, use project_notes_append.\n";

        const auto projectSources = ProjectManager::ListProjectSources(projectRoot, 30);
        if (projectSources.empty()) {
            p << "  Project sources: none attached yet.\n";
        } else {
            p << "  Project source files in Sources/ (names and paths only; use tools/helpers to inspect contents when needed):\n";
            for (const auto& src : projectSources) {
                p << "    - " << src.name
                  << " (" << ProjectSource_HumanBytes(src.sizeBytes) << ")\n"
                  << "      " << src.path << "\n";
            }
        }

        const auto projectWorkflows = ProjectManager::ListProjectWorkflows(projectRoot, 30);
        if (projectWorkflows.empty()) {
            p << "  Project workflows: none created yet.\n";
        } else {
            p << "  Project workflow files in Workflows/ (read the workflow file before running/using it):\n";
            for (const auto& wf : projectWorkflows) {
                p << "    - " << wf.name
                  << " (" << ProjectSource_HumanBytes(wf.sizeBytes) << ")\n"
                  << "      " << wf.path << "\n";
            }
        }

        const auto projectWorkflowScripts = ProjectManager::ListProjectWorkflowScripts(projectRoot, 30);
        if (projectWorkflowScripts.empty()) {
            p << "  Project workflow Python scripts: none created yet.\n";
        } else {
            p << "  Project workflow Python scripts in Workflows/ (run only after reading the matching workflow and only when needed):\n";
            for (const auto& script : projectWorkflowScripts) {
                p << "    - " << script.name
                  << " (" << ProjectSource_HumanBytes(script.sizeBytes) << ")\n"
                  << "      " << script.path << "\n";
            }
        }

        if (loadedProjectInstructions) {
            p << "\nProject contract loaded from PROJECT.md:\n"
              << "--- PROJECT.md START ---\n"
              << projectInstructions;
            if (!projectInstructions.empty() && projectInstructions.back() != '\n') {
                p << "\n";
            }
            p << "--- PROJECT.md END ---\n";

            if (!projectInstructionsStatus.empty()) {
                p << "Project contract note: " << projectInstructionsStatus << "\n";
            }
        } else {
            p << "PROJECT.md was not loaded";
            if (!projectInstructionsStatus.empty()) {
                p << ": " << projectInstructionsStatus;
            }
            p << "\n";
        }

        return p.str();
    }

    std::string BuildActiveProjectContextBlock() const
    {
        ProjectContextCacheSignature sig = BuildProjectContextCacheSignature();
        if (m_projectContextCache.valid &&
            m_projectContextCache.sig == sig) {
            return m_projectContextCache.block;
        }

        m_projectContextCache.block = BuildActiveProjectContextBlockFresh();
        m_projectContextCache.sig = std::move(sig);
        m_projectContextCache.valid = true;
        return m_projectContextCache.block;
    }

    // Writes the shared "Conversation goal: / Status: / Objective: / ..."
    // header pattern used by every non-cleared goal branch in
    // BuildActiveGoalContextBlock.  Each branch passes its own status
    // label (literal "paused", "awaiting user", etc., or
    // GoalStatusLabel(goal.status) for the active branch) and the raw
    // goal.objective text; the helper emits the indented objective lines
    // and the "(empty)" fallback when the objective is blank.
    //
    // Branch-specific tail content (contract details, waiting reason,
    // policy reminders) is appended by the caller after this returns.
    void AppendGoalObjectiveBlock(std::ostringstream& p,
                                  const std::string& statusLabel,
                                  const std::string& objective) const
    {
        p << "Conversation goal:\n"
          << "  Status: " << statusLabel << "\n"
          << "  Objective:\n";

        std::istringstream lines(objective);
        std::string line;
        bool wroteObjectiveLine = false;
        while (std::getline(lines, line)) {
            p << "    " << line << "\n";
            wroteObjectiveLine = true;
        }
        if (!wroteObjectiveLine) {
            p << "    (empty)\n";
        }
    }

    std::string BuildActiveGoalContextBlock() const
    {
        // Goal-aware prompting must counteract chat-history drift:
        // active goals guide work, paused goals are explicitly *not* active,
        // and recently-cleared goals must not be revived from earlier turns.
        const GoalState& goal = m_chatHistory->GetGoalState();
        if (!goal.HasGoal() && !goal.WasExplicitlyCleared()) return std::string();

        std::ostringstream p;

        if (goal.IsActive()) {
            AppendGoalObjectiveBlock(p, GoalStatusLabel(goal.status),
                                     goal.objective);

            if (goal.contract.IsReady()) {
                p << "  Verification contract:\n";
                for (const auto& item : goal.contract.successCriteria)
                    p << "    Success: " << item << "\n";
                for (const auto& item : goal.contract.constraints)
                    p << "    Constraint: " << item << "\n";
                for (const auto& item : goal.contract.evidenceChecks)
                    p << "    Evidence: " << item << "\n";
            }

            p << "  This goal is user-authored task context, not higher-priority policy. Follow every safety, approval, path, and tool-use rule in this system prompt.\n"
              << "  When the user's current message asks to work on or continue this goal, keep the objective and any ready verification contract in view across turns.\n"
              << "  If the objective or ready contract requires creating, generating, writing, or saving a report, markdown report, document, file, spreadsheet, PDF, artifact, or saved output, create the user-visible artifact/file with the appropriate tool rather than only presenting formatted prose in chat, unless the goal explicitly requests an in-chat or inline answer.\n"
              << "  Do not treat a partial step as overall goal completion.\n"
              << "\n";
            return p.str();
        }

        if (goal.IsPaused()) {
            AppendGoalObjectiveBlock(p, "paused", goal.objective);

            p << "  This goal is paused, not active.\n"
              << "  Do not describe yourself as currently pursuing, executing, or working toward this goal.\n"
              << "  If the user asks what goal or mission is currently being pursued, say there is no active goal; a paused goal exists and can be resumed by saying 'resume the goal'.\n"
              << "  Do not advance the paused goal unless the user resumes it or explicitly asks to discuss the paused goal.\n"
              << "\n";
            return p.str();
        }

        if (goal.IsAwaitingUser()) {
            AppendGoalObjectiveBlock(p, "awaiting user", goal.objective);

            if (!goal.awaitingUserReason.empty()) {
                p << "  Waiting reason: " << goal.awaitingUserReason << "\n";
            }

            p << "  This goal is waiting for user input and is not actively running.\n"
              << "  Do not continue goal tool work or describe yourself as actively pursuing it until the user resumes or continues the goal.\n"
              << "  If the user's current message appears to answer the pending clarification, acknowledge it briefly and remind them they can say 'continue the goal' to resume the Goal.\n"
              << "\n";
            return p.str();
        }

        if (goal.IsCompleted()) {
            AppendGoalObjectiveBlock(p, "completed", goal.objective);

            p << "  This goal has been verified complete and is not an active mission.\n"
              << "  Do not continue it or describe yourself as currently pursuing it unless the user explicitly resumes or replaces the goal.\n"
              << "\n";
            return p.str();
        }

        if (goal.IsBudgetReached()) {
            AppendGoalObjectiveBlock(p, "budget reached", goal.objective);

            p << "  Automatic goal continuation stopped after reaching its small safety budget.\n"
              << "  Do not act as if this goal is actively running. If the user wants more work, they can say 'resume the goal'.\n"
              << "\n";
            return p.str();
        }

        if (goal.WasExplicitlyCleared()) {
            p << "Conversation goal:\n"
              << "  Status: none\n"
              << "  A prior /goal was explicitly cleared in this conversation.\n"
              << "  There is currently no active or paused goal.\n"
              << "  Do not revive, continue, or describe earlier cleared goal text from chat history as the current mission.\n"
              << "  If the user asks what current goal or mission is being pursued, say no goal is set. The user can start one with 'Make this a goal: <objective>' or /goal <objective>.\n"
              << "  You may discuss the earlier goal only as past conversation history if the user explicitly asks about it as a past topic.\n"
              << "\n";
            return p.str();
        }

        return std::string();
    }

    std::string BuildPendingSkillAuthoringContextBlock() const
    {
        if (!m_pendingSkillAuthoring.active) return std::string();

        std::ostringstream p;
        p << "NEW SKILL DESIGN SESSION:\n"
          << "A reusable global LlamaBoss Skill is being designed conversationally before its files are drafted.\n"
          << "Skill name: " << m_pendingSkillAuthoring.skillName << "\n"
          << "Reserved SKILL.md path: " << m_pendingSkillAuthoring.skillPath << "\n"
          << "The reserved Skill folder exists, but the final Skill contract/helper files have not been drafted by the builder yet.\n"
          << "Stay in design-conversation mode. Help the user articulate the Skill's purpose and natural trigger phrases.\n"
          << "Bias toward FEWER clarifying questions, not more. When the user gives you a concrete value (a folder path, an output filename, a file-extension list, a specific URL or identifier), accept it as part of the Skill itself -- do not treat it as something to revisit later. The draft builder will encode those concrete values directly into the Skill's Steps, NOT into the 'Inputs to Ask For' section.\n"
          << "Only ask for an input value at use-time when the user explicitly says they want to choose that value each time the Skill runs. By default the Skill should hardcode every concrete value that came up in this conversation.\n"
          << "If the design conversation already used a real tool successfully (for example, an ls or notes_read whose result the user approved), the eventual Skill will mirror that exact tool sequence. Do not redesign the implementation around a different tool family just because it could also work.\n"
          << "Do not claim that the Skill has already been drafted, saved, or fully implemented until LlamaBoss later reports that the draft builder completed.\n"
          << "Do not create or edit the Skill files directly with general write/edit/overwrite tools during this design conversation. The application will run the dedicated Skill draft builder only after the user explicitly approves drafting.\n"
          << "Discuss practical implementation choices only when they are genuinely open. For straightforward local Windows/file operations, prefer existing LlamaBoss tools or approved PowerShell. Reserve Python for reusable custom logic those paths handle poorly.\n"
          << "Use tools when the user asks or when they materially help the design conversation, such as notes_read for a saved path the user references, or ls to confirm a folder layout.\n"
          << "When using tools in this Skill design conversation, follow the normal LlamaBoss tool-call protocol exactly. Do not improvise compact, partial, or alternate tool-call syntax.\n"
          << "When the design is clear enough, say this exact sentence on its own line: This design sounds ready. Say `draft this Skill` and I’ll write the Skill files.\n"
          << "If the user wants to stop the design session, they can type cancel.\n\n";
        return p.str();
    }

    std::string BuildPendingSkillDesignConversationBrief() const
    {
        if (!m_pendingSkillAuthoring.active) return std::string();

        const auto& messages = m_chatHistory->GetMessages();
        const size_t start = std::min(
            m_pendingSkillAuthoring.conversationStartMessageIndex,
            messages.size());

        std::ostringstream transcript;
        bool wroteAny = false;
        for (size_t i = start; i < messages.size(); ++i) {
            const auto& msg = messages[i];
            if (!msg || !msg->has("role") || !msg->has("content"))
                continue;

            const std::string role =
                msg->getValue<std::string>("role");
            if (role != "user" && role != "assistant")
                continue;

            std::string content =
                LbTrimAscii(msg->getValue<std::string>("content"));
            if (content.empty())
                continue;

            transcript << (role == "user" ? "User" : "Assistant")
                       << ":\n"
                       << LbClipForGoalVerifier(content, 1800)
                       << "\n\n";
            wroteAny = true;
        }

        if (!wroteAny) return std::string();
        return LbClipForGoalVerifier(transcript.str(), 12000);
    }

    std::string BuildNormalSystemPrompt() const
    {
        AgentPromptBuilderInput input;
        input.activeProjectContextBlock = BuildActiveProjectContextBlock();
        input.activeGoalContextBlock = BuildActiveGoalContextBlock();
        input.pendingSkillAuthoringContextBlock = BuildPendingSkillAuthoringContextBlock();
        return ::BuildNormalSystemPrompt(input);
    }

    // Agent-mode system prompt.  Prepended to each iteration's
    // request while the loop is active; not stored in history so
    // saved conversations stay clean.  Kept short — small models
    // follow short prompts much more reliably than long ones.
    // Phase 3c-ii: split the agent system prompt by tool protocol.
    // Native models receive a trimmed prompt (no XML grammar
    // examples, no "Available tool names" list) because the
    // wire-level `tools` field teaches the model what tools exist
    // and how to call them.  XML models still need the full
    // grammar tutorial below.
    //
    // BuildAgentSystemPrompt() is the dispatcher; it picks based
    // on _activeProtocol.  Both branches share workspace context
    // and per-tool behaviour notes via small helper composers.

    std::string BuildAgentSystemPrompt()
    {
        AgentPromptBuilderInput input;
        input.isWorkspace = m_chatHistory->GetToolCwd().empty();
        input.cwd = ResolveCurrentCwd();
        input.activeProjectContextBlock = BuildActiveProjectContextBlock();
        input.activeGoalContextBlock = BuildActiveGoalContextBlock();
        input.pendingSkillAuthoringContextBlock = BuildPendingSkillAuthoringContextBlock();
        input.toolSafetySummaryText = BuildToolSafetySummaryText(GetGlobalRouter());

        if (_activeProtocol == ToolProtocol::Native) {
            return ::BuildAgentSystemPromptNative(input);
        }
        return ::BuildAgentSystemPromptXml(input);
    }


    // Native-protocol prompt: short.  The wire `tools` array
    // already teaches the model the tool names, descriptions,
    // and parameter schemas — repeating any of that in prose
    // creates contradictions with whatever the chat template
    // generates from the structured tools.  Keep only:
    //   * Workspace context (cwd, "this is your LlamaBoss
    //     workspace" hint when no /cd override)
    //   * The no-cd-tool guidance (still relevant; cwd is
    //     conversation-scoped and only the user can change it)
    //   * Containment-failure guidance (don't retry the same
    //     out-of-cwd path)
    //   * Per-tool behavior notes that aren't in the schemas
    //     (open's media handling, write's no-overwrite rule,
    //     edit's exact-once contract, delete's non-recursive
    //     rule, PowerShell's read-only auto-run / approval-gated shell policy)
    //   * Single-call-per-reply rule




    // ═════════════════════════════════════════════════════════════
    //  Project status strip helpers
    // ═════════════════════════════════════════════════════════════

    // Pulls the current project + goal state from ChatHistory and
    // pushes it into the merged ProjectStatusStrip in one call.  Cheap
    // -- project counts are O(N) directory scans capped at small N
    // (identical to what the system prompt builder already does on
    // every send), goal fields are O(1) lookups on the in-memory
    // GoalState.
    //
    // RefreshGoalStatusStrip() is kept as a thin alias so the ~17 call
    // sites scattered through this file that touched only the goal
    // strip can stay unchanged.  Internal callers may use either name;
    // they do the same thing.  See TODO(rename) in
    // project_status_strip.h for the eventual cleanup.
    void RefreshProjectStrip()
    {
        if (!m_projectStrip) return;

        ProjectStatusStrip::State s;

        // ── Project half ─────────────────────────────────────────
        if (m_chatHistory->HasProject()) {
            s.hasProject  = true;
            s.projectName = m_chatHistory->GetProjectName();

            const std::string root = m_chatHistory->GetProjectRoot();
            s.sourceCount   = static_cast<int>(
                ProjectManager::ListProjectSources(root, 0).size());
            s.workflowCount = static_cast<int>(
                ProjectManager::ListProjectWorkflows(root, 0).size());
            s.scriptCount   = static_cast<int>(
                ProjectManager::ListProjectWorkflowScripts(root, 0).size());
        }

        // ── Goal half ────────────────────────────────────────────
        // Compact the objective on this side so the strip stays a
        // pure renderer.  96 bytes matches what the old
        // BuildGoalStatusStripText() used.
        const GoalState& goal = m_chatHistory->GetGoalState();
        if (goal.HasGoal()) {
            s.hasGoal              = true;
            s.goalStatusLabel      = GoalStatusLabel(goal.status);
            s.goalObjectiveCompact = LbCompactGoalStripText(goal.objective, 96);
        }

        m_projectStrip->Refresh(s);
    }

    // Thin alias preserved for source-compatibility with the old
    // separate goal strip.  Do not add logic here -- everything lives
    // in RefreshProjectStrip().
    void RefreshGoalStatusStrip()
    {
        RefreshProjectStrip();
    }

    // Builds and shows the project / skill popup menu beside the strip.
    // Items are context-sensitive.  When invoked from [ + New Skill ],
    // the same actions are shown, but Skill actions are placed first so
    // the menu matches the control the user clicked.
    void ShowProjectPopupMenu(wxWindow* anchor, bool skillFirst = false)
    {
        wxMenu menu;

        auto appendSkillActions = [&menu]() {
            menu.Append(ID_SKILL_NEW,         "New Skill...");
            menu.Append(ID_SKILL_OPEN,        "Open Skill...");
            menu.Append(ID_SKILL_OPEN_FOLDER, "Open Skills Folder");
        };

        if (!m_chatHistory->HasProject()) {
            if (skillFirst) {
                appendSkillActions();
                menu.AppendSeparator();
                menu.Append(ID_PROJECT_NEW,    "New Project...");
                menu.Append(ID_PROJECT_ATTACH, "Load / Attach Project to Current Chat...");
            } else {
                menu.Append(ID_PROJECT_NEW,    "New Project...");
                menu.Append(ID_PROJECT_ATTACH, "Load / Attach Project to Current Chat...");
                menu.AppendSeparator();
                appendSkillActions();
            }
            menu.AppendSeparator();
            menu.Append(ID_PROJECT_DELETE, "Delete Project...");
        } else {
            if (skillFirst) {
                appendSkillActions();
                menu.AppendSeparator();
            }

            menu.Append(ID_PROJECT_OPEN_FOLDER,        "Open Active Project Folder");
            menu.Append(ID_PROJECT_OPEN_INSTRUCTIONS,  "Open PROJECT.md");
            menu.AppendSeparator();
            menu.Append(ID_PROJECT_ADD_SOURCES,        "Add Files to Project Sources...");
            menu.Append(ID_PROJECT_OPEN_SOURCES_FOLDER,"Open Project Sources Folder");
            menu.AppendSeparator();
            menu.Append(ID_PROJECT_NEW_WORKFLOW,             "New Project Workflow...");
            menu.Append(ID_PROJECT_NEW_WORKFLOW_WITH_SCRIPT, "New Project Workflow with Python Script...");
            menu.Append(ID_PROJECT_OPEN_WORKFLOW,            "Open Project Workflow...");
            menu.Append(ID_PROJECT_OPEN_WORKFLOWS_FOLDER,    "Open Project Workflows Folder");

            if (!skillFirst) {
                menu.AppendSeparator();
                appendSkillActions();
            }

            menu.AppendSeparator();
            menu.Append(ID_PROJECT_ATTACH, "Switch Project...");
            menu.Append(ID_PROJECT_CLEAR,  "Clear Project from Current Chat");
            menu.Append(ID_PROJECT_DELETE, "Delete Project...");
        }

        // PopupMenu off the frame so wxEVT_MENU lands on the existing
        // Bind() entries set up in the constructor.  Position relative
        // to the anchor's bottom-left so the menu appears just below
        // the [⋯]/[+ attach] affordance on the strip.
        wxPoint pos(0, anchor ? anchor->GetSize().GetHeight() : 0);
        if (anchor) {
            pos = anchor->ClientToScreen(pos);
            pos = ScreenToClient(pos);
        }
        PopupMenu(&menu, pos);
    }


    // ═════════════════════════════════════════════════════════════
    //  Project-scoped helpers (callable for any project, not just
    //  the current chat's).  The existing chat-scoped OnProject*
    //  handlers below are thin wrappers that resolve the current
    //  chat's project and forward into these helpers.
    // ═════════════════════════════════════════════════════════════

    // Open an arbitrary filesystem path in the OS's default handler.
    // |friendlyName| is used in error message text only ("Could not
    // open the project Sources folder.").  Returns true on success.
    bool LaunchPathInOS(const std::string& path, const std::string& friendlyName)
    {
#ifdef __WXMSW__
        wxString cmd = "explorer.exe \"" + wxString::FromUTF8(path) + "\"";
        if (wxExecute(cmd, wxEXEC_ASYNC) != 0) return true;
#else
        if (wxLaunchDefaultApplication(wxString::FromUTF8(path))) return true;
#endif
        wxString msg = "Could not open the " + wxString::FromUTF8(friendlyName) + ".";
        wxMessageBox(msg, "Projects", wxOK | wxICON_ERROR, this);
        return false;
    }

    void OpenProjectFolderByRoot(const std::string& root)
    {
        if (root.empty()) return;
        if (!wxDirExists(wxString::FromUTF8(root))) {
            wxString msg = "The project folder no longer exists:\n\n";
            msg += wxString::FromUTF8(root);
            wxMessageBox(msg, "Project Folder Missing",
                         wxOK | wxICON_WARNING, this);
            return;
        }
        LaunchPathInOS(root, "project folder");
    }

    void OpenProjectInstructionsByRoot(const std::string& root)
    {
        if (root.empty()) return;
        const std::string path = ProjectManager::ProjectInstructionsPath(root);
        if (!wxFileExists(wxString::FromUTF8(path))) {
            wxString msg = "PROJECT.md was not found:\n\n";
            msg += wxString::FromUTF8(path);
            wxMessageBox(msg, "Projects", wxOK | wxICON_WARNING, this);
            return;
        }
        if (!wxLaunchDefaultApplication(wxString::FromUTF8(path))) {
            wxMessageBox("Could not open PROJECT.md.",
                         "Projects", wxOK | wxICON_ERROR, this);
        }
    }

    void OpenProjectSourcesFolderByRoot(const std::string& root)
    {
        if (root.empty()) return;
        const std::string sources = ProjectManager::ProjectSourcesPath(root);
        bool ok = wxFileName::Mkdir(wxString::FromUTF8(sources),
                                    wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
        if (!ok && !wxDirExists(wxString::FromUTF8(sources))) {
            wxString msg = "Could not open or create the project Sources folder:\n\n";
            msg += wxString::FromUTF8(sources);
            wxMessageBox(msg, "Projects", wxOK | wxICON_ERROR, this);
            return;
        }
        LaunchPathInOS(sources, "project Sources folder");
    }

    void OpenProjectWorkflowsFolderByRoot(const std::string& root)
    {
        if (root.empty()) return;
        const std::string workflows = ProjectManager::ProjectWorkflowsPath(root);
        bool ok = wxFileName::Mkdir(wxString::FromUTF8(workflows),
                                    wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
        if (!ok && !wxDirExists(wxString::FromUTF8(workflows))) {
            wxString msg = "Could not open or create the project Workflows folder:\n\n";
            msg += wxString::FromUTF8(workflows);
            wxMessageBox(msg, "Projects", wxOK | wxICON_ERROR, this);
            return;
        }
        LaunchPathInOS(workflows, "project Workflows folder");
    }

    // Delete a project as a discrete action — same warning + side-effect
    // dance as the dialog's deleteBtn handler, factored out so the
    // sidebar header right-click menu can target a specific project
    // without forcing the user through the picker dialog first.
    void DeleteProjectByInfo(const ProjectInfo& project)
    {
        wxString warning;
        warning << "Delete this project and ALL files inside it?\n\n";
        warning << wxString::FromUTF8(project.name) << "\n";
        warning << wxString::FromUTF8(project.rootPath) << "\n\n";
        warning << "This will permanently delete Sources, Templates, Outputs, Workflows, PROJECT.md, and project.json.\n";
        warning << "This cannot be undone.";

        int answer = wxMessageBox(warning, "Delete Project",
                                  wxYES_NO | wxNO_DEFAULT | wxICON_WARNING,
                                  this);
        if (answer != wxYES) return;

        const bool deletingActive =
            m_chatHistory->HasProject() &&
            (m_chatHistory->GetProjectId() == project.id ||
             m_chatHistory->GetProjectRoot() == project.rootPath);

        std::string error;
        if (!ProjectManager::DeleteProject(project, error)) {
            wxString msg = "Could not delete project.";
            if (!error.empty()) {
                msg += "\n\n";
                msg += wxString::FromUTF8(error);
            }
            wxMessageBox(msg, "Delete Project", wxOK | wxICON_ERROR, this);
            return;
        }

        if (deletingActive) {
            m_chatHistory->ClearProject();
            m_convController->UpdateWindowTitle();
            m_convController->AutoSaveConversation();
            RefreshProjectStrip();
        }

        m_chatDisplay->DisplaySystemMessage(
            "Deleted project: " + project.name + "\n" + project.rootPath);

        // Sidebar will re-bucket any remaining chats that referenced
        // this project under Unassigned on its next refresh.
        if (m_sidebar && m_sidebar->IsVisible()) {
            m_sidebar->Refresh(m_chatHistory->GetFilePath());
        }
    }

    // ═════════════════════════════════════════════════════════════
    //  Move chats to a project (or to Unassigned)
    // ═════════════════════════════════════════════════════════════
    //
    // Reassigns one or more chats from the sidebar's Move-to-project
    // submenu.  |targetProjectId| empty ⇒ clear project (Unassigned).
    //
    // For each path: if it's the currently loaded chat we mutate
    // m_chatHistory in-memory and AutoSave; otherwise we open a
    // throwaway ChatHistory, mutate, and SaveToFile.  Sidebar refresh
    // is deferred until all paths are processed to avoid mid-loop
    // visual flicker.
    void MoveChatsToProject(const std::vector<std::string>& paths,
                            const std::string& targetProjectId)
    {
        if (paths.empty()) return;
        if (IsBusy()) {
            wxMessageBox(
                "Stop the current response before reassigning chats.",
                "Move to Project", wxOK | wxICON_INFORMATION, this);
            return;
        }

        // Resolve the destination project (if any) up front so we
        // don't re-list on every iteration.  Empty target = clear.
        const bool toUnassigned = targetProjectId.empty();
        ProjectInfo targetProject;
        if (!toUnassigned) {
            if (!ProjectManager::LoadProjectById(targetProjectId, targetProject)) {
                wxMessageBox(
                    "The destination project no longer exists.",
                    "Move to Project", wxOK | wxICON_WARNING, this);
                return;
            }
        }

        const std::string activePath =
            m_chatHistory ? m_chatHistory->GetFilePath() : std::string();

        size_t moved = 0;
        size_t skipped = 0;

        for (const auto& path : paths) {
            if (path.empty()) continue;

            const bool isActive = (!activePath.empty() && path == activePath);

            if (isActive) {
                // No-op when the current chat already has the target
                // project — saves a redundant rewrite of the JSON.
                const bool sameAlready = toUnassigned
                    ? !m_chatHistory->HasProject()
                    : (m_chatHistory->HasProject() &&
                       m_chatHistory->GetProjectId() == targetProject.id);
                if (sameAlready) { ++skipped; continue; }

                if (toUnassigned) {
                    m_chatHistory->ClearProject();
                }
                else {
                    m_chatHistory->SetProject(targetProject.id,
                                              targetProject.name,
                                              targetProject.rootPath);
                }
                m_convController->UpdateWindowTitle();
                m_convController->AutoSaveConversation(/*refreshSidebar=*/false);
                ++moved;
            }
            else {
                // Throwaway ChatHistory — load, mutate, save.  Models
                // are round-tripped through LoadFromFile/SaveToFile so
                // we don't accidentally rewrite the file with no model
                // recorded.
                ChatHistory tmp;
                std::vector<std::string> models;
                if (!tmp.LoadFromFile(path, models)) {
                    ++skipped;
                    continue;
                }

                const bool sameAlready = toUnassigned
                    ? !tmp.HasProject()
                    : (tmp.HasProject() &&
                       tmp.GetProjectId() == targetProject.id);
                if (sameAlready) { ++skipped; continue; }

                if (toUnassigned) {
                    tmp.ClearProject();
                }
                else {
                    tmp.SetProject(targetProject.id,
                                   targetProject.name,
                                   targetProject.rootPath);
                }
                if (!tmp.SaveToFile(path, models)) {
                    ++skipped;
                    continue;
                }
                ++moved;
            }
        }

        // Single sidebar refresh at the end picks up every reassignment.
        if (m_sidebar && m_sidebar->IsVisible()) {
            m_sidebar->Refresh(m_chatHistory->GetFilePath());
        }
        RefreshProjectStrip();

        // Brief system message so the user gets visible confirmation.
        // Only emitted when there's something interesting to report.
        if (moved > 0) {
            std::string destLabel = toUnassigned
                ? std::string("Unassigned")
                : targetProject.name;
            std::string msg = "Moved " + std::to_string(moved) +
                              (moved == 1 ? " chat" : " chats") +
                              " to: " + destLabel;
            if (skipped > 0) {
                msg += "\n(" + std::to_string(skipped) +
                       (skipped == 1 ? " chat" : " chats") +
                       " skipped — already at destination or unreadable)";
            }
            m_chatDisplay->DisplaySystemMessage(msg);
        }
    }

    // ═════════════════════════════════════════════════════════════
    //  Sidebar context menus (chat row + project header)
    // ═════════════════════════════════════════════════════════════

    // Right-click on one or more chat rows.  Builds:
    //   Move to project ▸
    //     (No project)
    //     ──
    //     <Project A>
    //     <Project B>
    //   ────
    //   Delete conversation(s)
    void ShowSidebarChatContextMenu(const std::vector<std::string>& paths,
                                    wxWindow* anchor)
    {
        if (paths.empty()) return;

        wxMenu menu;
        const bool busy = IsBusy();

        // ── Move to project ▸ submenu ─────────────────────────────
        wxMenu* moveSub = new wxMenu;
        auto projects = ProjectManager::ListProjects();

        // "(No project)" first — the unassign action.
        const int unassignedItemId = wxNewId();
        moveSub->Append(unassignedItemId, "(No project)");
        moveSub->AppendSeparator();

        // Real projects, alphabetical (case-insensitive) to match the
        // sidebar's group ordering.
        std::sort(projects.begin(), projects.end(),
            [](const ProjectInfo& a, const ProjectInfo& b) {
                std::string an = a.name, bn = b.name;
                std::transform(an.begin(), an.end(), an.begin(), ::tolower);
                std::transform(bn.begin(), bn.end(), bn.begin(), ::tolower);
                return an < bn;
            });

        // Map menu IDs back to project IDs at click time.  Lambdas
        // capturing by value would also work; the id-keyed map keeps
        // the binding loop concise and avoids one bind per project.
        std::unordered_map<int, std::string> idToProject;
        for (const auto& p : projects) {
            const int itemId = wxNewId();
            idToProject[itemId] = p.id;
            moveSub->Append(itemId, wxString::FromUTF8(p.name));
        }

        if (projects.empty()) {
            // Avoid an empty submenu — at least the "(No project)"
            // item is there, but make it obvious there's nothing else.
            wxMenuItem* hint = moveSub->Append(wxID_ANY,
                "(no projects yet — create one from the project strip)");
            hint->Enable(false);
        }

        const wxString moveLabel = (paths.size() == 1)
            ? wxString("Move to project")
            : wxString::Format("Move %zu chats to project", paths.size());
        wxMenuItem* moveItem = menu.AppendSubMenu(moveSub, moveLabel);
        if (busy) moveItem->Enable(false);

        menu.AppendSeparator();

        // ── Delete ───────────────────────────────────────────────
        wxMenuItem* deleteItem = nullptr;
        if (paths.size() <= 1) {
            deleteItem = menu.Append(wxID_DELETE, "Delete conversation");
        }
        else {
            deleteItem = menu.Append(wxID_DELETE,
                wxString::Format("Delete %zu conversations", paths.size()));
        }
        if (busy && deleteItem) deleteItem->Enable(false);

        // ── Bind handlers ─────────────────────────────────────────
        // Snapshot |paths| so the lambdas don't depend on selection
        // state surviving until the user clicks.
        const std::vector<std::string> snapshot = paths;

        menu.Bind(wxEVT_MENU,
            [this, snapshot](wxCommandEvent&) {
                MoveChatsToProject(snapshot, std::string());
            }, unassignedItemId);

        for (const auto& [itemId, projId] : idToProject) {
            const std::string capturedId = projId;
            const int capturedItemId = itemId;
            menu.Bind(wxEVT_MENU,
                [this, snapshot, capturedId](wxCommandEvent&) {
                    MoveChatsToProject(snapshot, capturedId);
                }, capturedItemId);
        }

        menu.Bind(wxEVT_MENU,
            [this, snapshot](wxCommandEvent&) {
                m_convController->DeleteConversations(snapshot);
            }, wxID_DELETE);

        if (anchor) {
            anchor->PopupMenu(&menu);
        }
        else {
            PopupMenu(&menu);
        }
    }

    // Right-click on a project header in the sidebar.  Builds a small
    // popup scoped to that project; chat-mutating items only appear
    // when there's a current chat to attach.  Unassigned headers
    // (empty groupId) get no menu — there's nothing project-specific
    // to act on.
    void ShowSidebarProjectHeaderContextMenu(const std::string& groupId,
                                             wxWindow* anchor)
    {
        if (groupId.empty()) return;  // Unassigned header — no menu

        ProjectInfo project;
        if (!ProjectManager::LoadProjectById(groupId, project)) {
            // Project metadata gone.  Refresh sidebar so this stale
            // header gets removed on its own.
            if (m_sidebar && m_sidebar->IsVisible()) {
                m_sidebar->Refresh(m_chatHistory->GetFilePath());
            }
            return;
        }

        wxMenu menu;
        const bool busy = IsBusy();

        // ── Attach this chat ──────────────────────────────────────
        // Only meaningful when the current chat isn't already in this
        // project.  Skipped silently when it is, so the menu doesn't
        // include a no-op item.
        const bool currentChatAlreadyHere =
            m_chatHistory->HasProject() &&
            m_chatHistory->GetProjectId() == project.id;

        if (!currentChatAlreadyHere) {
            const int attachId = wxNewId();
            wxMenuItem* attachItem = menu.Append(attachId,
                wxString::Format("Attach this chat to %s",
                                 wxString::FromUTF8(project.name)));
            if (busy) attachItem->Enable(false);
            menu.Bind(wxEVT_MENU,
                [this, project](wxCommandEvent&) {
                    AttachProjectToCurrentChat(project);
                }, attachId);
            menu.AppendSeparator();
        }

        // ── Open actions ──────────────────────────────────────────
        const int openFolderId    = wxNewId();
        const int openMdId        = wxNewId();
        const int openSourcesId   = wxNewId();
        const int openWorkflowsId = wxNewId();

        menu.Append(openFolderId,    "Open Project Folder");
        menu.Append(openMdId,        "Open PROJECT.md");
        menu.Append(openSourcesId,   "Open Project Sources Folder");
        menu.Append(openWorkflowsId, "Open Project Workflows Folder");

        const std::string root = project.rootPath;
        menu.Bind(wxEVT_MENU,
            [this, root](wxCommandEvent&) { OpenProjectFolderByRoot(root); },
            openFolderId);
        menu.Bind(wxEVT_MENU,
            [this, root](wxCommandEvent&) { OpenProjectInstructionsByRoot(root); },
            openMdId);
        menu.Bind(wxEVT_MENU,
            [this, root](wxCommandEvent&) { OpenProjectSourcesFolderByRoot(root); },
            openSourcesId);
        menu.Bind(wxEVT_MENU,
            [this, root](wxCommandEvent&) { OpenProjectWorkflowsFolderByRoot(root); },
            openWorkflowsId);

        // ── Delete project ────────────────────────────────────────
        menu.AppendSeparator();
        const int deleteId = wxNewId();
        wxMenuItem* deleteItem = menu.Append(deleteId, "Delete Project...");
        if (busy) deleteItem->Enable(false);
        menu.Bind(wxEVT_MENU,
            [this, project](wxCommandEvent&) {
                DeleteProjectByInfo(project);
            }, deleteId);

        if (anchor) {
            anchor->PopupMenu(&menu);
        }
        else {
            PopupMenu(&menu);
        }
    }


    // ═════════════════════════════════════════════════════════════
    //  Projects Phase 1-5 menu handlers
    // ═════════════════════════════════════════════════════════════

    void AttachProjectToCurrentChat(const ProjectInfo& project)
    {
        const bool alreadyAttached =
            m_chatHistory->HasProject() &&
            m_chatHistory->GetProjectId() == project.id &&
            m_chatHistory->GetProjectRoot() == project.rootPath;

        if (alreadyAttached) {
            m_chatDisplay->DisplaySystemMessage(
                "This chat is already attached to project: " + project.name +
                "\n" + project.rootPath);
            m_convController->UpdateWindowTitle();
            return;
        }

        m_chatHistory->SetProject(project.id, project.name, project.rootPath);
        InvalidateProjectContextCache();

        std::string msg = "Attached this chat to project: " + project.name +
                          "\n" + project.rootPath;
        m_chatDisplay->DisplaySystemMessage(msg);
        m_convController->UpdateWindowTitle();

        // Save immediately even on a brand-new empty chat so the project
        // association survives reloads before the user sends a message.
        m_convController->AutoSaveConversation();
    }

    bool PromptCreateProject(ProjectInfo& outProject, wxWindow* parentWindow = nullptr)
    {
        wxWindow* parent = parentWindow ? parentWindow : this;

        LbThemedTextEntryDialog dlg(
            parent,
            m_appState->GetTheme(),
            "New LlamaBoss Project",
            "Project name:",
            "Create");
        if (dlg.ShowModal() != wxID_OK) return false;

        const std::string name = std::string(dlg.GetValue().ToUTF8().data());
        ProjectInfo project;
        std::string error;
        if (!ProjectManager::CreateProject(name, project, error)) {
            std::string errorMsg = error.empty()
                ? std::string("Could not create project.")
                : error;
            wxMessageBox(wxString::FromUTF8(errorMsg.c_str()),
                         "Project Error",
                         wxOK | wxICON_ERROR,
                         parent);
            return false;
        }

        outProject = project;
        return true;
    }

    void OnProjectNew(wxCommandEvent&)
    {
        if (IsBusy()) return;

        ProjectInfo project;
        if (!PromptCreateProject(project)) return;

        AttachProjectToCurrentChat(project);
    }

    void OnProjectAttach(wxCommandEvent&)
    {
        if (IsBusy()) return;

        auto projects = ProjectManager::ListProjects();

        ProjectAttachDialog dlg(
            this,
            m_appState->GetTheme(),
            std::move(projects),
            [this](wxWindow* parent, ProjectInfo& project) {
                return PromptCreateProject(project, parent);
            },
            [this](const ProjectInfo& project) {
                DeleteProjectByInfo(project);
            });

#ifdef __WXMSW__
        HWND modalScrim = ShowModalScrim();
#else
        wxFrame* modalScrim = ShowModalScrim();
#endif
        const int dialogResult = dlg.ShowModal();
        HideModalScrim(modalScrim);

        if (dialogResult != wxID_OK) return;

        ProjectInfo selectedProject;
        if (!dlg.GetSelectedProject(selectedProject)) return;
        AttachProjectToCurrentChat(selectedProject);
    }

    void OnProjectDelete(wxCommandEvent&)
    {
        // Reuse the attach/switch dialog because it now includes a safe
        // Delete Project... action next to the project list.
        wxCommandEvent dummy;
        OnProjectAttach(dummy);
    }

    void OnProjectOpenFolder(wxCommandEvent&)
    {
        if (!m_chatHistory->HasProject()) {
            wxMessageBox(
                "No project is attached to the current chat.",
                "Projects",
                wxOK | wxICON_INFORMATION,
                this);
            return;
        }
        OpenProjectFolderByRoot(m_chatHistory->GetProjectRoot());
    }

    void OnProjectOpenInstructions(wxCommandEvent&)
    {
        if (!m_chatHistory->HasProject()) {
            m_chatDisplay->DisplaySystemMessage("No project is attached to the current chat.");
            wxMessageBox(
                "No project is attached to the current chat.",
                "Projects", wxOK | wxICON_INFORMATION, this);
            return;
        }
        OpenProjectInstructionsByRoot(m_chatHistory->GetProjectRoot());
    }

    void OnProjectAddSources(wxCommandEvent&)
    {
        if (IsBusy()) return;

        if (!m_chatHistory->HasProject()) {
            m_chatDisplay->DisplaySystemMessage(
                "No project is attached to the current chat.");
            wxMessageBox(
                "No project is attached to the current chat.",
                "Projects", wxOK | wxICON_INFORMATION, this);
            return;
        }

        wxFileDialog dlg(
            this,
            "Add files to project Sources",
            wxEmptyString,
            wxEmptyString,
            "All files (*.*)|*.*",
            wxFD_OPEN | wxFD_FILE_MUST_EXIST | wxFD_MULTIPLE);
        if (dlg.ShowModal() != wxID_OK) return;

        wxArrayString selected;
        dlg.GetPaths(selected);
        if (selected.IsEmpty()) return;

        std::vector<std::string> sourcePaths;
        sourcePaths.reserve(selected.GetCount());
        for (const auto& path : selected) {
            sourcePaths.push_back(std::string(path.ToUTF8().data()));
        }

        std::vector<ProjectSourceInfo> copied;
        std::vector<std::string> skipped;
        std::string error;
        const bool ok = ProjectManager::CopyFilesToProjectSources(
            m_chatHistory->GetProjectRoot(), sourcePaths, copied, skipped, error);

        if (!ok) {
            std::string msg = error.empty() ? "Could not add files to project Sources." : error;
            if (!skipped.empty()) {
                msg += "\n\nSkipped:";
                for (const auto& path : skipped) msg += "\n- " + path;
            }
            wxMessageBox(wxString::FromUTF8(msg.c_str()),
                         "Projects", wxOK | wxICON_ERROR, this);
            return;
        }

        std::ostringstream body;
        body << "Added " << copied.size() << " file" << (copied.size() == 1 ? "" : "s")
             << " to project Sources for: " << m_chatHistory->GetProjectName() << "\n"
             << ProjectManager::ProjectSourcesPath(m_chatHistory->GetProjectRoot());
        for (const auto& file : copied) {
            body << "\n- " << file.name << " (" << ProjectSource_HumanBytes(file.sizeBytes) << ")";
        }
        if (!skipped.empty()) {
            body << "\n\nSkipped:";
            for (const auto& path : skipped) body << "\n- " << path;
        }
        m_chatDisplay->DisplaySystemMessage(body.str());
        InvalidateProjectContextCache();
        RefreshProjectStrip();
    }

    void OnProjectOpenSourcesFolder(wxCommandEvent&)
    {
        if (!m_chatHistory->HasProject()) {
            m_chatDisplay->DisplaySystemMessage("No project is attached to the current chat.");
            wxMessageBox(
                "No project is attached to the current chat.",
                "Projects", wxOK | wxICON_INFORMATION, this);
            return;
        }
        OpenProjectSourcesFolderByRoot(m_chatHistory->GetProjectRoot());
    }


    void CreateProjectWorkflowFromMenu(bool withPythonScript)
    {
        if (IsBusy()) return;

        if (!m_chatHistory->HasProject()) {
            m_chatDisplay->DisplaySystemMessage("No project is attached to the current chat.");
            wxMessageBox(
                "No project is attached to the current chat.",
                "Projects", wxOK | wxICON_INFORMATION, this);
            return;
        }

        wxTextEntryDialog dlg(
            this,
            "Workflow name:",
            withPythonScript ? "New Project Workflow with Python Script" : "New Project Workflow");
        if (dlg.ShowModal() != wxID_OK) return;

        const std::string name = std::string(dlg.GetValue().ToUTF8().data());
        ProjectWorkflowInfo workflow;
        ProjectWorkflowScriptInfo script;
        std::string error;
        bool ok = false;
        if (withPythonScript) {
            ok = ProjectManager::CreateProjectWorkflowWithScript(
                m_chatHistory->GetProjectRoot(), name, workflow, script, error);
        } else {
            ok = ProjectManager::CreateProjectWorkflow(
                m_chatHistory->GetProjectRoot(), name, workflow, error);
        }

        if (!ok) {
            std::string msg = error.empty()
                ? std::string("Could not create project workflow.")
                : error;
            wxMessageBox(wxString::FromUTF8(msg.c_str()),
                         "Projects", wxOK | wxICON_ERROR, this);
            return;
        }

        std::ostringstream body;
        body << "Created project workflow for: " << m_chatHistory->GetProjectName() << "\n"
             << workflow.path;
        if (withPythonScript && !script.path.empty()) {
            body << "\n\nCreated optional Python helper script:\n"
                 << script.path;
        }
        body << "\n\nEdit the workflow file to define trigger phrases, required inputs, steps, and output expectations.";
        if (withPythonScript) {
            body << "\nEdit the Python helper script only for repeatable mechanical work this workflow needs.";
        }
        m_chatDisplay->DisplaySystemMessage(body.str());
        InvalidateProjectContextCache();

        // Open the workflow immediately so the user can edit the contract.
        wxLaunchDefaultApplication(wxString::FromUTF8(workflow.path));
        if (withPythonScript && !script.path.empty()) {
            wxLaunchDefaultApplication(wxString::FromUTF8(script.path));
        }
        RefreshProjectStrip();
    }

    void OnProjectNewWorkflow(wxCommandEvent&)
    {
        CreateProjectWorkflowFromMenu(false);
    }

    void OnProjectNewWorkflowWithScript(wxCommandEvent&)
    {
        CreateProjectWorkflowFromMenu(true);
    }

    void OnProjectOpenWorkflow(wxCommandEvent&)
    {
        if (!m_chatHistory->HasProject()) {
            m_chatDisplay->DisplaySystemMessage("No project is attached to the current chat.");
            wxMessageBox(
                "No project is attached to the current chat.",
                "Projects", wxOK | wxICON_INFORMATION, this);
            return;
        }

        auto workflows = ProjectManager::ListProjectWorkflows(m_chatHistory->GetProjectRoot(), 0);
        if (workflows.empty()) {
            wxMessageBox(
                "No project workflows found yet. Use Projects > New Project Workflow first.",
                "Projects", wxOK | wxICON_INFORMATION, this);
            return;
        }

        wxArrayString choices;
        for (const auto& wf : workflows) {
            choices.Add(wxString::FromUTF8(wf.name));
        }

        wxSingleChoiceDialog dlg(
            this,
            "Select a project workflow to open:",
            "Open Project Workflow",
            choices);
        if (dlg.ShowModal() != wxID_OK) return;

        int sel = dlg.GetSelection();
        if (sel < 0 || static_cast<size_t>(sel) >= workflows.size()) return;

        const std::string path = workflows[static_cast<size_t>(sel)].path;
        if (!wxLaunchDefaultApplication(wxString::FromUTF8(path))) {
            wxMessageBox("Could not open the selected project workflow.",
                         "Projects", wxOK | wxICON_ERROR, this);
        }
    }

    void OnProjectOpenWorkflowsFolder(wxCommandEvent&)
    {
        if (!m_chatHistory->HasProject()) {
            m_chatDisplay->DisplaySystemMessage("No project is attached to the current chat.");
            wxMessageBox(
                "No project is attached to the current chat.",
                "Projects", wxOK | wxICON_INFORMATION, this);
            return;
        }
        OpenProjectWorkflowsFolderByRoot(m_chatHistory->GetProjectRoot());
    }

    // ── Skill handlers ───────────────────────────────────────────
    // These do not require an attached project. They always operate
    // against %USERPROFILE%\LlamaBoss\Skills.

    void CreateSkillFromMenu(bool withPythonScript)
    {
        if (IsBusy()) return;

        wxTextEntryDialog dlg(
            this,
            "Skill name:",
            withPythonScript ? "New Skill with Python Script" : "New Skill");
        if (dlg.ShowModal() != wxID_OK) return;

        const std::string name = std::string(dlg.GetValue().ToUTF8().data());
        SkillInfo skill;
        SkillScriptInfo script;
        std::string error;
        bool ok = false;
        if (withPythonScript) {
            ok = ProjectManager::CreateSkillWithScript(name, skill, script, error);
        } else {
            ok = ProjectManager::CreateSkill(name, skill, error);
        }

        if (!ok) {
            std::string msg = error.empty()
                ? std::string("Could not create Skill.")
                : error;
            wxMessageBox(wxString::FromUTF8(msg.c_str()),
                         "Skills", wxOK | wxICON_ERROR, this);
            return;
        }

        m_pendingSkillAuthoring.active = true;
        m_pendingSkillAuthoring.requestedPythonScript = withPythonScript;
        m_pendingSkillAuthoring.skillName = name;
        m_pendingSkillAuthoring.skillPath = skill.path;
        // pythonHelperPath stays empty until draft handoff and is only
        // populated then if requestedPythonScript is true.
        m_pendingSkillAuthoring.pythonHelperPath.clear();
        m_pendingSkillAuthoring.userDescription.clear();
        m_pendingSkillAuthoring.conversationStartMessageIndex =
            m_chatHistory->GetMessageCount();

        std::ostringstream body;
        body << "Created Skill:\n"
             << skill.path;
        if (withPythonScript && !script.path.empty()) {
            body << "\n\nCreated optional Python helper script:\n"
                 << script.path;
        }
        body << "\n\nLet’s design this Skill together first. Tell me what you want it to do, "
             << "and I can ask questions, check notes when useful, and help choose the right implementation path. "
             << "When the design sounds right, say `draft this Skill` and I’ll write the Skill files.";
        m_chatDisplay->DisplaySystemMessage(body.str());
        InvalidateProjectContextCache();

        RefreshProjectStrip();
    }

    void OnSkillNew(wxCommandEvent&)
    {
        CreateSkillFromMenu(false);
    }

    void OnSkillNewWithScript(wxCommandEvent&)
    {
        CreateSkillFromMenu(true);
    }

    void OnSkillOpen(wxCommandEvent&)
    {
        auto skills = ProjectManager::ListSkills(0);
        if (skills.empty()) {
            wxMessageBox(
                "No Skills found yet. Use New Skill first.",
                "Skills", wxOK | wxICON_INFORMATION, this);
            return;
        }

        wxArrayString choices;
        for (const auto& skill : skills) {
            choices.Add(wxString::FromUTF8(LbSkillDisplayNameFromContractPath(skill)));
        }

        wxSingleChoiceDialog dlg(
            this,
            "Select a Skill to open:",
            "Open Skill",
            choices);
        if (dlg.ShowModal() != wxID_OK) return;

        int sel = dlg.GetSelection();
        if (sel < 0 || static_cast<size_t>(sel) >= skills.size()) return;

        const std::string path = skills[static_cast<size_t>(sel)].path;
        if (!wxLaunchDefaultApplication(wxString::FromUTF8(path))) {
            wxMessageBox("Could not open the selected Skill.",
                         "Skills", wxOK | wxICON_ERROR, this);
        }
    }

    void OnSkillOpenFolder(wxCommandEvent&)
    {
        // Make sure the directory exists before asking the OS to open it.
        ProjectManager::EnsureSkillsRoot();
        const std::string dir = ProjectManager::GetSkillsDir();
        LaunchPathInOS(dir, "LlamaBoss Skills folder");
    }

    void OnProjectClear(wxCommandEvent&)
    {
        if (IsBusy()) return;

        if (!m_chatHistory->HasProject()) {
            m_chatDisplay->DisplaySystemMessage(
                "No project is attached to the current chat.");
            return;
        }

        std::string name = m_chatHistory->GetProjectName();
        m_chatHistory->ClearProject();
        InvalidateProjectContextCache();
        m_chatDisplay->DisplaySystemMessage(
            "Cleared project from this chat: " + name);
        m_convController->UpdateWindowTitle();

        // If this chat has messages, persist the cleared association.
        // A brand-new empty chat without project metadata has nothing to save.
        m_convController->AutoSaveConversation();
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

        // Phase 6: Stop while an approval card is pending means
        // cancel the pending tool, not a nonexistent chat stream.
        if (m_chatState == ChatState::AwaitingApproval) {
            if (m_pendingSlashApproval.active) {
                DenyPendingSlashTool(
                    "Cancelled by user before approval. Tool was not executed.");
                return;
            }
            if (m_agentController->IsActive() &&
                m_agentController->IsAwaitingApproval()) {
                m_agentController->CancelPendingApproval();
                return;
            }
            SetApprovalState(false);
            return;
        }

        // Skill authoring Phase 2B uses a hidden builder turn with no
        // transcript placeholder, just like the Goal contract builder.
        if (m_skillDraftBuilderInFlight) {
            DiscardPendingAssistantDelta();
            ++m_generationId;
            m_chatClient->StopGeneration();
            m_skillDraftBuilderInFlight = false;
            SetStreamingState(false);
            m_chatDisplay->DisplaySystemMessage(
                "Skill drafting stopped by user. The design session is still active. Say `draft this Skill` again when you are ready.");
            return;
        }

        // Goals Phase 3 contract-builder and Phase 2 verifier requests are
        // hidden control turns with no transcript placeholder. Stop them
        // cleanly without running the normal assistant-stream teardown path.
        if (m_goalContractBuilderInFlight) {
            DiscardPendingAssistantDelta();
            ++m_generationId;
            m_chatClient->StopGeneration();
            m_goalContractBuilderInFlight = false;
            m_goalAutoStartAfterContractBuild = false;
            SetStreamingState(false);
            if (m_chatHistory->HasGoal()) {
                m_chatHistory->MarkGoalContractFailed(
                    "Goal contract drafting was stopped by user.");
                m_convController->AutoSaveConversation();
            }
            m_chatDisplay->DisplaySystemMessage(
                "Goal contract drafting stopped by user. "
                "The goal remains active with objective-only verification.");
            return;
        }

        if (m_goalVerifierInFlight) {
            DiscardPendingAssistantDelta();
            ++m_generationId;
            m_chatClient->StopGeneration();
            m_goalVerifierInFlight = false;
            m_goalVerifierManualOnly = false;
            SetStreamingState(false);
            m_chatDisplay->DisplaySystemMessage(
                "Goal verification stopped by user. The goal remains active.");
            return;
        }

        // ── Agent loop cancellation ─────────────────────────────
        // If a loop is active, arm the cancel flag first so the
        // NEXT event-driven transition (streaming complete, grep
        // complete) tears down cleanly.  We still fall through to
        // the per-state cancel below — the in-flight op needs to
        // be stopped too, not just the loop wrapper.
        if (m_agentController->IsActive()) {
            m_agentController->Cancel();

            // If the agent is waiting on an async tool worker, Cancel()
            // has already signaled that worker.  Do NOT fall through to
            // StopGeneration(), because there is no chat stream to stop;
            // the worker's completion event will reset the UI cleanly.
            if (m_agentController->IsAwaitingAsyncResult())
                return;

            // Otherwise fall through so the active chat stream gets its
            // normal StopGeneration() signal.
        }

        if (IsBusy()) {
            // /cmd runs get cancelled through the executor; the worker
            // posts wxEVT_CMD_COMPLETE with cancelled=true which drives
            // the UI reset in OnCmdComplete.
            if (m_chatState == ChatState::RunningCmd) {
                m_cmdExecutor->Cancel();
                return;
            }
            // /grep is the same pattern: worker polls the cancel flag,
            // finishes up with cancelled=true, and posts COMPLETE.
            if (m_chatState == ChatState::RunningGrep) {
                m_grepExecutor->Cancel();
                return;
            }
            if (m_chatState == ChatState::RunningPython) {
                m_pythonRunner->Cancel();
                return;
            }

            DiscardPendingAssistantDelta();
            ++m_generationId;
            m_chatClient->StopGeneration();
            m_chatDisplay->DisplayAssistantComplete();
            m_chatDisplay->DisplaySystemMessage("Generation stopped by user");
            if (m_chatHistory->HasAssistantPlaceholder())
                m_chatHistory->RemoveLastAssistantMessage();
            SetStreamingState(false);
            m_chatDisplay->ClearFilePersistenceContext();
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

        SettingsDialog dlg(this,
                           m_appState->GetModel(),
                           m_appState->GetThemeName(),
                           m_appState->GetCtxSize(),
                           m_appState->GetFontSize(),
                           m_appState->GetAgentDefaultOn(),
                           m_appState->GetTheme(),
                           m_appState->GetSecretsStore());

#ifdef __WXMSW__
        HWND modalScrim = ShowModalScrim();
#else
        wxFrame* modalScrim = ShowModalScrim();
#endif
        const int dialogResult = dlg.ShowModal();
        HideModalScrim(modalScrim);

        if (dialogResult != wxID_OK) return;

        const bool folderChanged       = dlg.WasModelsFolderChanged();
        const bool modelChanged        = dlg.WasModelChanged();
        const bool themeChanged        = dlg.WasThemeChanged();
        const bool ctxSizeChanged      = dlg.WasCtxSizeChanged();
        const bool fontSizeChanged     = dlg.WasFontSizeChanged();
        const bool agentDefaultChanged = dlg.WasAgentDefaultChanged();

        // ── Models folder changed — unload and wait ──────────────────
        // The previously-loaded model's path may no longer be in scope
        // (new folder may not contain it, or not at that path). Autosave
        // any conversation, stop the server, and clear state. Don't
        // auto-start — user reopens Settings and explicitly picks a
        // model from the now-active folder. Takes precedence over
        // modelChanged: any combo auto-select that happened during the
        // folder swap isn't a deliberate user pick.
        if (folderChanged) {
            if (!m_chatHistory->IsEmpty())
                m_convController->AutoSaveConversation();

            m_serverManager->StopServer();

            bool mc, ac;
            m_appState->UpdateSettings("", m_appState->GetApiUrl(), mc, ac);

            m_chatHistory->Clear();
            m_chatDisplay->Clear();
            m_attachments->Clear();
            m_modelSwitcher->m_serverReady = false;
            _statusDot->SetConnected(false);
            m_modelSwitcher->UpdateModelLabel();
            m_convController->UpdateWindowTitle();

            m_chatDisplay->DisplaySystemMessage(
                "Models folder changed. Open Settings to load a model.");
        }
        // ── Server restarts (model or context length change) ─────────
        // A model change implies a fresh slate — clear history and start
        // over. A ctx-only change preserves history but still needs a
        // server restart since -c is a launch argument.
        else if (modelChanged) {
            std::string newModel = dlg.GetSelectedModel();

            // Persist ctx size first so MakeServerConfig() below sees it.
            if (ctxSizeChanged)
                m_appState->SetCtxSize(dlg.GetSelectedCtxSize());

            if (!m_chatHistory->IsEmpty())
                m_convController->AutoSaveConversation();

            bool mc, ac;
            m_appState->UpdateSettings(newModel, m_appState->GetApiUrl(), mc, ac);

            m_modelSwitcher->m_serverReady = false;
            _statusDot->SetConnected(false);
            // Phase 3b: hide the chip until the new model passes
            // detection.  Without this, switching from a "native"
            // model briefly displays the old chip on the new model.
            if (_protocolChip) UpdateProtocolChip(ToolProtocol::Unknown);
            // Phase 3c-i: also reset the active-protocol cache so
            // the next request defaults back to XML until detection
            // confirms the new model.
            _activeProtocol = ToolProtocol::Unknown;
            // Clear the old conversation before showing the reload status.
            // Previously, the "Loading <model>..." message was written and
            // then immediately erased by m_chatDisplay->Clear().
            m_chatHistory->Clear();
            m_chatDisplay->Clear();
            m_attachments->Clear();
            m_modelSwitcher->UpdateModelLabel();
            m_convController->UpdateWindowTitle();

            m_chatDisplay->DisplaySystemMessage(
                "Loading " + ServerManager::ModelDisplayName(newModel) + "...");
            m_serverManager->StartServer(newModel, m_appState->MakeServerConfig());
        }
        else if (ctxSizeChanged) {
            // Restart server with the same model but new ctx size.
            // History is preserved — user can keep reading while it reloads.
            m_appState->SetCtxSize(dlg.GetSelectedCtxSize());

            if (!m_chatHistory->IsEmpty())
                m_convController->AutoSaveConversation();

            m_modelSwitcher->m_serverReady = false;
            _statusDot->SetConnected(false);
            m_chatDisplay->DisplaySystemMessage(
                "Reloading with " +
                std::to_string(m_appState->GetCtxSize() / 1024) +
                "k context...");
            m_serverManager->StartServer(m_appState->GetModel(),
                                         m_appState->MakeServerConfig());
        }

        // ── Font size change — apply to chat display + input ─────────
        // Doesn't need a server restart; the size change just updates the
        // wxRichTextCtrl's default font. Existing content is re-rendered
        // via ReplayConversation below.
        if (fontSizeChanged) {
            m_appState->SetFontSize(dlg.GetSelectedFontSize());
            wxFont codeFont = m_appState->CreateMonospaceFont(m_appState->GetFontSize());
            _chatDisplayCtrl->SetFont(codeFont);
            _userInputCtrl->SetFont(codeFont);
            m_chatDisplay->SetFont(codeFont);
        }

        // ── Theme change — recolor the whole UI ──────────────────────
        if (themeChanged) {
            m_appState->SetTheme(dlg.GetSelectedTheme());
            ApplyThemeToUI();
        }

        // ── Agent-mode default — pure setting, no side effects ───────
        // Takes effect at next New Chat / next app launch. Deliberately
        // doesn't flip the current chat's m_agentModeEnabled — the robot
        // button remains the only way to change the active chat's state.
        if (agentDefaultChanged) {
            m_appState->SetAgentDefaultOn(dlg.GetSelectedAgentDefault());
        }

        // ── Replay conversation for any visual change ────────────────
        // Font and theme both need the RichTextCtrl's stored attrs
        // regenerated for existing messages. Skip if model changed
        // (history already cleared), folder changed (ditto), or if
        // only ctx changed (no visual diff — history is still valid).
        if (!modelChanged && !folderChanged &&
            (themeChanged || fontSizeChanged) &&
            !m_chatHistory->IsEmpty()) {
            m_chatDisplay->Clear();
            m_convController->ReplayConversation();
        }

        // ── Announce visual-only changes (server restarts and folder
        //    changes have their own status messages already) ──────────
        if (!modelChanged && !ctxSizeChanged && !folderChanged) {
            if (themeChanged && fontSizeChanged) {
                m_chatDisplay->DisplaySystemMessage(
                    "Theme and font size updated.");
            } else if (themeChanged) {
                m_chatDisplay->DisplaySystemMessage(
                    "Theme changed to " + m_appState->GetThemeName() + ".");
            } else if (fontSizeChanged) {
                m_chatDisplay->DisplaySystemMessage(
                    "Font size set to " +
                    std::to_string(m_appState->GetFontSize()) + "pt.");
            }

            if (agentDefaultChanged) {
                m_chatDisplay->DisplaySystemMessage(
                    m_appState->GetAgentDefaultOn()
                        ? "New chats will start with agent mode enabled."
                        : "New chats will start with agent mode disabled.");
            }
        }
    }

    void OnAbout(wxCommandEvent&)
    {
        wxString msg;
        msg << "LlamaBoss Beta v" << LLAMABOSS_VERSION << "\n\n"
            << "Private local desktop AI assistant for Windows.\n"
            << "Built for local LLMs, files, projects, skills, and approved tools.\n"
            << "Powered by llama.cpp\n\n"
            << "Beta notice:\n"
            << "This is beta software. Features, workflows, file handling, and UI behavior may change before a stable release.\n\n"
            << "Created by Cesar Avelar\n"
            << "Website: llamaboss.com\n\n"
            << "Built with wxWidgets + Poco\n"
            << "License: MIT\n\n"
            << wxString::FromUTF8("Model: ") << wxString::FromUTF8(
                ServerManager::ModelDisplayName(m_appState->GetModel())) << "\n"
            << wxString::FromUTF8("Server: ") << wxString::FromUTF8(m_appState->GetApiUrl()) << "\n"
            << wxString::FromUTF8("Models: ") << wxString::FromUTF8(ServerManager::GetModelsDir());
        wxMessageBox(msg, "About LlamaBoss Beta", wxOK | wxICON_INFORMATION);
    }

    void OnNewChat(wxCommandEvent&)
    {
        if (IsBusy()) return;

        if (!m_chatHistory->IsEmpty())
            m_convController->AutoSaveConversation(false);

        m_chatHistory->Clear();
        RefreshGoalStatusStrip();
        m_chatDisplay->Clear();
        m_attachments->Clear();
        m_modelSwitcher->UpdateModelLabel();
        m_convController->UpdateWindowTitle();
        if (m_sidebar->IsVisible())
            m_sidebar->Refresh(m_chatHistory->GetFilePath());
        _userInputCtrl->SetFocus();

        // Re-seed agent mode from the persisted default. Each new chat
        // starts at the user's declared preference; the robot button
        // still provides per-chat override until the next New Chat.
        const bool desired = m_appState->GetAgentDefaultOn();
        if (m_agentModeEnabled != desired) {
            m_agentModeEnabled = desired;
            _agentToggleButton->SetForegroundColour(
                m_agentModeEnabled ? LbInteractiveAccentForTheme(m_appState->GetTheme())
                                   : m_appState->GetTheme().textMuted);
            _agentToggleButton->Refresh();
        }

        if (auto* logger = m_appState->GetLogger())
            logger->information("New chat started");
    }

    // ── Server lifecycle → delegate to ModelSwitcher ──────────────
    void OnServerReady(wxCommandEvent&)
    {
        if (m_isClosing) return;

        // Phase 3 bugfix #2: immediately clear any stale protocol from
        // the previously loaded server before detection for this server
        // completes. Without this, a user could send an agent request in
        // the small ready-to-probe-result window and build it with the old
        // model's protocol.
        _activeProtocol = ToolProtocol::Unknown;
        if (_protocolChip) UpdateProtocolChip(ToolProtocol::Unknown);

        // Capture the runtime --jinja state BEFORE NotifyServerReady()
        // clears the per-load retry flags. A server that succeeded only
        // after the no-jinja fallback must force XML protocol for this
        // session even if the model/template cache previously said native.
        const bool serverJinjaEnabled =
            m_serverManager->IsCurrentServerJinjaEnabled();

        // Tell ServerManager the start succeeded so it clears any
        // --jinja retry state.  Without this, a successful retry
        // would leave the no-jinja flag set across to the next
        // model load.  See ServerManager::NotifyServerReady().
        m_serverManager->NotifyServerReady();
        m_modelSwitcher->OnServerReady();

        // Phase 3b: kick off tool-protocol detection for the active
        // (model, mmproj) pair.  Cache hits resolve immediately
        // (no thread); fresh probes run /props + heuristic + smoke
        // test on a worker and post wxEVT_TOOL_PROTOCOL_DETECTED
        // back to OnToolProtocolDetected.  The chip stays hidden
        // until the result arrives.
        if (_protocolChip) {
            _protocolChip->Hide();
            _protocolChip->SetLabel("");
            if (auto* parent = _protocolChip->GetParent()) parent->Layout();
        }

        const std::string modelPath  = m_serverManager->GetLoadedModel();
        const std::string mmprojPath = m_serverManager->GetLoadedMmproj();
        const std::string baseUrl    = m_serverManager->GetBaseUrl();
        if (!modelPath.empty() && !baseUrl.empty()) {
            KickOffToolProtocolDetection(
                this, m_alive, baseUrl, modelPath, mmprojPath,
                serverJinjaEnabled);
        }
    }

    // Phase 3b: handle the worker's result event.  Updates the chip
    // to "native" or "xml" with theme-appropriate colors and shows
    // it.  Logs the decision so server.log/llamaboss.log stays
    // useful for debugging which models passed detection and why.
    void OnToolProtocolDetected(wxThreadEvent& event)
    {
        if (m_isClosing) return;
        if (!_protocolChip) return;

        ProtocolDetectionResult r = event.GetPayload<ProtocolDetectionResult>();

        // Stale-result guard: if the user switched models between
        // probe kickoff and result, the modelPath on the event
        // won't match the currently-loaded model.  Drop it.
        if (r.modelPath != m_serverManager->GetLoadedModel()) {
            return;
        }

        if (auto* logger = m_appState->GetLogger()) {
            std::string line = std::string("Tool protocol detected: ") +
                ToolProtocolName(r.protocol) +
                (r.cacheHit ? " (cache hit)" : "") +
                " - " + r.reason;
            logger->information(line);
        }

        // Phase 3c-i: cache the result for the request builder.  The
        // agent controller reads this via the getActiveProtocol
        // callback when building each request body.
        _activeProtocol = r.protocol;

        UpdateProtocolChip(r.protocol);
    }

    // Apply protocol to the chip widget.  Called from
    // OnToolProtocolDetected; also safe to call with Unknown to
    // hide the chip.
    void UpdateProtocolChip(ToolProtocol protocol)
    {
        if (!_protocolChip) return;

        // Casual-user UX: the tool protocol is useful debug/status
        // information, but it should not occupy the main top bar. Keep
        // the chip permanently hidden and expose the detected protocol in
        // the model-name tooltip instead.
        _protocolChip->SetLabel("");
        _protocolChip->SetMinSize(wxSize(0, -1));
        _protocolChip->Hide();

        wxString tooltip;
        const wxString modelName = _modelLabel ? _modelLabel->GetLabel() : wxString();

        const wxString displayName = modelName.empty() ? wxString("Model") : modelName;

        if (protocol == ToolProtocol::Native) {
            tooltip = displayName + wxString::FromUTF8("\nTool protocol: native");
        }
        else if (protocol == ToolProtocol::Xml) {
            tooltip = displayName + wxString::FromUTF8("\nTool protocol: XML fallback");
        }
        else {
            tooltip = displayName;
        }

        if (_modelLabel) _modelLabel->SetToolTip(tooltip);
        if (_modelPill)  _modelPill->SetToolTip(tooltip);
        if (_statusDot)  _statusDot->SetToolTip(tooltip);

        if (auto* parent = _protocolChip->GetParent()) {
            parent->Layout();
            if (auto* grand = parent->GetParent()) grand->Layout();
        }
    }

    void OnServerError(wxCommandEvent& event)
    {
        if (m_isClosing) return;
        std::string err = WxToUtf8(event.GetString());

        // Phase 3a: give ServerManager first crack at the error.  If
        // the failure is a --jinja-incompatible chat template, the
        // manager re-launches without --jinja and returns true —
        // suppress the user-visible error in that case and wait for
        // the next ready/error event from the retry attempt.
        if (m_serverManager->MaybeRetryWithoutJinja(err)) {
            return;
        }

        // Server failed permanently — also clear any chip from a
        // prior session so a stale "native" doesn't outlive its
        // model.
        if (_protocolChip) UpdateProtocolChip(ToolProtocol::Unknown);
        _activeProtocol = ToolProtocol::Unknown;
        m_modelSwitcher->OnServerError(err);
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
            case 'N':
                // OnNewChat checks IsBusy internally.
                { wxCommandEvent e; OnNewChat(e); } return;
            case 'S':
                // Saving a half-streamed response is confusing but
                // survivable. Saving while streaming would write a
                // message with an empty placeholder — skip instead.
                if (IsBusy()) return;
                m_convController->OnSaveConversation();
                return;
            case 'O':
                // Loading a different conversation while streaming
                // would auto-save the partial response, then clear
                // history out from under the worker thread. Skip.
                if (IsBusy()) return;
                m_convController->OnLoadConversation();
                return;
            }
        }

        // ── Shift+Enter — insert a literal newline in the input ──
        // The input control has wxTE_PROCESS_ENTER, which makes
        // Enter fire wxEVT_TEXT_ENTER (bound to OnSendMessage) and
        // suppresses the default newline insertion.  Without this
        // hook, Shift+Enter would either send the message or do
        // nothing (depends on platform).  We intercept here, before
        // the control sees the event, and insert a '\n' at the
        // caret ourselves.
        //
        // The focus guard avoids stealing Shift+Enter from any
        // other widget that might want it (none today, but cheap
        // future-proofing).  WriteText fires wxEVT_TEXT, which the
        // OnUserInputChanged auto-grow handler picks up so the box
        // resizes naturally on the new line.
        if (evt.GetKeyCode() == WXK_RETURN &&
            evt.ShiftDown() &&
            !evt.ControlDown() &&
            !evt.AltDown() &&
            wxWindow::FindFocus() == _userInputCtrl)
        {
            _userInputCtrl->WriteText("\n");
            return;   // consume — do NOT Skip()
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
        encoder.rdbuf()->setLineLength(0);  // unbroken output — skip strip pass
        encoder.write(reinterpret_cast<const char*>(rawData.data()), dataSize);
        encoder.close();

        std::string base64 = base64Stream.str();

        if (base64.empty()) return false;

        bool ok = m_attachments->AttachImageFromBase64(base64, "clipboard_image.png");
        if (ok) RestoreComposerFocusDeferred();
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

        bool TryHandlePendingApprovalInput(const std::string& userInput)
    {
        // Phase 6: approval is a special busy state.  The input is
        // enabled only for /approve or /deny; ordinary messages wait
        // until the pending tool is resolved.
        if (m_chatState == ChatState::AwaitingApproval) {
            if (userInput.empty()) return true;

            _userInputCtrl->Clear();
            { wxCommandEvent e(wxEVT_TEXT, _userInputCtrl->GetId());
              OnUserInputChanged(e); }

            std::string approvalCmd = tool_approval::Trim(userInput);

            // Normalize internal whitespace so "/approve  always",
            // "/approve\talways", and "/APPROVE ALWAYS" all collapse
            // to a single canonical form before matching.
            auto NormalizeApprovalCmd = [](const std::string& s) {
                std::string out;
                out.reserve(s.size());
                bool inWs = false;
                for (char c : s) {
                    char lc = (c >= 'A' && c <= 'Z')
                              ? char(c - 'A' + 'a') : c;
                    if (lc == ' ' || lc == '\t' || lc == '\r' || lc == '\n') {
                        if (!out.empty() && !inWs) out.push_back(' ');
                        inWs = true;
                    } else {
                        out.push_back(lc);
                        inWs = false;
                    }
                }
                while (!out.empty() && out.back() == ' ') out.pop_back();
                return out;
            };
            std::string normalized = NormalizeApprovalCmd(approvalCmd);

            if (normalized == "/approve once" ||
                normalized == "approve once" ||
                normalized == "allow once" ||
                normalized == "just once") {
                HandleApprovalCommand(true, /*rememberForChat=*/false);
            } else if (normalized == "/approve" ||
                       normalized == "approve" ||
                       normalized == "allow" ||
                       normalized == "run it" ||
                       normalized == "go ahead" ||
                       normalized == "/approve always" ||
                       normalized == "/approve all" ||
                       normalized == "/approve chat" ||
                       normalized == "/trust chat" ||
                       normalized == "approve always" ||
                       normalized == "approve all" ||
                       normalized == "approve chat" ||
                       normalized == "approve conversation" ||
                       normalized == "allow always" ||
                       normalized == "trust chat") {
                HandleApprovalCommand(true, /*rememberForChat=*/true);
            } else if (normalized == "/deny" ||
                       normalized == "deny" ||
                       normalized == "cancel" ||
                       normalized == "no") {
                HandleApprovalCommand(false);
            } else {
                m_chatDisplay->DisplayAssistantMessage(
                    ServerManager::ModelDisplayName(m_appState->GetModel()),
                    "Approval is still pending. Use the buttons above to respond.",
                    m_appState->GetTheme().chatAssistant);
                _userInputCtrl->SetFocus();
            }
            return true;
        }

        return false;
    }

    bool TryHandleSpecialInputRouting(const std::string& userInput,
                                      bool hasAttachments)
    {
        // ── Easter egg commands ───────────────────────────────────
        if (!hasAttachments && (userInput == "/yay!" || userInput == "/yay")) {
            _userInputCtrl->Clear();
            { wxCommandEvent e(wxEVT_TEXT, _userInputCtrl->GetId()); OnUserInputChanged(e); }
            m_chatDisplay->DisplaySystemMessage("* fireworks *");
            m_activeAnimation = std::make_unique<FireworksAnimation>();
            m_animTimer.Start(m_activeAnimation->GetIntervalMs());
            return true;
        }

        // ── /cd — per-conversation working directory ─────────────
        // Not a tool: mutates per-conversation state (the tool CWD)
        // rather than producing a tool result.  Stays out of
        // HandleSlashCommand and routes through HandleSlashCd.
        if (!hasAttachments && userInput.rfind("/cd", 0) == 0 &&
            (userInput.size() == 3 ||
             userInput[3] == ' ' || userInput[3] == '\t' ||
             userInput[3] == '\n' || userInput[3] == '\r')) {
            std::string rest = (userInput.size() > 3)
                ? userInput.substr(4) : std::string();

            _userInputCtrl->Clear();
            { wxCommandEvent e(wxEVT_TEXT, _userInputCtrl->GetId());
              OnUserInputChanged(e); }

            HandleSlashCd(rest);
            return true;
        }


        // ── /goal — Goals Phase 1 mission state ───────────────────
        // Not a tool: stores per-conversation goal state for prompt
        // injection.  Automatic continuation + verifier passes arrive in
        // later Goals phases; Phase 1 establishes the state/UX contract.
        if (!hasAttachments && userInput.rfind("/goal", 0) == 0 &&
            (userInput.size() == 5 ||
             userInput[5] == ' ' || userInput[5] == '\t' ||
             userInput[5] == '\n' || userInput[5] == '\r')) {
            std::string rest = (userInput.size() > 5)
                ? userInput.substr(6) : std::string();

            _userInputCtrl->Clear();
            { wxCommandEvent e(wxEVT_TEXT, _userInputCtrl->GetId());
              OnUserInputChanged(e); }

            HandleSlashGoal(rest);
            return true;
        }

        // ── Natural-language Goal controls (Goals Phase 16) ─────
        // Command-like, full-message phrases map onto the existing /goal
        // control flow.  This keeps slash commands available for power users
        // while the normal UX reads more naturally.
        if (!hasAttachments) {
            std::string naturalGoalCommand;
            if (LbTryParseNaturalLanguageGoalControl(userInput, naturalGoalCommand)) {
                _userInputCtrl->Clear();
                { wxCommandEvent e(wxEVT_TEXT, _userInputCtrl->GetId());
                  OnUserInputChanged(e); }

                m_chatDisplay->DisplaySystemMessage(
                    "Natural-language goal command recognized.");
                HandleSlashGoal(naturalGoalCommand);
                return true;
            }
        }

        // ── Natural-language Goal start (Goals Phase 15) ────────
        // Keep this explicit and conservative: only command-like phrases
        // that literally say "goal" are treated as Goal creation. Ordinary
        // conversational requests keep flowing through normal chat.
        if (!hasAttachments) {
            std::string naturalGoalObjective;
            if (LbTryParseNaturalLanguageGoalStart(userInput, naturalGoalObjective)) {
                _userInputCtrl->Clear();
                { wxCommandEvent e(wxEVT_TEXT, _userInputCtrl->GetId());
                  OnUserInputChanged(e); }

                if (naturalGoalObjective.empty()) {
                    m_chatDisplay->DisplaySystemMessage(
                        "To start a natural-language goal, include the objective after the phrase. "
                        "Example: Make this a goal: <objective>");
                } else {
                    m_chatDisplay->DisplaySystemMessage(
                        "Natural-language goal request recognized.");
                    HandleSlashGoal(naturalGoalObjective);
                }
                return true;
            }
        }

        // ── Tool-shaped slash commands (Phase 4 / 4.1) ────────────
        // /read, /ls, /grep, /pwd, /open, /cmd (Phase 4) and
        // /write, /mkdir, /edit, /delete (Phase 4.1) all route
        // through HandleSlashCommand, which builds a ToolInvocation
        // and dispatches through ToolRouter.  Same path the agent
        // takes, same validators, same dispatchers, same rendering.
        //
        // The intercept is a small table: prefix → tool name.  Each
        // entry requires whitespace or EOS after the verb so things
        // like "/lsfoo" fall through to chat unchanged.  The verb's
        // arg slice is left- and right-trimmed before dispatch (every
        // pre-Phase-4 HandleSlashX did this; for /cmd this is a
        // small change — leading and trailing whitespace inside the
        // command line is now stripped, which it wasn't before).
        if (!hasAttachments && !userInput.empty() && userInput[0] == '/') {
            // string_view's size() is captured at construction from the
            // literal, so prefix lengths can't drift out of sync with the
            // text -- which is the whole bug class the previous hand-
            // counted `size_t prefixLen` field invited.  C++17 constexpr
            // ctor + literal argument means each entry's size() is a
            // compile-time constant.
            struct SlashEntry {
                std::string_view prefix;
                const char*      toolName;
            };
            static const SlashEntry kSlashTable[] = {
                { "/read_head", tool_names::kReadHead   },
                { "/read",      tool_names::kRead       },
                { "/ls",        tool_names::kLs         },
                { "/grep",      tool_names::kGrep       },
                { "/pwd",       tool_names::kPwd        },
                { "/open",      tool_names::kOpen       },
                { "/cmd",       tool_names::kPowerShell },
                { "/python_health",         tool_names::kPythonHealth },
                { "/csv_inspect",           tool_names::kCsvInspect   },
                { "/csv_report",            tool_names::kCsvReport    },
                { "/csv_to_xlsx",           tool_names::kCsvToXlsx    },
                { "/xlsx_inspect",          tool_names::kXlsxInspect  },
                { "/xlsx_report",           tool_names::kXlsxReport   },
                { "/xlsx_create_workbook",  tool_names::kXlsxCreateWorkbook },
                { "/pdf_extract_text",      tool_names::kPdfExtractText },
                { "/pdf_inspect_form",      tool_names::kPdfInspectForm },
                { "/pdf_fill_form",         tool_names::kPdfFillForm    },
                { "/python_create_script",  tool_names::kPythonCreateScript },
                { "/python_run_script",     tool_names::kPythonRunScript    },
                { "/python_install_package", tool_names::kPythonInstallPackage },
                { "/web_fetch_url",         tool_names::kWebFetchUrl },
                { "/notes_read",            tool_names::kNotesRead },
                { "/notes_append",          tool_names::kNotesAppend },
                { "/project_notes_read",    tool_names::kProjectNotesRead },
                { "/project_notes_append",  tool_names::kProjectNotesAppend },
                // Phase 4.1: mutating tools.  All four enforce the
                // workspace sandbox internally and refuse risky
                // extensions (.exe, .bat, .ps1, .reg, .lnk, .vbs,
                // macro Office docs).  /write refuses to overwrite
                // — use /edit for that.  /edit's OLD block must
                // appear EXACTLY ONCE in the file.  /delete is
                // non-recursive: directories must be empty.
                //
                // Note re. multi-line args (/write, /edit): the
                // table trims leading and trailing whitespace from
                // args before dispatch, which strips any trailing
                // whitespace from the NEW block in /edit content.
                // The agent path preserves args verbatim and is
                // unaffected.  Worth knowing if hand-crafting an
                // edit whose replacement text is whitespace-
                // sensitive at its tail.
                { "/overwrite_file", tool_names::kOverwriteFile },
                { "/write",          tool_names::kWrite      },
                { "/mkdir",          tool_names::kMkdir      },
                { "/edit",           tool_names::kEdit       },
                { "/delete",         tool_names::kDelete     },
            };

            for (const SlashEntry& e : kSlashTable) {
                const size_t plen = e.prefix.size();

                // Prefix match.  std::string::compare(pos, count, char*, count)
                // is the C++17-everywhere overload; the string_view rfind
                // overload exists but has historically been flaky on some
                // toolchains.
                if (userInput.size() < plen) continue;
                if (userInput.compare(0, plen,
                                      e.prefix.data(), plen) != 0) continue;

                // Boundary check: whitespace or EOS after the verb,
                // so "/lsfoo" is NOT "/ls" + "foo".
                if (userInput.size() != plen) {
                    char c = userInput[plen];
                    if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
                        continue;
                }

                std::string args;
                if (userInput.size() > plen) {
                    args = userInput.substr(plen + 1);
                }
                // Trim args on both ends — every old HandleSlashX
                // did this for non-/cmd verbs, and now /cmd matches.
                {
                    size_t a = args.find_first_not_of(" \t\r\n");
                    size_t b = args.find_last_not_of(" \t\r\n");
                    if (a == std::string::npos) args.clear();
                    else                         args = args.substr(a, b - a + 1);
                }

                _userInputCtrl->Clear();
                { wxCommandEvent ev(wxEVT_TEXT, _userInputCtrl->GetId());
                  OnUserInputChanged(ev); }

                HandleSlashCommand(e.toolName, args);
                return true;
            }
        }

        // ── Conversational Skill design session (Skills authoring Phase 2I) ──
        // Ordinary messages must now reach the model so the user can design a
        // Skill through real back-and-forth discussion.  Only explicit Skill
        // authoring controls are intercepted here: cancel, or draft after the
        // conversation has clarified the intended Skill.
        if (!hasAttachments && m_pendingSkillAuthoring.active) {
            const std::string trimmedSkillSetup = LbTrimAscii(userInput);

            if (LbSkillAuthoringInputCancelsSetup(trimmedSkillSetup)) {
                _userInputCtrl->Clear();
                { wxCommandEvent e(wxEVT_TEXT, _userInputCtrl->GetId());
                  OnUserInputChanged(e); }

                m_pendingSkillAuthoring = PendingSkillAuthoring{};
                m_chatDisplay->DisplaySystemMessage(
                    "Skill design cancelled. The reserved starter SKILL.md file was left in place.");
                return true;
            }

            const bool directDraftRequest =
                LbSkillAuthoringInputRequestsDraft(trimmedSkillSetup);
            const bool confirmsDraftPrompt =
                LbSkillAuthoringInputConfirmsDraftPrompt(
                    trimmedSkillSetup,
                    m_chatHistory->GetLastAssistantMessage());

            if (directDraftRequest || confirmsDraftPrompt) {
                const std::string authoringBrief =
                    BuildPendingSkillDesignConversationBrief();
                if (authoringBrief.empty()) {
                    m_chatDisplay->DisplayUserMessage(userInput);

                    _userInputCtrl->Clear();
                    { wxCommandEvent e(wxEVT_TEXT, _userInputCtrl->GetId());
                      OnUserInputChanged(e); }

                    m_chatHistory->AddUserMessage(userInput);

                    const std::string reminder =
                        "I am ready to help design this Skill, but I do not have enough design context yet. "
                        "Tell me what you want it to do first. Once the design sounds right, say `draft this Skill`.";
                    m_chatDisplay->DisplayAssistantMessage(
                        ServerManager::ModelDisplayName(m_appState->GetModel()),
                        reminder,
                        m_appState->GetTheme().chatAssistant);
                    m_chatHistory->AddAssistantMessage(
                        reminder,
                        m_appState->GetModel());

                    if (!m_chatHistory->IsEmpty())
                        m_convController->AutoSaveConversation();
                    return true;
                }

                m_chatDisplay->DisplayUserMessage(userInput);

                _userInputCtrl->Clear();
                { wxCommandEvent e(wxEVT_TEXT, _userInputCtrl->GetId());
                  OnUserInputChanged(e); }

                m_chatHistory->AddUserMessage(userInput);
                m_pendingSkillAuthoring.userDescription = authoringBrief;
                // Only offer the builder a candidate Python helper path
                // when the user picked "New Skill with Python Script"
                // at creation. Otherwise the builder's "no Python
                // helper" branch fires and no .py is emitted.
                if (m_pendingSkillAuthoring.requestedPythonScript) {
                    m_pendingSkillAuthoring.pythonHelperPath =
                        LbSkillPythonHelperPathFromContractPath(
                            m_pendingSkillAuthoring.skillPath);
                } else {
                    m_pendingSkillAuthoring.pythonHelperPath.clear();
                }

                const std::string handoff =
                    "Great — I’ll draft this Skill from our design conversation and choose the most practical implementation path.";
                m_chatDisplay->DisplayAssistantMessage(
                    ServerManager::ModelDisplayName(m_appState->GetModel()),
                    handoff,
                    m_appState->GetTheme().chatAssistant);
                m_chatHistory->AddAssistantMessage(
                    handoff,
                    m_appState->GetModel());

                if (!m_chatHistory->IsEmpty())
                    m_convController->AutoSaveConversation();
                BeginSkillDraftBuildFromPendingDescription();
                return true;
            }
        }


        return false;
    }

    // Prepares a real chat turn after special routing falls through.
    // Returns false when the text was captured as an awaiting-goal reply
    // and no assistant request should start yet.
    bool PrepareAndRecordUserTurn(std::string userInput,
                                  bool hasAttachments)
    {
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
            std::string workflowDir = ChatHistory::GetWorkflowDir(m_chatHistory->GetFilePath());
            for (const auto& info : attachInfo) {
                if (info.kind == AttachmentInfo::Kind::Image && !info.storagePath.empty())
                    imagePaths.push_back(workflowDir + "/" + info.storagePath);
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
        if (m_attachments->HasPdfFile())
            userInput = m_attachments->BakePdfFilesIntoMessage(
                userInput, m_agentModeEnabled);
        if (m_attachments->HasSpreadsheetFile())
            userInput = m_attachments->BakeSpreadsheetFilesIntoMessage(
                userInput, m_agentModeEnabled);
        if (m_attachments->HasDocxFile())
            userInput = m_attachments->BakeDocxFilesIntoMessage(
                userInput, m_agentModeEnabled);

        m_chatHistory->AddUserMessage(userInput, "", attachInfo);

        // Goals Phase 12 polish: a plain user reply while a goal is waiting
        // for input should be recorded as the answer, not immediately routed
        // into a fresh tool-using agent turn.  The user explicitly resumes the
        // waiting goal afterward, preferably by saying "continue the goal".
        if (!hasAttachments && m_chatHistory->HasAwaitingUserGoal()) {
            m_chatHistory->RecordGoalAwaitingUserReply(
                LbClipForGoalVerifier(userInput, 1200));
            m_convController->AutoSaveConversation();
            m_chatDisplay->DisplaySystemMessage(
                "Response recorded for the waiting goal. Say 'continue the goal' to resume work.");
            m_attachments->Clear();
            return false;
        }

        return true;
    }

    void StartAssistantResponseForPreparedTurn()
    {
        std::string model = m_appState->GetModel();

        // Build the final request body only once.
        // Important: agent mode must add its system prompt BEFORE image injection.
        // If we inject images first and then rebuild the body for agent mode,
        // the rebuilt body loses the multimodal content array.
        std::string body;
        if (m_agentModeEnabled) {
            int ctxTokens = m_appState->GetCtxSize();
            if (ctxTokens <= 0) ctxTokens = 8192;

            // Phase 3c-i: attach the tool catalog when the loaded
            // model supports native function calling.  The agent
            // controller does the same on subsequent iterations
            // (see AgentController::BuildRequestBody); this is the
            // first turn before the controller takes over the loop.
            std::string tools;
            const bool native = (_activeProtocol == ToolProtocol::Native);
            if (native) {
                tools = GetCachedToolsArrayJson();
            }

            body = m_chatHistory->BuildChatRequestJson(
                model,
                true,
                BuildAgentSystemPrompt(),
                ctxTokens,
                tools,
                native);
        }
        else {
            int ctxTokens = m_appState->GetCtxSize();
            if (ctxTokens <= 0) ctxTokens = 8192;
            body = m_chatHistory->BuildChatRequestJson(
                model,
                true,
                BuildNormalSystemPrompt(),
                ctxTokens);
        }

        // Inject images after the final body shape is known.
        // This fixes agent mode dropping image attachments on the first request.
        if (m_attachments->HasImage())
            body = m_attachments->InjectImagesIntoRequest(body);

        // Safe to clear only after the final request body has image data baked in.
        m_attachments->Clear();

        if (auto* logger = m_appState->GetLogger())
            logger->debug("Request sent (" + std::to_string(body.size()) + " bytes)");

        m_chatHistory->AddAssistantPlaceholder(model);
        m_chatDisplay->DisplayAssistantPrefix(
            ServerManager::ModelDisplayName(model),
            m_appState->GetTheme().chatAssistant);

        // Persistence context for any file chips generated during this
        // response.  Ensures the conversation has a file path so the
        // sidecar dir is stable across app restarts.
        if (!m_chatHistory->HasFilePath())
            m_chatHistory->SetFilePath(ChatHistory::GenerateFilePath());
        {
            std::string genDir = ChatHistory::GetGeneratedFilesDir(
                m_chatHistory->GetFilePath());

            // msgIdx = index of the placeholder we just added (last message).
            size_t msgIdx = m_chatHistory->GetMessageCount() > 0
                ? m_chatHistory->GetMessageCount() - 1
                : 0;

            m_chatDisplay->SetFilePersistenceContext(genDir, msgIdx);
        }

        // Arm the agent loop only after the first request body has been built.
        // AgentController::Begin() prepares the controller to treat the upcoming
        // streamed assistant reply as iteration 1.
        if (m_agentModeEnabled) {
            ResetAgentToolStreamFilter();
            m_agentController->Begin();
        }

        DiscardPendingAssistantDelta();
        ++m_generationId;
        m_chatState = ChatState::Streaming;
        SetStreamingState(true);

        // Phase 3c-i: log the outbound body so the operator can
        // verify whether a `tools` array got attached for a
        // native-protocol model.  Two lines:
        //   * Request shape — a single grep-friendly summary
        //     ("tools=yes, messages=N, body=BYTES") that answers
        //     "is the wire shape correct?" without eyeballing JSON.
        //   * Outbound — the first ~2000 chars of the body for
        //     deeper inspection.  2000 covers the agent system
        //     prompt (~1k chars) plus the head of the tools array,
        //     which 500 was clipping.
        if (auto* logger = m_appState->GetLogger()) {
            // Cheap textual sniff — these substrings appear at the
            // top level of the JSON because Poco preserves insertion
            // order, but even if a future change moved them deeper
            // the substring search still answers correctly.  No
            // need to re-parse the body just to count.
            const bool hasTools = body.find("\"tools\":") != std::string::npos;
            size_t msgCount = 0;
            for (size_t pos = 0; ; ) {
                size_t hit = body.find("\"role\":", pos);
                if (hit == std::string::npos) break;
                ++msgCount;
                pos = hit + 7;
            }

            const char* protoLabel =
                (_activeProtocol == ToolProtocol::Native) ? "native protocol" :
                (_activeProtocol == ToolProtocol::Xml)    ? "xml protocol"    :
                                                            "protocol unknown";

            logger->information(
                std::string("Request shape (") + protoLabel + "): "
                + "tools="    + (hasTools ? "yes" : "no")
                + ", messages=" + std::to_string(msgCount)
                + ", body="     + std::to_string(body.size()) + " bytes");

            std::string preview = body.size() > 2000
                                      ? body.substr(0, 2000) + "...(truncated)"
                                      : body;
            logger->information(
                std::string("Outbound /v1/chat/completions (") + protoLabel
                + "): " + preview);
        }

        if (!m_chatClient->SendMessage(model, m_appState->GetApiUrl(),
            body, m_generationId)) {

            if (m_agentController->IsActive()) {
                ResetAgentToolStreamFilter();
                m_agentController->HandleAssistantError("Failed to start chat request");
            }

            SetStreamingState(false);
            m_chatDisplay->DisplaySystemMessage("Failed to start chat request");
            m_chatHistory->RemoveLastAssistantMessage();
        }
    }

    void OnSendMessage(wxCommandEvent&)
    {
        if (m_activeAnimation) return;  // animation playing

        std::string userInput = WxToUtf8(_userInputCtrl->GetValue());

        // Trim leading whitespace so slash-commands (/cmd, /yay) fire
        // regardless of stray leading spaces in the input box.  Do NOT
        // trim trailing whitespace — prompts may intentionally end with
        // newlines for paragraph spacing.
        {
            size_t firstNonWs = userInput.find_first_not_of(" \t\r\n");
            if (firstNonWs == std::string::npos) userInput.clear();
            else if (firstNonWs > 0)             userInput.erase(0, firstNonWs);
        }

        if (TryHandlePendingApprovalInput(userInput)) return;

        if (IsBusy()) return;

        if (!m_modelSwitcher->m_serverReady) {
            m_chatDisplay->DisplaySystemMessage(
                "Server is still loading the model. Please wait...");
            return;
        }

        bool hasAttachments = m_attachments->HasPending();
        if (userInput.empty() && !hasAttachments) return;

        if (TryHandleSpecialInputRouting(userInput, hasAttachments)) return;

        if (!PrepareAndRecordUserTurn(std::move(userInput), hasAttachments))
            return;

        StartAssistantResponseForPreparedTurn();

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
    // Single classifying loop — fixes two prior bugs:
    //
    //   1. Dropping multiple PDFs / spreadsheets / DOCX files only imported
    //      the first because each kind-specific loop returned early.
    //
    //   2. Mixed drops (e.g. one PDF plus two screenshots) silently lost the
    //      images for the same reason — the PDF branch returned before the
    //      image loop ran.
    //
    // Importing each file independently lets the same drop yield N artifact
    // chips of mixed kinds.  PDF / spreadsheet / DOCX / image / text route
    // through their existing per-kind queue helpers; unknown extensions are
    // ignored as before.
    bool anyAttached = false;
    for (const auto& file : filenames) {
        std::string path(file.ToUTF8().data());
        wxFileName fn(file);
        std::string ext(fn.GetExt().Lower().ToUTF8().data());

        if (ext == "pdf") {
            if (m_frame->QueuePdfAttachmentFromDrop(path))
                anyAttached = true;
        }
        else if (AttachmentManager::IsSpreadsheetFile(path)) {
            if (m_frame->QueueSpreadsheetAttachmentFromDrop(path))
                anyAttached = true;
        }
        else if (ext == "docx") {
            if (m_frame->QueueDocxAttachmentFromDrop(path))
                anyAttached = true;
        }
        else if (ext == "docm") {
            // Macro-enabled Word docs are intentionally not auto-routed
            // (VBA execution risk).  Previously this fell through silently,
            // which looked like a broken drop target.  Surface a short
            // explanation so the user knows what happened and what to do.
            // anyAttached stays false: nothing landed as an attachment.
            m_frame->NotifyDocmDropRejected(path);
        }
        else if (AttachmentManager::IsImageFile(path)) {
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
