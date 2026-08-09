// agent_controller.h
//
// Phase 9: Agent harness — typed event envelope.
//
// Phase 7 added the small multi-step loop and repeated-tool guard.
// Phase 8 added replayable status cards for safety stops.
// Phase 9 routes loop notifications through a typed AgentEvent object
// while preserving the existing sink callbacks and UI behavior.
//
// Drives the agent's inner loop: parse assistant response → dispatch
// tool → append result to history → send next request → repeat until
// the model stops emitting tool calls or a stop condition fires.
//
// ─── Architecture ────────────────────────────────────────────────
// Event-driven, not blocking.  The controller is a state machine
// whose transitions fire on existing wx events:
//
//   wxEVT_ASSISTANT_COMPLETE  → HandleAssistantComplete()
//   wxEVT_ASSISTANT_ERROR     → HandleAssistantError()
//   wxEVT_GREP_COMPLETE       → HandleGrepComplete()
//   wxEVT_CMD_COMPLETE        → HandleCmdComplete()
//   wxEVT_PYTHON_COMPLETE     → HandlePythonComplete()
//   wxEVT_TOOL_WORKER_COMPLETE→ HandleToolWorkerComplete()
//
// MyFrame's normal handlers for these events check IsAgentActive()
// and route through us first; we decide whether to swallow the
// event (loop continues) or let normal flow proceed (loop ended).
//
// ─── Phase 5: AgentEvents ────────────────────────────────────────
// Phase 4 mixed two concerns inside Callbacks: the *logic* of
// driving a chat (sendRequest, buildSystemPrompt, bumpGenerationId,
// getActiveProtocol, buildToolContext) and the *UI side-effects* of
// running a loop (begin a new iteration's prefix + dots, finalize
// streaming on loop end).  Phase 4 also reached into ChatDisplay
// directly to render tool blocks and system messages.
//
// Phase 5 splits those:
//   - Logic callbacks stay in `Callbacks`.  These are pure functions
//     with no UI semantics.
//   - UI-shaped concerns move to AgentEventSink.  The controller no
//     longer holds a ChatDisplay pointer; it posts structured events
//     (loop begin, iteration begin, tool block ready, loop end) to
//     the sink, which MyFrame implements.
//
// This unblocks P6 (approval cards intercept ToolBlock events before
// rendering), P9 (sub-agents implement AgentEventSink to forward
// events to the parent loop), and any future test harness that
// wants to drive the controller without a wx UI in the loop.
//
// ─── Loop control ────────────────────────────────────────────────
//   kMaxIterations      : hard cap on tool calls per user turn.
//   kMaxMalformedPerTurn: bail out if the model produces this many
//                         un-parseable <tool_call> blocks in a row.
//
// User stop: calling Cancel() sets m_cancelled. Async tool workers
// finish through their completion event; a cancelled model stream
// must be finalized explicitly with FinishCancelledStream() because
// ChatClient intentionally suppresses terminal events after Stop.
//
// ─── ChatHistory lifetime ────────────────────────────────────────
// We hold a REFERENCE to MyFrame's std::unique_ptr<ChatHistory>,
// not a raw pointer to the underlying object.  Reason: loading a
// new conversation does `m_chatHistory = std::move(newHistory)` in
// ConversationController, which destroys the OLD ChatHistory and
// binds a new one.  A raw ChatHistory* captured at construction
// would dangle after that swap; the next agent iteration would
// write into freed heap and crash inside vector::push_back's
// iterator-debug walk.  Going through the unique_ptr reference
// always reaches the current ChatHistory.  Mirrors what
// ModelSwitcher and ConversationController already do.
//
#pragma once

#include "agent_event.h"
#include "tool_context.h"
#include "tool_invocation.h"
#include "tool_dispatcher.h"
#include "tool_protocol.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class ChatHistory;
class ChatClient;
class AppState;
class GrepExecutor;
class CmdExecutor;
class PythonRunner;
class WebFetchExecutor;
class WaitExecutor;
struct CmdResult;
struct PythonRunResult;
struct WebFetchResult;
struct WaitResult;

