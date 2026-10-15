// tool_read.cpp

#include "tool_read.h"
#include "lb_string_utils.h"
#include "lb_ascii.h"
#include "tool_path.h"
#include "tool_path_safety.h"
#include "path_safety.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_map>

namespace {

// ─── Limits ──────────────────────────────────────────────────────
// kReadRefuseAbove is the hard no-go point — we refuse to even open
// a file past this, to avoid surprising ballooning of memory when a
// user /reads a multi-GB blob.  Independent of model context size.
//
// The body cap itself is computed dynamically per-invocation by
// ComputeReadCap() below, so it scales with the model's ctx setting.
// On 8K ctx you get ~15 KB; on 128K you get ~375 KB.  This prevents
// the "read overflowed the model's context window" failure mode that
// a fixed 1 MiB cap causes on small-ctx local models.
constexpr size_t kReadRefuseAbove   = 64 * 1024 * 1024;  // 64 MiB

// Binary-detection window — git's rule of thumb.  If any NUL byte
// appears in the first N bytes, we treat the whole file as binary.
constexpr size_t kBinarySniffBytes  = 4096;
constexpr size_t kHexPreviewBytes   = 256;   // 16 rows of 16 bytes

// Compute the effective body cap for a /read given the active model's
// context size.  The guiding principle: reads should fit comfortably
// in-context alongside system prompt, chat template, a few turns of
// prior history, and the assistant's eventual output.
//
// Reserves 3000 tokens for all that overhead, then allots the
// remaining token budget to file body at a conservative 3 chars per
// token (source code averages 3–4 chars/token; we err on the side
// that produces a SMALLER cap, so the model doesn't hit a 400 at
// send time).  Floored at 4 KB (always read *something*), ceilinged
// at 512 KB (past that wxRichTextCtrl gets sluggish regardless of
// ctx).
size_t ComputeReadCap(int ctxTokens)
{
    constexpr int    kReservedTokens = 3000;
    constexpr size_t kFloor          =   4 * 1024;
    constexpr size_t kCeiling        = 512 * 1024;

    int usable = (ctxTokens > kReservedTokens)
                 ? (ctxTokens - kReservedTokens) : 0;
    size_t cap = (size_t)usable * 3;  // ~3 chars/token

    if (cap < kFloor)   cap = kFloor;
    if (cap > kCeiling) cap = kCeiling;
    return cap;
}

// The ONE limit that governs a deliberately requested read_range slice:
// it is both the output cap enforced below and the history-inline budget
// reported to the var store.
//
// They must be the same number.  If output were capped higher than the
// history-demotion threshold, anything landing in the gap would get
// neither the lines nor an actionable error: the var store would spool
// the slice to a fresh Vars\ file and hand back a handle card, so the
// model would have to issue a SECOND read_range against a copy of the
// thing it just asked for, with line numbers that no longer match the
// source file.
//
// One limit makes the body provably <= the budget, so a ranged read can
// never demote, and an over-large request comes back as "narrow the
// range or split the call" — one round trip either way, but the error
// says what to do.  ComputeRangedReadHistoryBudget is therefore an
// assertion more than a policy knob; the field name is kept
// (tool_dispatcher.h, the JSON case runner).
//
// The fixed ceiling keeps a 262K model from retaining 512 KiB per slice
// permanently; the context-aware half keeps small-context endpoints safe.
//
// Boundary note: the combined multi-range span caps at 1000 lines, which
// at ~50 bytes/line lands right around 48 KiB.  A 1000-line request over
// long-line content (minified JS, wide CSV rows, log lines) is rejected
// rather than demoted.  If that shows up in practice, raise
// kRangedReadInlineCeiling here — it is the single place that decides.
size_t ComputeRangedReadHistoryBudget(int ctxTokens)
{
    constexpr size_t kRangedReadInlineCeiling = 48 * 1024;
    return std::min(kRangedReadInlineCeiling, ComputeReadCap(ctxTokens));
}

// ─── Helpers ─────────────────────────────────────────────────────

// A byte-budget cut lands on an arbitrary byte.  Drop a trailing
// INCOMPLETE UTF-8 sequence so a partial body stays valid UTF-8.  A split
// character otherwise reaches the tool card (wxString::FromUTF8 renders
// invalid input as ""), the Vars\ spool, and -- whenever the body stays
// under the demotion threshold -- the wire, where llama-server rejects
// the entire request.  ASCII tails and malformed input are left as-is;
// at most 3 bytes are ever removed.
void DropIncompleteUtf8Tail(std::string& s)
{
    size_t j = s.size();
    while (j > 0 && ((unsigned char)s[j - 1] & 0xC0) == 0x80) --j;
    if (j == 0) return;
    const unsigned char lead = (unsigned char)s[j - 1];
    size_t seqLen = 0;
    if      ((lead & 0xE0) == 0xC0) seqLen = 2;
    else if ((lead & 0xF0) == 0xE0) seqLen = 3;
    else if ((lead & 0xF8) == 0xF0) seqLen = 4;
    if (seqLen > 0 && (j - 1) + seqLen > s.size())
        s.resize(j - 1);
}

// Any NUL byte in the sniff window ⇒ binary.  This misses UTF-16
// text (which has NULs by design) but that's rare on Windows dev
// workflows; the agent can always /cmd Get-Content as a fallback.
bool IsBinary(const char* data, size_t n)
{
    size_t check = std::min(n, kBinarySniffBytes);
    for (size_t i = 0; i < check; ++i)
        if (data[i] == '\0') return true;
    return false;
}

// xxd-style hex+ASCII preview:
//   00000000  89 50 4e 47 0d 0a 1a 0a  00 00 00 0d 49 48 44 52  |.PNG........IHDR|
std::string HexPreview(const std::string& data)
{
    std::ostringstream ss;
    size_t n = std::min(data.size(), kHexPreviewBytes);
    for (size_t row = 0; row < n; row += 16) {
        // Offset
        ss << std::hex << std::setw(8) << std::setfill('0') << row << "  ";
        // Hex bytes (16 per row, with a gap between the two halves)
        for (size_t col = 0; col < 16; ++col) {
            if (col == 8) ss << " ";
            if (row + col < n) {
                unsigned b = (unsigned char)data[row + col];
                ss << std::setw(2) << std::setfill('0') << b << " ";
            } else {
                ss << "   ";
            }
        }
        // ASCII gutter
        ss << " |";
        for (size_t col = 0; col < 16 && row + col < n; ++col) {
            unsigned char c = (unsigned char)data[row + col];
            ss << (char)((c >= 0x20 && c < 0x7F) ? c : '.');
        }
        ss << "|\n";
    }
    return ss.str();
}

// Fence hint inferred from extension.  Empty string for unknown
// extensions — the fence still works, just without syntax hint.
// Kept in one place so we can expand it later without threading the
// map through every caller.
std::string InferBodyLang(const std::string& absPath)
{
    size_t dot = absPath.rfind('.');
    if (dot == std::string::npos) return "";
    std::string ext = absPath.substr(dot + 1);
    for (char& c : ext) c = (char)lb_ascii::ToLower((unsigned char)c);

    static const std::unordered_map<std::string, std::string> kLangByExt = {
        { "cpp", "cpp" }, { "cc", "cpp" }, { "cxx", "cpp" },
        { "h",   "cpp" }, { "hpp","cpp" }, { "hh",  "cpp" },
        { "c",   "c" },
        { "py",  "python" },
        { "js",  "javascript" }, { "mjs", "javascript" },
        { "ts",  "typescript" },
        { "json","json" },
        { "md",  "markdown" },
        { "html","html" },       { "htm", "html" },
        { "css", "css" },
        { "sh",  "bash" },
        { "ps1", "powershell" }, { "psm1","powershell" },
        { "xml", "xml" },
        { "yaml","yaml" },       { "yml", "yaml" },
        { "toml","toml" },
        { "sql", "sql" },
        { "rs",  "rust" },
        { "go",  "go" },
        { "java","java" },
        { "rb",  "ruby" },
        { "php", "php" },
        { "ini", "ini" },
        { "bat", "batch" },      { "cmd", "batch" },
    };
    auto it = kLangByExt.find(ext);
    return (it == kLangByExt.end()) ? std::string() : it->second;
}

// Count of logical lines — a trailing non-newline line counts as one.
size_t CountLines(const std::string& s)
{
    if (s.empty()) return 0;
    size_t lines = 0;
    for (char c : s) if (c == '\n') ++lines;
    if (s.back() != '\n') ++lines;
    return lines;
}

} // anonymous namespace

