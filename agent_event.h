// agent_event.h
//
// Phase 9: typed AgentEvents — a small structured event envelope
// layered on top of the Phase 5 sink callbacks.
//
// ─── Why a typed event envelope, not a full bus? ────────────────
// Phase 5 intentionally kept the controller simple: AgentController
// reports progress to an AgentEventSink with direct typed methods
// such as OnAgentToolBlock() and OnAgentLoopEnd().  That is still the
// right architecture for the current app.  Phase 9 adds a lightweight
// AgentEvent value type so future code can forward, test, inspect, or
// record events without inventing a wx event hierarchy or rewriting
// the saved-history format.
//
// Existing sinks remain source-compatible.  AgentEventSink::OnAgentEvent()
// is the new central entry point, but its default implementation simply
// dispatches to the existing Phase 5/6 methods.  MyFrame can keep its
// current overrides.  A future sub-agent, logger, or test harness can
// override OnAgentEvent() directly and forward a single object.
//
// ─── Event taxonomy ─────────────────────────────────────────────
// The enum is deliberately a little richer than today's UI needs:
//   ToolCall           : model requested a tool; may still be approval-gated.
//   ToolOutput         : ordinary tool result card.
//   ApprovalRequired   : risky invocation paused before execution.
//   FileCreated        : specialized ToolOutput for write-created.
//   EditApplied        : specialized ToolOutput for edit-applied.
//   DirectoryCreated   : specialized ToolOutput for mkdir-created.
//   FileDeleted        : specialized ToolOutput for delete-deleted.
//   Error              : malformed/error tool-style output.
//   TurnComplete       : reserved for future explicit final-answer events.
//   AgentStatus        : replayable non-tool status cards, e.g. tool cap.
//
// These specific event names give Phase 10+ room to add approval cards,
// status displays, sub-agent forwarding, and test assertions without
// changing the stable rendering code.
//
// All sink calls are still synchronous and occur on the UI thread today.
// When a future worker-thread producer appears, wrap the sink in a wx
// QueueEvent adapter; do not make AgentController depend on wx.
//
// ─── End-reason taxonomy ─────────────────────────────────────────
// Every loop ends for exactly one reason.  MyFrame uses the reason
// to decide whether to show the controller-supplied user message
// (Cancelled/IterationCap/MalformedCap/SendFailed/LoopGuard) or stay silent
// (Normal/StreamError — MyFrame already surfaces error text in its
// own AssistantError handler before the loop unwinds).
//
#pragma once

#include "tool_block.h"

#include <string>

// Existing loop-end taxonomy, kept stable for MyFrame and tests.
enum class AgentEndReason {
    // Model produced a final answer with no tool call.  The loop
    // exits cleanly and MyFrame finalizes the chat turn.
    Normal,

    // User pressed Stop.  Reached either between iterations, while
    // an async tool worker was running, or while the next assistant
    // stream was in flight.
    Cancelled,

    // Hit AgentController::kMaxIterations.  Tool calls ran but the
    // model never converged; loop bails to prevent runaway cost.
    // Phase 8 also emits a replayable Agent Status card for this.
    IterationCap,

    // Hit AgentController::kMaxMalformedPerTurn back-to-back.  The
    // model is producing un-parseable <tool_call> blocks.
    MalformedCap,

    // Streaming reply failed (HTTP error, timeout, server gone).
    // MyFrame's AssistantError path already showed the friendly
    // error message — userFacingMessage is empty.
    StreamError,

    // The next-iteration request could not be sent (chat client
    // refused, e.g. another stream still in flight).  Rare; mostly
    // a defensive code path.
    SendFailed,

    // Phase 7 guardrail: the same normalized tool call repeated
    // too many times inside a small rolling window.  The loop stops
    // before dispatching the repeated call.
    LoopGuard,
};

enum class AgentEventType {
    LoopBegin,
    IterationBegin,
    ToolCall,
    ToolOutput,
    ApprovalRequired,
    AgentStatus,
    Error,
    TurnComplete,
    FileCreated,
    EditApplied,
    DirectoryCreated,
    FileDeleted,
    LoopEnd,
};

struct AgentEvent {
    AgentEventType type = AgentEventType::ToolOutput;