class AgentController {
public:
    // Hard limits.  Exposed publicly so tests and the malformed-
    // counter UI messages can reference the same constants.
    static constexpr int kMaxIterations       = 12;
    static constexpr int kMaxMalformedPerTurn = 3;

    // wait tool bounds.  Per-call range enforced at validation and
    // again at dispatch; the per-turn budget caps cumulative wait
    // time so a monitoring loop cannot hold a turn open forever.
    // Waits are exempt from the tool-step cap (they cost time, not
    // tokens) -- the budget is their own resource limit.  The floor
    // also blocks tiny spam waits from being used to reset the loop
    // guards cheaply (a completed wait clears the guard window).
    static constexpr int kMinWaitSecondsPerCall     = 15;
    static constexpr int kMaxWaitSecondsPerCall     = 600;
    static constexpr int kDefaultWaitBudgetSeconds  = 1800;   // 30 min/turn
    static constexpr int kMinConfigurableWaitBudget = 60;
    static constexpr int kMaxConfigurableWaitBudget = 14400;  // 4 h

    // Phase 7: repeated-tool guard.  If the same normalized
    // tool signature would appear this many times inside the
    // most recent window, stop the loop before dispatch.
    static constexpr int kLoopGuardWindow          = 5;
    static constexpr int kLoopGuardRepeatThreshold = 3;

    // Phase 7c: cycle guard for A/B/C/A/B/C-style loops that never
    // repeat one exact signature enough times to trip the exact-repeat
    // guard but are still visibly stuck.
    static constexpr int kCycleGuardWindow      = 6;
    static constexpr int kCycleGuardMaxDistinct = 3;
    static constexpr int kCycleGuardMinRepeats  = 2;

    // Phase 10: multi-tool dispatch.  Hard ceiling on how many native
    // tool calls from one assistant turn are queued for sequential
    // execution.  Anything past the ceiling is dropped from the
    // persisted tool_calls sidecar and reported to the model via a
    // soft-hint notice so it can re-issue the remainder next turn.
    static constexpr int kMaxNativeCallsPerTurn = 8;

    // Bounds for the user-configurable tool-step cap (see
    // SetMaxToolSteps).  kMaxIterations stays as the default.
    static constexpr int kMinConfigurableToolSteps = 4;
    static constexpr int kMaxConfigurableToolSteps = 60;

    // Logic-only callbacks.  Phase 5 stripped the UI-shaped entries
    // (beginNextIteration, onLoopEnd) — those moved to
    // AgentEventSink::OnAgentIterationBegin / OnAgentLoopEnd.
    struct Callbacks {
        // Kick off the next chat request.  Body is the full JSON
        // payload (history + generation params).  generationId is
        // the caller's per-turn monotonic counter used by the
        // existing event-vs-current-gen guard in MyFrame.
        std::function<bool(const std::string& model,
                           const std::string& body,
                           unsigned long      generationId)> sendRequest;

        // Supplies a fresh ToolContext for each dispatch.  Separate
        // from Begin() so caps (ctxTokens) stay current across
        // Settings changes mid-conversation.
        std::function<ToolContext()> buildToolContext;

        // Produces the system prompt to prepend to each request
        // while the loop is active.  MyFrame owns the prompt text
        // so it can evolve without touching the controller.
        std::function<std::string()> buildSystemPrompt;

        // Bumps MyFrame's generation ID and returns the new value.
        // Used between iterations so stale deltas from a previous
        // request can't land on a later loop step.
        std::function<unsigned long()> bumpGenerationId;

        // Phase 3c-i: returns the active model's tool protocol so
        // the request builder can decide whether to attach a
        // function-calling "tools" array.  Returning ToolProtocol::
        // Native enables the tool catalog; Xml/Unknown disable it.
        // Optional — when unset (or returning Unknown) the agent
        // uses XML-protocol behaviour.
        std::function<ToolProtocol()> getActiveProtocol;

