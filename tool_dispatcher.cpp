#define _CRT_SECURE_NO_WARNINGS

// tool_dispatcher.cpp
//
// Phase 2: this file used to host the per-tool DispatchXxx functions
// (DispatchRead, DispatchLs, ..., DispatchDelete) plus the if-chain
// inside DispatchInvocation that selected among them.  All built-in
// dispatch bodies now live inside the router as ToolSpec.dispatch
// closures (see tool_router.cpp -- DoRead through DoDelete).
//
// DispatchInvocation stays as the one public shim for callers; it
// validates, looks up the spec in the router, fills the dependency
// bundle, calls dispatch, and returns.

#include "tool_dispatcher.h"
#include "tool_router.h"

#include <cassert>
#include <string>

namespace {

std::string MakeCommandEcho(const std::string& name,
                            const std::string& args)
{
    if (args.empty()) return "/" + name;
    return "/" + name + " " + args;
}

// Build a fully-populated DispatchOutcome for the "invocation came
// in malformed" path -- used both when the parser flagged !inv.valid
// and when the router has no spec for inv.name (an unknown tool that
// somehow slipped past the parser's IsKnownToolName check).  The
// agent loop renders this and feeds it back to the model so it can
// self-correct.
DispatchOutcome MakeInvalidOutcome(const ToolInvocation& inv,
                                   const std::string&    reason)
{
    DispatchOutcome out;
    out.status = DispatchStatus::Invalid;
    out.result.toolTag       = inv.name;
    out.result.invocationRaw = inv.rawBlock;
    out.result.toolName      = inv.name.empty() ? std::string("Tool") : inv.name;
    out.result.commandEcho   = MakeCommandEcho(inv.name, inv.args);
    out.result.errorBody     = reason.empty()
                                   ? std::string("Invalid tool invocation.")
                                   : reason;
    return out;
}

} // namespace

DispatchOutcome DispatchInvocation(const ToolInvocation& inv,
                                   const ToolContext&    ctx,
                                   GrepExecutor*         grepExec,
                                   CmdExecutor*          cmdExec,
                                   PythonRunner*         pythonRunner)
{
    if (!inv.valid) {
        return MakeInvalidOutcome(inv, inv.invalidReason);
    }

    const ToolSpec* spec = GetGlobalRouter().Find(inv.name);
    if (!spec || !spec->dispatch) {
        // Shouldn't reach here under normal flow -- the parser rejects
        // unknown names via IsKnownToolName.  Belt-and-braces fallback
        // for any future path that bypasses the parser (e.g. a Phase 4
        // slash-command shim that wires straight into the dispatcher).
        return MakeInvalidOutcome(inv, "Unknown tool: " + inv.name);
    }

    DispatchDeps deps;
    deps.grepExec     = grepExec;
    deps.cmdExec      = cmdExec;
    deps.pythonRunner = pythonRunner;

    DispatchOutcome outcome = spec->dispatch(inv, ctx, deps);

    // ─── Debug-only sync/async cross-check ─────────────────────
    // Catches the coordination bug where a registration declares
    // safety.isAsync but the dispatch body returns Completed, or
    // vice versa.  Compiled out of release builds; assert() expands
    // to a no-op when NDEBUG is defined.
    //
    // Invalid status is excluded: an arg-validation failure can
    // legitimately short-circuit either an async or sync tool to
    // Completed/Invalid, and we do not want to mask those returns.
    if (outcome.status == DispatchStatus::Async) {
        assert(spec->safety.isAsync &&
               "Tool dispatch returned Async but safety.isAsync is false. "
               "Either set safety.isAsync = true at registration or return Completed.");
    } else if (outcome.status == DispatchStatus::Completed) {
        assert(!spec->safety.isAsync &&
               "Tool dispatch returned Completed but safety.isAsync is true. "
               "Either set safety.isAsync = false at registration or return Async.");
    }

    return outcome;
}