namespace {

// Conversation Scripts-lane fallback for read: python_create_script puts
// runnable .py artifacts in the conversation Scripts folder — a SIBLING
// of the default Workspace cwd — and models routinely try to read their
// own script back with `read name.py` right after creating or running
// it.  The cwd-resolved path then misses, costing one failed tool step
// plus a full-path retry.  Reading is side-effect free, so resolve the
// miss instead of coaching it — under strict conditions so /cd'ed
// workspaces and projects are unaffected:
//   * the requested path is a bare filename or the explicit lane form
//     Scripts\name / Scripts/name (no other separators, no "..");
//   * the cwd basename is "Workspace" (the conversation default; a
//     /cd'ed cwd has no meaningful Scripts sibling);
//   * the cwd-resolved path does not exist, so the Workspace always
//     wins on a name collision;
//   * the sibling Scripts file actually exists.
// Returns the resolved Scripts-lane path, or empty when any condition
// fails.
std::string TryResolveConversationScriptsFallback(const std::string& inputPath,
                                                  const ToolContext& ctx)
{
    if (ctx.cwd.empty()) return std::string();

    auto isSep = [](char c) { return c == '\\' || c == '/'; };

    // Strip an optional explicit "Scripts\" / "Scripts/" lane prefix.
    std::string name = inputPath;
    if (name.size() > 8 &&
        (name.compare(0, 8, "Scripts\\") == 0 ||
         name.compare(0, 8, "Scripts/") == 0)) {
        name = name.substr(8);
    }

    if (name.empty()) return std::string();
    for (char c : name) {
        if (isSep(c)) return std::string();   // nested paths: not eligible
    }
    if (name.find("..") != std::string::npos) return std::string();

    // cwd basename must be the conversation default "Workspace".
    size_t cut = ctx.cwd.find_last_not_of("\\/");
    if (cut == std::string::npos) return std::string();
    std::string trimmedCwd = ctx.cwd.substr(0, cut + 1);

    size_t lastSep = trimmedCwd.find_last_of("\\/");
    if (lastSep == std::string::npos) return std::string();
    if (trimmedCwd.substr(lastSep + 1) != "Workspace") return std::string();

    std::string scriptsPath =
        trimmedCwd.substr(0, lastSep + 1) + "Scripts\\" + name;
    if (!IsFile(scriptsPath)) return std::string();
    return scriptsPath;
}

} // anonymous namespace

