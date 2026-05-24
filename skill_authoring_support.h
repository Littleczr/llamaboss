#pragma once

// ─── skill_authoring_support.h ─────────────────────────────────────
// Helpers extracted from LlamaBoss.cpp for the Skills Phase 2I
// "skill authoring is a real design conversation" flow.
//
// Everything in here is pure: no wxWidgets, no MyFrame, no global
// state.  These helpers classify user control messages during an
// active skill design session, parse the model's skill-draft builder
// payload, derive a helper-script path from a SKILL.md contract path,
// and write UTF-8 text files to disk.

#include <string>

// ─── Public markers ────────────────────────────────────────────────
// The skill draft builder model emits an optional python helper block
// wrapped in these two markers.  BuildSkillDraftBuilderUserPrompt
// references them so the prompt and parser agree on the protocol.
extern const char* const kSkillPythonHelperBeginMarker;
extern const char* const kSkillPythonHelperEndMarker;

// ─── Public types ──────────────────────────────────────────────────
struct LbSkillDraftBuilderPayload {
    std::string markdown;
    std::string pythonHelperSource;
    bool        malformedHelperBlock = false;
};

// ─── Public API ────────────────────────────────────────────────────
//
// Control-message recognizers.  These compare against an explicit
// allow-list of phrases (lowered, trimmed, with optional polite
// prefixes stripped).  Ordinary conversation falls through to the
// model unchanged.
bool LbSkillAuthoringInputCancelsSetup(const std::string& input);
bool LbSkillAuthoringInputRequestsDraft(const std::string& input);
bool LbSkillAuthoringInputConfirmsDraftPrompt(const std::string& input,
                                              const std::string& lastAssistantMessage);

// Quick sniffer that decides whether a builder response actually
// produced a SKILL.md.  Looks for the required H2 sections.
bool LbLooksLikeSkillMarkdown(const std::string& markdown);

// Returns true iff the markdown contains the "## Optional Python script"
// section that signals the builder also wants to ship a helper script.
bool LbSkillMarkdownRequestsPythonHelper(const std::string& markdown);

// Split the raw builder response into its markdown + optional python
// helper source.  Robust against the model wrapping the whole thing
// in a ``` fence.  payload.malformedHelperBlock is true when the
// begin marker is present but the end marker is missing.
LbSkillDraftBuilderPayload LbParseSkillDraftBuilderPayload(const std::string& rawResponse);

// Derive the helper script path next to a SKILL.md contract path.
// Example: "C:/.../Skills/My Skill/SKILL.md"
//       -> "C:/.../Skills/My Skill/My Skill.py"
// Returns empty on malformed input (no slash, empty folder name, etc.).
std::string LbSkillPythonHelperPathFromContractPath(const std::string& skillContractPath);

// Write the given content to `path` as UTF-8 (binary mode, truncating).
// Returns true on success.  Generic utility that currently only lives
// here because the skill authoring flow is its only caller.
bool LbWriteUtf8TextFile(const std::string& path, const std::string& content);
