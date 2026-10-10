#include "endpoints_dialog.h"
#include <cassert>
#include <wx/file.h>
#include <wx/datetime.h>

#include "lb_windows.h"

#include "lb_ssl.h"
#include "ui_event_post.h"
#include "app.h"
#include "model_service.h"
#include <iterator>
#include <stdexcept>

// Poco headers for base64 and JSON
#include <Poco/Base64Decoder.h>   // generated-image data URL decode

#include "settings.h"
#include "chat_client.h"
#include "context_hud.h"         // ctx meter click -> context details panel
#include "bench_controller.h"     // /bench
#include "prompt_prewarm.h"        // New Chat prompt-cache pre-warm
#ifdef __WXMSW__
#include <wx/msw/wrapwin.h>       // GetForegroundWindow (context panel visibility)
#endif
#include "chat_display.h"
#include "chat_history.h"
#include "reasoning_policy.h"
#include "app_state.h"
#include "conversation_sidebar.h"
#include "attachment_manager.h"
#include "model_manager.h"
#include "server_manager.h"
#include "cmd_executor.h"
#include "python_runner.h"
#include "python_session.h"
#include "tool_path.h"
#include "tool_grep.h"
#include "tool_web_fetch.h"
#include "wait_executor.h"
#include "tool_call_parser.h"  // ToolCallStreamDetector for hiding raw <tool_call> blocks
#include "agent_controller.h"
#include "tool_protocol.h"     // tool-call protocol detection
#include "tool_router.h"       // BuildToolsArrayJson for native requests
#include "tool_approval.h"     // approval cards
#include "project_manager.h"   // Projects
#include "project_attach_dialog.h"
#include "project_status_strip.h"
#include "activity_strip.h"     // live long-task progress strip above the composer
#include "lb_themed_dialogs.h"
#include "lb_input_parsers.h"
#include "lb_project_ui_actions.h"
#include "lb_modal_scrim.h"
#include "lb_about_dialog.h"
#include "update_installer.h"

// ── File-local support modules (extracted helpers) ───────────────
#include "lb_string_utils.h"
#include "skill_authoring_support.h"
#include "agent_prompt_builder.h"
#include "python_package_recovery.h"
#include "artifact_presentation.h"
#include "drop_import_controller.h"

// ── Extracted widget & coordinator headers ────────────────────────
#include "widgets.h"
#include "attachment_chip.h"   // composer attachment cards
#include "image_lightbox.h"    // full-size viewer for pending image cards
#include "chat_input_ctrl.h"
#include "chat_display_ctrl.h"
#include "ui_builder.h"
#include "lb_icons.h"
#include "lb_hover_tile.h"
#include "lb_scroll_rail.h"
#include "model_switcher.h"
#include "path_safety.h"      // SameModelPath for queued-send matching
#include "conversation_controller.h"
#include "project_context_builder.h"
#include "tool_result_controller.h"
#include "reminder_store.h"
#include "skill_draft_controller.h"
#include "project_controller.h"
#include "ascii_animation.h"
#include "var_store.h"

// ─── Application version ─────────────────────────────────────
static const char* LLAMABOSS_VERSION = "0.1.21";

// Native menu command ids. Keep above wxID_HIGHEST to avoid collisions
// with stock wxWidgets commands.
enum {
    ID_ANIMATION_TIMER = wxID_HIGHEST + 2000,
    ID_ASSISTANT_DELTA_FLUSH_TIMER,
    ID_REMINDER_TIMER,
    ID_PY_SESSION_REAP_TIMER,
    ID_PENDING_SEND_PROTOCOL_TIMER,

    ID_PROJECT_NEW = wxID_HIGHEST + 2100,
    ID_PROJECT_ATTACH,
    ID_PROJECT_OPEN_FOLDER,
    ID_PROJECTS_OPEN_ROOT_FOLDER,   // opens the Projects root (no-project menu)
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
    ID_SKILL_IMPORT,
    ID_SKILL_EXPORT,
    ID_SKILL_OPEN,
    ID_SKILL_OPEN_FOLDER
};

namespace {

// LbSkillDisplayNameFromContractPath + LbPathMTimeTicks moved to
// project_context_builder.{h,cpp}.


std::string LbJsonEscape(std::string_view s)
{
    std::string out;
    out.reserve(s.size() + 16);

    for (unsigned char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"':  out += "\\\""; break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (c < 0x20) {
                const char* hex = "0123456789abcdef";
                out += "\\u00";
                out.push_back(hex[(c >> 4) & 0x0f]);
                out.push_back(hex[c & 0x0f]);
            } else {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }

    return out;
}

std::string LbJsonPreview(std::string_view s, size_t maxBytes = 4096)
{
    if (s.size() <= maxBytes) return std::string(s);
    return std::string(s.substr(0, maxBytes)) +
           "\n...[trace preview truncated, original bytes=" +
           std::to_string(s.size()) + "]";
}

std::filesystem::path LbUtf8FsPath(const std::string& path)
{
#ifdef _WIN32
    return std::filesystem::path(wxString::FromUTF8(path).ToStdWstring());
#else
    return std::filesystem::u8path(path);
#endif
}

const char* LbAgentEventTypeName(AgentEventType type)
{
    switch (type) {
    case AgentEventType::LoopBegin:        return "loop_begin";
    case AgentEventType::IterationBegin:   return "iteration_begin";
    case AgentEventType::ToolCall:         return "tool_call";
    case AgentEventType::ToolOutput:       return "tool_output";
    case AgentEventType::ApprovalRequired: return "approval_required";
    case AgentEventType::AgentStatus:      return "agent_status";
    case AgentEventType::Error:            return "error";
    case AgentEventType::TurnComplete:     return "turn_complete";
    case AgentEventType::FileCreated:      return "file_created";
    case AgentEventType::EditApplied:      return "edit_applied";
    case AgentEventType::DirectoryCreated: return "directory_created";
    case AgentEventType::FileDeleted:      return "file_deleted";
    case AgentEventType::LoopEnd:          return "loop_end";
    }
    return "unknown";
}

const char* LbAgentEndReasonName(AgentEndReason reason)
{
    switch (reason) {
    case AgentEndReason::Normal:        return "normal";
    case AgentEndReason::Cancelled:     return "cancelled";
    case AgentEndReason::IterationCap:  return "iteration_cap";
    case AgentEndReason::MalformedCap:  return "malformed_cap";
    case AgentEndReason::StreamError:   return "stream_error";
    case AgentEndReason::SendFailed:    return "send_failed";
    case AgentEndReason::LoopGuard:     return "loop_guard";
    case AgentEndReason::ToolFailedStop:return "tool_failed_stop";
    }
    return "unknown";
}

std::string LbUtcTimestampForJson()
{
    // Format explicitly in UTC: wxDateTime::Format defaults to the LOCAL
    // timezone, so the previous FormatISOCombined('T') + "Z" stamped
    // local wall time with a UTC designator into the agent traces.
    wxDateTime now = wxDateTime::UNow();
    return now.Format("%Y-%m-%dT%H:%M:%S", wxDateTime::UTC).ToStdString()
           + "Z";
}

std::string LbTraceTimestampForFilename()
{
    wxDateTime now = wxDateTime::UNow();
    return now.Format("%Y%m%d_%H%M%S").ToStdString();
}

// LbBackgroundThreadKeeper moved to lb_background_threads.h (used by
// the About dialog's update check / installer download).

const wxColour& LbInteractiveAccentForTheme(const ThemeData& theme)
{
    // Single rule lives in theme.h so the toolbar, the settings cogwheel
    // and the Project/Skills strip can never disagree about hover colour.
    return LbInteractiveAccent(theme);
}

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

// ═══════════════════════════════════════════════════════════════════
//  Chat State Machine
// ═══════════════════════════════════════════════════════════════════
enum class ChatState {
    Idle,
    Streaming,
    RunningCmd,
    RunningGrep,
    RunningPython,
    RunningWebFetch,
    RunningToolWorker,
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
        , m_appState(&wxGetApp().GetAppState())
        , m_chatClient(std::make_unique<ChatClient>(this, m_alive))
        , m_chatDisplay(nullptr)
        , m_chatHistory(std::make_unique<ChatHistory>())
        , m_attachments(std::make_unique<AttachmentManager>())
        , m_cmdExecutor(std::make_unique<CmdExecutor>(this, m_alive))
        , m_pythonRunner(std::make_unique<PythonRunner>(this, m_alive))
        , m_pySessionManager(std::make_unique<PythonSessionManager>(this, m_alive))
        , m_grepExecutor(std::make_unique<GrepExecutor>(this, m_alive))
        , m_webFetchExecutor(std::make_unique<WebFetchExecutor>(this, m_alive))
        , m_toolWorker(std::make_unique<ToolWorkerExecutor>(this, m_alive))
        , m_waitExecutor(std::make_unique<WaitExecutor>(this, m_alive))
        , m_chatState(ChatState::Idle)
        , m_agentModeEnabled(false)

    {
        InitializeCoreServices();
        BindMenuCommands();
        BuildMainLayout();
        InitializeChatDisplayAndDropImports();
        CreateControllersAndCallbacks();
        BindFrameEvents();
        InstallInputIntegrations();
        FinishStartup();
    }

    ~MyFrame() override
    {
        // Backstop for the worker-thread alive sentinel.  OnClose() also
        // marks it dead, but a *defaulted* destructor would leave m_alive
        // == true on any teardown path that doesn't route through a close
        // event (a direct Destroy(), app-shutdown top-level-window cleanup,
        // a future restart/multi-window feature, etc.), which would let a
        // detached worker post into a half-destroyed frame.  MarkDead is
        // idempotent, so doubling up with OnClose is harmless, and this runs
        // while the wxEvtHandler base subobject is still alive.
        LbMarkUiEventTargetDead(m_alive);
        if (m_modelService)
            m_modelService->DetachFrameSink(this);
        wxGetApp().GetConversationRegistry().Remove(this);
    }

    void OnClose(wxCloseEvent& evt)
    {
        // Keep the frame, event target, timers and workers usable if saving
        // fails and the user cancels closing. Persistence must precede teardown.
        if (m_isClosing) { evt.Skip(); return; }
        if (!m_convController->SaveBeforeLeaving(evt.CanVeto())) {
            if (evt.CanVeto()) { evt.Veto(); return; }
            // Windows shutdown can disallow a veto; do not open a blocking
            // recovery dialog then. A failed mandatory save remains logged.
            if (auto* logger = m_appState->GetLogger())
                logger->error("Forced close after conversation save failure.");
        }

        LbMarkUiEventTargetDead(m_alive);
        // The dead token above already blocks rebroadcasts at post
        // time; detaching just removes the stale registry row.
        if (m_modelService)
            m_modelService->DetachFrameSink(this);
        wxGetApp().GetConversationRegistry().Remove(this);

        StopAnimation();
        if (m_reminderTimer.IsRunning())
            m_reminderTimer.Stop();

        // Commit any short UI-side delta batch before autosave.  ChatHistory
        // already flushes its own stream buffer in SaveToFile(), but
        // m_pendingAssistantDelta lives here in MyFrame and can otherwise lose
        // the last few milliseconds of streamed text on close.
        if (m_chatClient && m_chatClient->IsStreaming()) {
            FlushPendingAssistantDelta();
            m_chatClient->StopGeneration();

            // If the request was closed before any assistant text arrived, do
            // not persist a dangling empty assistant placeholder.  Partial text
            // that did arrive is kept as a partial saved answer.
            if (m_chatHistory && m_chatHistory->HasAssistantPlaceholder())
                m_chatHistory->RemoveLastAssistantMessage();
        }

        // Make tool shutdown explicit while the frame/logger still exist.
        // Destructors also signal cancellation, but OnClose is the deterministic
        // place that mirrors the llama-server shutdown below.
        if (m_cmdExecutor)    m_cmdExecutor->Cancel();
        if (m_grepExecutor)   m_grepExecutor->Cancel();
        if (m_pythonRunner)   m_pythonRunner->Cancel();
        // Session kernels are long-lived by design, so frame close is
        // where they die: Shutdown() raises the cancel flag for any
        // in-flight exec and drops every session — each Job Object's
        // KILL_ON_JOB_CLOSE reaps the kernel and its grandchildren.
        m_pyReapTimer.Stop();
        if (m_pySessionManager) m_pySessionManager->Shutdown();
        if (m_webFetchExecutor) m_webFetchExecutor->Cancel();
        if (m_toolWorker)       m_toolWorker->Cancel();

        m_isClosing = true;

        m_appState->SaveWindowState(this);

        // Do not rely only on MyFrame destruction to release VRAM.
        // llama-server owns the loaded model/CUDA context, so make shutdown
        // explicit while a frame and logger are still alive — but ONLY if
        // this was the last window (we detached ourselves above, so a count
        // of zero means exactly that).  Other windows are still chatting
        // against this server; MyApp::OnExit remains the backstop for the
        // true end of the process.
        if (m_modelService && m_modelService->AttachedFrameCount() == 0)
            m_modelService->StopLocalServer();

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
        if (fileSize == wxInvalidSize) return false;

        // Small text is baked inline.  Large text routes like CSV/PDF:
        // workspace import + handle card, so it never floods the
        // request.  The threshold is shared with tool-result demotion,
        // so "large" means the same thing on every path into the
        // model's context.
        if (fileSize.GetValue() > varstore::DemotionConfig{}.thresholdBytes) {
            bool ok = m_dropImportController &&
                      m_dropImportController->QueueLargeTextAttachmentFromDrop(filePath);
            if (ok) RestoreComposerFocusDeferred();
            return ok;
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

    bool QueueCsvAttachmentFromDrop(const std::string& filePath)
    {
        return m_dropImportController &&
               m_dropImportController->QueueCsvAttachmentFromDrop(filePath);
    }

    bool QueueZipAttachmentFromDrop(const std::string& filePath)
    {
        return m_dropImportController &&
               m_dropImportController->QueueZipAttachmentFromDrop(filePath);
    }

    // ── Per-turn session context header ──────────────────────────
    // Models have no clock: without an injected timestamp, every
    // temporal request ("expired", "expiring soon", "overdue") runs
    // on a hallucinated date near the training cutoff.  The header
    // is prepended to the WIRE copy of each user message, after the
    // attachment bakes, so it:
    //   - reaches the model's reasoning, not just its scripts;
    //   - persists in history, keeping the llama.cpp KV prefix
    //     cache byte-stable across turns (a timestamp at the TOP of
    //     the system prompt would invalidate the whole cached prefix
    //     every send);
    //   - temporally grounds past turns when they are replayed into
    //     later requests.
    // The live transcript shows the user's original text (display
    // happens before baking); ConversationController::ReplayConversation
    // strips the header when re-rendering saved chats.
    static std::string BuildSessionContextHeader()
    {
        const wxDateTime now = wxDateTime::Now();
        const wxString stamp = now.Format("%A, %B %d, %Y, %I:%M %p");
        std::string header = "[Session context: current local date/time is ";
        const wxScopedCharBuffer utf8 = stamp.ToUTF8();
        header += utf8.data() ? utf8.data() : "(unavailable)";
        header += ". Dates are m/d/yyyy. Trust this over any internal "
                  "assumption about today's date.]";
        return header;
    }

    void NotifyDocmDropRejected(const std::string& filePath)
    {
        if (m_dropImportController)
            m_dropImportController->NotifyDocmDropRejected(filePath);
    }

private:

    // ─── Constructor setup phases ──────────────────────────────────
    // Keep MyFrame construction order explicit while keeping the
    // constructor body short.
    void InitializeCoreServices()
    {
        // AppState and ModelService are created and initialized by
        // MyApp::OnInit before any frame exists (Chunk C) — by the
        // time this ctor runs, settings are loaded, the logger is up
        // (or has been degraded to null on a locked log file), data
        // dirs exist, and llama-server's owner is waiting.  The frame
        // just wires itself to them.

        // Seed agent-mode flag from the persisted default: AppState's
        // Initialize() has already populated m_agentDefaultOn from
        // wxFileConfig in OnInit.
        m_agentModeEnabled = m_appState->GetAgentDefaultOn();

        // Wire the secrets store into the Python runner so
        // python_run_script subprocesses get the configured
        // Connections injected as environment variables.  Must come
        // after m_appState->Initialize() (the lazy ctor inside
        // GetSecretsStore touches the user-local data dir, which
        // wxStandardPaths only resolves correctly after SetAppName
        // has been called by the wxApp -- guaranteed here).
        m_pythonRunner->SetSecretsStore(m_appState->GetSecretsStore());

        // Borrow the app-owned model service and register this frame as
        // a rebroadcast sink for server lifecycle events (wxEVT_SERVER_READY
        // / wxEVT_SERVER_ERROR flow ServerManager -> service -> attached
        // frames; this frame's existing Bind()s receive the clones).
        m_modelService = &wxGetApp().GetModelService();
        // The probe classifies what this frame's busy state depends
        // on: a session on the shared llama-server (BusyLocal) can be
        // broken by a server stop/restart; a session pinned to a
        // remote endpoint (BusyRemote) cannot.  m_modelSwitcher is
        // constructed after this attach; probes only fire on later
        // user actions, but stay conservative (assume local) if the
        // switcher is somehow absent.
        m_modelService->AttachFrameSink(this, m_alive,
            [this]() {
                if (!IsBusy()) return FrameBusyKind::Idle;
                if (m_modelSwitcher &&
                    !m_modelSwitcher->SessionUsesLocalServer())
                    return FrameBusyKind::BusyRemote;
                return FrameBusyKind::BusyLocal;
            });

        SetBackgroundColour(m_appState->GetTheme().bgMain);
    }


    void BindMenuCommands()
    {
        // Projects: actions live on the ProjectStatusStrip (built below),
        // not on a native Windows menu bar.  The strip's popup menu uses
        // these wxEVT_MENU bindings exactly as the old menu bar did, so
        // the OnProject* handlers stay unchanged.
        Bind(wxEVT_MENU, &MyFrame::OnProjectNew, this, ID_PROJECT_NEW);
        Bind(wxEVT_MENU, &MyFrame::OnProjectAttach, this, ID_PROJECT_ATTACH);
        Bind(wxEVT_MENU, &MyFrame::OnProjectOpenFolder, this, ID_PROJECT_OPEN_FOLDER);
        Bind(wxEVT_MENU, &MyFrame::OnProjectsOpenRootFolder, this, ID_PROJECTS_OPEN_ROOT_FOLDER);
        Bind(wxEVT_MENU, &MyFrame::OnProjectOpenInstructions, this, ID_PROJECT_OPEN_INSTRUCTIONS);
        Bind(wxEVT_MENU, &MyFrame::OnProjectAddSources, this, ID_PROJECT_ADD_SOURCES);
        Bind(wxEVT_MENU, &MyFrame::OnProjectOpenSourcesFolder, this, ID_PROJECT_OPEN_SOURCES_FOLDER);
        Bind(wxEVT_MENU, &MyFrame::OnProjectNewWorkflow, this, ID_PROJECT_NEW_WORKFLOW);
        Bind(wxEVT_MENU, &MyFrame::OnProjectNewWorkflowWithScript, this, ID_PROJECT_NEW_WORKFLOW_WITH_SCRIPT);
        Bind(wxEVT_MENU, &MyFrame::OnProjectOpenWorkflow, this, ID_PROJECT_OPEN_WORKFLOW);
        Bind(wxEVT_MENU, &MyFrame::OnProjectOpenWorkflowsFolder, this, ID_PROJECT_OPEN_WORKFLOWS_FOLDER);
        Bind(wxEVT_MENU, &MyFrame::OnSkillNew, this, ID_SKILL_NEW);
        Bind(wxEVT_MENU, &MyFrame::OnSkillImport, this, ID_SKILL_IMPORT);
        Bind(wxEVT_MENU, &MyFrame::OnSkillExport, this, ID_SKILL_EXPORT);
        Bind(wxEVT_MENU, &MyFrame::OnSkillOpen, this, ID_SKILL_OPEN);
        Bind(wxEVT_MENU, &MyFrame::OnSkillOpenFolder, this, ID_SKILL_OPEN_FOLDER);
        Bind(wxEVT_MENU, &MyFrame::OnProjectClear, this, ID_PROJECT_CLEAR);
        Bind(wxEVT_MENU, &MyFrame::OnProjectDelete, this, ID_PROJECT_DELETE);
    }


    void BuildMainLayout()
    {
        auto* mainSizer = new wxBoxSizer(wxVERTICAL);

        // ─── TOP BAR (via UIBuilder) ─────────────────────────────────
        auto tb = UIBuilder::BuildTopBar(this, mainSizer, m_appState->GetTheme());
        _toolbarPanel   = tb.toolbarPanel;
        _titleLabel     = tb.titleLabel;
        _modelPill      = tb.modelPill;
        _modelPillLeftBracket  = tb.modelPillLeftBracket;
        _modelLabel     = tb.modelLabel;
        _thinkingChip   = tb.thinkingChip;
        _modelPillRightBracket = tb.modelPillRightBracket;
        _statusDot      = tb.statusDot;
        _protocolChip   = tb.protocolChip;
        _ctxMeter       = tb.ctxMeter;
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
            ShowSkillPopupMenu(anchor);
        };
        m_projectStrip = std::make_unique<ProjectStatusStrip>(
            this, m_appState->GetTheme(), stripCallbacks);
        mainSizer->Add(m_projectStrip->GetPanel(), 0, wxEXPAND);

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
        sidebarCallbacks.onNewWindowClicked = [this]() {
            OpenNewWindow();
        };
        sidebarCallbacks.onDeleteRequested = [this](const std::vector<std::string>& paths) {
            // Kill any persistent Python session keyed on a doomed
            // conversation's workspace BEFORE the files go away, so
            // the kernel can't hold handles inside a folder tree the
            // delete is about to walk.  Best effort: a session keyed
            // on a /cd override cwd isn't derivable here and is left
            // to the idle reaper / app close.
            if (m_pySessionManager) {
                for (const std::string& p : paths) {
                    m_pySessionManager->CloseSessionFor(
                        ChatHistory::GetConversationWorkspaceDir(p));
                }
            }
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
                m_projectController->MoveChatsToProject(paths, targetProjectId);
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

        // The transcript sits in a clip panel with the same slim scroll
        // rail as the sidebar: the native vertical scrollbar is pushed just
        // outside the clip (lb_scroll_rail.h), scrolling itself is unchanged.
        _chatClip = new wxPanel(_rightPanel, wxID_ANY);
        _chatClip->SetBackgroundColour(m_appState->GetTheme().bgMain);
        _chatDisplayCtrl = new ChatDisplayCtrl(
            _chatClip, wxID_ANY, wxEmptyString,
            wxDefaultPosition, wxDefaultSize,
            wxRE_MULTILINE | wxRE_READONLY | wxBORDER_NONE
        );
        _chatDisplayCtrl->SetBackgroundColour(m_appState->GetTheme().bgMain);
        _chatDisplayCtrl->SetForegroundColour(m_appState->GetTheme().textPrimary);

        // The chat display is presentation-only chrome: read-only, never
        // user-edited, undo never wanted.  wxRichTextCtrl nevertheless
        // routes every programmatic WriteText/Remove through its
        // wxCommandProcessor by default, allocating an undo action per
        // call -- and Remove() actions retain a styled copy of the text
        // they deleted.  The streaming path (MarkdownRenderer's 16 ms
        // flush: RemovePartialLine + segment-by-segment re-render) and
        // full-conversation replay generate thousands of such actions
        // per session, all dead weight.  Suppress undo for the
        // control's lifetime; never paired with EndSuppressUndo.
        _chatDisplayCtrl->BeginSuppressUndo();

        m_chatRail = new LbScrollRail(
            _rightPanel, _chatClip, _chatDisplayCtrl, _chatDisplayCtrl,
            [this]() -> const ThemeData& { return m_appState->GetTheme(); });
        m_chatRail->SetBackgroundSlot(&ThemeData::bgMain);
        // Wheel over the rail uses the transcript's own (faster) wheel step;
        // rail drags and track clicks report back so follow mode updates.
        m_chatRail->SetWheelTarget(_chatDisplayCtrl);
        m_chatRail->SetScrolledCallback([this]() {
            if (_chatDisplayCtrl) _chatDisplayCtrl->NotifyUserScrolled();
        });

        // One sizer item in the transcript's slot (index 0): later code
        // inserts the attachment bar at index 1.  The 8 px rail column
        // doubles as the right margin.
        auto* chatRow = new wxBoxSizer(wxHORIZONTAL);
        chatRow->Add(_chatClip, 1, wxEXPAND);
        chatRow->Add(m_chatRail, 0, wxEXPAND);
        rightSizer->Add(chatRow, 1, wxEXPAND | wxLEFT, 8);

        // ─── INPUT AREA (via UIBuilder) ──────────────────────────────
        auto ia = UIBuilder::BuildInputArea(_rightPanel, rightSizer, m_appState->GetTheme());
        _inputContainer = ia.inputContainer;
        _inputSeparator = ia.inputSeparator;
        _userInputCtrl  = ia.userInputCtrl;
        _sendButton     = ia.sendButton;
        _stopButton     = ia.stopButton;
        _attachButton   = ia.attachButton;
        _inputSizer     = ia.inputSizer;

        // Composer scroll rail: same slim themed rail as the transcript and
        // sidebar.  The native EDIT scrollbar is pushed outside ia.inputClip;
        // the rail sits on the input field colour so it reads as part of it.
        m_inputRail = new LbScrollRail(
            _inputContainer, ia.inputClip, _userInputCtrl,
            [this]() -> const ThemeData& { return m_appState->GetTheme(); });
        m_inputRail->SetBackgroundSlot(&ThemeData::bgInputField);
        ia.inputFieldRow->Add(m_inputRail, 0, wxEXPAND);

        // ─── ATTACHMENT CHIP BAR (hidden by default) ─────────────────
        // Keep pending cards on the chat surface, immediately above the
        // composer.  Parenting this bar to _inputContainer makes the whole
        // full-width attachment row use bgInputArea as soon as the first
        // chip is shown, which creates a visible horizontal band above the
        // message field.  The separate bgMain strip preserves the original
        // floating-card appearance while the input row remains unchanged.
        _attachChipBar = new wxPanel(_rightPanel, wxID_ANY);
        _attachChipBar->SetBackgroundColour(m_appState->GetTheme().bgMain);
        _attachChipSizer = new wxWrapSizer(wxHORIZONTAL);
        _attachChipBar->SetSizer(_attachChipSizer);
        _attachChipBar->Hide();
        rightSizer->Insert(
            1, _attachChipBar, 0,
            wxLEFT | wxTOP | wxRIGHT, FromDIP(10));

        // ─── ACTIVITY STRIP (hidden by default) ──────────────────────
        // One-row live status for a running async tool (powershell /
        // wait / python): gauge + elapsed clock + latest output line.
        // Sits directly above the composer so it reads as "the thing
        // you're waiting on", not as part of the transcript.
        m_activityStrip = std::make_unique<ActivityStrip>(
            this, _rightPanel, m_appState->GetTheme());
        rightSizer->Insert(
            rightSizer->GetItemCount() - 1, m_activityStrip->GetPanel(), 0,
            wxEXPAND | wxLEFT | wxRIGHT, 8);

        m_attachments->SetLogger(m_appState->GetLogger());
        m_attachments->SetOnChanged([this]() { RebuildAttachmentChips(); });

        // ─── Agent-mode toggle ───────────────────────────────────────
        // Sits right after the attach button in _inputSizer.  Visual
        // state: muted when off, interactive-accent-colored when on.  Click flips
        // m_agentModeEnabled and re-tints.
        _agentToggleButton = new wxButton(
            _inputContainer, wxID_ANY,
            wxEmptyString,
            wxDefaultPosition, wxSize(44, 46),
            wxBORDER_NONE);
        LbIcons::ApplyAgentToggle(_agentToggleButton, m_appState->GetTheme(), m_agentModeEnabled);
        _agentToggleButton->SetToolTip(
            "Agent mode: when ON, the model can call tools (read, ls, open, grep, pwd, powershell) "
            "to answer your questions.  Click to toggle.");
        // Insert right after attach button.  _inputSizer was built
        // as: [attach][userInput][send/stop].  Find attach's index
        // via GetChildren() to avoid hardcoding a position in case
        // UIBuilder changes later.
        {
            size_t attachIdx = 0;
            bool foundAttachButton = false;
            const auto& children = _inputSizer->GetChildren();
            for (size_t i = 0; i < children.size(); ++i) {
                if (children[i]->GetWindow() == _attachButton) {
                    attachIdx = i;
                    foundAttachButton = true;
                    break;
                }
            }
            wxASSERT_MSG(foundAttachButton,
                         "BuildMainLayout: attach button not found in input sizer");
            _inputSizer->Insert(foundAttachButton ? attachIdx + 1 : children.size(),
                                _agentToggleButton, 0,
                                wxALIGN_CENTER_VERTICAL | wxLEFT, 4);
        }

        _rightPanel->SetSizer(rightSizer);
        _contentSizer->Add(_rightPanel, 1, wxEXPAND);
        mainSizer->Add(_contentSizer, 1, wxEXPAND);
        SetSizer(mainSizer);

        // Composer drag-resize handle (separator above the input) +
        // restore of any persisted manual height.  Same pattern as the
        // sidebar-width restore above: read once at build, write on
        // drag release.  0 = auto-grow, nothing to apply.
        BindInputResizeHandle();
        if (const int h = m_appState->GetInputAreaHeight(); h > 0)
            ApplyInputHeightOverride(h);
    }


    void InitializeChatDisplayAndDropImports()
    {
        // ─── Setup fonts ─────────────────────────────────────────────
        // Font size is user-configurable via Settings; persisted in AppState.
        wxFont codeFont = m_appState->CreateMonospaceFont(m_appState->GetFontSize());
        _chatDisplayCtrl->SetFont(codeFont);
        _userInputCtrl->SetFont(codeFont);

        m_chatDisplay = std::make_unique<ChatDisplay>(_chatDisplayCtrl);
        m_chatDisplay->SetFont(codeFont);
        m_chatDisplay->ApplyTheme(m_appState->GetTheme());

        // Live progress for pending async tools lives in the activity
        // strip, not in the transcript.  The wait tool gets a countdown
        // (elapsed / requested); everything else gets elapsed / timeout
        // plus whatever the command last printed (wxEVT_CMD_OUTPUT).
        m_chatDisplay->SetPendingToolCallbacks(
            [this](const ToolBlock& block) {
                if (!m_activityStrip) return;
                ++m_activityRevision;
                if (block.pendingWaitTotalSec > 0) {
                    std::string title = "Waiting";
                    if (!block.pendingWaitReason.empty())
                        title += "  \xC2\xB7  " + block.pendingWaitReason;
                    m_activityStrip->Begin(title, block.pendingWaitTotalSec,
                                           /*countdown=*/true);
                } else {
                    m_activityStrip->Begin("Running " + block.toolName,
                                           block.pendingTimeoutSec,
                                           /*countdown=*/false);
                }
            },
            [this]() {
                ++m_activityRevision;
                if (m_activityStrip) m_activityStrip->End();
            });

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
            // Notice, not Message: the busy-drop rejection fires while a
            // tool may be running and must not tear down its live card.
            // With nothing live it renders identically.
            if (m_chatDisplay) m_chatDisplay->DisplaySystemNotice(message);
        };
        dropImportCallbacks.resolveCurrentCwd = [this]() {
            return ResolveCurrentCwd();
        };
        dropImportCallbacks.restoreComposerFocusDeferred = [this]() {
            RestoreComposerFocusDeferred();
        };
        dropImportCallbacks.noteWorkspaceSideEffect = [this]() {
            // A dropped file was copied into this conversation's
            // Workspace.  Mark the history dirty so AutoSaveConversation
            // writes the JSON even if the user never types: that puts
            // the chat in the sidebar and keeps DeleteConversation's
            // chat-folder cleanup reachable.  Without this the folder
            // minted by EnsureConversationChatFolder is orphaned forever.
            m_chatHistory->NoteWorkspaceSideEffect();
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
        dropImportCallbacks.attachCsvFile =
            [this](const std::string& absPath, const std::string& relPath) {
                return m_attachments->AttachCsvFile(absPath, relPath);
            };
        dropImportCallbacks.attachZipFile =
            [this](const std::string& absPath, const std::string& relPath) {
                return m_attachments->AttachZipFile(absPath, relPath);
            };
        dropImportCallbacks.attachTextFileRef =
            [this](const std::string& absPath, const std::string& relPath) {
                return m_attachments->AttachTextFileRef(absPath, relPath);
            };
        m_dropImportController =
            std::make_unique<DropImportController>(std::move(dropImportCallbacks));
    }


    void CreateControllersAndCallbacks()
    {
        _statusDot->SetColors(m_appState->GetTheme().accentButton,
                              m_appState->GetTheme().textMuted);

        // ─── Project-context builder ─────────────────────────────────
        // Owns the cached project/Skills system-prompt block and the
        // brief status-strip count cache.  Created first so the agent /
        // skill callbacks below can route through it at runtime.
        m_projectContextBuilder =
            std::make_unique<ProjectContextBuilder>(m_chatHistory);

        // ─── Create agent controller ─────────────────────────────────
        // MyFrame is the AgentEventSink: it receives structured
        // loop-progress events and translates them to UI operations.
        // Tool blocks arrive via OnAgentToolBlock and are forwarded to
        // the display from there.  Callbacks are wired below, after all
        // coordinators are in place.
        m_agentController = std::make_unique<AgentController>(
            m_chatHistory,
            this,
            m_appState,
            m_grepExecutor.get(),
            m_cmdExecutor.get(),
            m_pythonRunner.get(),
            m_pySessionManager.get(),
            m_webFetchExecutor.get(),
            m_toolWorker.get(),
            m_waitExecutor.get());

        // Apply the user-configurable tool-step cap persisted by AppState.
        m_agentController->SetMaxToolSteps(
            m_appState->GetAgentMaxToolSteps());

        // ─── Create coordinators ─────────────────────────────────────
        m_modelSwitcher = std::make_unique<ModelSwitcher>(
            *m_modelService,
            *m_appState, m_modelService->Server(), m_chatDisplay.get(),
            m_chatHistory, *m_attachments, _statusDot, _modelLabel, this);

        m_convController = std::make_unique<ConversationController>(
            *this, *m_appState, m_chatHistory, m_chatDisplay.get(),
            *m_attachments, *m_sidebar, m_modelService->Server(),
            *m_modelSwitcher, _statusDot, m_alive);

        m_modelSwitcher->SetCallbacks({
            /*isBusy*/            [this]() { return IsBusy(); },
            /*autoSave*/          [this]() { m_convController->AutoSaveConversation(); },
            /*updateWindowTitle*/ [this]() { m_convController->UpdateWindowTitle(); },
            /*onRemoteActivated*/ [this](ToolProtocol proto) {
                // Frame-owned half of the synthesized ready state: set
                // the active protocol (Native for remote) so request
                // bodies build correctly, and refresh the protocol chip.
                _activeProtocol = proto;
                if (_protocolChip) UpdateProtocolChip(proto);
            },
            /*appendThinkingSubmenu*/ [this](wxMenu& menu) {
                if (!m_chatHistory) return;
                auto* sub = new wxMenu;   // owned by the parent menu
                BuildThinkingMenu(*sub);
                // Show the current mode inline, "Thinking (Auto)", so the
                // setting is readable without opening the submenu.
                wxString mode = wxString::FromUTF8(
                    ThinkingModeName(m_chatHistory->GetThinkOverride()));
                mode = mode.Left(1).Upper() + mode.Mid(1);
                menu.AppendSubMenu(sub, "Thinking (" + mode + ")",
                    "Thinking override for this conversation");
            }
        });
        m_convController->SetCallbacks({
            /*isBusy*/                [this]() { return IsBusy(); },
            /*onProjectStateChanged*/ [this]() {
                RefreshProjectStrip();
                // Fires at the end of every UpdateWindowTitle(), i.e.
                // after loads, New Chat, save-as, and delete-switch.
                // Re-anchor the context meter when the conversation
                // identity actually changed; otherwise just refresh.
                const std::string histPath =
                    m_chatHistory ? m_chatHistory->GetFilePath() : std::string();
                if (histPath != m_ctxMeterHistoryPath) {
                    m_ctxMeterHistoryPath = histPath;
                    // A path change is NOT always an identity change: a
                    // new chat's first autosave assigns its path right
                    // after the first completed turn, and rename/save-as
                    // keep the same transcript, so neither may drop the
                    // exact anchor adopted seconds earlier.  ChatHistory's
                    // revision is the discriminator: Clear() and a load
                    // reset it to 0, so an anchor whose revision is still
                    // <= the current one describes this same transcript.
                    const std::uint64_t rev =
                        m_chatHistory ? m_chatHistory->GetRevision() : 0;
                    const bool sameTranscript =
                        m_ctxAnchorExact && rev > 0 &&
                        rev >= m_ctxAnchorRevision;
                    if (!sameTranscript) {
                        InvalidateContextAnchor();
                        // A loaded chat brings back its reply stats so
                        // the HUD's "Last reply" / "This chat" aren't
                        // blank until the next reply.
                        RestoreTurnStatsFromLog();
                    }
                }
                RefreshContextMeter();
            },
            /*cancelPendingSend*/     [this]() {
                CancelPendingSendForConversationSwitch();
            },
            /*beforeDurableSave*/     [this]() { FlushPendingAssistantDelta(); }
        });

        // Initial strip render now that the controller can drive refreshes.
        RefreshProjectStrip();

        // ─── AgentController callbacks ───────────────────────────────
        // Callbacks carry only logic concerns (sendRequest,
        // buildToolContext, buildSystemPrompt, bumpGenerationId,
        // getActiveProtocol).  UI-shaped work lives in the
        // AgentEventSink methods further down (OnAgentIterationBegin,
        // OnAgentLoopEnd).
        m_agentController->SetCallbacks({
            /*sendRequest*/ [this](const std::string& /*model*/,
                                   const std::string& body,
                                   unsigned long      genId) {
                // Pin agent iterations to this conversation's target:
                // another window switching the app-global model mid-
                // loop must not retarget iteration N+1.
                const InferenceTarget t =
                    m_modelSwitcher->ResolveTargetForConversation();
                // Keep the request builder's /think dialect in sync
                // with the target actually being hit.  This body was
                // already built (turn start set the dialect for it);
                // the refresh covers the NEXT iteration's build.
                if (m_chatHistory) {
                    m_chatHistory->SetActiveReasoningDialect(
                        t.reasoningDialect);
                    m_chatHistory->SetActiveResponsesApi(t.responsesApi);
                }
                return m_chatClient->SendMessage(t, body, genId);
            },
            /*buildToolContext*/ [this]() { return BuildToolContext(); },
            /*buildSystemPrompt*/ [this]() { return BuildAgentSystemPrompt(); },
            /*bumpGenerationId*/ [this]() {
                ++m_generationId;
                return m_generationId;
            },
            /*getActiveProtocol*/ [this]() { return _activeProtocol; },
            /*resolveWireModel*/ [this]() {
                // Same per-conversation value the main send path uses
                // (see the `model` local in the send handler).  Holds
                // the wire model id for remote lanes and the gguf path
                // for local ones, which is exactly what each provider
                // expects on the "model" field.
                return m_modelSwitcher
                     ? m_modelSwitcher->GetConversationModelForSave()
                     : std::string();
            },
        });

        // ─── ToolResultController ────────────────────────────────────
        // Owns the four async tool-completion/error handlers (cmd,
        // python, grep, web-fetch).  Created after the agent and
        // conversation controllers it depends on; its events are bound
        // to it directly in BindFrameEvents().  Frame keeps only the
        // streaming-UI and is-closing concerns via the callback seam.
        m_toolResultController = std::make_unique<ToolResultController>(
            m_chatDisplay.get(), m_chatHistory,
            *m_agentController, *m_convController);
        m_toolResultController->SetCallbacks({
            /*setStreamingState*/ [this](bool streaming) { SetStreamingState(streaming); },
            /*isClosing*/         [this]() { return m_isClosing; },
        });

        // SkillDraftController owns the conversational Skill design-session
        // state plus the hidden Skill Draft Builder control turn.
        m_skillDraftController = std::make_unique<SkillDraftController>(
            m_chatHistory, m_chatDisplay.get(), *m_appState, *m_chatClient,
            *m_modelSwitcher, *m_convController);

        m_skillDraftController->SetCallbacks({
            /*isBusy*/             [this]() { return IsBusy(); },
            /*isClosing*/          [this]() { return m_isClosing; },
            /*bumpGenerationId*/   [this]() { return ++m_generationId; },
            /*setChatStateStreaming*/ [this]() { m_chatState = ChatState::Streaming; },
            /*setStreamingUi*/     [this](bool s) { SetStreamingState(s); },
            /*discardPendingAssistantDelta*/ [this]() { DiscardPendingAssistantDelta(); },
            /*invalidateProjectContextCache*/ [this]() { m_projectContextBuilder->Invalidate(); },
            /*refreshProjectStrip*/ [this]() { RefreshProjectStrip(); },
        });

        // ProjectController owns the project action surface (create /
        // attach / switch / clear / delete, move-chats, add-sources,
        // workflows).  The strip popups and sidebar context menus stay
        // in the frame and call straight into it; the OnProject*
        // command handlers below are thin delegations.
        m_projectController = std::make_unique<ProjectController>(
            m_chatHistory, m_chatDisplay.get(), *m_appState,
            *m_projectContextBuilder, *m_convController,
            m_sidebar.get(), this);

        m_projectController->SetCallbacks({
            /*isBusy*/              [this]() { return IsBusy(); },
            /*refreshProjectStrip*/ [this]() { RefreshProjectStrip(); },
        });
    }


    void BindFrameEvents()
    {
        // ─── Bind events ─────────────────────────────────────────────
        _sendButton->Bind(wxEVT_BUTTON, &MyFrame::OnSendMessage, this);
        _stopButton->Bind(wxEVT_BUTTON, &MyFrame::OnStopGeneration, this);

        // Animation timer
        Bind(wxEVT_TIMER, &MyFrame::OnAnimationTimer, this, m_animTimer.GetId());
        Bind(wxEVT_TIMER, &MyFrame::OnPySessionReapTimer, this,
             m_pyReapTimer.GetId());
        m_pyReapTimer.Start(60 * 1000);   // sweep cadence, not the timeout

        // Streamed assistant chunks can arrive very quickly from local models.
        // Batch them into one UI update per frame-ish interval so wxRichTextCtrl
        // does far less Freeze/Thaw/scroll work while still feeling live.
        Bind(wxEVT_TIMER, &MyFrame::OnAssistantDeltaFlushTimer, this,
             m_assistantDeltaFlushTimer.GetId());

        // In-app reminders are process-local delivery: the persistent store
        // survives restarts, while this lightweight timer claims reminders
        // that become due whenever any LlamaBoss window is running.
        Bind(wxEVT_TIMER, &MyFrame::OnReminderTimer, this,
             m_reminderTimer.GetId());

        // Queued-send safety net: fires only if tool-protocol detection
        // never reports after the model became ready.
        Bind(wxEVT_TIMER, &MyFrame::OnPendingSendProtocolTimeout, this,
             m_pendingSendProtocolTimer.GetId());


        // Attach: native SVG bitmap states tint the paperclip on hover;
        // no background tile (glyph-only, like the ctx meter).
        _attachButton->Bind(wxEVT_BUTTON, &MyFrame::OnAttachImage, this);
        BindIconHover(_attachButton, &ThemeData::bgInputArea, false);

        // Agent toggle — glyph-only hover like attach; click flips
        // m_agentModeEnabled and re-tints.  The glyph keeps its state
        // colour (accent when ON); when OFF it brightens on hover.
        _agentToggleButton->Bind(wxEVT_BUTTON, &MyFrame::OnToggleAgentMode, this);
        LbHoverTile::BindGlyph(_agentToggleButton);
        
        
        _userInputCtrl->Bind(wxEVT_TEXT_ENTER, &MyFrame::OnSendMessage, this);
        _userInputCtrl->Bind(wxEVT_TEXT, &MyFrame::OnUserInputChanged, this);

        // Native bitmap states handle the glyph's hover, focus, pressed and
        // disabled colors; no background tile.
        _settingsButton->Bind(wxEVT_BUTTON, &MyFrame::OnOpenSettings, this);
        BindIconHover(_settingsButton, &ThemeData::bgToolbar, false);

        // New Chat (+): interactive accent glyph on hover.
        _newChatButton->Bind(wxEVT_BUTTON, &MyFrame::OnNewChat, this);
        BindIconHover(_newChatButton, &ThemeData::bgToolbar, true);


        // Sidebar/history toggle (hamburger): same glyph hover as + and ⚙.
        _sidebarToggle->Bind(wxEVT_BUTTON, &MyFrame::OnToggleSidebar, this);
        BindIconHover(_sidebarToggle, &ThemeData::bgToolbar, true);
        // About (i): same glyph hover.
        _aboutButton->Bind(wxEVT_BUTTON, &MyFrame::OnAbout, this);
        BindIconHover(_aboutButton, &ThemeData::bgToolbar, true);

        // Pinned context panel follows the chat view's corner: window
        // moves, chat view resizes (window resize, sidebar toggle, input
        // area growing), and hides while the window is minimized.
        Bind(wxEVT_MOVE, [this](wxMoveEvent& e) {
            if (m_contextHud) m_contextHud->Reposition();
            e.Skip();
        });
        if (_chatDisplayCtrl) {
            _chatDisplayCtrl->Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
                if (m_contextHud) CallAfter([this]() {
                    if (m_contextHud) m_contextHud->Reposition();
                });
                e.Skip();
            });
        }
        // On Windows a wxPopupWindow is WS_EX_TOPMOST: left alone it would
        // float over other apps after you switch away, and over modal
        // dialogs (Settings).  Hide it while LlamaBoss is not the active
        // app/window; bring it back on reactivation.  A click on the panel
        // itself may briefly make it the active window -- that is not a
        // reason to hide.
#ifndef __WXMSW__
        Bind(wxEVT_ACTIVATE, [this](wxActivateEvent& e) {
            if (m_contextHud) {
                if (e.GetActive()) {
                    if (!IsIconized() && !m_contextHud->IsShown()) {
                        m_contextHud->Show();
                        m_contextHud->Reposition();
                    }
                } else {
                    CallAfter([this]() {
                        if (!m_contextHud || IsActive()) return;
                        if (wxGetActiveWindow() == m_contextHud) return;
                        m_contextHud->Hide();
                    });
                }
            }
            e.Skip();
        });
#else
        // Windows: the panel is topmost, so visibility follows one rule,
        // polled: shown only while this frame (or the panel) is the
        // foreground window and the frame isn't minimized.  Covers app
        // switches, the Snipping Tool overlay, Settings and other dialogs
        // without depending on which window happened to get the
        // deactivate message.
        m_contextHudVisTimer.SetOwner(this);
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { SyncContextHudVisibility(); },
             m_contextHudVisTimer.GetId());
#endif
        m_prewarmTimer.SetOwner(this);
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
                 TryPromptPrewarm(m_prewarmReason);
             }, m_prewarmTimer.GetId());
        Bind(wxEVT_ICONIZE, [this](wxIconizeEvent& e) {
            if (m_contextHud) {
                if (e.IsIconized()) m_contextHud->Hide();
                else CallAfter([this]() {
                    if (m_contextHud) { m_contextHud->Show(); m_contextHud->Reposition(); }
                });
            }
            e.Skip();
        });

