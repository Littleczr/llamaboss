// tool_safety.h
//
// Single home for the descriptive safety metadata attached to every
// ToolSpec.  Promoted out of tool_router.h so tool_approval.h can
// share these types without pulling in the full router header.
//
// This is METADATA, not enforcement.  Enforcement still lives in
// per-tool implementations (path_safety, command_policy, the staged-
// write commit path, the approval gate).  The metadata exists so
// those layers, the model-facing prompt summary, and any future
// audit/telemetry surface read from one source of truth.
//
// ─── Migration note ──────────────────────────────────────────────
// Before this header existed:
//   - RiskTier               lived in tool_approval.h
//   - ToolSafetyProfile      lived in tool_router.h
//   - ClassifyTier()         was a hardcoded if/else ladder in
//                            tool_approval.h that duplicated the
//                            per-tool requiresApproval=true lines
//                            in BuildBuiltinSpecs.
//
// After this header:
//   - All safety types       live here.
//   - ClassifyTier()         is a one-line lookup against the router.
//   - safety.tier            is the only place a tool's classification
//                            is declared; safety.requiresApproval()
//                            is derived from it.
//
#pragma once

#include <string>

// ─── Risk tier — drives the approval gate ───────────────────────
// RequiresApproval used to walk a per-tool if/else ladder where each
// branch hard-coded "needs approval" for tools that touched the
// filesystem.  Once conversational consent matured (the model self-
// asks "want me to do X?" and the user replies in prose), that
// ladder produced double-prompts: model asks, user says yes, system
// ALSO renders an approval card asking the same thing.
//
// The tier system replaces that ladder.  Each tool is classified
// once on its ToolSpec.safety.tier; the approval gate renders a
// card only for the Dangerous tier.  Safe and Moderate tools rely
// on:
//   - in agent mode  : the model's natural-language ask + user "yes"
//   - in slash mode  : the user having literally typed the command
//
// PowerShell uses a hybrid safety path.  command_policy.cpp auto-allows
// clearly read-only inspection commands, rejects malformed commands, and
// routes broader syntactically usable shell automation into the normal
// approval-card layer for explicit user review.
enum class RiskTier {
    // Pure read-only or self-bounded helper.  No file modification,
    // no arbitrary code execution, no remote-mutating network reach.
    // Examples: read, ls, grep, pwd, open, csv_inspect, xlsx_inspect,
    // pdf_inspect_form, docx_inspect, python_health, web_fetch_url.
    Safe,

    // Modifies workspace state (files, directories, generated
    // reports) but cannot escape the workspace and cannot directly
    // create an executable artifact.  write blocks risky extensions;
    // edit only touches existing files; csv_report / xlsx_report /
    // pdf_extract_text / docx_extract_text write a single artifact
    // into a fixed conversation lane; mkdir creates an empty
    // directory; python_run_script runs a .py whose source was
    // already reviewed when python_create_script's approval card was
    // rendered, or lives as an optional helper in an active project
    // Workflows folder; notes_append / project_notes_append append
    // one entry to a fixed user-owned NOTES.md.
    Moderate,

    // Irreversible, or carries arbitrary code-execution risk that
    // benefits from explicit per-invocation review:
    //   - delete                : removes a file or empty directory;
    //                             can't undo.
    //   - python_create_script  : writes a .py the user will then
    //                             run; the approval card body shows
    //                             the script source for review.
    //   - python_install_package: installs one PyPI package into
    //                             the user-site Python environment;
    //                             changes local state and uses the
    //                             network.
    Dangerous,
};

// ─── Network footprint ──────────────────────────────────────────
// Orthogonal to RiskTier.  A Dangerous tool may be None (delete),
// a Safe tool may be PublicRead (web_fetch_url).  This enum exists
// primarily so future remote-API tools (Smartsheets, etc.) can be
// described accurately on the same ToolSpec.safety struct as
// local-only tools.
enum class NetworkReach {
    // No network access at all.  Default; nearly every current tool.
    None,

    // Unauthenticated GET against the public web.  Today: web_fetch_url.
    PublicRead,

