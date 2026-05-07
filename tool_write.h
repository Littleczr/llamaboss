// tool_write.h
//
// Phase 6: file creation -- write a brand-new file under the
// per-conversation tool CWD.
//
// WriteFile creates a NEW file at a path resolved against ctx.cwd.
// It refuses every overwrite, every escape from ctx.cwd, every
// risky executable / scriptable extension, and every input whose
// basename does not survive path_safety::SanitizeFilename intact.
// The model never gets to bypass these checks via prompt -- they
// live in this file, not in the system prompt.
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
// Synchronous on the caller's thread.  Local SSD I/O at our cap
// (1 MiB) is sub-ms; threading buys nothing and adds complexity.
//
// ─── Atomicity ───────────────────────────────────────────────────
// The bytes are written to a unique sibling staging file first and
// then MoveFileEx'd onto the final name without REPLACE_EXISTING, so a
// crash partway through the write leaves only the staging file behind
// for diagnostics rather than a half-formed real file.  Staging files
// are opened with CREATE_NEW semantics so an existing user-owned .tmp
// file is never overwritten.  The final MoveFileEx also fails if the
// target was created in the race window between check and rename.
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
