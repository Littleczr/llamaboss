#pragma once

// ─── skill_authoring_support.h ─────────────────────────────────────
// Helpers extracted from LlamaBoss.cpp for the Skills Phase 2I
// "skill authoring is a real design conversation" flow.
//
// Everything in here is pure: no wxWidgets, no MyFrame, no global
// state.  These helpers classify user control messages during an
// active skill design session, parse the model's skill-draft builder
// payload, derive a helper-script path from a SKILL.md contract path,
// read/synthesize the Agent Skills YAML frontmatter, and write UTF-8
// text files to disk.

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
// Tolerant of an Agent Skills YAML frontmatter block at the top.
bool LbLooksLikeSkillMarkdown(const std::string& markdown);

// Returns true iff the markdown contains the "## Optional Python script"
// section that signals the builder also wants to ship a helper script.
bool LbSkillMarkdownRequestsPythonHelper(const std::string& markdown);

// Split the raw builder response into its markdown + optional python
// helper source.  Robust against the model wrapping the whole thing
// in a ``` fence.  payload.malformedHelperBlock is true when the
// begin marker is present but the end marker is missing.
LbSkillDraftBuilderPayload LbParseSkillDraftBuilderPayload(const std::string& rawResponse);

// The Skill's folder stem (the Agent Skills `name`) derived from its
// SKILL.md contract path.
// Example: "C:/.../Skills/zip-source/SKILL.md" -> "zip-source"
// Returns empty on malformed input (no slash, empty folder name, etc.).
std::string LbSkillFolderNameFromContractPath(const std::string& skillContractPath);

// Derive the helper script path for a SKILL.md contract path.
// Agent Skills layout: helpers live in the skill's scripts\ subfolder.
// Example: "C:/.../Skills/zip-source/SKILL.md"
//       -> "C:/.../Skills/zip-source/scripts/zip-source.py"
// Returns empty on malformed input (no slash, empty folder name, etc.).
std::string LbSkillPythonHelperPathFromContractPath(const std::string& skillContractPath);

// Read the `description:` value from a SKILL.md's YAML frontmatter
// block, clipped for prompt use.  Returns empty when the file has no
// frontmatter, no description line, or cannot be read.  Legacy skills
// without frontmatter are expected and simply return empty.
std::string LbReadSkillFrontmatterDescription(const std::string& skillContractPath,
                                              std::size_t maxChars = 300);

// Read the `name:` value from a SKILL.md's YAML frontmatter block.
// Same tolerance rules as the description reader.  Used by skill
// import to prefer the authored Agent Skills name over the source
// folder name.
std::string LbReadSkillFrontmatterName(const std::string& skillContractPath);

// Rewrite (or insert) the `name:` field inside an existing frontmatter
// block so it matches the on-disk folder stem, which is authoritative
// in LlamaBoss.  Returns the markdown unchanged when there is no
// frontmatter block (run LbEnsureSkillFrontmatter first) or when the
// name already matches.  Used by skill import, where collision
// suffixes ("-2") can make the destination stem differ from the
// authored name.
std::string LbRewriteSkillFrontmatterName(std::string markdown,
                                          const std::string& name);

// Guarantee the drafted SKILL.md starts with an Agent Skills YAML
// frontmatter block (name + description).  If the builder already
// emitted one, the markdown is returned unchanged.  Otherwise a
// minimal block is synthesized: `name` from the skill folder stem and
// `description` from the first body paragraph.  This keeps every
// saved skill standard-compliant even when a smaller model ignores
// the frontmatter instruction.
std::string LbEnsureSkillFrontmatter(std::string markdown,
                                     const std::string& skillFolderName);

// Write the given content to `path` as UTF-8 (binary mode, truncating),
// creating missing parent directories first (needed for the skill's
// scripts subfolder on first helper save).  Returns true on success.
bool LbWriteUtf8TextFile(const std::string& path, const std::string& content);
