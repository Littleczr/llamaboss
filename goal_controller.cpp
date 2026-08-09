#include "goal_controller.h"

#include <cassert>
#include <sstream>
#include <utility>

#include "agent_controller.h"
#include "app_state.h"
#include "chat_client.h"
#include "chat_display.h"
#include "chat_history.h"
#include "conversation_controller.h"
#include "goal_state.h"
#include "goal_verifier_support.h"
#include "lb_string_utils.h"
#include "model_switcher.h"
#include "project_manager.h"
#include "server_manager.h"
#include "tool_block.h"

namespace {

void AppendGoalObjectiveBlock(std::ostringstream& p,
                              const std::string& statusLabel,
                              const std::string& objective)
{
    p << "Conversation goal:\n"
      << "  Status: " << statusLabel << "\n"
      << "  Objective:\n";

    std::istringstream lines(objective);
    std::string line;
    bool wroteObjectiveLine = false;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        p << "    " << line << "\n";
        wroteObjectiveLine = true;
    }
    if (!wroteObjectiveLine) {
        p << "    (empty)\n";
    }
}

} // namespace

GoalController::GoalController(std::unique_ptr<ChatHistory>& chatHistory,
                               ChatDisplay*                  chatDisplay,
                               AppState&                     appState,
                               ChatClient&                   chatClient,
                               ModelSwitcher&                modelSwitcher,
                               AgentController&              agentController,
                               ConversationController&       convController)
    : m_chatHistory(chatHistory)
    , m_chatDisplay(chatDisplay)
    , m_appState(appState)
    , m_chatClient(chatClient)
    , m_modelSwitcher(modelSwitcher)
    , m_agentController(agentController)
    , m_convController(convController)
{
}

void GoalController::SetCallbacks(Callbacks cb)
{
#ifndef NDEBUG
    assert(cb.isBusy);
    assert(cb.isClosing);
    assert(cb.isAgentModeEnabled);
    assert(cb.bumpGenerationId);
    assert(cb.setChatStateStreaming);
    assert(cb.setStreamingUi);
    assert(cb.discardPendingAssistantDelta);
    assert(cb.refreshGoalStatusStrip);
    assert(cb.buildAgentSystemPrompt);
    assert(cb.skillsBlock);
    assert(cb.getActiveProtocol);
    assert(cb.getCachedToolsArrayJson);
    assert(cb.resetAgentToolStreamFilter);
    assert(cb.callAfter);
#endif
    m_cb = std::move(cb);
}

void GoalController::NoteAgentLoopBegin()
{
    m_loopSawToolOutput = false;
}

void GoalController::NoteAgentToolOutput()
{
    if (m_chatHistory->HasActiveGoal()) {
        m_loopSawToolOutput = true;
    }
}

bool GoalController::NoteAgentLoopEnd(AgentEndReason reason)
{
    m_pendingVerificationAfterLoopEnd =
        reason == AgentEndReason::Normal &&
        m_chatHistory->HasActiveGoal() &&
        (m_loopSawToolOutput || m_autoContinuationTurn);

    bool savedInterruptedGoal = false;

    if (reason != AgentEndReason::Normal &&
        m_chatHistory->HasActiveGoal()) {
        const std::string goalInterruption = TurnInterruptionMessage(reason);
        if (!goalInterruption.empty()) {
            m_chatHistory->NoteGoalTurnInterrupted(goalInterruption);
            m_cb.refreshGoalStatusStrip();
            m_chatDisplay->DisplaySystemMessage(goalInterruption);
            m_convController.AutoSaveConversation();
            savedInterruptedGoal = true;
        }
    }

    m_loopSawToolOutput = false;
    m_autoContinuationTurn = false;
    return savedInterruptedGoal;
}

void GoalController::MaybeScheduleVerificationAfterLoopEnd()
{
    if (!m_pendingVerificationAfterLoopEnd) return;
    m_pendingVerificationAfterLoopEnd = false;

    m_cb.callAfter([this]() {
        BeginVerificationIfNeeded();
    });
}

bool GoalController::ConsumeAssistantComplete(const std::string& fullResponse)
{
    if (m_contractBuilderInFlight) {
        HandleContractBuilderComplete(fullResponse);
        return true;
    }
    if (m_verifierInFlight) {
        HandleVerifierComplete(fullResponse);
        return true;
    }
    return false;
}

bool GoalController::ConsumeAssistantError(const std::string& error)
{
    if (m_contractBuilderInFlight) {
        HandleContractBuilderError(error);
        return true;
    }
    if (m_verifierInFlight) {
        HandleVerifierError(error);
        return true;
    }
    return false;
}

bool GoalController::HandleStopGeneration()
{
    if (m_contractBuilderInFlight) {
        m_cb.discardPendingAssistantDelta();
        m_cb.bumpGenerationId();
        m_chatClient.StopGeneration();
        m_contractBuilderInFlight = false;
        m_autoStartAfterContractBuild = false;
        m_cb.setStreamingUi(false);

        if (m_chatHistory->HasGoal()) {
            m_chatHistory->MarkGoalContractFailed(
                "Goal contract drafting was stopped by user.");
            m_convController.AutoSaveConversation();
        }
        m_chatDisplay->DisplaySystemMessage(
            "Goal contract drafting stopped by user. "
            "The goal remains active with objective-only verification.");
        return true;
    }

    if (m_verifierInFlight) {
        m_cb.discardPendingAssistantDelta();
        m_cb.bumpGenerationId();
        m_chatClient.StopGeneration();
        m_verifierInFlight = false;
        m_verifierManualOnly = false;
        m_cb.setStreamingUi(false);
        m_chatDisplay->DisplaySystemMessage(
            "Goal verification stopped by user. The goal remains active.");
        return true;
    }

    return false;
}

bool GoalController::TryCaptureAwaitingUserReply(const std::string& userInput)
{
    if (!m_chatHistory->HasAwaitingUserGoal()) return false;

    m_chatHistory->RecordGoalAwaitingUserReply(
        LbClipForGoalVerifier(userInput, 1200));
    m_convController.AutoSaveConversation();
    m_chatDisplay->DisplaySystemMessage(
        "Response recorded for the waiting goal. Say 'continue the goal' to resume work.");
    return true;
}

void GoalController::ResetTransientState()
{
    m_loopSawToolOutput = false;
    m_autoContinuationTurn = false;
    m_autoStartAfterContractBuild = false;
    m_contractBuilderInFlight = false;
    m_verifierInFlight = false;
    m_verifierManualOnly = false;
    m_pendingVerificationAfterLoopEnd = false;
}


