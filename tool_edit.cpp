// tool_edit.cpp

#include "tool_edit.h"
#include "tool_path.h"
#include "tool_path_safety.h"   // IsUnderCwd, Basename
#include "tool_staged_write.h"  // StagedTempFile, CreateStagedTempFile
#include "tool_open.h"          // ClassifyForOpen, FileRisk
#include "path_safety.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {

// ─── Limits ──────────────────────────────────────────────────────
// kEditMaxBytes caps both the source file we agree to edit AND
// the post-edit content we'd write back.  Same shape as
// tool_write's 1 MiB ceiling.  Keeps any pathological "expand the
// file by 100x via repeated insertion" failure mode bounded.
constexpr size_t kEditMaxBytes      = 1 * 1024 * 1024;  // 1 MiB

// Diff context: lines on each side of the change shown as ' '
// prefixed context.  3 is the unified-diff convention.
constexpr size_t kDiffContextLines  = 3;

// Maximum lines of changed content (-/+) shown inline before we
// truncate with a "[... N more lines unchanged ...]" marker.
// Generous enough that most real edits show in full; tight enough
// that a 200-line replacement doesn't dominate the chat.
constexpr size_t kMaxChangedLines   = 20;

// Sentinel strings for the heredoc args grammar.  Triple-angle
// brackets are distinctive enough that accidental collision with
// real source code is negligible; if the worst happens, the
// "must be on its own line" rule narrows it further.
const std::string kOldSentinel = "<<<OLD>>>";
const std::string kNewSentinel = "<<<NEW>>>";

// ─── Helpers (file-local) ────────────────────────────────────────

std::string HumanBytes(size_t b)
{
    std::ostringstream ss;
    ss << std::fixed;
    if (b < 1024) {
        ss << b << " B";
    } else if (b < 1024 * 1024) {
        ss.precision(1);
        ss << (b / 1024.0) << " KB";
    } else {
        ss.precision(2);
        ss << (b / (1024.0 * 1024.0)) << " MB";
    }
    return ss.str();
}

std::string ElapsedChip(std::chrono::steady_clock::time_point t0)
{
    double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();
    std::ostringstream ts;
    ts << std::fixed;
    ts.precision(elapsed < 10.0 ? 2 : 1);
    ts << elapsed << "s";
    return ts.str();
}

// ─── Heredoc args parsing ────────────────────────────────────────
//
// Locate `needle` in `text` such that it starts at position 0 (or
// is preceded by '\n') AND ends at end-of-text (or is followed by
// '\n').  Returns position of needle, or npos if no on-line match
// exists at or after `start`.
size_t FindOnOwnLine(const std::string& text,
                     const std::string& needle,
                     size_t             start)
{
    size_t pos = start;
    while (pos <= text.size()) {
        size_t found = text.find(needle, pos);
        if (found == std::string::npos) return std::string::npos;

        const bool leftOk  = (found == 0) || (text[found - 1] == '\n');
        const size_t end   = found + needle.size();
        const bool rightOk = (end == text.size()) || (text[end] == '\n');

        if (leftOk && rightOk) return found;
        pos = found + 1;
    }
    return std::string::npos;
}

// Strip trailing whitespace from the path line.  Leading whitespace
// is left alone; if the model intentionally indented its path,
// that's a model-side bug, not something this tool covers up.
void RstripPath(std::string& s)
{
    while (!s.empty()) {
        char c = s.back();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') s.pop_back();
        else break;
    }
}

struct ParsedEditArgs {
    bool        ok = false;
    std::string path;
    std::string oldString;
    std::string newString;
    std::string error;
};