        // The wire "model" field for this conversation's requests.
        //
        // MUST NOT be AppState::GetModel().  That is app-GLOBAL state:
        // ModelService::SetActiveTarget writes it on every model
        // activation in ANY window, so a model switch in window B
        // rewrote the model name that window A's in-flight agent loop
        // put on its next request.  A remote (OpenRouter) loop then
        // sent a local .gguf path as the model ID and the provider
        // rejected it with a 400.
        //
        // The transport was already pinned per conversation (see the
        // sendRequest lambda in LlamaBoss.cpp, which discards its
        // `model` argument and resolves the target itself) -- this
        // pins the payload to match.
        //
        // Optional: when unset the controller falls back to the
        // app-global model, preserving the previous behaviour for any
        // caller that has not wired it up.
        std::function<std::string()> resolveWireModel;
    };

    // Phase 5: takes an AgentEventSink* in the slot Phase 4 used for
    // ChatDisplay*.  MyFrame implements the sink and passes `this`.
    AgentController(std::unique_ptr<ChatHistory>& history,
                    AgentEventSink* sink,
                    AppState*       appState,
                    GrepExecutor*   grepExec,
                    CmdExecutor*    cmdExec,
                    PythonRunner*   pythonRunner,
                    WebFetchExecutor* webFetchExec,
                    ToolWorkerExecutor* toolWorker,
                    WaitExecutor*   waitExec);
    ~AgentController() = default;

    void SetCallbacks(Callbacks cb) { m_cb = std::move(cb); }

    // ─── Loop lifecycle ──────────────────────────────────────────
    // Called by MyFrame when the USER sends a message in agent
    // mode.  The user's message must already be added to history
    // and a generation request must be about to fire — Begin()
    // simply arms the controller to treat the upcoming streaming
    // reply as iteration 1.  Phase 5: also fires
    // OnAgentLoopBegin() so the frame can hook loop-scoped UI.
    void Begin();

    // Phase 10: user-configurable tool-step cap.  Clamped to
    // [kMinConfigurableToolSteps, kMaxConfigurableToolSteps]; takes
    // effect on the NEXT cap comparison, so raising it mid-loop
    // extends the current loop and lowering it can stop the loop at
    // the next fed result.  Persisted by AppState; wired by MyFrame.
    void SetMaxToolSteps(int steps);
    int  GetMaxToolSteps() const { return m_maxToolSteps; }

    // Session-level per-turn wait-time budget (seconds).  Clamped to
    // [kMinConfigurableWaitBudget, kMaxConfigurableWaitBudget]; wired
    // by MyFrame's /wait_budget command.  Not persisted (v1).
    void SetWaitBudgetSeconds(int seconds);
    int  GetWaitBudgetSeconds() const { return m_waitBudgetSeconds; }

    // True iff a loop is in progress.  MyFrame uses this to
    // decide whether to route events through us.
    bool IsActive() const { return m_active; }

    // True while an async agent-owned tool worker is running.  MyFrame
    // uses this to avoid treating Stop as a chat-stream cancel when the
    // agent is actually waiting on grep or PowerShell.
    bool IsAwaitingAsyncResult() const { return m_awaitingAsyncResult; }

    // Phase 6: true while the loop is paused on an approval card.
    // MyFrame keeps the input enabled in this state so /approve or
    // /deny can resolve the pending tool invocation.
    bool IsAwaitingApproval() const { return m_awaitingApproval; }

    // Phase 6: resolve a pending approval.  Approve executes the
    // stored invocation; Deny records a denied tool result and lets
    // the model continue; Cancel records a cancelled result and ends
    // the loop without another model request.
    //
    // rememberForChat: when true, enables one-approval mode for the
    // rest of the conversation so subsequent approval-required tools
    // dispatch without re-prompting.  In the polished UI, plain
    // approve maps here; approve once passes false.
    bool ApprovePendingTool(bool rememberForChat = false);
    bool DenyPendingTool();
    bool CancelPendingApproval();

    // User-initiated cancel (Stop button).  Safe no-op if inactive.
    // Async tool workers still finish through their completion event.
    void Cancel();

