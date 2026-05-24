#pragma once

// ─── goal_prompt_builder.h ─────────────────────────────────────────
// Prompt construction for Goal contract drafting and Goal verification.
//
// LlamaBoss.cpp decides when a Goal contract should be drafted or
// verified.  This module owns the large prompt bodies and turns the
// already-prepared Goal/context evidence into:
//   1) the Goal Contract Builder system/user prompts, and
//   2) the Goal Verifier system/user prompts.
//
// Keeping this prompt text here prevents the main frame implementation
// from carrying large Goal-specific policy blocks.

#include <string>
#include <vector>

struct GoalContractPromptInput {
    std::string skillContext;
    std::string projectContext;
    std::string objective;
};

struct GoalVerifierPromptInput {
    std::string skillContext;
    std::string projectContext;
    std::string objective;

    bool hasReadyContract = false;
    std::vector<std::string> successCriteria;
    std::vector<std::string> constraints;
    std::vector<std::string> evidenceChecks;

    std::string awaitingUserPromptEvidence;
    std::string awaitingUserReplyEvidence;
    std::string structuredAgentEvidence;
    std::string recentTranscriptEvidence;
};

std::string BuildGoalContractBuilderSystemPrompt();
std::string BuildGoalContractBuilderUserPrompt(const GoalContractPromptInput& input);

std::string BuildGoalVerifierSystemPrompt();
std::string BuildGoalVerifierUserPrompt(const GoalVerifierPromptInput& input);
