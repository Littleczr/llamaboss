#pragma once

// ─── python_package_recovery.h ─────────────────────────────────────
// Helpers extracted from LlamaBoss.cpp for the python-package missing-
// dependency recovery flow.  When a python tool invocation fails with
// "ModuleNotFoundError: No module named X" or a similar diagnostic,
// this module classifies the missing dependency, normalizes its
// import name to a canonical PyPI package name, and decorates the
// ToolInvocationResult with a recovery suggestion the agent can act
// on (typically by proposing `python_install_package <name>`).
//
// Everything in here is pure: no wxWidgets, no MyFrame, no global
// state.  The character whitelist used to vet a candidate package
// name mirrors NormalizeAllowedPythonPackage in python_runner.cpp,
// which remains the authoritative safety boundary for actually
// running pip.

#include <string>

// Forward declarations — the function takes both by reference, so the
// header avoids dragging tool_invocation.h / python_runner.h along.
// Callers naturally have these types in scope via the headers they
// already include.
struct ToolInvocationResult;
struct PythonRunResult;

// Inspect a failed python run for a "missing module" pattern.  If
// one is detected, mutate `r` in place so its body, toolName, and
// iconUtf8 surface the recovery suggestion to the agent and user.
// No-op when `py` succeeded, was cancelled, or timed out, and no-op
// when no recognizable missing-package signal is found.
void ApplyMissingPythonPackageRecovery(ToolInvocationResult&  r,
                                       const PythonRunResult& py);