    // Complete cancellation when Stop terminated the active MODEL stream.
    // ChatClient posts no completion/error event after cancellation, so the
    // frame must call this after stopping the transport. Returns true only
    // when it ended an active, non-worker, non-approval agent turn. The frame
    // owns the already-visible "Generation stopped by user" message.
    bool FinishCancelledStream();

    // ─── Event handlers (called from MyFrame on event dispatch) ──
    // Each returns true iff the controller "consumed" the event:
    //   - HandleAssistantComplete returns true if the reply had a
    //     tool call and another iteration is queued; false means
    //     the loop has ended and MyFrame should do its normal
    //     completion (finalize history, auto-save, etc.).
    //   - HandleGrepComplete returns true if the result was fed
    //     back to the model and the next iteration is in flight.
    //   - HandleAssistantError always returns false (errors always
    //     end the loop).
    //
    // Phase 3c-ii: toolCallsJson is the structured tool_calls
    // payload from the streaming response (empty when the model
    // emitted no native tool calls — typical XML-protocol case).
    // When present and non-empty AND the active protocol is Native,
    // the controller bypasses the XML stream-detector path and
    // synthesizes invocations directly from the structured calls.
    bool HandleAssistantComplete(const std::string& fullResponse,
                                 const std::string& toolCallsJson = "");
    bool HandleGrepComplete(const struct GrepResult& grepResult);
    bool HandleCmdComplete(const CmdResult& cmdResult);

    // wait completion (WaitExecutor timer expiry or Cancel).  Mirrors
    // HandleCmdComplete's consume-or-decline contract.  A completed
    // (non-cancelled) wait also clears the loop-guard signature
    // window: time passed, so identical re-checks of external state
    // are legitimate again rather than doom-loop evidence.
    bool HandleWaitComplete(const WaitResult& waitResult);
    bool HandlePythonComplete(const PythonRunResult& pythonResult);
    bool HandleWebFetchComplete(const WebFetchResult& webResult);
    bool HandleToolWorkerComplete(const ToolWorkerResult& workerResult);
    bool HandleCmdError(const std::string& errorText);
    bool HandlePythonError(const std::string& errorText);
    bool HandleWebFetchError(const std::string& errorText);
    bool HandleAssistantError(const std::string& errorText);

private:
    // Dispatch a parsed invocation.  Sync tools render + feed
    // result + start next iteration synchronously.  Async tools
    // (grep) return without starting the next iteration; the next
    // step happens in HandleGrepComplete.
    // Returns true iff loop should continue; false on any error
    // that ends the loop.
    bool DispatchAndContinue(const ToolInvocation& inv);

    // Phase 6: same dispatch body after the user has approved the
    // paused invocation.  This deliberately skips the approval check
    // so /approve does not re-open the same card forever.
    bool DispatchApprovedAndContinue(const ToolInvocation& inv,
                                     const ToolContext&    ctx);

    // Shared result handling for ordinary synchronous dispatches and
    // ToolWorkerExecutor completions.  Preserves the create-script
    // one-shot approval and standalone-write terminal heuristics.
    bool FinishDispatchedInvocation(const ToolInvocation& inv,
                                    const ToolContext&    ctx,
                                    const DispatchOutcome& out);

    // Render result + append to history, then continue the loop
    // (next queued batch call, or next model request).  Shared
    // between sync dispatch and async completion.  Phase 10 split
    // the body into FeedResultOnly + ContinueLoop so the deny path
    // can interleave a batch drain between the two halves.
    void FeedResultAndIterate(const ToolInvocationResult& r,
                              bool countTowardIterationCap = true);

    // First half: render the result, round-trip it to history with
    // the step trailer, consume m_currentToolCallId, and advance the
    // iteration counter when counted.
    void FeedResultOnly(const ToolInvocationResult& r,
                        bool countTowardIterationCap);

    // Second half: cap / cancel checks (draining any queued batch
    // calls before ending), then either dispatch the next queued
    // invocation or fire the next model request.
    void ContinueLoop();