        // ctx meter: click opens the context details panel; hover uses the
        // shared accent except while the meter is showing amber/red.
        if (_ctxMeter) {
            _ctxMeter->SetCursor(wxCURSOR_HAND);
            _ctxMeter->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { ToggleContextHud(); });
            // Reopen the panel if it was open when the app last closed.
            // CallAfter: the frame is shown and laid out by then, so the
            // panel can anchor to the chat view's corner.
            if (m_appState && m_appState->GetContextHudOpen() && m_appState->GetContextMeterOn())
                CallAfter([this]() {
                    if (!m_isClosing && !m_contextHud && IsShown() && !IsIconized())
                        ToggleContextHud();
                });
            _ctxMeter->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& e) {
                m_ctxMeterHover = true;
                RefreshContextMeter();
                e.Skip();
            });
            _ctxMeter->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) {
                m_ctxMeterHover = false;
                RefreshContextMeter();
                e.Skip();
            });
        }
        Bind(wxEVT_ACTIVATE, &MyFrame::OnFrameActivate, this);

        Bind(wxEVT_ASSISTANT_DELTA, &MyFrame::OnAssistantDelta, this);
        Bind(wxEVT_ASSISTANT_COMPLETE, &MyFrame::OnAssistantComplete, this);
        Bind(wxEVT_ASSISTANT_ERROR, &MyFrame::OnAssistantError, this);

        // ─── PowerShell executor ──────────────────────────────────
        Bind(wxEVT_CMD_COMPLETE, &ToolResultController::OnCmdComplete, m_toolResultController.get());
        Bind(wxEVT_CMD_OUTPUT, [this](wxCommandEvent& e) {
            if (m_isClosing || !m_activityStrip) return;
            m_activityStrip->SetLiveLine(WxToUtf8(e.GetString()));
        });
        Bind(wxEVT_CMD_ERROR,    &ToolResultController::OnCmdError,    m_toolResultController.get());

        // ─── controlled Python helper runner ─────────────────────
        Bind(wxEVT_PYTHON_COMPLETE, &ToolResultController::OnPythonComplete, m_toolResultController.get());
        Bind(wxEVT_PYTHON_ERROR,    &ToolResultController::OnPythonError,    m_toolResultController.get());

        // ─── persistent Python session (RLM step 2) ──────────────
        // S2: the py tool is router-registered, so completion routes
        // through ToolResultController like every other async tool
        // (agent loop first, slash card otherwise).
        Bind(wxEVT_PY_SESSION_COMPLETE, &ToolResultController::OnPySessionComplete, m_toolResultController.get());

        // ─── grep (threaded executor) ─────────────────────────────
        Bind(wxEVT_GREP_COMPLETE, &ToolResultController::OnGrepComplete, m_toolResultController.get());
        Bind(wxEVT_WEB_FETCH_COMPLETE, &ToolResultController::OnWebFetchComplete, m_toolResultController.get());
        Bind(wxEVT_WEB_FETCH_ERROR,    &ToolResultController::OnWebFetchError,    m_toolResultController.get());
        Bind(wxEVT_TOOL_WORKER_COMPLETE, &ToolResultController::OnToolWorkerComplete, m_toolResultController.get());
        Bind(wxEVT_WAIT_COMPLETE, &ToolResultController::OnWaitComplete, m_toolResultController.get());

        // Thinking chip: the ". Auto" segment of the model pill opens the
        // thinking popup menu.  Same six modes as the model picker's
        // "Thinking" submenu (see BuildThinkingMenu).
        _thinkingChip->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
            if (!m_chatHistory || m_isClosing || IsBusy()) return;
            wxMenu menu;
            BuildThinkingMenu(menu);
            // Popup menus run a nested event loop; BuildThinkingMenu's
            // handler rechecks the guards before applying.
            _thinkingChip->PopupMenu(&menu,
                wxPoint(0, _thinkingChip->GetClientSize().GetHeight()));
            if (!m_isClosing && _userInputCtrl) _userInputCtrl->SetFocus();
        });
        _thinkingChip->Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent&) {
            RefreshThinkingSelector();
        });
        // Hover: the chip lights up on its own (it opens a different menu
        // than the rest of the pill, so the brackets stay muted).
        _thinkingChip->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& e) {
            _thinkingChip->SetForegroundColour(
                LbInteractiveAccentForTheme(m_appState->GetTheme()));
            _thinkingChip->Refresh();
            e.Skip();
        });
        _thinkingChip->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) {
            _thinkingChip->SetForegroundColour(m_appState->GetTheme().textMuted);
            _thinkingChip->Refresh();
            e.Skip();
        });

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

        // Hover recoloring: brackets AND the model name light up in the
        // interactive accent when the pointer is anywhere over the pill
        // (any child widget); back to muted / primary on leave.  The
        // thinking chip is excluded -- it opens its own menu and has its
        // own hover.  We bind on every child because wxWidgets does NOT
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
            if (_modelLabel) {
                _modelLabel->SetForegroundColour(LbInteractiveAccentForTheme(th));
                _modelLabel->Refresh();
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
            if (_modelLabel) {
                _modelLabel->SetForegroundColour(th.textPrimary);
                _modelLabel->Refresh();
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
        Bind(wxEVT_MODEL_SERVICE_STATE_CHANGED,
             &MyFrame::OnModelServiceStateChanged, this);
        Bind(wxEVT_SERVER_READY, &MyFrame::OnServerReady, this);
        Bind(wxEVT_SERVER_ERROR, &MyFrame::OnServerError, this);

        // Tool protocol detection result
        Bind(wxEVT_TOOL_PROTOCOL_DETECTED,
             &MyFrame::OnToolProtocolDetected, this);
    }


    void InstallInputIntegrations()
    {
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
        _userInputCtrl->SetTextPasteHandler([this]() -> bool {
            if (IsBusy() || m_isClosing) return false;
            return TryPasteLargeTextFromClipboard();
        });
        _userInputCtrl->SetToolTip(
            "Enter to send; Shift+Enter for a new line.\n"
            "Large pastes become text attachments.\n"
            "Ctrl+Shift+V pastes directly into the message instead.");

        // Keyboard shortcuts
        Bind(wxEVT_CHAR_HOOK, &MyFrame::OnCharHook, this);
    }


    void FinishStartup()
    {
        // Record the TLS posture exactly once, so "downloads stopped
        // working" and "certificate verify failed" are one log line
        // apart instead of a guessing game.  Initialization itself is
        // idempotent; calling it here just makes the outcome visible
        // before the first HTTPS request rather than after.
        lb::EnsureSSLInitialized();
        if (auto* logger = m_appState->GetLogger()) {
            if (lb::SSLVerificationEnabled())
                logger->information(lb::SSLInitSummary());
            else
                logger->warning(lb::SSLInitSummary());
        }

        // Load icon and update model display
        m_appState->LoadApplicationIcon(this);
        m_modelSwitcher->UpdateModelLabel();

        // Restore window state
        m_appState->RestoreWindowState(this);
        Bind(wxEVT_CLOSE_WINDOW, &MyFrame::OnClose, this);

        // Final setup
        // Polling is cheap: ReminderStore loads REMINDERS.json once into memory,
        // so each tick is only an in-memory due-time scan. Overdue reminders
        // are intentionally eligible on the first tick after launch.
        if (!m_reminderTimer.IsRunning())
            m_reminderTimer.Start(1000);

        CallAfter([this]() {
            _userInputCtrl->SetFocus();
            wxCommandEvent anEvent(wxEVT_TEXT, _userInputCtrl->GetId());
            OnUserInputChanged(anEvent);
            // Once per application, not once per window: the first
            // frame boots the last-used model; any later frame joins
            // the server that is already running (Chunk D).
            if (m_modelService->ConsumeInitialBootstrap()) {
                m_modelSwitcher->StartInitialServer();
            }
            else {
                // Late joiner: synchronize local/remote target identity and
                // per-frame readiness even when no llama-server event exists
                // (remote targets).
                m_modelService->QueueCurrentStateTo(this, m_alive);

                if (m_modelService->IsServerReady() &&
                    m_modelService->ResolveTarget().managed &&
                    m_modelService->Server().IsProcessRunning()) {
                    // Local late joiners additionally need the normal ready
                    // path for protocol probing and ready UI effects.
                    auto* readyEvent = new wxCommandEvent(wxEVT_SERVER_READY);
                    SetServerEventGeneration(
                        *readyEvent,
                        m_modelService->Server().GetLaunchGeneration());
                    readyEvent->SetInt(
                        m_modelService->Server().IsCurrentServerJinjaEnabled()
                            ? 1 : 0);
                    wxQueueEvent(this, readyEvent);
                }
            }
        });
    }

    // Drag-and-drop on Windows can finish focus/activation negotiation after
    // the wxFileDropTarget callback returns. A single wxTextCtrl::SetFocus()
    // can leave the edit control drawing a caret while keyboard input is still
    // routed to the frame, the read-only transcript, or even the shell drag
    // source. That mismatch is what produces the Windows "ding" when the user
    // starts typing after dropping an image.
    //
    // Reclaim both top-level activation and the native edit-control focus, then
    // repeat once on the next idle turn to win any late focus cleanup posted by
    // OLE drag/drop. This path is only called after an explicit user drop/paste
    // into LlamaBoss, so it is safe to bring the frame forward here.
    void RestoreComposerFocusNow()
    {
        if (m_isClosing || !_userInputCtrl || !_userInputCtrl->IsEnabled())
            return;

#ifdef __WXMSW__
        HWND frameHwnd = reinterpret_cast<HWND>(GetHandle());
        HWND inputHwnd = reinterpret_cast<HWND>(_userInputCtrl->GetHandle());
        if (frameHwnd) {
            ::SetForegroundWindow(frameHwnd);
            ::SetActiveWindow(frameHwnd);
        }
        if (inputHwnd) {
            ::SetFocus(inputHwnd);
        }
#endif

        Raise();
        _userInputCtrl->SetFocus();
        _userInputCtrl->SetInsertionPointEnd();
        _userInputCtrl->Refresh();
    }

    void RestoreComposerFocusDeferred()
    {
        CallAfter([this]() {
            RestoreComposerFocusNow();
            CallAfter([this]() {
                RestoreComposerFocusNow();
            });
        });
    }

    // Telegram-style modal scrim moved to lb_modal_scrim.{h,cpp};
    // call sites use LbShowModalWithScrim(*this, dlg).

    // ─── UI Controls ──────────────────────────────────────────────
    ChatDisplayCtrl* _chatDisplayCtrl;
    wxPanel*         _chatClip = nullptr;    // clips the transcript's native scrollbar
    LbScrollRail*    m_chatRail = nullptr;   // slim themed scrollbar beside the transcript
    LbScrollRail*    m_inputRail = nullptr;  // same rail beside the composer
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

    // Decoded composer thumbnails, keyed by a cheap fingerprint of the
    // pending item.  RebuildAttachmentChips() runs on every add, every
    // remove, AND every theme change -- without this a 40 MB pasted
    // screenshot would be base64-decoded and rescaled on each repaint
    // of the strip.
    std::map<std::string, wxImage> m_chipThumbCache;
    wxBoxSizer*      _inputSizer;
    wxBoxSizer*      _contentSizer;

    wxPanel*       _toolbarPanel;
    wxStaticText*  _titleLabel;
    wxPanel*       _modelPill;
    wxPanel*       _topSeparator;

    wxPanel*       _rightPanel;
    wxPanel*       _inputContainer;
    wxPanel*       _inputSeparator;

    // ── Composer vertical drag-resize state ───────────────────────
    // Mirror of ConversationSidebar's border drag, rotated 90°.
    // m_inputHeightOverride > 0 means the user dragged the handle;
    // OnUserInputChanged treats it as a *floor*, so content-driven
    // auto-grow can still expand past it but never shrinks below it.
    // 0 = pure auto-grow (the pre-feature behavior, and the default).
    bool m_inputDragActive      = false;
    int  m_inputDragStartY      = 0;
    int  m_inputDragStartH      = 0;
    int  m_inputDragAutoHeight  = 0;  // content floor sampled once per drag
    int  m_inputHeightOverride  = 0;
    static constexpr int kInputMinHeightPx = 30;  // auto-grow base height

    std::unique_ptr<ConversationSidebar> m_sidebar;
    bool m_isClosing;

    wxStaticText* _modelLabel;
    wxStaticText* _thinkingChip = nullptr;   // ". Auto" segment of the model pill
    wxStaticText* _modelPillLeftBracket = nullptr;   // "[" — hover-recolored
    wxStaticText* _modelPillRightBracket = nullptr;  // "]" — hover-recolored
    StatusDot*    _statusDot;
    wxStaticText* _protocolChip;   // native/xml chip beside model name
    ToolProtocol  _activeProtocol = ToolProtocol::Unknown;   // detected protocol of the loaded model

    // ── Context meter ──────────────────────────────────────────────
    // Top-bar "ctx <used>/<window>" occupancy readout.
    //
    // Anchor semantics (hybrid): after every completed transcript turn
    // the server's exact `usage` counts become the anchor (prompt +
    // completion = exact occupancy of the context as of that turn).
    // Between turns the composer's pending text rides on top as a cheap
    // estimate.  When no exact anchor exists (fresh load, new chat,
    // model switch, endpoint without usage reporting) the meter falls
    // back to a byte heuristic over the stored history and renders with
    // a "~" prefix.  The accounting always runs regardless of the
    // Settings toggle — the toggle controls widget visibility only —
    // so enabling the meter mid-conversation shows an exact value
    // immediately.
    //
    // Known bounded staleness (accepted for v1): a tool-protocol flip
    // changes the next request's shape by the tools-catalog size, and
    // hidden skill control turns do not update the anchor; both
    // self-correct on the next transcript turn.
    wxStaticText* _ctxMeter = nullptr;
    long long m_ctxAnchorPromptTokens     = -1;   // -1 = no anchor
    long long m_ctxAnchorCompletionTokens = 0;
    bool      m_ctxAnchorExact            = false;
    // ChatHistory revision at anchor adoption; lets the path-change
    // callback tell a first save / rename (same transcript, keep the
    // anchor) from a load / New Chat (revision reset, drop it).
    std::uint64_t m_ctxAnchorRevision     = 0;
    // Fallback occupancy when the endpoint reports no usage: a size-based
    // estimate of the history, re-priced whenever the history changes
    // (keyed on object identity + ChatHistory revision) rather than only at
    // New Chat / load, which left it stuck at the value from reset time.
    // Mutable: filled lazily by the const ComputeContextUsage().
    mutable long long           m_ctxHistoryEstimateTokens   = 0;
    mutable bool                m_ctxFallbackWouldElide      = false;   // fallback estimate hit the elision cap
    mutable const ChatHistory*  m_ctxEstimateHistory         = nullptr;
    mutable std::uint64_t       m_ctxEstimateRevision        = 0;
    std::string m_ctxMeterHistoryPath;             // conversation-identity tracker
    std::string m_ctxCalibHeaderCheckedPath;       // ctx_calibration.tsv whose last header is current
    // Timings / speeds of the last completed transcript reply in this
    // window (turn_stats.h).  Empty until the first reply; cleared with
    // the context anchor on New Chat / load / model switch.
    TurnStats m_lastTurnStats;
    // Context details panel (click the ctx meter).  History of this chat's
    // replies since the last anchor reset, and the request breakdown that
    // produced the last reply -- captured together so the panel's
    // per-section split always pairs with that reply's exact prompt count.
    std::vector<TurnStats> m_chatTurnStats;
    RequestBreakdown       m_lastRequestBreakdown;
    ContextHud*            m_contextHud = nullptr;
    // Windows only: re-checks every 250 ms whether the panel should be
    // visible (see SyncContextHudVisibility).
    wxTimer                m_contextHudVisTimer;
    // New Chat prompt-cache pre-warm (prompt_prewarm.h).  One-shot,
    // debounced so New Chat -> immediately open an old chat primes nothing.
    wxTimer                m_prewarmTimer;
    std::string            m_prewarmReason;
    bool                   m_ctxMeterHover = false;
    wxString  m_ctxMeterLastTooltip;
    wxString  m_ctxMeterLastLabel;                // skip redundant SetLabel churn

    // ─── Thread safety ────────────────────────────────────────────
    std::shared_ptr<std::atomic<bool>> m_alive;
    unsigned long m_generationId;

    // ─── Application Components ───────────────────────────────────
    // Non-owning: AppState is owned by MyApp (Chunk C) — one instance,
    // one wxFileConfig writer, shared by every window.  Initialized in
    // the ctor init list from wxGetApp(), so it is valid for the
    // frame's whole lifetime; MyApp's member order guarantees AppState
    // outlives ModelService, which outlives every frame.
    AppState*                      m_appState;
    std::unique_ptr<ChatClient>    m_chatClient;
    // /bench runner (own ChatClient; never touches chat history).
    std::unique_ptr<BenchController> m_bench;
    std::unique_ptr<ChatDisplay>   m_chatDisplay;
    std::unique_ptr<ChatHistory>   m_chatHistory;
    std::unique_ptr<AttachmentManager> m_attachments;
    // Non-owning: the ModelService (and the ServerManager inside it)
    // is owned by MyApp and exists before any frame does (Chunk C) —
    // borrowed in InitializeCoreServices, stable for the frame's
    // whole lifetime.
    ModelService* m_modelService = nullptr;
    std::unique_ptr<CmdExecutor>   m_cmdExecutor;
    std::unique_ptr<PythonRunner>  m_pythonRunner;
    std::unique_ptr<PythonSessionManager> m_pySessionManager;
    std::unique_ptr<GrepExecutor>  m_grepExecutor;
    std::unique_ptr<WebFetchExecutor> m_webFetchExecutor;
    std::unique_ptr<ToolWorkerExecutor> m_toolWorker;
    std::unique_ptr<WaitExecutor>  m_waitExecutor;

    // ─── Coordinators ────────────────────────────────────────────
    std::unique_ptr<ModelSwitcher>          m_modelSwitcher;
    std::unique_ptr<ConversationController> m_convController;
    std::unique_ptr<ProjectContextBuilder>  m_projectContextBuilder;
    std::unique_ptr<ToolResultController>   m_toolResultController;
    std::unique_ptr<AgentController>        m_agentController;
    std::unique_ptr<SkillDraftController>   m_skillDraftController;
    std::unique_ptr<ProjectController>      m_projectController;
    std::unique_ptr<DropImportController>   m_dropImportController;

    // Project status strip — replaces the native menu bar; renders
    // current project state in a single line under the top toolbar.
    std::unique_ptr<ProjectStatusStrip>     m_projectStrip;
    std::unique_ptr<ActivityStrip>          m_activityStrip;
    // Main-thread operation identity. Begin AND End invalidate a modal
    // Stop confirmation, even if another tool starts before it returns.
    std::uint64_t m_activityRevision = 0;
    // Filled by the synchronous setup tool; consumed after the agent loop ends.
    std::string m_pendingSetupModel;

    // Agent mode — when true, the next user message begins an
    // agent loop via m_agentController->Begin().  Toggled by the
    // agent button on the input area.
    bool m_agentModeEnabled;

    // Lazy model loading (paired with ModelSwitcher deferred-model helpers).
    // Opening a saved conversation does not reload its model immediately;
    // the model is parked and loaded on the first Send.  If the user hits
    // Send before that model is ready, the typed prompt is stashed here and
    // fired automatically from OnServerReady once the matching model finishes
    // loading.  |modelPath| records which model the prompt was queued under,
    // so a model switch between queueing and ready drops the prompt instead
    // of misdirecting it.
    //
    // The composer is the single source of truth for the queued text.  The
    // prompt is NOT copied out of the input box: it stays there, visibly,
    // until the matching model is ready and it is sent through the normal
    // path.  Every cancel site therefore just clears the flag — the user's
    // text is never lost, and no cancel path has to explain what it dropped.
    struct PendingSend {
        bool        active = false;
        // Model is ready but tool-protocol detection has not resolved yet.
        // The prompt fires from OnToolProtocolDetected (or the protocol
        // wait timer) so the first request is built with the real protocol.
        bool        awaitingProtocol = false;
        std::string modelPath;
    };
    PendingSend m_pendingSend;

    // Safety net: if detection never reports (probe hang), fall back to
    // XML and send anyway rather than leave the prompt stuck "Queued".
    wxTimer m_pendingSendProtocolTimer{this, ID_PENDING_SEND_PROTOCOL_TIMER};
    static constexpr int kPendingSendProtocolTimeoutMs = 15000;

    // Flip the composer into / out of "queued behind a model load" state.
    void SetPendingSendUi(bool queued)
    {
        if (!_sendButton) return;
        _sendButton->SetLabel(queued ? "Queued" : "Send");
        _sendButton->Enable(!queued);
        if (auto* parent = _sendButton->GetParent()) parent->Layout();
    }

    void ClearPendingSend()
    {
        if (m_pendingSendProtocolTimer.IsRunning())
            m_pendingSendProtocolTimer.Stop();
        if (!m_pendingSend.active) return;
        m_pendingSend = PendingSend{};
        SetPendingSendUi(false);
    }

    // Fire the prompt that was queued behind a model load.  Caller has
    // already verified the ready model matches m_pendingSend.modelPath
    // and that the tool protocol is resolved (or deliberately defaulted).
    void FlushPendingSend()
    {
        if (!m_pendingSend.active) return;
        ClearPendingSend();

        if (IsBusy() || HasPendingApproval()) {
            m_chatDisplay->DisplaySystemNotice(
                "Another operation started before the model finished "
                "loading. Your message is still in the input box.");
            return;
        }

        // Read the composer *now*: any edits the user made while
        // waiting are exactly what should go out.
        std::string queued = WxToUtf8(_userInputCtrl->GetValue());
        const size_t firstNonWs = queued.find_first_not_of(" \t\r\n");
        if (firstNonWs == std::string::npos) queued.clear();
        else if (firstNonWs > 0)             queued.erase(0, firstNonWs);
        DispatchUserTurn(queued);
    }

    void OnPendingSendProtocolTimeout(wxTimerEvent&)
    {
        if (m_isClosing) return;
        if (!m_pendingSend.active || !m_pendingSend.awaitingProtocol) return;

        if (_activeProtocol == ToolProtocol::Unknown) {
            _activeProtocol = ToolProtocol::Xml;
            if (_protocolChip) UpdateProtocolChip(ToolProtocol::Xml);
            if (auto* logger = m_appState->GetLogger())
                logger->warning(
                    "Tool protocol detection did not report in time for a "
                    "queued prompt - sending with xml");
        }
        FlushPendingSend();
    }

    void CancelPendingSendForConversationSwitch()
    {
        ClearPendingSend();
        if (m_skillDraftController)
            m_skillDraftController->CancelForChatSwitch(/*notifyUser=*/false);
    }


    // Slash-command approval state.  Agent approvals live inside
    // AgentController because native tool_call_id threading must
    // remain with the loop.  Slash approvals live here because MyFrame
    // owns slash rendering, persistence, and async state.
    struct PendingSlashApproval {
        ToolInvocation invocation;
        ToolContext    context;
        std::string    writeRootGrant;
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
    //  AgentEventSink implementation
    // ═════════════════════════════════════════════════════════════
    //
    // The agent loop reports progress through these hooks instead of
    // reaching into ChatDisplay or invoking UI lambdas, so the
    // controller stays UI-free.

    std::filesystem::path NewAgentTracePath()
    {
        const std::string root = ServerManager::GetLogsDir() +
            std::string(1, wxFILE_SEP_PATH) + "agent_traces";
        std::filesystem::path rootPath = LbUtf8FsPath(root);

        std::error_code ec;
        std::filesystem::create_directories(rootPath, ec);

#ifdef _WIN32
        const unsigned long pid = static_cast<unsigned long>(::GetCurrentProcessId());
#else
        const unsigned long pid = 0;
#endif

        ++m_agentTraceSequence;
        const std::string filename =
            "agent_" + LbTraceTimestampForFilename() +
            "_p" + std::to_string(pid) +
            "_" + std::to_string(m_agentTraceSequence) + ".jsonl";

        return rootPath / filename;
    }

    void AppendAgentTraceEvent(const AgentEvent& event)
    {
        if (event.type == AgentEventType::LoopBegin) {
            if (m_agentTraceStream.is_open()) {
                m_agentTraceStream.close();
            }

            m_agentTracePath = NewAgentTracePath();
            m_agentTraceEventIndex = 0;
            m_agentTraceToolInFlight = false;
            m_agentTraceActiveToolName.clear();
            m_agentTraceActiveSignature.clear();
            m_agentTraceActiveCallId.clear();

            m_agentTraceStream.open(
                m_agentTracePath,
                std::ios::out | std::ios::binary);
            if (!m_agentTraceStream) {
                m_agentTracePath.clear();
                return;
            }
        }

        if (m_agentTracePath.empty() || !m_agentTraceStream.is_open()) return;

        const auto nowSteady = std::chrono::steady_clock::now();
        long long durationMs = -1;

        const bool isTerminalToolResult =
            event.type == AgentEventType::ToolOutput ||
            event.type == AgentEventType::Error ||
            event.type == AgentEventType::FileCreated ||
            event.type == AgentEventType::EditApplied ||
            event.type == AgentEventType::DirectoryCreated ||
            event.type == AgentEventType::FileDeleted;

        if (isTerminalToolResult && m_agentTraceToolInFlight) {
            durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                nowSteady - m_agentTraceToolStartedAt).count();
            m_agentTraceToolInFlight = false;
        }

        std::ostream& out = m_agentTraceStream;

        out << "{"
            << "\"ts\":\"" << LbJsonEscape(LbUtcTimestampForJson()) << "\""
            << ",\"seq\":" << (++m_agentTraceEventIndex)
            << ",\"event\":\"" << LbAgentEventTypeName(event.type) << "\"";

        if (event.type == AgentEventType::ToolCall) {
            out << ",\"tool\":\"" << LbJsonEscape(event.toolName) << "\""
                << ",\"tool_call_id\":\"" << LbJsonEscape(event.toolCallId) << "\""
                << ",\"signature\":\"" << LbJsonEscape(event.toolSignature) << "\""
                << ",\"command_echo\":\"" << LbJsonEscape(LbJsonPreview(event.commandEcho)) << "\"";

            m_agentTraceToolInFlight = true;
            m_agentTraceToolStartedAt = nowSteady;
            m_agentTraceActiveToolName = event.toolName;
            m_agentTraceActiveSignature = event.toolSignature;
            m_agentTraceActiveCallId = event.toolCallId;
        }

        if (event.type == AgentEventType::ApprovalRequired ||
            event.type == AgentEventType::AgentStatus ||
            isTerminalToolResult) {
            const ToolBlock& b = event.toolBlock;
            out << ",\"tool\":\"" << LbJsonEscape(b.toolName) << "\""
                << ",\"command_echo\":\"" << LbJsonEscape(LbJsonPreview(b.commandEcho)) << "\""
                << ",\"body_bytes\":" << b.body.size()
                << ",\"error_bytes\":" << b.errorBody.size()
                << ",\"presented_files\":" << b.presentedFiles.size()
                << ",\"requires_approval\":" << (b.requiresApproval ? "true" : "false")
                << ",\"start_expanded\":" << (event.startExpanded ? "true" : "false");

            out << ",\"chips\":[";
            for (size_t i = 0; i < b.statusChips.size(); ++i) {
                if (i) out << ",";
                out << "\"" << LbJsonEscape(b.statusChips[i]) << "\"";
            }
            out << "]";

            if (durationMs >= 0) {
                out << ",\"duration_ms\":" << durationMs
                    << ",\"dispatch_tool\":\"" << LbJsonEscape(m_agentTraceActiveToolName) << "\""
                    << ",\"dispatch_signature\":\"" << LbJsonEscape(m_agentTraceActiveSignature) << "\""
                    << ",\"dispatch_tool_call_id\":\"" << LbJsonEscape(m_agentTraceActiveCallId) << "\"";

                m_agentTraceActiveToolName.clear();
                m_agentTraceActiveSignature.clear();
                m_agentTraceActiveCallId.clear();
            }
        }

        if (event.type == AgentEventType::TurnComplete) {
            out << ",\"message_bytes\":" << event.userFacingMessage.size();
        }

        if (event.type == AgentEventType::LoopEnd) {
            out << ",\"reason\":\"" << LbAgentEndReasonName(event.endReason) << "\""
                << ",\"message_bytes\":" << event.userFacingMessage.size();
        }

        out << "}\n";
        m_agentTraceStream.flush();

        if (event.type == AgentEventType::LoopEnd) {
            m_agentTraceStream.close();
            m_agentTracePath.clear();
            m_agentTraceToolInFlight = false;
            m_agentTraceActiveToolName.clear();
            m_agentTraceActiveSignature.clear();
            m_agentTraceActiveCallId.clear();
        }
    }

    // Tees the typed AgentEvent stream to a per-loop JSONL trace
    // before the default sink bridge fans events back out to the UI
    // callbacks.
    void OnAgentEvent(const AgentEvent& event) override
    {
        AppendAgentTraceEvent(event);
        AgentEventSink::OnAgentEvent(event);
    }

    // No loop-scoped UI state today.  The user's message is on
    // screen and the first chat request is in flight by the time
    // Begin() runs, so there's nothing to set up here.  Hook is
    // kept for future loop-scoped indicators (a "thinking…" status,
    // a Stop-button enable, etc.).
    void OnAgentLoopBegin() override
    {
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
            ServerManager::ModelDisplayName(
                m_modelSwitcher->GetConversationModelForSave()),
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
        m_chatDisplay->DisplayToolBlock(block, startExpanded);
    }

    // Agent approval pauses the loop before the risky tool runs.  The
    // card is UI-only; the Allow Once / Allow Always / Deny buttons (or
    // the typed-command fallback) resolve the pending invocation held
    // by AgentController.
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

        const std::string model =
            m_modelSwitcher->GetConversationModelForSave();
        m_chatDisplay->DisplayAssistantMessage(
            ServerManager::ModelDisplayName(model),
            message,
            m_appState->GetTheme().chatAssistant);

        if (m_chatHistory->HasAssistantPlaceholder())
            m_chatHistory->UpdateLastAssistantMessage(message);
        else
            m_chatHistory->AddAssistantMessage(message, model);
    }


    // Loop ended for any reason.  If the controller supplied a
    // user-facing message (cancel/iter-cap/malformed-cap/send-fail/
    // tool-failed-stop cases), surface it as a system message before we finalize.
    // Normal and StreamError both arrive with empty messages —
    // Normal because the model's final answer is the message, and
    // StreamError because OnAssistantError already showed friendly
    // error text before unwinding the loop.
    void OnAgentLoopEnd(AgentEndReason     reason,
                        const std::string& userFacingMessage) override
    {
        if (!userFacingMessage.empty()) {
            m_chatDisplay->DisplaySystemMessage(userFacingMessage);
        }

        m_chatClient->ResetStreamingState();
        ResetAgentToolStreamFilter();
        SetStreamingState(false);
        m_chatDisplay->ClearFilePersistenceContext();
        if (!m_chatHistory->IsEmpty())
            m_convController->AutoSaveConversation();

        if (!m_pendingSetupModel.empty()) {
            const std::string modelToUse = std::move(m_pendingSetupModel);
            m_pendingSetupModel.clear();
            if (!m_isClosing && reason == AgentEndReason::Normal) {
                // Streaming state and agent activity are now reset. Use the same
                // switch path as the model picker, including multi-window checks.
                m_modelSwitcher->SwitchToModel(modelToUse);
                _userInputCtrl->SetFocus();
            }
            return;
        }
    }
    // ─── Chat state machine ──────────────────────────────────────
    ChatState m_chatState;

    // ── ASCII Animation ──────────────────────────────────────────
    wxTimer                          m_animTimer{this, ID_ANIMATION_TIMER};
    // Sweeps idle Python sessions every minute; a session is reaped
    // after PythonSessionManager::kIdleReapAfterMs without an exec.
    wxTimer                          m_pyReapTimer{this, ID_PY_SESSION_REAP_TIMER};
    std::unique_ptr<AsciiAnimation>  m_activeAnimation;

    // Assistant streaming delta batcher.  The worker still posts deltas as
    // quickly as llama-server emits them, but the UI/history path receives
    // combined chunks at most about once per frame.
    wxTimer       m_assistantDeltaFlushTimer{this, ID_ASSISTANT_DELTA_FLUSH_TIMER};
    std::string   m_pendingAssistantDelta;
    unsigned long m_pendingAssistantDeltaGenerationId = 0;

    // Option 1 reminder delivery. Store state is shared process-wide; each
    // frame may tick, but ReminderStore::ClaimDue() guarantees only one window
    // presents a given reminder.
    wxTimer       m_reminderTimer{this, ID_REMINDER_TIMER};

    // Agent trace logging: one JSONL file per AgentController::Begin(),
    // written under the normal LlamaBoss logs directory.  It is intentionally
    // additive and non-fatal: if the log path cannot be created/opened, the
    // agent/UI continue normally.
    std::filesystem::path m_agentTracePath;
    std::ofstream         m_agentTraceStream;
    std::uint64_t         m_agentTraceSequence = 0;
    std::uint64_t         m_agentTraceEventIndex = 0;
    bool                  m_agentTraceToolInFlight = false;
    std::chrono::steady_clock::time_point m_agentTraceToolStartedAt{};
    std::string           m_agentTraceActiveToolName;
    std::string           m_agentTraceActiveSignature;
    std::string           m_agentTraceActiveCallId;
    std::string           m_chatFolderEnsuredForFilePath;
    std::string           m_workspaceDirEnsuredForFilePath;

    // ═════════════════════════════════════════════════════════════
    //  HELPERS
    // ═════════════════════════════════════════════════════════════

    // Glyph-only hover for a flat icon button: no background tile, just
    // the glyph colour, like the ctx meter and [ Skills ] label.
    // `restingBg` is unused (kept for call-site compatibility).
    // With tintGlyph a text glyph switches to the interactive accent while
    // hovered; without it (the SVG icons) SetBitmapCurrent() does the tint
    // and the helper only forces the repaint.
    void BindIconHover(wxButton* button, wxColour ThemeData::* /*restingBg*/,
                       bool tintGlyph)
    {
        std::function<void(bool)> glyph;
        if (tintGlyph) {
            glyph = [this, button](bool hovered) {
                const ThemeData& t = m_appState->GetTheme();
                button->SetForegroundColour(
                    hovered ? LbInteractiveAccentForTheme(t) : t.textMuted);
            };
        }
        LbHoverTile::BindGlyph(button, glyph);
    }

    void ApplyThemeToUI()
    {
        const ThemeData& t = m_appState->GetTheme();
        if (m_contextHud) m_contextHud->ApplyTheme(t);

        SetBackgroundColour(t.bgMain);

        _toolbarPanel->SetBackgroundColour(t.bgToolbar);
        _sidebarToggle->SetBackgroundColour(t.bgToolbar);
        _sidebarToggle->SetForegroundColour(t.textMuted);
        _titleLabel->SetForegroundColour(t.textPrimary);
        _modelPill->SetBackgroundColour(t.bgToolbar);
        _modelLabel->SetForegroundColour(t.textPrimary);
        if (_thinkingChip) {
            _thinkingChip->SetForegroundColour(t.textMuted);
            _thinkingChip->Refresh();
        }
        if (_modelPillLeftBracket)  _modelPillLeftBracket->SetForegroundColour(t.textMuted);
        if (_modelPillRightBracket) _modelPillRightBracket->SetForegroundColour(t.textMuted);
        if (_protocolChip) UpdateProtocolChip(_activeProtocol);
        _newChatButton->SetBackgroundColour(t.bgToolbar);
        _newChatButton->SetForegroundColour(t.textMuted);
        LbIcons::ApplyFlatButton(_settingsButton, LbIcons::Id::Settings,
                                 t, t.bgToolbar);
        _aboutButton->SetBackgroundColour(t.bgToolbar);
        _aboutButton->SetForegroundColour(t.textMuted);
        _topSeparator->SetBackgroundColour(t.borderSubtle);
        _statusDot->SetColors(t.accentButton, t.textMuted);
        if (_ctxMeter) {
            // Force a full reapply: the label-change guard would other-
            // wise skip the fg/bg repaint when the text is unchanged.
            m_ctxMeterLastLabel.clear();
            RefreshContextMeter();
        }

        if (m_projectStrip) m_projectStrip->ApplyTheme(t);
        if (m_activityStrip) m_activityStrip->ApplyTheme(t);
        RefreshProjectStrip();

        if (m_sidebar) m_sidebar->ApplyTheme(t);

        _rightPanel->SetBackgroundColour(t.bgMain);
        _chatDisplayCtrl->SetBackgroundColour(t.bgMain);
        _chatDisplayCtrl->SetForegroundColour(t.textPrimary);
        if (m_chatRail) m_chatRail->ApplyTheme();   // also recolours _chatClip
        _attachChipBar->SetBackgroundColour(t.bgMain);
        RebuildAttachmentChips();

        _inputContainer->SetBackgroundColour(t.bgInputArea);
        _inputSeparator->SetBackgroundColour(t.borderSubtle);
        LbIcons::ApplyFlatButton(_attachButton, LbIcons::Id::Attach,
                                 t, t.bgInputArea);
        LbIcons::ApplyAgentToggle(_agentToggleButton, t, m_agentModeEnabled);
        _userInputCtrl->SetBackgroundColour(t.bgInputField);
        _userInputCtrl->SetForegroundColour(t.textPrimary);
        if (m_inputRail) m_inputRail->ApplyTheme();  // also recolours the clip
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

    // ── Composer attachment cards ──────────────────────────
    //
    // The pending-attachment strip shows a thumbnail card per queued
    // image and a labelled file card for everything else.  Painting
    // lives in AttachmentChip; the frame's job is turning a
    // PendingAttachment into an AttachmentChipModel and keeping the
    // decode off the repaint path.

    static std::string ChipThumbKey(const PendingAttachment& item)
    {
        // Name + payload length + a 24-byte payload prefix separates two
        // same-named pastes.  Hashing 40 MB of base64 would cost more
        // than the decode the cache exists to avoid.
        return item.name + "|" + std::to_string(item.data.size()) + "|" +
               item.data.substr(0, std::min<size_t>(24, item.data.size()));
    }

    // Decode a pending image's base64 payload at full resolution.
    // Returns an invalid image for anything wx can't parse.
    static wxImage DecodePendingImage(const PendingAttachment& item)
    {
        std::string raw;
        try {
            std::istringstream b64(item.data);
            Poco::Base64Decoder decoder(b64);
            raw.assign(std::istreambuf_iterator<char>(decoder),
                       std::istreambuf_iterator<char>());
        } catch (...) {
            return wxImage();
        }
        if (raw.empty()) return wxImage();

        wxImage img;
        {
            // A corrupt or exotic payload is a fallback, not a dialog.
            wxLogNull quiet;
            wxMemoryInputStream ms(raw.data(), raw.size());
            if (!img.LoadFile(ms, wxBITMAP_TYPE_ANY) || !img.IsOk())
                return wxImage();
        }
        return img;
    }

    // Decode a pending image's base64 payload into a small preview
    // (longest side <= 240 px).  Returns an invalid image for anything
    // wx can't parse -- the chip falls back to a file card on its own.
    wxImage GetChipThumbnail(const PendingAttachment& item)
    {
        const std::string key = ChipThumbKey(item);
        if (auto it = m_chipThumbCache.find(key); it != m_chipThumbCache.end())
            return it->second;

        wxImage img = DecodePendingImage(item);
        if (!img.IsOk()) return wxImage();

        const int maxSide = 240;
        const int w = img.GetWidth(), h = img.GetHeight();
        if (w > maxSide || h > maxSide) {
            const double s = std::min((double)maxSide / w, (double)maxSide / h);
            img.Rescale(std::max(1, (int)(w * s)),
                        std::max(1, (int)(h * s)), wxIMAGE_QUALITY_HIGH);
        }

        if (m_chipThumbCache.size() > 24) m_chipThumbCache.clear();
        m_chipThumbCache[key] = img;
        return img;
    }

    static std::string ChipKindLabel(const PendingAttachment& item)
    {
        switch (item.type) {
            case PendingAttachment::Type::Image:           return "Image";
            case PendingAttachment::Type::PdfFile:         return "PDF document";
            case PendingAttachment::Type::SpreadsheetFile: return "Spreadsheet";
            case PendingAttachment::Type::DocxFile:        return "Word document";
            case PendingAttachment::Type::CsvFile:         return "CSV data";
            case PendingAttachment::Type::ZipFile:         return "ZIP archive";
            case PendingAttachment::Type::TextFileRef:     return "Text file (workspace)";
            case PendingAttachment::Type::TextFile:        return "Text file";
        }
        return "File";
    }

    // Click on a pending image card → full-size lightbox, same viewer
    // the transcript thumbnails use.  Decoded fresh at full resolution
    // (the chip cache only holds 240 px previews).  The index is
    // re-validated because this runs via CallAfter.
    void OpenPendingImageViewer(size_t idx)
    {
        if (!m_attachments || idx >= m_attachments->GetCount()) return;

        const PendingAttachment& item = m_attachments->GetAt(idx);
        if (item.type != PendingAttachment::Type::Image) return;

        const wxString name = wxString::FromUTF8(item.name);
        wxImage full;
        {
            wxBusyCursor busy;   // a 40 MB paste takes a beat to decode
            full = DecodePendingImage(item);
        }
        if (!full.IsOk()) { wxBell(); return; }

        const ThemeData& t = m_appState->GetTheme();
        LbShowImageLightbox(*this, std::move(full),
                            t.bgMain, t.textPrimary, name);
    }

    void RebuildAttachmentChips()
    {
        _attachChipSizer->Clear(true);

        if (!m_attachments->HasPending()) {
            m_chipThumbCache.clear();
            _attachChipBar->Hide();
            LayoutInputAreaOnly();
            return;
        }

        const ThemeData& t = m_appState->GetTheme();

        for (size_t i = 0; i < m_attachments->GetCount(); ++i) {
            const auto& item = m_attachments->GetAt(i);

            AttachmentChipModel model;
            model.name      = item.name;
            model.kindLabel = ChipKindLabel(item);
            model.byteSize  = item.originalSize;
            model.isImage   = (item.type == PendingAttachment::Type::Image);
            if (model.isImage) model.preview = GetChipThumbnail(item);

            auto* chip = new AttachmentChip(
                _attachChipBar, i, std::move(model), t,
                [this](size_t idx) { m_attachments->RemoveAt(idx); },
                [this](size_t idx) { OpenPendingImageViewer(idx); });

            _attachChipSizer->Add(chip, 0, wxRIGHT | wxBOTTOM, FromDIP(8));
        }

        _attachChipBar->Show();
        _attachChipBar->Layout();

        // The strip is a sibling immediately above the composer.  Re-layout
        // the right panel so its newly visible height is taken from the chat
        // display without changing the message row itself.
        LayoutInputAreaOnly();
    }

    void SetStreamingState(bool streaming)
    {
        // Most callers set a more specific busy state before toggling the UI
        // (RunningPython, RunningCmd, RunningGrep, RunningWebFetch, or
        // Streaming).  This backstop prevents any future UI-only true toggle
        // from accidentally leaving the frame logically Idle while work is
        // still in flight.
        if (streaming && m_chatState == ChatState::Idle) {
            m_chatState = ChatState::Streaming;
        }

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

    bool IsBusy() const
    {
        return m_chatState != ChatState::Idle ||
               (m_bench && m_bench->IsRunning()) ||
               (m_convController && m_convController->IsSaveRecoveryActive()) ||
               (m_agentController && m_agentController->IsActive());
    }

    static bool ThinkingModeAtSelection(int selection, ChatHistory::ThinkOverride& mode)
    {
        using Think = ChatHistory::ThinkOverride;
        switch (selection) {
            case 0: mode = Think::Auto;   return true;
            case 1: mode = Think::Off;    return true;
            case 2: mode = Think::On;     return true;
            case 3: mode = Think::Low;    return true;
            case 4: mode = Think::Medium; return true;
            case 5: mode = Think::High;   return true;
            default: return false;
        }
    }

    static const char* ThinkingModeName(ChatHistory::ThinkOverride mode)
    {
        using Think = ChatHistory::ThinkOverride;
        switch (mode) {
            case Think::On:     return "on";
            case Think::Off:    return "off";
            case Think::Low:    return "low";
            case Think::Medium: return "medium";
            case Think::High:   return "high";
            default:           return "auto";
        }
    }

    bool ConversationRequiresReasoning()
    {
        if (!m_modelSwitcher) return false;
        const InferenceTarget target =
            m_modelSwitcher->ResolveTargetForConversation();
        return !target.managed && !target.imageOutput &&
            lb_reasoning::RequiresReasoning(target.modelId,
                target.reasoningDialect == ReasoningDialect::OpenAIStyle);
    }

    // Fills a menu with the six thinking modes as radio items, the
    // current mode checked, and binds a handler that applies the pick.
    // Used both by the chip's own popup and as the "Thinking" submenu of
    // the model picker (the submenu handler runs before the parent
    // menu's, and does not Skip, so the model picker never sees it).
    void BuildThinkingMenu(wxMenu& menu)
    {
        static const char* labels[] = { "Auto (model default)", "Off", "On",
                                        "Low", "Medium", "High" };
        std::vector<int> ids(6, wxID_NONE);
        const ChatHistory::ThinkOverride current = m_chatHistory
            ? m_chatHistory->GetThinkOverride()
            : ChatHistory::ThinkOverride::Auto;
        const bool requiresReasoning = ConversationRequiresReasoning();
        for (int selection = 0; selection < 6; ++selection) {
            auto* item = menu.AppendRadioItem(wxID_ANY,
                wxString::FromUTF8(selection == 1 && requiresReasoning
                    ? "Off (unavailable for this model)" : labels[selection]));
            ids[selection] = item->GetId();
            ChatHistory::ThinkOverride mode;
            if (ThinkingModeAtSelection(selection, mode))
                item->Check(mode == current);
            if (selection == 1 && requiresReasoning) item->Enable(false);
        }
        menu.Bind(wxEVT_MENU, [this, ids](wxCommandEvent& event) {
            if (!m_chatHistory || m_isClosing || IsBusy()) return;
            for (int selection = 0; selection < 6; ++selection) {
                ChatHistory::ThinkOverride mode;
                if (event.GetId() == ids[selection] &&
                    ThinkingModeAtSelection(selection, mode)) {
                    SetConversationThinking(mode);
                    return;
                }
            }
        });
    }

    void RefreshThinkingSelector()
    {
        if (!_thinkingChip || !m_chatHistory) return;
        wxString mode = wxString::FromUTF8(
            ThinkingModeName(m_chatHistory->GetThinkOverride()));
        mode = mode.Left(1).Upper() + mode.Mid(1);
        // "\xC2\xB7" == U+00B7 middle dot, the pill's segment separator.
        const wxString label = wxString::FromUTF8("\xC2\xB7 ") + mode;
        if (_thinkingChip->GetLabel() != label) {
            _thinkingChip->SetLabel(label);
            _thinkingChip->Refresh();
            // Width changed: reflow the pill and re-center it in the bar.
            if (auto* parent = _thinkingChip->GetParent()) {
                parent->Layout();
                if (auto* grand = parent->GetParent()) grand->Layout();
            }
        }
    }

    void SetConversationThinking(ChatHistory::ThinkOverride mode)
    {
        if (!m_chatHistory) return;
        if (mode == ChatHistory::ThinkOverride::Off &&
            ConversationRequiresReasoning()) {
            m_chatDisplay->DisplaySystemMessage(
                "This model requires reasoning. Choose Low for the lowest "
                "effort, or Auto to use the model's default. "
                "The current thinking setting has not changed.");
            return;
        }
        m_chatHistory->SetThinkOverride(mode);
        RefreshThinkingSelector();
        m_chatDisplay->DisplaySystemMessage(
            std::string("Thinking set to ") + ThinkingModeName(mode) +
            " for this conversation. Applies to the next message." +
            (mode == ChatHistory::ThinkOverride::Auto
                ? " Using the model's default." : ""));
        // Reuse normal persistence; an empty chat remains unsaved until it
        // has content, and then carries its selected mode with its snapshot.
        if (m_convController) m_convController->AutoSaveConversation();
    }

    static bool IsPythonAsyncToolName(const std::string& toolName)
    {
        if (toolName == tool_names::kGrep ||
            toolName == tool_names::kPowerShell ||
            toolName == tool_names::kWebFetchUrl) {
            return false;
        }

        const ToolSpec* spec = GetGlobalRouter().Find(toolName);
        return spec && spec->safety.isAsync;
    }

    static ChatState ChatStateForAsyncToolName(const std::string& toolName)
    {
        if (toolName == tool_names::kGrep) {
            return ChatState::RunningGrep;
        }
        if (toolName == tool_names::kPowerShell) {
            return ChatState::RunningCmd;
        }
        if (toolName == tool_names::kWebFetchUrl) {
            return ChatState::RunningWebFetch;
        }
        if (IsPythonAsyncToolName(toolName)) {
            return ChatState::RunningPython;
        }

        // Defensive fallback: DispatchInvocation already verified that this
        // tool is registered as async.  Even if a future async tool is not
        // added to the explicit mapping above, never leave the frame Idle
        // while its worker is in flight.
        assert(false && "Async tool has no ChatState mapping.");
        return ChatState::RunningCmd;
    }

    // ═════════════════════════════════════════════════════════════
    //  EVENT HANDLERS
    // ═════════════════════════════════════════════════════════════

    void OnToggleAgentMode(wxCommandEvent&)
    {
        if (IsBusy()) return;  // ignore toggle while something's running
        m_agentModeEnabled = !m_agentModeEnabled;
        LbIcons::ApplyAgentToggle(_agentToggleButton, m_appState->GetTheme(), m_agentModeEnabled);
        m_chatDisplay->DisplaySystemMessage(
            m_agentModeEnabled
              ? "\xF0\x9F\xA4\x96 Agent mode ON. The model can use read/ls/open/grep/pwd/powershell."
              : "\xF0\x9F\xA4\x96 Agent mode OFF.");
        if (m_agentModeEnabled) SchedulePromptPrewarm("agent mode on");
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
        // CSV lives under Spreadsheets — it routes through
        // QueueCsvAttachmentFromDrop (workspace import + csv_inspect
        // hint), NOT through AttachTextFile.
        const wxString filter =
            "All supported files"
            "|*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.webp;"
             "*.pdf;*.xlsx;*.docx;*.csv;*.zip;"
             "*.txt;*.md;*.json;*.cpp;*.h;*.hpp;*.py;*.js;*.ts;*.jsx;*.tsx;"
             "*.css;*.html;*.xml;*.yaml;*.yml;*.toml;*.log;*.ini;*.cfg;"
             "*.sh;*.bat;*.rs;*.go;*.java;*.kt;*.swift;*.rb;*.php;*.sql;"
             "*.dockerfile;.env;.gitignore"
            "|Image files (*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.webp)"
            "|*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.webp"
            "|PDF documents (*.pdf)|*.pdf"
            "|Word documents (*.docx)|*.docx"
            "|Spreadsheets (*.xlsx;*.csv)|*.xlsx;*.csv"
            "|Archives (*.zip)|*.zip"
            "|Text & code files"
            "|*.txt;*.md;*.json;*.cpp;*.h;*.hpp;*.py;*.js;*.ts;*.jsx;*.tsx;"
             "*.css;*.html;*.xml;*.yaml;*.yml;*.toml;*.log;*.ini;*.cfg;"
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
        // pdf -> IsSpreadsheetFile (xlsx) -> docx -> IsCsvFile -> IsImageFile
        // -> IsTextFile -> unsupported.  PDF/XLSX/DOCX/CSV route through the
        // same Queue* helpers the drop path uses, so the click and drag
        // paths share the cwd-copy logic, the 100 MB cap, and the
        // system-message feedback.  IsCsvFile MUST be tested before
        // IsTextFile.  .docm is intentionally not auto-routed here --
        // matches the drop-target gate.
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
            else if (AttachmentManager::IsCsvFile(pathUtf8)) {
                ok = QueueCsvAttachmentFromDrop(pathUtf8);
            }
            else if (AttachmentManager::IsZipFile(pathUtf8)) {
                ok = QueueZipAttachmentFromDrop(pathUtf8);
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
                "DOCX, ZIP, and text/code files (txt, md, json, cpp, h, py, "
                "js, csv, etc.).",
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

        // Hidden control turns are deliberately invisible: they have no
        // assistant placeholder in the user-visible transcript, so streamed
        // Skill-draft deltas must not append onto the previous assistant reply.
        if (m_skillDraftController && m_skillDraftController->AnyHiddenTurnInFlight()) return;

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

    // ── Generated images (image-output models) ───────────────────
    // Decode the base64 data URLs an image model returned, persist
    // them under the conversation's artifacts folder, attach the
    // chat-folder-relative paths to the assistant message (sidecar,
    // survives save/load), and render thumbnails in the chat.
    //
    // Runs on the UI thread from OnAssistantComplete: the decode is
    // a few ms even for multi-MB images, and both the chat-folder-path
    // resolution and the history mutation are UI-thread-only anyway.
    void HandleGeneratedImages(const std::vector<std::string>& dataUrls)
    {
        // StartAssistantResponseForPreparedTurn assigns a file path
        // before every turn, so this only trips if a future caller
        // reorders that guarantee away.
        const std::string convPath = m_chatHistory->GetFilePath();
        if (convPath.empty()) {
            m_chatDisplay->DisplaySystemMessage(
                "generated image dropped: conversation has no save path.");
            return;
        }

        ChatHistory::EnsureChatFolder(convPath, m_chatHistory->GetChatFolderTitle());
        const std::string genDir = ChatHistory::GetGeneratedFilesDir(convPath);
        const std::string relDir = ChatHistory::GetGeneratedFilesRelDir(convPath);
        if (!wxDirExists(wxString::FromUTF8(genDir))) {
            wxFileName::Mkdir(wxString::FromUTF8(genDir),
                              wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
        }

        const std::string stamp(
            wxDateTime::Now().Format("%Y%m%d_%H%M%S").ToUTF8().data());

        std::vector<std::string> absPaths;
        std::vector<std::string> relPaths;
        int seq = 0;

        for (const auto& url : dataUrls) {
            ++seq;

            if (url.rfind("data:", 0) != 0) {
                // Hosted-URL fallback — rare, but a provider may return
                // a link instead of inline base64.  Surface it rather
                // than silently dropping the image; no auto-download,
                // the UI thread is not the place for another HTTP call.
                m_chatDisplay->DisplaySystemMessage(
                    "generated image (remote URL): " + url);
                continue;
            }

            // "data:image/png;base64,<payload>"
            const size_t comma = url.find(',');
            if (comma == std::string::npos) continue;
            const std::string header = url.substr(5, comma - 5);
            if (header.find("base64") == std::string::npos) continue;

            // Extension from the mime subtype; png covers the absent/
            // unknown cases (it's what OpenRouter image models emit).
            //
            // `sub` is remote-endpoint-controlled text: everything
            // between "image/" and the first ';' in a header that came
            // off the wire.  It must not reach the filename verbatim: a
            // subtype like "png:hidden" would produce an NTFS
            // alternate-data-stream write, and one ending in '.' or ' '
            // would defeat the collision-bump loop below (Windows
            // normalizes those away at open time, so wxFileExists would
            // miss and the write would clobber the file the loop
            // protects).  This field has exactly one legitimate shape,
            // so allowlist rather than sanitize.
            std::string ext = "png";
            if (header.rfind("image/", 0) == 0) {
                std::string sub = header.substr(6);
                const size_t semi = sub.find(';');
                if (semi != std::string::npos) sub = sub.substr(0, semi);
                if      (sub == "jpeg" || sub == "jpg") ext = "jpg";
                else if (sub == "webp")                 ext = "webp";
                else if (sub == "gif")                  ext = "gif";
                else if (sub == "png" || sub.empty())   ext = "png";
                else {
                    const bool clean =
                        sub.size() <= 8 &&
                        std::all_of(sub.begin(), sub.end(),
                            [](unsigned char c) {
                                return std::isalnum(c) != 0;
                            });
                    ext = clean ? sub : std::string("png");
                }
            }

            std::string decoded;
            try {
                std::istringstream is(url.substr(comma + 1));
                Poco::Base64Decoder dec(is);
                std::ostringstream os;
                Poco::StreamCopier::copyStream(dec, os);
                decoded = os.str();
            } catch (...) {
                decoded.clear();
            }
            if (decoded.empty()) {
                m_chatDisplay->DisplaySystemMessage(
                    "generated image could not be decoded; skipped.");
                continue;
            }

            // generated_<stamp>_<n>.<ext>, bumping <n> on the (rare)
            // same-second collision with an existing file.
            std::string fname;
            std::string absPath;
            bool freeNameFound = false;
            for (int bump = 0; bump < 1000; ++bump) {
                fname = "generated_" + stamp + "_" +
                        std::to_string(seq + bump) + "." + ext;
                absPath = genDir + "/" + fname;
                if (!wxFileExists(wxString::FromUTF8(absPath))) {
                    freeNameFound = true;
                    break;
                }
            }
            if (!freeNameFound) {
                // Practically unreachable (1000 same-second collisions),
                // but the old loop fell out holding an EXISTING path and
                // silently overwrote it.  Skip instead.
                m_chatDisplay->DisplaySystemMessage(
                    "generated image skipped: no free filename in " + genDir);
                continue;
            }

            bool written = false;
            {
                std::ofstream f(wxString::FromUTF8(absPath).fn_str(),
                                std::ios::out | std::ios::binary
                                              | std::ios::trunc);
                if (f) {
                    f.write(decoded.data(),
                            (std::streamsize)decoded.size());
                    written = (bool)f;
                }
            }
            if (!written) {
                m_chatDisplay->DisplaySystemMessage(
                    "generated image could not be written: " + absPath);
                continue;
            }

            absPaths.push_back(absPath);
            relPaths.push_back(relDir + "/" + fname);
        }

        if (absPaths.empty()) return;

        // Sidecar first (so the autosave at the end of
        // OnAssistantComplete picks it up), then the visuals.
        m_chatHistory->SetLastAssistantImages(relPaths);
        m_chatDisplay->DisplayInlineImages(absPaths);

        std::string savedLine = "saved: ";
        for (size_t i = 0; i < relPaths.size(); ++i) {
            if (i) savedLine += ", ";
            savedLine += relPaths[i];
        }
        m_chatDisplay->DisplaySystemMessage(savedLine);

        if (auto* logger = m_appState->GetLogger()) {
            logger->information(
                "Generated image(s) saved: " +
                std::to_string(absPaths.size()) + " file(s) in " + genDir);
        }
    }

    void OnAssistantComplete(wxCommandEvent& event)
    {
        // CRITICAL leak fix: wxCommandEvent does NOT delete its client
        // object (wx/event.h keeps a raw m_clientObject; no wx destructor
        // frees it -- it exists for pointing at control-owned item data).
        // Take ownership as the very first action so EVERY exit path,
        // including the stale-generation guard and the hidden skill
        // turn consume below, frees the payload.  Image-generation turns
        // carry the full base64 image data in here, so the old leak was
        // multiple MB per generated image and one small payload per agent
        // iteration on text turns.
        std::unique_ptr<wxClientData> payloadOwner(event.GetClientObject());
        event.SetClientObject(nullptr);
        auto* payload =
            dynamic_cast<AssistantCompletePayload*>(payloadOwner.get());

        if (m_isClosing) return;
        if (static_cast<unsigned long>(event.GetExtraLong()) != m_generationId) return;

        // Make sure any final batched text is in history/UI before we
        // decide whether this was visible prose, XML tool-only, or native
        // tool-only completion.
        FlushPendingAssistantDelta();

        std::string fullResponse = WxToUtf8(event.GetString());

        // Hidden control turns are not transcript replies.  Their streamed
        // deltas were discarded above; consume their completed text before
        // any normal chat UI finalization runs.
        // A hidden Skill draft must not reach the save path when the
        // worker cut it short, even if its partial text looks valid.
        const bool interruptedTurn =
            payload && payload->Stats().stoppedForRepetition;
        if (interruptedTurn) {
            if (m_skillDraftController->ConsumeAssistantError(
                    "Stopped this reply: the model kept repeating the same text. "
                    "The interrupted Skill draft was not saved.")) return;
        }
        else if (m_skillDraftController->ConsumeAssistantComplete(fullResponse)) return;

        // Context meter: adopt the server's exact token usage for this
        // completed transcript turn as the occupancy anchor.  Placed
        // AFTER the hidden-turn consume above on purpose — skill
        // control turns are built from different (smaller) prompts and
        // would drag the anchor below the real transcript occupancy.
        // Agent-loop iterations do flow through here and re-anchor on
        // every iteration, which is exactly right: the meter visibly
        // climbs as the loop spends budget.
        if (payload) {
            if (payload->PromptTokens() >= 0) {
                m_ctxAnchorPromptTokens = payload->PromptTokens();
                m_ctxAnchorCompletionTokens =
                    payload->CompletionTokens() > 0
                        ? payload->CompletionTokens() : 0;
                m_ctxAnchorExact = true;
                m_ctxAnchorRevision =
                    m_chatHistory ? m_chatHistory->GetRevision() : 0;
                // Self-calibrating elision budget: same request/usage
                // pairing as the calibration log below.
                if (m_chatHistory)
                    m_chatHistory->RecordExactPromptTokens(payload->PromptTokens());
                LogContextCalibration(payload->PromptTokens());
            }
            // Speeds / timings for this reply: shown in the ctx meter
            // tooltip and appended to the chat folder's turn_stats.tsv.
            m_lastTurnStats = payload->Stats();
            if (m_lastTurnStats.stoppedForRepetition) {
                m_chatDisplay->DisplaySystemNotice(
                    m_agentController->IsActive()
                    ? "Stopped this reply: the model kept repeating the same text. "
                      "The agent turn was ended and no tool call from this reply "
                      "was run. Send your message again or rephrase it; if it "
                      "keeps happening with this model, try /think off."
                    : "Stopped this reply: the model kept repeating the same text. "
                      "Send your message again or rephrase it; if it keeps "
                      "happening with this model, try /think off.");
            }
            if (m_chatHistory)
                m_lastRequestBreakdown = m_chatHistory->GetLastBuildBreakdown();
            if (!m_lastTurnStats.empty()) {
                if (m_chatTurnStats.size() >= 1000)
                    m_chatTurnStats.erase(m_chatTurnStats.begin());
                m_chatTurnStats.push_back(m_lastTurnStats);
            }
            LogTurnStats(m_lastTurnStats);
            RefreshContextMeter();
        }

        // Extract native tool_calls before deciding whether this
        // assistant turn has visible UI text.  Native function-calling
        // turns often complete with content == "" and tool_calls != [];
        // calling DisplayAssistantComplete() first would render an
        // empty "model:" row before the tool card.
        std::string toolCallsJson;
        std::vector<std::string> imageDataUrls;
        if (payload) {
            toolCallsJson = payload->ToolCallsJson();
            imageDataUrls = payload->TakeImageDataUrls();
        }

        // Repetition-guard stop (repetition_guard.h): the reply was cut
        // off by LlamaBoss, not finished by the model.  Treat it as an
        // interrupted turn -- keep the partial text, but never act on it.
        // ChatWorkerThread already drops native tool calls from such a
        // reply; clearing here as well keeps the invariant local, and the
        // agent routing below ends the loop instead of letting the
        // controller parse an XML <tool_call> out of fullResponse.
        if (interruptedTurn) toolCallsJson.clear();

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
        const bool agentEmptyCompletion =
            agentStreamActive &&
            toolCallsJson.empty() &&
            imageDataUrls.empty() &&
            !hasVisibleText(fullResponse) &&
            m_agentToolVisibleProseLen == 0;
        const bool suppressAgentAssistantRow =
            xmlToolOnlyCall || nativeToolOnlyCall || agentEmptyCompletion;

        if (agentStreamActive) {
            FlushAgentHeldProseIfSafe();
        }

        if (suppressAgentAssistantRow) {
            m_chatDisplay->CancelPendingAssistantDisplay();
        } else {
            m_chatDisplay->DisplayAssistantComplete();
        }

        if (hasVisibleText(fullResponse)) {
            m_chatHistory->UpdateLastAssistantMessage(fullResponse);
        }
        else if (auto* logger = m_appState->GetLogger();
                 logger && toolCallsJson.empty() && imageDataUrls.empty()) {
            logger->warning("Assistant complete event arrived empty; keeping streamed content");
        }

        // OpenAI Responses: keep the model's own output items
        // (encrypted reasoning + function calls) on this assistant turn.
        // Must land BEFORE the agent controller attaches tool_calls and
        // appends the first tool result, so the next iteration's request
        // can replay reasoning in front of the calls it produced.  The
        // adapter only replays entries whose call ids survive the
        // controller's batch/overflow filtering.
        if (payload && !toolCallsJson.empty() &&
            !payload->ResponsesOutputJson().empty()) {
            m_chatHistory->SetLastAssistantResponsesOutput(
                payload->ResponsesOutputJson());
        }

        // ── Generated images (image-output models) ───────────────
        // Decode + persist to the conversation's artifacts folder,
        // attach the sidecar to the assistant message, render
        // thumbnails.  Runs before the agent-routing block below,
        // but in practice never coexists with it: image turns are
        // sent without tools and never arm the agent loop.
        if (!imageDataUrls.empty()) {
            HandleGeneratedImages(imageDataUrls);
        }

        // ── Agent mode routing ──────────────────────────────────
        // If a loop is active and the controller consumed this
        // event (tool call found, loop continuing), skip the
        // normal "finalize and stop streaming" path — the next
        // iteration is already in flight and SetStreamingState(true)
        // was re-applied by OnAgentIterationBegin.
        if (interruptedTurn && m_agentController->IsActive()) {
            // Same unwind as OnAssistantError: reset the XML stream
            // filter, end the loop (StreamError reason -- the notice
            // above is the user-facing message), then finalize below.
            ResetAgentToolStreamFilter();
            m_agentController->HandleAssistantError("repetition loop");
        }
        else if (m_agentController->IsActive()) {
            // Structured tool_calls were extracted above before UI
            // finalization so native tool-only turns can be hidden
            // cleanly instead of rendering blank assistant rows.
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

        const std::string error = WxToUtf8(event.GetString());

        // Any assistant error ends the current stream. Preserve already-
        // streamed visible text so history and the on-screen transcript stay
        // consistent after reload. Hidden control turns still discard safely
        // inside FlushPendingAssistantDelta() instead of appending onto the
        // previous visible assistant message.
        FlushPendingAssistantDelta();

        if (m_skillDraftController->ConsumeAssistantError(error)) return;

        std::string modelName = ServerManager::ModelDisplayName(
            m_modelSwitcher->GetConversationModelForSave());

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

        // Match Stop/Close behavior: remove only an empty placeholder.  If
        // useful partial text was streamed before the error, keep it in
        // history so reload does not appear to lose a visible answer.
        if (m_chatHistory->HasAssistantPlaceholder()) {
            m_chatHistory->RemoveLastAssistantMessage();
        }

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

    // /cmd, Python, grep, web-fetch completion handlers moved to
    // tool_result_controller.{h,cpp} (bound there in BindFrameEvents).

    // ── Slash-command handlers ───────────────────────────────────
    // Tool-shaped slash commands route through HandleSlashCommand →
    // DispatchInvocation, the same path the agent uses.  Stateful
    // conversation commands keep their own handlers:
    //   - /cd mutates the per-conversation tool cwd.
    //
    // /cd resolution: per-conversation tool CWD if set, else the
    // conversation workspace.  Env-var expansion (%USERPROFILE% etc.)
    // is handled inside ResolveToolPath.  AutoSave fires only if
    // the conversation has content — empty-conversation /cd lives
    // in memory until the first real message pins it to disk.

    // Ensures the current conversation has a stable identity and a
    // user-visible chat folder before tools or attachments need a
    // real path on disk.  Conversation JSON still saves under
    // %LOCALAPPDATA%\LlamaBoss\conversations, but files for this chat
    // live under %USERPROFILE%\LlamaBoss\Chats\<date>_<title-slug>_<id>.
    void EnsureConversationChatFolder()
    {
        if (!m_chatHistory->HasFilePath())
            m_chatHistory->SetFilePath(ChatHistory::GenerateFilePath());

        const std::string convFilePath = m_chatHistory->GetFilePath();
        if (m_chatFolderEnsuredForFilePath != convFilePath) {
            ChatHistory::EnsureChatFolder(convFilePath,
                                         m_chatHistory->GetChatFolderTitle());
            m_chatFolderEnsuredForFilePath = convFilePath;
        }
    }

    // Resolves the effective working directory for a tool invocation:
    // per-conversation override first, falling back to this
    // conversation's own chat Workspace folder.  Never returns
    // empty.  The Workspace folder is created on first use rather than
    // up-front in EnsureChatFolder, so chats that never invoke tools
    // don't grow an empty Workspace/ subfolder.
    std::string ResolveCurrentCwd()
    {
        std::string cwd = m_chatHistory->GetToolCwd();
        if (cwd.empty()) {
            EnsureConversationChatFolder();

            const std::string convFilePath = m_chatHistory->GetFilePath();
            cwd = ChatHistory::GetConversationWorkspaceDir(convFilePath);

            // BuildToolContext() calls ResolveCurrentCwd() on every agent
            // iteration.  The workspace path is stable per conversation, so
            // ensure it once instead of paying a recursive Mkdir call every
            // time through the loop.
            if (m_workspaceDirEnsuredForFilePath != convFilePath) {
                wxFileName::Mkdir(wxString::FromUTF8(cwd),
                                  wxS_DIR_DEFAULT,
                                  wxPATH_MKDIR_FULL);
                m_workspaceDirEnsuredForFilePath = convFilePath;
            }
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

        // Bare /cd reports the current directory.
        if (path.empty()) {
            m_chatDisplay->DisplaySystemMessage(
                "Working directory: " + ResolveCurrentCwd() +
                "\nUsage: /cd <path>");
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

    // Minute sweep for idle Python sessions.  Silent: the death note
    // recorded by ReapIdle surfaces as the restart notice on that
    // conversation's next py call.  Also ages out zombie sessions a
    // frame can no longer reach (conversation moved to another window).
    void OnPySessionReapTimer(wxTimerEvent&)
    {
        if (m_pySessionManager)
            m_pySessionManager->ReapIdle();
    }

    // ─── Unified slash-command dispatch ──────────────────────────
    //
    // Typed tool commands (/reminder_create, /reminder_list and
    // /reminder_cancel; see kToolSlashTable in lb_input_parsers.cpp)
    // flow through HandleSlashCommand below.  The method builds a
    // ToolInvocation, calls DispatchInvocation, and either renders the
    // sync result or sets the chat-state so the matching
    // OnGrepComplete / OnCmdComplete picks up the async continuation.
    //
    // /cd is NOT a tool — it mutates per-conversation state and keeps
    // a dedicated handler.  Dispatch, validation, rendering, and
    // history for everything else are identical to the agent path.
    //
    // toolCallId is always empty for slash invocations: there is no
    // model-emitted call to thread.  AddUserMessage (rather than
    // AddToolResultMessage) is therefore the correct persistence
    // call for the result — see RenderAndPersistSlashResult.
    void DisplaySlashPendingIndicator(const std::string& toolName,
                                      const std::string& args)
    {
        const ToolSpec* spec = GetGlobalRouter().Find(toolName);
        if (!spec || (!spec->safety.isAsync && !spec->safety.dispatchOnWorker)) return;

        // Grep benefits from the richer historical indicator because the
        // resolved target path is not obvious from the raw slash text.
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
                    spec->iconUtf8 + " Grep: '" + pat + "' in " + resolved);
            }
            return;
        }

        // Keep the command echo for PowerShell; it is the one async slash
        // indicator where the raw args are the useful status text.
        if (toolName == tool_names::kPowerShell && !args.empty()) {
            m_chatDisplay->DisplaySystemMessage(
                spec->iconUtf8 + " PowerShell: " + args);
            return;
        }

        const std::string icon = spec->iconUtf8.empty()
            ? std::string("\xE2\x9A\xA0")
            : spec->iconUtf8;
        const std::string label = spec->displayName.empty()
            ? toolName
            : spec->displayName;

        m_chatDisplay->DisplaySystemMessage(icon + " " + label);
    }

    void DispatchSlashInvocation(const ToolInvocation& inv,
                                 const ToolContext&    ctx)
    {
        // Only announce work once approval (when required) has been granted
        // and dispatch is actually about to begin.  Emitting this at the top
        // of HandleSlashCommand made approval-gated async/worker tools look as
        // though they were already running while the user was still deciding.
        if (inv.valid)
            DisplaySlashPendingIndicator(inv.name, inv.args);

        if (inv.valid && ShouldDispatchToolOnWorker(inv.name)) {
            if (m_toolWorker && m_toolWorker->Start(inv, ctx)) {
                m_chatState = ChatState::RunningToolWorker;
                SetStreamingState(true);
                return;
            }

            ToolInvocationResult r;
            r.toolTag       = inv.name;
            r.invocationRaw = inv.rawBlock;
            r.iconUtf8      = tool_approval::ToolIcon(inv.name);
            r.toolName      = tool_approval::ToolDisplayName(inv.name);
            r.commandEcho   = inv.args.empty() ? inv.name : inv.args;
            r.chips         = { "error" };
            r.errorBody     =
                "The background tool worker is already busy or could not start.";
            m_toolResultController->RenderAndPersistSlashResult(r);
            if (!m_chatHistory->IsEmpty())
                m_convController->AutoSaveConversation();
            return;
        }

        DispatchOutcome out = DispatchInvocation(
            inv, ctx, m_grepExecutor.get(), m_cmdExecutor.get(),
            m_pythonRunner.get(), m_webFetchExecutor.get(),
            m_pySessionManager.get());

        switch (out.status) {
        case DispatchStatus::Completed:
        case DispatchStatus::Invalid:
            m_toolResultController->RenderAndPersistSlashResult(out.result);
            if (!m_chatHistory->IsEmpty())
                m_convController->AutoSaveConversation();
            return;

        case DispatchStatus::Async:
            // The dispatcher already started the specialized worker.
            m_chatState = ChatStateForAsyncToolName(inv.name);
            SetStreamingState(true);
            return;
        }
    }

    void GateOrDispatchSlashInvocation(const ToolInvocation& inv,
                                       const ToolContext&    ctx)
    {
        // Location authorization is independent of general tool trust. Even
        // a chat that previously selected Allow Always must explicitly grant
        // each out-of-root folder before a native mutation can run there.
        const bool alreadyApproved =
            m_chatHistory && m_chatHistory->IsToolChatApproved(inv.name);

        tool_approval::ApprovalDecision approval;
        const bool needsWriteRoot =
            tool_approval::RequiresWriteRootGrant(inv, ctx, approval);
        const bool needsActionApproval =
            !needsWriteRoot &&
            !alreadyApproved &&
            tool_approval::RequiresApproval(inv, ctx, approval);

        if (needsWriteRoot || needsActionApproval) {
            if (HasPendingApproval()) {
                m_chatDisplay->DisplaySystemMessage(
                    "Approval is already pending. Use the buttons above to respond.");
                return;
            }
            m_pendingSlashApproval.invocation = inv;
            m_pendingSlashApproval.context    = ctx;
            m_pendingSlashApproval.writeRootGrant = needsWriteRoot
                ? approval.writeRoot
                : std::string();
            m_pendingSlashApproval.active = true;

            ToolBlock card = approval.block;
            card.requiresApproval = true;
            m_chatDisplay->DisplayToolBlock(card, false);
            SetApprovalState(true);
            return;
        }

        DispatchSlashInvocation(inv, ctx);
    }

    void HandleSlashCommand(const std::string& toolName,
                            const std::string& args)
    {
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

        GateOrDispatchSlashInvocation(inv, BuildToolContext());
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
        std::string    writeRootGrant =
            m_pendingSlashApproval.writeRootGrant;
        m_pendingSlashApproval = PendingSlashApproval{};
        SetApprovalState(false);

        if (!writeRootGrant.empty()) {
            if (!m_chatHistory ||
                !m_chatHistory->GrantWriteRootForChat(writeRootGrant)) {
                ToolInvocationResult r = tool_approval::DeniedResult(
                    inv,
                    "Folder access could not be granted. Tool was not executed.");
                m_toolResultController->RenderAndPersistSlashResult(r);
                return;
            }

            // Rebuild so the new root is present, then re-run the ordinary
            // action gate. A delete therefore still gets its delete card.
            GateOrDispatchSlashInvocation(inv, BuildToolContext());
            return;
        }

        // Mark BEFORE dispatch so the per-chat approval state is
        // already in place if the model immediately requests another
        // approval-required tool.  "Approve always" now means
        // one-approval mode for this conversation, not just this
        // individual tool name.
        if (rememberForChat && m_chatHistory) {
            m_chatHistory->RememberAllToolApprovalsForChat();
        }

        DispatchSlashInvocation(inv, ctx);
    }

    void DenyPendingSlashTool(const std::string& message =
        "Denied by user. Tool was not executed.")
    {
        if (!m_pendingSlashApproval.active) return;

        ToolInvocation inv = m_pendingSlashApproval.invocation;
        m_pendingSlashApproval = PendingSlashApproval{};
        SetApprovalState(false);

        ToolInvocationResult r = tool_approval::DeniedResult(inv, message);
        m_toolResultController->RenderAndPersistSlashResult(r);
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

        // Session trust: chat-wide approval survives switching away and
        // back within this run of LlamaBoss.  Record the path here — the
        // single choke point both the slash and agent paths route
        // through for clicks AND typed commands — and let
        // LoadConversationFromPath re-arm the ChatHistory flag on
        // reload.  Unsaved chats have an empty path and are ignored;
        // AutoSaveConversation syncs them once a path exists.
        const bool resolvingWriteRoot =
            (m_pendingSlashApproval.active &&
             !m_pendingSlashApproval.writeRootGrant.empty()) ||
            (m_agentController &&
             m_agentController->IsAwaitingWriteRootGrant());

        if (approve && rememberForChat && !resolvingWriteRoot &&
            m_chatHistory) {
            wxGetApp().GetConversationRegistry()
                .RememberSessionTrust(m_chatHistory->GetFilePath());
        }

        if (m_pendingSlashApproval.active) {
            if (approve) ExecuteApprovedSlashTool(rememberForChat);
            else         DenyPendingSlashTool();
            return;
        }

        if (m_agentController && m_agentController->IsAwaitingApproval()) {
            SetApprovalState(false);
            if (approve) {
                bool ok = m_agentController->ApprovePendingTool(rememberForChat);
                if (ok && m_agentController->IsAwaitingAsyncResult()) {
                    // Agent-owned async tools are part of the active turn just
                    // like slash async tools.  Set the logical busy state before
                    // toggling widgets so IsBusy() blocks sidebar loads,
                    // Ctrl+N/Ctrl+O/Ctrl+S, and model switches until the
                    // completion event returns through AgentController.
                    m_chatState = ChatState::Streaming;
                    SetStreamingState(true);
                }
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
        ctx.setupConnection = [this](const std::string& provider, const std::string& query) {
            if (!wxIsMainThread() || m_isClosing) return std::string("Connection setup is unavailable.");
            std::string modelToUse;
            const bool saved = LbShowAIConnectionSetup(this, m_appState->GetEndpointStore(),
                m_appState->GetSecretsStore(), m_appState->GetTheme(), provider, query, &modelToUse);
            if (!saved) return std::string("Connection setup cancelled. No changes were saved.");
            // Plain Save from the setup dialog keeps the provider's model
            // list without touching this chat's model.
            if (modelToUse.empty()) return std::string("Connection saved. Model selection unchanged.");
            if (m_agentController && m_agentController->IsActive()) {
                m_pendingSetupModel = modelToUse;
                return std::string("Connection saved. Applying your selected model when this setup turn ends.");
            }
            // Also covers a direct slash invocation of the setup tool.
            m_modelSwitcher->SwitchToModel(modelToUse);
            _userInputCtrl->SetFocus();
            return std::string("Connection saved. Model selection requested.");
        };
        ctx.cwd = ResolveCurrentCwd();

        unsigned long t = m_chatHistory->GetToolTimeoutMs();
        ctx.timeoutMs = (t == 0) ? kDefaultToolTimeoutMs : t;

        ctx.ctxTokens = m_modelSwitcher->ConversationContextTokens();

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
        ctx.additionalWriteRoots = m_chatHistory->GetChatWriteRoots();
        return ctx;
    }

    // Project-context build + cache moved to project_context_builder.{h,cpp};
    // frame uses m_projectContextBuilder (created in CreateControllersAndCallbacks).

    std::string BuildNormalSystemPrompt() const
    {
        AgentPromptBuilderInput input;
        input.activeProjectContextBlock = m_projectContextBuilder->BuildActiveProjectContextBlock();
        input.pendingSkillAuthoringContextBlock = m_skillDraftController->BuildPendingSkillAuthoringContextBlock();
        return ::BuildNormalSystemPrompt(input);
    }

    // Agent-mode system prompt.  Prepended to each iteration's
    // request while the loop is active; not stored in history so
    // saved conversations stay clean.  Kept short — small models
    // follow short prompts much more reliably than long ones.
    //
    // The prompt is split by tool protocol.  Native models receive a
    // trimmed prompt (no XML grammar examples, no "Available tool
    // names" list) because the wire-level `tools` field teaches the
    // model what tools exist and how to call them.  XML models still
    // need the full grammar tutorial below.
    //
    // BuildAgentSystemPrompt() is the dispatcher; it picks based
    // on _activeProtocol.  Both branches share workspace context
    // and per-tool behaviour notes via small helper composers.

    std::string BuildAgentSystemPrompt()
    {
        AgentPromptBuilderInput input;
        input.isWorkspace = m_chatHistory->GetToolCwd().empty();
        input.cwd = ResolveCurrentCwd();
        input.activeProjectContextBlock = m_projectContextBuilder->BuildActiveProjectContextBlock();
        input.pendingSkillAuthoringContextBlock = m_skillDraftController->BuildPendingSkillAuthoringContextBlock();
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

    // Pulls the current project state from ChatHistory and pushes it
    // into the ProjectStatusStrip in one call. Project counts are exact,
    // but cached briefly so refresh churn does not repeatedly walk
    // Sources/Workflows on the UI thread.
    void RefreshProjectStrip()
    {
        if (!m_projectStrip) return;

        ProjectStatusStrip::State s;

        // ── Project half ─────────────────────────────────────────
        if (m_chatHistory->HasProject()) {
            s.hasProject  = true;
            s.projectName = m_chatHistory->GetProjectName();

            const std::string root = m_chatHistory->GetProjectRoot();
            const ProjectStripCounts counts = m_projectContextBuilder->GetProjectStripCounts(root);
            s.sourceCount   = counts.sourceCount;
            s.workflowCount = counts.workflowCount;
            s.scriptCount   = counts.scriptCount;
        }

        m_projectStrip->Refresh(s);
    }

    // Builds and shows the project / skill popup menu beside the strip.
    // Items are context-sensitive.  When invoked from [ + New Skill ],
    // the same actions are shown, but Skill actions are placed first so
    // the menu matches the control the user clicked.
    // Project-scoped popup ([ Project ▾ ] / right-click on the strip).
    // Project actions only -- Skills live on their own [ Skills ▾ ] menu
    // (ShowSkillPopupMenu) so the two scopes don't bleed into each other.
    void ShowProjectPopupMenu(wxWindow* anchor)
    {
        wxMenu menu;

        if (!m_chatHistory->HasProject()) {
            menu.Append(ID_PROJECT_NEW,    "New Project...");
            menu.Append(ID_PROJECT_ATTACH, "Load / Attach Project to Current Chat...");
            menu.Append(ID_PROJECTS_OPEN_ROOT_FOLDER, "Open Projects Folder");
        } else {
            menu.Append(ID_PROJECT_OPEN_FOLDER,        "Open Project Folder");
            menu.Append(ID_PROJECT_OPEN_INSTRUCTIONS,  "Open PROJECT.md");
            menu.AppendSeparator();
            menu.Append(ID_PROJECT_ADD_SOURCES,        "Add Source Files...");
            menu.Append(ID_PROJECT_OPEN_SOURCES_FOLDER,"Open Sources Folder");
            menu.AppendSeparator();
            // Single "New Workflow..." (md-only) -- the old "with Python
            // Script" variant is retired from the UI.  Python is always
            // available; a workflow can carry a .py whenever one is needed.
            menu.Append(ID_PROJECT_NEW_WORKFLOW,          "New Workflow...");
            menu.Append(ID_PROJECT_OPEN_WORKFLOW,         "Open a Workflow...");
            menu.Append(ID_PROJECT_OPEN_WORKFLOWS_FOLDER, "Open Workflows Folder");
            menu.AppendSeparator();
            menu.Append(ID_PROJECT_ATTACH, "Switch Project...");
            menu.Append(ID_PROJECT_CLEAR,  "Clear from This Chat");
        }

        PopupMenuAtAnchor(menu, anchor);
    }

    // Skills-scoped popup ([ Skills ▾ ]).  Skills are global / cross-project,
    // so this menu is identical whether or not a project is attached.
    void ShowSkillPopupMenu(wxWindow* anchor)
    {
        wxMenu menu;
        // Single "New Skill..." -- the old "with Python Script" variant
        // is retired from the UI, mirroring the project workflow menu.
        // Python is always available; the Skill draft builder decides
        // whether a helper ships based on the implementation-path
        // preference, and saves it under the skill's scripts subfolder.
        menu.Append(ID_SKILL_NEW, "New Skill...");
        // Import an external Agent Skills folder or .zip (SKILL.md +
        // optional scripts/references/assets), e.g. a skill authored by
        // Claude or GPT, without hand-placing files.  Same trust model
        // as a manual copy: helper runs stay behind normal approval
        // cards.  "Import", not "Load": skills are always available
        // once they are in the Skills folder -- there is no separate
        // activation step, so the verb names the copy, not a load.
        menu.Append(ID_SKILL_IMPORT, "Import a Skill...");
        menu.AppendSeparator();
        menu.Append(ID_SKILL_OPEN,        "Open a Skill...");
        menu.Append(ID_SKILL_EXPORT,      "Export a Skill...");
        menu.Append(ID_SKILL_OPEN_FOLDER, "Open Skills Folder");

        PopupMenuAtAnchor(menu, anchor);
    }

    // Shared positioning for the strip popups.  PopupMenu off the frame so
    // wxEVT_MENU lands on the existing Bind() entries set up in the
    // constructor.  Anchored to the bottom-left of the clicked affordance
    // so the menu drops just below the [ Project ▾ ] / [ Skills ▾ ] token.
    void PopupMenuAtAnchor(wxMenu& menu, wxWindow* anchor)
    {
        wxPoint pos(0, anchor ? anchor->GetSize().GetHeight() : 0);
        if (anchor) {
            pos = anchor->ClientToScreen(pos);
            pos = ScreenToClient(pos);
        }
        PopupMenu(&menu, pos);
    }


    // DeleteProjectByInfo() moved to ProjectController (project_controller.cpp).

    // MoveChatsToProject() moved to ProjectController (project_controller.cpp).

    // ═════════════════════════════════════════════════════════════
    //  Sidebar context menus (chat row + project header)
    // ═════════════════════════════════════════════════════════════

    // Right-click on one or more chat rows: move, delete, rename, pin,
    // and archive actions.
    void ShowSidebarChatContextMenu(const std::vector<std::string>& paths,
                                    wxWindow* anchor)
    {
        if (paths.empty()) return;

        wxMenu menu;
        const bool busy = IsBusy();
        const bool allPinned =
            m_sidebar && m_sidebar->AreAllPinned(paths);
        const bool archivedView =
            m_sidebar && m_sidebar->IsShowingArchived();

        // Snapshot |paths| so every handler is independent of later
        // selection changes while the popup is open.
        const std::vector<std::string> snapshot = paths;

        // ── Rename (single conversation only) ─────────────────────
        wxMenuItem* renameItem = nullptr;
        int renameItemId = 0;
        if (paths.size() == 1) {
            renameItem = menu.Append(wxID_ANY, "Rename conversation...");
            renameItemId = renameItem->GetId();
            if (busy) renameItem->Enable(false);
        }

        // ── Pin / unpin ───────────────────────────────────────────
        const wxString pinLabel = allPinned
            ? (paths.size() == 1
                ? wxString("Unpin conversation")
                : wxString::Format("Unpin %zu conversations", paths.size()))
            : (paths.size() == 1
                ? wxString("Pin conversation")
                : wxString::Format("Pin %zu conversations", paths.size()));
        wxMenuItem* pinItem = menu.Append(wxID_ANY, pinLabel);
        const int pinItemId = pinItem->GetId();
        if (busy) pinItem->Enable(false);

        menu.AppendSeparator();

        // ── Move to project ▸ submenu ─────────────────────────────
        wxMenu* moveSub = new wxMenu;
        auto projects = ProjectManager::ListProjects();

        // "(No project)" first — the unassign action.
        wxMenuItem* unassignedItem = moveSub->Append(wxID_ANY, "(No project)");
        const int unassignedItemId = unassignedItem->GetId();
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

        std::unordered_map<int, std::string> idToProject;
        for (const auto& p : projects) {
            wxMenuItem* projectItem = moveSub->Append(
                wxID_ANY, wxString::FromUTF8(p.name));
            const int itemId = projectItem->GetId();
            idToProject[itemId] = p.id;
        }

        if (projects.empty()) {
            wxMenuItem* hint = moveSub->Append(
                wxID_ANY,
                "(no projects yet - create one from the project strip)");
            hint->Enable(false);
        }

        const wxString moveLabel = (paths.size() == 1)
            ? wxString("Move to project")
            : wxString::Format("Move %zu chats to project", paths.size());
        wxMenuItem* moveItem = menu.AppendSubMenu(moveSub, moveLabel);
        if (busy) moveItem->Enable(false);

        // ── Export (single conversation; read-only, so not busy-gated) ─
        int exportItemId = 0;
        int exportMetricsItemId = 0;
        if (paths.size() == 1) {
            wxMenuItem* exportItem =
                menu.Append(wxID_ANY, "Export conversation...");
            exportItemId = exportItem->GetId();
            wxMenuItem* exportMetricsItem =
                menu.Append(wxID_ANY, "Export with metrics...");
            exportMetricsItemId = exportMetricsItem->GetId();
        }

        menu.AppendSeparator();

        // ── Archive / restore ─────────────────────────────────────
        const wxString archiveLabel = archivedView
            ? (paths.size() == 1
                ? wxString("Restore conversation")
                : wxString::Format("Restore %zu conversations", paths.size()))
            : (paths.size() == 1
                ? wxString("Archive conversation")
                : wxString::Format("Archive %zu conversations", paths.size()));
        wxMenuItem* archiveItem = menu.Append(wxID_ANY, archiveLabel);
        const int archiveItemId = archiveItem->GetId();
        if (busy) archiveItem->Enable(false);

        // ── Delete ───────────────────────────────────────────────
        wxMenuItem* deleteItem = nullptr;
        if (paths.size() <= 1) {
            deleteItem = menu.Append(wxID_DELETE, "Delete conversation");
        }
        else {
            deleteItem = menu.Append(
                wxID_DELETE,
                wxString::Format("Delete %zu conversations", paths.size()));
        }
        if (busy && deleteItem) deleteItem->Enable(false);

        // ── Bind handlers ─────────────────────────────────────────
        if (renameItem) {
            menu.Bind(
                wxEVT_MENU,
                [this, path = paths.front()](wxCommandEvent&) {
                    m_convController->RenameConversation(path);
                },
                renameItemId);
        }

        menu.Bind(
            wxEVT_MENU,
            [this, snapshot, allPinned](wxCommandEvent&) {
                m_convController->SetConversationsPinned(
                    snapshot, !allPinned);
            },
            pinItemId);

        menu.Bind(
            wxEVT_MENU,
            [this, snapshot](wxCommandEvent&) {
                m_projectController->MoveChatsToProject(
                    snapshot, std::string());
            },
            unassignedItemId);

        for (const auto& [itemId, projId] : idToProject) {
            const std::string capturedId = projId;
            const int capturedItemId = itemId;
            menu.Bind(
                wxEVT_MENU,
                [this, snapshot, capturedId](wxCommandEvent&) {
                    m_projectController->MoveChatsToProject(
                        snapshot, capturedId);
                },
                capturedItemId);
        }

        if (exportItemId) {
            menu.Bind(
                wxEVT_MENU,
                [this, path = paths.front()](wxCommandEvent&) {
                    m_convController->ExportConversation(path);
                },
                exportItemId);
        }
        if (exportMetricsItemId) {
            menu.Bind(
                wxEVT_MENU,
                [this, path = paths.front()](wxCommandEvent&) {
                    m_convController->ExportConversation(path, /*withMetrics=*/true);
                },
                exportMetricsItemId);
        }

        menu.Bind(
            wxEVT_MENU,
            [this, snapshot, archivedView](wxCommandEvent&) {
                m_convController->SetConversationsArchived(
                    snapshot, !archivedView);
            },
            archiveItemId);

        menu.Bind(
            wxEVT_MENU,
            [this, snapshot](wxCommandEvent&) {
                if (m_pySessionManager) {
                    for (const std::string& p : snapshot) {
                        m_pySessionManager->CloseSessionFor(
                            ChatHistory::GetConversationWorkspaceDir(p));
                    }
                }
                m_convController->DeleteConversations(snapshot);
            },
            wxID_DELETE);

        if (anchor)
            anchor->PopupMenu(&menu);
        else
            PopupMenu(&menu);
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
            wxMenuItem* attachItem = menu.Append(
                wxID_ANY,
                wxString::Format("Attach this chat to %s",
                                 wxString::FromUTF8(project.name)));
            const int attachId = attachItem->GetId();
            if (busy) attachItem->Enable(false);
            menu.Bind(wxEVT_MENU,
                [this, project](wxCommandEvent&) {
                    m_projectController->AttachProjectToCurrentChat(project);
                }, attachId);
            menu.AppendSeparator();
        }

        // ── Open actions ──────────────────────────────────────────
        wxMenuItem* openFolderItem =
            menu.Append(wxID_ANY, "Open Project Folder");
        wxMenuItem* openMdItem =
            menu.Append(wxID_ANY, "Open PROJECT.md");
        wxMenuItem* openSourcesItem =
            menu.Append(wxID_ANY, "Open Project Sources Folder");
        wxMenuItem* openWorkflowsItem =
            menu.Append(wxID_ANY, "Open Project Workflows Folder");

        const int openFolderId    = openFolderItem->GetId();
        const int openMdId        = openMdItem->GetId();
        const int openSourcesId   = openSourcesItem->GetId();
        const int openWorkflowsId = openWorkflowsItem->GetId();

        const std::string root = project.rootPath;
        menu.Bind(wxEVT_MENU,
            [this, root](wxCommandEvent&) { LbOpenProjectFolderByRoot(this, root); },
            openFolderId);
        menu.Bind(wxEVT_MENU,
            [this, root](wxCommandEvent&) { LbOpenProjectInstructionsByRoot(this, root); },
            openMdId);
        menu.Bind(wxEVT_MENU,
            [this, root](wxCommandEvent&) { LbOpenProjectSourcesFolderByRoot(this, root); },
            openSourcesId);
        menu.Bind(wxEVT_MENU,
            [this, root](wxCommandEvent&) { LbOpenProjectWorkflowsFolderByRoot(this, root); },
            openWorkflowsId);

        // ── Delete project ────────────────────────────────────────
        menu.AppendSeparator();
        wxMenuItem* deleteItem = menu.Append(wxID_ANY, "Delete Project...");
        const int deleteId = deleteItem->GetId();
        if (busy) deleteItem->Enable(false);
        menu.Bind(wxEVT_MENU,
            [this, project](wxCommandEvent&) {
                m_projectController->DeleteProjectByInfo(project);
            }, deleteId);

        if (anchor) {
            anchor->PopupMenu(&menu);
        }
        else {
            PopupMenu(&menu);
        }
    }


    // ═════════════════════════════════════════════════════════════
    //  Project menu handlers
    // ═════════════════════════════════════════════════════════════

    // AttachProjectToCurrentChat() moved to ProjectController (project_controller.cpp).

    // PromptCreateProject() moved to ProjectController (project_controller.cpp).

    void OnProjectNew(wxCommandEvent&) { m_projectController->NewProject(); }

    void OnProjectAttach(wxCommandEvent&) { m_projectController->AttachOrSwitchProject(); }

    void OnProjectDelete(wxCommandEvent&) { m_projectController->DeleteProjectViaPicker(); }

    void OnProjectOpenFolder(wxCommandEvent&) { m_projectController->OpenProjectFolder(); }

    void OnProjectOpenInstructions(wxCommandEvent&) { m_projectController->OpenProjectInstructions(); }

    void OnProjectAddSources(wxCommandEvent&) { m_projectController->AddSourceFiles(); }

    void OnProjectOpenSourcesFolder(wxCommandEvent&) { m_projectController->OpenSourcesFolder(); }


    // CreateProjectWorkflowFromMenu() moved to ProjectController (project_controller.cpp).

    void OnProjectNewWorkflow(wxCommandEvent&) { m_projectController->NewWorkflow(false); }

    void OnProjectNewWorkflowWithScript(wxCommandEvent&) { m_projectController->NewWorkflow(true); }

    void OnProjectOpenWorkflow(wxCommandEvent&) { m_projectController->OpenWorkflow(); }

    void OnProjectOpenWorkflowsFolder(wxCommandEvent&) { m_projectController->OpenWorkflowsFolder(); }

    // ── Skill handlers ───────────────────────────────────────────
    // These do not require an attached project. They always operate
    // against %USERPROFILE%\LlamaBoss\Skills.

    // Single creation path: no with/without-Python-Script split.  The
    // Skill draft builder decides whether a helper ships (implementation-
    // path preference), so nothing is asked up front and no stub .py is
    // pre-created -- the helper is written at draft-save time, into the
    // skill's scripts subfolder.
    void CreateSkillFromMenu()
    {
        if (IsBusy()) return;

        LbThemedTextEntryDialog dlg(
            this,
            m_appState->GetTheme(),
            "New Skill",
            "Skill name:",
            "Create");
        if (LbShowModalWithScrim(*this, dlg) != wxID_OK) return;

        const std::string name = std::string(dlg.GetValue().ToUTF8().data());
        SkillInfo skill;
        std::string error;
        if (!ProjectManager::CreateSkill(name, skill, error)) {
            std::string msg = error.empty()
                ? std::string("Could not create Skill.")
                : error;
            wxMessageBox(wxString::FromUTF8(msg.c_str()),
                         "Skills", wxOK | wxICON_ERROR, this);
            return;
        }

        m_skillDraftController->StartDesignSession(name, skill.path);

        std::ostringstream body;
        body << "Created Skill:\n"
             << skill.path;
        body << "\n\nLet\xe2\x80\x99s design this Skill together first. Tell me what you want it to do, "
             << "and I can ask questions, check notes when useful, and help choose the right implementation path. "
             << "If reusable Python code turns out to be the right fit, the draft builder will create the helper script automatically. "
             << "When the design sounds right, say `draft this Skill` and I\xe2\x80\x99ll write the Skill files.";
        m_chatDisplay->DisplaySystemMessage(body.str());
        m_projectContextBuilder->Invalidate();

        RefreshProjectStrip();
    }

    void OnSkillNew(wxCommandEvent&)
    {
        CreateSkillFromMenu();
    }

    // Import an external Agent Skills skill into LlamaBoss\Skills, from
    // either a folder (pick its SKILL.md -- unambiguous, unlike a folder
    // picker) or a .zip archive.  Two-phase (probe -> confirm -> import)
    // so the user sees the skill's name, description, and size before
    // anything is copied.
    void OnSkillImport(wxCommandEvent&)
    {
        if (IsBusy()) return;

        wxFileDialog picker(
            this,
            "Import a Skill: pick its SKILL.md, or a Skill .zip archive",
            wxEmptyString,
            wxEmptyString,
            "Skill contract or archive (SKILL.md;*.zip)|SKILL.md;*.zip|"
            "Skill archives (*.zip)|*.zip|"
            "All files (*.*)|*.*",
            wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (picker.ShowModal() != wxID_OK) return;

        const std::string picked =
            std::string(picker.GetPath().ToUTF8().data());
        const std::string pickedLower = [&picked]() {
            std::string v = picked;
            std::transform(v.begin(), v.end(), v.begin(),
                           [](unsigned char ch) {
                               return static_cast<char>(std::tolower(ch));
                           });
            return v;
        }();

        const bool isZip =
            pickedLower.size() >= 4 &&
            pickedLower.compare(pickedLower.size() - 4, 4, ".zip") == 0;

        // Resolve the source folder.  Zip: extract to a temp folder first
        // (validated + capped inside ExtractSkillZipToTemp), and ALWAYS
        // remove that temp folder on every exit path below.
        std::string tempRoot;
        std::string sourceFolder;
        std::string error;
        if (isZip) {
            if (!ProjectManager::ExtractSkillZipToTemp(
                    picked, tempRoot, sourceFolder, error)) {
                wxMessageBox(wxString::FromUTF8(error.c_str()),
                             "Import Skill", wxOK | wxICON_ERROR, this);
                return;
            }
        } else {
            const wxFileName fn(picker.GetPath());
            if (!fn.GetFullName().IsSameAs("SKILL.md", false)) {
                wxMessageBox(
                    "Pick the Skill's SKILL.md contract file, or a Skill "
                    ".zip archive.",
                    "Import Skill", wxOK | wxICON_ERROR, this);
                return;
            }
            sourceFolder = std::string(fn.GetPath().ToUTF8().data());
        }

        const auto cleanupTemp = [&tempRoot]() {
            if (!tempRoot.empty()) {
                wxFileName::Rmdir(wxString::FromUTF8(tempRoot),
                                  wxPATH_RMDIR_RECURSIVE);
            }
        };

        ProjectManager::SkillImportProbe probe;
        if (!ProjectManager::ProbeSkillImportFolder(sourceFolder, probe,
                                                    error)) {
            cleanupTemp();
            wxMessageBox(wxString::FromUTF8(error.c_str()),
                         "Import Skill", wxOK | wxICON_ERROR, this);
            return;
        }

        std::ostringstream confirm;
        confirm << "Import this Skill into LlamaBoss?\n\n"
                << "Name: " << probe.proposedName << "\n"
                << "Description: "
                << (probe.description.empty()
                        ? std::string("(none in SKILL.md; a starter one will be added)")
                        : probe.description)
                << "\n"
                << "Files: " << probe.fileCount << " ("
                << ProjectSource_HumanBytes(probe.totalBytes) << ")\n";
        if (!probe.finalName.empty() &&
            probe.finalName != probe.proposedName) {
            confirm << "\nA Skill named " << probe.proposedName
                    << " already exists, so this one will be imported as "
                    << probe.finalName << ".\n";
        }
        confirm << "\nIt will be copied into your LlamaBoss Skills folder "
                   "and offered to the model like any other Skill. Helper "
                   "scripts still run under the normal approval rules.";

        if (wxMessageBox(wxString::FromUTF8(confirm.str().c_str()),
                         "Import Skill",
                         wxYES_NO | wxICON_QUESTION, this) != wxYES) {
            cleanupTemp();
            return;
        }

        SkillInfo skill;
        const bool imported =
            ProjectManager::ImportSkillFolder(sourceFolder, skill, error);
        cleanupTemp();
        if (!imported) {
            wxMessageBox(wxString::FromUTF8(error.c_str()),
                         "Import Skill", wxOK | wxICON_ERROR, this);
            return;
        }

        std::ostringstream body;
        body << "Imported Skill:\n"
             << skill.path
             << "\n\nYou can now ask me to use this Skill whenever you "
                "are ready, or open it from the Skills menu to review it. "
                "I will not run it until you ask.";
        m_chatDisplay->DisplaySystemMessage(body.str());
        m_projectContextBuilder->Invalidate();

        RefreshProjectStrip();
    }

    // Export one Skill folder as a shareable .zip -- the inverse of
    // Import a Skill, and the packaging step for publishing skills on
    // llamaboss.com.  The archive roots the skill as "<stem>/..." so it
    // re-imports through the single-top-level-folder path.
    void OnSkillExport(wxCommandEvent&)
    {
        auto skills = ProjectManager::ListSkills(0);
        if (skills.empty()) {
            wxMessageBox(
                "No Skills found yet. Use New Skill or Import a Skill first.",
                "Skills", wxOK | wxICON_INFORMATION, this);
            return;
        }

        wxArrayString choices;
        wxArrayString details;
        for (const auto& skill : skills) {
            choices.Add(wxString::FromUTF8(
                LbSkillDisplayNameFromContractPath(skill)));
            details.Add(wxString::FromUTF8(
                LbReadSkillFrontmatterDescription(skill.path, 220)));
        }

        LbThemedSingleChoiceDialog dlg(
            this,
            m_appState->GetTheme(),
            "Export Skill",
            "Select a Skill to export as a .zip:",
            choices,
            "Export");
        dlg.SetItemDetails(details);

        if (LbShowModalWithScrim(*this, dlg) != wxID_OK) return;
        const int sel = dlg.GetSelection();
        if (sel < 0 || static_cast<size_t>(sel) >= skills.size()) return;

        const SkillInfo skill = skills[static_cast<size_t>(sel)];
        const std::string stem =
            LbSkillDisplayNameFromContractPath(skill);

        wxFileDialog saver(
            this,
            "Export Skill as .zip",
            wxEmptyString,
            wxString::FromUTF8(stem + "-skill.zip"),
            "Skill archives (*.zip)|*.zip",
            wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (saver.ShowModal() != wxID_OK) return;

        const std::string zipPath =
            std::string(saver.GetPath().ToUTF8().data());

        std::string error;
        if (!ProjectManager::ExportSkillToZip(skill.path, zipPath, error)) {
            wxMessageBox(wxString::FromUTF8(error.c_str()),
                         "Export Skill", wxOK | wxICON_ERROR, this);
            return;
        }

        std::ostringstream body;
        body << "Exported Skill " << stem << " to:\n" << zipPath
             << "\n\nAnyone running LlamaBoss can import it with "
                "Skills > Import a Skill, and the layout follows the "
                "Agent Skills standard.";
        m_chatDisplay->DisplaySystemMessage(body.str());
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
        wxArrayString details;
        for (const auto& skill : skills) {
            choices.Add(wxString::FromUTF8(LbSkillDisplayNameFromContractPath(skill)));
            // Frontmatter description as the picker's detail line; legacy
            // skills without frontmatter return empty and show nothing.
            details.Add(wxString::FromUTF8(
                LbReadSkillFrontmatterDescription(skill.path, 220)));
        }

        LbThemedSingleChoiceDialog dlg(
            this,
            m_appState->GetTheme(),
            "Open Skill",
            "Select a Skill to open:",
            choices,
            "Open");
        dlg.SetItemDetails(details);

        dlg.SetDeleteHandler([this, &dlg, &skills](int sel, const wxString& label) -> bool {
            if (sel < 0 || static_cast<size_t>(sel) >= skills.size()) return false;

            const SkillInfo skill = skills[static_cast<size_t>(sel)];

            auto makeFsPath = [](const std::string& utf8) -> std::filesystem::path {
#ifdef _WIN32
                return std::filesystem::path(wxString::FromUTF8(utf8).ToStdWstring());
#else
                return std::filesystem::u8path(utf8);
#endif
            };
            auto fsPathToUtf8 = [](const std::filesystem::path& path) -> std::string {
#ifdef _WIN32
                return std::string(wxString(path.wstring()).ToUTF8().data());
#else
                return path.string();  // native paths are UTF-8 on POSIX
#endif
            };

            const std::filesystem::path contractPath = makeFsPath(skill.path);
            const std::filesystem::path skillDir = contractPath.parent_path();
            const std::filesystem::path skillsRoot = makeFsPath(ProjectManager::GetSkillsDir());

            if (contractPath.filename() != std::filesystem::path("SKILL.md") || skillDir.empty()) {
                wxMessageBox("This Skill has an unexpected path and was not deleted.",
                             "Delete Skill", wxOK | wxICON_ERROR, &dlg);
                return false;
            }

            std::error_code ec;
            if (!std::filesystem::is_regular_file(contractPath, ec)) {
                wxMessageBox("Could not find the selected Skill contract on disk.",
                             "Delete Skill", wxOK | wxICON_ERROR, &dlg);
                return false;
            }

            ec.clear();
            const std::filesystem::path canonicalRoot =
                std::filesystem::weakly_canonical(skillsRoot, ec);
            if (ec) {
                wxMessageBox("Could not verify the LlamaBoss Skills folder.",
                             "Delete Skill", wxOK | wxICON_ERROR, &dlg);
                return false;
            }

            ec.clear();
            const std::filesystem::path canonicalSkillDir =
                std::filesystem::weakly_canonical(skillDir, ec);
            if (ec) {
                wxMessageBox("Could not verify the selected Skill folder.",
                             "Delete Skill", wxOK | wxICON_ERROR, &dlg);
                return false;
            }

            ec.clear();
            const std::filesystem::path relative =
                std::filesystem::relative(canonicalSkillDir, canonicalRoot, ec);
            bool unsafePath = ec || relative.empty() ||
                              relative == std::filesystem::path(".") ||
                              relative.is_absolute();
            for (const auto& part : relative) {
                if (part == std::filesystem::path("..")) {
                    unsafePath = true;
                    break;
                }
            }
            if (unsafePath) {
                wxMessageBox("Refusing to delete a path outside the LlamaBoss Skills folder.",
                             "Delete Skill", wxOK | wxICON_ERROR, &dlg);
                return false;
            }

            wxString warning;
            warning << "Delete Skill \"" << label << "\"?\n\n"
                    << "This deletes the entire Skill folder and cannot be undone.\n\n"
                    << wxString::FromUTF8(skill.path);

            if (wxMessageBox(warning,
                             "Delete Skill",
                             wxYES_NO | wxNO_DEFAULT | wxICON_WARNING,
                             &dlg) != wxYES) {
                return false;
            }

            ec.clear();
            const auto removed = std::filesystem::remove_all(canonicalSkillDir, ec);
            if (ec || removed == 0) {
                wxString msg;
                msg << "Could not delete the selected Skill folder.";
                if (ec) {
                    msg << "\n\n" << wxString::FromUTF8(ec.message().c_str());
                }
                wxMessageBox(msg, "Delete Skill", wxOK | wxICON_ERROR, &dlg);
                return false;
            }

            if (m_skillDraftController->IsDesignSessionForSkillPath(skill.path)) {
                m_skillDraftController->ClearDesignSession();
            }

            skills.erase(skills.begin() + sel);
            m_projectContextBuilder->Invalidate();
            RefreshProjectStrip();

            std::ostringstream body;
            body << "Deleted Skill: " << std::string(label.ToUTF8().data()) << "\n"
                 << fsPathToUtf8(canonicalSkillDir);
            m_chatDisplay->DisplaySystemMessage(body.str());
            return true;
        });

        if (LbShowModalWithScrim(*this, dlg) != wxID_OK) return;

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
        LbLaunchPathInOS(this, dir, "LlamaBoss Skills folder");
    }

    void OnProjectsOpenRootFolder(wxCommandEvent&) { m_projectController->OpenProjectsRootFolder(); }

    void OnProjectClear(wxCommandEvent&) { m_projectController->ClearProjectFromChat(); }

    void OnToggleSidebar(wxCommandEvent&)
    {
        m_sidebar->Toggle();
        if (m_sidebar->IsVisible())
            m_sidebar->Refresh(m_chatHistory->GetFilePath());
        _contentSizer->Layout();
        GetSizer()->Layout();
    }

    // ── New Chat prompt-cache pre-warm (prompt_prewarm.h) ────────
    // Delay before priming: long enough that New Chat followed by
    // opening an old chat (or typing a quick /command) primes nothing,
    // short compared with the ~8 s the prime itself takes.
    static constexpr int kPrewarmDelayMs = 1000;

    void SchedulePromptPrewarm(const char* reason)
    {
        m_prewarmReason = reason;
        m_prewarmTimer.StartOnce(kPrewarmDelayMs);
    }

    // Queues a prime of the stable part of the agent prompt when this
    // window shows an empty agent-mode chat on a local model and nothing
    // is running.  Returns false (and logs why) otherwise.  Always on.
    bool TryPromptPrewarm(const std::string& reason)
    {
        auto skip = [&](const std::string& why) {
            if (auto* logger = m_appState->GetLogger())
                logger->debug("prewarm: not started (" + reason + "): " + why);
            return false;
        };
        if (m_isClosing) return skip("window closing");
        if (!m_agentModeEnabled) return skip("Agent mode is off");
        if (IsBusy() || m_pendingSend.active) return skip("this window is busy");
        if (!m_chatHistory || !m_chatHistory->IsEmpty())
            return skip("only a new, empty chat is primed (this chat's own KV is saved/restored instead)");
        if (!m_modelSwitcher->IsServerReady() || !m_modelSwitcher->IsConversationTargetActive())
            return skip("the model isn't loaded yet");
        const InferenceTarget target = m_modelSwitcher->ResolveTargetForConversation();
        if (!target.managed)
            return skip("remote models cache prompts on the provider side");
        if (target.noTools || target.imageOutput)
            return skip("this model runs without agent tools");
        if (_activeProtocol == ToolProtocol::Unknown)
            return skip("tool protocol not detected yet");

        // Same request shaping as the agent branch of
        // StartAssistantResponseForPreparedTurn, so /apply-template renders
        // the prefix exactly as the first real request will.  Only the
        // user message differs, and it comes after the cut.
        const bool native = (_activeProtocol == ToolProtocol::Native);
        const std::string tools = native ? GetCachedToolsArrayJson() : std::string();
        const std::string systemPrompt = BuildAgentSystemPrompt();
        const std::string model = m_modelSwitcher->GetConversationModelForSave();
        int ctxTokens = m_appState->GetCtxSize();
        if (ctxTokens <= 0) ctxTokens = 8192;

        ChatHistory scratch;
        scratch.SetActiveReasoningDialect(target.reasoningDialect);
        scratch.SetActiveResponsesApi(target.responsesApi);
        scratch.SetThinkOverride(m_chatHistory->GetThinkOverride());
        scratch.AddUserMessage(".");
        const std::string body = scratch.BuildChatRequestJson(
            model, true, systemPrompt, ctxTokens, tools, native, true);

        const std::string stable =
            prompt_prewarm::StablePart(systemPrompt, kAgentWorkingContextHeading);
        const std::string key = prompt_prewarm::MakeKey(
            model, ToolProtocolName(_activeProtocol),
            ThinkingModeName(m_chatHistory->GetThinkOverride()), stable, tools);

        std::string why;
        if (!m_modelService->PrewarmPromptPrefix(this, body,
                kAgentWorkingContextHeading, key, why))
            return skip(why);

        if (auto* logger = m_appState->GetLogger())
            logger->information("prewarm: queued (" + reason + ", " +
                ToolProtocolName(_activeProtocol) + ", " +
                std::to_string(stable.size()) + " stable system bytes) [key " + key + "]");
        return true;
    }

    // /bench [runs] [cold] [long] | stop | help  -- see bench_stats.h.
    // Sends a fixed prompt to this conversation's model N times on its own
    // connection (chat history untouched), prints one line per run and a
    // median summary, and saves a TSV under Shared\\Benchmarks.
    void HandleBenchCommand(const std::string& args)
    {
        bench::Options opts;
        bool stop = false, help = false;
        std::string error;
        if (!bench::ParseArgs(args, opts, stop, help, error)) {
            m_chatDisplay->DisplaySystemMessage(error + "\n" + bench::Usage());
            return;
        }
        if (help) { m_chatDisplay->DisplaySystemMessage(bench::Usage()); return; }
        if (stop) {
            if (m_bench && m_bench->IsRunning()) m_bench->Stop();
            else m_chatDisplay->DisplaySystemMessage("No benchmark is running.");
            return;
        }
        if (!m_modelSwitcher->IsServerReady()) {
            m_chatDisplay->DisplaySystemMessage("Load a model first, then run /bench.");
            return;
        }

        const InferenceTarget target = m_modelSwitcher->ResolveTargetForConversation();
        if (target.imageOutput) {
            m_chatDisplay->DisplaySystemMessage(
                "/bench measures text generation; this is an image-output model.");
            return;
        }
        // One shared llama-server slot: another window's stream would
        // queue our runs behind it and wreck the timings.
        if (target.managed && m_modelService->AnyOtherWindowBusyOnLocalServer(this)) {
            m_chatDisplay->DisplaySystemMessage(
                "Another window is using the model. Run /bench when it's idle "
                "so the timings aren't skewed.");
            return;
        }

        const std::string sep(1, static_cast<char>(wxFILE_SEP_PATH));
        BenchController::Setup setup;
        setup.target    = target;
        setup.bodyModel = m_modelSwitcher->GetConversationModelForSave();
        setup.label     = _modelLabel ? WxToUtf8(_modelLabel->GetLabel()) : std::string();
        if (setup.label.empty() || setup.label == "loading...")
            setup.label = WxToUtf8(wxFileName(wxString::FromUTF8(setup.bodyModel)).GetName());
        setup.think     = m_chatHistory ? m_chatHistory->GetThinkOverride()
                                        : ChatHistory::ThinkOverride::Auto;
        setup.outDir    = ServerManager::GetSharedLanesRootDir() + sep + "Benchmarks";
        setup.options   = opts;

        if (!m_bench) {
            BenchController::Callbacks cb;
            cb.progress = [this](const std::string& line) {
                if (!m_isClosing) m_chatDisplay->DisplaySystemMessage(line);
            };
            cb.finished = [this](const std::string& summary) {
                if (m_isClosing) return;
                m_chatDisplay->DisplaySystemMessage(summary);
                SetStreamingState(false);
            };
            m_bench = std::make_unique<BenchController>(std::move(cb));
        }

        // The benchmark overwrites llama-server's single KV slot.  Claim it
        // under a benchmark-only name so switching conversations never
        // saves this prompt's KV as a chat's cache.
        if (target.managed)
            m_modelService->NoteSlotOwner(this, setup.outDir + sep + "bench.json");

        std::string startError;
        if (!m_bench->Start(setup, startError)) {
            m_chatDisplay->DisplaySystemMessage(startError);
            return;
        }
        SetStreamingState(true);   // Stop button cancels; input locked
        const char* dot = " \xC2\xB7 ";
        m_chatDisplay->DisplaySystemMessage(
            "Benchmark" + std::string(dot) + setup.label + dot +
            std::to_string(opts.runs) + (opts.runs == 1 ? " run" : " runs") + dot +
            (opts.longPrompt ? "~4k" : "~540") + "-token prompt" + dot +
            std::to_string(opts.genTokens) + "-token generation" + dot +
            (target.managed ? (opts.allCold ? "every run cold" : "run 1 cold")
                            : "remote (output length varies)") +
            ". Press Stop to cancel.");
    }

    void OnStopGeneration(wxCommandEvent&)
    {
        if (m_isClosing) return;
        if (m_bench && m_bench->IsRunning()) { m_bench->Stop(); return; }
        // Stop any running Easter egg animation
        if (m_activeAnimation) { StopAnimation(); return; }

        // Stop while an approval card is pending means cancel the
        // pending tool, not a nonexistent chat stream.
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

        // ── Long-task guard ─────────────────────────────────────
        // Stop kills the whole process tree.  Past a couple of minutes
        // into a download / build that is almost always a mis-click, and
        // the cost of confirming is one dialog; the cost of not confirming
        // is the last 14 minutes of a 15-minute download.
        if (m_activityStrip && m_activityStrip->IsActive() &&
            m_activityStrip->ElapsedSec() > 120.0) {
            const std::uint64_t confirmedActivity = m_activityRevision;
            const long long elapsed =
                static_cast<long long>(m_activityStrip->ElapsedSec());
            const wxString msg = wxString::Format(
                "A tool has been running for %lld:%02lld. Stop this tool "
                "and cancel the current turn?\n\n"
                "Output already captured is kept. Files already written "
                "are not undone.\n\n"
                "If this tool finishes while this dialog is open, this "
                "confirmation will not stop the next step.",
                elapsed / 60, elapsed % 60);
            if (wxMessageBox(msg, "Stop Long-Running Task",
                             wxYES_NO | wxNO_DEFAULT | wxICON_WARNING,
                             this) != wxYES) {
                return;
            }
            // wxMessageBox pumps events: A can finish and B can become
            // active before Yes is clicked. Never apply A's confirmation
            // to B, a new approval, or the next model stream.
            if (m_isClosing || !m_activityStrip ||
                !m_activityStrip->IsActive() ||
                m_activityRevision != confirmedActivity)
                return;
        }

        // Hidden Skill control turns have no transcript placeholder.
        // Let the controller stop them before normal assistant or agent
        // teardown runs.
        if (m_skillDraftController->HandleStopGeneration()) return;

        // ── Agent loop cancellation ─────────────────────────────
        // Arm agent cancellation before stopping the in-flight operation.
        // Async workers retain their event-driven teardown. A model stream is
        // different: ChatClient intentionally suppresses COMPLETE/ERROR after
        // StopGeneration(), so we finalize that agent loop explicitly below.
        const bool stoppingAgentStream = m_agentController->IsActive();
        if (stoppingAgentStream) {
            m_agentController->Cancel();

            // If the agent is waiting on an async tool worker, Cancel()
            // has already signaled that worker. Do NOT fall through to
            // StopGeneration(), because there is no chat stream to stop;
            // the worker's completion event will reset the UI cleanly.
            if (m_agentController->IsAwaitingAsyncResult())
                return;

            // Otherwise fall through so the active model stream receives
            // StopGeneration(), then synchronously close the agent lifecycle.
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
                // RunningPython covers both Python backends; Cancel()
                // on the idle one is a harmless flag set.  A cancelled
                // /py exec kills its session (state lost, said
                // explicitly in the result card).
                m_pythonRunner->Cancel();
                if (m_pySessionManager) m_pySessionManager->Cancel();
                return;
            }
            if (m_chatState == ChatState::RunningWebFetch) {
                m_webFetchExecutor->Cancel();
                return;
            }
            if (m_chatState == ChatState::RunningToolWorker) {
                m_toolWorker->Cancel();
                return;
            }

            // Policy alignment with OnClose/OnAssistantError: commit the
            // last batched delta (up to ~16 ms of streamed text) instead
            // of dropping it, so Stop preserves exactly what was
            // generated.  Must run BEFORE ++m_generationId or the flush
            // discards the batch as stale.
            FlushPendingAssistantDelta();
            ++m_generationId;
            m_chatClient->StopGeneration();
            m_chatDisplay->DisplayAssistantComplete();
            m_chatDisplay->DisplaySystemMessage("Generation stopped by user");
            if (m_chatHistory->HasAssistantPlaceholder())
                m_chatHistory->RemoveLastAssistantMessage();

            // A cancelled ChatClient posts no terminal event. Without this
            // explicit agent-loop completion, m_agentController stays active
            // forever and IsBusy() blocks both Send and model switching.
            if (stoppingAgentStream) {
                ResetAgentToolStreamFilter();
                if (m_agentController->FinishCancelledStream())
                    return;  // OnAgentLoopEnd performs the standard reset/save.
            }

            SetStreamingState(false);
            m_chatDisplay->ClearFilePersistenceContext();
            if (!m_chatHistory->IsEmpty()) m_convController->AutoSaveConversation();
        }
    }

    // ── In-app reminders ────────────────────────────────────────
    void OnReminderTimer(wxTimerEvent&)
    {
        if (m_isClosing) return;

        auto& reminderStore = lb_reminders::GetReminderStore();
        const auto due = reminderStore.ClaimDue();
        for (const auto& reminder : due) {
            if (m_isClosing) break;

            wxString text = wxString::FromUTF8(reminder.message.c_str());
            text += "\n\nDue: ";
            text += wxString::FromUTF8(reminder.dueLocal.c_str());

            wxMessageDialog dlg(this, text, "LlamaBoss Reminder",
                                wxYES_NO | wxICON_INFORMATION | wxCENTRE);
            dlg.SetYesNoLabels("Snooze 10 min", "Dismiss");

            const int result = dlg.ShowModal();
            bool saved = false;
            if (result == wxID_YES) {
                saved = reminderStore.Snooze(reminder.id, 10 * 60);
            }
            else {
                // Closing the dialog counts as dismissing this occurrence.
                saved = reminderStore.Complete(reminder.id);
            }

            if (!saved && m_chatDisplay) {
                m_chatDisplay->DisplaySystemMessage(
                    "Reminder state could not be saved. It may appear again after restart.");
            }
        }
        reminderStore.EndPresentation();
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
                           m_modelSwitcher->GetConversationModelForSave(),
                           m_modelService->Server().GetLoadedModel(),
                           m_appState->GetThemeName(),
                           m_appState->GetCtxSize(),
                           m_appState->GetFontSize(),
                           m_appState->GetAgentDefaultOn(),
                           m_appState->GetContextMeterOn(),
                           m_appState->GetKvCacheQ8(),
                           m_appState->GetMtpEnabled(),
                           m_appState->GetTheme(),
                           m_appState->GetSecretsStore(),
                           m_appState->GetEndpointStore());

        const int dialogResult = LbShowModalWithScrim(*this, dlg);

        if (dialogResult != wxID_OK) return;

        const std::string connectionModelToUse = dlg.GetConnectionModelToUse();
        bool folderChanged             = dlg.WasModelsFolderChanged();
        bool modelChanged              = dlg.WasModelChanged();
        const bool themeChanged        = dlg.WasThemeChanged();
        bool ctxSizeChanged            = dlg.WasCtxSizeChanged();
        const bool fontSizeChanged     = dlg.WasFontSizeChanged();
        const bool agentDefaultChanged = dlg.WasAgentDefaultChanged();
        bool kvCacheQ8Changed          = dlg.WasKvCacheQ8Changed();
        bool mtpChanged                = dlg.WasMtpEnabledChanged();

        // ── Multi-window courtesy check ───────────────────────────
        // The folder / model / launch-arg branches below stop or
        // restart the shared llama-server, which kills any stream
        // another window has in flight.  Confirm once, up front,
        // before any of those branches persist settings — same
        // policy as the model pill's switch paths.  Declining zeroes
        // the server-affecting flags so every downstream consumer
        // (branch selection, context-anchor invalidation, the
        // visual-only announcement gate) sees a consistent "nothing
        // server-side changed" state; visual settings (theme, font,
        // agent default) still apply normally below.
        if (connectionModelToUse.empty() &&
            (folderChanged || modelChanged || ctxSizeChanged ||
             kvCacheQ8Changed || mtpChanged) &&
            m_modelService->AnyOtherWindowBusyOnLocalServer(this)) {
            const int r = wxMessageBox(
                "Another window is generating on the local model. Applying the "
                "model/server changes will interrupt it.\n\nApply anyway?",
                "Model Switch", wxYES_NO | wxICON_WARNING, this);
            if (r != wxYES) {
                folderChanged   = false;
                modelChanged    = false;
                ctxSizeChanged  = false;
                kvCacheQ8Changed = false;
                mtpChanged      = false;
                m_chatDisplay->DisplaySystemMessage(
                    "Model/server changes were not applied \xE2\x80\x94 "
                    "another window is generating. Visual settings were "
                    "applied.");
            }
        }

        // Persist every accepted launch-argument setting before branch
        // selection.  The folder-change branch takes precedence over model
        // and restart branches, so persisting only inside those later
        // branches would silently lose context/KV/MTP changes made in the
        // same Settings session as a models-folder change.
        if (ctxSizeChanged)
            m_appState->SetCtxSize(dlg.GetSelectedCtxSize());
        if (kvCacheQ8Changed)
            m_appState->SetKvCacheQ8(dlg.GetSelectedKvCacheQ8());
        if (mtpChanged)
            m_appState->SetMtpEnabled(dlg.GetSelectedMtpEnabled());

        // ── Models folder changed — unload and wait ──────────────────
        // The previously-loaded model's path may no longer be in scope
        // (new folder may not contain it, or not at that path). Autosave
        // any conversation, stop the server, and clear state. Don't
        // auto-start — user reopens Settings and explicitly picks a
        // model from the now-active folder. Takes precedence over
        // modelChanged: any combo auto-select that happened during the
        // folder swap isn't a deliberate user pick.
        if (!connectionModelToUse.empty()) {
            // Guided setup deliberately selects a remote model. Preserve the
            // conversation and use the picker path rather than the local reload.
            ClearPendingSend();
            m_modelSwitcher->SwitchToModel(connectionModelToUse);
            _userInputCtrl->SetFocus();
        }
        else if (folderChanged) {
            // Deliberate server action supersedes any lazy-load intent.
            m_modelSwitcher->ClearPendingDeferredModel();
            ClearPendingSend();

            // Do not clear the conversation after a failed save.
            if (!m_convController->SaveBeforeLeaving()) return;

            m_modelService->StopLocalServer();
            m_modelSwitcher->ClearConversationPreference();

            bool mc, ac;
            m_appState->UpdateSettings("", m_appState->GetApiUrl(), mc, ac);

            m_chatHistory->Clear();
            m_chatDisplay->Clear();
            m_attachments->Clear();
            _statusDot->SetConnected(false);
            if (_protocolChip) UpdateProtocolChip(ToolProtocol::Unknown);
            _activeProtocol = ToolProtocol::Unknown;
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
            // Deliberate server action supersedes any lazy-load intent.
            m_modelSwitcher->ClearPendingDeferredModel();
            ClearPendingSend();

            std::string newModel = dlg.GetSelectedModel();

            // Launch-argument settings were persisted before branch
            // selection, so MakeServerConfig() sees the accepted values.

            // Do not clear the conversation after a failed save.
            if (!m_convController->SaveBeforeLeaving()) return;

            m_modelSwitcher->SetConversationPreferredLocalModel(newModel);
            _statusDot->SetConnected(false);
            // Hide the chip until the new model passes detection, so
            // the old model's chip never shows on the new one.
            if (_protocolChip) UpdateProtocolChip(ToolProtocol::Unknown);
            // Also reset the active-protocol cache so the next request
            // defaults back to XML until detection confirms the new model.
            _activeProtocol = ToolProtocol::Unknown;
            // Clear the old conversation before showing the reload status,
            // so Clear() doesn't erase the "Loading <model>..." message.
            m_chatHistory->Clear();
            m_chatDisplay->Clear();
            m_attachments->Clear();
            m_modelSwitcher->UpdateModelLabel();
            m_convController->UpdateWindowTitle();

            m_chatDisplay->DisplaySystemMessage(
                "Loading " + ServerManager::ModelDisplayName(newModel) + "...");
            m_modelService->RequestLocalModel(
                newModel, m_appState->MakeServerConfig());
        }
        else if (ctxSizeChanged || kvCacheQ8Changed || mtpChanged) {
            // Deliberate server action supersedes any lazy-load intent.
            m_modelSwitcher->ClearPendingDeferredModel();
            ClearPendingSend();

            // Restart server with the same model but new launch args.
            // Values were persisted before branch selection.  History is
            // preserved — the user can keep reading while it reloads.

            if (!m_chatHistory->IsEmpty())
                m_convController->AutoSaveConversation();

            m_modelSwitcher->MarkServerNotReady();
            _statusDot->SetConnected(false);
            m_chatDisplay->DisplaySystemMessage(
                "Reloading with " +
                std::to_string(m_appState->GetCtxSize() / 1024) +
                "k context" +
                std::string(m_appState->GetKvCacheQ8()
                    ? ", q8 KV cache" : ", f16 KV cache") +
                std::string(m_appState->GetMtpEnabled()
                    ? ", MTP auto..." : ", MTP off..."));
            const std::string localModel =
                m_modelService->Server().GetLoadedModel();
            if (!localModel.empty() &&
                m_modelService->ResolveTarget().managed) {
                m_modelService->RequestLocalModel(
                    localModel, m_appState->MakeServerConfig());
            }
            else {
                _statusDot->SetConnected(m_modelSwitcher->IsServerReady());
                m_chatDisplay->DisplaySystemMessage(
                    "Local server settings will apply the next time a "
                    "local model is loaded.");
            }
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

        // Context meter toggle: apply live and unconditionally — cheap,
        // and SetContextMeterOn() no-ops when unchanged.  Visibility
        // flips inside RefreshContextMeter; the anchor bookkeeping never
        // stopped, so enabling mid-conversation is exact immediately.
        // This also re-prices the denominator after a ctx-size change.
        // Model/folder changes cleared the history above, so drop the
        // exact anchor too (OnServerReady re-invalidates on the next
        // load, but the folder-changed path stops the server without a
        // follow-up ready event).
        if (modelChanged || folderChanged) InvalidateContextAnchor();
        m_appState->SetContextMeterOn(dlg.GetSelectedContextMeter());
        RefreshContextMeter();

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
        LbAboutDialog dlg(this,
                          m_appState->GetTheme(),
                          LLAMABOSS_VERSION,
                          ServerManager::ModelDisplayName(m_appState->GetModel()),
                          m_appState->GetApiUrl(),
                          ServerManager::GetModelsDir(),
                          [this]() { return UpdateInstallBlocker(); });

        if (LbShowModalWithScrim(*this, dlg) == LbAboutDialog::ID_INSTALL)
            InstallUpdateAndQuit(dlg.GetInstallerPath());
    }

    // ── In-app update install ─────────────────────────────────────
    // Non-empty = a reason the installer must not start right now.
    // Checked when the user clicks Download and Install, and again right
    // before launch (a chat may have started during the download).
    wxString UpdateInstallBlocker()
    {
        if (IsBusy() ||
            (m_modelService && m_modelService->AnyOtherWindowBusy(this)))
            return "A chat is still running. Stop it or let it finish, "
                   "then install the update.";
        return wxString();
    }

    // Order matters -- every save/recovery decision happens BEFORE the
    // installer exists, so nothing after launch is allowed to veto:
    //   1. this window saves its conversation (with the normal Retry /
    //      Save Elsewhere / Keep Chat Open recovery); Keep Chat Open or a
    //      failed save aborts the update with nothing launched;
    //   2. every OTHER window closes (each saves, and may veto the same
    //      way); a veto aborts the update with nothing launched;
    //   3. the installer starts (per-user Inno Setup, no UAC prompt);
    //   4. this window closes with force=true.  Its conversation was
    //      saved durably in step 1 and nothing can run in between (the
    //      busy check passed), so the close cannot be vetoed out from
    //      under a running installer.  The app exits and ModelService
    //      stops llama-server; /CLOSEAPPLICATIONS covers anything slow to
    //      let go of files, and [Run] relaunches LlamaBoss afterwards.
    void InstallUpdateAndQuit(const std::wstring& installerPath)
    {
        const wxString blocker = UpdateInstallBlocker();
        if (!blocker.empty()) {
            m_chatDisplay->DisplaySystemMessage(
                "Update not installed: " + blocker.ToStdString() +
                " Open About to try again.");
            return;
        }

        if (!m_convController->SaveBeforeLeaving()) {
            m_chatDisplay->DisplaySystemMessage(
                "Update paused: this conversation could not be saved, so "
                "LlamaBoss stayed open. Fix the save problem, then open "
                "About to try again.");
            return;
        }

        std::vector<wxWindow*> others;
        for (wxWindow* w : wxTopLevelWindows) {
            if (w == this || w->GetParent() != nullptr) continue;
            if (!dynamic_cast<wxFrame*>(w) || w->IsBeingDeleted()) continue;
            others.push_back(w);
        }
        for (wxWindow* w : others) {
            if (!w->Close(/*force=*/false)) {
                m_chatDisplay->DisplaySystemMessage(
                    "Update paused: another LlamaBoss window could not close. "
                    "Close it manually, then open About to try again.");
                return;
            }
        }

        // Recovery dialogs in the other windows pump events; re-check that
        // nothing started while they were up.
        const wxString lateBlocker = UpdateInstallBlocker();
        if (!lateBlocker.empty()) {
            m_chatDisplay->DisplaySystemMessage(
                "Update not installed: " + lateBlocker.ToStdString() +
                " Open About to try again.");
            return;
        }

        std::string error;
        if (!UpdateInstaller::LaunchInstaller(installerPath, error)) {
            m_chatDisplay->DisplaySystemMessage("Update failed: " + error);
            return;
        }
        // Saved in step 1; force so the close cannot be vetoed now that the
        // installer is running (OnClose still re-saves, without a dialog).
        Close(/*force=*/true);
    }

    // ── Multiple windows ─────────────────────────────────────────
    // Every window is a full MyFrame borrowing the app-owned AppState
    // and ModelService; the ctor's ConsumeInitialBootstrap gate means
    // a new window never boots a second server — it joins the running
    // one (see the late-joiner sync in the ctor's CallAfter).
    void OpenNewWindow()
    {
        MyFrame* frame = nullptr;
        try {
            frame = new MyFrame();
        }
        catch (const std::exception& ex) {
            m_chatDisplay->DisplaySystemMessage(
                std::string("Could not open a new window: ") + ex.what());
            return;
        }

        // Cascade from the invoking window.  RestoreWindowState in the
        // ctor puts every window at the same saved position; without
        // this offset the new window lands exactly on top of this one
        // and looks like nothing happened.
        const wxPoint pos = GetPosition();
        frame->SetPosition(wxPoint(pos.x + 48, pos.y + 48));
        frame->Show();
    }

    void OnNewChat(wxCommandEvent&)
    {
        if (IsBusy()) return;

        const bool hadSkillAuthoring =
            m_skillDraftController && m_skillDraftController->HasActiveDesignSession();

        // Preserve the current chat when saving fails or recovery is cancelled.
        if (!m_convController->SaveBeforeLeaving()) return;

        // KV fast path: snapshot the outgoing conversation's slot state
        // before Clear().  Ownership-guarded no-op unless the slot holds
        // this conversation's KV and a generation ran since restore.
        // Routed through ModelService: skipped when another window is
        // mid-generation on the shared slot.
        m_modelService->SaveSlotStateForConversation(
            this, m_chatHistory->GetFilePath());

        m_chatHistory->Clear();
        // Fresh chat = no conversation claim; the first autosave will
        // claim the newly generated path.
        wxGetApp().GetConversationRegistry().SetCurrent(this, "");
        CancelPendingSendForConversationSwitch();
        RefreshProjectStrip();
        m_chatDisplay->Clear();
        if (hadSkillAuthoring) {
            m_chatDisplay->DisplaySystemMessage(
                "Skill design session cancelled because a new chat was started.");
        }
        m_attachments->Clear();
        m_modelSwitcher->AdoptActiveTargetForConversation();
        m_modelSwitcher->OnServiceStateChanged();
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
            LbIcons::ApplyAgentToggle(_agentToggleButton, m_appState->GetTheme(), m_agentModeEnabled);
        }

        if (auto* logger = m_appState->GetLogger())
            logger->information("New chat started");

        // Runs after the save-away above was queued; the slot-action
        // worker executes them in order.
        SchedulePromptPrewarm("new chat");
    }

    // App-level target/readiness changes, including remote synthesized ready
    // and local switch initiation.  The event carries only a state version;
    // the authoritative snapshot remains owned by ModelService.
    void OnModelServiceStateChanged(wxCommandEvent& event)
    {
        if (m_isClosing) return;

        const unsigned long eventVersion =
            static_cast<unsigned long>(event.GetExtraLong());
        if (eventVersion != m_modelService->GetStateVersion())
            return;

        m_modelSwitcher->OnServiceStateChanged();

        const ModelServiceChange change =
            static_cast<ModelServiceChange>(event.GetInt());
        const bool frameUsesActiveTarget =
            m_modelSwitcher->IsConversationTargetActive();
        const InferenceTarget target = m_modelService->ResolveTarget();

        if (frameUsesActiveTarget && m_modelService->IsServerReady() &&
            !target.managed) {
            _activeProtocol = m_modelService->GetActiveProtocol();
            if (_protocolChip) UpdateProtocolChip(_activeProtocol);
        }
        else if (change == ModelServiceChange::LoadingLocal ||
                 change == ModelServiceChange::ErrorLocal ||
                 change == ModelServiceChange::Stopped ||
                 !frameUsesActiveTarget) {
            _activeProtocol = ToolProtocol::Unknown;
            if (_protocolChip) UpdateProtocolChip(ToolProtocol::Unknown);
        }

        if (change != ModelServiceChange::Sync) {
            InvalidateContextAnchor();
            RefreshContextMeter();
        }
    }

    // ── Server lifecycle → delegate to ModelSwitcher ──────────────
    void OnServerReady(wxCommandEvent& event)
    {
        if (m_isClosing) return;

        // ModelService validates before rebroadcast, but the clone waits in
        // this frame's event queue.  A newer launch may start in that gap, so
        // validate again at the final mutation boundary.
        const ServerLaunchGeneration eventGeneration =
            GetServerEventGeneration(event);
        const ServerLaunchGeneration currentGeneration =
            m_modelService->Server().GetLaunchGeneration();
        if (eventGeneration == kInvalidServerLaunchGeneration ||
            eventGeneration != currentGeneration) {
            if (auto* logger = m_appState->GetLogger()) {
                logger->warning(
                    "Frame dropped stale server ready event: eventGeneration=" +
                    std::to_string(eventGeneration) +
                    " currentGeneration=" +
                    std::to_string(currentGeneration));
            }
            return;
        }

        // A remote transition stops the local process and invalidates its
        // generation, so this should be unreachable for accepted events. Keep
        // the guard as defense-in-depth against future transition paths.
        if (!m_modelService->ResolveTarget().managed) {
            if (auto* logger = m_appState->GetLogger())
                logger->information(
                    "Local server ready while a remote endpoint is active; "
                    "keeping the remote session.");
            return;
        }

        // Every frame receives the accepted service event, but only frames
        // whose current conversation prefers this target should run local
        // ready UI, protocol probing, or queued-send dispatch.
        if (!m_modelSwitcher->IsConversationTargetActive())
            return;

        // Immediately clear any stale protocol from the previously
        // loaded server before detection for this server completes, so
        // a request sent in the ready-to-probe-result window can't be
        // built with the old model's protocol.
        _activeProtocol = ToolProtocol::Unknown;
        if (_protocolChip) UpdateProtocolChip(ToolProtocol::Unknown);

        // Context meter: a (re)loaded server may carry a different model
        // and therefore a different tokenizer; an exact anchor priced by
        // the previous model no longer describes this one.  Fall back to
        // the byte heuristic until the first completed turn re-anchors.
        InvalidateContextAnchor();
        RefreshContextMeter();

        // ModelService captured the runtime --jinja state before clearing
        // per-load retry bookkeeping. A server that succeeded only after the
        // no-jinja fallback must force XML for this session.
        const bool serverJinjaEnabled = event.GetInt() != 0;

        // ModelService has already accepted this generation, cleared shared
        // retry state, installed the active local target, and marked the
        // service ready.  The frame now performs UI-only work.
        m_modelSwitcher->OnServerReady();

        // Kick off tool-protocol detection for the active (model,
        // mmproj) pair.  Cache hits resolve immediately (no thread);
        // fresh probes run /props + heuristic + smoke test on a worker
        // and post wxEVT_TOOL_PROTOCOL_DETECTED back to
        // OnToolProtocolDetected.  The chip stays hidden until the
        // result arrives.
        if (_protocolChip) {
            _protocolChip->Hide();
            _protocolChip->SetLabel("");
            if (auto* parent = _protocolChip->GetParent()) parent->Layout();
        }

        const std::string modelPath  = m_modelService->Server().GetLoadedModel();
        const std::string mmprojPath = m_modelService->Server().GetLoadedMmproj();
        const std::string baseUrl    = m_modelService->Server().GetBaseUrl();
        if (!modelPath.empty() && !baseUrl.empty()) {
            if (!KickOffToolProtocolDetection(
                    this, m_alive, baseUrl, modelPath, mmprojPath,
                    serverJinjaEnabled)) {
                // The probe never started.  Leaving _activeProtocol at
                // Unknown would fail silently: the request builder gets
                // no protocol, the tooltip never updates, and tool
                // calling just quietly does not happen.  Fall back to
                // the XML path, which every model can drive, and say so.
                _activeProtocol = ToolProtocol::Xml;
                UpdateProtocolChip(ToolProtocol::Xml);
                if (auto* logger = m_appState->GetLogger())
                    logger->warning(
                        "Tool protocol probe failed to start - "
                        "falling back to xml");
            }
        }

        // Lazy load: if the user hit Send while this model was loading, fire
        // the queued prompt now — but only if the model that became ready is
        // the one it was queued under.  A model switch between queueing and
        // ready leaves the prompt orphaned; drop it rather than send it to
        // the wrong model.
        //
        // The queued prompt does NOT fire here.  Detection always reports
        // through a queued event (even a cache hit), so at this point
        // _activeProtocol is still Unknown; sending now built an XML-shaped
        // request whose reply was later parsed as native once detection
        // landed mid-stream, silently dropping the model's tool call.
        // Wait for OnToolProtocolDetected instead.  If the probe failed to
        // start, the XML fallback above already resolved the protocol, so
        // send immediately.
        if (m_pendingSend.active) {
            const std::string queuedFor = m_pendingSend.modelPath;

            if (queuedFor.empty() ||
                !path_safety::SameModelPath(queuedFor, modelPath)) {
                ClearPendingSend();
                m_chatDisplay->DisplaySystemMessage(
                    "A different model finished loading than the one your "
                    "message was queued for. Your message is still in the "
                    "input box \xE2\x80\x94 press Send to use this model.");
            }
            else if (_activeProtocol != ToolProtocol::Unknown) {
                FlushPendingSend();
            }
            else {
                m_pendingSend.awaitingProtocol = true;   // stays "Queued"
                m_pendingSendProtocolTimer.StartOnce(
                    kPendingSendProtocolTimeoutMs);
            }
        }
    }

    // Handle the protocol-detection worker's result event.  Updates the
    // chip to "native" or "xml" with theme-appropriate colors and shows
    // it.  Logs the decision so server.log/llamaboss.log stays useful
    // for debugging which models passed detection and why.
    void OnToolProtocolDetected(wxThreadEvent& event)
    {
        if (m_isClosing) return;
        if (!_protocolChip) return;

        ProtocolDetectionResult r = event.GetPayload<ProtocolDetectionResult>();

        // Stale-result guard: if the user switched models between
        // probe kickoff and result, the modelPath on the event
        // won't match the currently-loaded model.  Drop it.
        if (r.modelPath != m_modelService->Server().GetLoadedModel()) {
            return;
        }

        if (auto* logger = m_appState->GetLogger()) {
            std::string line = std::string("Tool protocol detected: ") +
                ToolProtocolName(r.protocol) +
                (r.cacheHit ? " (cache hit)" : "") +
                " - " + r.reason;
            logger->information(line);
        }

        // Cache the result for the request builder.  The agent
        // controller reads this via the getActiveProtocol callback when
        // building each request body.
        _activeProtocol = r.protocol;

        UpdateProtocolChip(r.protocol);

        // A prompt queued behind this model's load was waiting for the
        // protocol (see OnServerReady).  Now the first request can be
        // built with the right prompt shape and tools array.
        if (m_pendingSend.active && m_pendingSend.awaitingProtocol) {
            if (!m_pendingSend.modelPath.empty() &&
                path_safety::SameModelPath(m_pendingSend.modelPath, r.modelPath)) {
                FlushPendingSend();
            }
        }

        // The prompt shape is known now.  An app launch or model switch
        // usually lands on an empty chat -- the most common "new chat".
        // (TryPromptPrewarm re-checks idle/empty when the timer fires.)
        SchedulePromptPrewarm("model ready");
    }

    // Apply protocol to the chip widget.  Called from
    // OnToolProtocolDetected; also safe to call with Unknown to
    // hide the chip.
    // ── Context meter helpers ──────────────────────────────────────

    // Drop the exact anchor and re-price the fallback estimate from the
    // (now-current) history.  Call whenever the conversation identity or
    // the tokenizer changes: New Chat, conversation load, model switch.
    void InvalidateContextAnchor()
    {
        m_lastTurnStats             = TurnStats{};
        m_chatTurnStats.clear();
        m_lastRequestBreakdown      = RequestBreakdown{};
        m_ctxAnchorPromptTokens     = -1;
        m_ctxAnchorCompletionTokens = 0;
        m_ctxAnchorExact            = false;
        m_ctxAnchorRevision         = 0;
        m_ctxHistoryEstimateTokens  = m_chatHistory
            ? FallbackContextEstimate(*m_chatHistory, CurrentContextWindow(),
                                      &m_ctxFallbackWouldElide)
            : 0;
        m_ctxEstimateHistory  = m_chatHistory.get();
        m_ctxEstimateRevision = m_chatHistory ? m_chatHistory->GetRevision() : 0;
    }

    // Append one TSV row per turn that carried an exact usage report:
    // the bytes we actually put on the wire versus the prompt_tokens
    // the server counted.  bytes_per_token is the measured ratio for
    // the running model; est_error_pct is how far the kBytesPerToken
    // heuristic would have been off on this same request.  Lands in
    // the conversation's chat folder as ctx_calibration.tsv.
    //
    // Deliberately best-effort and silent: an unsaved conversation has
    // no chat folder, and a logging failure must never disturb a turn.
    void LogContextCalibration(long long promptTokens)
    {
        if (!m_chatHistory || promptTokens <= 0) return;

        const size_t bytes = m_chatHistory->GetLastBuildRequestBytes();
        if (bytes == 0) return;

        const std::string dir =
            ChatHistory::GetChatFolder(m_chatHistory->GetFilePath());
        if (dir.empty()) return;

        const long long est =
            (long long)ChatHistory::EstimateTokensFromBytes(bytes);
        const double bpt   = (double)bytes / (double)promptTokens;
        const double errPc =
            ((double)(est - promptTokens) * 100.0) / (double)promptTokens;

        try {
            const std::filesystem::path p =
                std::filesystem::path(dir) / "ctx_calibration.tsv";

            // Request-makeup columns follow `elided` (read by "Export
            // with metrics...").  New columns go at the END so the
            // elision seeder's positions never move.  A log started by
            // an older build gets the new header repeated once
            // (export_metrics.h and turn_stats.tsv handle repeated
            // headers; the seeder skips the line as an unparsable row).
            static const char* const kHeader =
                "time\tmodel\treq_bytes\tprompt_tokens\test_tokens"
                "\tbytes_per_token\test_error_pct\telided"
                "\targs_elided\tsystem_bytes\ttools_bytes\tuser_bytes"
                "\tassistant_bytes\ttool_result_bytes\timage_bytes"
                "\treasoning_replay_bytes";
            const std::string pathKey = p.string();
            if (pathKey != m_ctxCalibHeaderCheckedPath ||
                !std::filesystem::exists(p)) {
                std::string lastHeader;
                if (std::filesystem::exists(p)) {
                    std::ifstream existing(p, std::ios::binary);
                    if (!existing) return;
                    std::string line;
                    while (std::getline(existing, line)) {
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        if (line.compare(0, 5, "time\t") == 0) lastHeader = line;
                    }
                    if (existing.bad()) return;
                }
                if (lastHeader != kHeader) {
                    std::ofstream h(p, std::ios::app);
                    if (!h) return;
                    h << kHeader << '\n';
                }
                m_ctxCalibHeaderCheckedPath = pathKey;
            }

            std::ofstream f(p, std::ios::app);
            if (!f) return;

            char nums[96];
            snprintf(nums, sizeof(nums), "%.3f\t%+.1f", bpt, errPc);

            f << wxDateTime::Now().FormatISOCombined(' ').ToStdString() << '\t'
              << (!m_chatHistory->GetLastBuildModel().empty()
                      ? m_chatHistory->GetLastBuildModel()
                      : (m_appState ? m_appState->GetModel() : std::string())) << '\t'
              << bytes         << '\t'
              << promptTokens  << '\t'
              << est           << '\t'
              << nums          << '\t'
              << m_chatHistory->GetLastBuildElidedCount() << '\t'
              << m_chatHistory->GetLastBuildArgsElidedCount() << '\t';
            const RequestBreakdown& b = m_chatHistory->GetLastBuildBreakdown();
            f << b.systemBytes     << '\t'
              << b.toolsBytes      << '\t'
              << b.userBytes       << '\t'
              << b.assistantBytes  << '\t'
              << b.toolResultBytes << '\t'
              << b.imageBytes      << '\t'
              << b.replayBytes
              << '\n';
        } catch (...) {
            // best-effort only
        }
    }

    // Reload m_lastTurnStats / m_chatTurnStats from the current chat's
    // turn_stats.tsv (same 1000-reply cap as the live list).  No file, no
    // folder (new chat) or an unreadable file leaves them empty.
    void RestoreTurnStatsFromLog()
    {
        if (!m_chatHistory || !m_chatHistory->HasFilePath()) return;
        const std::string dir = ChatHistory::GetChatFolder(m_chatHistory->GetFilePath());
        if (dir.empty()) return;
        try {
            const std::filesystem::path p =
                std::filesystem::path(path_safety::Utf8ToWide(dir)) / L"turn_stats.tsv";
            std::ifstream f(p, std::ios::binary);
            if (!f) return;
            std::ostringstream ss;
            ss << f.rdbuf();
            m_chatTurnStats = TurnStats::ParseTsv(ss.str(), 1000);
            if (!m_chatTurnStats.empty()) m_lastTurnStats = m_chatTurnStats.back();
        } catch (...) {
            m_chatTurnStats.clear();
            m_lastTurnStats = TurnStats{};
        }
    }

    // Append one row per completed reply to <chat folder>\turn_stats.tsv:
    // token counts, first-token / total time, generation and prompt speed,
    // and llama-server's own timings when present.  Columns are defined in
    // turn_stats.h.  Best-effort and silent, like LogContextCalibration.
    void LogTurnStats(const TurnStats& stats)
    {
        if (!m_chatHistory || stats.empty()) return;
        const std::string dir =
            ChatHistory::GetChatFolder(m_chatHistory->GetFilePath());
        if (dir.empty() || !wxDirExists(wxString::FromUTF8(dir))) return;

        try {
            const std::filesystem::path p =
                std::filesystem::path(path_safety::Utf8ToWide(dir)) / L"turn_stats.tsv";
            // Older logs have fewer columns. A repeated header starts the
            // new schema without rewriting their historical rows; ParseTsv
            // already supports header changes within an appended log.
            std::string lastHeader;
            if (std::filesystem::exists(p)) {
                std::ifstream existing(p, std::ios::binary);
                if (!existing) return;
                std::string line;
                while (std::getline(existing, line)) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.compare(0, 5, "time\t") == 0) lastHeader = line;
                }
                if (existing.bad()) return;
            }
            std::ofstream f(p, std::ios::app);
            if (!f) return;
            if (lastHeader != TurnStats::TsvHeader()) f << TurnStats::TsvHeader() << '\n';
            f << stats.TsvRow(
                     wxDateTime::Now().FormatISOCombined(' ').ToStdString(),
                     stats.modelId)
              << '\n';
        } catch (...) {
            // best-effort only
        }
    }

    // Tooltip for the ctx meter: what the number means, plus the last
    // reply's speed line once there is one.
    wxString BuildContextMeterTooltip() const
    {
        std::string tip =
            "Click for context details.\n\n"
            "Context window occupancy for the next request.\n"
            "Exact (from the server's reported token usage) after each "
            "completed turn; \"~\" marks a size-based estimate.\n"
            "Amber: past the elision threshold - older tool-result "
            "bodies are being trimmed from what the model sees "
            "(\"\xC2\xB7" "elided\" appears when the last request was "
            "trimmed).\n"
            "Red: the window is nearly full - responses may degrade; "
            "consider starting a new chat.\n"
            "Window size is the Context Length in Settings.";
        if (!m_lastTurnStats.empty()) {
            tip += "\n\nLast reply: " + m_lastTurnStats.OneLineSummary();
            if (!m_lastTurnStats.hasServerTimings)
                tip += "\n(Measured by LlamaBoss; includes network and provider time.)";
        }
        return wxString::FromUTF8(tip);
    }

    // Occupancy of the next request, exactly as the ctx meter shows it.
    // Context window the meter prices against (same source as
    // ComputeContextUsage).
    int CurrentContextWindow() const
    {
        if (m_modelSwitcher) return m_modelSwitcher->ConversationContextTokens();
        if (m_appState && m_appState->GetCtxSize() > 0) return m_appState->GetCtxSize();
        return 8192;
    }

    // Meter fallback when no exact usage anchor exists (endpoint sends no
    // usage, or the chat was just reopened).  A flat bytes-per-token
    // guess badly overstates long reopened chats, so this uses the
    // measured bytes-per-token for the conversation model (seeded from
    // ctx_calibration.tsv on load), capped at the elision budget the
    // next request will enforce; *wouldElide drives "·elided".
    long long FallbackContextEstimate(const ChatHistory& h, int window,
                                      bool* wouldElide) const
    {
        const std::string model = m_modelSwitcher
            ? m_modelSwitcher->GetConversationModelForSave()
            : (m_appState ? m_appState->GetModel() : std::string());
        return (long long)h.EstimateNextRequestTokens(model, window, wouldElide);
    }

    struct ContextUsage {
        int       window    = 8192;
        long long used      = 0;     // base + draft
        bool      exact     = false; // `used` as a whole is a server count
        bool      baseExact = false; // the history part is a server count
        long long draft     = 0;     // unsent composer text, estimated
    };
    ContextUsage ComputeContextUsage() const
    {
        ContextUsage u;
        if (m_modelSwitcher)
            u.window = m_modelSwitcher->ConversationContextTokens();
        else if (m_appState && m_appState->GetCtxSize() > 0)
            u.window = m_appState->GetCtxSize();

        if (m_ctxAnchorPromptTokens >= 0) {
            u.used      = m_ctxAnchorPromptTokens + m_ctxAnchorCompletionTokens;
            u.baseExact = m_ctxAnchorExact;
        } else {
            // No server count (endpoint sends no usage, or none yet):
            // re-price the history estimate whenever the history changed.
            const ChatHistory* h = m_chatHistory.get();
            if (!h) {
                m_ctxHistoryEstimateTokens = 0;
                m_ctxEstimateHistory = nullptr;
            } else if (h != m_ctxEstimateHistory ||
                       h->GetRevision() != m_ctxEstimateRevision) {
                m_ctxHistoryEstimateTokens =
                    FallbackContextEstimate(*h, u.window, &m_ctxFallbackWouldElide);
                m_ctxEstimateHistory  = h;
                m_ctxEstimateRevision = h->GetRevision();
            }
            u.used      = m_ctxHistoryEstimateTokens;
            u.baseExact = false;
        }
        u.exact = u.baseExact;

        // Pending composer text rides on top as an estimate.
        // GetLastPosition() is O(1) — deliberately NOT GetValue(),
        // which copies the whole buffer (see OnUserInputChanged).
        if (_userInputCtrl) {
            const long pendingChars = _userInputCtrl->GetLastPosition();
            if (pendingChars > 0) {
                u.draft = (long long)ChatHistory::EstimateTokensFromBytes(
                    (size_t)pendingChars);
                u.used += u.draft;
                // Server count + estimated draft is an estimate as a
                // whole; the meter shows "~" and the HUD says which part
                // is exact.
                u.exact = false;
            }
        }
        return u;
    }

    // Everything the context details panel shows, gathered from the meter
    // state, the last request breakdown and this chat's reply stats.
    HudInputs BuildHudInputs() const
    {
        const ContextUsage u = ComputeContextUsage();
        HudInputs in;
        in.ctxUsed         = u.used;
        in.ctxWindow       = u.window;
        in.ctxExact        = u.exact;
        in.ctxBaseExact    = u.baseExact;
        in.draftTokens     = u.draft;
        in.elisionFraction = ChatHistory::ElisionBudgetFraction();
        in.request         = m_lastRequestBreakdown;
        in.requestPromptTokens = m_lastTurnStats.promptTokens;
        // The HUD reads model identity from the recorded turn, never from
        // the current model pill or the app-global active target.
        in.last            = m_lastTurnStats;
        in.history         = m_chatTurnStats;
        return in;
    }

    // "[ Open log ]" for the current chat, or empty when it has no folder yet.
    std::function<void()> MakeOpenLogAction() const
    {
        const std::string chatDir = (m_chatHistory && m_chatHistory->HasFilePath())
            ? ChatHistory::GetChatFolder(m_chatHistory->GetFilePath())
            : std::string();
        if (chatDir.empty() || !wxDirExists(wxString::FromUTF8(chatDir)))
            return {};
        return [chatDir]() {
            const wxString log = wxString::FromUTF8(chatDir + "/turn_stats.tsv");
            if (!wxFileExists(log) || !wxLaunchDefaultApplication(log))
                wxLaunchDefaultApplication(wxString::FromUTF8(chatDir));
        };
    }

    // Click on the ctx meter: pin the context details panel to the chat
    // view's bottom-right corner, or close it if it is already open.
    void ToggleContextHud()
    {
        if (!m_appState || m_isClosing) return;
        if (m_contextHud) { CloseContextHud(); return; }

        ContextHud::Actions actions;
        actions.openLog = MakeOpenLogAction();
        actions.close   = [this]() { CloseContextHud(); };

        m_contextHud = new ContextHud(this, m_appState->GetTheme(),
                                      context_stats::BuildContextHudModel(BuildHudInputs()),
                                      std::move(actions));
        // Anchor to the visible transcript area: the control itself extends
        // under the clip edge by the hidden native scrollbar's width.
        m_contextHud->ShowInCorner(_chatClip ? static_cast<wxWindow*>(_chatClip)
                                             : static_cast<wxWindow*>(this));
        m_appState->SetContextHudOpen(true);   // reopened on next launch
#ifdef __WXMSW__
        m_contextHudVisTimer.Start(250);
#endif
    }

#ifdef __WXMSW__
    void SyncContextHudVisibility()
    {
        if (!m_contextHud || m_isClosing) return;
        const HWND fg = ::GetForegroundWindow();
        const bool ours = fg == static_cast<HWND>(GetHWND()) ||
                          fg == static_cast<HWND>(m_contextHud->GetHWND());
        const bool want = ours && !IsIconized();
        if (want != m_contextHud->IsShown()) {
            m_contextHud->Show(want);
            if (want) m_contextHud->Reposition();
        }
    }
#endif

    void CloseContextHud()
    {
        if (!m_contextHud) return;
#ifdef __WXMSW__
        m_contextHudVisTimer.Stop();
#endif
        ContextHud* hud = m_contextHud;
        m_contextHud = nullptr;
        // Only user actions (meter click, the panel's close button) reach
        // here; window teardown destroys the panel as a child instead, so
        // quitting with the panel open keeps it open next launch.
        if (m_appState) m_appState->SetContextHudOpen(false);
        hud->Hide();
        hud->Destroy();
    }

    // Live update: called at the end of every RefreshContextMeter (replies,
    // New Chat, loads, model switches, draft typing).
    void UpdateContextHud()
    {
        if (!m_contextHud || m_isClosing) return;
        m_contextHud->SetOpenLog(MakeOpenLogAction());
        m_contextHud->SetModel(context_stats::BuildContextHudModel(BuildHudInputs()));
    }

    void RefreshContextMeter()
    {
        if (!_ctxMeter || !m_appState) return;

        // The Settings toggle controls visibility only; the anchor
        // bookkeeping above runs unconditionally so flipping the meter
        // on mid-conversation is exact immediately.
        const bool wantShown = m_appState->GetContextMeterOn();
        if (_ctxMeter->IsShown() != wantShown) {
            _ctxMeter->Show(wantShown);
            if (_toolbarPanel) _toolbarPanel->Layout();
        }
        if (!wantShown) {
            // The meter is only the panel's toggle, not its data source:
            // an open HUD must keep following chat switches and replies
            // (it still has its own [ Close ]), so don't return early.
            UpdateContextHud();
            return;
        }

        const ContextUsage u = ComputeContextUsage();
        const int ctxTokens = u.window;
        const long long used = u.used;
        const bool exact = u.exact;

        const bool elided =
            m_chatHistory && (m_chatHistory->GetLastBuildElidedCount() > 0 ||
                              m_chatHistory->GetLastBuildArgsElidedCount() > 0 ||
                              (!u.baseExact && m_ctxFallbackWouldElide));

        auto fmtK = [](long long t) -> std::string {
            if (t < 1000) return std::to_string(t < 0 ? 0 : t);
            char buf[32];
            snprintf(buf, sizeof(buf), "%.1fk", (double)t / 1000.0);
            return std::string(buf);
        };

        std::string text = "ctx ";
        if (!exact) text += "~";
        text += fmtK(used);
        text += "/";
        text += fmtK((long long)ctxTokens);
        if (elided) text += " \xC2\xB7" "elided";   // " ·elided"

        // Color states keyed to the request builder's real thresholds:
        // amber exactly when BuildChatRequestJson starts eliding old
        // tool-result bodies; red when the window is nearly spent.
        const double frac =
            ctxTokens > 0 ? (double)used / (double)ctxTokens : 0.0;
        const ThemeData& t = m_appState->GetTheme();
        wxColour fg = t.textMuted;
        if (frac >= 0.90)
            fg = wxColour(224, 108, 117);           // red: degradation imminent
        else if (frac >= ChatHistory::ElisionBudgetFraction())
            fg = wxColour(224, 175, 104);           // amber: elision active zone

        // Clickable now: light up on hover like the other toolbar controls,
        // but only in the calm state -- amber/red keep their warning colour.
        if (m_ctxMeterHover && fg == t.textMuted)
            fg = LbInteractiveAccent(t);

        const wxString label = wxString::FromUTF8(text);
        bool changed = false;
        if (label != m_ctxMeterLastLabel) {
            _ctxMeter->SetLabel(label);
            m_ctxMeterLastLabel = label;
            changed = true;
        }
        if (_ctxMeter->GetForegroundColour() != fg) {
            _ctxMeter->SetForegroundColour(fg);
            changed = true;
        }
        _ctxMeter->SetBackgroundColour(t.bgToolbar);
        if (changed) _ctxMeter->Refresh();
        // Refreshed on every call (cheap) so the "Last reply" line follows
        // each completed turn even when the label text didn't change.
        {
            const wxString tip = BuildContextMeterTooltip();
            if (tip != m_ctxMeterLastTooltip) {
                _ctxMeter->SetToolTip(tip);
                m_ctxMeterLastTooltip = tip;
            }
        }
        UpdateContextHud();
    }

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

        const ServerLaunchGeneration eventGeneration =
            GetServerEventGeneration(event);
        const ServerLaunchGeneration currentGeneration =
            m_modelService->Server().GetLaunchGeneration();
        if (eventGeneration == kInvalidServerLaunchGeneration ||
            eventGeneration != currentGeneration) {
            if (auto* logger = m_appState->GetLogger()) {
                logger->warning(
                    "Frame dropped stale server error event: eventGeneration=" +
                    std::to_string(eventGeneration) +
                    " currentGeneration=" +
                    std::to_string(currentGeneration));
            }
            return;
        }

        if (!m_modelSwitcher->IsConversationTargetActive())
            return;

        std::string err = WxToUtf8(event.GetString());

        // Server failed permanently — also clear any chip from a
        // prior session so a stale "native" doesn't outlive its
        // model.
        if (_protocolChip) UpdateProtocolChip(ToolProtocol::Unknown);
        _activeProtocol = ToolProtocol::Unknown;
        m_modelSwitcher->OnServerError(err);

        // A prompt queued behind this (failed) load can never fire — drop it
        // and say so, rather than leaving it stuck.
        if (m_pendingSend.active) {
            ClearPendingSend();
            m_chatDisplay->DisplaySystemMessage(
                "Your queued message was not sent because the model failed "
                "to load. It is still in the input box.");
        }
    }

    int CalculateAutoInputHeight() const
    {
        if (!_userInputCtrl) return kInputMinHeightPx;

        constexpr int MAX_LINES_TO_SHOW = 5;
        const int lineHeight = _userInputCtrl->GetCharHeight() + 4;
        const int lines = _userInputCtrl->GetNumberOfLines();

        if (_userInputCtrl->IsEmpty() || lines <= 1)
            return std::max(kInputMinHeightPx, lineHeight);

        return std::max(lineHeight * std::min(lines, MAX_LINES_TO_SHOW),
                        kInputMinHeightPx);
    }

    void LayoutInputAreaOnly()
    {
        // The composer lives entirely inside _rightPanel.  A full frame
        // Layout() needlessly recalculates the toolbar, strips, sidebar,
        // and outer content hierarchy on every mouse-move event.
        if (_rightPanel && _rightPanel->GetSizer())
            _rightPanel->GetSizer()->Layout();
        if (_inputContainer && _inputContainer->GetSizer())
            _inputContainer->GetSizer()->Layout();
    }

    void OnUserInputChanged(wxCommandEvent&)
    {
        if (!_userInputCtrl || !_inputSizer) return;

        const bool hasExpandedContent =
            !_userInputCtrl->IsEmpty() &&
            _userInputCtrl->GetNumberOfLines() > 1;

        // The resize affordance is contextual: keep the compact one-line
        // composer visually clean, then reveal the handle as soon as the
        // text wraps or contains multiple lines.  Once the draft collapses
        // back to one line, discard any manual height floor so the composer
        // returns to its normal compact state and the handle can disappear.
        if (!hasExpandedContent && m_inputHeightOverride > 0) {
            m_inputHeightOverride = 0;
            m_appState->SetInputAreaHeight(0);
        }

        int newH = CalculateAutoInputHeight();

        // Manual drag override acts as a floor only while there is expanded
        // content to work with.  Content-driven auto-grow can still exceed
        // the dragged height.  Clamp against half the client height so a
        // large value cannot swallow the chat display after a window resize.
        if (hasExpandedContent && m_inputHeightOverride > 0) {
            const int maxH = std::max(kInputMinHeightPx,
                                      GetClientSize().y / 2);
            newH = std::max(newH, std::min(m_inputHeightOverride, maxH));
        }

        bool layoutNeeded = false;

        if (_inputSeparator &&
            _inputSeparator->IsShown() != hasExpandedContent) {
            _inputSeparator->Show(hasExpandedContent);
            layoutNeeded = true;
        }

        if (_userInputCtrl->GetMinSize().y != newH) {
            _userInputCtrl->SetMinSize(wxSize(-1, newH));
            layoutNeeded = true;
        }

        // Height and separator visibility are applied in one local layout,
        // avoiding a second layout pass when the draft crosses the one-line
        // / multi-line threshold.
        if (layoutNeeded)
            LayoutInputAreaOnly();

        // Context meter: re-price the pending composer text.  Cheap —
        // the refresh uses GetLastPosition() (O(1), no buffer copy) and
        // only touches the widget when the rendered label changes.
        RefreshContextMeter();
    }

    // Fast path used only while the separator is actively being dragged.
    // The content-driven floor was sampled on LEFT_DOWN, so there is no
    // need to call GetNumberOfLines(), IsEmpty(), or RefreshContextMeter()
    // for every mouse motion.  Typing cannot occur while this control owns
    // mouse capture, so the sampled floor remains valid for the drag.
    void ApplyInputDragHeight(int requestedHeight)
    {
        if (!_userInputCtrl) return;

        const int maxH = std::max(kInputMinHeightPx,
                                  GetClientSize().y / 2);
        const int clampedH = std::clamp(requestedHeight,
                                        kInputMinHeightPx, maxH);
        const int newOverride =
            (clampedH <= kInputMinHeightPx) ? 0 : clampedH;
        const int effectiveH = std::max(
            std::max(kInputMinHeightPx, m_inputDragAutoHeight),
            newOverride > 0 ? newOverride : kInputMinHeightPx);

        // Skip duplicate pixel positions generated by high-frequency mice.
        if (m_inputHeightOverride == newOverride &&
            _userInputCtrl->GetMinSize().y == effectiveH) {
            return;
        }

        m_inputHeightOverride = newOverride;
        if (_userInputCtrl->GetMinSize().y != effectiveH) {
            _userInputCtrl->SetMinSize(wxSize(-1, effectiveH));
            LayoutInputAreaOnly();
        }
    }

    // ── Composer drag-resize ───────────────────────────────────────
    // The separator above the input doubles as a vertical drag handle
    // (same pattern as ConversationSidebar's right-edge border: mouse
    // capture on LEFT_DOWN, screen-coordinate deltas on MOTION, persist
    // on LEFT_UP, bail on CAPTURE_LOST).  Drag up = taller composer.
    // Dragging back down to the base height — or double-clicking the
    // handle — clears the override and returns to pure auto-grow.
    void BindInputResizeHandle()
    {
        if (!_inputSeparator || !_userInputCtrl) return;

        _inputSeparator->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
            m_inputDragActive = true;
            m_inputDragStartY =
                _inputSeparator->ClientToScreen(e.GetPosition()).y;
            m_inputDragStartH = _userInputCtrl->GetSize().y;
            m_inputDragAutoHeight = CalculateAutoInputHeight();
            _inputSeparator->CaptureMouse();
        });
        _inputSeparator->Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
            if (!m_inputDragActive) return;
            const int screenY =
                _inputSeparator->ClientToScreen(e.GetPosition()).y;
            const int delta = m_inputDragStartY - screenY;   // up = grow
            ApplyInputDragHeight(m_inputDragStartH + delta);
        });
        _inputSeparator->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
            if (!m_inputDragActive) return;
            m_inputDragActive = false;
            if (_inputSeparator->HasCapture())
                _inputSeparator->ReleaseMouse();
            m_appState->SetInputAreaHeight(m_inputHeightOverride);
        });
        _inputSeparator->Bind(wxEVT_MOUSE_CAPTURE_LOST,
            [this](wxMouseCaptureLostEvent&) {
                m_inputDragActive = false;
            });

        // Double-click = reset to auto-grow, persisted immediately.
        _inputSeparator->Bind(wxEVT_LEFT_DCLICK, [this](wxMouseEvent&) {
            m_inputHeightOverride = 0;
            m_appState->SetInputAreaHeight(0);
            wxCommandEvent e(wxEVT_TEXT, _userInputCtrl->GetId());
            OnUserInputChanged(e);   // snap back to content-driven height
        });

        // A window shrink can strand a large override (persisted on a
        // taller monitor, say) above the half-height clamp.  Re-run the
        // height computation on resize; OnUserInputChanged only touches
        // layout when the effective height actually changes, so this is
        // a no-op in the steady state.
        Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
            e.Skip();
            if (m_inputHeightOverride > 0 && _userInputCtrl && _inputSizer) {
                wxCommandEvent te(wxEVT_TEXT, _userInputCtrl->GetId());
                OnUserInputChanged(te);
            }
        });
    }

    // Applies a manual composer height outside an active drag (startup
    // restore and other programmatic paths).  Those infrequent paths keep
    // the normal content-aware calculation; mouse motion uses the fast path
    // above instead.
    void ApplyInputHeightOverride(int h)
    {
        m_inputHeightOverride = (h <= kInputMinHeightPx) ? 0 : h;
        wxCommandEvent e(wxEVT_TEXT, _userInputCtrl->GetId());
        OnUserInputChanged(e);
    }

    void OnCharHook(wxKeyEvent& evt)
    {
        if (evt.ControlDown() && evt.ShiftDown() && !evt.AltDown() &&
            (evt.GetKeyCode() == 'V' || evt.GetKeyCode() == 'v') &&
            wxWindow::FindFocus() == _userInputCtrl &&
            _userInputCtrl->IsEnabled() && _userInputCtrl->IsEditable()) {
            _userInputCtrl->PasteAsText();
            return;
        }
        if (evt.ControlDown()) {
            switch (evt.GetKeyCode()) {
            case 'N':
                // Ctrl+Shift+N: new WINDOW.  Deliberately no IsBusy
                // gate — opening another window while this one is
                // mid-generation is precisely the point.
                if (evt.ShiftDown()) { OpenNewWindow(); return; }
                // Ctrl+N: new chat (checks IsBusy internally).
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

    bool TryPasteLargeTextFromClipboard()
    {
        if (!_userInputCtrl || !m_attachments || !m_chatHistory ||
            IsBusy() || m_isClosing || !wxTheClipboard->Open())
            return false;

        // Close the clipboard before any file I/O, callbacks, or dialogs.
        wxTextDataObject clipboardText;
        const bool hasText = wxTheClipboard->IsSupported(wxDF_UNICODETEXT) ||
                             wxTheClipboard->IsSupported(wxDF_TEXT);
        const bool gotText = hasText && wxTheClipboard->GetData(clipboardText);
        wxTheClipboard->Close();
        if (!gotText) return false;

        const wxScopedCharBuffer utf8 = clipboardText.GetText().ToUTF8();
        if (!utf8.data()) return false;
        const size_t byteCount = utf8.length();
        if (byteCount <= varstore::DemotionConfig{}.thresholdBytes)
            return false; // native paste preserves normal selection/undo

        auto reportFailure = [this](const wxString& reason) {
            wxMessageBox(reason +
                "\n\nYour draft and clipboard have not been changed. "
                "Ctrl+Shift+V pastes directly into the message instead.",
                "Paste Text Attachment", wxOK | wxICON_WARNING, this);
            return true; // handled: do not fall through to native paste
        };
        if (byteCount > AttachmentManager::kMaxPastedTextBytes)
            return reportFailure("This paste exceeds the 64 MiB attachment limit.");
        if (m_attachments->GetCount() >= AttachmentManager::kMaxAttachments)
            return reportFailure("Remove an attachment before adding another.");

        // Always use this conversation's Workspace, not a /cd override or
        // a project source directory. Use an absolute tool path so later
        // working-directory changes do not make the attachment unreachable.
        EnsureConversationChatFolder();
        const wxString directory = wxString::FromUTF8(
            ChatHistory::GetConversationWorkspaceDir(m_chatHistory->GetFilePath()));
        if (!wxDirExists(directory) &&
            !wxFileName::Mkdir(directory, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL))
            return reportFailure("Could not create the folder for this attachment.");

        const wxString stem = "Pasted-text-" +
            wxDateTime::Now().Format("%Y%m%d-%H%M%S");
        wxString filePath;
        wxFile file;
        for (int suffix = 0; suffix < 10000; ++suffix) {
            const wxString name = stem +
                (suffix == 0 ? wxString() : wxString::Format("-%d", suffix)) +
                ".txt";
            const wxString candidate = wxFileName(directory, name).GetFullPath();
            if (wxFileExists(candidate) || wxDirExists(candidate)) continue;
            // Exclusive creation: even a competing writer cannot be overwritten.
            if (file.Create(candidate, false)) {
                filePath = candidate;
                break;
            }
            if (!wxFileExists(candidate) && !wxDirExists(candidate))
                return reportFailure("Could not save the pasted text attachment.");
        }
        if (filePath.empty())
            return reportFailure("Could not choose a filename for this attachment.");

        auto discardUnattachedFile = [this, &filePath]() {
            if (!wxRemoveFile(filePath))
                m_chatHistory->NoteWorkspaceSideEffect();
        };
        const bool wroteAll = file.Write(utf8.data(), byteCount) == byteCount;
        const bool closed = file.Close();
        if (!wroteAll || !closed) {
            discardUnattachedFile();
            return reportFailure("Could not finish saving the pasted text attachment.");
        }

        // Length-aware construction preserves UTF-8 bytes, whitespace, and
        // line endings; the existing draft (including selection) is untouched.
        const std::string content(utf8.data(), byteCount);
        if (!m_attachments->AttachPastedText(content, WxToUtf8(filePath))) {
            discardUnattachedFile();
            return reportFailure("Could not add the pasted text attachment.");
        }
        m_chatHistory->NoteWorkspaceSideEffect();
        _userInputCtrl->SetFocus();
        return true;
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
        // Approval is a special busy state.  The input is enabled only
        // for /approve or /deny; ordinary messages wait until the
        // pending tool is resolved.
        if (m_chatState == ChatState::AwaitingApproval) {
            if (userInput.empty()) return true;

            const auto action =
                lb_input_parsers::ParseApprovalInput(userInput);

            if (action ==
                lb_input_parsers::ApprovalInputAction::Unrecognized) {
                // Do NOT clear the composer (that would destroy the
                // user's typed draft), and use a system line rather than
                // a fake assistant turn that never reaches history (the
                // transcript and saved conversation would diverge on
                // reload).
                m_chatDisplay->DisplaySystemMessage(
                    "Approval is still pending. Use the buttons above, "
                    "or type approve / allow once / deny. Your draft is "
                    "kept in the input box.");
                _userInputCtrl->SetFocus();
                return true;
            }

            _userInputCtrl->Clear();
            { wxCommandEvent e(wxEVT_TEXT, _userInputCtrl->GetId());
              OnUserInputChanged(e); }

            switch (action) {
            case lb_input_parsers::ApprovalInputAction::ApproveOnce:
                HandleApprovalCommand(true, /*rememberForChat=*/false);
                break;
            case lb_input_parsers::ApprovalInputAction::ApproveAlways:
                HandleApprovalCommand(true, /*rememberForChat=*/true);
                break;
            case lb_input_parsers::ApprovalInputAction::Deny:
                HandleApprovalCommand(false);
                break;
            default:
                break;
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

        // -- /agent_steps -- agent tool-step safety cap ------------
        // Not a tool: mutates the persisted AppState setting and the
        // live AgentController cap.  Bare "/agent_steps" reports the
        // current value; "/agent_steps <n>" sets it (clamped 4..60).
        if (!hasAttachments && userInput.rfind("/agent_steps", 0) == 0 &&
            (userInput.size() == 12 ||
             userInput[12] == ' ' || userInput[12] == '\t' ||
             userInput[12] == '\n' || userInput[12] == '\r')) {
            std::string rest = (userInput.size() > 12)
                ? userInput.substr(13) : std::string();

            _userInputCtrl->Clear();
            { wxCommandEvent e(wxEVT_TEXT, _userInputCtrl->GetId());
              OnUserInputChanged(e); }

            // Trim
            size_t a = rest.find_first_not_of(" \t\r\n");
            size_t b = rest.find_last_not_of(" \t\r\n");
            std::string token = (a == std::string::npos)
                ? std::string() : rest.substr(a, b - a + 1);

            if (token.empty()) {
                m_chatDisplay->DisplaySystemMessage(
                    "Agent tool-step cap: " +
                    std::to_string(m_appState->GetAgentMaxToolSteps()) +
                    " steps per turn. Use /agent_steps <n> (4-60) to change it.");
                return true;
            }

            bool allDigits = !token.empty();
            for (char c : token) {
                if (c < '0' || c > '9') { allDigits = false; break; }
            }
            // Manual accumulation (token is verified all-digits) so no
            // extra <cstdlib> include is needed; cap well above the
            // clamp range so absurd inputs can't overflow.
            long parsed = 0;
            if (allDigits) {
                for (char c : token) {
                    parsed = parsed * 10 + (c - '0');
                    if (parsed > 100000) { parsed = 100000; break; }
                }
            }
            if (!allDigits || parsed <= 0) {
                m_chatDisplay->DisplaySystemMessage(
                    "Usage: /agent_steps <n> where n is 4-60. "
                    "Current: " +
                    std::to_string(m_appState->GetAgentMaxToolSteps()) + ".");
                return true;
            }

            const int requested = (int)parsed;
            m_appState->SetAgentMaxToolSteps(requested);
            const int applied = m_appState->GetAgentMaxToolSteps();
            if (m_agentController)
                m_agentController->SetMaxToolSteps(applied);

            std::string note = "Agent tool-step cap set to " +
                std::to_string(applied) + " steps per turn.";
            if (applied != requested) {
                note += " (Requested " + std::to_string(requested) +
                        " was clamped to the supported 4-60 range.)";
            }
            m_chatDisplay->DisplaySystemMessage(note);
            return true;
        }

        // -- /think -- per-conversation reasoning override ----------
        // Not a tool: mutates per-conversation ChatHistory state, like
        // /cd.  Bare "/think" reports the current mode; "/think
        // on|off|auto|low|medium|high" sets it.  Auto sends nothing on
        // the wire (the historical request shape); everything else asks
        // the backend to enable or suppress model reasoning.  Local
        // llama-server targets get chat_template_kwargs.enable_thinking
        // (honored by hybrid reasoning models such as Qwen3 under
        // --jinja, ignored by templates that never reference it;
        // boolean only, so any effort level enables).  Remote targets
        // branch on the endpoint's resolved reasoning dialect — the
        // OpenRouter-style reasoning object, or direct OpenAI's
        // reasoning_effort string where off maps to "none" (which also
        // unblocks function tools on reasoning models there).  See
        // ChatHistory::ThinkOverride for the full mapping.
        if (!hasAttachments && userInput.rfind("/think", 0) == 0 &&
            (userInput.size() == 6 ||
             userInput[6] == ' ' || userInput[6] == '\t' ||
             userInput[6] == '\n' || userInput[6] == '\r')) {
            std::string rest = (userInput.size() > 6)
                ? userInput.substr(7) : std::string();

            _userInputCtrl->Clear();
            { wxCommandEvent e(wxEVT_TEXT, _userInputCtrl->GetId());
              OnUserInputChanged(e); }

            // Trim + ASCII-lowercase (no <cctype> dependency).
            size_t a = rest.find_first_not_of(" \t\r\n");
            size_t b = rest.find_last_not_of(" \t\r\n");
            std::string token = (a == std::string::npos)
                ? std::string() : rest.substr(a, b - a + 1);
            for (char& c : token)
                if (c >= 'A' && c <= 'Z') c = (char)(c + 32);

            if (token.empty()) {
                m_chatDisplay->DisplaySystemMessage(
                    std::string("Thinking mode: ") +
                    ThinkingModeName(m_chatHistory->GetThinkOverride()) +
                    ". Use /think on | off | auto, or an effort level: "
                    "/think low | medium | high. "
                    "auto = backend default (nothing added to the "
                    "request); off requests no reasoning; "
                    "on/low/medium/high request thinking. Support "
                    "depends on the model and provider. Local models "
                    "treat any level as on.");
                return true;
            }

            ChatHistory::ThinkOverride mode;
            if      (token == "on")     mode = ChatHistory::ThinkOverride::On;
            else if (token == "off")    mode = ChatHistory::ThinkOverride::Off;
            else if (token == "auto")   mode = ChatHistory::ThinkOverride::Auto;
            else if (token == "low")    mode = ChatHistory::ThinkOverride::Low;
            else if (token == "medium") mode = ChatHistory::ThinkOverride::Medium;
            else if (token == "high")   mode = ChatHistory::ThinkOverride::High;
            else {
                m_chatDisplay->DisplaySystemMessage(
                    "Usage: /think on | off | auto | low | medium | "
                    "high. Current: " +
                    std::string(ThinkingModeName(m_chatHistory->GetThinkOverride())) +
                    ".");
                return true;
            }

            SetConversationThinking(mode);
            return true;
        }


        // ── /bench — model speed benchmark ───────────────────────
        if (!hasAttachments && userInput.rfind("/bench", 0) == 0 &&
            (userInput.size() == 6 ||
             userInput[6] == ' ' || userInput[6] == '\t' ||
             userInput[6] == '\n' || userInput[6] == '\r')) {
            _userInputCtrl->Clear();
            { wxCommandEvent ev(wxEVT_TEXT, _userInputCtrl->GetId());
              OnUserInputChanged(ev); }
            HandleBenchCommand(userInput.size() > 6 ? userInput.substr(7) : std::string());
            return true;
        }

        // ── Tool-shaped slash commands ────────────────────────────
        // Parsing lives in lb_input_parsers so MyFrame keeps the
        // execution/UI responsibilities while the command table stays
        // isolated and easier to test.
        if (!hasAttachments) {
            const auto slash = lb_input_parsers::TryParseToolSlashCommand(userInput);
            if (slash.matched) {
                _userInputCtrl->Clear();
                { wxCommandEvent ev(wxEVT_TEXT, _userInputCtrl->GetId());
                  OnUserInputChanged(ev); }

                HandleSlashCommand(slash.toolName, slash.args);
                return true;
            }
        }

        // ── Conversational Skill design-session routing ──
        // Ordinary messages must now reach the model so the user can design a
        // Skill through real back-and-forth discussion.  Only explicit Skill
        // authoring controls are intercepted here: cancel, or draft after the
        // conversation has clarified the intended Skill.
        if (!hasAttachments && m_skillDraftController->HasActiveDesignSession()) {
            const std::string trimmedSkillSetup = LbTrimAscii(userInput);

            if (LbSkillAuthoringInputCancelsSetup(trimmedSkillSetup)) {
                _userInputCtrl->Clear();
                { wxCommandEvent e(wxEVT_TEXT, _userInputCtrl->GetId());
                  OnUserInputChanged(e); }

                m_skillDraftController->ClearDesignSession();
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
                    m_skillDraftController->BuildPendingSkillDesignConversationBrief();
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
                        ServerManager::ModelDisplayName(
                            m_modelSwitcher->GetConversationModelForSave()),
                        reminder,
                        m_appState->GetTheme().chatAssistant);
                    m_chatHistory->AddAssistantMessage(
                        reminder,
                        m_modelSwitcher->GetConversationModelForSave());

                    if (!m_chatHistory->IsEmpty())
                        m_convController->AutoSaveConversation();
                    return true;
                }

                m_chatDisplay->DisplayUserMessage(userInput);

                _userInputCtrl->Clear();
                { wxCommandEvent e(wxEVT_TEXT, _userInputCtrl->GetId());
                  OnUserInputChanged(e); }

                m_chatHistory->AddUserMessage(userInput);
                m_skillDraftController->PrepareDraftFromDesignConversationBrief(
                    authoringBrief);

                const std::string handoff =
                    "Great — I’ll draft this Skill from our design conversation and choose the most practical implementation path.";
                m_chatDisplay->DisplayAssistantMessage(
                    ServerManager::ModelDisplayName(
                        m_modelSwitcher->GetConversationModelForSave()),
                    handoff,
                    m_appState->GetTheme().chatAssistant);
                m_chatHistory->AddAssistantMessage(
                    handoff,
                    m_modelSwitcher->GetConversationModelForSave());

                if (!m_chatHistory->IsEmpty())
                    m_convController->AutoSaveConversation();
                m_skillDraftController->BeginDraftBuildFromPendingDescription();
                return true;
            }
        }


        return false;
    }

    // Prepares a real chat turn after special routing falls through.
    void PrepareAndRecordUserTurn(std::string userInput,
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

            // Images are saved before this message is recorded, so on a
            // first turn the history has no title yet.  Name the chat
            // folder from the message being sent instead of leaving it
            // untitled.  No-op when the folder already exists.
            {
                std::string folderTitle = m_chatHistory->GetChatFolderTitle();
                if (folderTitle == "Untitled conversation")
                    folderTitle = ChatHistory::TitleFromUserText(userInput);
                ChatHistory::EnsureChatFolder(m_chatHistory->GetFilePath(), folderTitle);
            }

            std::string attachDir = ChatHistory::GetAttachmentDir(m_chatHistory->GetFilePath());
            std::string relDir = ChatHistory::GetAttachmentRelDir(m_chatHistory->GetFilePath());
            size_t msgIndex = m_chatHistory->GetMessageCount();
            m_attachments->SaveImagesToDisk(attachDir, relDir, msgIndex, attachInfo);
        }

        std::vector<std::string> imagePaths;
        if (m_attachments->HasImage()) {
            std::string chatDir = ChatHistory::GetChatFolder(m_chatHistory->GetFilePath());
            for (const auto& info : attachInfo) {
                if (info.kind == AttachmentInfo::Kind::Image && !info.storagePath.empty())
                    imagePaths.push_back(chatDir + "/" + info.storagePath);
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

        if (m_attachments->HasTextFile()) {
            // Decide at Send, not Paste: the user may toggle Agent mode or
            // choose a no-tools model while this attachment is pending.
            const InferenceTarget attachmentTarget =
                m_modelSwitcher->ResolveTargetForConversation();
            const bool usePastedFileReferences = m_agentModeEnabled &&
                !attachmentTarget.noTools && !attachmentTarget.imageOutput;
            userInput = m_attachments->BakeTextFilesIntoMessage(
                userInput, usePastedFileReferences);
        }
        if (m_attachments->HasTextFileRef())
            userInput = m_attachments->BakeTextFileRefsIntoMessage(userInput);
        if (m_attachments->HasPdfFile())
            userInput = m_attachments->BakePdfFilesIntoMessage(
                userInput, m_agentModeEnabled);
        if (m_attachments->HasSpreadsheetFile())
            userInput = m_attachments->BakeSpreadsheetFilesIntoMessage(
                userInput, m_agentModeEnabled);
        if (m_attachments->HasDocxFile())
            userInput = m_attachments->BakeDocxFilesIntoMessage(
                userInput, m_agentModeEnabled);
        if (m_attachments->HasCsvFile())
            userInput = m_attachments->BakeCsvFilesIntoMessage(
                userInput, m_agentModeEnabled);
        if (m_attachments->HasZipFile())
            userInput = m_attachments->BakeZipFilesIntoMessage(
                userInput, m_agentModeEnabled);

        // Per-turn ambient context: prepended AFTER the bakes so the
        // header is the first line of the wire message.  Stored in
        // history (stable KV prefix; temporal grounding of past turns).
        const std::string wireInput =
            BuildSessionContextHeader() + "\n\n" + userInput;

        m_chatHistory->AddUserMessage(wireInput, "", attachInfo);
    }

    void StartAssistantResponseForPreparedTurn()
    {
        std::string model =
            m_modelSwitcher->GetConversationModelForSave();

        // Resolve once so request policy and transport use the exact same
        // per-conversation target.  Two per-model flags bypass Agent mode:
        //   * image_output: providers reject tool-bearing image requests,
        //     and the body also needs modalities ["image","text"];
        //   * no_tools: a chat/reasoning-only model whose provider rejects
        //     function tools (for example direct OpenAI Luna with reasoning).
        //
        // Bypassing the complete Agent path matters: an empty native tools
        // catalog alone would still leave the XML tool prompt and agent loop
        // available on other protocols.  These models instead receive the
        // normal system prompt and an ordinary reasoning/chat request.
        const InferenceTarget target =
            m_modelSwitcher->ResolveTargetForConversation();
        const bool imageModel   = target.imageOutput;
        const bool noToolsModel = target.noTools;
        const bool agentToolsAllowed = !imageModel && !noToolsModel;

        // Tell the request builder which reasoning dialect the target
        // speaks BEFORE any body is built this turn, so /think emits
        // the right shape (direct OpenAI: reasoning_effort string;
        // everything else: the historical reasoning object).  Agent
        // iterations within the turn reuse the same pinned target, so
        // one set here covers the whole loop; the sendRequest lambda
        // refreshes it defensively as well.
        m_chatHistory->SetActiveReasoningDialect(target.reasoningDialect);
        // Same lifetime: lets the request builder attach the Responses
        // reasoning-replay sidecar only for Responses targets.
        m_chatHistory->SetActiveResponsesApi(target.responsesApi);

        // A conversation can restore Off or carry it across a model switch.
        // Normalize before the first body (including agent/tool requests),
        // update the chip, and tell the user what will actually be sent.
        if (!target.managed && !imageModel &&
            m_chatHistory->GetThinkOverride() == ChatHistory::ThinkOverride::Off &&
            lb_reasoning::RequiresReasoning(target.modelId,
                target.reasoningDialect == ReasoningDialect::OpenAIStyle)) {
            m_chatHistory->SetThinkOverride(ChatHistory::ThinkOverride::Low);
            RefreshThinkingSelector();
            m_chatDisplay->DisplaySystemMessage(
                "This model does not support Thinking Off. Changed this "
                "conversation to Low, its lowest supported reasoning effort.");
            if (m_convController) m_convController->AutoSaveConversation();
        }

        // Build the final request body only once.
        // Important: agent mode must add its system prompt BEFORE image injection.
        // If we inject images first and then rebuild the body for agent mode,
        // the rebuilt body loses the multimodal content array.
        std::string body;
        // Protocol snapshot for this first request; handed to the agent
        // controller after Begin() so the reply is parsed the same way the
        // request was built, even if detection resolves mid-stream.
        ToolProtocol firstTurnProto = ToolProtocol::Unknown;
        if (m_agentModeEnabled && agentToolsAllowed) {
            const int ctxTokens = m_modelSwitcher->ConversationContextTokens();
            m_chatHistory->SetElisionSpoolWorkspace(ResolveCurrentCwd());

            // Attach the tool catalog when the loaded model supports
            // native function calling.  The agent controller does the
            // same on subsequent iterations (see
            // AgentController::BuildRequestBody); this is the first
            // turn before the controller takes over the loop.
            firstTurnProto = _activeProtocol;
            std::string tools;
            const bool native = (firstTurnProto == ToolProtocol::Native);
            if (native) {
                tools = GetCachedToolsArrayJson();
            }

            body = m_chatHistory->BuildChatRequestJson(
                model,
                true,
                BuildAgentSystemPrompt(),
                ctxTokens,
                tools,
                native,
                true);
        }
        else {
            if (m_agentModeEnabled) {
                if (imageModel) {
                    m_chatDisplay->DisplaySystemMessage(
                        "image model: agent tools are disabled for this turn.");
                }
                else if (noToolsModel) {
                    m_chatDisplay->DisplaySystemMessage(
                        "chat/reasoning-only model: agent tools are disabled for this turn.");
                }
            }

            const int ctxTokens = m_modelSwitcher->ConversationContextTokens();
            m_chatHistory->SetElisionSpoolWorkspace(ResolveCurrentCwd());
            body = m_chatHistory->BuildChatRequestJson(
                model,
                true,
                BuildNormalSystemPrompt(),
                ctxTokens,
                /*toolsArrayJson*/ "",
                /*nativeProtocol*/ false,
                /*agentSamplingProfile*/ false,
                /*imageOutput*/ imageModel);
        }

        // Inject images after the final body shape is known, so agent
        // mode keeps image attachments on the first request.
        //
        // Skip when the carrier projection already emitted the
        // multimodal content array from the persisted attachments: the
        // injector's only remaining action on such a body is a full
        // Poco parse of the multi-MB request to find there is nothing
        // to do.  It stays as the fallback for what projection cannot
        // cover — no chat folder yet, or images without a persisted
        // storage_path.
        if (m_attachments->HasImage() &&
            !m_chatHistory->LastBuildProjectedImages())
            body = m_attachments->InjectImagesIntoRequest(body);

        if (auto* logger = m_appState->GetLogger())
            logger->debug("Request sent (" + std::to_string(body.size()) + " bytes)");

        const size_t requestMessageCount = m_chatHistory->GetMessageCount();

        m_chatHistory->AddAssistantPlaceholder(model);
        m_chatDisplay->DisplayAssistantPrefix(
            ServerManager::ModelDisplayName(model),
            m_appState->GetTheme().chatAssistant);

        // Persistence context for any file chips generated during this
        // response.  Ensures the conversation has a file path so the
        // sidecar dir is stable across app restarts.
        if (!m_chatHistory->HasFilePath())
            m_chatHistory->SetFilePath(ChatHistory::GenerateFilePath());

        // KV fast path: the slot is about to hold THIS conversation's
        // state.  Stamp ownership so switch-away knows a save is both
        // safe and worthwhile.  No-op on the remote lane (no loaded
        // local model — StopServer cleared it before going remote).
        // Routed through ModelService: if another window is
        // mid-generation, this request queues behind it and the stamp
        // is invalidated instead — see ModelService::NoteSlotOwner.
        m_modelService->NoteSlotOwner(this, m_chatHistory->GetFilePath());
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
        // streamed assistant reply as iteration 1.  Image/no-tools turns never
        // arm it — the request carried no tools, so treating the reply as an
        // agent iteration would only hold prose in the stream filter and log
        // a phantom loop.
        if (m_agentModeEnabled && agentToolsAllowed) {
            ResetAgentToolStreamFilter();
            m_agentController->SetConversationContextTokens(
                m_modelSwitcher->ConversationContextTokens());
            m_agentController->Begin();
            m_agentController->SetRequestProtocol(firstTurnProto);
        }

        DiscardPendingAssistantDelta();
        ++m_generationId;
        m_chatState = ChatState::Streaming;
        SetStreamingState(true);

        // Log the outbound body so the operator can verify whether a
        // `tools` array got attached for a native-protocol model.  Two
        // lines:
        //   * Request shape — a single grep-friendly summary
        //     ("tools=yes, messages=N, body=BYTES") that answers
        //     "is the wire shape correct?" without eyeballing JSON.
        //   * Outbound — the first ~2000 chars of the body for deeper
        //     inspection: enough for the agent system prompt (~1k
        //     chars) plus the head of the tools array.
        if (auto* logger = m_appState->GetLogger()) {
            // Cheap textual sniff — these substrings appear at the
            // top level of the JSON because Poco preserves insertion
            // order, but even if a future change moved them deeper
            // the substring search still answers correctly.  No
            // need to re-parse the body just to count.
            const bool hasTools = body.find("\"tools\":") != std::string::npos;

            const char* protoLabel =
                (_activeProtocol == ToolProtocol::Native) ? "native protocol" :
                (_activeProtocol == ToolProtocol::Xml)    ? "xml protocol"    :
                                                            "protocol unknown";

            logger->information(
                std::string("Request shape (") + protoLabel + "): "
                + "tools="    + (hasTools ? "yes" : "no")
                + ", messages=" + std::to_string(requestMessageCount)
                + ", body="     + std::to_string(body.size()) + " bytes");

            std::string preview = body.size() > 2000
                                      ? body.substr(0, 2000) + "...(truncated)"
                                      : body;
            logger->debug(
                std::string("Outbound chat projection (target ") + target.chatPath + "; " + protoLabel
                + "): " + preview);
        }

        if (!m_chatClient->SendMessage(target, body, m_generationId)) {

            if (m_agentController->IsActive()) {
                ResetAgentToolStreamFilter();
                m_agentController->HandleAssistantError("Failed to start chat request");
            }

            SetStreamingState(false);
            m_chatDisplay->DisplaySystemMessage("Failed to start chat request");
            m_chatHistory->RemoveLastAssistantMessage();
            return;
        }

        // Clear attachments only after the request is known to have started.
        // The final body already contains any injected image data; keeping the
        // attachment tray intact on SendMessage failure lets the user retry
        // without re-attaching files.
        m_attachments->Clear();
    }

    // Runs the post-gate portion of a user turn for an already-decided input
    // string (server is ready, no slash-routing/approval pre-checks pending).
    // Used to fire a prompt that was queued while a deferred model loaded —
    // see OnServerReady.  Mirrors the tail of OnSendMessage.
    void DispatchUserTurn(const std::string& userInput)
    {
        const bool hasAttachments = m_attachments->HasPending();
        if (userInput.empty() && !hasAttachments) return;
        if (TryHandleSpecialInputRouting(userInput, hasAttachments)) return;
        PrepareAndRecordUserTurn(userInput, hasAttachments);
        StartAssistantResponseForPreparedTurn();
    }

    void OnSendMessage(wxCommandEvent&)
    {
        if (m_activeAnimation) return;  // animation playing

        std::string userInput = WxToUtf8(_userInputCtrl->GetValue());

        // Trim leading whitespace so slash-commands (/cd, /yay) fire
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

        // The model is ready but the queued prompt is waiting for tool-
        // protocol detection (see OnServerReady).  IsServerReady() is
        // already true here, so without this guard an Enter press would
        // bypass the queue and send with an unresolved protocol.
        if (m_pendingSend.active && m_pendingSend.awaitingProtocol) {
            m_chatDisplay->DisplaySystemNotice(
                "Model is ready \xE2\x80\x94 finishing setup, your message "
                "will send in a moment.");
            return;
        }

        const bool hasAttachments = m_attachments->HasPending();

        // ── Multi-window queue notice ─────────────────────────────
        // llama-server runs a single slot, so if another window is
        // mid-generation this request waits silently inside the
        // server until that stream finishes.  Say so up front — a
        // send that appears to do nothing reads as a hang.  Local
        // managed lane only: remote endpoints have no shared slot.
        if ((!userInput.empty() || hasAttachments) &&
            m_modelSwitcher->IsServerReady() &&
            m_modelService->ResolveTarget().managed &&
            m_modelService->AnyOtherWindowBusyOnLocalServer(this)) {
            m_chatDisplay->DisplaySystemMessage(
                "Model busy in another window \xE2\x80\x94 this reply will "
                "start when that response finishes.");
        }

        if (!m_modelSwitcher->IsServerReady() &&
            m_modelSwitcher->NeedsRemoteActivationForConversation()) {
            if (userInput.empty() && !hasAttachments) return;
            if (!m_modelSwitcher->ActivateConversationPreferredRemoteTarget())
                return;
        }

        if (!m_modelSwitcher->IsServerReady()) {
            // Nothing to send — don't queue an empty turn.
            if (userInput.empty() && !hasAttachments) return;

            // A prompt is already queued behind a load in flight.  The
            // composer holds the text, so there is nothing to update —
            // just wait.  Checked first so repeated Sends (Enter key; the
            // button is disabled while queued) can never launch a second
            // server.
            if (m_pendingSend.active) {
                m_chatDisplay->DisplaySystemNotice(
                    "Model is still loading \xE2\x80\x94 your message will send "
                    "when it's ready.");
                return;
            }

            // Lazy load: opening a saved conversation parked its model in
            // ModelSwitcher instead of reloading it.  The user has
            // now sent a prompt, so load that model and queue this prompt to
            // fire from OnServerReady once it's up.

            // ── Multi-window courtesy check ───────────────────────
            // The deferred load below restarts llama-server, which
            // kills any stream another window has in flight — and
            // unlike the pill switch, the user's mental action here
            // was just "send", so the interruption is invisible
            // without this confirm.  Peek (don't Take) so declining
            // leaves the deferral parked and the composer text
            // untouched; the user can retry after the other window
            // finishes.
            {
                const std::string& parked =
                    m_modelSwitcher->PendingDeferredModel();
                if (!parked.empty() &&
                    wxFileExists(wxString::FromUTF8(parked)) &&
                    m_modelService->AnyOtherWindowBusyOnLocalServer(this)) {
                    const int r = wxMessageBox(
                        "Another window is generating on the local model. Loading "
                        "this conversation's model will interrupt it.\n\n"
                        "Load anyway?",
                        "Model Switch", wxYES_NO | wxICON_WARNING, this);
                    if (r != wxYES) return;
                }
            }

            const std::string deferred = m_modelSwitcher->TakePendingDeferredModel();
            if (!deferred.empty() &&
                wxFileExists(wxString::FromUTF8(deferred)))
            {
                m_pendingSend.active    = true;
                m_pendingSend.modelPath = deferred;
                SetPendingSendUi(true);   // text stays in the composer

                // A fresh model invalidates the previous tool protocol; reset
                // so the next request doesn't build with a stale capability.
                if (_protocolChip) UpdateProtocolChip(ToolProtocol::Unknown);
                _activeProtocol = ToolProtocol::Unknown;

                _statusDot->SetConnected(false);
                m_chatDisplay->DisplaySystemMessage(
                    "Loading " + ServerManager::ModelDisplayName(deferred) +
                    "\xE2\x80\xA6 your message will send when it's ready.");

                m_modelService->RequestLocalModel(
                    deferred, m_appState->MakeServerConfig());
                return;
            }
            if (!deferred.empty()) {
                m_chatDisplay->DisplaySystemMessage(
                    "The saved model for this conversation was not found. "
                    "Choose a model from Settings or the model pill, then send again.");
                return;
            }

            // Genuinely mid-load with nothing deferred (initial boot, or a
            // switch already in flight).  Queue behind that load the same
            // way the deferred path does, so "Send while loading" behaves
            // identically regardless of why the model is loading.  The
            // selection key is the path RequestLocalModel was called with.
            {
                const std::string inFlight =
                    m_modelService->GetActiveSelectionKey();
                if (!inFlight.empty() &&
                    m_modelService->ResolveTarget().managed) {
                    m_pendingSend.active    = true;
                    m_pendingSend.modelPath = inFlight;
                    SetPendingSendUi(true);
                    m_chatDisplay->DisplaySystemMessage(
                        "Loading " + ServerManager::ModelDisplayName(inFlight) +
                        "\xE2\x80\xA6 your message will send when it's ready.");
                    return;
                }
            }
            m_chatDisplay->DisplaySystemMessage(
                "Server is still loading the model. Please wait...");
            return;
        }

        if (userInput.empty() && !hasAttachments) return;

        if (TryHandleSpecialInputRouting(userInput, hasAttachments)) return;

        PrepareAndRecordUserTurn(std::move(userInput), hasAttachments);

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
    // Single classifying loop: each file is imported independently, so
    // one drop of several PDFs / spreadsheets / DOCX files imports all
    // of them, and a mixed drop (one PDF plus two screenshots) keeps
    // the images too.  A per-kind loop that returns early would lose
    // the rest.  PDF / spreadsheet / DOCX / image / text route through
    // their per-kind queue helpers; unknown extensions are ignored.
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
            // (VBA execution risk).  Surface a short explanation rather
            // than ignoring the drop, so it doesn't look like a broken
            // drop target.  anyAttached stays false: nothing landed as
            // an attachment.
            m_frame->NotifyDocmDropRejected(path);
        }
        else if (AttachmentManager::IsCsvFile(path)) {
            // Before IsTextFile by necessity: CSV routes through the
            // workspace-import path (csv_inspect hint), and IsTextFile
            // does not claim the extension.
            if (m_frame->QueueCsvAttachmentFromDrop(path))
                anyAttached = true;
        }
        else if (AttachmentManager::IsZipFile(path)) {
            // ZIP routes through the workspace-import path with a
            // zip_inspect hint; the archive is never decompressed on drop.
            if (m_frame->QueueZipAttachmentFromDrop(path))
                anyAttached = true;
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

// ─────────────────────────────────────────────────────────────────
// MyApp now lives in app.h / app.cpp.  This factory is the one seam
// the app TU needs: MyFrame's definition is file-local here, so the
// application object creates the main window without naming the type.
wxFrame* CreateMainFrame()
{
    return new MyFrame();
}