ReadResult ReadFile(const std::string& inputPath, const ToolContext& ctx)
{
    ReadResult r;
    auto t0 = std::chrono::steady_clock::now();

    // ── Path resolution ──────────────────────────────────────────
    bool usedConversationLane = false;
    std::string resolved = tool_path_safety::ResolveReadOnlyToolPath(
        inputPath, ctx.cwd, ctx.activeProjectRoot, &usedConversationLane);
    if (resolved.empty()) {
        r.chips.push_back("failed");
        r.errorBody = "Could not resolve path: " + inputPath;
        r.chips.push_back(LbElapsedChip(t0));
        return r;
    }
    if (usedConversationLane) r.chips.push_back("conversation lane");

    // ── Existence / type check ───────────────────────────────────
    if (!IsFile(resolved)) {
        // Side-effect-free fallback: bare filenames that miss the
        // Workspace but exist in the conversation Scripts lane resolve
        // there (see TryResolveConversationScriptsFallback).
        std::string scriptsFallback;
        if (!IsDirectory(resolved)) {
            scriptsFallback =
                TryResolveConversationScriptsFallback(inputPath, ctx);
        }
        if (!scriptsFallback.empty()) {
            resolved = scriptsFallback;
            r.chips.push_back("scripts lane");
        } else {
            r.chips.push_back("failed");
            if (IsDirectory(resolved)) {
                r.errorBody = "Not a file (is a directory): " + resolved;
            } else {
                r.errorBody = "File not found: " + resolved;
            }
            r.chips.push_back(LbElapsedChip(t0));
            return r;
        }
    }

    // ── Size check (ate + tellg) ─────────────────────────────────
    std::ifstream f(std::filesystem::path(path_safety::Utf8ToWide(resolved)), std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        r.chips.push_back("failed");
        r.errorBody = "Could not open file: " + resolved;
        r.chips.push_back(LbElapsedChip(t0));
        return r;
    }

    std::streamsize fileSize = f.tellg();
    if (fileSize < 0) {
        r.chips.push_back("failed");
        r.errorBody = "Could not determine file size: " + resolved;
        r.chips.push_back(LbElapsedChip(t0));
        return r;
    }

    const size_t totalBytes = (size_t)fileSize;

    if (totalBytes > kReadRefuseAbove) {
        r.chips.push_back(LbToolHumanBytes(totalBytes));
        r.chips.push_back("too large");
        r.errorBody = "File too large to read: " + LbToolHumanBytes(totalBytes)
                      + " (max " + LbToolHumanBytes(kReadRefuseAbove) + ")";
        r.chips.push_back(LbElapsedChip(t0));
        return r;
    }

    // ── Bounded read ─────────────────────────────────────────────
    // Cap scales with the active model's context size, via
    // ComputeReadCap().  This is what prevents /read of a 40 KB source
    // file from wedging an 8 KB-ctx model with a 400 at the next send.
    const size_t readCap  = ComputeReadCap(ctx.ctxTokens);
    const bool truncated  = totalBytes > readCap;
    const size_t toRead   = truncated ? readCap : totalBytes;

    f.seekg(0, std::ios::beg);
    std::string content(toRead, '\0');
    if (toRead > 0) {
        f.read(&content[0], (std::streamsize)toRead);
        // Short reads are legal (e.g. encoding transforms); trim to what
        // we actually got so body.size() always matches body.length().
        if (f.gcount() < (std::streamsize)toRead) {
            content.resize((size_t)f.gcount());
        }
    }
    if (truncated) DropIncompleteUtf8Tail(content);

    // Count the visible file lines BEFORE appending our synthetic
    // truncation marker.  Otherwise a truncated read reports a line
    // count that includes the marker itself.
    const size_t shownLineCount = CountLines(content);

    // If truncated, append an explicit marker to the body so the
    // model doesn't silently reason about a partial file as if it
    // were whole.  Cheap to include (<100 bytes) and enormously
    // helpful for agent workflows.
    if (truncated) {
        if (!content.empty() && content.back() != '\n') content += '\n';
        std::ostringstream marker;
        marker << "\n[... truncated: showing " << LbToolHumanBytes(content.size())
               << " of " << LbToolHumanBytes(totalBytes)
               << " to fit model context (ctx=" << ctx.ctxTokens
               << " tokens) ...]\n";
        content += marker.str();
    }

    // ── Binary branch ────────────────────────────────────────────
    if (IsBinary(content.data(), content.size())) {
        r.chips.push_back(LbToolHumanBytes(totalBytes));
        r.chips.push_back("binary");
        r.chips.push_back("hex preview");
        r.body = HexPreview(content);
        // No language hint for hex — it's not a real source dump.
        r.chips.push_back(LbElapsedChip(t0));
        return r;
    }

    // ── Text branch ──────────────────────────────────────────────
    r.chips.push_back(LbToolHumanBytes(totalBytes));
    r.chips.push_back(std::to_string(shownLineCount) +
                      (truncated ? " shown lines" : " lines"));
    if (truncated) r.chips.push_back("truncated");
    // A UTF-8 BOM is file metadata, not text: shown inline it became an
    // invisible character glued to line 1 that a model could copy into
    // an edit's OLD block.  Strip it from the view and say so.
    if (content.size() >= 3 && content.compare(0, 3, "\xEF\xBB\xBF") == 0) {
        content.erase(0, 3);
        r.chips.push_back("UTF-8 BOM");
    }
    // An empty body rendered as "(no output)", which reads like a tool
    // failure.  Say plainly that the file is empty.
    if (totalBytes == 0) content = "(empty file: 0 bytes)\n";
    r.body     = std::move(content);
    r.bodyLang = InferBodyLang(resolved);

    // Instruction files (a Skill's SKILL.md, a project WORKFLOW.md) are meant
    // to be read whole.  Past the global 12 KiB demotion threshold they used
    // to come back as a Vars\ handle card, so every Skill use cost an extra
    // read_range round trip -- or the model read only part of the rules.
    // Give them the same context-aware inline budget as read_range.
    if (!truncated) {
        std::string base = tool_path_safety::Basename(resolved);
        for (char& c : base)
            c = static_cast<char>(lb_ascii::ToLower(static_cast<unsigned char>(c)));
        if (base == "skill.md" || base == "workflow.md")
            r.historyInlineBudgetBytes =
                ComputeRangedReadHistoryBudget(ctx.ctxTokens);
    }

    r.chips.push_back(LbElapsedChip(t0));
    return r;
}