    // Reads from an authenticated remote service (uses a credential
    // but does not change remote state).  Reserved for future API
    // tools like "list rows from Smartsheets".
    AuthenticatedRead,

    // Writes or mutates state on an authenticated remote service.
    // Reserved for future API tools like "update a Smartsheets row",
    // "post a Slack message", "send an email".  These will typically
    // be RiskTier::Dangerous and Reversibility::AffectsOthers.
    AuthenticatedWrite,
};

// ─── Reversibility — blast radius if invoked wrongly ────────────
// Ordered by severity.  AffectsOthers implies Irreversible by
// definition (a sent message can't be made un-seen).  Used today
// for descriptive metadata; future audit/telemetry surfaces will
// branch on it.
enum class Reversibility {
    // Undoable, or creates new content that can simply be removed.
    // Default; covers reads, listings, write (new file), mkdir, the
    // report-generating tools.
    Reversible,

    // Destroys or replaces local content that the user may not have
    // a backup of.  Today: delete.  Edit and overwrite_file are
    // borderline; left as Reversible by default because staged-write
    // commits are atomic and the prior content is recoverable until
    // the next commit, but specific registrations may override.
    Irreversible,

    // Visible to other humans the instant it runs.  Reserved for
    // future remote-API write tools (Smartsheets row update, Slack
    // post, email send, calendar invite).  Implies Irreversible.
    AffectsOthers,
};

// ─── ToolSafetyProfile ──────────────────────────────────────────
// Attached to every ToolSpec.  Previously lived in tool_router.h;
// promoted here so tool_approval.h's ClassifyTier() can read the
// tier field without dragging the dispatcher headers into every
// translation unit that needs the enum.
struct ToolSafetyProfile {
    // ── Tier (single source of truth for the approval gate) ─────
    // ClassifyTier() in tool_approval.h reads this field via a
    // GetGlobalRouter().Find(name) lookup.  Setting it correctly
    // here is the ONLY place a tool's approval-card behaviour is
    // declared.
    RiskTier tier = RiskTier::Safe;

    // ── Local filesystem footprint ──────────────────────────────
    // True when the tool is intended only to inspect/read state.
    bool readOnly = false;

    // True when the tool can create, edit, delete, or otherwise
    // mutate local files/folders or create artifact outputs.
    bool mutatesFiles = false;

    // True when read-only inspection may target absolute local
    // paths outside the conversation working directory, if the
    // underlying tool/policy allows the path.
    bool mayInspectOutsideCwd = false;

    // True when writes/mutations are restricted to the conversation
    // working directory, plus the active project root when a project
    // is attached.
    bool writesInsideCwdOnly = false;

    // ── Network + remote-side-effect footprint ──────────────────
    // Describes the tool's reach beyond the local filesystem.
    // Defaults to None so existing local-only tools need no change.
    NetworkReach network = NetworkReach::None;

    // Describes the blast radius if the tool is invoked wrongly.
    // Defaults to Reversible; specific tools (delete) override.
    Reversibility reversibility = Reversibility::Reversible;

    // ── Dispatch shape ──────────────────────────────────────────
    // Declarative async-or-sync flag.  Reserved for a follow-up
    // change that asserts DispatchInvocation's returned status
    // matches this declaration; descriptive only for now.
    bool isAsync = false;

    // ── Policy-enforced (orthogonal to tier) ────────────────────
    // True for broad shell/process tools whose safety is classified
    // by a separate policy layer (today: powershell, where clearly
    // read-only commands auto-run and broader valid commands require
    // approval through command_policy.cpp + tool_approval.h).
    bool policyEnforced = false;

    // ── Human-readable summary ──────────────────────────────────
    // Keep short: this is used in prompts and may be appended to
    // native tool descriptions.
    std::string summary;

    // ── Derived ─────────────────────────────────────────────────
    // Approval card requirement is fully derived from tier; tools
    // no longer set this directly.  Existing call sites that read
    // spec.safety.requiresApproval now invoke spec.safety.requiresApproval().
    bool requiresApproval() const { return tier == RiskTier::Dangerous; }
};
