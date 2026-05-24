// ─── goal_prompt_builder.cpp ──────────────────────────────────────
// Goal contract-drafting and verification prompt construction extracted
// from LlamaBoss.cpp.

#include "goal_prompt_builder.h"
#include "goal_verifier_support.h"

#include <sstream>

std::string BuildGoalContractBuilderSystemPrompt()
{
    return
        "You are the LlamaBoss Goal Contract Builder. "
        "You do not perform the user's task. "
        "You convert one active goal into a concise verifier contract. "
        "Do not invent requirements beyond the goal. "
        "Project rule: if ACTIVE PROJECT CONTEXT FOR CONTRACT DRAFTING is present "
        "and the Goal is project-related, use PROJECT.md as trusted project contract "
        "context when drafting success criteria, constraints, and evidence checks. "
        "Do not force unrelated Goals into project deliverables or invent project "
        "requirements that are not supported by the Goal or PROJECT.md. "
        "Skill rule: if AVAILABLE LLAMABOSS SKILL CONTEXT FOR CONTRACT DRAFTING is present "
        "and the Goal explicitly names a Skill or clearly depends on a listed Skill outcome, "
        "use that context when drafting the contract. For Skill-driven Goals, prefer success "
        "criteria or evidence checks that require reading the relevant SKILL.md contract before "
        "running a same-folder helper script or relying on Skill-specific execution, and require "
        "the requested user-visible deliverable/output to be produced. Do not force Skill use for "
        "unrelated Goals. "
        "Prefer criteria that a later verifier can judge from tool results, "
        "artifacts, or the assistant's final answer. "
        "Artifact rule: if the goal asks to create, generate, write, or save "
        "a report, markdown report, document, file, spreadsheet, PDF, artifact, "
        "or saved output, treat that as a user-visible created artifact/file requirement "
        "unless the goal explicitly says in chat, inline, reply only, or otherwise makes "
        "clear that no file artifact is expected. "
        "For such artifact goals, include a SUCCESS line requiring the artifact/file to be "
        "created and an EVIDENCE line requiring visible tool/artifact/file-creation evidence. "
        "Return only plain labeled lines using this format:\n"
        "SUCCESS: one concrete success criterion\n"
        "SUCCESS: another concrete success criterion\n"
        "CONSTRAINT: an important do-not-cross rule, only if needed\n"
        "EVIDENCE: what visible/tool evidence should support completion\n"
        "REASON: one short note explaining the contract shape\n"
        "Provide 2 to 5 SUCCESS lines, 0 to 4 CONSTRAINT lines, "
        "and 1 to 5 EVIDENCE lines.";
}

std::string BuildGoalContractBuilderUserPrompt(const GoalContractPromptInput& input)
{
    std::ostringstream p;
    if (!input.skillContext.empty()) {
        p << input.skillContext << "\n";
    }

    if (!input.projectContext.empty()) {
        p << input.projectContext << "\n";
    }

    p << "ACTIVE GOAL:\n"
      << input.objective << "\n\n"
      << "Draft the concise verifier contract now.";
    return p.str();
}

