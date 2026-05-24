#pragma once

// ─── skill_prompt_builder.h ────────────────────────────────────────
// Prompt construction for the Skills Phase 2I authoring flow.
//
// This module owns the two large prompt bodies used by the hidden
// Skill Draft Builder request.  LlamaBoss.cpp decides when drafting
// should begin; this helper only turns the already-collected authoring
// state into:
//   1) the Skill Draft Builder system prompt, and
//   2) the one-shot user prompt that carries the Skill brief.
//
// Keeping this prompt text here prevents the main frame implementation
// from carrying a large Skill-specific policy block.

#include <string>

struct SkillPromptBuilderInput {
    std::string skillName;
    std::string skillPath;
    std::string pythonHelperPath;
    std::string userDescription;
};

std::string BuildSkillDraftBuilderSystemPrompt(const SkillPromptBuilderInput& input);
std::string BuildSkillDraftBuilderUserPrompt(const SkillPromptBuilderInput& input);