    // ─── Phase 10: native multi-call batch queue ─────────────────
    // Pops and dispatches the next queued invocation from the current
    // assistant turn.  Returns the DispatchAndContinue result.
    bool DispatchNextQueuedInvocation();

    // Emits and stores a terminal "skipped" tool result (threaded with
    // each call's tool_call_id so native transcript pairing survives)
    // for every still-queued invocation, then clears the queue.  Must
    // run BEFORE any EndLoop that abandons a partially-executed batch.
    void DrainQueuedInvocationsWithSkippedResults(const std::string& reason);

    // Shared tail for every async tool completion.  The four
    // HandleXxxComplete handlers used to each repeat the same
    // cancel-vs-continue epilogue (render partial result, round-trip
    // to history, EndLoop(Cancelled) — or FeedResultAndIterate).
    // They now build their tool-specific ToolInvocationResult and
    // delegate here, so the next async tool cannot add a fifth copy
    // of the epilogue.  `cancelled` is (m_cancelled || worker reported
    // cancelled); `inv` supplies the tool_call_id for the cancel-path
    // history record.
    // countTowardIterationCap: waits are exempt (they consume the
    // wait-time budget instead of tool steps); every other async
    // tool counts as before.
    bool FinishAsyncToolResult(const ToolInvocation&       inv,
                               const ToolInvocationResult& r,
                               bool                        cancelled,
                               bool countTowardIterationCap = true);

    // wait tool dispatch: parse '<seconds> [reason]', enforce the
    // per-call bounds and the per-turn budget, arm the WaitExecutor
    // timer, and suspend the loop on m_awaitingAsyncResult exactly
    // like the specialized async executors.  Called from the agent
    // dispatch path BEFORE DispatchInvocation.
    bool StartWaitTool(const ToolInvocation& inv, const ToolContext& ctx);
    static bool ParseWaitArgs(const std::string& args,
                              int&               secondsOut,
                              std::string&       reasonOut);

    // Phase 7: build and track recent tool signatures so a small
    // model cannot spin forever on the same exact call.
    std::string BuildToolSignature(const ToolInvocation& inv) const;
    bool WouldTripLoopGuard(const ToolInvocation& inv,
                            std::string&          signatureOut,
                            int&                  repeatCountOut) const;
    bool WouldTripCycleGuard(const ToolInvocation& inv,
                             std::string&          signatureOut,
                             int&                  distinctCountOut) const;
    void RecordToolSignature(const std::string& signature);

    // Phase 7d: called when a dispatched invocation's result lands.
    // Marks the most recent matching record's outcome; on success,
    // purges earlier FAILED records of the same signature (see the
    // ToolSignatureRecord comment for the rationale).
    //
    // Phase 7e: outputText is the success body of the result.  Its
    // hash is stored on the record and drives the progress purge (see
    // ToolSignatureRecord).  Callers on failure paths may omit it.
    void ResolveToolSignatureOutcome(const ToolInvocation& inv,
                                     bool                  failed,
                                     const std::string&    outputText
                                         = std::string());

    // Phase 7e: FNV-1a 64-bit hash of a tool result body with
    // whitespace runs collapsed.  Static so tests can call it.
    static uint64_t HashToolOutputForLoopGuard(const std::string& text);

    // Phase 9/Phase 3 trace: emit a non-rendered ToolCall AgentEvent
    // at the approved dispatch point.  Useful for sub-agent forwarders,
    // logging, and tests.
    void EmitToolCallEvent(const ToolInvocation& inv,
                           const std::string&   signature);

    // Render + persist a terminal tool-style result without asking
    // the model for another iteration.  Used by cancellation,
    // loop-guard, and Phase-8 safety-cap paths so saved-history
    // replay stays stable.
    void EmitAndStoreTerminalToolResult(const ToolInvocationResult& r,
                                        bool startExpanded = true);