std::string BuildGoalVerifierSystemPrompt()
{
    return
        "You are the LlamaBoss Goal Verifier. "
        "You do not continue the task. You only judge whether the active goal "
        "is already satisfied from the evidence provided. "
        "When a structured verification contract is present, judge completion "
        "against its success criteria, constraints, and evidence checks. "
        "If ACTIVE PROJECT CONTEXT FOR VERIFICATION is present and the Goal or "
        "structured contract is project-related, use PROJECT.md and project facts "
        "as context for verification. Do not add project requirements to unrelated Goals. "
        "If AVAILABLE LLAMABOSS SKILL CONTEXT FOR VERIFICATION is present and the Goal or "
        "structured contract is Skill-related, use the listed Skill names, SKILL.md paths, "
        "and helper-script paths as verification context. When the contract requires Skill use, "
        "require evidence that the relevant SKILL.md contract was read before a Skill helper "
        "script or Skill-specific execution was treated as satisfying the goal. Do not add Skill "
        "requirements to unrelated Goals. "
        "Prefer STRUCTURED AGENTEVENT EVIDENCE for tool execution, artifact creation, "
        "edits, deletes, statuses, and tool-error facts; use transcript evidence to "
        "judge user-visible prose, requested final explanations, and any content that "
        "needs to be read directly. "
        "Tool result metadata such as byte counts, line counts, or status chips is not "
        "file content. If exact file content matters, require direct read/content evidence. "
        "If AWAITING-USER CHECKPOINT EVIDENCE is present, treat it as durable proof that "
        "the blocking assistant prompt was issued and/or that a waiting user reply was recorded. "
        "Use AWAIT_USER only when the transcript evidence shows the assistant has asked "
        "a genuine blocking clarification, confirmation, or user-decision question that "
        "must be answered before safe progress can continue. Do not use AWAIT_USER when "
        "the agent can reasonably continue using available evidence or tools. "
        "Artifact proof rule: if the goal or contract requires creating, generating, "
        "writing, or saving a report, markdown report, document, file, spreadsheet, PDF, "
        "artifact, or saved output, do not count assistant chat prose, Markdown-looking prose, "
        "or a mere claim of creation as completion. Require visible tool/artifact/file-creation "
        "evidence in the provided transcript unless the goal explicitly says the result should be "
        "in chat, inline, or reply only. "
        "Be strict: if the evidence is incomplete, ambiguous, merely a claim, "
        "or the requested deliverable is not clearly present, return CONTINUE. "
        "Return exactly two lines using one of these verdict tokens:\n"
        "VERDICT: COMPLETE\n"
        "REASON: one short sentence\n"
        "or\n"
        "VERDICT: CONTINUE\n"
        "REASON: one short sentence\n"
        "or\n"
        "VERDICT: AWAIT_USER\n"
        "REASON: one short sentence";
}

std::string BuildGoalVerifierUserPrompt(const GoalVerifierPromptInput& input)
{
    std::ostringstream p;
    if (!input.skillContext.empty()) {
        p << input.skillContext << "\n";
    }

    if (!input.projectContext.empty()) {
        p << input.projectContext << "\n";
    }

    p << "ACTIVE GOAL:\n"
      << input.objective << "\n\n";

    if (input.hasReadyContract) {
        p << "STRUCTURED VERIFICATION CONTRACT:\n"
          << "Success criteria:\n";
        for (const auto& item : input.successCriteria)
            p << "- " << item << "\n";

        if (!input.constraints.empty()) {
            p << "Constraints:\n";
            for (const auto& item : input.constraints)
                p << "- " << item << "\n";
        }

        if (!input.evidenceChecks.empty()) {
            p << "Evidence checks:\n";
            for (const auto& item : input.evidenceChecks)
                p << "- " << item << "\n";
        }

        p << "\n";
    }
    else {
        p << "STRUCTURED VERIFICATION CONTRACT:\n"
          << "(No structured contract is available. Judge against the goal text only.)\n\n";
    }

    if (!input.awaitingUserPromptEvidence.empty() ||
        !input.awaitingUserReplyEvidence.empty()) {
        p << "AWAITING-USER CHECKPOINT EVIDENCE:\n";
        if (!input.awaitingUserPromptEvidence.empty()) {
            p << "Blocking assistant prompt previously issued:\n"
              << LbClipForGoalVerifier(input.awaitingUserPromptEvidence, 1800)
              << "\n";
        }
        if (!input.awaitingUserReplyEvidence.empty()) {
            p << "User reply recorded while the goal was waiting:\n"
              << LbClipForGoalVerifier(input.awaitingUserReplyEvidence, 1200)
              << "\n";
        }
        p << "\n";
    }

    p << "STRUCTURED AGENTEVENT EVIDENCE:\n"
      << input.structuredAgentEvidence << "\n"
      << "RECENT TRANSCRIPT EVIDENCE:\n"
      << input.recentTranscriptEvidence << "\n"
      << "Judge only whether the active goal is complete.";
    return p.str();
}