std::string GoalController::TurnInterruptionMessage(AgentEndReason reason) const
{
    switch (reason) {
    case AgentEndReason::Cancelled:
        return "Goal turn stopped by user before verification. Goal remains active.";
    case AgentEndReason::IterationCap:
        return "Goal turn stopped at the agent tool-step safety cap before verification. Goal remains active.";
    case AgentEndReason::MalformedCap:
        return "Goal turn stopped because malformed tool calls hit the safety cap before verification. Goal remains active.";
    case AgentEndReason::StreamError:
        return "Goal turn stopped because the assistant stream failed before verification. Goal remains active.";
    case AgentEndReason::SendFailed:
        return "Goal turn stopped because the next agent request could not be sent before verification. Goal remains active.";
    case AgentEndReason::LoopGuard:
        return "Goal turn stopped because the loop guard blocked a repeated tool call before verification. Goal remains active.";
    case AgentEndReason::ToolFailedStop:
        return "Goal turn stopped because a tool failed before verification. Goal remains active.";
    case AgentEndReason::Normal:
        break;
    }
    return std::string();
}

void GoalController::DisplayGoalStatus()
{
    const GoalState& goal = m_chatHistory->GetGoalState();
    if (!goal.HasGoal()) {
        m_chatDisplay->DisplaySystemMessage(
            "No goal is set. Start one with 'Make this a goal: <objective>' or /goal <objective>.");
        return;
    }

    std::ostringstream body;
    body << "Goal status: " << GoalStatusLabel(goal.status) << "\n"
         << "Objective: " << goal.objective << "\n"
         << "Contract: " << GoalContractStatusLabel(goal.contract.status);

    if (goal.contract.IsReady()) {
        if (!goal.contract.successCriteria.empty()) {
            body << "\nSuccess criteria:";
            for (const auto& item : goal.contract.successCriteria)
                body << "\n- " << item;
        }
        if (!goal.contract.constraints.empty()) {
            body << "\nConstraints:";
            for (const auto& item : goal.contract.constraints)
                body << "\n- " << item;
        }
        if (!goal.contract.evidenceChecks.empty()) {
            body << "\nEvidence checks:";
            for (const auto& item : goal.contract.evidenceChecks)
                body << "\n- " << item;
        }
    }
    else if (!goal.contract.lastBuilderReason.empty()) {
        body << "\nContract note: " << goal.contract.lastBuilderReason;
    }

    if (goal.turnsUsed > 0 || goal.IsBudgetReached()) {
        body << "\nAutomatic continuations used: "
             << goal.turnsUsed << "/" << kMaxAutoContinuations;
    }
    if (!goal.lastVerifierReason.empty()) {
        body << "\nLast verifier: " << goal.lastVerifierReason;
    }
    if (!goal.lastInterruptionReason.empty()) {
        body << "\nLast interruption: " << goal.lastInterruptionReason;
    }
    if (goal.IsAwaitingUser() && !goal.awaitingUserReason.empty()) {
        body << "\nAwaiting user: " << goal.awaitingUserReason;
    }
    if (!goal.structuredAgentEvidence.empty()) {
        body << "\nStructured evidence events: "
             << goal.structuredAgentEvidence.size();
    }

    m_chatDisplay->DisplaySystemMessage(body.str());
}

void GoalController::HandleSlashGoal(const std::string& arg)
{
    std::string text = arg;
    {
        size_t a = text.find_first_not_of(" \t\r\n");
        size_t b = text.find_last_not_of(" \t\r\n");
        text = (a == std::string::npos) ? std::string()
                                         : text.substr(a, b - a + 1);
    }

    const std::string command = LbLowerAscii(text);

    // Goal control commands must be sent by themselves. Without this
    // guard, a pasted multi-line message such as:
    //
    //   /goal resume
    //   Begin working on the goal.
    //
    // falls through as a brand-new goal objective whose literal text is
    // "resume\nBegin working on the goal."  That silently corrupts the
    // existing goal state.  Reject that shape explicitly and ask the user
    // to send the follow-up instruction as a separate message.
    {
        const size_t newlinePos = text.find_first_of("\r\n");
        if (newlinePos != std::string::npos) {
            std::string firstLine = text.substr(0, newlinePos);
            const size_t firstA = firstLine.find_first_not_of(" \t\r\n");
            const size_t firstB = firstLine.find_last_not_of(" \t\r\n");
            firstLine = (firstA == std::string::npos)
                ? std::string()
                : firstLine.substr(firstA, firstB - firstA + 1);

            std::string trailing = text.substr(newlinePos + 1);
            const size_t trailingA = trailing.find_first_not_of(" \t\r\n");
            const bool hasTrailingContent = trailingA != std::string::npos;

            const std::string firstCommand = LbLowerAscii(firstLine);
            const bool isStandaloneGoalControl =
                firstCommand == "status" ||
                firstCommand == "pause" ||
                firstCommand == "resume" ||
                firstCommand == "clear" ||
                firstCommand == "verify" ||
                firstCommand == "continue" ||
                firstCommand == "rebuild" ||
                firstCommand == "rebuild contract" ||
                firstCommand == "contract rebuild";

            if (isStandaloneGoalControl && hasTrailingContent) {
                m_chatDisplay->DisplaySystemMessage(
                    "/goal " + firstLine +
                    " must be sent by itself. Send the next instruction as a separate message.");
                return;
            }
        }
    }

    if (text.empty() || command == "status") {
        DisplayGoalStatus();
        return;
    }

    if (command == "pause") {
        if (!m_chatHistory->HasGoal()) {
            m_chatDisplay->DisplaySystemMessage(
                "No goal is set. Start one with 'Make this a goal: <objective>' or /goal <objective>.");
        } else if (m_chatHistory->HasPausedGoal()) {
            m_chatDisplay->DisplaySystemMessage("Goal is already paused.");
        } else {
            m_chatHistory->PauseGoal();
            m_cb.refreshGoalStatusStrip();
            m_chatDisplay->DisplaySystemMessage("Goal paused.");
            m_convController.AutoSaveConversation();
        }
        return;
    }

    if (command == "resume") {
        if (!m_chatHistory->HasGoal()) {
            m_chatDisplay->DisplaySystemMessage(
                "No goal is set. Start one with 'Make this a goal: <objective>' or /goal <objective>.");
        } else if (m_chatHistory->HasActiveGoal()) {
            m_chatDisplay->DisplaySystemMessage("Goal is already active.");
        } else {
            m_chatHistory->ResumeGoal();
            m_cb.refreshGoalStatusStrip();
            m_chatDisplay->DisplaySystemMessage("Goal resumed.");
            m_convController.AutoSaveConversation();
        }
        return;
    }

    if (command == "clear") {
        if (!m_chatHistory->HasGoal()) {
            m_chatDisplay->DisplaySystemMessage("No goal is set.");
        } else {
            m_chatHistory->ClearGoal();
            m_cb.refreshGoalStatusStrip();
            m_chatDisplay->DisplaySystemMessage("Goal cleared.");
            m_convController.AutoSaveConversation();
        }
        return;
    }

    if (command == "verify") {
        if (!m_chatHistory->HasActiveGoal()) {
            m_chatDisplay->DisplaySystemMessage(
                "No active goal is available to verify. Start a goal or resume the existing one first.");
        } else if (m_cb.isBusy()) {
            m_chatDisplay->DisplaySystemMessage(
                "Goal verification cannot start while LlamaBoss is busy.");
        } else {
            BeginVerificationIfNeeded(true);
        }
        return;
    }

    if (command == "continue") {
        if (!m_chatHistory->HasActiveGoal() &&
            !m_chatHistory->HasAwaitingUserGoal()) {
            m_chatDisplay->DisplaySystemMessage(
                "No active goal is available to continue. Start a goal or resume the existing one first.");
        } else if (!m_cb.isAgentModeEnabled()) {
            m_chatDisplay->DisplaySystemMessage(
                "Goal continuation requires Agent mode.");
        } else if (m_cb.isBusy()) {
            m_chatDisplay->DisplaySystemMessage(
                "Goal continuation cannot start while LlamaBoss is busy.");
        } else {
            const bool wasAwaitingUser = m_chatHistory->HasAwaitingUserGoal();
            if (wasAwaitingUser) {
                m_chatHistory->ResumeGoal();
                m_cb.refreshGoalStatusStrip();
                m_convController.AutoSaveConversation();
                m_chatDisplay->DisplaySystemMessage(
                    "Resuming the goal that was waiting for your input.");
            } else {
                m_chatDisplay->DisplaySystemMessage(
                    "Continuing the active goal.");
            }

            BeginContinuationTurn(
                wasAwaitingUser
                    ? "The user provided input and resumed a waiting goal."
                    : "The user requested goal continuation.");
        }
        return;
    }

    if (command == "rebuild" || command == "rebuild contract" || command == "contract rebuild") {
        if (!m_chatHistory->HasActiveGoal()) {
            m_chatDisplay->DisplaySystemMessage(
                "No active goal is available for contract rebuilding. Start a goal or resume the existing one first.");
        } else if (m_cb.isBusy()) {
            m_chatDisplay->DisplaySystemMessage(
                "Goal contract rebuilding cannot start while LlamaBoss is busy.");
        } else {
            m_chatHistory->BeginGoalContractDrafting();
            m_autoStartAfterContractBuild = false;
            m_chatDisplay->DisplaySystemMessage("Goal contract rebuild requested.");
            m_convController.AutoSaveConversation();
            m_cb.callAfter([this]() {
                BeginContractBuildIfNeeded();
            });
        }
        return;
    }

    m_chatHistory->StartGoal(text);
    m_cb.refreshGoalStatusStrip();
    m_autoStartAfterContractBuild = true;
    m_convController.AutoSaveConversation();
    m_chatDisplay->DisplaySystemMessage(
        "Goal started. You can say 'show goal status', 'pause the goal', 'resume the goal', 'continue the goal', 'verify the goal', 'rebuild the goal contract', or 'clear the goal'.");

    m_cb.callAfter([this]() {
        BeginContractBuildIfNeeded();
    });
}

