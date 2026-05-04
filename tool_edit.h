// tool_edit.h
//
// Phase 7: edit an existing file by find/replace.
//
// EditFile reads a file, finds exactly one occurrence of OLD, replaces
// it with NEW, and writes the result back atomically.  The file must
// already exist (write is the create-new tool; edit is the modify
// tool -- they're complementary).  All the same containment,
// sanitization, risky-extension, and size-cap floors that tool_write
// enforces apply here.
//
// ─── Args grammar ────────────────────────────────────────────────
// Three pieces, encoded with heredoc-style sentinels inside a
// single <args> blob:
//
//    <tool_call>
//    <name>edit</name>
//    <args>relative/path/to/file.cpp
//    <<<OLD>>>
//    size_t kMaxFoo = 100;
//    <<<NEW>>>
//    size_t kMaxFoo = 200;
//    </args>
//    </tool_call>
//
// The first line of <args> is the path.  The literal text
// "<<<OLD>>>" on its own line introduces OLD.  The literal text
// "<<<NEW>>>" on its own line introduces NEW.  OLD runs from the
// line after <<<OLD>>> to the line before <<<NEW>>>; NEW runs from
// the line after <<<NEW>>> to the end of <args>.  Both are treated
// as opaque byte sequences -- whatever the model puts between the
// sentinels is what we use, including embedded newlines.
//
// "On its own line" means the sentinel is the entire line: the
// preceding character is '\n' (or the sentinel is at the start of
// args after the path), and the following character is '\n' (or
// the sentinel is at the end of args).  This rules out accidental
// matches against text that happens to contain "<<<OLD>>>" inline.
//
// Empty NEW is allowed and means "delete OLD."  Empty OLD is
// rejected -- "match nothing" has no defensible behavior, and a
// model that wants to prepend can include the first line of the
// file as OLD and the prepended text plus that line as NEW.
//
// ─── Match policy ────────────────────────────────────────────────
// Strict single match.  After line-ending normalization, OLD must
// appear in the file exactly once.
//   0 occurrences  → error: "old_string not found"
//   1 occurrence   → edit, succeed
//   2+ occurrences → error: "old_string appears N times; provide
//                    more surrounding context to make the match
//                    unique"
//
// This forces the model to pick OLD strings with enough context
// that the change is deterministic.  Verbose, but every edit is
// auditable from a single tool_call block.
//
// ─── Line endings ────────────────────────────────────────────────
// File on disk may be CRLF (the Windows convention -- and the
// shape of every existing source file in the LlamaBoss tree),
// LF-only, or mixed.  The model emits LF.  Naive matching breaks
// every multi-line edit on a CRLF file.
//
// Strategy: detect the file's dominant line ending by counting
// CRLF sequences vs lone LFs.  Normalize file content, OLD, and
// NEW to LF for matching.  After substitution, convert the result
// back to the file's native ending before writing.  Files with
// mixed endings get the dominant ending applied uniformly -- a
// tradeoff that prefers internal consistency over perfect
// preservation of pre-existing inconsistency.
//
// ─── Atomicity ───────────────────────────────────────────────────
// Same unique sibling staging-file + MoveFileExW pattern as
// tool_write.  Crash partway through the write leaves only the staging
// file behind for inspection; the real file is unchanged until the
// rename succeeds.  Staging files are opened with CREATE_NEW semantics
// so an existing user-owned .tmp file is never overwritten.  Unlike
// write, the final rename here uses MOVEFILE_REPLACE_EXISTING because
// we WANT to overwrite -- that's the whole point.
//
// ─── Diff body ───────────────────────────────────────────────────
// The body is a unified-diff-style snippet showing the change with
// up to 3 lines of context above and below.  bodyLang is "diff" so
// the markdown renderer's diff highlighting picks it up.  Capped
// at ~20 lines visible; longer changes get a "[... N more lines
// unchanged ...]" truncation marker.
//
#pragma once

#include "tool_context.h"

#include <string>
#include <vector>

struct EditResult {
    // Header chips.  On success: ["edited", "1-", "1+", "0.02s"].
    // On any failure: an outcome chip plus elapsed.
    //   ["edited",   "N-", "M+",   "0.02s"]   success
    //   ["not found",                "0.00s"]  OLD wasn't in file
    //   ["ambiguous", "Nx",          "0.00s"]  OLD matched N>=2 times
    //   ["blocked",                   "0.00s"]  containment / risky / sanitize
    //   ["exists",                    "0.00s"]  file is missing (we don't create)
    //                                            -- chip text is "missing", not "exists"
    //   ["too large",                 "0.00s"]  file or new_size > cap
    //   ["failed",                    "0.00s"]  parse / I/O / Win32
    std::vector<std::string> chips;

    // Unified diff snippet on success; failure diagnostic-style on
    // failure paths that use body for human-readable context (e.g.
    // ambiguous match shows the line numbers of every hit).  Most
    // failure paths leave body empty and put the message in
    // errorBody.
    std::string body;

    // Populated on any failure path.  The model uses this to
    // decide whether to retry, ask the user, or give up.
    std::string errorBody;

    // "diff" on success, "" on failure.  The dispatcher copies it
    // straight into the ToolBlock so the renderer can apply diff
    // highlighting where appropriate.
    std::string bodyLang;
};

// Parses the heredoc args, applies all safety floors, performs
// the edit if everything checks out.  Never throws.  Every path
// produces a fully-populated EditResult.
EditResult EditFile(const std::string& argsBlob,
                    const ToolContext& ctx);
