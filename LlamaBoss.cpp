#define _CRT_SECURE_NO_WARNINGS

#include <cassert>
#include <cctype>
#include <cstdio>    // snprintf (context meter's k-formatter)
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
#include "app.h"
#include "model_service.h"
#include <fstream>
#include <algorithm>
#include <memory>
#include <functional>
#include <utility>
#include <filesystem>
#include <system_error>
#include <atomic>
#include <thread>
#include <mutex>
#include <stdexcept>
#include <chrono>
#include <cstdint>

// Poco headers for base64 and JSON
#include <Poco/Base64Encoder.h>
#include <Poco/Base64Decoder.h>   // generated-image data URL decode
#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Stringifier.h>
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
#include "tool_web_fetch.h"
#include "tool_call_parser.h"  // ToolCallStreamDetector for hiding raw <tool_call> blocks
#include "agent_controller.h"
#include "tool_protocol.h"     // Phase 3b: tool-call protocol detection
#include "tool_router.h"       // Phase 3c-i: BuildToolsArrayJson for native requests
#include "tool_approval.h"     // Phase 6 approval cards
#include "project_manager.h"   // Projects Phase 1/2
#include "project_attach_dialog.h"
#include "project_status_strip.h"
#include "lb_themed_dialogs.h"
#include "lb_input_parsers.h"
#include "lb_project_ui_actions.h"
#include "lb_update_ui.h"
#include "lb_modal_scrim.h"
#include "update_checker.h"

wxDEFINE_EVENT(wxEVT_UPDATE_CHECK_RESULT, wxThreadEvent);

// Ã¢â€â‚¬Ã¢â€â‚¬ File-local support modules (extracted helpers) Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
#include "lb_string_utils.h"
#include "skill_authoring_support.h"
#include "agent_prompt_builder.h"
#include "goal_verifier_support.h"
#include "python_package_recovery.h"
#include "artifact_presentation.h"
#include "drop_import_controller.h"

// Ã¢â€â‚¬Ã¢â€â‚¬ Extracted widget & coordinator headers Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
#include "widgets.h"
#include "chat_input_ctrl.h"
#include "chat_display_ctrl.h"
#include "ui_builder.h"
#include "model_switcher.h"
#include "conversation_controller.h"
#include "project_context_builder.h"
#include "tool_result_controller.h"
#include "goal_controller.h"
#include "skill_draft_controller.h"
#include "project_controller.h"
#include "ascii_animation.h"

// Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ Application version Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
static const char* LLAMABOSS_VERSION = "0.1.10";

// Native menu command ids. Keep above wxID_HIGHEST to avoid collisions
// with stock wxWidgets commands.
enum {
    ID_ANIMATION_TIMER = wxID_HIGHEST + 2000,
    ID_ASSISTANT_DELTA_FLUSH_TIMER,

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
    ID_SKILL_NEW_WITH_SCRIPT,
    ID_SKILL_OPEN,
    ID_SKILL_OPEN_FOLDER,
    ID_GOAL_SET,
    ID_GOAL_STATUS,
    ID_GOAL_PAUSE,
    ID_GOAL_RESUME,
    ID_GOAL_VERIFY,
    ID_GOAL_CLEAR
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

// Joins fire-and-forget background threads at process exit.
//
// CheckForUpdates() used to std::thread(...).detach().  If the user quit
// while the HTTP check was stalled, the leftover thread kept running
// through static destruction and could touch function-local statics
// (ui_event_post's mutex) mid-teardown -- the classic sporadic
// crash-on-exit that never reproduces under a debugger.
//
// Threads launched through the keeper behave exactly like detached ones
// while the app runs; the keeper's destructor joins them at exit.  The
// singleton is constructed lazily on first use (well after the statics
// those threads depend on), so reverse-order static destruction
// guarantees the join happens while everything they touch is still
// alive.  UpdateChecker::CheckBlocking has a hard 8 s network timeout,
// so the worst-case exit delay is bounded and small; typical exits see
// no delay because no check is in flight.
class LbBackgroundThreadKeeper
{
public:
    static LbBackgroundThreadKeeper& Instance()
    {
        static LbBackgroundThreadKeeper keeper;
        return keeper;
    }

    void Launch(std::function<void()> fn)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_threads.emplace_back(std::move(fn));
    }

    ~LbBackgroundThreadKeeper()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& t : m_threads) {
            if (t.joinable())
                t.join();
        }
    }

    LbBackgroundThreadKeeper(const LbBackgroundThreadKeeper&) = delete;
    LbBackgroundThreadKeeper& operator=(const LbBackgroundThreadKeeper&) = delete;

private:
    LbBackgroundThreadKeeper() = default;

    // Update checks are rare (gated by a per-frame in-flight flag), so the
    // vector holds at most a handful of entries per session; finished
    // threads join instantly at exit.
    std::mutex m_mutex;
    std::vector<std::thread> m_threads;
};

const wxColour& LbInteractiveAccentForTheme(const ThemeData& theme)
{
    // The original LlamaBoss Dark theme intentionally uses the mint assistant
    // color for small interactive highlights (paperclip, robot, + New Chat).
    // Other themes keep their author/palette-correct primary accent so assistant
    // body text can remain a true foreground color instead of driving UI chrome.
    return (theme.name == "dark") ? theme.chatAssistant : theme.accentButton;
}

} // namespace

// Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ Forward declaration Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
class MyFrame;

// Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ Drag-and-drop target for files Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
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

// Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â
//  Chat State Machine
// Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â
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

// Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â
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
        , m_grepExecutor(std::make_unique<GrepExecutor>(this, m_alive))
        , m_webFetchExecutor(std::make_unique<WebFetchExecutor>(this, m_alive))
        , m_toolWorker(std::make_unique<ToolWorkerExecutor>(this, m_alive))
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
        LbMarkUiEventTargetDead(m_alive);
        // The dead token above already blocks rebroadcasts at post
        // time; detaching just removes the stale registry row.
        if (m_modelService)
            m_modelService->DetachFrameSink(this);
        wxGetApp().GetConversationRegistry().Remove(this);

        StopAnimation();

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
        if (m_webFetchExecutor) m_webFetchExecutor->Cancel();
        if (m_toolWorker)       m_toolWorker->Cancel();

        m_isClosing = true;

        // Durable: the window (and its in-memory history) is going away,
        // so the file becomes the only copy.  No sidebar refresh — the
        // frame is closing.
        if (!m_chatHistory->IsEmpty())
            m_convController->AutoSaveConversation(false, /*durable=*/true);

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

    // Ã¢â€â‚¬Ã¢â€â‚¬ Public interface for attachments (used by drop target) Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
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
            const wxString limitText = wxString::FromUTF8(
                ProjectSource_HumanBytes(AttachmentManager::kMaxTextFileBytes));
            const wxString msg = wxString::FromUTF8("Text file too large (max ")
                + limitText + wxString::FromUTF8(").");
            wxMessageBox(msg, "Attachment Error", wxOK | wxICON_WARNING);
            return false;
        }

        bool ok = m_attachments->AttachTextFile(filePath);
        if (ok) RestoreComposerFocusDeferred();
        return ok;
    }

    // Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ Drag-and-drop document import routing Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
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

    // â”€â”€ Per-turn session context header â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
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

    // Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ Constructor setup phases Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    // Keep MyFrame construction order explicit while shrinking the
    // constructor body. These helpers intentionally remain in this .cpp
    // for now; no behavior should change in this Phase 1 refactor.
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
        m_modelService->AttachFrameSink(this, m_alive,
                                        [this]() { return IsBusy(); });

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
        Bind(wxEVT_MENU, &MyFrame::OnSkillNewWithScript, this, ID_SKILL_NEW_WITH_SCRIPT);
        Bind(wxEVT_MENU, &MyFrame::OnSkillOpen, this, ID_SKILL_OPEN);
        Bind(wxEVT_MENU, &MyFrame::OnSkillOpenFolder, this, ID_SKILL_OPEN_FOLDER);
        Bind(wxEVT_MENU, &MyFrame::OnProjectClear, this, ID_PROJECT_CLEAR);
        Bind(wxEVT_MENU, &MyFrame::OnProjectDelete, this, ID_PROJECT_DELETE);
        Bind(wxEVT_MENU, &MyFrame::OnGoalSet,    this, ID_GOAL_SET);
        Bind(wxEVT_MENU, &MyFrame::OnGoalStatus, this, ID_GOAL_STATUS);
        Bind(wxEVT_MENU, &MyFrame::OnGoalPause,  this, ID_GOAL_PAUSE);
        Bind(wxEVT_MENU, &MyFrame::OnGoalResume, this, ID_GOAL_RESUME);
        Bind(wxEVT_MENU, &MyFrame::OnGoalVerify, this, ID_GOAL_VERIFY);
        Bind(wxEVT_MENU, &MyFrame::OnGoalClear,  this, ID_GOAL_CLEAR);
    }


    void BuildMainLayout()
    {
        auto* mainSizer = new wxBoxSizer(wxVERTICAL);

        // Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ TOP BAR (via UIBuilder) Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
        auto tb = UIBuilder::BuildTopBar(this, mainSizer, m_appState->GetTheme());
        _toolbarPanel   = tb.toolbarPanel;
        _titleLabel     = tb.titleLabel;
        _modelPill      = tb.modelPill;
        _modelPillLeftBracket  = tb.modelPillLeftBracket;
        _modelLabel     = tb.modelLabel;
        _modelPillRightBracket = tb.modelPillRightBracket;
        _statusDot      = tb.statusDot;
        _protocolChip   = tb.protocolChip;
        _ctxMeter       = tb.ctxMeter;
        _sidebarToggle  = tb.sidebarToggle;
        _newChatButton  = tb.newChatButton;
        _settingsButton = tb.settingsButton;
        _aboutButton    = tb.aboutButton;
        _topSeparator   = tb.topSeparator;

        // Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ PROJECT STATUS STRIP Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
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
        stripCallbacks.onAttachRequested = [this]() {
            wxCommandEvent e;
            OnProjectAttach(e);
        };
        // Goal action ([ Goal v ]) opens a state-aware popup menu, matching
        // the project and skill affordances.  The menu items route through
        // the same HandleSlashGoal / DisplayGoalStatus paths the /goal
        // slash command uses, so behavior stays unified.
        stripCallbacks.onGoalMenuRequested = [this](wxWindow* anchor) {
            ShowGoalPopupMenu(anchor);
        };
        m_projectStrip = std::make_unique<ProjectStatusStrip>(
            this, m_appState->GetTheme(), stripCallbacks);
        mainSizer->Add(m_projectStrip->GetPanel(), 0, wxEXPAND);

        // NOTE: the old separate Goal status strip (BuildGoalStatusStrip)
        // has been merged into ProjectStatusStrip as the right-hand pair
        // on the same row.  RefreshGoalStatusStrip() is now a thin alias
        // for RefreshProjectStrip() so the existing ~17 call sites keep
        // working without churn.

        // Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ CONTENT AREA (sidebar + chat) Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
        _contentSizer = new wxBoxSizer(wxHORIZONTAL);

        // Ã¢â€â‚¬Ã¢â€â‚¬ Sidebar (collapsible conversation list) Ã¢â€â‚¬Ã¢â€â‚¬
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

        // Ã¢â€â‚¬Ã¢â€â‚¬ Right panel (chat display + input) Ã¢â€â‚¬Ã¢â€â‚¬
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
        rightSizer->Add(_chatDisplayCtrl, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);

        // Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ ATTACHMENT CHIP BAR (hidden by default) Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
        _attachChipBar = new wxPanel(_rightPanel, wxID_ANY);
        _attachChipBar->SetBackgroundColour(m_appState->GetTheme().bgMain);
        _attachChipSizer = new wxWrapSizer(wxHORIZONTAL);
        _attachChipBar->SetSizer(_attachChipSizer);
        _attachChipBar->Hide();
        rightSizer->Add(_attachChipBar, 0, wxLEFT | wxTOP, 12);

        m_attachments->SetLogger(m_appState->GetLogger());
        m_attachments->SetOnChanged([this]() { RebuildAttachmentChips(); });

        // Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ INPUT AREA (via UIBuilder) Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
        auto ia = UIBuilder::BuildInputArea(_rightPanel, rightSizer, m_appState->GetTheme());
        _inputContainer = ia.inputContainer;
        _inputSeparator = ia.inputSeparator;
        _userInputCtrl  = ia.userInputCtrl;
        _sendButton     = ia.sendButton;
        _stopButton     = ia.stopButton;
        _attachButton   = ia.attachButton;
        _inputSizer     = ia.inputSizer;

        // Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ Agent-mode toggle (Phase 4) Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
        // Sits right after the attach button in _inputSizer.  Visual
        // state: muted when off, interactive-accent-colored when on.  Click flips
        // m_agentModeEnabled and re-tints.
        _agentToggleButton = new wxButton(
            _inputContainer, wxID_ANY,
            wxString::FromUTF8("\xF0\x9F\xA4\x96"),   // Ã°Å¸Â¤â€“
            wxDefaultPosition, wxSize(44, 46),
            wxBORDER_NONE);
        _agentToggleButton->SetBackgroundColour(m_appState->GetTheme().bgInputArea);
        _agentToggleButton->SetForegroundColour(
            m_agentModeEnabled ? LbInteractiveAccentForTheme(m_appState->GetTheme())
                               : m_appState->GetTheme().textMuted);
        // 17pt keeps the robot visually balanced against the enlarged
        // 21pt paperclip in ui_builder (same ~0.8 ratio as the old
        // 14pt/18pt pairing).
        _agentToggleButton->SetFont(wxFont(wxFontInfo(17)));
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
        // Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ Setup fonts Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
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
        dropImportCallbacks.attachCsvFile =
            [this](const std::string& absPath, const std::string& relPath) {
                return m_attachments->AttachCsvFile(absPath, relPath);
            };
        dropImportCallbacks.attachZipFile =
            [this](const std::string& absPath, const std::string& relPath) {
                return m_attachments->AttachZipFile(absPath, relPath);
            };
        m_dropImportController =
            std::make_unique<DropImportController>(std::move(dropImportCallbacks));
    }


    void CreateControllersAndCallbacks()
    {
        _statusDot->SetColors(m_appState->GetTheme().accentButton,
                              m_appState->GetTheme().textMuted);

        // â”€â”€â”€ Project-context builder â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
        // Owns the cached project/Skills system-prompt block and the
        // brief status-strip count cache.  Created first so the agent /
        // skill callbacks below can route through it at runtime.
        m_projectContextBuilder =
            std::make_unique<ProjectContextBuilder>(m_chatHistory);

        // Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ Create agent controller (Phase 5) Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
        // Phase 5: MyFrame is the AgentEventSink Ã¢â‚¬â€ it receives
        // structured loop-progress events and translates them to UI
        // operations.  The Phase-4 ChatDisplay* slot is gone; tool
        // blocks now arrive via OnAgentToolBlock and are forwarded
        // to the display from there.  Callbacks are wired below,
        // after all coordinators are in place.
        m_agentController = std::make_unique<AgentController>(
            m_chatHistory,
            this,
            m_appState,
            m_grepExecutor.get(),
            m_cmdExecutor.get(),
            m_pythonRunner.get(),
            m_webFetchExecutor.get(),
            m_toolWorker.get());

        // Phase 10: apply the user-configurable tool-step cap persisted
        // by AppState.  /agent_steps updates both sides at runtime.
        m_agentController->SetMaxToolSteps(
            m_appState->GetAgentMaxToolSteps());

        // Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ Create coordinators Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
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
                    InvalidateContextAnchor();
                }
                RefreshContextMeter();
            },
            /*cancelPendingSend*/     [this]() {
                CancelPendingSendForConversationSwitch();
            }
        });

        // Initial strip render now that the controller can drive refreshes.
        RefreshProjectStrip();

        // Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ AgentController callbacks Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
        // Phase 5: Callbacks now contain only logic concerns
        // (sendRequest, buildToolContext, buildSystemPrompt,
        // bumpGenerationId, getActiveProtocol).  The Phase-4 UI-shaped
        // entries (beginNextIteration, onLoopEnd) moved into the
        // AgentEventSink methods further down (OnAgentIterationBegin,
        // OnAgentLoopEnd) Ã¢â‚¬â€ same body, cleaner separation.
        m_agentController->SetCallbacks({
            /*sendRequest*/ [this](const std::string& /*model*/,
                                   const std::string& body,
                                   unsigned long      genId) {
                return m_chatClient->SendMessage(
                    m_modelService->ResolveTarget(), body, genId);
            },
            /*buildToolContext*/ [this]() { return BuildToolContext(); },
            /*buildSystemPrompt*/ [this]() { return BuildAgentSystemPrompt(); },
            /*bumpGenerationId*/ [this]() {
                ++m_generationId;
                return m_generationId;
            },
            /*getActiveProtocol*/ [this]() { return _activeProtocol; },
        });

        // â”€â”€â”€ ToolResultController â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
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

        // GoalController owns the goal lifecycle and drives frame-owned
        // streaming/UI concerns through this callback seam.
        m_goalController = std::make_unique<GoalController>(
            m_chatHistory, m_chatDisplay.get(), *m_appState, *m_chatClient,
            *m_modelSwitcher, *m_agentController, *m_convController);

        m_goalController->SetCallbacks({
            /*isBusy*/             [this]() { return IsBusy(); },
            /*isClosing*/          [this]() { return m_isClosing; },
            /*isAgentModeEnabled*/ [this]() { return m_agentModeEnabled; },
            /*bumpGenerationId*/   [this]() { return ++m_generationId; },
            /*setChatStateStreaming*/ [this]() { m_chatState = ChatState::Streaming; },
            /*setStreamingUi*/     [this](bool s) { SetStreamingState(s); },
            /*discardPendingAssistantDelta*/ [this]() { DiscardPendingAssistantDelta(); },
            /*refreshGoalStatusStrip*/ [this]() { RefreshGoalStatusStrip(); },
            /*buildAgentSystemPrompt*/ [this]() { return BuildAgentSystemPrompt(); },
            /*skillsBlock*/        [this]() {
                std::ostringstream s;
                m_projectContextBuilder->AppendSkillsBlock(s);
                return s.str();
            },
            /*getActiveProtocol*/  [this]() { return _activeProtocol; },
            /*getCachedToolsArrayJson*/ [this]() { return GetCachedToolsArrayJson(); },
            /*resetAgentToolStreamFilter*/ [this]() { ResetAgentToolStreamFilter(); },
            /*callAfter*/          [this](std::function<void()> fn) {
                CallAfter([this, fn = std::move(fn)]() mutable {
                    if (m_isClosing) return;
                    fn();
                });
            },
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
        // Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ Bind events Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
        _sendButton->Bind(wxEVT_BUTTON, &MyFrame::OnSendMessage, this);
        _stopButton->Bind(wxEVT_BUTTON, &MyFrame::OnStopGeneration, this);

        // Animation timer
        Bind(wxEVT_TIMER, &MyFrame::OnAnimationTimer, this, m_animTimer.GetId());

        // Streamed assistant chunks can arrive very quickly from local models.
        // Batch them into one UI update per frame-ish interval so wxRichTextCtrl
        // does far less Freeze/Thaw/scroll work while still feeling live.
        Bind(wxEVT_TIMER, &MyFrame::OnAssistantDeltaFlushTimer, this,
             m_assistantDeltaFlushTimer.GetId());


        // Attach (Ã°Å¸â€œÅ½) button hover Ã¢â‚¬â€ use the theme's interactive accent.
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

        // Agent toggle Ã¢â‚¬â€ hover mirrors attach styling; click
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

        // Settings (Ã¢Å¡â„¢) button hover
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

        // New Chat (+) button hover Ã¢â‚¬â€ use the theme's interactive accent.
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


        // Sidebar/history toggle hover Ã¢â‚¬â€ match New Chat's interactive accent affordance.
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

        // Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ /cmd (Phase 1 tool executor) Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
        Bind(wxEVT_CMD_COMPLETE, &ToolResultController::OnCmdComplete, m_toolResultController.get());
        Bind(wxEVT_CMD_ERROR,    &ToolResultController::OnCmdError,    m_toolResultController.get());

        // Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ controlled Python helper runner Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
        Bind(wxEVT_PYTHON_COMPLETE, &ToolResultController::OnPythonComplete, m_toolResultController.get());
        Bind(wxEVT_PYTHON_ERROR,    &ToolResultController::OnPythonError,    m_toolResultController.get());

        // Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ /grep (Phase 3 threaded executor) Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
        Bind(wxEVT_GREP_COMPLETE, &ToolResultController::OnGrepComplete, m_toolResultController.get());
        Bind(wxEVT_WEB_FETCH_COMPLETE, &ToolResultController::OnWebFetchComplete, m_toolResultController.get());
        Bind(wxEVT_WEB_FETCH_ERROR,    &ToolResultController::OnWebFetchError,    m_toolResultController.get());
        Bind(wxEVT_TOOL_WORKER_COMPLETE, &ToolResultController::OnToolWorkerComplete, m_toolResultController.get());

        // Model pill click Ã¢â€ â€™ delegate to ModelSwitcher
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
        Bind(wxEVT_MODEL_SERVICE_STATE_CHANGED,
             &MyFrame::OnModelServiceStateChanged, this);
        Bind(wxEVT_SERVER_READY, &MyFrame::OnServerReady, this);
        Bind(wxEVT_SERVER_ERROR, &MyFrame::OnServerError, this);

        // Phase 3b: tool protocol detection result
        Bind(wxEVT_TOOL_PROTOCOL_DETECTED,
             &MyFrame::OnToolProtocolDetected, this);

        // Detached update-check worker result.  Posted through the
        // alive-token gate, never CallAfter on a raw frame pointer.
        Bind(wxEVT_UPDATE_CHECK_RESULT,
             &MyFrame::OnUpdateCheckThreadResult, this);
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

        // Keyboard shortcuts
        Bind(wxEVT_CHAR_HOOK, &MyFrame::OnCharHook, this);
    }


    void FinishStartup()
    {
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

    // Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ UI Controls Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
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
    std::atomic<bool> m_updateCheckInFlight{false};
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
    wxStaticText* _modelPillLeftBracket = nullptr;   // "[" Ã¢â‚¬â€ hover-recolored
    wxStaticText* _modelPillRightBracket = nullptr;  // "]" Ã¢â‚¬â€ hover-recolored
    StatusDot*    _statusDot;
    wxStaticText* _protocolChip;   // Phase 3b: native/xml chip beside model name
    ToolProtocol  _activeProtocol = ToolProtocol::Unknown;   // Phase 3c-i

    // â”€â”€ Context meter â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
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
    // Settings toggle â€” the toggle controls widget visibility only â€”
    // so enabling the meter mid-conversation shows an exact value
    // immediately.
    //
    // Known bounded staleness (accepted for v1): a tool-protocol flip
    // changes the next request's shape by the tools-catalog size, and
    // hidden goal/skill control turns do not update the anchor; both
    // self-correct on the next transcript turn.
    wxStaticText* _ctxMeter = nullptr;
    long long m_ctxAnchorPromptTokens     = -1;   // -1 = no anchor
    long long m_ctxAnchorCompletionTokens = 0;
    bool      m_ctxAnchorExact            = false;
    long long m_ctxHistoryEstimateTokens  = 0;    // fallback, cached at invalidation
    std::string m_ctxMeterHistoryPath;             // conversation-identity tracker
    wxString  m_ctxMeterLastLabel;                // skip redundant SetLabel churn

    // Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ Thread safety Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    std::shared_ptr<std::atomic<bool>> m_alive;
    unsigned long m_generationId;

    // Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ Application Components Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    // Non-owning: AppState is owned by MyApp (Chunk C) — one instance,
    // one wxFileConfig writer, shared by every window.  Initialized in
    // the ctor init list from wxGetApp(), so it is valid for the
    // frame's whole lifetime; MyApp's member order guarantees AppState
    // outlives ModelService, which outlives every frame.
    AppState*                      m_appState;
    std::unique_ptr<ChatClient>    m_chatClient;
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
    std::unique_ptr<GrepExecutor>  m_grepExecutor;
    std::unique_ptr<WebFetchExecutor> m_webFetchExecutor;
    std::unique_ptr<ToolWorkerExecutor> m_toolWorker;

    // Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ Coordinators Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    std::unique_ptr<ModelSwitcher>          m_modelSwitcher;
    std::unique_ptr<ConversationController> m_convController;
    std::unique_ptr<ProjectContextBuilder>  m_projectContextBuilder;
    std::unique_ptr<ToolResultController>   m_toolResultController;
    std::unique_ptr<AgentController>        m_agentController;
    std::unique_ptr<GoalController>         m_goalController;
    std::unique_ptr<SkillDraftController>   m_skillDraftController;
    std::unique_ptr<ProjectController>      m_projectController;
    std::unique_ptr<DropImportController>   m_dropImportController;

    // Project status strip Ã¢â‚¬â€ replaces the native menu bar; renders
    // current project state in a single line under the top toolbar.
    std::unique_ptr<ProjectStatusStrip>     m_projectStrip;

    // Agent mode Ã¢â‚¬â€ when true, the next user message begins an
    // agent loop via m_agentController->Begin().  Toggled by the
    // agent button on the input area.
    bool m_agentModeEnabled;

    // Lazy model loading (paired with ModelSwitcher deferred-model helpers).
    // Opening a saved conversation no longer reloads its model immediately;
    // the model is parked and loaded on the first Send.  If the user hits
    // Send before that model is ready, the typed prompt is stashed here and
    // fired automatically from OnServerReady once the matching model finishes
    // loading.  |modelPath| records which model the prompt was queued under,
    // so a model switch between queueing and ready drops the prompt instead
    // of misdirecting it.
    struct PendingSend {
        bool        active = false;
        std::string userInput;
        std::string modelPath;
    };
    PendingSend m_pendingSend;

    void CancelPendingSendForConversationSwitch()
    {
        m_pendingSend = PendingSend{};
        if (m_skillDraftController)
            m_skillDraftController->CancelForChatSwitch(/*notifyUser=*/false);
        if (m_goalController)
            m_goalController->ResetTransientState();
    }


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

    // Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â
    //  AgentEventSink implementation (Phase 5)
    // Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â
    //
    // The agent loop reports progress through these four hooks
    // instead of reaching into ChatDisplay or invoking UI lambdas.
    // Bodies are the same work the Phase-4 callbacks did Ã¢â‚¬â€ just
    // moved here so the controller stays UI-free.

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

    // Goals Phase 10 captures the typed AgentEvent stream for the active
    // goal; Phase 3 also tees the same stream to a per-loop JSONL trace.
    // Both observers run before the default sink bridge fans events back out
    // to existing UI callbacks, so renderer behavior stays unchanged.
    void OnAgentEvent(const AgentEvent& event) override
    {
        AppendAgentTraceEvent(event);
        m_goalController->RecordStructuredAgentEvidence(event);
        AgentEventSink::OnAgentEvent(event);
    }

    // No loop-scoped UI state today.  The user's message is on
    // screen and the first chat request is in flight by the time
    // Begin() runs, so there's nothing to set up here.  Hook is
    // kept for future loop-scoped indicators (a "thinkingÃ¢â‚¬Â¦" status,
    // a Stop-button enable, etc.).
    void OnAgentLoopBegin() override
    {
        // Fresh per-turn observation. A goal continuation should only spin
        // the outer verifier loop when real work occurred, unless this very
        // loop was itself launched automatically by the goal verifier.
        m_goalController->NoteAgentLoopBegin();
    }

    // Between iterations: the previous streaming worker has exited
    // (that's what fired wxEVT_ASSISTANT_COMPLETE), but
    // ChatClient::m_isStreaming stays true until someone clears it
    // Ã¢â‚¬â€ the normal-completion path inside OnAssistantComplete that
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

    // The controller emits one of these for every tool result Ã¢â‚¬â€
    // sync dispatches, async grep/cmd completions, malformed-call
    // errors.  Phase-5 plumbing forwards straight to ChatDisplay;
    // future P6 approval cards will intercept this seam to gate
    // dangerous results before they hit the chat.
    void OnAgentToolBlock(const ToolBlock& block,
                          bool startExpanded) override
    {
        m_goalController->NoteAgentToolOutput();
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
    // Normal and StreamError both arrive with empty messages Ã¢â‚¬â€
    // Normal because the model's final answer is the message, and
    // StreamError because OnAssistantError already showed friendly
    // error text before unwinding the loop.
    void OnAgentLoopEnd(AgentEndReason     reason,
                        const std::string& userFacingMessage) override
    {
        if (!userFacingMessage.empty()) {
            m_chatDisplay->DisplaySystemMessage(userFacingMessage);
        }

        const bool savedInterruptedGoal =
            m_goalController->NoteAgentLoopEnd(reason);

        m_chatClient->ResetStreamingState();
        ResetAgentToolStreamFilter();
        SetStreamingState(false);
        m_chatDisplay->ClearFilePersistenceContext();
        if (!savedInterruptedGoal && !m_chatHistory->IsEmpty())
            m_convController->AutoSaveConversation();

        m_goalController->MaybeScheduleVerificationAfterLoopEnd();
    }
    // Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ Chat state machine Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    ChatState m_chatState;

    // Ã¢â€â‚¬Ã¢â€â‚¬ ASCII Animation Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    wxTimer                          m_animTimer{this, ID_ANIMATION_TIMER};
    std::unique_ptr<AsciiAnimation>  m_activeAnimation;

    // Assistant streaming delta batcher.  The worker still posts deltas as
    // quickly as llama-server emits them, but the UI/history path receives
    // combined chunks at most about once per frame.
    wxTimer       m_assistantDeltaFlushTimer{this, ID_ASSISTANT_DELTA_FLUSH_TIMER};
    std::string   m_pendingAssistantDelta;
    unsigned long m_pendingAssistantDeltaGenerationId = 0;

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
    std::string           m_workflowRootEnsuredForFilePath;
    std::string           m_workspaceDirEnsuredForFilePath;

    // Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â
    //  HELPERS
    // Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â

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
        if (_protocolChip) UpdateProtocolChip(_activeProtocol);
        _newChatButton->SetBackgroundColour(t.bgToolbar);
        _newChatButton->SetForegroundColour(t.textMuted);
        _settingsButton->SetBackgroundColour(t.bgToolbar);
        _settingsButton->SetForegroundColour(t.textMuted);
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
            _attachChipSizer->Add(chip, 0, wxRIGHT | wxBOTTOM, 6);
        }

        _attachChipBar->Show();
        _attachChipBar->Layout();
        _rightPanel->GetSizer()->Layout();
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
               (m_agentController && m_agentController->IsActive());
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

    // Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â
    //  EVENT HANDLERS
    // Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â

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
        // CSV lives under Spreadsheets â€” it routes through
        // QueueCsvAttachmentFromDrop (workspace import + csv_inspect
        // hint), NOT through AttachTextFile.  IsTextFile no longer
        // claims the extension.
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
        // IsTextFile.  .docm is intentionally not auto-routed here even
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
        // contract-builder/verifier/Skill-draft deltas must not append onto
        // the previous assistant reply.
        if ((m_goalController && m_goalController->AnyHiddenTurnInFlight()) ||
            (m_skillDraftController && m_skillDraftController->AnyHiddenTurnInFlight())) return;

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
    // workflow-relative paths to the assistant message (sidecar,
    // survives save/load), and render thumbnails in the chat.
    //
    // Runs on the UI thread from OnAssistantComplete: the decode is
    // a few ms even for multi-MB images, and both the workflow-path
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

        ChatHistory::EnsureWorkflowDir(convPath);
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
            std::string ext = "png";
            if (header.rfind("image/", 0) == 0) {
                std::string sub = header.substr(6);
                const size_t semi = sub.find(';');
                if (semi != std::string::npos) sub = sub.substr(0, semi);
                if      (sub == "jpeg" || sub == "jpg") ext = "jpg";
                else if (sub == "webp")                 ext = "webp";
                else if (sub == "gif")                  ext = "gif";
                else if (sub == "png" || sub.empty())   ext = "png";
                else                                    ext = sub;
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
        // including the stale-generation guard and the hidden skill/goal
        // turn consumes below, frees the payload.  Image-generation turns
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
        if (m_skillDraftController->ConsumeAssistantComplete(fullResponse)) return;
        if (m_goalController->ConsumeAssistantComplete(fullResponse)) return;

        // Context meter: adopt the server's exact token usage for this
        // completed transcript turn as the occupancy anchor.  Placed
        // AFTER the hidden-turn consumes above on purpose â€” goal/skill
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
                RefreshContextMeter();
            }
        }

        // Phase 3 bugfix #3: extract native tool_calls before deciding
        // whether this assistant turn has visible UI text. Native
        // function-calling turns often complete with content == "" and
        // tool_calls != []; if we call DisplayAssistantComplete() first,
        // the chat renders an empty "model:" row before the tool card.
        std::string toolCallsJson;
        std::vector<std::string> imageDataUrls;
        if (payload) {
            toolCallsJson = payload->ToolCallsJson();
            imageDataUrls = payload->ImageDataUrls();
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
                 logger && toolCallsJson.empty() && imageDataUrls.empty()) {
            logger->warning("Assistant complete event arrived empty; keeping streamed content");
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

        // Ã¢â€â‚¬Ã¢â€â‚¬ Agent mode routing Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
        // If a loop is active and the controller consumed this
        // event (tool call found, loop continuing), skip the
        // normal "finalize and stop streaming" path Ã¢â‚¬â€ the next
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

        const std::string error = WxToUtf8(event.GetString());

        // Any assistant error ends the current stream. Preserve already-
        // streamed visible text so history and the on-screen transcript stay
        // consistent after reload. Hidden control turns still discard safely
        // inside FlushPendingAssistantDelta() instead of appending onto the
        // previous visible assistant message.
        FlushPendingAssistantDelta();

        if (m_skillDraftController->ConsumeAssistantError(error)) return;
        if (m_goalController->ConsumeAssistantError(error)) return;

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

    // Ã¢â€â‚¬Ã¢â€â‚¬ Slash-command handlers Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    // After Phase 4, tool-shaped slash commands all route through
    // HandleSlashCommand Ã¢â€ â€™ DispatchInvocation, the same path the agent
    // uses.  Stateful conversation commands keep their own handlers:
    //   - /cd mutates the per-conversation tool cwd.
    //   - /goal manages Goals Phase 1 mission state.
    //
    // /cd resolution: per-conversation tool CWD if set, else the
    // conversation workspace.  Env-var expansion (%USERPROFILE% etc.)
    // is handled inside ResolveToolPath.  AutoSave fires only if
    // the conversation has content Ã¢â‚¬â€ empty-conversation /cd lives
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

        const std::string convFilePath = m_chatHistory->GetFilePath();
        if (m_workflowRootEnsuredForFilePath != convFilePath) {
            ChatHistory::EnsureWorkflowDir(convFilePath);
            m_workflowRootEnsuredForFilePath = convFilePath;
        }
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
        // Trim surrounding whitespace Ã¢â‚¬â€ users sometimes paste paths
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


    // Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬ Unified slash-command dispatch (Phase 4 / 4.1) Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    //
    // Every tool-shaped slash command Ã¢â‚¬â€ /read, /ls, /grep, /pwd,
    // /open, /cmd (Phase 4) plus /write, /mkdir, /edit, /delete
    // (Phase 4.1) Ã¢â‚¬â€ flows through HandleSlashCommand below.  The
    // method builds a ToolInvocation, calls DispatchInvocation, and
    // either renders the sync result or sets the chat-state so the
    // matching OnGrepComplete / OnCmdComplete picks up the async
    // continuation.
    //
    // /cd and /goal are NOT tools Ã¢â‚¬â€ they mutate per-conversation state
    // and keep dedicated handlers.  Everything else that used to live in
    // HandleSlashRead/Ls/Grep/Open/Pwd is gone: dispatch,
    // validation, rendering, and history are now identical to the
    // agent path.
    //
    // toolCallId is always empty for slash invocations: there is no
    // model-emitted call to thread.  AddUserMessage (rather than
    // AddToolResultMessage) is therefore the correct persistence
    // call for the result Ã¢â‚¬â€ see RenderAndPersistSlashResult.
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
            m_pythonRunner.get(), m_webFetchExecutor.get());

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

        DispatchSlashInvocation(inv, ctx);
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
        if (approve && rememberForChat && m_chatHistory) {
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
    // current instant: resolves CWD (per-conv override Ã¢â€ â€™ app CWD),
    // resolves timeout (per-conv override Ã¢â€ â€™ kDefaultToolTimeoutMs),
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

    // Project-context build + cache moved to project_context_builder.{h,cpp};
    // frame uses m_projectContextBuilder (created in CreateControllersAndCallbacks).

    std::string BuildNormalSystemPrompt() const
    {
        AgentPromptBuilderInput input;
        input.activeProjectContextBlock = m_projectContextBuilder->BuildActiveProjectContextBlock();
        input.activeGoalContextBlock = m_goalController->BuildActiveGoalContextBlock();
        input.pendingSkillAuthoringContextBlock = m_skillDraftController->BuildPendingSkillAuthoringContextBlock();
        return ::BuildNormalSystemPrompt(input);
    }

    // Agent-mode system prompt.  Prepended to each iteration's
    // request while the loop is active; not stored in history so
    // saved conversations stay clean.  Kept short Ã¢â‚¬â€ small models
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
        input.activeProjectContextBlock = m_projectContextBuilder->BuildActiveProjectContextBlock();
        input.activeGoalContextBlock = m_goalController->BuildActiveGoalContextBlock();
        input.pendingSkillAuthoringContextBlock = m_skillDraftController->BuildPendingSkillAuthoringContextBlock();
        input.toolSafetySummaryText = BuildToolSafetySummaryText(GetGlobalRouter());

        if (_activeProtocol == ToolProtocol::Native) {
            return ::BuildAgentSystemPromptNative(input);
        }
        return ::BuildAgentSystemPromptXml(input);
    }


    // Native-protocol prompt: short.  The wire `tools` array
    // already teaches the model the tool names, descriptions,
    // and parameter schemas Ã¢â‚¬â€ repeating any of that in prose
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


    // Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â
    //  Project status strip helpers
    // Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â

    // Pulls the current project + goal state from ChatHistory and
    // pushes it into the merged ProjectStatusStrip in one call. Project
    // counts are exact, but cached briefly so goal-only refresh churn does
    // not repeatedly walk Sources/Workflows on the UI thread. Goal fields
    // are O(1) lookups on the in-memory GoalState.
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

        // Ã¢â€â‚¬Ã¢â€â‚¬ Project half Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
        if (m_chatHistory->HasProject()) {
            s.hasProject  = true;
            s.projectName = m_chatHistory->GetProjectName();

            const std::string root = m_chatHistory->GetProjectRoot();
            const ProjectStripCounts counts = m_projectContextBuilder->GetProjectStripCounts(root);
            s.sourceCount   = counts.sourceCount;
            s.workflowCount = counts.workflowCount;
            s.scriptCount   = counts.scriptCount;
        }

        // Ã¢â€â‚¬Ã¢â€â‚¬ Goal half Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
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
    // Project-scoped popup ([ Project Ã¢â€“Â¾ ] / right-click on the strip).
    // Project actions only -- Skills live on their own [ Skills Ã¢â€“Â¾ ] menu
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

    // Skills-scoped popup ([ Skills Ã¢â€“Â¾ ]).  Skills are global / cross-project,
    // so this menu is identical whether or not a project is attached.
    void ShowSkillPopupMenu(wxWindow* anchor)
    {
        wxMenu menu;
        menu.Append(ID_SKILL_NEW,         "New Skill...");
        menu.Append(ID_SKILL_OPEN,        "Open a Skill...");
        menu.Append(ID_SKILL_OPEN_FOLDER, "Open Skills Folder");

        PopupMenuAtAnchor(menu, anchor);
    }

    // Shared positioning for the strip popups.  PopupMenu off the frame so
    // wxEVT_MENU lands on the existing Bind() entries set up in the
    // constructor.  Anchored to the bottom-left of the clicked affordance
    // so the menu drops just below the [ Project Ã¢â€“Â¾ ] / [ Skills Ã¢â€“Â¾ ] token.
    void PopupMenuAtAnchor(wxMenu& menu, wxWindow* anchor)
    {
        wxPoint pos(0, anchor ? anchor->GetSize().GetHeight() : 0);
        if (anchor) {
            pos = anchor->ClientToScreen(pos);
            pos = ScreenToClient(pos);
        }
        PopupMenu(&menu, pos);
    }

    // Goal popup ([ Goal v ]).  State-aware.  Items map onto the existing
    // /goal subcommands so there's no parallel logic.  Two disabled header
    // rows echo the objective + status so the menu is self-describing
    // without opening Goal Details (which prints into the transcript).
    //
    // Action availability is matched to what the controller actually
    // accepts, so the menu never offers a command that would be rejected:
    //   * Verify Now  -> Active only (HandleSlashGoal "verify" requires an
    //     active goal; it errors out otherwise).
    //   * Resume Goal -> any non-active goal ("resume" only rejects "no
    //     goal" / "already active"), so Paused, AwaitingUser, and
    //     BudgetReached all get it; Resume() even resets the budget window.
    // The actions separator is emitted only when the current state has a
    // mid-run action, so states that fall through (Completed / Cancelled /
    // Failed) no longer render the old back-to-back double separator.
    void ShowGoalPopupMenu(wxWindow* anchor)
    {
        wxMenu menu;

        if (!m_chatHistory->HasGoal()) {
            menu.Append(ID_GOAL_SET, "Set a Goal...");
            PopupMenuAtAnchor(menu, anchor);
            return;
        }

        const GoalState& goal = m_chatHistory->GetGoalState();

        // Disabled informational header: objective + status at a glance.
        if (wxMenuItem* h = menu.Append(
                wxID_ANY,
                "Goal: " + wxString::FromUTF8(
                    LbCompactGoalStripText(goal.objective, 48))))
            h->Enable(false);
        if (wxMenuItem* s = menu.Append(
                wxID_ANY,
                wxString("Status: ") + GoalStatusLabel(goal.status) +
                "  \xC2\xB7  contract: " +
                GoalContractStatusLabel(goal.contract.status)))
            s->Enable(false);

        menu.AppendSeparator();
        menu.Append(ID_GOAL_STATUS, "Goal Details...");

        bool actionSep = false;
        auto ensureSep = [&] {
            if (!actionSep) { menu.AppendSeparator(); actionSep = true; }
        };

        switch (goal.status) {
        case GoalStatus::Active:
            ensureSep();
            menu.Append(ID_GOAL_PAUSE,  "Pause Goal");
            menu.Append(ID_GOAL_VERIFY, "Verify Now");
            break;
        case GoalStatus::Paused:
            ensureSep();
            menu.Append(ID_GOAL_RESUME, "Resume Goal");
            break;
        case GoalStatus::AwaitingUser:
            ensureSep();
            menu.Append(ID_GOAL_RESUME, "Resume Goal");
            break;
        case GoalStatus::BudgetReached:
            ensureSep();
            menu.Append(ID_GOAL_RESUME, "Resume Goal (reset budget)");
            break;
        default:
            // Completed / Cancelled / Failed / None: Details + Clear only.
            break;
        }

        menu.AppendSeparator();
        menu.Append(ID_GOAL_CLEAR, "Clear Goal");

        PopupMenuAtAnchor(menu, anchor);
    }


    // DeleteProjectByInfo() moved to ProjectController (project_controller.cpp).

    // MoveChatsToProject() moved to ProjectController (project_controller.cpp).

    // Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â
    //  Sidebar context menus (chat row + project header)
    // Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â

    // Right-click on one or more chat rows.  Builds:
    //   Move to project Ã¢â€“Â¸
    //     (No project)
    //     Ã¢â€â‚¬Ã¢â€â‚¬
    //     <Project A>
    //     <Project B>
    //   Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    //   Delete conversation(s)
    void ShowSidebarChatContextMenu(const std::vector<std::string>& paths,
                                    wxWindow* anchor)
    {
        if (paths.empty()) return;

        wxMenu menu;
        const bool busy = IsBusy();

        // Ã¢â€â‚¬Ã¢â€â‚¬ Move to project Ã¢â€“Â¸ submenu Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
        wxMenu* moveSub = new wxMenu;
        auto projects = ProjectManager::ListProjects();

        // "(No project)" first Ã¢â‚¬â€ the unassign action.
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

        // Map menu IDs back to project IDs at click time.  Lambdas
        // capturing by value would also work; the id-keyed map keeps
        // the binding loop concise and avoids one bind per project.
        std::unordered_map<int, std::string> idToProject;
        for (const auto& p : projects) {
            wxMenuItem* projectItem = moveSub->Append(
                wxID_ANY, wxString::FromUTF8(p.name));
            const int itemId = projectItem->GetId();
            idToProject[itemId] = p.id;
        }

        if (projects.empty()) {
            // Avoid an empty submenu Ã¢â‚¬â€ at least the "(No project)"
            // item is there, but make it obvious there's nothing else.
            wxMenuItem* hint = moveSub->Append(wxID_ANY,
                "(no projects yet Ã¢â‚¬â€ create one from the project strip)");
            hint->Enable(false);
        }

        const wxString moveLabel = (paths.size() == 1)
            ? wxString("Move to project")
            : wxString::Format("Move %zu chats to project", paths.size());
        wxMenuItem* moveItem = menu.AppendSubMenu(moveSub, moveLabel);
        if (busy) moveItem->Enable(false);

        menu.AppendSeparator();

        // Ã¢â€â‚¬Ã¢â€â‚¬ Delete Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
        wxMenuItem* deleteItem = nullptr;
        if (paths.size() <= 1) {
            deleteItem = menu.Append(wxID_DELETE, "Delete conversation");
        }
        else {
            deleteItem = menu.Append(wxID_DELETE,
                wxString::Format("Delete %zu conversations", paths.size()));
        }
        if (busy && deleteItem) deleteItem->Enable(false);

        // Ã¢â€â‚¬Ã¢â€â‚¬ Bind handlers Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
        // Snapshot |paths| so the lambdas don't depend on selection
        // state surviving until the user clicks.
        const std::vector<std::string> snapshot = paths;

        menu.Bind(wxEVT_MENU,
            [this, snapshot](wxCommandEvent&) {
                m_projectController->MoveChatsToProject(snapshot, std::string());
            }, unassignedItemId);

        for (const auto& [itemId, projId] : idToProject) {
            const std::string capturedId = projId;
            const int capturedItemId = itemId;
            menu.Bind(wxEVT_MENU,
                [this, snapshot, capturedId](wxCommandEvent&) {
                    m_projectController->MoveChatsToProject(snapshot, capturedId);
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
    // (empty groupId) get no menu Ã¢â‚¬â€ there's nothing project-specific
    // to act on.
    void ShowSidebarProjectHeaderContextMenu(const std::string& groupId,
                                             wxWindow* anchor)
    {
        if (groupId.empty()) return;  // Unassigned header Ã¢â‚¬â€ no menu

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

        // Ã¢â€â‚¬Ã¢â€â‚¬ Attach this chat Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
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

        // Ã¢â€â‚¬Ã¢â€â‚¬ Open actions Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
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

        // Ã¢â€â‚¬Ã¢â€â‚¬ Delete project Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
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


    // Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â
    //  Projects Phase 1-5 menu handlers
    // Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â

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

    // Ã¢â€â‚¬Ã¢â€â‚¬ Skill handlers Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    // These do not require an attached project. They always operate
    // against %USERPROFILE%\LlamaBoss\Skills.

    void CreateSkillFromMenu(bool withPythonScript)
    {
        if (IsBusy()) return;

        LbThemedTextEntryDialog dlg(
            this,
            m_appState->GetTheme(),
            withPythonScript ? "New Skill with Python Script" : "New Skill",
            "Skill name:",
            "Create");
        if (LbShowModalWithScrim(*this, dlg) != wxID_OK) return;

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

        m_skillDraftController->StartDesignSession(
            name,
            skill.path,
            withPythonScript);

        std::ostringstream body;
        body << "Created Skill:\n"
             << skill.path;
        if (withPythonScript && !script.path.empty()) {
            body << "\n\nCreated optional Python helper script:\n"
                 << script.path;
        }
        body << "\n\nLetÃ¢â‚¬â„¢s design this Skill together first. Tell me what you want it to do, "
             << "and I can ask questions, check notes when useful, and help choose the right implementation path. "
             << "When the design sounds right, say `draft this Skill` and IÃ¢â‚¬â„¢ll write the Skill files.";
        m_chatDisplay->DisplaySystemMessage(body.str());
        m_projectContextBuilder->Invalidate();

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

        LbThemedSingleChoiceDialog dlg(
            this,
            m_appState->GetTheme(),
            "Open Skill",
            "Select a Skill to open:",
            choices,
            "Open");

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
                return path.u8string();
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

    // Ã¢â€â‚¬Ã¢â€â‚¬ Goal menu handlers Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    // Each routes through the same paths as the /goal slash command so the
    // menu and the command can never drift apart.
    void OnGoalSet(wxCommandEvent&)
    {
        if (IsBusy()) return;

        wxTextEntryDialog dlg(this, "Goal objective:", "Set a Goal");
        if (dlg.ShowModal() != wxID_OK) return;

        // Identical to typing "/goal <objective>": HandleSlashGoal trims the
        // input, an empty objective just shows status, and a real objective
        // starts the goal (including the contract build).
        m_goalController->HandleSlashGoal(std::string(dlg.GetValue().ToUTF8().data()));
    }

    void OnGoalStatus(wxCommandEvent&) { m_goalController->DisplayGoalStatus(); }
    void OnGoalPause (wxCommandEvent&) { m_goalController->HandleSlashGoal("pause"); }
    void OnGoalResume(wxCommandEvent&) { m_goalController->HandleSlashGoal("resume"); }
    void OnGoalVerify(wxCommandEvent&) { m_goalController->HandleSlashGoal("verify"); }
    void OnGoalClear (wxCommandEvent&) { m_goalController->HandleSlashGoal("clear"); }

    void OnProjectClear(wxCommandEvent&) { m_projectController->ClearProjectFromChat(); }

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

        // Hidden Skill/goal control turns have no transcript placeholder.
        // Let their controllers stop them before normal assistant or agent
        // teardown runs.
        if (m_skillDraftController->HandleStopGeneration()) return;
        if (m_goalController->HandleStopGeneration()) return;

        // Ã¢â€â‚¬Ã¢â€â‚¬ Agent loop cancellation Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
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
                m_pythonRunner->Cancel();
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

    // Ã¢â€â‚¬Ã¢â€â‚¬ ASCII Animation engine Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
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
            // Animation finished Ã¢â‚¬â€ stop timer, leave final frame
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
                           m_appState->GetTheme(),
                           m_appState->GetSecretsStore(),
                           m_appState->GetEndpointStore());

        const int dialogResult = LbShowModalWithScrim(*this, dlg);

        if (dialogResult != wxID_OK) return;

        bool folderChanged             = dlg.WasModelsFolderChanged();
        bool modelChanged              = dlg.WasModelChanged();
        const bool themeChanged        = dlg.WasThemeChanged();
        bool ctxSizeChanged            = dlg.WasCtxSizeChanged();
        const bool fontSizeChanged     = dlg.WasFontSizeChanged();
        const bool agentDefaultChanged = dlg.WasAgentDefaultChanged();
        bool kvCacheQ8Changed          = dlg.WasKvCacheQ8Changed();

        // ── Multi-window courtesy check (Phase 3c) ────────────────
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
        if ((folderChanged || modelChanged || ctxSizeChanged ||
             kvCacheQ8Changed) &&
            m_modelService->AnyOtherWindowBusy(this)) {
            const int r = wxMessageBox(
                "Another window is generating a response. Applying the "
                "model/server changes will interrupt it.\n\nApply anyway?",
                "Model Switch", wxYES_NO | wxICON_WARNING, this);
            if (r != wxYES) {
                folderChanged   = false;
                modelChanged    = false;
                ctxSizeChanged  = false;
                kvCacheQ8Changed = false;
                m_chatDisplay->DisplaySystemMessage(
                    "Model/server changes were not applied \xE2\x80\x94 "
                    "another window is generating. Visual settings were "
                    "applied.");
            }
        }

        // Ã¢â€â‚¬Ã¢â€â‚¬ Models folder changed Ã¢â‚¬â€ unload and wait Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
        // The previously-loaded model's path may no longer be in scope
        // (new folder may not contain it, or not at that path). Autosave
        // any conversation, stop the server, and clear state. Don't
        // auto-start Ã¢â‚¬â€ user reopens Settings and explicitly picks a
        // model from the now-active folder. Takes precedence over
        // modelChanged: any combo auto-select that happened during the
        // folder swap isn't a deliberate user pick.
        if (folderChanged) {
            // Deliberate server action supersedes any lazy-load intent.
            m_modelSwitcher->ClearPendingDeferredModel();
            m_pendingSend = PendingSend{};

            // Durable: history is cleared just below.
            if (!m_chatHistory->IsEmpty())
                m_convController->AutoSaveConversation(true, /*durable=*/true);

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
        // Ã¢â€â‚¬Ã¢â€â‚¬ Server restarts (model or context length change) Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
        // A model change implies a fresh slate Ã¢â‚¬â€ clear history and start
        // over. A ctx-only change preserves history but still needs a
        // server restart since -c is a launch argument.
        else if (modelChanged) {
            // Deliberate server action supersedes any lazy-load intent.
            m_modelSwitcher->ClearPendingDeferredModel();
            m_pendingSend = PendingSend{};

            std::string newModel = dlg.GetSelectedModel();

            // Persist launch-argument settings first so MakeServerConfig()
            // below sees them.
            if (ctxSizeChanged)
                m_appState->SetCtxSize(dlg.GetSelectedCtxSize());
            if (kvCacheQ8Changed)
                m_appState->SetKvCacheQ8(dlg.GetSelectedKvCacheQ8());

            // Durable: history is cleared just below.
            if (!m_chatHistory->IsEmpty())
                m_convController->AutoSaveConversation(true, /*durable=*/true);

            m_modelSwitcher->SetConversationPreferredLocalModel(newModel);
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
            m_modelService->RequestLocalModel(
                newModel, m_appState->MakeServerConfig());
        }
        else if (ctxSizeChanged || kvCacheQ8Changed) {
            // Deliberate server action supersedes any lazy-load intent.
            m_modelSwitcher->ClearPendingDeferredModel();
            m_pendingSend = PendingSend{};

            // Restart server with the same model but new launch args
            // (ctx size and/or KV cache type).
            // History is preserved Ã¢â‚¬â€ user can keep reading while it reloads.
            if (ctxSizeChanged)
                m_appState->SetCtxSize(dlg.GetSelectedCtxSize());
            if (kvCacheQ8Changed)
                m_appState->SetKvCacheQ8(dlg.GetSelectedKvCacheQ8());

            if (!m_chatHistory->IsEmpty())
                m_convController->AutoSaveConversation();

            m_modelSwitcher->MarkServerNotReady();
            _statusDot->SetConnected(false);
            m_chatDisplay->DisplaySystemMessage(
                "Reloading with " +
                std::to_string(m_appState->GetCtxSize() / 1024) +
                "k context" +
                std::string(m_appState->GetKvCacheQ8()
                    ? ", q8 KV cache..." : ", f16 KV cache..."));
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
                    "Local context/KV settings will apply the next time a "
                    "local model is loaded.");
            }
        }

        // Ã¢â€â‚¬Ã¢â€â‚¬ Font size change Ã¢â‚¬â€ apply to chat display + input Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
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

        // Ã¢â€â‚¬Ã¢â€â‚¬ Theme change Ã¢â‚¬â€ recolor the whole UI Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
        if (themeChanged) {
            m_appState->SetTheme(dlg.GetSelectedTheme());
            ApplyThemeToUI();
        }

        // Ã¢â€â‚¬Ã¢â€â‚¬ Agent-mode default Ã¢â‚¬â€ pure setting, no side effects Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
        // Takes effect at next New Chat / next app launch. Deliberately
        // doesn't flip the current chat's m_agentModeEnabled Ã¢â‚¬â€ the robot
        // button remains the only way to change the active chat's state.
        if (agentDefaultChanged) {
            m_appState->SetAgentDefaultOn(dlg.GetSelectedAgentDefault());
        }

        // Context meter toggle: apply live and unconditionally â€” cheap,
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

        // Ã¢â€â‚¬Ã¢â€â‚¬ Replay conversation for any visual change Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
        // Font and theme both need the RichTextCtrl's stored attrs
        // regenerated for existing messages. Skip if model changed
        // (history already cleared), folder changed (ditto), or if
        // only ctx changed (no visual diff Ã¢â‚¬â€ history is still valid).
        if (!modelChanged && !folderChanged &&
            (themeChanged || fontSizeChanged) &&
            !m_chatHistory->IsEmpty()) {
            m_chatDisplay->Clear();
            m_convController->ReplayConversation();
        }

        // Ã¢â€â‚¬Ã¢â€â‚¬ Announce visual-only changes (server restarts and folder
        //    changes have their own status messages already) Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
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
        const wxString msg = LbBuildAboutMessage(
            LLAMABOSS_VERSION,
            ServerManager::ModelDisplayName(m_appState->GetModel()),
            m_appState->GetApiUrl(),
            ServerManager::GetModelsDir());

        wxMessageDialog dlg(this, msg, "About LlamaBoss Beta",
                            wxYES_NO | wxICON_INFORMATION);
        dlg.SetYesNoLabels("Check for Updates", "Close");
        if (dlg.ShowModal() == wxID_YES)
            CheckForUpdates(/*silent=*/false);
    }

    // Ã¢â€â‚¬Ã¢â€â‚¬ Update check Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
    // silent=false : user-initiated. Always reports outcome.
    // silent=true  : startup. One quiet status line only if an update
    //                exists; errors and "up to date" stay silent.
    void CheckForUpdates(bool silent)
    {
        bool expected = false;
        if (!m_updateCheckInFlight.compare_exchange_strong(expected, true)) {
            if (!silent)
                m_chatDisplay->DisplaySystemMessage(
                    "Update check already running.");
            return;
        }
        if (!silent)
            m_chatDisplay->DisplaySystemMessage(
                "Checking llamaboss.com for updates...");

        const std::string current = LLAMABOSS_VERSION;
        wxEvtHandler* target = this;
        std::weak_ptr<std::atomic<bool>> alive = m_alive;

        // Keeper instead of std::thread(...).detach(): identical runtime
        // behavior, but the thread is joined at process exit so it can
        // never race static destruction (see LbBackgroundThreadKeeper).
        LbBackgroundThreadKeeper::Instance().Launch(
            [target, alive, current, silent]() {
                UpdateChecker::UpdateInfo info =
                    UpdateChecker::CheckBlocking(current);

                auto* ev = new wxThreadEvent(wxEVT_UPDATE_CHECK_RESULT);
                ev->SetPayload(info);
                ev->SetInt(silent ? 1 : 0);
                LbQueueEventIfAlive(target, alive, ev);
            });
    }

    void OnUpdateCheckThreadResult(wxThreadEvent& event)
    {
        m_updateCheckInFlight.store(false);
        if (m_isClosing) return;

        const bool silent = event.GetInt() != 0;
        OnUpdateCheckResult(
            event.GetPayload<UpdateChecker::UpdateInfo>(),
            silent);
    }

    void OnUpdateCheckResult(const UpdateChecker::UpdateInfo& info, bool silent)
    {
        if (!info.ok) {
            if (!silent)
                m_chatDisplay->DisplaySystemMessage(
                    "Update check failed: " + info.error);
            return;
        }
        if (!info.available) {
            if (!silent)
                m_chatDisplay->DisplaySystemMessage(
                    "LlamaBoss is up to date (v" +
                    std::string(LLAMABOSS_VERSION) + ").");
            return;
        }

        m_chatDisplay->DisplaySystemMessage(
            "Update available: v" + info.latest + "  -  llamaboss.com");

        if (silent) return; // startup: one line, no modal

        wxMessageDialog dlg(this,
                            LbBuildUpdateAvailableMessage(LLAMABOSS_VERSION, info),
                            "Update Available",
                            wxYES_NO | wxICON_INFORMATION);
        dlg.SetYesNoLabels("Open Download", "Later");
        if (dlg.ShowModal() == wxID_YES) {
            const std::string fallback = LbUpdateFallbackUrl();
            std::string target = info.url.empty() ? fallback : info.url;
            if (!LbIsTrustedUpdateUrl(target)) {
                m_chatDisplay->DisplaySystemMessage(
                    "Update manifest returned an untrusted download URL; "
                    "opening llamaboss.com instead.");
                target = fallback;
            }
            wxLaunchDefaultBrowser(wxString::FromUTF8(target));
        }
    }

    // ── Phase 3a: multiple windows ───────────────────────────────
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

        // Durable: history is cleared just below — the file becomes the
        // only copy of this conversation.
        if (!m_chatHistory->IsEmpty())
            m_convController->AutoSaveConversation(false, /*durable=*/true);

        // KV fast path: snapshot the outgoing conversation's slot state
        // before Clear().  Ownership-guarded no-op unless the slot holds
        // this conversation's KV and a generation ran since restore.
        // Routed through ModelService (Phase 3c): skipped when another
        // window is mid-generation on the shared slot.
        m_modelService->SaveSlotStateForConversation(
            this, m_chatHistory->GetFilePath());

        m_chatHistory->Clear();
        // Fresh chat = no conversation claim; the first autosave will
        // claim the newly generated path (Phase 3b).
        wxGetApp().GetConversationRegistry().SetCurrent(this, "");
        CancelPendingSendForConversationSwitch();
        RefreshGoalStatusStrip();
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
            _agentToggleButton->SetForegroundColour(
                m_agentModeEnabled ? LbInteractiveAccentForTheme(m_appState->GetTheme())
                                   : m_appState->GetTheme().textMuted);
            _agentToggleButton->Refresh();
        }

        if (auto* logger = m_appState->GetLogger())
            logger->information("New chat started");
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

    // Ã¢â€â‚¬Ã¢â€â‚¬ Server lifecycle Ã¢â€ â€™ delegate to ModelSwitcher Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
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

        // Phase 3 bugfix #2: immediately clear any stale protocol from
        // the previously loaded server before detection for this server
        // completes. Without this, a user could send an agent request in
        // the small ready-to-probe-result window and build it with the old
        // model's protocol.
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

        const std::string modelPath  = m_modelService->Server().GetLoadedModel();
        const std::string mmprojPath = m_modelService->Server().GetLoadedMmproj();
        const std::string baseUrl    = m_modelService->Server().GetBaseUrl();
        if (!modelPath.empty() && !baseUrl.empty()) {
            KickOffToolProtocolDetection(
                this, m_alive, baseUrl, modelPath, mmprojPath,
                serverJinjaEnabled);
        }

        // Lazy load: if the user hit Send while this model was loading, fire
        // the queued prompt now Ã¢â‚¬â€ but only if the model that became ready is
        // the one it was queued under.  A model switch between queueing and
        // ready leaves the prompt orphaned; drop it rather than send it to
        // the wrong model.  (The prompt builds against whatever protocol
        // detection has resolved so far, same as any fast manual send during
        // the readyÃ¢â€ â€™probe window noted above.)
        if (m_pendingSend.active) {
            const std::string queued    = m_pendingSend.userInput;
            const std::string queuedFor = m_pendingSend.modelPath;
            m_pendingSend = PendingSend{};

            if (queuedFor.empty() || queuedFor != modelPath) {
                m_chatDisplay->DisplaySystemMessage(
                    "Queued message was dropped because the loaded model "
                    "changed.");
            }
            else if (IsBusy() || HasPendingApproval()) {
                m_chatDisplay->DisplaySystemMessage(
                    "Queued message was dropped because another operation "
                    "started before the model finished loading.");
            }
            else {
                DispatchUserTurn(queued);
            }
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

        // Phase 3c-i: cache the result for the request builder.  The
        // agent controller reads this via the getActiveProtocol
        // callback when building each request body.
        _activeProtocol = r.protocol;

        UpdateProtocolChip(r.protocol);
    }

    // Apply protocol to the chip widget.  Called from
    // OnToolProtocolDetected; also safe to call with Unknown to
    // hide the chip.
    // â”€â”€ Context meter helpers â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

    // Drop the exact anchor and re-price the fallback estimate from the
    // (now-current) history.  Call whenever the conversation identity or
    // the tokenizer changes: New Chat, conversation load, model switch.
    void InvalidateContextAnchor()
    {
        m_ctxAnchorPromptTokens     = -1;
        m_ctxAnchorCompletionTokens = 0;
        m_ctxAnchorExact            = false;
        m_ctxHistoryEstimateTokens  = m_chatHistory
            ? (long long)m_chatHistory->EstimateHistoryTokens()
            : 0;
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
        if (!wantShown) return;

        int ctxTokens = m_appState->GetCtxSize();
        if (ctxTokens <= 0) ctxTokens = 8192;

        long long used;
        bool exact;
        if (m_ctxAnchorPromptTokens >= 0) {
            used  = m_ctxAnchorPromptTokens + m_ctxAnchorCompletionTokens;
            exact = m_ctxAnchorExact;
        } else {
            used  = m_ctxHistoryEstimateTokens;
            exact = false;
        }

        // Pending composer text rides on top as an estimate.
        // GetLastPosition() is O(1) â€” deliberately NOT GetValue(),
        // which copies the whole buffer (see OnUserInputChanged).
        if (_userInputCtrl) {
            const long pendingChars = _userInputCtrl->GetLastPosition();
            if (pendingChars > 0) {
                used += (long long)ChatHistory::EstimateTokensFromBytes(
                    (size_t)pendingChars);
            }
        }

        const bool elided =
            m_chatHistory && m_chatHistory->GetLastBuildElidedCount() > 0;

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
        if (elided) text += " \xC2\xB7" "elided";   // " Â·elided"

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
        if (changed) {
            _ctxMeter->Refresh();
            _ctxMeter->SetToolTip(
                "Context window occupancy for the next request.\n"
                "Exact (from the server's reported token usage) after each "
                "completed turn; \"~\" marks a size-based estimate.\n"
                "Amber: past the elision threshold - older tool-result "
                "bodies are being trimmed from what the model sees "
                "(\"\xC2\xB7" "elided\" appears when the last request was "
                "trimmed).\n"
                "Red: the window is nearly full - responses may degrade; "
                "consider starting a new chat.\n"
                "Window size is the Context Length in Settings.");
        }
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

        // Server failed permanently Ã¢â‚¬â€ also clear any chip from a
        // prior session so a stale "native" doesn't outlive its
        // model.
        if (_protocolChip) UpdateProtocolChip(ToolProtocol::Unknown);
        _activeProtocol = ToolProtocol::Unknown;
        m_modelSwitcher->OnServerError(err);

        // A prompt queued behind this (failed) load can never fire Ã¢â‚¬â€ drop it
        // and say so, rather than leaving it stuck.
        if (m_pendingSend.active) {
            m_pendingSend = PendingSend{};
            m_chatDisplay->DisplaySystemMessage(
                "Your queued message was not sent because the model failed "
                "to load.");
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
                // message with an empty placeholder Ã¢â‚¬â€ skip instead.
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

        // Ã¢â€â‚¬Ã¢â€â‚¬ Shift+Enter Ã¢â‚¬â€ insert a literal newline in the input Ã¢â€â‚¬Ã¢â€â‚¬
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
            return;   // consume Ã¢â‚¬â€ do NOT Skip()
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
        encoder.rdbuf()->setLineLength(0);  // unbroken output Ã¢â‚¬â€ skip strip pass
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

    // Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â
    //  SEND MESSAGE
    // Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â

        bool TryHandlePendingApprovalInput(const std::string& userInput)
    {
        // Phase 6: approval is a special busy state.  The input is
        // enabled only for /approve or /deny; ordinary messages wait
        // until the pending tool is resolved.
        if (m_chatState == ChatState::AwaitingApproval) {
            if (userInput.empty()) return true;

            const auto action =
                lb_input_parsers::ParseApprovalInput(userInput);

            if (action ==
                lb_input_parsers::ApprovalInputAction::Unrecognized) {
                // Two fixes vs the old branch:
                //  * do NOT clear the composer -- clearing silently
                //    destroyed the user's typed draft;
                //  * use a system line, not DisplayAssistantMessage --
                //    the old fake assistant turn was never added to
                //    history, so the transcript and the saved
                //    conversation diverged on reload.
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
        // Ã¢â€â‚¬Ã¢â€â‚¬ Easter egg commands Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
        if (!hasAttachments && (userInput == "/yay!" || userInput == "/yay")) {
            _userInputCtrl->Clear();
            { wxCommandEvent e(wxEVT_TEXT, _userInputCtrl->GetId()); OnUserInputChanged(e); }
            m_chatDisplay->DisplaySystemMessage("* fireworks *");
            m_activeAnimation = std::make_unique<FireworksAnimation>();
            m_animTimer.Start(m_activeAnimation->GetIntervalMs());
            return true;
        }

        // Ã¢â€â‚¬Ã¢â€â‚¬ /cd Ã¢â‚¬â€ per-conversation working directory Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
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
        // on|off|auto" sets it.  Auto sends nothing on the wire (the
        // historical request shape); on/off ask the backend to enable
        // or suppress model reasoning.  Local llama-server targets get
        // chat_template_kwargs.enable_thinking (honored by hybrid
        // reasoning models such as Qwen3 under --jinja, ignored by
        // templates that never reference it); remote OpenAI-compatible
        // targets get an OpenRouter-style reasoning object.
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

            auto modeName = [](ChatHistory::ThinkOverride m) -> const char* {
                switch (m) {
                    case ChatHistory::ThinkOverride::On:  return "on";
                    case ChatHistory::ThinkOverride::Off: return "off";
                    default:                              return "auto";
                }
            };

            if (token.empty()) {
                m_chatDisplay->DisplaySystemMessage(
                    std::string("Thinking mode: ") +
                    modeName(m_chatHistory->GetThinkOverride()) +
                    ". Use /think on, /think off, or /think auto. "
                    "auto = backend default (nothing added to the "
                    "request); on/off ask the model to reason (or not) "
                    "before answering -- honored by hybrid-reasoning "
                    "models, ignored by models without a thinking mode.");
                return true;
            }

            ChatHistory::ThinkOverride mode;
            if      (token == "on")   mode = ChatHistory::ThinkOverride::On;
            else if (token == "off")  mode = ChatHistory::ThinkOverride::Off;
            else if (token == "auto") mode = ChatHistory::ThinkOverride::Auto;
            else {
                m_chatDisplay->DisplaySystemMessage(
                    "Usage: /think on | off | auto. Current: " +
                    std::string(modeName(m_chatHistory->GetThinkOverride())) +
                    ".");
                return true;
            }

            m_chatHistory->SetThinkOverride(mode);
            std::string note = std::string("Thinking mode set to ") +
                               modeName(mode) +
                               " for this conversation.";
            if (mode != ChatHistory::ThinkOverride::Auto) {
                note += " Takes effect on the next message. If a remote "
                        "endpoint rejects the reasoning field, run "
                        "/think auto to restore the default request.";
            }
            m_chatDisplay->DisplaySystemMessage(note);
            return true;
        }


        // Ã¢â€â‚¬Ã¢â€â‚¬ /goal Ã¢â‚¬â€ per-conversation goal state Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
        // Not a tool: forwards goal commands to GoalController.
        if (!hasAttachments && userInput.rfind("/goal", 0) == 0 &&
            (userInput.size() == 5 ||
             userInput[5] == ' ' || userInput[5] == '\t' ||
             userInput[5] == '\n' || userInput[5] == '\r')) {
            std::string rest = (userInput.size() > 5)
                ? userInput.substr(6) : std::string();

            _userInputCtrl->Clear();
            { wxCommandEvent e(wxEVT_TEXT, _userInputCtrl->GetId());
              OnUserInputChanged(e); }

            m_goalController->HandleSlashGoal(rest);
            return true;
        }

        // Ã¢â€â‚¬Ã¢â€â‚¬ Natural-language Goal controls (Goals Phase 16) Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
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
                m_goalController->HandleSlashGoal(naturalGoalCommand);
                return true;
            }
        }

        // Ã¢â€â‚¬Ã¢â€â‚¬ Natural-language Goal start (Goals Phase 15) Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
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
                    m_goalController->HandleSlashGoal(naturalGoalObjective);
                }
                return true;
            }
        }

        // Ã¢â€â‚¬Ã¢â€â‚¬ Tool-shaped slash commands (Phase 4 / 4.1) Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬Ã¢â€â‚¬
        // Parsing now lives in lb_input_parsers so MyFrame keeps the
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

        // Ã¢â€â‚¬Ã¢â€â‚¬ Conversational Skill design-session routing Ã¢â€â‚¬Ã¢â€â‚¬
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
                    "Great Ã¢â‚¬â€ IÃ¢â‚¬â„¢ll draft this Skill from our design conversation and choose the most practical implementation path.";
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
        if (m_attachments->HasCsvFile())
            userInput = m_attachments->BakeCsvFilesIntoMessage(
                userInput, m_agentModeEnabled);
        if (m_attachments->HasZipFile())
            userInput = m_attachments->BakeZipFilesIntoMessage(
                userInput, m_agentModeEnabled);

        // Per-turn ambient context: prepended AFTER the bakes so the
        // header is the first line of the wire message.  Stored in
        // history (stable KV prefix; temporal grounding of past turns).
        // Goal-answer capture below intentionally receives the clean
        // userInput, not the wire copy.
        const std::string wireInput =
            BuildSessionContextHeader() + "\n\n" + userInput;

        m_chatHistory->AddUserMessage(wireInput, "", attachInfo);

        // A plain user reply while a goal is waiting for input is recorded
        // as the answer, not immediately routed into a fresh agent turn.
        if (!hasAttachments &&
            m_goalController->TryCaptureAwaitingUserReply(userInput)) {
            m_attachments->Clear();
            return false;
        }

        return true;
    }

    void StartAssistantResponseForPreparedTurn()
    {
        std::string model =
            m_modelSwitcher->GetConversationModelForSave();

        // Image-generation model (per-model image_output flag on the
        // active remote target)?  Two consequences for this turn:
        //   * never attach the tool catalog — OpenRouter routes only
        //     to providers supporting every requested feature, and no
        //     provider of an image model supports tool use, so a
        //     tools-bearing request 404s before generation starts;
        //   * the request builder adds "modalities": ["image","text"]
        //     so the provider actually returns image data.
        // Agent mode is bypassed rather than silently degraded: an
        // image model can't drive the tool loop, and pretending it
        // can would just burn a paid API call on a guaranteed error.
        const bool imageModel = m_modelService->ResolveTarget().imageOutput;

        // Build the final request body only once.
        // Important: agent mode must add its system prompt BEFORE image injection.
        // If we inject images first and then rebuild the body for agent mode,
        // the rebuilt body loses the multimodal content array.
        std::string body;
        if (m_agentModeEnabled && !imageModel) {
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
                native,
                true);
        }
        else {
            if (m_agentModeEnabled && imageModel) {
                m_chatDisplay->DisplaySystemMessage(
                    "image model: agent tools are disabled for this turn.");
            }

            int ctxTokens = m_appState->GetCtxSize();
            if (ctxTokens <= 0) ctxTokens = 8192;
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

        // Inject images after the final body shape is known.
        // This fixes agent mode dropping image attachments on the first request.
        if (m_attachments->HasImage())
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
        // Routed through ModelService (Phase 3c): if another window
        // is mid-generation, this request queues behind it and the
        // stamp is invalidated instead — see ModelService::NoteSlotOwner.
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
        // streamed assistant reply as iteration 1.  Image-model turns never
        // arm it — the request carried no tools (see imageModel above), so
        // treating the reply as an agent iteration would only hold prose in
        // the stream filter and log a phantom loop.
        if (m_agentModeEnabled && !imageModel) {
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
        //   * Request shape Ã¢â‚¬â€ a single grep-friendly summary
        //     ("tools=yes, messages=N, body=BYTES") that answers
        //     "is the wire shape correct?" without eyeballing JSON.
        //   * Outbound Ã¢â‚¬â€ the first ~2000 chars of the body for
        //     deeper inspection.  2000 covers the agent system
        //     prompt (~1k chars) plus the head of the tools array,
        //     which 500 was clipping.
        if (auto* logger = m_appState->GetLogger()) {
            // Cheap textual sniff Ã¢â‚¬â€ these substrings appear at the
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
                std::string("Outbound /v1/chat/completions preview (") + protoLabel
                + "): " + preview);
        }

        if (!m_chatClient->SendMessage(m_modelService->ResolveTarget(),
            body, m_generationId)) {

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
    // Used to fire a prompt that was queued while a deferred model loaded Ã¢â‚¬â€
    // see OnServerReady.  Mirrors the tail of OnSendMessage.
    void DispatchUserTurn(const std::string& userInput)
    {
        const bool hasAttachments = m_attachments->HasPending();
        if (userInput.empty() && !hasAttachments) return;
        if (TryHandleSpecialInputRouting(userInput, hasAttachments)) return;
        if (!PrepareAndRecordUserTurn(userInput, hasAttachments)) return;
        StartAssistantResponseForPreparedTurn();
    }

    void OnSendMessage(wxCommandEvent&)
    {
        if (m_activeAnimation) return;  // animation playing

        std::string userInput = WxToUtf8(_userInputCtrl->GetValue());

        // Trim leading whitespace so slash-commands (/cmd, /yay) fire
        // regardless of stray leading spaces in the input box.  Do NOT
        // trim trailing whitespace Ã¢â‚¬â€ prompts may intentionally end with
        // newlines for paragraph spacing.
        {
            size_t firstNonWs = userInput.find_first_not_of(" \t\r\n");
            if (firstNonWs == std::string::npos) userInput.clear();
            else if (firstNonWs > 0)             userInput.erase(0, firstNonWs);
        }

        if (TryHandlePendingApprovalInput(userInput)) return;

        if (IsBusy()) return;

        const bool hasAttachments = m_attachments->HasPending();

        // ── Multi-window queue notice (Phase 3c) ─────────────────
        // llama-server runs a single slot, so if another window is
        // mid-generation this request waits silently inside the
        // server until that stream finishes.  Say so up front — a
        // send that appears to do nothing reads as a hang.  Local
        // managed lane only: remote endpoints have no shared slot.
        if ((!userInput.empty() || hasAttachments) &&
            m_modelSwitcher->IsServerReady() &&
            m_modelService->ResolveTarget().managed &&
            m_modelService->AnyOtherWindowBusy(this)) {
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
            // Nothing to send Ã¢â‚¬â€ don't queue an empty turn.
            if (userInput.empty() && !hasAttachments) return;

            // A prompt is already queued behind a load we kicked off.
            // Keep the freshest text and wait.  Checked first so repeated
            // Sends can never launch a second server.
            if (m_pendingSend.active) {
                m_pendingSend.userInput = userInput;
                _userInputCtrl->ChangeValue("");
                m_chatDisplay->DisplaySystemMessage(
                    "Model is still loading \xE2\x80\x94 your message will send "
                    "when it's ready.");
                return;
            }

            // Lazy load: opening a saved conversation parked its model in
            // ModelSwitcher instead of reloading it.  The user has
            // now sent a prompt, so load that model and queue this prompt to
            // fire from OnServerReady once it's up.

            // ── Multi-window courtesy check (Phase 3c) ────────────
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
                    m_modelService->AnyOtherWindowBusy(this)) {
                    const int r = wxMessageBox(
                        "Another window is generating a response. Loading "
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
                m_pendingSend.userInput = userInput;
                m_pendingSend.modelPath = deferred;
                _userInputCtrl->ChangeValue("");

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
            // switch already in flight) Ã¢â‚¬â€ original wait behavior.
            m_chatDisplay->DisplaySystemMessage(
                "Server is still loading the model. Please wait...");
            return;
        }

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

// Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â
//  ImageDropTarget Implementation
// Ã¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢ÂÃ¢â€¢Â

bool ImageDropTarget::OnDropFiles(wxCoord /*x*/, wxCoord /*y*/,
    const wxArrayString& filenames)
{
    // Single classifying loop Ã¢â‚¬â€ fixes two prior bugs:
    //
    //   1. Dropping multiple PDFs / spreadsheets / DOCX files only imported
    //      the first because each kind-specific loop returned early.
    //
    //   2. Mixed drops (e.g. one PDF plus two screenshots) silently lost the
    //      images for the same reason Ã¢â‚¬â€ the PDF branch returned before the
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
        else if (AttachmentManager::IsCsvFile(path)) {
            // Before IsTextFile by necessity: CSV routes through the
            // workspace-import path (csv_inspect hint), and IsTextFile
            // no longer claims the extension.
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
