// tool_delete.h
//
// Phase 8: delete a single file or empty directory under the
// per-conversation tool CWD.
//
// DeleteEntry removes one filesystem entry at a time.  Files are
// deleted unconditionally (subject to the usual safety floor).
// Directories are deleted ONLY if empty -- non-empty dirs are
// refused with an entry-count diagnostic so the model can list,
// delete the contents one-by-one, then retry the dir delete.
//
// Mismatch with tool_mkdir's idempotency is intentional: mkdir
// returns success when the dir already exists because creation
// converged on the desired state.  Delete returns "not found"
// when the path doesn't exist because deletion is destructive
// and the model should know whether it actually destroyed
// something it expected to destroy.
//
// ─── Why no recursive delete ─────────────────────────────────────
// The model has no way to wipe a tree of files in one call.  This
// is on purpose: every deletion is auditable as a single tool
// block in chat history.  A multi-file cleanup walks naturally
// through ls + per-entry delete iterations.  PowerShell's
// Remove-Item is also rejected by command_policy (Remove-* verb
// prefix is not on the read-only allowlist), so there is no
// shell-side back door either -- the read-only policy and the
// no-recursive-delete policy are mutually consistent.
//
// ─── Safety floor (same shape as write/edit/mkdir) ───────────────
// 1. Containment: the resolved path must be inside ctx.cwd.
// 2. Sanitization: the basename must survive SanitizeFilename
//    intact (defends against weird models emitting names with
//    Windows-invalid characters).
// 3. Risky-extension block: refuse to delete files matching
//    ClassifyForOpen's Risky kill-list (.exe, .bat, .ps1, .reg,
//    .lnk, .vbs, macro Office docs, etc.).  Symmetric with
//    write -- if we won't create scriptable files, we won't
//    delete them either.  User can clean those up manually.
// 4. Existence: the path must exist.  No-op deletes return
//    "not found" rather than silently succeeding.
//
// Synchronous on the caller's thread.  DeleteFileW / RemoveDirectoryW
// on a local NTFS volume is microsecond-scale.
//
#pragma once

#include "tool_context.h"

#include <string>
#include <vector>

struct DeleteResult {
    // Header chips: outcome + (sometimes) detail + elapsed.
    //   File deleted:        ["deleted", "1.2 KB",     "0.02s"]
    //   Empty dir deleted:   ["deleted", "directory",  "0.01s"]
    //   Refuse non-empty:    ["not empty", "5 entries", "0.00s"]
    //   Refuse blocked:      ["blocked",                "0.00s"]
    //   Not found:           ["not found",              "0.00s"]
    //   Failed:              ["failed",                 "0.00s"]
    std::vector<std::string> chips;

    // One-line confirmation on success: "Deleted FILE" or
    // "Deleted directory DIR".  Empty on failure (errorBody
    // carries the diagnostic).
    std::string body;

    // Failure reason.  Empty on success.
    std::string errorBody;

    // Always empty -- shape parity with the other tool results.
    std::string bodyLang;
};

// Deletes the file or empty directory at `path` (relative paths
// resolve against ctx.cwd).  Refuses traversal escapes, unsafe
// names, risky extensions, and non-empty directories.  Returns a
// fully-populated DeleteResult on every path; never throws.
DeleteResult DeleteEntry(const std::string& path,
                         const ToolContext& ctx);
