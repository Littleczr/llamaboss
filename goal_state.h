#pragma once
// goal_state.h
//
// Goals Phase 12 polish: durable awaiting-user prompt/reply checkpoints.
//
// Phase 12 added verifier-driven AwaitingUser pauses. This polish pass records
// the blocking assistant prompt and the user's later reply so the verifier can
// retain that proof after the waiting goal resumes and recent transcript
// windows roll forward.
//
// Phase 1 established goal state and prompt injection.
// Phase 2 added verifier verdicts plus a small automatic continuation budget.
// Phase 3 added a structured goal contract drafted by a hidden control turn.
// Phase 4 added manual verify/rebuild controls.
// Phase 5 persisted goals and contracts with saved conversations.
// Phase 6 guarded pasted multi-line /goal control commands.
// Phase 7 recorded pre-verifier interruptions.
// Phase 8 added /goal continue.
// Phase 9 auto-started fresh goals once the contract was ready.
// Phase 10 fed structured AgentEvent evidence into the verifier.
// Phase 11 persisted that compact structured evidence.
// Phase 12 adds an AwaitingUser goal status when the verifier determines the
// agent has asked a genuine blocking clarification or user decision question.

#include <string>
#include <utility>
#include <vector>

enum class GoalStatus {
    None,
    Active,
    Paused,
    AwaitingUser,
    Completed,
    Cancelled,
    Failed,
    BudgetReached
};

inline const char* GoalStatusLabel(GoalStatus status)
{
    switch (status) {
    case GoalStatus::None:          return "none";
    case GoalStatus::Active:        return "active";
    case GoalStatus::Paused:        return "paused";
    case GoalStatus::AwaitingUser:  return "awaiting user";
    case GoalStatus::Completed:     return "completed";
    case GoalStatus::Cancelled:     return "cancelled";
    case GoalStatus::Failed:        return "failed";
    case GoalStatus::BudgetReached: return "budget reached";
    }
    return "unknown";
}

enum class GoalContractStatus {
    None,
    Drafting,
    Ready,
    Failed
};

inline const char* GoalContractStatusLabel(GoalContractStatus status)
{
    switch (status) {
    case GoalContractStatus::None:     return "none";
    case GoalContractStatus::Drafting: return "drafting";
    case GoalContractStatus::Ready:    return "ready";
    case GoalContractStatus::Failed:   return "objective-only";
    }
    return "unknown";
}

struct GoalContract {
    GoalContractStatus status = GoalContractStatus::None;
    std::vector<std::string> successCriteria;
    std::vector<std::string> constraints;
    std::vector<std::string> evidenceChecks;
    std::string lastBuilderReason;

    bool IsDrafting() const
    {
        return status == GoalContractStatus::Drafting;
    }

    bool IsReady() const
    {
        return status == GoalContractStatus::Ready &&
               !successCriteria.empty();
    }

    bool IsFailed() const
    {
        return status == GoalContractStatus::Failed;
    }

    void BeginDrafting()
    {
        status = GoalContractStatus::Drafting;
        successCriteria.clear();
        constraints.clear();
        evidenceChecks.clear();
        lastBuilderReason.clear();
    }

    void SetReady(std::vector<std::string> newSuccessCriteria,
                  std::vector<std::string> newConstraints,
                  std::vector<std::string> newEvidenceChecks,
                  std::string builderReason)
    {
        successCriteria = std::move(newSuccessCriteria);
        constraints = std::move(newConstraints);
        evidenceChecks = std::move(newEvidenceChecks);
        lastBuilderReason = std::move(builderReason);
        status = successCriteria.empty()
            ? GoalContractStatus::Failed
            : GoalContractStatus::Ready;
    }

    void MarkFailed(std::string reason)
    {
        status = GoalContractStatus::Failed;
        successCriteria.clear();
        constraints.clear();
        evidenceChecks.clear();
        lastBuilderReason = std::move(reason);
    }

    void Reset()
    {
        *this = GoalContract{};
    }
};

struct GoalState {
    GoalStatus status = GoalStatus::None;
    std::string objective;

    int turnsUsed = 0;
    int verifierPasses = 0;
    int verifierFailures = 0;
    std::string lastVerifierReason;
    std::string lastInterruptionReason;
    std::string awaitingUserReason;
    std::string awaitingUserPromptEvidence;
    std::string awaitingUserReplyEvidence;

    static constexpr size_t kMaxStructuredAgentEvidenceEvents = 56;
    std::vector<std::string> structuredAgentEvidence;

    GoalContract contract;

    // Phase 1 polish: keep a tiny in-memory tombstone after /goal clear so
    // prompt construction can stop old goal text in chat history from being
    // misread as a currently active mission.
    bool explicitlyCleared = false;

    bool HasGoal() const
    {
        return status != GoalStatus::None && !objective.empty();
    }

    bool IsActive() const
    {
        return HasGoal() && status == GoalStatus::Active;
    }

    bool IsPaused() const
    {
        return HasGoal() && status == GoalStatus::Paused;
    }

    bool IsAwaitingUser() const
    {
        return HasGoal() && status == GoalStatus::AwaitingUser;
    }