    // Phase 8: helper for loop-ending status cards that are not
    // associated with a real tool invocation.  These cards are
    // intentionally persisted using the existing tool-block history
    // format rather than introducing a new saved-history record type.
    void EmitAndStoreAgentStatusCard(const std::string& title,
                                     const std::vector<std::string>& chips,
                                     const std::string& message,
                                     bool startExpanded = true);

    // Phase 3c-ii: parse the OpenAI-shape tool_calls array (as
    // emitted by ChatClient's accumulator) into our internal
    // ToolInvocation form.  Each invocation carries the call id
    // for downstream tool_call_id threading.  Empty result on
    // parse failure or empty input.
    std::vector<ToolInvocation> ParseStructuredToolCalls(
        const std::string& toolCallsJson);

    // Phase 3c-ii: convert an OpenAI-style structured arguments
    // JSON payload to LlamaBoss's legacy single-string args form
    // expected by the dispatchers.  Schema-aware: pulls the right
    // field(s) from the JSON object based on the tool name.
    std::string ProjectStructuredArgs(const std::string& toolName,
                                      const std::string& argsJson);

    // Phase 5: post a tool-block event to the sink.  Centralizes
    // the (otherwise four-times-repeated) ToolInvocationResult →
    // ToolBlock packing.
    void EmitToolBlock(const ToolInvocationResult& r,
                       bool startExpanded = false);

    // Lightweight non-history UI card shown as soon as an async tool starts.
    // This prevents the blank-turn feeling between approval/"yes run it"
    // and the eventual completed tool card.
    void EmitPendingToolBlock(const ToolInvocation& inv);

    // Terminate the loop, fire OnAgentLoopEnd, reset state.
    // `userFacingMessage` is forwarded to the sink — empty for
    // normal exits, populated for cap/cancel/send-fail cases the
    // user should see.
    void EndLoop(AgentEndReason     reason,
                 const std::string& userFacingMessage);

    // Build the chat request body for the next iteration.  Uses
    // ChatHistory::BuildChatRequestJson and prepends the system
    // prompt as a synthetic message at position 0.
    std::string BuildRequestBody();

    // Refs.  Non-owning — MyFrame owns the lifetimes.
    // m_history is a reference to MyFrame's unique_ptr so we follow
    // ConversationController's move-replacement on conversation load
    // (see "ChatHistory lifetime" note at top of file).
    std::unique_ptr<ChatHistory>&  m_history;
    AgentEventSink* m_sink;
    AppState*       m_appState;
    GrepExecutor*   m_grepExec;
    CmdExecutor*    m_cmdExec;
    PythonRunner*   m_pythonRunner;
    WebFetchExecutor* m_webFetchExec;
    ToolWorkerExecutor* m_toolWorker;
    WaitExecutor*   m_waitExec;

    Callbacks m_cb;

    // Single funnel for the wire model name.  Both the request builder
    // and the iteration kickoff go through this so they can never
    // disagree about which model this conversation is talking to.
    std::string ResolveWireModel() const;

    // Loop state.
    bool          m_active               = false;
    bool          m_cancelled            = false;
    int           m_iterationsUsed       = 0;
    int           m_consecutiveMalformed = 0;

    // Phase 7/7d: rolling window of recently DISPATCHED tool signatures,
    // used by the exact-repeat and cycle guards.  Phase 7d added the
    // outcome flag: when a previously-failing call later SUCCEEDS, its
    // earlier failed records are purged, because the success proves the
    // blocking state changed (e.g. run(missing) -> create_script ->
    // run(ok)) and the past failures no longer indicate a stuck loop.
    // Successful repeats still accumulate, so identical-successful-call
    // doom loops (the original Phase 7 motivation) remain guarded.
    // Phase 7e added output-hash progress detection: when a call
    // SUCCEEDS with output that differs from an earlier success of the
    // same signature, the earlier records are purged -- same question,
    // different answer means the call is doing real work (scroll ->
    // snapshot paging is the canonical transcript), so it must not
    // accumulate toward the repeat guard.  Successful repeats whose
    // output MATCHES still accumulate, so identical-successful-call
    // doom loops (same question, same answer, repeated) remain guarded
    // at the unchanged threshold.
    struct ToolSignatureRecord {
        std::string signature;
        bool        failed        = false;  // set when the call's result lands
        bool        resolved      = false;  // outcome landed (vs dispatched-only)
        bool        hasOutputHash = false;  // outputHash is valid (successes only)
        uint64_t    outputHash    = 0;      // hash of the success output body
    };
    std::vector<ToolSignatureRecord> m_recentToolSignatures;

