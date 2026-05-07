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
// User stop: calling Cancel() sets m_cancelled, which causes the
// next event handler to stop the loop and emit OnAgentLoopEnd.
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
struct CmdResult;
struct PythonRunResult;

class AgentController {
public:
    // Hard limits.  Exposed publicly so tests and the malformed-
    // counter UI messages can reference the same constants.
    static constexpr int kMaxIterations       = 12;
    static constexpr int kMaxMalformedPerTurn = 3;

    // Phase 7: repeated-tool guard.  If the same normalized
    // tool signature would appear this many times inside the
    // most recent window, stop the loop before dispatch.
    static constexpr int kLoopGuardWindow          = 5;
    static constexpr int kLoopGuardRepeatThreshold = 3;

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
    };

    // Phase 5: takes an AgentEventSink* in the slot Phase 4 used for
    // ChatDisplay*.  MyFrame implements the sink and passes `this`.
    AgentController(std::unique_ptr<ChatHistory>& history,
                    AgentEventSink* sink,
                    AppState*       appState,
                    GrepExecutor*   grepExec,
                    CmdExecutor*    cmdExec,
                    PythonRunner*   pythonRunner);
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
    void Cancel();

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
    bool HandlePythonComplete(const PythonRunResult& pythonResult);
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

    // Render result + append to history, then fire next request.
    // Shared between sync dispatch and async completion.
    void FeedResultAndIterate(const ToolInvocationResult& r);

    // Phase 7: build and track recent tool signatures so a small
    // model cannot spin forever on the same exact call.
    std::string BuildToolSignature(const ToolInvocation& inv) const;
    bool WouldTripLoopGuard(const ToolInvocation& inv,
                            std::string&          signatureOut,
                            int&                  repeatCountOut) const;
    void RecordToolSignature(const std::string& signature);

    // Phase 9: emit a non-rendered ToolCall AgentEvent before a
    // valid invocation is approved/dispatched.  Useful for future
    // sub-agent forwarders, logging, and tests.
    void EmitToolCallEvent(const ToolInvocation& inv);

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

    Callbacks m_cb;

    // Loop state.
    bool          m_active               = false;
    bool          m_cancelled            = false;
    int           m_iterationsUsed       = 0;
    int           m_consecutiveMalformed = 0;
    std::vector<std::string> m_recentToolSignatures;

    // When the current iteration triggered an async tool call, we
    // stash the invocation here so HandleGrepComplete can build the
    // ToolBlock and round-trip it to history properly.
    ToolInvocation m_pendingAsyncInvocation;
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

    // Phase 3c-ii: tool_call_id of the currently-dispatching
    // invocation, threaded into AddToolResultMessage so the next
    // request can emit a properly-tagged role:"tool" reply.  Empty
    // for XML-protocol invocations (no ids exist there) and
    // between iterations.
    std::string    m_currentToolCallId;
};
