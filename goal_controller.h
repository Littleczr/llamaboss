// goal_controller.h — Goals subsystem extracted from MyFrame.
//
// Owns the full goal lifecycle that previously lived inline in
// LlamaBoss.cpp: the /goal command surface, the hidden contract-builder
// and verifier control turns, automatic continuation turns, structured
// AgentEvent evidence capture, and the goal block of the system prompt.
//
// Ownership rules (same split as before, now enforced by the seam):
//   - ChatHistory owns all durable goal data (GoalState).  This
//     controller only reads it and calls its mutators.
//   - GoalController owns the *transient* orchestration flags that used
//     to be MyFrame members: contract-builder/verifier in-flight,
//     auto-start-after-contract-build, loop-saw-tool-output,
//     auto-continuation-turn, verifier-manual-only.
//   - MyFrame keeps the chat-state machine, generation id, streaming
//     UI, and the assistant-delta batcher; this controller drives them
//     through Callbacks so the sequencing stays byte-identical to the
//     pre-extraction code.
//
// Threading: every method must be called on the UI thread, same as the
// MyFrame methods they replace.  Deferred work goes through
// Callbacks::callAfter, which the frame implements as
// CallAfter + m_isClosing guard, so lambdas queued here are dropped
// once the frame begins closing.

#pragma once

#include <functional>
#include <memory>
#include <string>

#include "agent_event.h"          // AgentEvent, AgentEventType, AgentEndReason
#include "goal_prompt_builder.h"  // GoalContractPromptInput, GoalVerifierPromptInput
#include "tool_protocol.h"        // ToolProtocol

class AgentController;
class AppState;
class ChatClient;
class ChatDisplay;
class ChatHistory;
class ConversationController;
class ModelSwitcher;
struct ToolBlock;

class GoalController {
public:
    // Frame-owned concerns the controller drives but does not own.
    // Wired in CreateControllersAndCallbacks(), after every coordinator
    // exists.  All callbacks are required; SetCallbacks asserts none
    // are empty in debug builds.
    struct Callbacks {
        // m_chatState != ChatState::Idle.  Gates verify/continue/rebuild
        // and the Begin* entry points, exactly as before.
        std::function<bool()> isBusy;

        // m_isClosing.  Checked at the top of the Begin* methods.
        std::function<bool()> isClosing;

        // m_agentModeEnabled.  Continuation turns and automatic
        // verification require agent mode.
        std::function<bool()> isAgentModeEnabled;

        // ++m_generationId; returns the new id.  Used when starting
        // hidden control turns and continuation turns, and when
        // stopping a hidden turn (the bump orphans late deltas).
        std::function<unsigned long()> bumpGenerationId;

        // m_chatState = ChatState::Streaming.  Paired with
        // setStreamingUi(true) when a turn starts.
        std::function<void()> setChatStateStreaming;

        // SetStreamingState(bool) — the stop-button / input-lock UI.
        // Note: SetStreamingState(false) is what returns m_chatState
        // to Idle, same as today.
        std::function<void(bool)> setStreamingUi;

        // DiscardPendingAssistantDelta() — flushes the batcher's
        // buffered text without rendering.  Called before hidden turns
        // start and before their completions are consumed.
        std::function<void()> discardPendingAssistantDelta;

        // RefreshGoalStatusStrip().
        std::function<void()> refreshGoalStatusStrip;

        // BuildAgentSystemPrompt() — continuation turns are full agent
        // turns and reuse the frame's (cached) agent prompt assembly.
        std::function<std::string()> buildAgentSystemPrompt;

        // AppendSkillsBlock(...) rendered to a string; empty when no
        // skills are listed.  Feeds BuildSkillContextBlock for the
        // contract-builder and verifier prompts.
        std::function<std::string()> skillsBlock;

        // _activeProtocol — native vs XML tool protocol for the
        // continuation request body.
        std::function<ToolProtocol()> getActiveProtocol;

        // GetCachedToolsArrayJson() — only consulted when the active
        // protocol is Native.
        std::function<std::string()> getCachedToolsArrayJson;

        // ResetAgentToolStreamFilter() — continuation turns reset the
        // tool-stream detector exactly like a user-initiated agent turn.
        std::function<void()> resetAgentToolStreamFilter;

        // Frame CallAfter with the m_isClosing guard applied by the
        // frame:  CallAfter([this, fn]{ if (m_isClosing) return; fn(); });
        // Used for deferred status cards and deferred Begin* calls so
        // hidden-turn completions fully unwind before follow-up work.
        std::function<void(std::function<void()>)> callAfter;
    };

    // chatHistory is passed as the owning unique_ptr (house style —
    // matches AgentController / ConversationController) so the
    // controller always sees the live conversation even if the pointer
    // is ever swapped.
    GoalController(std::unique_ptr<ChatHistory>& chatHistory,
                   ChatDisplay*                  chatDisplay,
                   AppState&                     appState,
                   ChatClient&                   chatClient,
                   ModelSwitcher&                modelSwitcher,
                   AgentController&              agentController,
                   ConversationController&       convController);

    void SetCallbacks(Callbacks cb);

    // ── User-facing command surface ─────────────────────────────────
    // Everything /goal, the natural-language goal phrases, and the
    // Goal menu items route here (the menu's text-entry dialog stays
    // in the frame; it just forwards the typed objective).
    // Was: MyFrame::HandleSlashGoal.
    void HandleSlashGoal(const std::string& arg);

    // Was: MyFrame::DisplayGoalStatus (OnGoalStatus menu handler).
    void DisplayGoalStatus();