// Parse the args blob into (path, oldString, newString).  Returns
// `ok=false` with a human-readable error on any structural issue.
ParsedEditArgs ParseEditArgs(const std::string& args)
{
    ParsedEditArgs out;

    if (args.empty()) {
        out.error = "edit args are empty.";
        return out;
    }

    // ── Path on first line ───────────────────────────────────────
    size_t nl1 = args.find('\n');
    if (nl1 == std::string::npos) {
        out.error = "edit args must contain a path on the first "
                    "line, then <<<OLD>>>...<<<NEW>>>... blocks.";
        return out;
    }

    out.path = args.substr(0, nl1);
    RstripPath(out.path);
    if (out.path.empty()) {
        out.error = "edit requires a path on the first line of <args>.";
        return out;
    }

    // ── <<<OLD>>> sentinel on its own line ───────────────────────
    size_t oldPos = FindOnOwnLine(args, kOldSentinel, nl1 + 1);
    if (oldPos == std::string::npos) {
        out.error = "missing <<<OLD>>> sentinel. The grammar is:\n"
                    "  <args>path/to/file\n"
                    "  <<<OLD>>>\n"
                    "  text being replaced\n"
                    "  <<<NEW>>>\n"
                    "  text replacing it\n"
                    "  </args>";
        return out;
    }

    // After-the-sentinel-line position: skip the trailing '\n' of
    // the sentinel line.  If the sentinel is at the very end (no
    // following newline), there's no room for a <<<NEW>>>, so error.
    size_t oldSentinelEnd = oldPos + kOldSentinel.size();
    if (oldSentinelEnd >= args.size()) {
        out.error = "missing <<<NEW>>> sentinel after <<<OLD>>>.";
        return out;
    }
    if (args[oldSentinelEnd] != '\n') {
        out.error = "<<<OLD>>> must be on its own line.";
        return out;
    }
    size_t oldStart = oldSentinelEnd + 1;

    // ── <<<NEW>>> sentinel on its own line, after oldStart ───────
    size_t newPos = FindOnOwnLine(args, kNewSentinel, oldStart);
    if (newPos == std::string::npos) {
        out.error = "missing <<<NEW>>> sentinel.";
        return out;
    }

    // The newline that precedes <<<NEW>>> is the line-boundary of
    // the sentinel, not part of OLD.  Confirm and exclude it.
    if (newPos == 0 || args[newPos - 1] != '\n') {
        // Shouldn't happen because FindOnOwnLine guarantees left-
        // boundary, but defensive.
        out.error = "<<<NEW>>> must be on its own line.";
        return out;
    }
    out.oldString = args.substr(oldStart, (newPos - 1) - oldStart);

    // ── New string runs from after-the-newline-after-NEW to end ──
    size_t newSentinelEnd = newPos + kNewSentinel.size();
    if (newSentinelEnd == args.size()) {
        // <<<NEW>>> is the last thing in args -- new_string is empty
        // (delete OLD).  Allowed.
        out.newString.clear();
    } else if (args[newSentinelEnd] == '\n') {
        out.newString = args.substr(newSentinelEnd + 1);
    } else {
        out.error = "<<<NEW>>> must be on its own line.";
        return out;
    }

    // ── Empty OLD is rejected ────────────────────────────────────
    // Match-nothing has no sensible behaviour and a model that
    // wants to prepend can express it as "match the first line,
    // replace with prepended-text + first line."
    if (out.oldString.empty()) {
        out.error = "<<<OLD>>> block is empty. Provide the text "
                    "you want replaced, with enough surrounding "
                    "context that it appears exactly once in the "
                    "file. To prepend, include the file's first "
                    "line as OLD and your-prepended-text + that "
                    "line as NEW.";
        return out;
    }

    out.ok = true;
    return out;
}

// ─── Line-ending detection and normalization ─────────────────────

// Detected line-ending style for a file.  Lone-CR (classic Mac)
// is intentionally not represented -- it's vanishingly rare on
// any modern Windows codebase, and a file with lone CRs would be
// detected here as LF (no \r\n found, no CRLF dominance) which
// is a reasonable fallback.
enum class LineEnding {
    Lf,     // \n only
    Crlf    // \r\n
};

// Examine the first kSniffBytes of `content` and decide which
// ending style dominates.  Threshold: at least half of the \n
// characters in the sniff window are part of \r\n sequences.
LineEnding DetectLineEnding(const std::string& content)
{
    constexpr size_t kSniffBytes = 64 * 1024;
    const size_t n = std::min(content.size(), kSniffBytes);

    size_t lfCount   = 0;
    size_t crlfCount = 0;
    for (size_t i = 0; i < n; ++i) {
        if (content[i] == '\n') {
            ++lfCount;
            if (i > 0 && content[i - 1] == '\r') ++crlfCount;
        }
    }

    if (lfCount == 0) return LineEnding::Lf;       // no newlines at all
    return (crlfCount * 2 >= lfCount) ? LineEnding::Crlf
                                       : LineEnding::Lf;
}

