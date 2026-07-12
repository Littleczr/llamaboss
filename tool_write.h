// tool_write.h
//
// Phase 6: file creation -- write a brand-new file under the
// per-conversation tool CWD.
//
// WriteNewFile creates a NEW file at a path resolved against ctx.cwd.
// It refuses every overwrite, every escape from ctx.cwd, every risky
// executable / scriptable extension (use write_powershell_script for
// approved .ps1 project scripts), and every input whose basename
// does not survive path_safety::SanitizeFilename intact.  The model
// never gets to bypass these checks via prompt -- they live in this
// file, not in the system prompt.
//
// OverwriteFileContent shares every gate above, but atomically
// replaces an existing regular file instead of refusing it.
//
// ─── Args shape ──────────────────────────────────────────────────
// The tool is invoked with a single <args> blob whose first line
// is the path and whose remaining lines are file content:
//
//    <tool_call>
//    <name>write</name>
//    <args>notes/today.md
//    # Today's notes
//    - one
//    - two
//    </args>
//    </tool_call>
//
// The parser's Trim() strips the trailing newline that wraps the
// </args> tag onto its own line; this tool re-adds a single trailing
// newline if the content doesn't already have one. Lossless for any
// file that already ended in a newline (the conventional case).
//
// Empty content (path-only args) is allowed and creates a zero-byte
// file -- useful for marker / sentinel files.
//
// ─── Threading ───────────────────────────────────────────────────
// Synchronous on the caller's thread.  The local SSD write itself is
// sub-ms at our 1 MiB cap, so the bytes-to-disk path needs no
// threading.  The one non-trivial latency is the optional Python
// syntax pre-check (see below): for a .py/.pyw target it spawns a
// short-lived interpreter process, which dominates the call's wall
// time (interpreter startup, not I/O).  That subprocess is bounded by
// a 10s timeout and its output is drained on a helper thread so a
// chatty child can never deadlock against our wait.  Callers should
// therefore run write/overwrite_file off the UI thread (the agent
// loop already does) so a Python check can't stall the UI.
//
// ─── Atomicity ───────────────────────────────────────────────────
// In both modes the bytes are written to a unique sibling staging
// file first, then MoveFileEx'd onto the final name:
//
//   • write (create-new) renames WITHOUT REPLACE_EXISTING.  A crash
//     partway through leaves only the staging file behind for
//     diagnostics rather than a half-formed real file, and the rename
//     itself fails cleanly if the target was created in the race
//     window between check and rename.
//
//   • overwrite_file renames WITH MOVEFILE_REPLACE_EXISTING, so an
//     existing regular file is swapped for the fully-written staging
//     file in a single step -- readers see either the old file or the
//     new one, never a torn intermediate.  Directories are never
//     replaced; an existing directory at the target is refused.
//
// Staging files are opened with CREATE_NEW semantics so an existing
// user-owned .tmp file is never overwritten.
//
// ─── Python syntax pre-check ──────────────────────────────────────
// When the target is a .py/.pyw file, the staged content is run
// through `py_compile` before the rename.  The gate is deliberately
// narrow: only a recognized SyntaxError / IndentationError / TabError
// blocks the write (the staging file is deleted and the model gets the
// compiler message back to retry).  Anything else -- no interpreter on
// PATH, a py launcher with no usable 3.x runtime, a check that times
// out -- is treated as "unverified" and does NOT block the write; the
// later python_health / python_run_script step surfaces a genuine
// runtime problem instead of this tool conflating it with bad syntax.
//
#pragma once

#include "tool_context.h"

#include <cstddef>
#include <string>
#include <vector>

struct WriteResult {
    // Header chips: outcome + size + line-count + elapsed.
    //   Success new file:  ["created", "1.2 KB", "42 lines", "0.02s"]
    //   Success empty:     ["created", "empty",            "0.01s"]
    //   Fail exists:       ["exists",                       "0.00s"]
    //   Fail blocked:      ["blocked",                      "0.00s"]
    //   Fail other:        ["failed",                       "0.00s"]
    std::vector<std::string> chips;

    // One-line confirmation on success: the absolute resolved path
    // of the file we wrote, in a brief "Wrote N to PATH" sentence.
    // Empty when the write fails -- in that case errorBody carries
    // the diagnostic and chips["failed"|"blocked"|"exists"] explains
    // the category.
    std::string body;

    // Populated on any failure path.  The model uses this to decide
    // whether to retry, ask the user, or give up.
    std::string errorBody;

    // Always empty for write -- body is a confirmation sentence,
    // not source.  Carried for shape parity with ReadResult /
    // OpenResult so the dispatcher can pack uniformly.
    std::string bodyLang;

    // Success metadata used by the UI to present a clickable file chip.
    // Empty/zero when the write fails.
    std::string createdPath;
    std::string displayName;
    size_t      sizeBytes = 0;
    int         lineCount = 0;
};

// Creates a NEW file from `argsBlob`.  The first line of `argsBlob`
// is the path (relative paths resolve against ctx.cwd); everything
// after the first newline is the file content.  See tool_write.h
// header comment for the full args grammar and refusal rules.
//
// Never throws.  Every failure path returns a fully-populated
// WriteResult with chips, errorBody, and (on success only) body.
WriteResult WriteNewFile(const std::string& argsBlob,
                         const ToolContext& ctx);

// Creates or atomically replaces a text file from `argsBlob`.
// Same path/content grammar and safety gates as WriteNewFile, but
// existing regular files are replaced. Directories are never overwritten.
WriteResult OverwriteFileContent(const std::string& argsBlob,
                                 const ToolContext& ctx);

// Creates or atomically replaces a PowerShell .ps1 script after the
// caller has routed the invocation through the approval-card layer.
// This is intentionally separate from write/overwrite_file: ordinary
// file writes still block executable/scriptable extensions, while this
// approved path supports project build/run scripts without forcing the
// model to create them through ad-hoc PowerShell Set-Content commands.
//
// Safety gates are the same as overwrite_file (cwd/project containment,
// basename sanitization, parent must exist, directory targets refused),
// plus an explicit .ps1-only extension check. The file is written but
// never executed by this function.
WriteResult WritePowerShellScriptFile(const std::string& argsBlob,
                                      const ToolContext& ctx);