ReadResult ReadFileHead(const std::string& inputPath,
                        const ToolContext& ctx,
                        size_t maxLines)
{
    ReadResult r;
    auto t0 = std::chrono::steady_clock::now();

    if (maxLines == 0) maxLines = 40;
    if (maxLines > 500) maxLines = 500;

    bool usedConversationLane = false;
    std::string resolved = tool_path_safety::ResolveReadOnlyToolPath(
        inputPath, ctx.cwd, ctx.activeProjectRoot, &usedConversationLane);
    if (resolved.empty()) {
        r.chips.push_back("failed");
        r.errorBody = "Could not resolve path: " + inputPath;
        r.chips.push_back(LbElapsedChip(t0));
        return r;
    }
    if (usedConversationLane) r.chips.push_back("conversation lane");

    if (!IsFile(resolved)) {
        // Side-effect-free fallback: bare filenames that miss the
        // Workspace but exist in the conversation Scripts lane resolve
        // there (see TryResolveConversationScriptsFallback).
        std::string scriptsFallback;
        if (!IsDirectory(resolved)) {
            scriptsFallback =
                TryResolveConversationScriptsFallback(inputPath, ctx);
        }
        if (!scriptsFallback.empty()) {
            resolved = scriptsFallback;
            r.chips.push_back("scripts lane");
        } else {
            r.chips.push_back("failed");
            if (IsDirectory(resolved)) {
                r.errorBody = "Not a file (is a directory): " + resolved;
            } else {
                r.errorBody = "File not found: " + resolved;
            }
            r.chips.push_back(LbElapsedChip(t0));
            return r;
        }
    }

    std::ifstream f(std::filesystem::path(path_safety::Utf8ToWide(resolved)), std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        r.chips.push_back("failed");
        r.errorBody = "Could not open file: " + resolved;
        r.chips.push_back(LbElapsedChip(t0));
        return r;
    }

    std::streamsize fileSize = f.tellg();
    if (fileSize < 0) {
        r.chips.push_back("failed");
        r.errorBody = "Could not determine file size: " + resolved;
        r.chips.push_back(LbElapsedChip(t0));
        return r;
    }

    const size_t totalBytes = (size_t)fileSize;
    if (totalBytes > kReadRefuseAbove) {
        r.chips.push_back(LbToolHumanBytes(totalBytes));
        r.chips.push_back("too large");
        r.errorBody = "File too large to preview: " + LbToolHumanBytes(totalBytes)
                      + " (max " + LbToolHumanBytes(kReadRefuseAbove) + ")";
        r.chips.push_back(LbElapsedChip(t0));
        return r;
    }

    const size_t previewCap = 128 * 1024;
    const size_t toRead = std::min(totalBytes, previewCap);
    f.seekg(0, std::ios::beg);
    std::string content(toRead, '\0');
    if (toRead > 0) {
        f.read(&content[0], (std::streamsize)toRead);
        if (f.gcount() < (std::streamsize)toRead) {
            content.resize((size_t)f.gcount());
        }
    }
    if (toRead < totalBytes) DropIncompleteUtf8Tail(content);

    if (IsBinary(content.data(), content.size())) {
        r.chips.push_back(LbToolHumanBytes(totalBytes));
        r.chips.push_back("binary");
        r.chips.push_back("hex preview");
        r.body = HexPreview(content);
        r.chips.push_back(LbElapsedChip(t0));
        return r;
    }

    std::string out;
    out.reserve(std::min(content.size(), (size_t)32 * 1024));
    size_t lines = 0;
    size_t pos = 0;
    bool truncatedByLines = false;
    // read_head is a preview: one enormous line (minified JSON, a log
    // with no newlines) must not turn it into a 128 KB body that gets
    // demoted to a Vars\ card claiming to hold "the complete content".
    constexpr size_t kMaxHeadLineBytes = 2000;
    bool clampedAnyLine = false;
    auto appendLine = [&](size_t from, size_t len, bool withNewline) {
        if (len > kMaxHeadLineBytes) {
            size_t cut = kMaxHeadLineBytes;
            while (cut > 0 &&
                   (static_cast<unsigned char>(content[from + cut]) & 0xC0) == 0x80)
                --cut;   // keep UTF-8 sequences whole
            out.append(content.data() + from, cut);
            out += " ...[line clamped: " + std::to_string(len) +
                   " bytes in this line; use read_range or grep for the rest]";
            if (withNewline) out += '\n';
            clampedAnyLine = true;
        } else {
            out.append(content.data() + from, len + (withNewline ? 1 : 0));
        }
    };
    while (pos < content.size() && lines < maxLines) {
        size_t nl = content.find('\n', pos);
        if (nl == std::string::npos) {
            appendLine(pos, content.size() - pos, false);
            pos = content.size();
            ++lines;
            break;
        }
        appendLine(pos, nl - pos, true);
        pos = nl + 1;
        ++lines;
    }
    bool headBomStripped = false;
    if (out.size() >= 3 && out.compare(0, 3, "\xEF\xBB\xBF") == 0) {
        out.erase(0, 3);
        headBomStripped = true;
    }
    if (pos < content.size() || toRead < totalBytes) {
        truncatedByLines = true;
        if (!out.empty() && out.back() != '\n') out += '\n';
        std::ostringstream marker;
        marker << "\n[... preview: showing first " << lines
               << " line(s) of " << LbToolHumanBytes(totalBytes)
               << " file ...]\n";
        out += marker.str();
    }

    r.chips.push_back(LbToolHumanBytes(totalBytes));
    r.chips.push_back(std::to_string(lines) + " shown lines");
    r.chips.push_back("preview");
    if (truncatedByLines) r.chips.push_back("truncated");
    if (clampedAnyLine) r.chips.push_back("long lines clamped");
    if (headBomStripped) r.chips.push_back("UTF-8 BOM");
    r.body = std::move(out);
    r.bodyLang = InferBodyLang(resolved);
    r.chips.push_back(LbElapsedChip(t0));
    return r;
}

