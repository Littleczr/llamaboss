// tool_notes.h
//
// NOTES.md — append-only personal notes file.
//
// Phase 1 of the cross-conversation memory layer. The user has a single
// markdown file at %USERPROFILE%\LlamaBoss\NOTES.md (created on first
// append) that holds facts, paths, preferences, and small workflows the
// agent should remember across conversations. Two global tools touch it, and two project tools use the active
// project context:
//
//   notes_read   — return the global NOTES.md contents inline.
//   notes_append — when no project is active, append one full entry to global
//                  NOTES.md. When a project is active, append the full entry
//                  to <Project>\Notes\NOTES.md and append only a compact
//                  pointer/index entry to global NOTES.md.
//   project_notes_read   — return active project Notes/NOTES.md.
//   project_notes_append — append directly to active project Notes/NOTES.md
//                          without a global pointer.
//
// Notes are NOT injected into the system prompt, ever.  This keeps the
// llama-server prefix cache stable (a key win for local models) and lets
// NOTES.md grow much larger than Hermes' MEMORY.md cap of ~2,200 chars.
// The cost is paid only on turns where the model decides to read.
//
// Threading: synchronous on the caller's thread.  File ops are fast
// (a few KB at most for the foreseeable future), and both call sites
// (slash handler — none in v1 — and the agent dispatcher in
// tool_router.cpp) are already on the GUI thread when invoking, matching
// every other sync tool here.
//
// Path safety: the tool ignores its `ctx.cwd`.  The target path is fixed
// at %USERPROFILE%\LlamaBoss\NOTES.md and is the only path either entry
// point will ever read or write.  The append body is treated as opaque
// markdown text — newlines and special characters are preserved verbatim
// after a single trailing-whitespace trim.
//
#pragma once

#include "tool_context.h"

#include <string>
#include <vector>

// Result type — mirrors the chip / body / errorBody shape used by every
// other sync tool (ReadResult, LsResult, OpenResult) so the dispatcher
// can pack it into a ChatDisplay::ToolBlock the same way.
struct NotesResult {
    std::vector<std::string> chips;
    std::string              body;
    std::string              errorBody;
    std::string              bodyLang;     // "markdown" for read, empty for append
};

// Returns the absolute path to the user's NOTES.md.  Used by both tool
// entry points and exposed for tests / future UI affordances (e.g. an
// "Open Notes in editor" menu item that ShellExecuteWs this path).
//
// Resolution order matches LlamaBossUserRootDir() in chat_history.cpp:
//   1. %USERPROFILE%\LlamaBoss\NOTES.md   (Windows, normal case)
//   2. ~/LlamaBoss/NOTES.md               (fallback if USERPROFILE unset)
//   3. <Documents>\LlamaBoss\NOTES.md     (last resort)
//
// The parent directory is NOT created by this call — it is created
// lazily by NotesAppend() on first write.  NotesRead() tolerates a
// missing file or directory and returns an empty result.
std::string GetNotesPath();

// Returns the active project notes path (<ProjectRoot>\Notes\NOTES.md),
// or an empty string when no project is attached to the current chat.
std::string GetProjectNotesPath(const ToolContext& ctx);

// Read the full contents of NOTES.md.  `ctx` is accepted for signature
// uniformity with the dispatcher; its fields are not read.
//
// Behavior:
//   - File missing or empty   → body="", chip="no notes yet".
//   - File present            → body=contents, bodyLang="markdown",
//                               chips include byte-size and line-count.
//   - Read error (permission, I/O) → errorBody set, "failed" chip.
NotesResult NotesRead(const ToolContext& ctx);

// Read the active project notes file. Missing/empty file is not an error.
// Fails clearly when there is no active project.
NotesResult ProjectNotesRead(const ToolContext& ctx);

// Append `entryText` to NOTES.md as a new dated entry.  The on-disk
// shape is:
//
//     ## YYYY-MM-DD HH:MM
//     <trimmed entryText>
//
//     <next entry...>
//
// One blank line separates entries; the timestamp header is always
// emitted so the file reads as a chronological log.  The entry text is
// trimmed at both ends but its internal newlines are preserved verbatim.
//
// Empty / whitespace-only `entryText` is rejected with errorBody set;
// the tool refuses to write a header with no body.
//
// The write is atomic via tool_staged_write::CreateStagedTempFile +
// MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH), matching SaveToFile in
// chat_history.cpp.  The parent directory is created on demand.
NotesResult NotesAppend(const std::string& entryText,
                        const ToolContext& ctx);

// Append directly to the active project notes file only. Unlike NotesAppend,
// this does not create a global NOTES.md pointer entry.
NotesResult ProjectNotesAppend(const std::string& entryText,
                               const ToolContext& ctx);