    // Phase 10: user-configurable tool-step cap.  Defaults to the
    // historical constant; AppState persists the user's value and
    // MyFrame pushes it here at startup and on /agent_steps.
    int           m_maxToolSteps = kMaxIterations;

    // wait tool per-turn accounting.  m_waitSecondsUsed accumulates
    // GRANTED wait time (reset by Begin); m_waitBudgetSeconds is the
    // session-level cap (/wait_budget).
    int           m_waitSecondsUsed   = 0;
    int           m_waitBudgetSeconds = kDefaultWaitBudgetSeconds;

    // Phase 10: remaining native tool calls from the current assistant
    // turn, executed sequentially after the first.  Non-empty only
    // between HandleAssistantComplete and the moment the last batched
    // result has been fed back; always empty while a model request is
    // in flight.  Begin() and EndLoop() clear it defensively.
    std::vector<ToolInvocation> m_queuedInvocations;

    // Pending model-facing notice appended to the next real tool result.
    // Used for the one-step-early loop warning and for native multi-call
    // truncation notices. Cleared on Begin(), Deny/Cancel approval, and
    // EndLoop() so it cannot leak onto unrelated results.
    std::string   m_pendingSoftHint;

    // Phase 7e: loop-guard checkpoint (soft block before hard block).
    // The FIRST time a signature trips the exact-repeat guard, the
    // agent gets a model-facing challenge result instead of a hard
    // stop; the signature is remembered here, and a SECOND trip of the
    // same signature hard-blocks as before.  The cycle guard gets one
    // checkpoint per agent turn via the bool.  Both cleared on Begin()
    // and EndLoop().  Challenged calls are never dispatched and never
    // recorded, so the guard window is unchanged when the model simply
    // re-issues the identical call.
    std::string   m_challengedLoopGuardSignature;
    bool          m_cycleGuardChallenged = false;

    // When the current iteration triggered an async tool call, we
    // stash the invocation here so HandleGrepComplete can build the
    // ToolBlock and round-trip it to history properly.
    ToolInvocation m_pendingAsyncInvocation;
    ToolContext    m_pendingAsyncContext;
    bool           m_awaitingAsyncResult = false;

    // Phase 6: single pending approval slot for the agent path.
    // The invocation and context are captured at the moment the
    // model requested the tool so /approve executes exactly what was
    // previewed, even if later UI state changes.
    ToolInvocation m_pendingApprovalInvocation;
    ToolContext    m_pendingApprovalContext;
    bool           m_awaitingApproval = false;

    // Python workflow polish: approving python_create_script grants
    // one immediate agent-owned python_run_script for the exact script
    // artifact that was just created. This avoids the awkward
    // create-approve -> run-approve double prompt while still scoping
    // the auto-run to one filename and one active agent loop.
    //
    // Holds final on-disk artifact filenames (lower-cased, normalized)
    // that an immediate python_run_script can match without re-prompting.
    //
    // Do NOT include the originally-requested filename when a collision
    // rename occurs (foo.py -> foo_2.py). The bypass must apply only to
    // the script artifact the user just reviewed and approved for create.
    std::vector<std::string> m_oneShotApprovedScriptRun;
    int m_oneShotApprovedScriptRunBeginCredits = 0;

    // Phase 3c-ii: tool_call_id of the currently-dispatching
    // invocation, threaded into AddToolResultMessage so the next
    // request can emit a properly-tagged role:"tool" reply.  Empty
    // for XML-protocol invocations (no ids exist there) and
    // between iterations.
    std::string    m_currentToolCallId;
};
