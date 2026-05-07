// tool_ls.h
//
// Implementation of the /ls slash command — Phase 3.
//
// Mirrors tool_read.h: takes a path and a resolved ToolContext,
// returns an LsResult ready to be packed into a ChatDisplay::ToolBlock
// and passed to ChatHistory::FormatToolBlockAsUserMessage.  No wx
// includes so the Phase 4 agent harness can drive this identically
// to how the user does.
//
// Synchronous: directory enumeration on a local SSD completes in
// sub-millisecond time for normal dev dirs.  Network shares or
// very-deep trees will block the UI thread briefly; acceptable
// given the 500-entry cap.  Can be threaded later if it ever
// actually matters.
//
#pragma once

#include "tool_context.h"

#include <string>
#include <vector>

struct LsResult {
    // Chips for the header ("N entries", "truncated", and always a
    // trailing elapsed-time chip).
    std::vector<std::string> chips;

    // Formatted directory listing — aligned columns, monospace.
    // Empty on failure; populated with "(empty directory)" for
    // genuinely empty dirs.
    std::string body;

    // Populated on any failure path (path resolution, not-a-directory,
    // access denied).  When non-empty, body is empty and chips
    // include a "failed" indicator.
    std::string errorBody;

    // Always empty for /ls — listings are plain text, not a language.
    std::string bodyLang;
};

// Lists the directory at `inputPath` (resolved against ctx.cwd).
// Empty inputPath means "list ctx.cwd itself".  Listing is sorted
// dirs-first / then-files, both alphabetically case-insensitive.
LsResult ListDirectory(const std::string& inputPath, const ToolContext& ctx);