const char* GoalController::StructuredEvidenceEventLabel(AgentEventType type) const
{
    switch (type) {
    case AgentEventType::LoopBegin:        return "LoopBegin";
    case AgentEventType::IterationBegin:   return "IterationBegin";
    case AgentEventType::ToolCall:         return "ToolCall";
    case AgentEventType::ToolOutput:       return "ToolOutput";
    case AgentEventType::ApprovalRequired: return "ApprovalRequired";
    case AgentEventType::AgentStatus:      return "AgentStatus";
    case AgentEventType::Error:            return "Error";
    case AgentEventType::TurnComplete:     return "TurnComplete";
    case AgentEventType::FileCreated:      return "FileCreated";
    case AgentEventType::EditApplied:      return "EditApplied";
    case AgentEventType::DirectoryCreated: return "DirectoryCreated";
    case AgentEventType::FileDeleted:      return "FileDeleted";
    case AgentEventType::LoopEnd:          return "LoopEnd";
    }
    return "Unknown";
}

std::string GoalController::StructuredEvidenceChips(const ToolBlock& block) const
{
    if (block.statusChips.empty()) return std::string();

    std::ostringstream chips;
    for (size_t i = 0; i < block.statusChips.size(); ++i) {
        if (i) chips << ", ";
        chips << block.statusChips[i];
    }
    return chips.str();
}

void GoalController::AppendStructuredAgentEvidence(std::string chunk)
{
    chunk = LbTrimAscii(std::move(chunk));
    if (chunk.empty()) return;

    m_chatHistory->AppendGoalStructuredAgentEvidence(chunk);
}

void GoalController::RecordStructuredAgentEvidence(const AgentEvent& event)
{
    if (!m_chatHistory->HasActiveGoal()) return;

    // Iteration bookkeeping is useful for orchestration but adds little
    // completion proof.  Keep the evidence packet focused on work facts.
    if (event.type == AgentEventType::LoopBegin ||
        event.type == AgentEventType::IterationBegin) {
        return;
    }

    std::ostringstream evidence;
    evidence << "- Event: " << StructuredEvidenceEventLabel(event.type) << "\n";

    switch (event.type) {
    case AgentEventType::ToolCall:
        if (!event.toolName.empty())
            evidence << "  Tool: " << event.toolName << "\n";
        if (!event.commandEcho.empty())
            evidence << "  Command: " << event.commandEcho << "\n";
        if (!event.toolCallId.empty())
            evidence << "  Tool call id: " << event.toolCallId << "\n";
        break;

    case AgentEventType::ToolOutput:
    case AgentEventType::ApprovalRequired:
    case AgentEventType::AgentStatus:
    case AgentEventType::Error:
    case AgentEventType::FileCreated:
    case AgentEventType::EditApplied:
    case AgentEventType::DirectoryCreated:
    case AgentEventType::FileDeleted:
    {
        const ToolBlock& block = event.toolBlock;

        if (!block.toolName.empty())
            evidence << "  Tool: " << block.toolName << "\n";
        if (!block.commandEcho.empty())
            evidence << "  Command: " << block.commandEcho << "\n";

        const std::string chips = StructuredEvidenceChips(block);
        if (!chips.empty())
            evidence << "  Tool result metadata (not file content): "
                     << chips << "\n";

        if (!block.presentedFiles.empty()) {
            for (const auto& file : block.presentedFiles) {
                evidence << "  Presented artifact: "
                         << (file.displayName.empty()
                                 ? std::string("(unnamed)")
                                 : file.displayName);
                if (!file.diskPath.empty())
                    evidence << " | disk path: " << file.diskPath;
                if (file.sizeBytes > 0)
                    evidence << " | bytes: " << file.sizeBytes;
                if (file.lineCount > 0)
                    evidence << " | lines: " << file.lineCount;
                evidence << "\n";
            }
        }

        const std::string body = LbTrimAscii(block.body);
        if (!body.empty()) {
            evidence << "  Body excerpt:\n"
                     << LbClipForGoalVerifier(body, 1800) << "\n";
        }

        const std::string error = LbTrimAscii(block.errorBody);
        if (!error.empty()) {
            evidence << "  Error excerpt:\n"
                     << LbClipForGoalVerifier(error, 900) << "\n";
        }
        break;
    }

    case AgentEventType::TurnComplete:
        if (!event.userFacingMessage.empty()) {
            evidence << "  Final deterministic assistant message:\n"
                     << LbClipForGoalVerifier(
                            LbTrimAscii(event.userFacingMessage), 1200)
                     << "\n";
        }
        break;

    case AgentEventType::LoopEnd:
        evidence << "  End reason: ";
        switch (event.endReason) {
        case AgentEndReason::Normal:        evidence << "normal"; break;
        case AgentEndReason::Cancelled:     evidence << "cancelled"; break;
        case AgentEndReason::IterationCap:  evidence << "tool_step_cap"; break;
        case AgentEndReason::MalformedCap:  evidence << "malformed_tool_cap"; break;
        case AgentEndReason::StreamError:   evidence << "stream_error"; break;
        case AgentEndReason::SendFailed:    evidence << "send_failed"; break;
        case AgentEndReason::LoopGuard:     evidence << "loop_guard"; break;
        case AgentEndReason::ToolFailedStop:evidence << "tool_failed_stop"; break;
        }
        evidence << "\n";
        if (!event.userFacingMessage.empty()) {
            evidence << "  Loop-end message: "
                     << LbClipForGoalVerifier(
                            LbTrimAscii(event.userFacingMessage), 800)
                     << "\n";
        }
        break;

    case AgentEventType::LoopBegin:
    case AgentEventType::IterationBegin:
        break;
    }

    AppendStructuredAgentEvidence(evidence.str());
}

