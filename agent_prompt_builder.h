#pragma once

// ─── agent_prompt_builder.h ────────────────────────────────────────
// Prompt construction for normal chat context and agent tool-use modes.
//
// LlamaBoss.cpp decides whether a turn is normal chat or agent mode,
// gathers the live frame state/context blocks, and chooses the active
// tool-call protocol.  This module owns the large prompt bodies and
// turns that already-prepared input into:
//   1) the normal non-agent context prompt,
//   2) the XML tool-call agent system prompt, and
//   3) the native function-calling agent system prompt.
//
// Keeping this prompt text here prevents the main frame implementation
// from carrying the largest remaining agent-policy text block.

#include <string>

// Heading of the per-chat section at the very end of both agent prompts.
// Everything before "\n\n" + this heading is byte-identical across chats,
// which is what prompt caching and the New Chat pre-warm
// (prompt_prewarm.h) rely on.  One definition so the builder and the
// pre-warm cut can never drift apart.
inline constexpr const char* kAgentWorkingContextHeading =
    "WORKING CONTEXT (this conversation)";

struct AgentPromptBuilderInput {
    bool        isWorkspace = false;
    std::string cwd;
    std::string activeProjectContextBlock;
    std::string pendingSkillAuthoringContextBlock;
    std::string toolSafetySummaryText;
};

std::string BuildNormalSystemPrompt(const AgentPromptBuilderInput& input);
std::string BuildAgentSystemPromptXml(const AgentPromptBuilderInput& input);
std::string BuildAgentSystemPromptNative(const AgentPromptBuilderInput& input);