    bool IsCompleted() const
    {
        return HasGoal() && status == GoalStatus::Completed;
    }

    bool IsBudgetReached() const
    {
        return HasGoal() && status == GoalStatus::BudgetReached;
    }

    bool WasExplicitlyCleared() const
    {
        return explicitlyCleared && !HasGoal();
    }

    void Start(std::string newObjective)
    {
        status = GoalStatus::Active;
        objective = std::move(newObjective);
        turnsUsed = 0;
        verifierPasses = 0;
        verifierFailures = 0;
        lastVerifierReason.clear();
        lastInterruptionReason.clear();
        awaitingUserReason.clear();
        awaitingUserPromptEvidence.clear();
        awaitingUserReplyEvidence.clear();
        structuredAgentEvidence.clear();
        contract.Reset();
        explicitlyCleared = false;
    }

    void Pause()
    {
        if (HasGoal()) status = GoalStatus::Paused;
    }

    void Resume()
    {
        if (!HasGoal()) return;

        // Resuming after an automatic-continuation cap should begin a fresh
        // small budget window. Paused or completed goals preserve the count
        // because those statuses did not necessarily consume the whole cap.
        if (status == GoalStatus::BudgetReached)
            turnsUsed = 0;

        awaitingUserReason.clear();
        status = GoalStatus::Active;
    }

    void BeginContractDrafting()
    {
        if (HasGoal()) contract.BeginDrafting();
    }

    void SetContractReady(std::vector<std::string> successCriteria,
                          std::vector<std::string> constraints,
                          std::vector<std::string> evidenceChecks,
                          std::string builderReason)
    {
        if (!HasGoal()) return;
        contract.SetReady(std::move(successCriteria),
                          std::move(constraints),
                          std::move(evidenceChecks),
                          std::move(builderReason));
    }

    void MarkContractFailed(std::string reason)
    {
        if (!HasGoal()) return;
        contract.MarkFailed(std::move(reason));
    }

    void AppendStructuredAgentEvidence(std::string chunk)
    {
        if (chunk.empty()) return;

        if (structuredAgentEvidence.size() >= kMaxStructuredAgentEvidenceEvents) {
            structuredAgentEvidence.erase(structuredAgentEvidence.begin());
        }

        structuredAgentEvidence.push_back(std::move(chunk));
    }

    void TrimStructuredAgentEvidenceToCap()
    {
        while (structuredAgentEvidence.size() > kMaxStructuredAgentEvidenceEvents) {
            structuredAgentEvidence.erase(structuredAgentEvidence.begin());
        }
    }

    void ClearStructuredAgentEvidence()
    {
        structuredAgentEvidence.clear();
    }

    void MarkInterrupted(std::string reason)
    {
        lastInterruptionReason = std::move(reason);
    }

    void MarkAwaitingUser(std::string reason, std::string promptEvidence)
    {
        status = GoalStatus::AwaitingUser;
        lastInterruptionReason.clear();
        awaitingUserReason = reason;
        awaitingUserPromptEvidence = std::move(promptEvidence);
        awaitingUserReplyEvidence.clear();
        lastVerifierReason = std::move(reason);
    }

    void RecordAwaitingUserReply(std::string replyEvidence)
    {
        if (replyEvidence.empty()) return;
        awaitingUserReplyEvidence = std::move(replyEvidence);
    }

    void ClearInterruption()
    {
        lastInterruptionReason.clear();
    }

    void MarkVerifierContinue(std::string reason)
    {
        lastInterruptionReason.clear();
        awaitingUserReason.clear();
        lastVerifierReason = std::move(reason);
        ++verifierFailures;
    }

    void MarkVerifierUnclear(std::string reason)
    {
        lastInterruptionReason.clear();
        awaitingUserReason.clear();
        lastVerifierReason = std::move(reason);
        ++verifierFailures;
    }

    void MarkVerifiedComplete(std::string reason)
    {
        status = GoalStatus::Completed;
        lastInterruptionReason.clear();
        awaitingUserReason.clear();
        lastVerifierReason = std::move(reason);
        ++verifierPasses;
    }

    bool CanAutoContinue(int maxContinuations) const
    {
        return IsActive() && turnsUsed < maxContinuations;
    }

    void ConsumeAutoContinuation()
    {
        if (IsActive())
            ++turnsUsed;
    }

    void MarkBudgetReached(std::string reason)
    {
        status = GoalStatus::BudgetReached;
        lastInterruptionReason.clear();
        awaitingUserReason.clear();
        lastVerifierReason = std::move(reason);
    }

    void Clear()
    {
        status = GoalStatus::None;
        objective.clear();
        turnsUsed = 0;
        verifierPasses = 0;
        verifierFailures = 0;
        lastVerifierReason.clear();
        lastInterruptionReason.clear();
        awaitingUserReason.clear();
        awaitingUserPromptEvidence.clear();
        awaitingUserReplyEvidence.clear();
        structuredAgentEvidence.clear();
        contract.Reset();
        explicitlyCleared = true;
    }

    void Reset()
    {
        *this = GoalState{};
    }
};