std::string GoalController::BuildStructuredAgentEvidence() const
{
    const auto& structuredEvidence =
        m_chatHistory->GetGoalStructuredAgentEvidence();

    if (structuredEvidence.empty()) {
        return "(No structured AgentEvent evidence has been recorded for this saved goal.)";
    }

    constexpr size_t kMaxTotalBytes = 18000;
    std::ostringstream out;
    size_t totalBytes = 0;

    for (const auto& chunk : structuredEvidence) {
        std::string clipped = LbClipForGoalVerifier(chunk, 2600);
        if (totalBytes + clipped.size() + 2 > kMaxTotalBytes) {
            out << "[structured AgentEvent evidence truncated]\n";
            break;
        }
        out << clipped << "\n\n";
        totalBytes += clipped.size() + 2;
    }

    const std::string text = out.str();
    return text.empty()
        ? std::string("(No structured AgentEvent evidence has been recorded for this saved goal.)")
        : text;
}

std::string GoalController::LatestAssistantReplyForAwaitUserFallback() const
{
    const auto& messages = m_chatHistory->GetMessages();
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        const auto& msg = *it;
        if (!msg) continue;

        std::string role;
        std::string content;
        try { role = msg->getValue<std::string>("role"); }
        catch (...) { role.clear(); }
        if (role != "assistant") continue;

        try { content = msg->getValue<std::string>("content"); }
        catch (...) { content.clear(); }

        content = LbTrimAscii(content);
        if (!content.empty())
            return content;
    }
    return std::string();
}

bool GoalController::LatestAssistantReplyLooksLikeBlockingUserQuestion() const
{
    const std::string latest =
        LbLowerAscii(LatestAssistantReplyForAwaitUserFallback());
    if (latest.empty()) return false;

    const bool hasQuestionMark =
        latest.find('?') != std::string::npos;

    const bool asksChoiceOrDecision =
        latest.find("which option") != std::string::npos ||
        latest.find("which do you prefer") != std::string::npos ||
        latest.find("which would you prefer") != std::string::npos ||
        latest.find("what do you prefer") != std::string::npos ||
        latest.find("do you prefer") != std::string::npos ||
        latest.find("would you like") != std::string::npos ||
        latest.find("do you want") != std::string::npos ||
        latest.find("please choose") != std::string::npos ||
        latest.find("please let me know") != std::string::npos ||
        latest.find("please provide") != std::string::npos ||
        latest.find("could you please") != std::string::npos ||
        latest.find("can you confirm") != std::string::npos ||
        latest.find("need your choice") != std::string::npos ||
        latest.find("need your confirmation") != std::string::npos ||
        latest.find("need you to confirm") != std::string::npos;

    const bool explicitlyWaiting =
        latest.find("i am waiting for") != std::string::npos ||
        latest.find("i'll wait for") != std::string::npos ||
        latest.find("i will wait for") != std::string::npos ||
        latest.find("before i can continue") != std::string::npos ||
        latest.find("before creating") != std::string::npos ||
        latest.find("before proceeding") != std::string::npos ||
        latest.find("i am blocked until") != std::string::npos ||
        latest.find("i'm blocked until") != std::string::npos ||
        latest.find("i am still blocked") != std::string::npos ||
        latest.find("i'm still blocked") != std::string::npos ||
        latest.find("until the working directory is changed") != std::string::npos ||
        latest.find("once the directory is set correctly") != std::string::npos;

    const bool reportsWritableRootBlock =
        latest.find("outside of my allowed writable workspace") != std::string::npos ||
        latest.find("outside of my current writable workspace") != std::string::npos ||
        latest.find("outside the allowed write roots") != std::string::npos ||
        latest.find("outside of the allowed write roots") != std::string::npos ||
        latest.find("file system restriction") != std::string::npos ||
        latest.find("refuses to edit outside") != std::string::npos;

    const bool requestsUserUnblockAction =
        latest.find("please run /cd") != std::string::npos ||
        latest.find("run /cd") != std::string::npos ||
        latest.find("change the working directory") != std::string::npos ||
        latest.find("change the directory") != std::string::npos ||
        latest.find("ensure the source directory is within") != std::string::npos;

    // Keep this fallback deliberately conservative.  The verifier's model
    // verdict remains primary; this only rescues obvious cases where the
    // assistant is plainly waiting on the user, but the verifier returned
    // CONTINUE instead of AWAIT_USER.
    return (hasQuestionMark && asksChoiceOrDecision) ||
           (hasQuestionMark && explicitlyWaiting) ||
           (asksChoiceOrDecision && explicitlyWaiting) ||
           (reportsWritableRootBlock &&
            (explicitlyWaiting || requestsUserUnblockAction));
}

std::string GoalController::BuildVerifierRecentEvidence() const
{
    std::ostringstream evidence;
    const auto& messages = m_chatHistory->GetMessages();

    if (messages.empty()) {
        return "(No recent transcript evidence.)";
    }

    constexpr size_t kMaxMessages = 8;
    constexpr size_t kMaxDefaultMessageBytes = 1400;
    constexpr size_t kMaxToolResultMessageBytes = 3600;
    constexpr size_t kMaxTotalBytes = 12000;

    const size_t start = messages.size() > kMaxMessages
        ? messages.size() - kMaxMessages
        : 0;

    size_t totalBytes = 0;
    for (size_t i = start; i < messages.size(); ++i) {
        const auto& msg = messages[i];
        if (!msg) continue;

        std::string role;
        std::string content;
        try { role = msg->getValue<std::string>("role"); }
        catch (...) { role = "unknown"; }
        try { content = msg->getValue<std::string>("content"); }
        catch (...) { content.clear(); }

        content = LbTrimAscii(content);
        if (content.empty()) continue;

        const bool looksLikeToolResult =
            content.compare(0, 7, "[tool: ") == 0;
        const size_t clipBudget = looksLikeToolResult
            ? kMaxToolResultMessageBytes
            : kMaxDefaultMessageBytes;
        content = LbClipForGoalVerifier(content, clipBudget);

        std::ostringstream block;
        block << "[" << role << "]\n"
              << content << "\n\n";
        const std::string chunk = block.str();

        if (totalBytes + chunk.size() > kMaxTotalBytes) {
            evidence << "[evidence truncated]\n";
            break;
        }

        evidence << chunk;
        totalBytes += chunk.size();
    }

    const std::string out = evidence.str();
    return out.empty() ? std::string("(No recent transcript evidence.)") : out;
}