// Convert all CRLF to LF in-place.  Lone CRs are left as-is --
// they're rare and changing them silently could damage data we
// don't understand.
std::string ToLf(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n') continue;
        out += s[i];
    }
    return out;
}

// Convert every \n to \r\n.  Used to restore CRLF style after
// substitution.  Idempotent on already-CRLF input would NOT be
// safe (it'd produce \r\r\n) but we always call this on
// LF-normalized text.
std::string ToCrlf(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + s.size() / 32);  // ~3% slack
    for (char c : s) {
        if (c == '\n') out += '\r';
        out += c;
    }
    return out;
}

// ─── Diff rendering ──────────────────────────────────────────────
//
// Produce a unified-diff-style snippet showing the change region
// with kDiffContextLines lines of context above and below.  Lines
// are split on '\n' with the convention that a trailing newline
// produces an empty last "line" we suppress.  Long changes are
// truncated symmetrically (head + tail, with a marker).
//
// Inputs are LF-only.  Output is LF-only -- the markdown renderer
// handles its own line breaks.
std::string MakeDiff(const std::string& fileLf,
                     size_t             matchPos,
                     size_t             oldLen,
                     const std::string& newStringLf)
{
    // Locate the line containing the match start.
    size_t lineStart = (matchPos == 0)
                           ? 0
                           : fileLf.rfind('\n', matchPos - 1);
    lineStart = (lineStart == std::string::npos) ? 0 : lineStart + 1;

    // Locate the line containing the match end.
    const size_t matchEnd = matchPos + oldLen;
    size_t lineEnd = fileLf.find('\n', matchEnd);
    if (lineEnd == std::string::npos) lineEnd = fileLf.size();
    else                              lineEnd = lineEnd;  // exclude '\n' for now

    // The OLD lines: full lines covering [lineStart, lineEnd).
    std::string oldLines = fileLf.substr(lineStart, lineEnd - lineStart);

    // The NEW lines: take the prefix of the file up to lineStart,
    // then everything from lineStart to matchPos (the part of the
    // first changed line BEFORE the match), then newStringLf, then
    // everything from matchEnd to lineEnd (the part of the last
    // changed line AFTER the match).
    std::string newLines =
        fileLf.substr(lineStart, matchPos - lineStart) +
        newStringLf +
        fileLf.substr(matchEnd, lineEnd - matchEnd);

    // Context above: up to kDiffContextLines lines preceding lineStart.
    std::string aboveCtx;
    {
        size_t end = lineStart;       // end is exclusive
        size_t ctxStart = end;
        for (size_t i = 0; i < kDiffContextLines; ++i) {
            if (ctxStart == 0) break;
            // step back over the trailing \n then back to the
            // start of the previous line
            size_t prev = (ctxStart >= 2) ? fileLf.rfind('\n', ctxStart - 2)
                                          : std::string::npos;
            ctxStart = (prev == std::string::npos) ? 0 : prev + 1;
        }
        if (ctxStart < end) {
            // strip the trailing '\n' that separates context from
            // the OLD block (we re-add per line below)
            size_t aboveLen = end - ctxStart;
            if (aboveLen > 0 && fileLf[end - 1] == '\n') --aboveLen;
            aboveCtx = fileLf.substr(ctxStart, aboveLen);
        }
    }

    // Context below: up to kDiffContextLines lines after lineEnd.
    std::string belowCtx;
    {
        size_t start = lineEnd;
        if (start < fileLf.size() && fileLf[start] == '\n') ++start;
        size_t ctxEnd = start;
        for (size_t i = 0; i < kDiffContextLines; ++i) {
            if (ctxEnd >= fileLf.size()) break;
            size_t nx = fileLf.find('\n', ctxEnd);
            ctxEnd = (nx == std::string::npos) ? fileLf.size()
                                                : nx + 1;
        }
        if (ctxEnd > start) {
            size_t belowLen = ctxEnd - start;
            if (belowLen > 0 && fileLf[ctxEnd - 1] == '\n') --belowLen;
            belowCtx = fileLf.substr(start, belowLen);
        }
    }

    // Helper: split a multi-line string on '\n' into a vector of
    // lines (no trailing empty entry from a final newline -- the
    // caller has already trimmed those).
    auto split = [](const std::string& s) {
        std::vector<std::string> v;
        if (s.empty()) return v;
        size_t i = 0;
        while (i <= s.size()) {
            size_t nl = s.find('\n', i);
            if (nl == std::string::npos) {
                v.push_back(s.substr(i));
                break;
            }
            v.push_back(s.substr(i, nl - i));
            i = nl + 1;
        }
        return v;
    };

    auto oldVec   = split(oldLines);
    auto newVec   = split(newLines);
    auto aboveVec = split(aboveCtx);
    auto belowVec = split(belowCtx);

    // Truncate runs longer than kMaxChangedLines symmetrically.
    auto cap = [](std::vector<std::string>& v) -> std::string {
        if (v.size() <= kMaxChangedLines) return {};
        const size_t head = kMaxChangedLines / 2;
        const size_t tail = kMaxChangedLines - head;
        std::ostringstream marker;
        marker << "[... " << (v.size() - head - tail)
               << " more lines unchanged ...]";
        std::string m = marker.str();
        std::vector<std::string> out;
        out.reserve(head + 1 + tail);
        for (size_t i = 0; i < head; ++i)         out.push_back(v[i]);
        out.push_back(m);
        for (size_t i = v.size() - tail; i < v.size(); ++i) out.push_back(v[i]);
        v.swap(out);
        return m;
    };
    cap(oldVec);
    cap(newVec);

    std::ostringstream diff;
    for (const auto& l : aboveVec) diff << ' ' << l << '\n';
    for (const auto& l : oldVec)   diff << '-' << l << '\n';
    for (const auto& l : newVec)   diff << '+' << l << '\n';
    for (const auto& l : belowVec) diff << ' ' << l << '\n';
    return diff.str();
}

