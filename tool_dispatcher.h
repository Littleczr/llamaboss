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
// read, ls, pwd all run synchronously on the caller's thread and
// return immediately.  grep is threaded and posts back via
// wxEVT_GREP_COMPLETE; for these, Dispatch() returns Status::Async
// and the caller (AgentController) waits for the event before
// continuing the loop.  The dispatcher takes a non-owning pointer
// to a GrepExecutor so the caller controls its lifetime.
//
// Note: this header deliberately does NOT include any agent-loop
// types — it's usable from a future harness, a test harness, or a
// REPL-style tool shell without pulling in loop state.
//
#pragma once

#include "tool_context.h"
#include "tool_invocation.h"
#include "presented_file.h"

#include <string>
#include <vector>

class ChatDisplay;    // only needed for the "Display" helper below
class GrepExecutor;   // forward: defined in tool_grep.h
class CmdExecutor;    // forward: defined in cmd_executor.h
class PythonRunner;   // forward: defined in python_runner.h

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
                                   PythonRunner*         pythonRunner);
