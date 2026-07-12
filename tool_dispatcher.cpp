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
#include "ui_event_post.h"

#include <cassert>
#include <exception>
#include <string>
#include <thread>
#include <utility>

wxDEFINE_EVENT(wxEVT_TOOL_WORKER_COMPLETE, wxCommandEvent);

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

bool ShouldDispatchToolOnWorker(const std::string& toolName)
{
    const ToolSpec* spec = GetGlobalRouter().Find(toolName);
    if (!spec) return false;

    // A ToolSpec either starts its own specialized async executor OR returns
    // a synchronous outcome that this generic worker wraps.  Registering both
    // would create two competing completion paths, so prefer the specialized
    // path in release builds and trip loudly in debug builds.
    assert(!(spec->safety.isAsync && spec->safety.dispatchOnWorker) &&
           "Tool cannot be both isAsync and dispatchOnWorker.");
    return spec->safety.dispatchOnWorker && !spec->safety.isAsync;
}

ToolWorkerExecutor::ToolWorkerExecutor(
    wxEvtHandler* eventHandler,
    std::weak_ptr<std::atomic<bool>> aliveToken)
    : m_eventHandler(eventHandler)
    , m_aliveToken(std::move(aliveToken))
    , m_cancelFlag(std::make_shared<std::atomic<bool>>(false))
    , m_isRunning(std::make_shared<std::atomic<bool>>(false))
{
}

ToolWorkerExecutor::~ToolWorkerExecutor()
{
    Cancel();
}

void ToolWorkerExecutor::Cancel()
{
    if (m_cancelFlag) m_cancelFlag->store(true);
}

bool ToolWorkerExecutor::Start(const ToolInvocation& inv,
                               const ToolContext&    ctx)
{
    if (!ShouldDispatchToolOnWorker(inv.name)) return false;

    bool expected = false;
    if (!m_isRunning ||
        !m_isRunning->compare_exchange_strong(expected, true)) {
        return false;
    }

    m_cancelFlag = std::make_shared<std::atomic<bool>>(false);

    const auto cancelFlag = m_cancelFlag;
    const auto running    = m_isRunning;
    const auto aliveToken = m_aliveToken;
    wxEvtHandler* handler = m_eventHandler;

    ToolInvocation invCopy = inv;
    ToolContext ctxCopy = ctx;

    // The background-dispatched tools do not use these UI/history fields.
    // Clear them so a future implementation change cannot accidentally touch
    // mutable frame-owned state from the worker thread.
    ctxCopy.eventHandler = nullptr;
    ctxCopy.aliveToken.reset();
    ctxCopy.history = nullptr;

    try {
        std::thread([invCopy = std::move(invCopy),
                     ctxCopy = std::move(ctxCopy),
                     cancelFlag,
                     running,
                     aliveToken,
                     handler]() mutable {
            ToolWorkerResult payload;
            payload.toolName = invCopy.name;
            payload.cancelled = cancelFlag->load();

            if (!payload.cancelled) {
                try {
                    payload.outcome = DispatchInvocation(
                        invCopy, ctxCopy, nullptr, nullptr, nullptr, nullptr);

                    // dispatchOnWorker is only for synchronous ToolSpecs.  A
                    // specialized async executor returning Async here would
                    // have no matching completion route and must fail loudly.
                    if (payload.outcome.status == DispatchStatus::Async) {
                        payload.outcome.status = DispatchStatus::Invalid;
                        payload.outcome.result.toolTag = invCopy.name;
                        payload.outcome.result.invocationRaw = invCopy.rawBlock;
                        payload.outcome.result.toolName = invCopy.name.empty()
                            ? std::string("Tool") : invCopy.name;
                        payload.outcome.result.commandEcho =
                            MakeCommandEcho(invCopy.name, invCopy.args);
                        payload.outcome.result.chips = { "error" };
                        payload.outcome.result.errorBody =
                            "Internal tool registration error: a generic worker "
                            "tool attempted to start a second async executor.";
                    }
                }
                catch (const std::exception& ex) {
                    payload.outcome = MakeInvalidOutcome(
                        invCopy,
                        std::string("Background tool failed: ") + ex.what());
                }
                catch (...) {
                    payload.outcome = MakeInvalidOutcome(
                        invCopy, "Background tool failed with an unknown error.");
                }
            }
            else {
                payload.outcome = MakeInvalidOutcome(
                    invCopy, "Tool cancelled before execution began.");
                payload.outcome.result.chips = { "cancelled" };
            }

            payload.cancelled = payload.cancelled || cancelFlag->load();
            running->store(false);

            auto* ev = new wxCommandEvent(wxEVT_TOOL_WORKER_COMPLETE);
            ev->SetClientObject(
                new ToolWorkerResultClientData(std::move(payload)));
            LbQueueEventIfAlive(handler, aliveToken, ev);
        }).detach();
    }
    catch (...) {
        m_isRunning->store(false);
        return false;
    }

    return true;
}

DispatchOutcome DispatchInvocation(const ToolInvocation& inv,
                                   const ToolContext&    ctx,
                                   GrepExecutor*         grepExec,
                                   CmdExecutor*          cmdExec,
                                   PythonRunner*         pythonRunner,
                                   WebFetchExecutor*     webFetchExec)
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
    deps.webFetchExec = webFetchExec;

    DispatchOutcome outcome = spec->dispatch(inv, ctx, deps);

    // ─── Sync/async registration cross-check ────────────────────
    // Catches the coordination bug where a registration declares
    // safety.isAsync but the dispatch body returns Completed, or
    // vice versa.
    //
    // Keep the assert() for debug builds, but also degrade loudly in
    // release builds.  The dangerous mismatch is a sync-registered
    // tool returning Async: AgentController would wait for a worker
    // completion event that will never arrive.  Returning Invalid here
    // keeps the agent loop alive and gives the model/user a visible
    // "report this bug" result instead of a silent hang.
    //
    // Invalid status is excluded: an arg-validation failure can
    // legitimately short-circuit either an async or sync tool to
    // Invalid, and we do not want to mask those returns.
    if (outcome.status == DispatchStatus::Async && !spec->safety.isAsync) {
        assert(spec->safety.isAsync &&
               "Tool dispatch returned Async but safety.isAsync is false. "
               "Either set safety.isAsync = true at registration or return Completed.");
        return MakeInvalidOutcome(
            inv,
            "Internal tool registration error: tool returned Async but is "
            "registered as synchronous. Report this LlamaBoss bug.");
    }

    if (outcome.status == DispatchStatus::Completed && spec->safety.isAsync) {
        assert(!spec->safety.isAsync &&
               "Tool dispatch returned Completed but safety.isAsync is true. "
               "Either set safety.isAsync = false at registration or return Async.");
        return MakeInvalidOutcome(
            inv,
            "Internal tool registration error: tool returned Completed but is "
            "registered as asynchronous. Report this LlamaBoss bug.");
    }

    return outcome;
}
