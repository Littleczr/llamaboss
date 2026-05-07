// tool_read.h
//
// Implementation of the /read slash command — Phase 3.
//
// ReadFile is intentionally UI-framework-free: it takes a path and a
// resolved ToolContext, and returns a ReadResult that's ready to be
// packed into a ChatDisplay::ToolBlock for rendering AND passed to
// ChatHistory::FormatToolBlockAsUserMessage for history round-trip.
// The handler on MyFrame does the plumbing; this file contains no
// wx includes so it's straightforward to exercise from the Phase 4
// agent harness.
//
// The read is synchronous.  Worst case is the 1 MiB content cap,
// which is ~ms on any SSD — blocking the UI thread briefly is
// simpler than threading and has no observable cost.
//
#pragma once

#include "tool_context.h"

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
};

// Reads the file at `inputPath` (resolved against ctx.cwd via
// ResolveToolPath).  Applies a 1 MiB body cap (with "truncated" chip)
// and a 64 MiB hard refusal (with "too large" chip).  Binary files
// are detected via a null-byte scan on the first 4 KiB and rendered
// as a 256-byte hex preview.
ReadResult ReadFile(const std::string& inputPath, const ToolContext& ctx);
