// command_policy_zsh.h
//
// macOS counterpart of EvaluatePowerShellCommand: classifies one command
// for the zsh-backed shell tool into the same three outcomes.
//
//   allowed          : clearly read-only; may run immediately
//   requiresApproval : anything broader; runs only after an approval card
//   neither          : empty or malformed (unterminated quote); rejected
//
// Auto-run needs ALL of:
//   - plain ASCII, one line, no unterminated quotes
//   - no shell constructs that run or redirect code: ; & && || (grouping)
//     {braces} `backticks` $(...) ${...} and redirection other than
//     2>/dev/null, >/dev/null and 2>&1
//   - every pipeline stage starts with an allowlisted read-only program
//     (no path, no leading VAR=value), and that program's arguments pass
//     its write-flag checks (find -delete/-exec, sort -o, tree -o, xxd -r,
//     sysctl a=b, defaults write, ...)
//
// Pure function: no I/O, no global state, safe from any thread.  Built
// only on non-Windows targets; EvaluatePowerShellCommand forwards here.
#pragma once

#include <string>

#include "command_policy.h"   // PolicyDecision

PolicyDecision EvaluateZshCommand(const std::string& command);