    // ── System-prompt assembly ──────────────────────────────────────
    // The goal block injected into normal and agent system prompts.
    // Was: MyFrame::BuildActiveGoalContextBlock (+ AppendGoalObjectiveBlock).
    std::string BuildActiveGoalContextBlock() const;

    // ── Agent loop observation (forwarded by MyFrame's AgentEventSink) ─
    // Was: the goal lines inside OnAgentLoopBegin / OnAgentToolBlock /
    // OnAgentEvent.
    void NoteAgentLoopBegin();                              // resets per-turn observation
    void NoteAgentToolOutput();                             // marks real work for verification gating
    void RecordStructuredAgentEvidence(const AgentEvent& event);

    // Goal-specific portion of OnAgentLoopEnd: surfaces the
    // interruption message for non-Normal end reasons, notes it in
    // GoalState, refreshes the strip, auto-saves, latches whether a
    // verification pass should follow, and clears the per-turn flags.
    // Returns true if it already auto-saved (the frame then skips its
    // own AutoSaveConversation, preserving today's single-save
    // behavior).  Call MaybeScheduleVerificationAfterLoopEnd() after
    // the frame's stream teardown completes.
    bool NoteAgentLoopEnd(AgentEndReason reason);
    void MaybeScheduleVerificationAfterLoopEnd();

    // ── Hidden control-turn routing ─────────────────────────────────
    // Call from OnAssistantComplete / OnAssistantError after the
    // skill-draft-builder check, before any normal-chat finalization.
    // Returns true when a goal hidden turn (contract builder or
    // verifier) consumed the event; the frame must then return
    // immediately.  Was: the m_goalContractBuilderInFlight /
    // m_goalVerifierInFlight branches + Handle*Complete / Handle*Error.
    bool ConsumeAssistantComplete(const std::string& fullResponse);
    bool ConsumeAssistantError(const std::string& error);

    // True while either hidden goal turn is streaming.  Used by
    // OnAssistantDelta (alongside the skill-draft flag) to discard
    // deltas instead of rendering them.
    bool AnyHiddenTurnInFlight() const
    {
        return m_contractBuilderInFlight || m_verifierInFlight;
    }

    // ── Stop button ─────────────────────────────────────────────────
    // Goal section of OnStopGeneration: stops an in-flight contract
    // build or verification (including the contract-failed bookkeeping
    // and status messages).  Returns true when consumed; the frame
    // checks this after the skill-draft stop and before the agent-loop
    // cancellation block.
    bool HandleStopGeneration();

    // ── Awaiting-user reply capture ─────────────────────────────────
    // Goals Phase 12: a plain user reply while the goal is awaiting
    // input is recorded as the answer instead of starting a new turn.
    // Returns true when captured; the frame then clears attachments
    // and aborts the send, same as today.
    // Was: the HasAwaitingUserGoal block in PrepareAndRecordUserTurn.
    bool TryCaptureAwaitingUserReply(const std::string& userInput);

    // ── Lifecycle ───────────────────────────────────────────────────
    // Clears transient orchestration flags (in-flight markers,
    // auto-start latch, per-turn observation).  Call on new chat /
    // conversation switch for defense in depth — today IsBusy()
    // blocks switching mid-turn, but the reset is cheap insurance.
    void ResetTransientState();

private:
    // ── Contract builder (Goals Phase 3) ────────────────────────────
    void BeginContractBuildIfNeeded();
    void HandleContractBuilderComplete(const std::string& builderResponse);
    void HandleContractBuilderError(const std::string& error);
    GoalContractPromptInput BuildContractPromptInput() const;

    // ── Verifier (Goals Phase 2) ────────────────────────────────────
    void BeginVerificationIfNeeded(bool manualOnly = false);
    void HandleVerifierComplete(const std::string& verifierResponse);
    void HandleVerifierError(const std::string& error);
    GoalVerifierPromptInput BuildVerifierPromptInput() const;

    // ── Automatic continuation ──────────────────────────────────────
    void BeginContinuationTurn(const std::string& verifierReason);
    std::string TurnInterruptionMessage(AgentEndReason reason) const;

    // ── Evidence + prompt-context builders ──────────────────────────
    const char* StructuredEvidenceEventLabel(AgentEventType type) const;
    std::string StructuredEvidenceChips(const ToolBlock& block) const;
    void        AppendStructuredAgentEvidence(std::string chunk);
    std::string BuildStructuredAgentEvidence() const;
    std::string BuildVerifierRecentEvidence() const;
    std::string LatestAssistantReplyForAwaitUserFallback() const;
    bool        LatestAssistantReplyLooksLikeBlockingUserQuestion() const;
    std::string BuildProjectContextBlock(const char* purposeLabel) const;
    std::string BuildSkillContextBlock(const char* purposeLabel) const;


    // ── Services ────────────────────────────────────────────────────
    std::unique_ptr<ChatHistory>& m_chatHistory;
    ChatDisplay*                  m_chatDisplay;
    AppState&                     m_appState;
    ChatClient&                   m_chatClient;
    ModelSwitcher&                m_modelSwitcher;
    AgentController&              m_agentController;
    ConversationController&       m_convController;
    Callbacks                     m_cb;

    // ── Transient orchestration state (moved from MyFrame) ──────────
    static constexpr int kMaxAutoContinuations = 3;
    bool m_loopSawToolOutput        = false;
    bool m_autoContinuationTurn     = false;
    bool m_autoStartAfterContractBuild = false;
    bool m_contractBuilderInFlight  = false;
    bool m_verifierInFlight         = false;
    bool m_verifierManualOnly       = false;

    // Latched by NoteAgentLoopEnd, consumed by
    // MaybeScheduleVerificationAfterLoopEnd.
    bool m_pendingVerificationAfterLoopEnd = false;
};