std::string GoalController::BuildProjectContextBlock(const char* purposeLabel) const
{
    if (!m_chatHistory->HasProject()) return std::string();

    const std::string projectRoot = m_chatHistory->GetProjectRoot();
    const std::string projectMdPath =
        ProjectManager::ProjectInstructionsPath(projectRoot);

    std::string projectInstructions;
    std::string projectInstructionsStatus;
    const bool loadedProjectInstructions =
        ProjectManager::ReadProjectInstructions(
            projectRoot,
            projectInstructions,
            projectInstructionsStatus,
            6000);

    std::ostringstream p;
    p << kGoalPromptProjectContextHeader;
    if (purposeLabel && *purposeLabel)
        p << " FOR " << purposeLabel;
    p << ":\n"
      << "Project name: " << m_chatHistory->GetProjectName() << "\n"
      << "Project root: " << projectRoot << "\n"
      << "PROJECT.md: " << projectMdPath << "\n"
      << "project.json: " << ProjectManager::ProjectJsonPath(projectRoot) << "\n"
      << "Standard project lanes: Inputs\\, Outputs\\, Workflows\\, Notes\\, Sources\\, Templates\\, PROJECT.md, project.json.\n"
      << "Use this project context only when the active Goal is project-related. Do not force unrelated Goals into project deliverables or invent project requirements.\n";

    const auto projectSources =
        ProjectManager::ListProjectSources(projectRoot, 12);
    if (!projectSources.empty()) {
        p << "Project source files available in Sources/:\n";
        for (const auto& src : projectSources) {
            p << "- " << src.name
              << " (" << ProjectSource_HumanBytes(src.sizeBytes) << ")\n"
              << "  " << src.path << "\n";
        }
    }

    const auto projectWorkflows =
        ProjectManager::ListProjectWorkflows(projectRoot, 12);
    if (!projectWorkflows.empty()) {
        p << "Project workflow files available in Workflows/:\n";
        for (const auto& wf : projectWorkflows) {
            p << "- " << wf.name
              << " (" << ProjectSource_HumanBytes(wf.sizeBytes) << ")\n"
              << "  " << wf.path << "\n";
        }
    }

    const auto projectWorkflowScripts =
        ProjectManager::ListProjectWorkflowScripts(projectRoot, 12);
    if (!projectWorkflowScripts.empty()) {
        p << "Project workflow helper scripts available in Workflows/:\n";
        for (const auto& script : projectWorkflowScripts) {
            p << "- " << script.name
              << " (" << ProjectSource_HumanBytes(script.sizeBytes) << ")\n"
              << "  " << script.path << "\n";
        }
    }

    if (loadedProjectInstructions) {
        p << "Project contract loaded from PROJECT.md:\n"
          << "--- PROJECT.md START ---\n"
          << projectInstructions;
        if (!projectInstructions.empty() &&
            projectInstructions.back() != '\n') {
            p << "\n";
        }
        p << "--- PROJECT.md END ---\n";

        if (!projectInstructionsStatus.empty()) {
            p << "Project contract note: "
              << projectInstructionsStatus << "\n";
        }
    } else {
        p << "PROJECT.md was not loaded";
        if (!projectInstructionsStatus.empty()) {
            p << ": " << projectInstructionsStatus;
        }
        p << "\n";
    }

    return p.str();
}

std::string GoalController::BuildSkillContextBlock(const char* purposeLabel) const
{
    const std::string listedSkills = m_cb.skillsBlock();
    if (listedSkills.empty()) return std::string();

    std::ostringstream p;
    p << kGoalPromptSkillContextHeader;
    if (purposeLabel && *purposeLabel)
        p << " FOR " << purposeLabel;
    p << ":\n"
      << "Skills are cross-project reusable abilities. Use this Skill context only when the active Goal explicitly names a Skill or clearly requires an outcome that matches a listed Skill. Do not force unrelated Goals into Skill use or invent Skill requirements.\n"
      << listedSkills;

    return p.str();
}

GoalContractPromptInput GoalController::BuildContractPromptInput() const
{
    const GoalState& goal = m_chatHistory->GetGoalState();

    GoalContractPromptInput input;
    input.skillContext = BuildSkillContextBlock(kGoalPromptPurposeContractDrafting);
    input.projectContext = BuildProjectContextBlock(kGoalPromptPurposeContractDrafting);
    input.objective = goal.objective;
    return input;
}

void GoalController::BeginContractBuildIfNeeded()
{
    if (m_cb.isClosing()) return;
    if (!m_chatHistory->HasActiveGoal()) return;
    if (m_contractBuilderInFlight) return;
    if (m_chatHistory->GoalContractReady()) return;
    if (m_cb.isBusy()) return;

    if (!m_modelSwitcher.IsServerReady()) {
        m_autoStartAfterContractBuild = false;
        m_chatHistory->MarkGoalContractFailed(
            "Contract drafting was skipped because the model server was not ready.");
        m_convController.AutoSaveConversation();
        m_chatDisplay->DisplaySystemMessage(
            "Goal contract drafting was skipped because the model server is not ready. "
            "The goal remains active with objective-only verification.");
        return;
    }

    // Conversation-pinned, not app-global: a model switch in another
    // window must not retarget an in-flight goal send.
    const std::string model = m_modelSwitcher.GetConversationModelForSave();
    if (model.empty()) {
        m_autoStartAfterContractBuild = false;
        m_chatHistory->MarkGoalContractFailed(
            "Contract drafting was skipped because no model is selected.");
        m_convController.AutoSaveConversation();
        m_chatDisplay->DisplaySystemMessage(
            "Goal contract drafting was skipped because no model is selected. "
            "The goal remains active with objective-only verification.");
        return;
    }

    ChatHistory contractHistory;
    contractHistory.AddUserMessage(BuildGoalContractBuilderUserPrompt(BuildContractPromptInput()));

    int ctxTokens = m_appState.GetCtxSize();
    if (ctxTokens <= 0) ctxTokens = 8192;

    std::string body = contractHistory.BuildChatRequestJson(
        model,
        true,
        BuildGoalContractBuilderSystemPrompt(),
        ctxTokens,
        "",
        false,
        true);

    m_cb.discardPendingAssistantDelta();
    m_chatHistory->BeginGoalContractDrafting();
    m_contractBuilderInFlight = true;

    // KV fast path: this generation runs against the local slot with a
    // throwaway history, so the slot will no longer hold the active
    // conversation's state.  Forget ownership now so a later
    // switch-away doesn't serialize contract-builder KV under the
    // conversation's filename.  Harmless if SendMessage fails below —
    // worst case is one skipped save.
    m_modelSwitcher.InvalidateKvSlotOwner();

    const unsigned long genId = m_cb.bumpGenerationId();
    m_cb.setChatStateStreaming();
    m_cb.setStreamingUi(true);

    m_chatDisplay->DisplaySystemMessage(
        "Goal contract builder is drafting success criteria.");

    if (!m_chatClient.SendMessage(
            // Pinned: honor this conversation's preferred model even
            // if another window flipped the app-global target.
            m_modelSwitcher.ResolveTargetForConversation(),
            body, genId)) {
        m_contractBuilderInFlight = false;
        m_autoStartAfterContractBuild = false;
        m_cb.setStreamingUi(false);
        m_chatHistory->MarkGoalContractFailed(
            "Failed to start the contract drafting request.");
        m_convController.AutoSaveConversation();
        m_chatDisplay->DisplaySystemMessage(
            "Failed to start goal contract drafting. "
            "The goal remains active with objective-only verification.");
    }
}