    // Rendering payload for ToolOutput/ApprovalRequired/AgentStatus/
    // Error/specialized file events.  Uses the existing ToolBlock
    // shape so Phase 9 does not touch chat rendering or saved history.
    ToolBlock toolBlock;
    bool      startExpanded = false;

    // ToolCall metadata.  This is intentionally lightweight: enough
    // for future loggers/sub-agents/tests, without depending on the
    // full ToolInvocation type from this shared header.
    std::string toolName;
    std::string commandEcho;
    std::string toolCallId;

    // LoopEnd payload.
    AgentEndReason endReason = AgentEndReason::Normal;
    std::string    userFacingMessage;

    static AgentEvent LoopBegin()
    {
        AgentEvent e;
        e.type = AgentEventType::LoopBegin;
        return e;
    }

    static AgentEvent IterationBegin()
    {
        AgentEvent e;
        e.type = AgentEventType::IterationBegin;
        return e;
    }

    static AgentEvent ToolCall(const std::string& name,
                               const std::string& echo,
                               const std::string& callId)
    {
        AgentEvent e;
        e.type        = AgentEventType::ToolCall;
        e.toolName    = name;
        e.commandEcho = echo;
        e.toolCallId  = callId;
        return e;
    }

    static AgentEvent ToolOutput(const ToolBlock& block,
                                 bool expanded = false,
                                 AgentEventType specializedType = AgentEventType::ToolOutput)
    {
        AgentEvent e;
        e.type          = specializedType;
        e.toolBlock     = block;
        e.startExpanded = expanded;
        return e;
    }

    static AgentEvent ApprovalRequired(const ToolBlock& block)
    {
        AgentEvent e;
        e.type          = AgentEventType::ApprovalRequired;
        e.toolBlock     = block;
        e.startExpanded = true;
        return e;
    }

    static AgentEvent LoopEnd(AgentEndReason reason,
                              const std::string& message)
    {
        AgentEvent e;
        e.type              = AgentEventType::LoopEnd;
        e.endReason         = reason;
        e.userFacingMessage = message;
        return e;
    }
};

// Implemented by MyFrame.  AgentController calls OnAgentEvent(); the
// default bridge below fans the typed event back out to the existing
// Phase 5/6 virtual methods so current UI code remains unchanged.
class AgentEventSink {
public:
    virtual ~AgentEventSink() = default;

    virtual void OnAgentEvent(const AgentEvent& event)
    {
        switch (event.type) {
        case AgentEventType::LoopBegin:
            OnAgentLoopBegin();
            break;

        case AgentEventType::IterationBegin:
            OnAgentIterationBegin();
            break;

        case AgentEventType::ToolCall:
            // ToolCall is informational for Phase 9.  It is not rendered
            // by default, otherwise every model request would add noise.
            break;

        case AgentEventType::ApprovalRequired:
            OnAgentApprovalRequired(event.toolBlock);
            break;

        case AgentEventType::ToolOutput:
        case AgentEventType::AgentStatus:
        case AgentEventType::Error:
        case AgentEventType::FileCreated:
        case AgentEventType::EditApplied:
        case AgentEventType::DirectoryCreated:
        case AgentEventType::FileDeleted:
            OnAgentToolBlock(event.toolBlock, event.startExpanded);
            break;

        case AgentEventType::TurnComplete:
            // Reserved for a future explicit final-answer event.  Today
            // MyFrame's normal assistant-complete path owns final prose.
            break;

        case AgentEventType::LoopEnd:
            OnAgentLoopEnd(event.endReason, event.userFacingMessage);
            break;
        }
    }

    // Loop began — fired exactly once per AgentController::Begin().
    virtual void OnAgentLoopBegin() {}

    // About to fire the next chat request.
    virtual void OnAgentIterationBegin() {}

    // A tool block is ready for display.
    virtual void OnAgentToolBlock(const ToolBlock& block,
                                  bool startExpanded) = 0;

    // Phase 6: a risky tool invocation is paused before execution
    // and represented as an approval card.
    virtual void OnAgentApprovalRequired(const ToolBlock& block)
    {
        OnAgentToolBlock(block, true);
    }

    // Loop ended.  `userFacingMessage` is the text the controller
    // wants surfaced via DisplaySystemMessage — empty for Normal
    // and StreamError.
    virtual void OnAgentLoopEnd(AgentEndReason         reason,
                                const std::string&     userFacingMessage) = 0;
};