// ─── RLM Phase C: ranged read ───────────────────────────────────────

ReadResult ReadFileRange(const std::string& inputPath,
                         const ToolContext& ctx,
                         size_t startLine,
                         size_t endLine)
{
    ReadResult r;
    auto t0 = std::chrono::steady_clock::now();

    // Keep the public single-range primitive consistent with the multi-range
    // path.  Router validation normally catches these shapes, but callers of
    // ReadFileRange/ReadFileRanges must not silently receive different lines
    // when malformed arguments reach this lower boundary.
    if (startLine == 0 || endLine == 0) {
        r.chips.push_back("failed");
        r.errorBody = "Range 1 must use positive 1-based line numbers.";
        r.chips.push_back(LbElapsedChip(t0));
        return r;
    }
    if (endLine < startLine) {
        r.chips.push_back("failed");
        r.errorBody = "Range 1 has END before START.";
        r.chips.push_back(LbElapsedChip(t0));
        return r;
    }
    constexpr size_t kMaxSpan = 1000;
    const size_t requestedSpan = endLine - startLine + 1;
    if (requestedSpan > kMaxSpan) {
        r.chips.push_back("failed");
        r.errorBody = "Range 1 requests " + std::to_string(requestedSpan)
                    + " lines (" + std::to_string(startLine) + ":"
                    + std::to_string(endLine) + "); maximum is "
                    + std::to_string(kMaxSpan)
                    + ". Split the request into smaller inclusive ranges; "
                      "sequential pages must not repeat the previous end line.";
        r.chips.push_back(LbElapsedChip(t0));
        return r;
    }

    bool usedConversationLane = false;
    std::string resolved = tool_path_safety::ResolveReadOnlyToolPath(
        inputPath, ctx.cwd, ctx.activeProjectRoot, &usedConversationLane);
    if (resolved.empty()) {
        r.chips.push_back("failed");
        r.errorBody = "Could not resolve path: " + inputPath;
        r.chips.push_back(LbElapsedChip(t0));
        return r;
    }
    if (usedConversationLane) r.chips.push_back("conversation lane");

    if (!IsFile(resolved)) {
        std::string scriptsFallback;
        if (!IsDirectory(resolved)) {
            scriptsFallback =
                TryResolveConversationScriptsFallback(inputPath, ctx);
        }
        if (!scriptsFallback.empty()) {
            resolved = scriptsFallback;
            r.chips.push_back("scripts lane");
        } else {
            r.chips.push_back("failed");
            if (IsDirectory(resolved)) {
                r.errorBody = "Not a file (is a directory): " + resolved;
            } else {
                r.errorBody = "File not found: " + resolved;
            }
            r.chips.push_back(LbElapsedChip(t0));
            return r;
        }
    }

    std::ifstream f(std::filesystem::path(path_safety::Utf8ToWide(resolved)), std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        r.chips.push_back("failed");
        r.errorBody = "Could not open file: " + resolved;
        r.chips.push_back(LbElapsedChip(t0));
        return r;
    }

    std::streamsize fileSize = f.tellg();
    if (fileSize < 0) {
        r.chips.push_back("failed");
        r.errorBody = "Could not determine file size: " + resolved;
        r.chips.push_back(LbElapsedChip(t0));
        return r;
    }

    const size_t totalBytes = (size_t)fileSize;
    if (totalBytes > kReadRefuseAbove) {
        r.chips.push_back(LbToolHumanBytes(totalBytes));
        r.chips.push_back("too large");
        r.errorBody = "File too large to read: " + LbToolHumanBytes(totalBytes)
                      + " (max " + LbToolHumanBytes(kReadRefuseAbove) + ")";
        r.chips.push_back(LbElapsedChip(t0));
        return r;
    }

    // Unlike the head preview, ranges must reach the deep interior of
    // the file, so the whole (<= 64 MiB) file is loaded and scanned.
    f.seekg(0, std::ios::beg);
    std::string content(totalBytes, '\0');
    if (totalBytes > 0) {
        f.read(&content[0], (std::streamsize)totalBytes);
        if (f.gcount() < (std::streamsize)totalBytes)
            content.resize((size_t)f.gcount());
    }

    if (IsBinary(content.data(), std::min(content.size(), (size_t)4096))) {
        r.chips.push_back(LbToolHumanBytes(totalBytes));
        r.chips.push_back("binary");
        r.chips.push_back("hex preview");
        r.body = HexPreview(content);
        r.chips.push_back(LbElapsedChip(t0));
        return r;
    }

    // Single forward scan: count lines, record [startLine, endLine] as byte
    // slices.  Nothing is copied until the output size is known to fit the
    // context cap below, so a huge line (minified JSON, a one-line log) is
    // rejected without first duplicating it in memory.
    struct LineSlice { size_t begin; size_t end; };
    std::vector<LineSlice> slices;
    size_t lineNo = 0;
    size_t pos = 0;
    size_t captured = 0;
    size_t outBytes = 0;
    // `pos == content.size()` is the byte position after a trailing newline,
    // not another logical line.  Using <= here created a phantom empty line
    // that disagreed with CountLines() and the multi-range indexer.
    while (pos < content.size() && !content.empty()) {
        size_t nl  = content.find('\n', pos);
        size_t end = (nl == std::string::npos) ? content.size() : nl;
        ++lineNo;
        if (lineNo >= startLine && lineNo <= endLine) {
            slices.push_back({ pos, end });
            outBytes += (end - pos) + 1;   // + the '\n' appended below
            ++captured;
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
        if (lineNo >= endLine && pos <= content.size()) {
            // Keep counting total lines cheaply for the chip.
            lineNo += (size_t)std::count(content.begin() + (std::ptrdiff_t)pos,
                                         content.end(), '\n');
            if (!content.empty() && content.back() != '\n') ++lineNo;
            break;
        }
    }

    if (captured == 0) {
        r.chips.push_back(LbToolHumanBytes(totalBytes));
        r.chips.push_back("failed");
        r.errorBody = "Range starts beyond end of file: requested line "
                      + std::to_string(startLine) + ", file has "
                      + std::to_string(lineNo) + " line(s).";
        r.chips.push_back(LbElapsedChip(t0));
        return r;
    }

    // A 1000-line slice can still be very large when the file contains long
    // logical lines.  Apply the same context-aware atomic limit as multi-range
    // reads so the compatibility path cannot flood the next model request.
    //
    // This is the SAME number reported as historyInlineBudgetBytes below, so
    // a slice that gets returned is guaranteed to fit inline and can never be
    // demoted to a Vars\ handle card behind the caller's back.
    const size_t outputCap = ComputeRangedReadHistoryBudget(ctx.ctxTokens);
    if (outBytes > outputCap) {
        r.chips.push_back("failed");
        r.errorBody = "Range output is " + LbToolHumanBytes(outBytes)
                    + ", above the safe " + LbToolHumanBytes(outputCap)
                    + " limit for the active context; narrow the range or "
                      "split the call.";
        r.chips.push_back(LbElapsedChip(t0));
        return r;
    }

    std::string out;
    out.reserve(outBytes);
    for (const LineSlice& s : slices) {
        out.append(content.data() + s.begin, s.end - s.begin);
        out += '\n';
    }

    const size_t returnedLastLine = startLine + captured - 1;
    r.chips.push_back("file " + LbToolHumanBytes(totalBytes));
    r.chips.push_back("slice " + LbToolHumanBytes(out.size()));
    r.chips.push_back("lines " + std::to_string(startLine) + "-"
                      + std::to_string(returnedLastLine)
                      + " of " + std::to_string(lineNo));
    if (returnedLastLine >= lineNo) {
        r.chips.push_back("reached EOF");
    } else {
        r.chips.push_back("next line " + std::to_string(returnedLastLine + 1));
    }
    r.body = std::move(out);
    r.bodyLang = InferBodyLang(resolved);
    r.historyInlineBudgetBytes =
        ComputeRangedReadHistoryBudget(ctx.ctxTokens);
    r.chips.push_back(LbElapsedChip(t0));
    return r;
}

// ─── RLM Step 2: multiple non-contiguous ranged reads ──────────────

ReadResult ReadFileRanges(const std::string& inputPath,
                          const ToolContext& ctx,
                          const std::vector<ReadLineRange>& ranges)
{
    // Preserve valid single-range display behavior, including its chips and
    // verbatim (unlabelled) body.  ReadFileRange owns the same malformed-range
    // and context-cap invariants enforced below for multiple ranges.
    if (ranges.size() == 1) {
        return ReadFileRange(inputPath, ctx,
                             ranges.front().startLine,
                             ranges.front().endLine);
    }

    ReadResult r;
    auto t0 = std::chrono::steady_clock::now();

    constexpr size_t kMaxRanges        = 20;
    constexpr size_t kMaxSpanPerRange = 1000;
    constexpr size_t kMaxCombinedSpan = 1000;

    auto fail = [&](const std::string& message) -> ReadResult {
        r.chips.push_back("failed");
        r.errorBody = message;
        r.chips.push_back(LbElapsedChip(t0));
        return r;
    };

    if (ranges.empty()) {
        return fail("read_range requires at least one line range.");
    }
    if (ranges.size() > kMaxRanges) {
        return fail("Too many read ranges: requested "
                    + std::to_string(ranges.size()) + ", maximum is "
                    + std::to_string(kMaxRanges) + ".");
    }

    std::vector<ReadLineRange> normalized;
    normalized.reserve(ranges.size());
    size_t combinedSpan = 0;

    for (size_t i = 0; i < ranges.size(); ++i) {
        ReadLineRange one = ranges[i];
        if (one.startLine == 0 || one.endLine == 0) {
            return fail("Range " + std::to_string(i + 1)
                        + " must use positive 1-based line numbers.");
        }
        if (one.endLine < one.startLine) {
            return fail("Range " + std::to_string(i + 1)
                        + " has END before START.");
        }

        // Compare the zero-based distance first so an extreme END value
        // cannot overflow when the inclusive span adds one.
        const size_t distance = one.endLine - one.startLine;
        if (distance >= kMaxSpanPerRange) {
            const size_t requestedSpan = distance + 1;
            return fail("Range " + std::to_string(i + 1)
                        + " requests " + std::to_string(requestedSpan)
                        + " lines (" + std::to_string(one.startLine) + ":"
                        + std::to_string(one.endLine) + "); maximum is "
                        + std::to_string(kMaxSpanPerRange)
                        + ". Split the request into smaller inclusive ranges; "
                          "sequential pages must not repeat the previous end line.");
        }

        normalized.push_back(one);
    }

    // Out-of-order or overlapping ranges are sorted and merged instead of
    // rejected ("195:260,1:25" has an unambiguous intent).  Each range
    // was validated above; the merged result is re-checked against the
    // same caps, and a chip records the normalization so it is never
    // silent.
    bool reordered = false, merged = false;
    for (size_t i = 1; i < normalized.size(); ++i) {
        if (normalized[i].startLine < normalized[i - 1].startLine) {
            reordered = true;
            break;
        }
    }
    if (reordered) {
        std::stable_sort(normalized.begin(), normalized.end(),
                         [](const ReadLineRange& a, const ReadLineRange& b) {
                             return a.startLine < b.startLine;
                         });
    }
    {
        std::vector<ReadLineRange> mergedRanges;
        mergedRanges.reserve(normalized.size());
        for (const ReadLineRange& one : normalized) {
            if (!mergedRanges.empty() &&
                one.startLine <= mergedRanges.back().endLine) {
                if (one.endLine > mergedRanges.back().endLine)
                    mergedRanges.back().endLine = one.endLine;
                merged = true;
            } else {
                mergedRanges.push_back(one);
            }
        }
        normalized.swap(mergedRanges);
    }
    for (const ReadLineRange& one : normalized) {
        const size_t normalizedSpan = one.endLine - one.startLine + 1;
        if (normalizedSpan > kMaxSpanPerRange) {
            return fail("Overlapping ranges merge to "
                        + std::to_string(one.startLine) + ":"
                        + std::to_string(one.endLine) + " ("
                        + std::to_string(normalizedSpan)
                        + " lines); maximum is "
                        + std::to_string(kMaxSpanPerRange)
                        + ". Request fewer lines.");
        }
        if (combinedSpan > kMaxCombinedSpan - normalizedSpan) {
            return fail("Combined read-range span exceeds "
                        + std::to_string(kMaxCombinedSpan)
                        + " lines; narrow the ranges or split the call.");
        }
        combinedSpan += normalizedSpan;
    }
    if (reordered) r.chips.push_back("ranges sorted");
    if (merged)    r.chips.push_back("ranges merged");

    bool usedConversationLane = false;
    std::string resolved = tool_path_safety::ResolveReadOnlyToolPath(
        inputPath, ctx.cwd, ctx.activeProjectRoot, &usedConversationLane);
    if (resolved.empty()) {
        return fail("Could not resolve path: " + inputPath);
    }
    if (usedConversationLane) r.chips.push_back("conversation lane");

    if (!IsFile(resolved)) {
        std::string scriptsFallback;
        if (!IsDirectory(resolved)) {
            scriptsFallback =
                TryResolveConversationScriptsFallback(inputPath, ctx);
        }
        if (!scriptsFallback.empty()) {
            resolved = scriptsFallback;
            r.chips.push_back("scripts lane");
        } else if (IsDirectory(resolved)) {
            return fail("Not a file (is a directory): " + resolved);
        } else {
            return fail("File not found: " + resolved);
        }
    }

    std::ifstream f(std::filesystem::path(path_safety::Utf8ToWide(resolved)),
                    std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        return fail("Could not open file: " + resolved);
    }

    std::streamsize fileSize = f.tellg();
    if (fileSize < 0) {
        return fail("Could not determine file size: " + resolved);
    }

    const size_t totalBytes = (size_t)fileSize;
    if (totalBytes > kReadRefuseAbove) {
        r.chips.push_back(LbToolHumanBytes(totalBytes));
        r.chips.push_back("too large");
        r.errorBody = "File too large to read: " + LbToolHumanBytes(totalBytes)
                    + " (max " + LbToolHumanBytes(kReadRefuseAbove) + ")";
        r.chips.push_back(LbElapsedChip(t0));
        return r;
    }

    f.seekg(0, std::ios::beg);
    std::string content(totalBytes, '\0');
    if (totalBytes > 0) {
        f.read(&content[0], (std::streamsize)totalBytes);
        if (f.gcount() < (std::streamsize)totalBytes)
            content.resize((size_t)f.gcount());
    }

    if (IsBinary(content.data(), std::min(content.size(), (size_t)4096))) {
        r.chips.push_back(LbToolHumanBytes(totalBytes));
        r.chips.push_back("binary");
        r.chips.push_back("hex preview");
        r.body = HexPreview(content);
        r.chips.push_back(LbElapsedChip(t0));
        return r;
    }

    // Total logical lines without indexing every line: each '\n' ends one,
    // plus a final unterminated line.  (A per-line offset index of the whole
    // file cost ~16 bytes per line on top of the file itself.)
    size_t totalLines = static_cast<size_t>(
        std::count(content.begin(), content.end(), '\n'));
    if (!content.empty() && content.back() != '\n') ++totalLines;
    if (totalLines == 0) {
        return fail("Cannot read ranges from an empty file: " + resolved);
    }

    // Validate all starts before returning any content.  A bad range should
    // never masquerade as successful evidence coverage for the other ranges.
    for (size_t i = 0; i < normalized.size(); ++i) {
        if (normalized[i].startLine > totalLines) {
            return fail("Range " + std::to_string(i + 1)
                        + " starts beyond end of file: requested line "
                        + std::to_string(normalized[i].startLine)
                        + ", file has " + std::to_string(totalLines)
                        + " line(s).");
        }
    }

    // One forward pass to the last requested line, recording only the
    // requested lines as [byte start, byte end) slices (excluding '\n' but
    // retaining a preceding '\r', preserving established byte behavior).
    // Ranges are ascending and non-overlapping (validated above) and capped
    // at 1000 lines combined, so `picked` stays small.
    struct LineSlice { size_t begin; size_t end; };
    std::vector<std::vector<LineSlice>> picked(normalized.size());
    {
        size_t rangeIdx = 0;
        size_t lineNo = 0;
        size_t pos = 0;
        while (pos < content.size() && rangeIdx < normalized.size()) {
            const size_t nl  = content.find('\n', pos);
            const size_t end = (nl == std::string::npos) ? content.size() : nl;
            ++lineNo;
            while (rangeIdx < normalized.size() &&
                   lineNo > std::min(normalized[rangeIdx].endLine, totalLines)) {
                ++rangeIdx;
            }
            if (rangeIdx < normalized.size() &&
                lineNo >= normalized[rangeIdx].startLine) {
                picked[rangeIdx].push_back({ pos, end });
            }
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
    }

    // Build the range headers and measure the exact output size before
    // copying any line bytes, so an oversized request is rejected without
    // first materializing it.
    std::vector<std::string> headers(normalized.size());
    size_t outBytes = 0;
    size_t capturedLines = 0;
    for (size_t i = 0; i < normalized.size(); ++i) {
        const size_t first = normalized[i].startLine;
        const size_t last  = std::min(normalized[i].endLine, totalLines);
        headers[i] = "[range " + std::to_string(i + 1) + ": lines "
                   + std::to_string(first) + "-" + std::to_string(last)
                   + " of " + std::to_string(totalLines) + "]\n";
        if (i > 0) outBytes += 1;   // blank separator line
        outBytes += headers[i].size();
        for (const LineSlice& s : picked[i]) {
            outBytes += (s.end - s.begin) + 1;
            ++capturedLines;
        }
    }

    // Multi-range is an orchestration optimization, not permission to flood
    // the next model request.  Reject the combined body atomically so the
    // caller can narrow ranges without accidentally reasoning over a silently
    // truncated subset.
    //
    // Same number as historyInlineBudgetBytes below: what comes back always
    // fits inline, so multi-range output never demotes into Vars\ either.
    const size_t outputCap = ComputeRangedReadHistoryBudget(ctx.ctxTokens);
    if (outBytes > outputCap) {
        return fail("Combined range output is " + LbToolHumanBytes(outBytes)
                    + ", above the safe " + LbToolHumanBytes(outputCap)
                    + " limit for the active context; narrow the ranges or "
                      "split the call.");
    }

    std::string out;
    out.reserve(outBytes);
    for (size_t i = 0; i < normalized.size(); ++i) {
        if (i > 0) out += '\n';
        out += headers[i];
        for (const LineSlice& s : picked[i]) {
            out.append(content.data() + s.begin, s.end - s.begin);
            out += '\n';
        }
    }

    r.chips.push_back("file " + LbToolHumanBytes(totalBytes));
    r.chips.push_back("slice " + LbToolHumanBytes(out.size()));
    r.chips.push_back(std::to_string(normalized.size()) + " ranges");
    r.chips.push_back(std::to_string(capturedLines) + " lines of "
                      + std::to_string(totalLines));
    r.chips.push_back("request complete");
    r.body = std::move(out);
    r.bodyLang = InferBodyLang(resolved);
    r.historyInlineBudgetBytes =
        ComputeRangedReadHistoryBudget(ctx.ctxTokens);
    r.chips.push_back(LbElapsedChip(t0));
    return r;
}
