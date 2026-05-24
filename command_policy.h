// command_policy.h
//
// Phase 5+: Agent harness — PowerShell command policy.
//
// EvaluatePowerShellCommand is the single classifier between the
// agent dispatcher, the approval gate, and CmdExecutor.  Given a
// command string emitted for the `powershell` tool, it returns one of
// three practical outcomes encoded as booleans:
//
//   allowed          : clearly read-only; may run immediately
//   requiresApproval : broader shell automation; may run only after
//                      the normal approval-card path has paused the
//                      invocation for user review
//   neither          : malformed/empty command; reject outright
//
// ─── Policy summary ──────────────────────────────────────────────
// 1. Reject empty / whitespace-only commands.
// 2. Reject only malformed quote drift that this lightweight scanner
//    can identify reliably (unterminated single/double-quoted strings).
// 3. Preserve the existing quote-aware pipeline split so clearly
//    read-only commands can still be auto-classified stage by stage.
// 4. Auto-run when every pipeline stage has a simple head on the
//    read-only allowlist and the command contains none of the shell
//    constructs that historically made allowlist-only classification
//    unsafe.
// 5. Require approval instead of hard-blocking when a command falls
//    outside that narrow auto-run profile, including:
//      - non-allowlisted command heads such as powershell, Compress-Archive,
//        Remove-Item, git, etc.
//      - script blocks, grouping/subexpressions, redirection, separators,
//        backticks, call/background operators, or double-quoted `$` expansion
//      - newlines or other syntax that is valid PowerShell but no longer
//        eligible for silent read-only auto-run
//
// This keeps the old low-friction inspection path intact while allowing
// developers to perform broader PowerShell workflows through explicit
// user approval rather than forcing LlamaBoss to grow one bespoke tool
// per shell task.
#pragma once

#include <string>

struct PolicyDecision {
    // True only for clearly read-only allowlisted commands that may run
    // immediately without showing an approval card.
    bool        allowed = false;

    // True for syntactically usable PowerShell commands that are outside
    // the automatic read-only profile.  Callers must pause for approval
    // before dispatching these commands.
    bool        requiresApproval = false;

    // Human-facing diagnostic.  Empty when `allowed` is true; for
    // approval-gated commands it explains why review is required; for
    // rejected commands it explains why the command cannot be classified.
    std::string reason;
};

// Classify `command` against the PowerShell auto-run / approval policy.
// Pure function: no I/O, no global state, safe to call from any thread.
PolicyDecision EvaluatePowerShellCommand(const std::string& command);
