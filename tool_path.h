// tool_path.h
//
// Path-resolution helpers for Phase 3 slash commands.
//
// ResolveToolPath takes a user-supplied path (possibly relative,
// possibly containing %VAR% env-vars, possibly with mixed separators)
// and returns an absolute canonicalized UTF-8 path, or an empty string
// on any failure.  It does NOT check whether the path exists —
// callers layer IsDirectory() / IsFile() on top.
//
// The helpers are Windows-only, matching the rest of LlamaBoss Pro's
// target platform (CreateProcessW, Job Objects, wxRegKey, etc.).
//
#pragma once

#include <string>

// Expands %VAR%, resolves relative paths against `cwd`, and
// canonicalizes (normalizes `.`, `..`, separators) via GetFullPathNameW.
// Returns empty on failure.  Inputs and outputs are UTF-8.
std::string ResolveToolPath(const std::string& input,
                            const std::string& cwd);

// Returns true iff `absPath` names an existing directory.  Pass in
// the output of ResolveToolPath — must be an absolute path.
bool IsDirectory(const std::string& absPath);

// Returns true iff `absPath` names an existing regular file.
bool IsFile(const std::string& absPath);
