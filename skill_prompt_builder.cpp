// ─── skill_prompt_builder.cpp ─────────────────────────────────────
// Skill Draft Builder prompt construction extracted from LlamaBoss.cpp.

#include "skill_prompt_builder.h"
#include "skill_authoring_support.h"

#include <sstream>

std::string BuildSkillDraftBuilderSystemPrompt(const SkillPromptBuilderInput& input)
{
    // Agent Skills `name` = the skill's folder stem (kebab-case at
    // creation time).  Derived here so the frontmatter instruction can
    // pin the exact value instead of letting the model improvise one.
    const std::string skillFolderName =
        LbSkillFolderNameFromContractPath(input.skillPath);

    std::ostringstream p;
    p << "You are the LlamaBoss Skill Draft Builder. "
      << "You write the initial SKILL.md instruction file for one reusable cross-project LlamaBoss Skill based on the completed design conversation supplied by the application. "
      << "You do not run the Skill yourself. "
      << "Return the complete SKILL.md file first, with no Markdown fence around it and no commentary. "

      // Agent Skills interop: the frontmatter block is the industry-
      // standard contract (name + description) that lets LlamaBoss
      // surface each skill's description cheaply in the system prompt
      // and lets skills be shared with other Agent Skills consumers.
      << "\n\nFRONTMATTER (REQUIRED, FIRST). "
      << "The very first line of the file must be `---` opening a YAML frontmatter block, followed by exactly two fields and a closing `---` line: "
      << "`name: " << (skillFolderName.empty() ? input.skillName : skillFolderName) << "` (use this exact value; do not change it) "
      << "and `description: ` -- one or two plain sentences, under 500 characters, stating BOTH what the Skill does AND when the user would want it used. "
      << "The description is shown to the model when it decides whether to use this Skill, so make it concrete: name the task, the inputs it acts on, and the trigger situation. "
      << "Do not add any other frontmatter fields. "

      << "\n\nAfter the closing `---`, use these exact required top-level sections in this order: "
      << "# <Skill Name> Skill, an introductory paragraph, ## Trigger Phrases, "
      << "## Inputs to Ask For, ## Steps, ## Output Expectations. "

      // The single most important rule -- this is what was failing
      // before. The builder used to convert every concrete value
      // from the design conversation into an "Inputs to Ask For"
      // entry, which made every Skill re-interrogate the user at
      // use-time even when the design conversation had already
      // settled the same values.
      << "\n\nCAPTURE-SPECIFICS RULE (HIGHEST PRIORITY). "
      << "Every concrete value the user established in the design conversation -- folder paths, output filenames, file extensions, recursion choices, target directories, specific files, URLs, named identifiers, numeric thresholds -- MUST be encoded directly in the Steps section as a fixed hardcoded value. "
      << "Do NOT move these values into ## Inputs to Ask For. "
      << "If the user said `in this folder` and the conversation contains a specific folder path, write that exact path into Steps. "
      << "If the user approved a specific extension filter (for example by saying `that list is fine` after an ls of .cpp and .h files), encode those exact extensions into Steps. "
      << "If the user named an output filename or location, encode that exact path into Steps. "
      << "## Inputs to Ask For is reserved for values the user EXPLICITLY said they want to choose at use-time. "
      << "An empty ## Inputs to Ask For section (write just `* None.`) is the preferred outcome when the design conversation fully specifies the task. "
      << "It is better for a Skill to be too specific (and easily edited later by the user) than for it to re-ask the user for things they already decided during design. "

      // Tool selection rule, tightened.  Without this, the builder
      // would draft PowerShell for tasks the user had already
      // demonstrated working with native ls/write.
      << "\n\nDEMONSTRATED-TOOL RULE. "
      << "If the design conversation included an actual tool call that produced a result the user approved (for example, the user said `that list is fine` after an `ls` of a folder), encode that exact tool sequence in Steps. "
      << "Do NOT substitute a different tool family (PowerShell, Python) for a task that was already demonstrated to work with native LlamaBoss tools. "
      << "For one-level or shallow folder listings, simple extension filtering, file reads, writes, grep, or notes lookups, draft the Skill around native LlamaBoss tools: ls, read, write, grep, edit, notes_read, open. "

      // Implementation-path preference order (only when no tool
      // sequence was demonstrated -- otherwise the rule above wins).
      // This order is now also the Python-helper decision rule: the
      // user no longer picks with/without a script at creation, so
      // whether a helper ships is decided HERE, by this preference.
      << "\n\nIMPLEMENTATION-PATH PREFERENCE (apply only when the design conversation did NOT demonstrate a working tool sequence). "
      << "First, prefer native LlamaBoss tools for ordinary file and text operations. "
      << "Second, prefer approved PowerShell for Windows-native tasks that native tools cannot express -- recursive multi-folder searches, metadata-heavy reports, archive workflows, bulk file operations, OS-level utilities. "
      << "Third, only choose Python when the task genuinely needs reusable custom code that the above paths handle poorly: API/web requests, transcript fetching, structured parsing, nontrivial data transformation, deterministic format-specific logic. "
      << "Do not choose Python merely because Python could solve the task. "
      << "Do not choose Python for ordinary file lists, source manifests, basic ZIP backups, basic text reports, or other straightforward Windows file operations. "

      // PowerShell guardrails -- the empty-output bug from
      // Get-ChildItem -Recurse -Include without a wildcard in -Path
      // is the specific failure that motivated this rewrite.
      << "\n\nPOWERSHELL GUARDRAILS (when PowerShell is the chosen path). "
      << "For Get-ChildItem with a recursive multi-extension filter, the correct pattern is `Get-ChildItem -Path '<dir>\\*' -Recurse -Include '*.ext1','*.ext2'`. "
      << "The trailing `\\*` on -Path is REQUIRED for -Include to return any results when -Recurse is set. "
      << "Do NOT write `Get-ChildItem -Path '<dir>' -Recurse -Include '*.ext1','*.ext2'` (no wildcard on -Path) -- it silently returns nothing and writes an empty file. "
      << "Do NOT use -Filter with multiple patterns; -Filter accepts exactly one pattern. "
      << "When the Skill writes a file via Out-File, add a follow-up verification step that confirms the file exists and is non-empty before reporting success. ";

    if (!input.pythonHelperPath.empty()) {
        // Merged branch: Python is always AVAILABLE, never mandatory.
        // The old two-branch design ("user picked with-script so a
        // helper MUST ship" / "user didn't so it MUST NOT") is retired
        // along with the two-entry creation menu.  The builder decides,
        // and the implementation-path preference above is the rule.
        p << "\n\nPYTHON HELPER (available, not mandatory -- your decision). "
          << "A Python helper file slot is reserved for this Skill, but whether one ships is decided by the IMPLEMENTATION-PATH PREFERENCE above. "
          << "Ship a helper ONLY when Python is genuinely the right implementation path for this Skill under that preference order. "
          << "If native LlamaBoss tools or approved PowerShell cover the task, do NOT ship a helper: do not add a ## Optional Python Script section, do not emit any Python source block, and do not reference python_run_script in Steps. "
          << "If and only if Python IS the right path: "
          << "add the exact top-level section ## Optional Python Script immediately after ## Output Expectations and before ## Safety / Approval Notes. "
          << "Name the helper script by filename and describe exactly what runtime inputs it expects. "
          << "The helper must be a complete runnable implementation, not a starter scaffold, placeholder, TODO, or script to finish later. "
          << "Do not say that LlamaBoss will implement, refine, or complete the helper on first use. "
          << "The Skill steps may run the helper directly with `python_run_script` once the required inputs are known. "
          << "The reserved helper script path (inside the Skill's scripts subfolder) is: "
          << input.pythonHelperPath << ". "
          << "Do not reference temporary workspace scripts, design-time test scripts, or other one-off artifacts from the design conversation in SKILL.md; the saved Skill contract must stand alone and point only to its saved in-folder helper. "
          << "If the helper needs runtime inputs such as a folder path, URL, output name, or mode, describe the args contract clearly: first line is the helper filename or path, and each optional later line is one command-line argument. "
          << "Prefer the Python standard library unless the user description clearly implies another dependency. "
          << "Write defensive argument validation, return a nonzero exit code on invalid inputs or failed work, and print clear success output including generated artifact paths when files are created. "
          << "For ZIP or archive helpers that walk a user-selected folder, treat an individual unreadable, locked, or permission-denied file as a recoverable warning rather than a whole-task failure whenever a valid archive can still be created. Skip the affected file, continue archiving, summarize what was skipped, and return success when the requested archive was produced. "
          << "If the requested Skill is specifically for source-code backups rather than a full byte-for-byte folder mirror, describe and implement sensible handling for transient IDE/cache/build content so files such as Visual Studio .vs cache entries do not make the backup fail. "
          << "When you ship a helper, after the Markdown body emit the full Python source only inside this exact machine-readable block, with no code fences:\n"
          << kSkillPythonHelperBeginMarker << "\n"
          << "<complete runnable Python source>\n"
          << kSkillPythonHelperEndMarker << " "
          << "\nCONSISTENCY: the ## Optional Python Script section and the marker block must appear together or not at all. Never emit one without the other. ";
    } else {
        // Defensive fallback only: the controller now always reserves a
        // helper path, so this branch fires only on a malformed contract
        // path.  Keep the firm no-Python wording so the draft stays
        // internally consistent with the save-side validation.
        p << "\n\nNO PYTHON HELPER (firm). "
          << "No helper script path could be reserved for this Skill, so this Skill MUST NOT include a Python helper. "
          << "Do not add a ## Optional Python Script section. "
          << "Do not emit any Python source block. "
          << "Do not reference python_run_script in Steps. "
          << "Draft the Skill around native LlamaBoss tools or approved PowerShell only. ";
    }

    p << "\n\nFINAL STRUCTURAL RULES. "
      << "Always finish the Markdown body with the exact top-level section ## Safety / Approval Notes. "
      << "The introductory paragraph should explain that this is a reusable LlamaBoss Skill, that it is available across conversations, that LlamaBoss should read this SKILL.md first when the user asks to run or use the Skill, and that project workflows with the same name take precedence when a project is attached. "
      << "Trigger phrases should be 3-6 natural examples drawn from how the user actually talked about this task in the design conversation. "
      << "Steps should be practical, concise, ordered, and contain the concrete values established in the design conversation. "
      << "Do not invent APIs, credentials, or tools that the user did not imply. "
      << "For ## Safety / Approval Notes, include sensible LlamaBoss guidance that writes, edits, deletes, Python runs, or other mutating actions should follow normal approval rules, and project files should not be modified unless the user explicitly asks.";
    return p.str();
}

std::string BuildSkillDraftBuilderUserPrompt(const SkillPromptBuilderInput& input)
{
    std::ostringstream p;
    p << "SKILL NAME:\n"
      << input.skillName << "\n\n"
      << "SKILL FILE PATH:\n"
      << input.skillPath << "\n\n";
    if (!input.pythonHelperPath.empty()) {
        p << "RESERVED PYTHON HELPER SCRIPT PATH (use only if Python is the chosen implementation path):\n"
          << input.pythonHelperPath << "\n\n";
    }
    p << "SKILL DESIGN CONVERSATION / AUTHORING BRIEF:\n"
      << input.userDescription << "\n\n"
      << "Draft the complete SKILL.md now based on the design conversation above, starting with the required YAML frontmatter block. "
      << "Respect the implementation direction established in that conversation, especially any preference for existing LlamaBoss tools or PowerShell. Do not switch to Python merely because Python could solve the task. "
      << "If a Python helper is genuinely needed after applying that preference order, append the exact machine-readable helper source block required by the system instructions.";
    return p.str();
}
