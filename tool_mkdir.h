// tool_mkdir.h
//
// Phase 6 sibling to tool_write: create a single directory under
// the per-conversation tool CWD.
//
// MakeDirectory is idempotent on directories: if the leaf already
// exists AS a directory, the call succeeds and is reported as
// "exists" so the model can tell the difference from a fresh
// creation.  If the leaf exists as a FILE, the call fails -- we
// won't silently substitute one inode shape for the other.
//
// Same containment story as tool_write: the resolved path must
// land inside ctx.cwd, the leaf basename must survive
// path_safety::SanitizeFilename intact, and the parent directory
// must already exist (single operations only; no implicit chain
// creation).  Risky-extension classification doesn't apply --
// directories aren't executed by ShellExecute.
//
// Synchronous on the caller's thread.  CreateDirectoryW on a
// local NTFS volume is microsecond-scale.
//
#pragma once

#include "tool_context.h"

#include <string>
#include <vector>

struct MkdirResult {
    // Header chips: outcome + elapsed.  No size / line count to
    // report for a directory creation.
    //   Newly created:        ["created", "0.01s"]
    //   Already a directory:  ["exists",  "0.00s"]
    //   Fail outside cwd:     ["blocked", "0.00s"]
    //   Fail other:           ["failed",  "0.00s"]
    std::vector<std::string> chips;

    // One-line confirmation: the absolute resolved path of the
    // directory, in a brief sentence.  Populated on both success
    // and idempotent-exists outcomes.
    std::string body;

    // Failure diagnostic.  Empty on success.
    std::string errorBody;

    // Always empty -- shape parity with the other tool results.
    std::string bodyLang;
};

// Creates a single directory at `path` (relative paths resolve
// against ctx.cwd).  Refuses traversal escapes, unsafe names, and
// inputs whose parent directory doesn't exist.  Returns "exists"
// rather than failing if the path is already a directory.
//
// Never throws.  Every path through the function returns a fully
// populated MkdirResult.
MkdirResult MakeDirectory(const std::string& path,
                          const ToolContext& ctx);
