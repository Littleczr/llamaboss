// command_policy.h
//
// Phase 5: Agent harness — PowerShell command policy.
//
// EvaluatePowerShellCommand is the single chokepoint between the
// agent dispatcher and CmdExecutor.  Given a command string the
// model emitted, it decides whether the command is safe to run
// under the read-only policy that gates the `powershell` tool.
//
// ─── Policy summary ──────────────────────────────────────────────
// 1. Reject empty / whitespace-only commands.
// 2. Reject dangerous PowerShell syntax that could bypass the
//    per-stage allowlist:
//      ;  &  >  <  `  {  }  (  )  plus the digraphs $(  @(  @{
//
//    The rejection is quote-aware:
//      - dangerous characters are rejected outside quotes
//      - literal text inside single-quoted strings is allowed
//      - double-quoted strings are allowed only when they contain no $
//      - backtick is rejected everywhere
//
// 3. Split on pipeline `|` only outside quotes.  Literal `|` inside a
//    quoted string, such as Select-String -Pattern 'foo|bar', is data.
// 4. For each pipeline stage, take the first whitespace-delimited token
//    as the command head and require it to be a plain cmdlet name.
// 5. Allow the head iff it matches the read-only allowlist by:
//      - exact name, case-insensitive
//      - verb-prefix, case-insensitive, e.g. Get-, Test-, Select-
//
// Any rejection produces a human-readable `reason` that the
// dispatcher feeds back to the model as a tool-result errorBody so
// the model can self-correct on the next iteration.
//
// ─── Why this shape ──────────────────────────────────────────────
// PowerShell is too rich to parse safely with regex. Anything involving
// subexpressions, script blocks, the call operator, or statement
// separators can hide arbitrary code from a head-verb check.  This
// policy statically excludes those constructs outside strings, keeps
// literal quoted strings usable for common read-only commands, and
// leans on the allowlist as the actual security boundary.
//
// The boundary is the allowlist. Anything not on the allowlist
// (e.g. Set-*, Remove-*, Invoke-*, New-*, Add-*, Stop-*) is denied —
// and the obvious paths around the allowlist are syntactically excluded.
//
#pragma once

#include <string>

struct PolicyDecision {
    bool        allowed = false;
    // User-facing diagnostic. Empty when allowed; on deny it names
    // the offending construct so the model can rephrase.
    std::string reason;
};

// Evaluate `command` against the read-only policy. Pure function:
// no I/O, no global state, safe to call from any thread.
PolicyDecision EvaluatePowerShellCommand(const std::string& command);
