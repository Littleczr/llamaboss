// tool_read.h
//
// Implementation of the read tool.
//
// ReadFile is intentionally UI-framework-free: it takes a path and a
// resolved ToolContext, and returns a ReadResult that's ready to be
// packed into a ChatDisplay::ToolBlock for rendering AND passed to
// ChatHistory::FormatToolBlockAsUserMessage for history round-trip.
// No wx includes, so the native test runner and the agent harness
// drive it directly.
//
// The read itself is synchronous; the router dispatches read on a
// worker thread (ToolWorkerExecutor), so even the 1 MiB content cap
// never blocks the UI.  The open tool's inline-text path calls it on
// the UI thread, where the cap keeps it to milliseconds.
//
#pragma once

#include "tool_context.h"

#include <cstddef>
#include <string>
#include <vector>

struct ReadResult {
    // Chips for the header ("1.2 KB", "42 lines", "binary", "truncated",
    // "too large", "failed", and always a trailing elapsed-time chip).
    std::vector<std::string> chips;

    // Primary content — file text for text files, xxd-style hex
    // preview for binaries, empty on failure.
    std::string body;

    // Populated on any failure path (path resolution, open, read,
    // size-refusal).  When non-empty, body is typically empty and
    // chips include a "failed"-like indicator.
    std::string errorBody;

    // Language hint for the fenced code block in the history
    // round-trip.  Inferred from extension; "" when unknown or
    // inappropriate (e.g. hex preview of a binary).
    std::string bodyLang;

    // Optional structural history policy for deliberately requested slices.
    // 0 means use the normal global var-store threshold. read_range sets a
    // context-aware value capped at 48 KiB; callers copy it into the unified
    // ToolInvocationResult instead of inferring policy from display text.
    //
    // For read_range this is now the SAME number as the output cap that
    // produced `body`, so body.size() <= historyInlineBudgetBytes always
    // holds on success and a ranged read can never demote to a Vars\ handle
    // card.  It is therefore an assertion the var store re-checks, not a
    // threshold that ever fires — an over-large slice is rejected up front
    // with a "narrow the range" error instead.
    size_t historyInlineBudgetBytes = 0;
};

// One requested 1-based inclusive line interval for ReadFileRanges.
// The public type keeps the router/controller boundary simple and makes
// multi-range calls straightforward to exercise in a small native harness.
struct ReadLineRange {
    size_t startLine = 0;
    size_t endLine   = 0;
};

// Reads the file at `inputPath` (resolved against ctx.cwd via
// ResolveToolPath).  Applies a 1 MiB body cap (with "truncated" chip)
// and a 64 MiB hard refusal (with "too large" chip).  Binary files
// are detected via a null-byte scan on the first 4 KiB and rendered
// as a 256-byte hex preview.
ReadResult ReadFile(const std::string& inputPath, const ToolContext& ctx);

// Reads only the first maxLines logical lines of a text file.  Same
// path resolution and binary handling as ReadFile, but intentionally
// keeps the body small for project setup/code inspection loops.
ReadResult ReadFileHead(const std::string& inputPath,
                        const ToolContext& ctx,
                        size_t maxLines);

// RLM Phase C: reads only lines [startLine, endLine] (1-based,
// inclusive) of a text file.  The slicing primitive for variable
// files — grep locates, read_range extracts the neighborhood.  Same
// path resolution, scripts-lane fallback, 64 MiB refusal, and binary
// handling as ReadFile, but the WHOLE file is scanned (no 128 KiB
// preview cap) so deep ranges are reachable.  A range may request at most
// 1000 lines; a larger span is rejected rather than silently shortened.
// Lines are returned
// verbatim — no line-number prefixes — so ranged output stays safe to
// feed into edit workflows; the chips carry the range and total.  (That
// verbatim guarantee is single-range only: ReadFileRanges labels each
// block, see below.)  A slice whose body would exceed the ranged budget
// is rejected outright rather than truncated or demoted.
ReadResult ReadFileRange(const std::string& inputPath,
                         const ToolContext& ctx,
                         size_t startLine,
                         size_t endLine);

// Reads several non-contiguous line intervals from one text file in a
// single pass.  Existing one-range body formatting stays byte-for-byte
// compatible: a single entry delegates to ReadFileRange.  Multi-range calls accept at
// most 20 strictly ordered, non-overlapping intervals.  Each interval and the
// combined requested span are limited to 1000 lines; oversized requests fail
// atomically rather than returning a silently shortened subset. Returned
// blocks are labelled ("[range N: lines A-B of T]") so evidence from separate
// physical locations cannot be confused — which deliberately trades away the
// single-range verbatim guarantee, so a multi-range body must NOT be pasted
// straight into an edit workflow.  The combined body is rejected atomically
// when it exceeds the ranged budget; nothing is silently truncated.
ReadResult ReadFileRanges(const std::string& inputPath,
                          const ToolContext& ctx,
                          const std::vector<ReadLineRange>& ranges);
