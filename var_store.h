// var_store.h
//
// RLM Phase A — the external-context "variable store".
//
// Design (agreed 20260810): the substrate is the filesystem, not a
// bespoke object registry.  A "variable" is a plain file in the
// conversation workspace's Vars\ lane, which means the model's query
// surface is the EXISTING tool suite (read / grep / ls / open /
// python_run_script) — no new tool names, no new parser contracts, no
// new vocabulary for small models to learn.  This subsystem's only
// jobs are:
//
//   1. Spool a large tool-result body to Vars\<tag>_<NNNN>[_<hint>].txt
//      instead of letting it flow whole into the model context.
//   2. Build the replacement "handle card" the model sees in history:
//      sentinel line, file pointer, size/line stats, a type-aware
//      SHAPE report (headings / declarations / CSV header), and a
//      head+tail preview.
//   3. Decide WHEN to demote (threshold), and when NOT to (errors are
//      never demoted; already-demoted cards are never re-demoted; a
//      whole-file `read` of an existing Vars\ spool points back at the
//      same file instead of copying it again — loop guard). Derived
//      outputs from grep/inspect/extract/command tools are always
//      spooled independently even when their echo names an input file.
//
// Critical invariant: demotion changes only what enters ChatHistory
// (and therefore the wire request).  The on-screen ToolBlock the user
// sees keeps the FULL body — display is not context.  Corollary: a
// conversation reloaded from disk re-renders the demoted card, and
// the full text lives in the Vars\ file, visible in Explorer.
//
// Wire-prefix stability: demotion happens at APPEND time, before the
// body is ever persisted or sent, so — unlike the build-time elision
// pass — earlier messages are never mutated afterward and llama-server
// prompt-cache prefix reuse survives across turns.
//
// This header is intentionally UI-framework-free (same convention as
// tool_read.h / tool_ls.h) so it can be exercised from a plain test
// harness.  All paths are UTF-8 std::string; conversion to UTF-16
// happens at the fstream/Win32 boundary via path_safety::Utf8ToWide.
//
#pragma once

#include <cstddef>
#include <string>