void GoalController::HandleContractBuilderComplete(const std::string& builderResponse)
{
    const bool autoStartAfterContractBuild = m_autoStartAfterContractBuild;
    m_autoStartAfterContractBuild = false;
    m_contractBuilderInFlight = false;
    m_chatClient.ResetStreamingState();
    m_cb.discardPendingAssistantDelta();
    m_cb.setStreamingUi(false);

    if (!m_chatHistory->HasGoal()) {
        return;
    }

    GoalContractDraft draft = ParseGoalContractDraft(builderResponse);
    if (!draft.Parsed()) {
        m_chatHistory->MarkGoalContractFailed(
            "Contract builder did not return usable SUCCESS lines.");
        m_convController.AutoSaveConversation();
        const std::string statusText =
            "Goal contract builder could not draft a structured contract. "
            "The goal remains active with objective-only verification.";
        m_cb.callAfter([this, statusText]() {
            m_chatDisplay->DisplaySystemMessage(statusText);
        });
        return;
    }

    std::string reason = LbTrimAscii(draft.reason);
    if (reason.empty())
        reason = "Structured contract drafted from the goal.";

    const size_t successCount = draft.successCriteria.size();
    const size_t constraintCount = draft.constraints.size();
    const size_t evidenceCount = draft.evidenceChecks.size();

    m_chatHistory->SetGoalContractReady(
        draft.successCriteria,
        draft.constraints,
        draft.evidenceChecks,
        reason);
    m_convController.AutoSaveConversation();

    std::ostringstream msg;
    msg << "Goal contract ready: "
        << successCount << " success "
        << (successCount == 1 ? "criterion" : "criteria")
        << ", " << constraintCount << " "
        << (constraintCount == 1 ? "constraint" : "constraints")
        << ", and " << evidenceCount << " evidence "
        << (evidenceCount == 1 ? "check" : "checks")
        << ". Say 'show goal status' to review it.";

    const std::string statusText = msg.str();
    m_cb.callAfter([this, statusText, autoStartAfterContractBuild]() {
        m_chatDisplay->DisplaySystemMessage(statusText);

        if (!autoStartAfterContractBuild ||
            !m_chatHistory->HasActiveGoal()) {
            return;
        }

        if (!m_cb.isAgentModeEnabled()) {
            m_chatDisplay->DisplaySystemMessage(
                "Goal contract is ready. Agent mode is off, so automatic goal start was skipped. "
                "Turn on Agent mode and say 'continue the goal'.");
            return;
        }

        if (m_cb.isBusy()) {
            m_chatDisplay->DisplaySystemMessage(
                "Goal contract is ready, but LlamaBoss is busy. Say 'continue the goal' when ready.");
            return;
        }

        m_chatDisplay->DisplaySystemMessage(
            "Goal contract is ready. Starting work on the active goal.");
        BeginContinuationTurn(
            "The goal contract is ready. Begin work on the active goal.");
    });
}

void GoalController::HandleContractBuilderError(const std::string& error)
{
    m_contractBuilderInFlight = false;
    m_autoStartAfterContractBuild = false;
    m_cb.discardPendingAssistantDelta();
    m_chatClient.ResetStreamingState();
    m_cb.setStreamingUi(false);

    if (m_chatHistory->HasGoal()) {
        m_chatHistory->MarkGoalContractFailed(
            "Goal contract drafting request failed.");
        m_convController.AutoSaveConversation();
    }

    std::string msg =
        "Goal contract drafting failed. "
        "The goal remains active with objective-only verification.";
    const std::string trimmed = LbTrimAscii(error);
    if (!trimmed.empty())
        msg += " " + LbClipForGoalVerifier(trimmed, 240);

    m_chatDisplay->DisplaySystemMessage(msg);
}

GoalVerifierPromptInput GoalController::BuildVerifierPromptInput() const
{
    const GoalState& goal = m_chatHistory->GetGoalState();

    GoalVerifierPromptInput input;
    input.skillContext = BuildSkillContextBlock(kGoalPromptPurposeVerification);
    input.projectContext = BuildProjectContextBlock(kGoalPromptPurposeVerification);
    input.objective = goal.objective;
    input.hasReadyContract = goal.contract.IsReady();
    input.successCriteria = goal.contract.successCriteria;
    input.constraints = goal.contract.constraints;
    input.evidenceChecks = goal.contract.evidenceChecks;
    input.awaitingUserPromptEvidence = goal.awaitingUserPromptEvidence;
    input.awaitingUserReplyEvidence = goal.awaitingUserReplyEvidence;
    input.structuredAgentEvidence = BuildStructuredAgentEvidence();
    input.recentTranscriptEvidence = BuildVerifierRecentEvidence();
    return input;
}

void GoalController::BeginVerificationIfNeeded(bool manualOnly)
{
    if (m_cb.isClosing() || m_verifierInFlight) return;
    if (!m_chatHistory->HasActiveGoal()) return;
    if (!manualOnly && !m_cb.isAgentModeEnabled()) return;
    if (m_cb.isBusy()) return;

    if (!m_modelSwitcher.IsServerReady()) {
        m_chatDisplay->DisplaySystemMessage(
            "Goal verifier could not run because the model server is not ready. "
            "The goal remains active.");
        return;
    }

    // Conversation-pinned (see the contract send above).
    const std::string model = m_modelSwitcher.GetConversationModelForSave();
    if (model.empty()) {
        m_chatDisplay->DisplaySystemMessage(
            "Goal verifier could not run because no model is selected. "
            "The goal remains active.");
        return;
    }

    ChatHistory verifierHistory;
    verifierHistory.AddUserMessage(BuildGoalVerifierUserPrompt(BuildVerifierPromptInput()));

    int ctxTokens = m_appState.GetCtxSize();
    if (ctxTokens <= 0) ctxTokens = 8192;

    std::string body = verifierHistory.BuildChatRequestJson(
        model,
        true,
        BuildGoalVerifierSystemPrompt(),
        ctxTokens,
        "",
        false,
        true);

    m_cb.discardPendingAssistantDelta();
    m_verifierManualOnly = manualOnly;
    m_verifierInFlight = true;

    // KV fast path: throwaway-history generation — same reasoning as
    // the contract builder dispatch above.
    m_modelSwitcher.InvalidateKvSlotOwner();

    const unsigned long genId = m_cb.bumpGenerationId();
    m_cb.setChatStateStreaming();
    m_cb.setStreamingUi(true);

    m_chatDisplay->DisplaySystemMessage(
        "Goal verifier is checking whether the goal is complete.");

    if (!m_chatClient.SendMessage(
            // Pinned: honor this conversation's preferred model even
            // if another window flipped the app-global target.
            m_modelSwitcher.ResolveTargetForConversation(),
            body, genId)) {
        m_verifierInFlight = false;
        m_verifierManualOnly = false;
        m_cb.setStreamingUi(false);
        m_chatDisplay->DisplaySystemMessage(
            "Failed to start goal verification. The goal remains active.");
    }
}