// Count occurrences of `needle` in `hay`, capped at `cap` (we only
// need to know "0 / 1 / 2+", not the full count, except for the
// ambiguity message which benefits from a real count up to a small
// ceiling).
size_t CountOccurrences(const std::string& hay,
                        const std::string& needle,
                        size_t             cap)
{
    if (needle.empty()) return 0;       // defensive; caller rejects empty OLD
    size_t count = 0;
    size_t pos = 0;
    while (count < cap) {
        size_t found = hay.find(needle, pos);
        if (found == std::string::npos) break;
        ++count;
        pos = found + 1;                // overlapping matches counted once each
    }
    return count;
}

// Convert a 0-indexed line offset to a 1-indexed "line N" string,
// for the ambiguity message.  Walks the file once -- not great for
// many matches, but capped by the cap on CountOccurrences.
std::vector<size_t> LinesOfOccurrences(const std::string& hay,
                                       const std::string& needle,
                                       size_t             cap)
{
    std::vector<size_t> out;
    if (needle.empty()) return out;
    size_t pos = 0;
    while (out.size() < cap) {
        size_t found = hay.find(needle, pos);
        if (found == std::string::npos) break;
        // Count newlines up to `found` to get the 1-indexed line.
        size_t line = 1;
        for (size_t i = 0; i < found; ++i) if (hay[i] == '\n') ++line;
        out.push_back(line);
        pos = found + 1;
    }
    return out;
}

} // anonymous namespace