namespace varstore {

// ── Tuning ─────────────────────────────────────────────────────────
// Bodies at or below the threshold pass through untouched.  12 KiB is
// ~4K tokens at the 3-bytes/token constant ChatHistory's elision
// budget and context meter already share — big enough that ordinary
// command output, directory listings, and small file reads never
// demote; small enough that a pasted source file or long build log
// does.  Deliberately a compile-time default for Phase A; a Settings
// knob (and a per-endpoint override for small-ctx local models) is a
// later phase, same pattern as the elision constants.
struct DemotionConfig {
    size_t thresholdBytes  = 12 * 1024;  // demote strictly above this
    size_t headLines       = 14;         // preview: first N lines
    size_t tailLines       = 8;          // preview: last N lines
    size_t maxPreviewBytes = 2560;       // combined head+tail cap
    size_t maxShapeLines   = 24;         // shape report entry cap
    size_t maxShapeBytes   = 2048;       // shape report byte cap
};

// Sentinel that opens every handle card.  Also the re-demotion guard:
// a body that already starts with this line is never demoted again.
// Single source of truth — the card builder and the guard both read
// this constant, so they can never disagree.
extern const char* const kCardSentinel;   // "[LARGE OUTPUT -> stored as variable]"

// Name of the lane, relative to the conversation workspace (tool cwd).
// The card quotes paths as "Vars\<file>" so the model's relative-path
// tool calls resolve against ToolContext::cwd with zero extra plumbing.
extern const char* const kVarsLaneName;   // "Vars"

struct DemoteOutcome {
    bool        demoted = false;  // false → use the original body unchanged
    std::string relPath;          // "Vars\\cmd_0007.txt" — as quoted in the card
    std::string absPath;          // full spool path (empty when reusing a source
                                  //   file already inside Vars\ — see loop guard)
    std::string cardBody;         // replacement body for history round-trip
};

// Ensures <workspaceDirUtf8>\Vars exists (recursively — also covers a
// not-yet-created workspace).  Returns the absolute Vars path, or ""
// on failure.  Failure is always safe: callers fall back to injecting
// the original body, i.e. today's behaviour.
std::string EnsureVarsDir(const std::string& workspaceDirUtf8);

// Type-aware structure sketch so a model that has NOT read the text
// can decide where to look.  Dispatch on the extension of `nameHint`
// (a filename or tool echo; only the extension is consulted):
//   .md/.markdown        → heading lines with line numbers
//   code extensions      → declaration-looking lines with line numbers
//                          (.h .hpp .c .cc .cpp .cs .py .js .ts .ps1 .rs .java .go)
//   .csv/.tsv            → header row + record count
//   .json                → top-level keys (depth-1 scan, first ~kMaxShapeLines)
//   anything else        → "" (head/tail preview carries it)
// Output is capped by cfg.maxShapeLines / cfg.maxShapeBytes and is
// always valid UTF-8 given valid UTF-8 input.  Note the MECHANISM: the
// per-entry clamps are byte counts, so they are walked back to a UTF-8
// character boundary before anything is written.  Do not reintroduce a
// raw substr()/write() at a byte offset here -- the card ends up in
// ChatHistory, Poco emits non-ASCII verbatim, and llama-server rejects
// a malformed sequence by failing the entire request.
std::string BuildShapeReport(const std::string& text,
                             const std::string& nameHint,
                             const DemotionConfig& cfg = {});

// Builds a handle card for content that already lives at relPath
// (relative to the workspace).  Used by Phase B attachment routing,
// where the file was imported by the drop controller and nothing
// needs spooling.  `shapeHint` drives the type-aware shape section
// (pass the filename).  When utf16Normalized is true the card notes
// that the on-disk copy may not be UTF-8, steering the model toward
// python for exact reads.
std::string BuildHandleCard(const std::string& relPath,
                            const std::string& body,
                            const std::string& shapeHint,
                            const DemotionConfig& cfg = {},
                            bool reusedSameFile = false,
                            bool utf16Original = false);

// The main entry point, called at the tool-result → history seam.
//
//   toolTag       protocol tag, e.g. "cmd", "read", "python_run_script".
//                 Sanitized into the spool filename.
//   commandEcho   the tool card's echo line (a path for read, a command
//                 line for cmd/powershell).  Used for (a) the filename
//                 hint, (b) the read-of-a-Vars-file loop guard, and
//                 (c) shape-report type dispatch for `read` results.
//   body          the candidate body.  NEVER pass errorBody — error
//                 text is evidence the model must see whole (the same
//                 principle as the agent's error-preserving compaction).
//   workspaceDirUtf8  ToolContext::cwd (or the resolved equivalent).
//                 Empty → no demotion (unsaved conversation).
//
// Behaviour:
//   * body.size() <= cfg.thresholdBytes            → not demoted
//   * body already begins with kCardSentinel       → not demoted
//   * toolTag is `read` AND its echo path resolves to an existing file
//     inside the workspace (Vars\ spools and imported text attachments)
//     → demoted, but the card points at THAT file; nothing new is
//     written.  This is both the read→demote→read loop guard and the
//     no-duplicate rule for whole-file reads of imported text.  Derived
//     tool output (grep, *_inspect, *_extract, cmd, py, etc.) NEVER
//     reuses an input path from its echo; it is spooled to Vars instead.
//     A reused card references a LIVE file: if the model later edits it,
//     the card's preview describes the version read at demotion time.
//   * spool write fails at any point               → not demoted
//   * otherwise: body written verbatim (binary mode, byte-exact) to a
//     fresh Vars\<tag>_<NNNN>[_<hint>].txt (NNNN = first free index),
//     card built and returned
//
// Thread-safety: call from the UI thread only (both integration seams
// already are).  Index probing is existence-based, so two LlamaBoss
// windows demoting into the SAME conversation cannot collide silently —
// CreateFileW with CREATE_NEW semantics resolves the race by retrying
// the next index.
DemoteOutcome MaybeDemoteToolBody(const std::string& toolTag,
                                  const std::string& commandEcho,
                                  const std::string& body,
                                  const std::string& workspaceDirUtf8,
                                  const DemotionConfig& cfg = {});

}  // namespace varstore
