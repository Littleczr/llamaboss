// tool_dispatcher.h
//
// Phase 4: Agent harness — tool dispatcher.
//
// Maps a parsed ToolInvocation onto one of the Phase 3 tool
// functions and returns a unified result.  This is the SINGLE
// place where the agent harness touches ReadFile / ListDirectory
// / GrepExecutor — everything upstream (parser, loop control,
// compaction) deals only in ToolInvocation + ToolInvocationResult.
//
// ─── Sync vs async ───────────────────────────────────────────────
// ToolSpec dispatch functions are either synchronous or start a
// specialized async executor.  Potentially blocking synchronous file tools
// can additionally be wrapped by ToolWorkerExecutor, which preserves their
// Completed/Invalid result shape while moving execution off the wx UI thread.
// Specialized async tools post their own completion events and return Async.
//
// Note: this header deliberately does NOT include any agent-loop
// types — it's usable from a future harness, a test harness, or a
// REPL-style tool shell without pulling in loop state.
//
#pragma once

#include "tool_context.h"
#include "tool_invocation.h"
#include "presented_file.h"

#include <wx/event.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <utility>

class ChatDisplay;    // only needed for the "Display" helper below
class GrepExecutor;   // forward: defined in tool_grep.h
class CmdExecutor;    // forward: defined in cmd_executor.h
class PythonRunner;   // forward: defined in python_runner.h
class WebFetchExecutor; // forward: defined in tool_web_fetch.h

// Renderable output of a completed tool invocation.  Mirrors the
// shape of ReadResult/LsResult/GrepResult so a ChatDisplay::ToolBlock
// can be built from it uniformly.
struct ToolInvocationResult {
    // Display fields — caller packs these into a ChatDisplay::ToolBlock.
    std::string              iconUtf8;     // 📄 / 📁 / 🔍 / ➤ etc.
    std::string              toolName;     // "Read", "List", "Grep", "Pwd"
    std::vector<std::string> chips;
    std::string              commandEcho;  // "/read chat_display.h" style
    std::string              body;
    std::string              errorBody;
    std::string              bodyLang;

    // Optional clickable file chips associated with this result.
    // The first producer is /write: after a successful write, the
    // result carries the created file path so the UI can present a
    // "save/copy" chip without re-parsing human text.
    std::vector<PresentedFile> presentedFiles;

    // Protocol fields — caller packs these back to the model as a
    // <tool_result>.  toolTag is the lowercase protocol name
    // (matches tool_names::*); invocationRaw is the verbatim
    // <tool_call> block so collapsed history can reference it.
    std::string              toolTag;
    std::string              invocationRaw;
};

enum class DispatchStatus {
    Completed,   // Result is filled in.  Render + feed back + continue.
    Async,       // Tool runs on a worker thread.  Caller awaits its event.
    Invalid,     // Invocation was rejected; errorBody explains why.
};

struct DispatchOutcome {
    DispatchStatus         status = DispatchStatus::Invalid;
    ToolInvocationResult   result;   // valid iff status == Completed or Invalid
};

// Generic completion event for synchronous tool dispatches that are wrapped
// in ToolWorkerExecutor because they may block on filesystem/process work.
wxDECLARE_EVENT(wxEVT_TOOL_WORKER_COMPLETE, wxCommandEvent);

struct ToolWorkerResult {
    std::string toolName;
    DispatchOutcome outcome;
    bool cancelled = false;
};

class ToolWorkerResultClientData : public wxClientData {
public:
    explicit ToolWorkerResultClientData(ToolWorkerResult r)
        : m_result(std::move(r)) {}
    const ToolWorkerResult& GetResult() const { return m_result; }
private:
    ToolWorkerResult m_result;
};

// One serialized worker per frame for synchronous tools that should not run
// inside a wx event handler (file reads/listings, write/edit,
// python_create_script, and related filesystem work).  The tool
// implementations remain synchronous and return their normal DispatchOutcome;
// this facade only moves execution off the UI thread.
class ToolWorkerExecutor {
public:
    ToolWorkerExecutor(wxEvtHandler* eventHandler,
                       std::weak_ptr<std::atomic<bool>> aliveToken);
    ~ToolWorkerExecutor();

    bool Start(const ToolInvocation& inv, const ToolContext& ctx);
    void Cancel();

    bool IsRunning() const {
        return m_isRunning && m_isRunning->load();
    }

private:
    wxEvtHandler*                        m_eventHandler;
    std::weak_ptr<std::atomic<bool>>     m_aliveToken;
    std::shared_ptr<std::atomic<bool>>   m_cancelFlag;
    std::shared_ptr<std::atomic<bool>>   m_isRunning;
};

// Router-backed predicate used by both the agent and slash-command paths.
bool ShouldDispatchToolOnWorker(const std::string& toolName);

// Run a parsed invocation.  `grepExec` may be nullptr only if the
// caller is sure no grep calls will arrive (e.g. a reduced test
// environment); otherwise it's required — pass the MyFrame-owned
// GrepExecutor.  Same convention applies to `cmdExec` for the
// `powershell` tool. Same convention applies to `pythonRunner` for
// controlled Python-backed helper tools such as python_health.
DispatchOutcome DispatchInvocation(const ToolInvocation& inv,
                                   const ToolContext&    ctx,
                                   GrepExecutor*         grepExec,
                                   CmdExecutor*          cmdExec,
                                   PythonRunner*         pythonRunner,
                                   WebFetchExecutor*     webFetchExec = nullptr);