void GoalController::HandleVerifierComplete(const std::string& verifierResponse)
{
    const bool manualOnly = m_verifierManualOnly;
    m_verifierManualOnly = false;
    m_verifierInFlight = false;
    m_chatClient.ResetStreamingState();
    m_cb.discardPendingAssistantDelta();
    m_cb.setStreamingUi(false);

    if (!m_chatHistory->HasActiveGoal()) {
        return;
    }

    GoalVerifierVerdict verdict = ParseGoalVerifierVerdict(verifierResponse);
    if (!verdict.Parsed()) {
        m_chatHistory->NoteGoalVerifierUnclear(
            "Verifier did not return a clear COMPLETE, CONTINUE, or AWAIT_USER verdict.");
        m_convController.AutoSaveConversation();

        // Defer the verifier-result status card until after the stream-
        // completion event fully unwinds. In testing, synchronous status
        // messages from this hidden verifier completion path could be
        // skipped visually even though goal state updated correctly.
        const std::string statusText =
            "Goal verifier could not return a clear COMPLETE, CONTINUE, or AWAIT_USER verdict. "
            "The goal remains active.";
        m_cb.callAfter([this, statusText]() {
            m_chatDisplay->DisplaySystemMessage(statusText);
        });
        return;
    }

    std::string reason = LbTrimAscii(verdict.reason);
    if (reason.empty()) {
        reason = verdict.IsComplete()
            ? "The verifier judged the goal satisfied."
            : "The verifier judged the goal not yet complete.";
    }

    if (verdict.IsComplete()) {
        m_chatHistory->MarkGoalVerifiedComplete(reason);
        m_cb.refreshGoalStatusStrip();
        m_convController.AutoSaveConversation();

        const std::string statusText =
            "Goal verified complete. " + reason;
        m_cb.callAfter([this, statusText]() {
            m_chatDisplay->DisplaySystemMessage(statusText);
        });
        return;
    }

    if (verdict.IsAwaitUser() ||
        LatestAssistantReplyLooksLikeBlockingUserQuestion()) {
        m_chatHistory->MarkGoalAwaitingUser(
            reason,
            LbClipForGoalVerifier(
                LatestAssistantReplyForAwaitUserFallback(), 1800));
        m_cb.refreshGoalStatusStrip();
        m_convController.AutoSaveConversation();

        const std::string statusText =
            "Goal is waiting for your input. " + reason +
            " Reply in chat, then say 'continue the goal' to resume work.";
        m_cb.callAfter([this, statusText]() {
            m_chatDisplay->DisplaySystemMessage(statusText);
        });
        return;
    }

    m_chatHistory->NoteGoalVerifierContinue(reason);
    m_convController.AutoSaveConversation();

    if (manualOnly) {
        const std::string statusText =
            "Goal verifier: continue - " + reason;
        m_cb.callAfter([this, statusText]() {
            m_chatDisplay->DisplaySystemMessage(statusText);
        });
        return;
    }

    if (!m_chatHistory->CanGoalAutoContinue(kMaxAutoContinuations)) {
        m_chatHistory->MarkGoalBudgetReached(reason);
        m_cb.refreshGoalStatusStrip();
        m_convController.AutoSaveConversation();

        std::ostringstream msg;
        msg << "Goal verifier: continue - " << reason << "\n"
            << "Automatic continuation budget reached ("
            << kMaxAutoContinuations << "/"
            << kMaxAutoContinuations
            << "). Say 'resume the goal' to reopen this goal.";

        const std::string statusText = msg.str();
        m_cb.callAfter([this, statusText]() {
            m_chatDisplay->DisplaySystemMessage(statusText);
        });
        return;
    }

    m_chatHistory->ConsumeGoalAutoContinuation();
    m_convController.AutoSaveConversation();
    const int used = m_chatHistory->GetGoalState().turnsUsed;

    std::ostringstream msg;
    msg << "Goal verifier: continue - " << reason << "\n"
        << "Continuing automatically ("
        << used << "/" << kMaxAutoContinuations << ").";

    const std::string statusText = msg.str();
    m_cb.callAfter([this, statusText, reason]() {
        m_chatDisplay->DisplaySystemMessage(statusText);
        BeginContinuationTurn(reason);
    });
}

void GoalController::HandleVerifierError(const std::string& error)
{
    m_verifierInFlight = false;
    m_verifierManualOnly = false;
    m_cb.discardPendingAssistantDelta();
    m_chatClient.ResetStreamingState();
    m_cb.setStreamingUi(false);

    if (m_chatHistory->HasActiveGoal()) {
        m_chatHistory->NoteGoalVerifierUnclear(
            "Goal verification request failed.");
        m_convController.AutoSaveConversation();
    }

    std::string msg =
        "Goal verification failed. The goal remains active.";
    const std::string trimmed = LbTrimAscii(error);
    if (!trimmed.empty())
        msg += " " + LbClipForGoalVerifier(trimmed, 240);

    m_chatDisplay->DisplaySystemMessage(msg);
}

