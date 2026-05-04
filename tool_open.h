// tool_open.h
//
// Phase 2: Open / play / view files.
//
// Single tool that handles three semantically distinct dispositions
// based on the resolved file's classification:
//
//   TextLike  — file is text or code.  Returns content inline (same
//               shape as /read) so the model can answer questions
//               about the file without needing a separate read step.
//   Safe      — file is a media or document type that's safe to open
//               with the user's default application (audio, video,
//               image, PDF, Office docs, archives).  Launches via
//               ShellExecuteW with the default verb.
//   Risky     — file extension is on the executable / scriptable
//               kill-list (.exe, .bat, .ps1, .reg, .lnk, .vbs,
//               macro Office docs, etc.).  Returns a "blocked" tool
//               block; no launch.  The user can still open these
//               manually from File Explorer.
//
// ─── Resolution order ────────────────────────────────────────────
//  1. Try ResolveToolPath(input, ctx.cwd) directly.  If it lands on
//     an existing file or directory, use it.
//  2. Otherwise, fuzzy-match the input against basenames from the
//     most recent file-listing context in ChatHistory: native /ls or
//     a recognized PowerShell Get-ChildItem result.  Recursive filtered
//     Get-ChildItem searches keep relative paths, so "D: somewhere"
//     discovery can still open the nested file afterward.  This is what
//     lets the user (or model) say "open the eagles" after listing D:\Music
//     and have it resolve to "D:\Music\The Eagles - Hotel California.mp3".
//  3. If fuzzy-match returns multiple equally-good candidates, the
//     result reports them as an ambiguous list so the model can ask
//     the user which one.
//
// ─── Security posture ────────────────────────────────────────────
// Risky-file blocking is the mandatory floor.  The model never gets
// to bypass it via prompt — the classifier is local code, not part
// of the system prompt.  When risky files are blocked, the result
// body explicitly tells the model to ask the user to open it
// manually from File Explorer.  Phase 2b will add a click-to-confirm
// affordance on the rendered tool block; until then, blocked is
// blocked.
//
// ─── Threading ───────────────────────────────────────────────────
// Synchronous on the caller's thread.  ShellExecuteW must run on
// the GUI thread (some shell extension handlers assume STA / message
// pump); both call sites (slash handler on MyFrame and agent
// dispatcher) are already on the GUI thread when invoking, so this
// is satisfied.
//
#pragma once

#include "tool_context.h"

#include <string>
#include <vector>

class ChatHistory;

// ─── Risk classification ─────────────────────────────────────────
enum class FileRisk {
    TextLike,   // Render content inline; do not launch.
    Safe,       // Launch via ShellExecuteW (audio, video, doc, etc.).
    Risky,      // Block; do not launch (executable / scriptable).
};

// Classifies a path purely by extension.  Files with no extension or
// unknown extensions fall into Safe — but ShellExecuteW will simply
// fail for unknown types, so this is safe by accident.  Risky always
// wins ties: anything ending in a kill-list extension is Risky even
// if upper-cased or in a multi-extension chain.
FileRisk ClassifyForOpen(const std::string& path);

// Returns the language tag (for fenced code blocks) implied by a
// file's extension, or "" if not text-like.  Used both by tool_open
// itself (to set bodyLang on text reads) and by callers that want to
// label content consistently with /read.
std::string LanguageForExt(const std::string& path);

// ─── Listings collected from history ─────────────────────────────
// One entry per recent file-listing context. `directory` is resolved
// to an absolute path; `entries` are basenames with no trailing slash.
struct LsListing {
    std::string              directory;
    std::vector<std::string> entries;
};

// Walks `history` backward, parsing native `[tool: ls]` results and
// recognized `[tool: powershell]` Get-ChildItem results. Returns up to
// `maxListings` listings, most-recent-first. The default is 3 so /open
// can still resolve follow-up requests when another tool ran after the relevant listing. Each
// listing's directory is resolved against `fallbackCwd` so callers do
// not need to redo path resolution. Empty result means no recent file
// listing in history.
std::vector<LsListing> CollectRecentLsListings(
    const ChatHistory& history,
    const std::string& fallbackCwd,
    size_t             maxListings = 3);

// ─── Fuzzy matching ──────────────────────────────────────────────
// Tokenizes `query` (lowercase, whitespace-split).  Returns indices
// of candidates whose lowercase basename-without-extension contains
// EVERY query token as a substring.  Indices are sorted by stem
// length ascending (shortest = tightest match).  Empty result =
// nothing matched.  Multiple results = ambiguous; caller should
// surface the candidates so the user can disambiguate.
//
// Capped at 5 returns to keep the ambiguity message readable.
std::vector<size_t> FuzzyMatchBasenames(
    const std::string&              query,
    const std::vector<std::string>& candidates);

// ─── Result type ─────────────────────────────────────────────────
// Mirrors the chip / body / errorBody shape of ReadResult / LsResult
// so the dispatcher can pack it into a ChatDisplay::ToolBlock the
// same way as every other tool.
struct OpenResult {
    std::vector<std::string> chips;
    std::string              body;
    std::string              errorBody;
    std::string              bodyLang;
};

// ─── Main entry ──────────────────────────────────────────────────
// Resolves and dispatches a single open request.  If `recentListings`
// is empty the function still works — direct path resolution still
// applies; only the fuzzy-name path is unavailable.
//
// Side effects: on the Safe branch (and only that branch), the
// function calls ShellExecuteW to launch the file.  The result still
// describes what happened (chips, body) so the agent loop has a
// renderable tool block to feed back.
OpenResult OpenFile(const std::string&                   inputPath,
                    const ToolContext&                   ctx,
                    const std::vector<LsListing>&        recentListings);
