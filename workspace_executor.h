// workspace_executor.h
// Executes workspace actions (currently: file search).
//
// This module is the "do" layer — it runs after the classifier
// (action_classifier.h) has identified a workspace command.
//
// v1 scope:
//   - Filename search only (no content search)
//   - Case-insensitive substring matching against filenames
//   - Multi-term queries: a file matches if its name contains ANY term
//   - Recursive directory traversal
//   - Results capped at a configurable limit
//
// Requires C++17 for std::filesystem.

#pragma once

#include <string>
#include <vector>
#include <cstdint>

// ── A single matching file ───────────────────────────────────────
struct WorkspaceFileMatch
{
    std::string relativePath;   // path relative to workspace root (e.g. "reports/hours.xlsx")
    std::string fileName;       // just the filename (e.g. "hours.xlsx")
    std::uintmax_t fileSize;    // size in bytes (0 if unavailable)
};

// ── Result of a workspace search ─────────────────────────────────
struct WorkspaceSearchResult
{
    bool        success;            // true if the search ran without errors
    std::string errorMessage;       // non-empty if success == false
    std::vector<WorkspaceFileMatch> matches;
    int         totalFilesScanned;  // how many files were examined
};

// ── Execute a filename search in the workspace ───────────────────
//
//   query         : the raw search terms (e.g. "hours employee")
//   workspacePath : absolute path to the workspace folder
//   maxResults    : cap on number of matches returned (default 50)
//
// The query is split on whitespace into individual terms.
// A file matches if its filename (case-insensitive) contains ANY term.
// Results are sorted alphabetically by relative path.
WorkspaceSearchResult ExecuteWorkspaceSearch(
    const std::string& query,
    const std::string& workspacePath,
    int maxResults = 50);

// ── Format a file size for display ───────────────────────────────
// Returns a human-readable string like "3 KB", "1.2 MB", "847 bytes".
std::string FormatFileSize(std::uintmax_t bytes);

// ── Format a full search result for display in chat ──────────────
// Produces the multi-line string shown as a system message.
std::string FormatSearchResult(const WorkspaceSearchResult& result,
                               const std::string& query,
                               const std::string& workspacePath);

// ═════════════════════════════════════════════════════════════════
//  Save Chat
// ═════════════════════════════════════════════════════════════════

// ── A single chat message (Poco-free for testability) ────────────
struct ChatMessage
{
    std::string role;       // "user", "assistant", "system"
    std::string content;
    std::string model;      // which model produced this (assistant only)
};

// ── Result of a save-chat operation ──────────────────────────────
struct WorkspaceSaveChatResult
{
    bool        success;
    std::string errorMessage;   // non-empty if success == false
    std::string savedFilePath;  // absolute path to the written file
    std::string savedFileName;  // just the filename
    int         messageCount;   // how many messages were written
};

// ── Save the current conversation to a markdown file ─────────────
//
//   messages      : the conversation to save
//   workspacePath : absolute path to the workspace folder (created if absent)
//   customName    : optional user-chosen name (empty = auto-generate from timestamp)
//   primaryModel  : the main model name for the file header
//
// The file is written as markdown to workspacePath/<name>.md.
WorkspaceSaveChatResult ExecuteWorkspaceSaveChat(
    const std::vector<ChatMessage>& messages,
    const std::string& workspacePath,
    const std::string& customName = "",
    const std::string& primaryModel = "");

// ── Format a save-chat result for display in chat ────────────────
std::string FormatSaveChatResult(const WorkspaceSaveChatResult& result);