void GoalController::BeginContinuationTurn(const std::string& verifierReason)
{
    if (m_cb.isClosing()) return;
    if (!m_cb.isAgentModeEnabled() || !m_chatHistory->HasActiveGoal()) return;
    if (m_cb.isBusy()) return;

    if (!m_modelSwitcher.IsServerReady()) {
        m_chatDisplay->DisplaySystemMessage(
            "Automatic goal continuation could not start because the model server is not ready. "
            "The goal remains active.");
        return;
    }

    std::ostringstream continuation;
    continuation
        << "Goal continuation instruction:\n"
        << "The active goal is not yet complete.\n"
        << "Verifier reason: "
        << (LbTrimAscii(verifierReason).empty()
                ? std::string("The verifier requested more work.")
                : LbTrimAscii(verifierReason))
        << "\n"
        << "Continue with the next concrete step toward the active goal. "
        << "Use tools when useful. Do not claim the goal is complete unless "
        << "the requested outcome is actually satisfied.";

    m_chatHistory->AddSystemMessage(continuation.str());

    // Conversation-pinned (see the contract send above).
    const std::string model = m_modelSwitcher.GetConversationModelForSave();
    int ctxTokens = m_appState.GetCtxSize();
    if (ctxTokens <= 0) ctxTokens = 8192;

    std::string tools;
    const bool native = (m_cb.getActiveProtocol() == ToolProtocol::Native);
    if (native) {
        tools = m_cb.getCachedToolsArrayJson();
    }

    std::string body = m_chatHistory->BuildChatRequestJson(
        model,
        true,
        m_cb.buildAgentSystemPrompt(),
        ctxTokens,
        tools,
        native,
        true);

    m_chatHistory->AddAssistantPlaceholder(model);
    m_chatDisplay->DisplayAssistantPrefix(
        ServerManager::ModelDisplayName(model),
        m_appState.GetTheme().chatAssistant);

    if (!m_chatHistory->HasFilePath())
        m_chatHistory->SetFilePath(ChatHistory::GenerateFilePath());

    // KV fast path: this continuation extends the real conversation, so
    // the slot is about to hold its state again — stamp ownership,
    // mirroring the main send path in MyFrame.  Without this, a verifier
    // turn's InvalidateKvSlotOwner would leave the whole remainder of an
    // auto-continuing goal run unsaveable on switch-away.
    m_modelSwitcher.NoteKvSlotOwner(m_chatHistory->GetFilePath());
    {
        std::string genDir = ChatHistory::GetGeneratedFilesDir(
            m_chatHistory->GetFilePath());

        size_t msgIdx = m_chatHistory->GetMessageCount() > 0
            ? m_chatHistory->GetMessageCount() - 1
            : 0;

        m_chatDisplay->SetFilePersistenceContext(genDir, msgIdx);
    }

    m_cb.resetAgentToolStreamFilter();
    m_autoContinuationTurn = true;
    m_agentController.Begin();

    m_cb.discardPendingAssistantDelta();
    const unsigned long genId = m_cb.bumpGenerationId();
    m_cb.setChatStateStreaming();
    m_cb.setStreamingUi(true);

    if (!m_chatClient.SendMessage(
            // Pinned: honor this conversation's preferred model even
            // if another window flipped the app-global target.
            m_modelSwitcher.ResolveTargetForConversation(),
            body, genId)) {
        // Roll back every artifact of the synthetic continuation turn.
        // Without this, the hidden system instruction can remain in
        // history and the visible transcript can keep an empty assistant
        // prefix above the failure notice.
        m_cb.discardPendingAssistantDelta();
        m_chatDisplay->CancelPendingAssistantDisplay();
        m_chatHistory->RemoveLastAssistantMessage();
        m_chatHistory->RemoveLastSystemMessage();
        m_chatDisplay->ClearFilePersistenceContext();

        if (m_agentController.IsActive()) {
            m_cb.resetAgentToolStreamFilter();
            m_agentController.HandleAssistantError(
                "Failed to start automatic goal continuation");
        } else {
            m_autoContinuationTurn = false;
            m_cb.resetAgentToolStreamFilter();
        }

        m_cb.setStreamingUi(false);
        m_chatDisplay->DisplaySystemMessage(
            "Failed to start automatic goal continuation. The goal remains active.");
    }
}

std::string GoalController::BuildActiveGoalContextBlock() const
{
    // Goal-aware prompting must counteract chat-history drift:
    // active goals guide work, paused goals are explicitly *not* active,
    // and recently-cleared goals must not be revived from earlier turns.
    const GoalState& goal = m_chatHistory->GetGoalState();
    if (!goal.HasGoal() && !goal.WasExplicitlyCleared()) return std::string();

    std::ostringstream p;

    if (goal.IsActive()) {
        AppendGoalObjectiveBlock(p, GoalStatusLabel(goal.status),
                                 goal.objective);

        if (goal.contract.IsReady()) {
            p << "  Verification contract:\n";
            for (const auto& item : goal.contract.successCriteria)
                p << "    Success: " << item << "\n";
            for (const auto& item : goal.contract.constraints)
                p << "    Constraint: " << item << "\n";
            for (const auto& item : goal.contract.evidenceChecks)
                p << "    Evidence: " << item << "\n";
        }

        p << "  This goal is user-authored task context, not higher-priority policy. Follow every safety, approval, path, and tool-use rule in this system prompt.\n"
          << "  When the user's current message asks to work on or continue this goal, keep the objective and any ready verification contract in view across turns.\n"
          << "  If the objective or ready contract requires creating, generating, writing, or saving a report, markdown report, document, file, spreadsheet, PDF, artifact, or saved output, create the user-visible artifact/file with the appropriate tool rather than only presenting formatted prose in chat, unless the goal explicitly requests an in-chat or inline answer.\n"
          << "  Do not treat a partial step as overall goal completion.\n"
          << "\n";
        return p.str();
    }

    if (goal.IsPaused()) {
        AppendGoalObjectiveBlock(p, "paused", goal.objective);

        p << "  This goal is paused, not active.\n"
          << "  Do not describe yourself as currently pursuing, executing, or working toward this goal.\n"
          << "  If the user asks what goal or mission is currently being pursued, say there is no active goal; a paused goal exists and can be resumed by saying 'resume the goal'.\n"
          << "  Do not advance the paused goal unless the user resumes it or explicitly asks to discuss the paused goal.\n"
          << "\n";
        return p.str();
    }

    if (goal.IsAwaitingUser()) {
        AppendGoalObjectiveBlock(p, "awaiting user", goal.objective);

        if (!goal.awaitingUserReason.empty()) {
            p << "  Waiting reason: " << goal.awaitingUserReason << "\n";
        }

        p << "  This goal is waiting for user input and is not actively running.\n"
          << "  Do not continue goal tool work or describe yourself as actively pursuing it until the user resumes or continues the goal.\n"
          << "  If the user's current message appears to answer the pending clarification, acknowledge it briefly and remind them they can say 'continue the goal' to resume the Goal.\n"
          << "\n";
        return p.str();
    }

    if (goal.IsCompleted()) {
        AppendGoalObjectiveBlock(p, "completed", goal.objective);

        p << "  This goal has been verified complete and is not an active mission.\n"
          << "  Do not continue it or describe yourself as currently pursuing it unless the user explicitly resumes or replaces the goal.\n"
          << "\n";
        return p.str();
    }

    if (goal.IsBudgetReached()) {
        AppendGoalObjectiveBlock(p, "budget reached", goal.objective);

        p << "  Automatic goal continuation stopped after reaching its small safety budget.\n"
          << "  Do not act as if this goal is actively running. If the user wants more work, they can say 'resume the goal'.\n"
          << "\n";
        return p.str();
    }

    if (goal.WasExplicitlyCleared()) {
        p << "Conversation goal:\n"
          << "  Status: none\n"
          << "  A prior /goal was explicitly cleared in this conversation.\n"
          << "  There is currently no active or paused goal.\n"
          << "  Do not revive, continue, or describe earlier cleared goal text from chat history as the current mission.\n"
          << "  If the user asks what current goal or mission is being pursued, say no goal is set. The user can start one with 'Make this a goal: <objective>' or /goal <objective>.\n"
          << "  You may discuss the earlier goal only as past conversation history if the user explicitly asks about it as a past topic.\n"
          << "\n";
        return p.str();
    }

    return std::string();
}