EditResult EditFile(const std::string& argsBlob,
                    const ToolContext& ctx)
{
    EditResult r;
    auto t0 = std::chrono::steady_clock::now();

    // ── Parse the heredoc args ───────────────────────────────────
    ParsedEditArgs parsed = ParseEditArgs(argsBlob);
    if (!parsed.ok) {
        r.chips.push_back("failed");
        r.errorBody = parsed.error;
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    // ── Resolve path + containment ───────────────────────────────
    if (ctx.cwd.empty()) {
        r.chips.push_back("blocked");
        r.errorBody = "No working directory set; refuse to edit.";
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    std::string resolved = ResolveToolPath(parsed.path, ctx.cwd);
    if (resolved.empty()) {
        r.chips.push_back("failed");
        r.errorBody = "Could not resolve path: " + parsed.path;
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    if (!tool_path_safety::IsUnderCwd(resolved, ctx.cwd)) {
        r.chips.push_back("blocked");
        r.errorBody = "Refuses to edit outside the working "
                      "directory.\n  resolved: " + resolved +
                      "\n  cwd:      " + ctx.cwd;
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    // ── Filename sanitization ────────────────────────────────────
    std::string basename  = tool_path_safety::Basename(resolved);
    std::string sanitized = path_safety::SanitizeFilename(basename, "");
    if (sanitized.empty() || sanitized != basename) {
        r.chips.push_back("blocked");
        r.errorBody = "Filename '" + basename + "' has characters or "
                      "a shape that aren't safe on Windows.";
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    // ── Risky-extension block ────────────────────────────────────
    // Same kill-list as write/open.  We don't edit .bat / .ps1 /
    // .exe / .reg / .lnk / .vbs / macro Office docs / etc.  If the
    // user wants to modify one of these, they can do it manually.
    if (ClassifyForOpen(resolved) == FileRisk::Risky) {
        r.chips.push_back("blocked");
        r.errorBody = "Refuses to edit files with executable or "
                      "scriptable extensions. If the user wants this, "
                      "they can edit the file manually.";
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    // ── File must exist and be a regular file ────────────────────
    if (!IsFile(resolved)) {
        r.chips.push_back("missing");
        if (IsDirectory(resolved)) {
            r.errorBody = "Path is a directory, not a file: " + resolved;
        } else {
            r.errorBody = "File does not exist: " + resolved +
                          ".\nUse write to create new files.";
        }
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    // ── Read the file ────────────────────────────────────────────
    std::wstring wPath = path_safety::Utf8ToWide(resolved);
    if (wPath.empty()) {
        r.chips.push_back("failed");
        r.errorBody = "Path conversion failed: " + resolved;
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    std::ifstream f(wPath, std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        r.chips.push_back("failed");
        r.errorBody = "Could not open file for reading: " + resolved;
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }
    std::streamsize fileSize = f.tellg();
    if (fileSize < 0) {
        r.chips.push_back("failed");
        r.errorBody = "Could not determine file size: " + resolved;
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }
    if ((size_t)fileSize > kEditMaxBytes) {
        r.chips.push_back("too large");
        r.errorBody = "File too large to edit: " + HumanBytes((size_t)fileSize) +
                      " (max " + HumanBytes(kEditMaxBytes) + ").";
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    std::string fileContent((size_t)fileSize, '\0');
    if (fileSize > 0) {
        f.seekg(0, std::ios::beg);
        f.read(&fileContent[0], fileSize);
        if (f.gcount() < fileSize) {
            fileContent.resize((size_t)f.gcount());
        }
    }
    f.close();

    // ── Detect dominant ending and normalize all sides to LF ─────
    LineEnding native = DetectLineEnding(fileContent);
    std::string fileLf = (native == LineEnding::Crlf) ? ToLf(fileContent)
                                                       : fileContent;
    std::string oldLf  = ToLf(parsed.oldString);
    std::string newLf  = ToLf(parsed.newString);

    // ── Strict-single-match check ────────────────────────────────
    // Cap the count at 16 to keep the ambiguity diagnostic readable
    // when OLD is a very common substring.
    const size_t kCountCap = 16;
    size_t hits = CountOccurrences(fileLf, oldLf, kCountCap);
    if (hits == 0) {
        r.chips.push_back("not found");
        r.errorBody = "OLD string not found in file: " + resolved +
                      ".\nThe match is byte-for-byte (with line "
                      "endings normalized to LF). If the file has "
                      "indentation or whitespace different from "
                      "your OLD block, copy the exact text from a "
                      "read of the file first.";
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }
    if (hits >= 2) {
        std::vector<size_t> lines = LinesOfOccurrences(fileLf, oldLf, kCountCap);
        std::ostringstream lineList;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (i > 0) lineList << ", ";
            lineList << lines[i];
        }
        r.chips.push_back("ambiguous");
        r.chips.push_back(std::to_string(hits) +
                          (hits == kCountCap ? "+x" : "x"));
        r.errorBody = "OLD string matches " +
                      std::to_string(hits) +
                      (hits == kCountCap ? " or more" : "") +
                      " places in " + resolved +
                      " (lines " + lineList.str() + ").\n"
                      "Add more surrounding context to your OLD "
                      "block so it identifies exactly one location.";
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    // ── Substitute ───────────────────────────────────────────────
    size_t matchPos = fileLf.find(oldLf);
    std::string editedLf =
        fileLf.substr(0, matchPos) +
        newLf +
        fileLf.substr(matchPos + oldLf.size());

    if (editedLf.size() > kEditMaxBytes) {
        r.chips.push_back("too large");
        r.errorBody = "Result of edit would exceed cap: " +
                      HumanBytes(editedLf.size()) +
                      " (max " + HumanBytes(kEditMaxBytes) + ").";
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    // ── Build diff body BEFORE we touch disk ─────────────────────
    // If the write fails we still want the user to see what change
    // we attempted; the diff is a function of the LF content we
    // already have in memory.
    std::string diffBody = MakeDiff(fileLf, matchPos, oldLf.size(), newLf);

    // Count plus/minus lines for the chip pair.  Walk the diff
    // body once -- cheap, and avoids a second split.
    size_t minusLines = 0;
    size_t plusLines  = 0;
    {
        size_t i = 0;
        while (i < diffBody.size()) {
            char c = diffBody[i];
            if      (c == '-') ++minusLines;
            else if (c == '+') ++plusLines;
            // skip to next line
            size_t nl = diffBody.find('\n', i);
            if (nl == std::string::npos) break;
            i = nl + 1;
        }
    }

    // ── Restore native line ending and write atomically ──────────
    std::string outputBytes = (native == LineEnding::Crlf)
                                  ? ToCrlf(editedLf)
                                  : editedLf;

    tool_staged_write::StagedTempFile tmp =
        tool_staged_write::CreateStagedTempFile(resolved);
    if (tmp.handle == INVALID_HANDLE_VALUE) {
        r.chips.push_back("failed");
        r.errorBody = "Could not create unique temp file near '" +
                      resolved + "' for writing (Win32 error " +
                      std::to_string(tmp.error) + ").";
        if (!tmp.path.empty()) {
            r.errorBody += "\nLast attempted temp path: " + tmp.path;
        }
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    std::string&  tmpPath = tmp.path;
    std::wstring& wTmp    = tmp.wPath;
    HANDLE        hFile   = tmp.handle;

    {
        const char* data    = outputBytes.data();
        size_t      remain  = outputBytes.size();
        while (remain > 0) {
            DWORD chunk    = (remain > 0x40000000U)
                                 ? 0x40000000U
                                 : static_cast<DWORD>(remain);
            DWORD written  = 0;
            BOOL  ok = ::WriteFile(hFile, data, chunk, &written, nullptr);
            if (!ok || written == 0) {
                DWORD err = ::GetLastError();
                ::CloseHandle(hFile);
                ::DeleteFileW(wTmp.c_str());
                r.chips.push_back("failed");
                r.errorBody = "WriteFile failed (Win32 error " +
                              std::to_string(err) + ").";
                r.chips.push_back(ElapsedChip(t0));
                return r;
            }
            data    += written;
            remain  -= written;
        }
    }

    if (!::CloseHandle(hFile)) {
        DWORD err = ::GetLastError();
        r.chips.push_back("failed");
        r.errorBody = "CloseHandle on tmp failed (Win32 error " +
                      std::to_string(err) +
                      "); tmp file preserved at: " + tmpPath;
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    // Atomic replace.  Unlike write, edit USES MOVEFILE_REPLACE_EXISTING
    // because overwriting the original is the entire point.  The
    // pre-checks above (file must exist, must be a regular file)
    // make this safe.
    BOOL movedOK = ::MoveFileExW(
        wTmp.c_str(),
        wPath.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);

    if (!movedOK) {
        DWORD err = ::GetLastError();
        r.chips.push_back("failed");
        r.errorBody = "Rename failed (Win32 error " +
                      std::to_string(err) +
                      ").\nThe edited content is preserved at: " +
                      tmpPath;
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    // ── Success ──────────────────────────────────────────────────
    r.chips.push_back("edited");
    r.chips.push_back(std::to_string(minusLines) + "-");
    r.chips.push_back(std::to_string(plusLines) + "+");
    r.body     = diffBody;
    r.bodyLang = "diff";
    r.chips.push_back(ElapsedChip(t0));
    return r;
}
