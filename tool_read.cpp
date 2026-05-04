// tool_read.cpp

#include "tool_read.h"
#include "tool_path.h"
#include "path_safety.h"

#include <algorithm>
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

// ─── Helpers ─────────────────────────────────────────────────────

// Human-readable byte count.  Base-1024 throughout (KB=1024 etc.) —
// that's what most developers mean when they say "KB" casually,
// and it matches what Windows Explorer shows in "Size".
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
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);

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

// Elapsed-time chip — same format as /cmd's header ("0.02s" / "12.3s").
std::string ElapsedChip(
    std::chrono::steady_clock::time_point t0)
{
    double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();
    std::ostringstream ts;
    ts << std::fixed;
    ts.precision(elapsed < 10.0 ? 2 : 1);
    ts << elapsed << "s";
    return ts.str();
}

} // anonymous namespace

ReadResult ReadFile(const std::string& inputPath, const ToolContext& ctx)
{
    ReadResult r;
    auto t0 = std::chrono::steady_clock::now();

    // ── Path resolution ──────────────────────────────────────────
    std::string resolved = ResolveToolPath(inputPath, ctx.cwd);
    if (resolved.empty()) {
        r.chips.push_back("failed");
        r.errorBody = "Could not resolve path: " + inputPath;
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    // ── Existence / type check ───────────────────────────────────
    if (!IsFile(resolved)) {
        r.chips.push_back("failed");
        if (IsDirectory(resolved)) {
            r.errorBody = "Not a file (is a directory): " + resolved;
        } else {
            r.errorBody = "File not found: " + resolved;
        }
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    // ── Size check (ate + tellg) ─────────────────────────────────
    std::ifstream f(path_safety::Utf8ToWide(resolved), std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        r.chips.push_back("failed");
        r.errorBody = "Could not open file: " + resolved;
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

    const size_t totalBytes = (size_t)fileSize;

    if (totalBytes > kReadRefuseAbove) {
        r.chips.push_back(HumanBytes(totalBytes));
        r.chips.push_back("too large");
        r.errorBody = "File too large to read: " + HumanBytes(totalBytes)
                      + " (max " + HumanBytes(kReadRefuseAbove) + ")";
        r.chips.push_back(ElapsedChip(t0));
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
        marker << "\n[... truncated: showing " << HumanBytes(content.size())
               << " of " << HumanBytes(totalBytes)
               << " to fit model context (ctx=" << ctx.ctxTokens
               << " tokens) ...]\n";
        content += marker.str();
    }

    // ── Binary branch ────────────────────────────────────────────
    if (IsBinary(content.data(), content.size())) {
        r.chips.push_back(HumanBytes(totalBytes));
        r.chips.push_back("binary");
        r.chips.push_back("hex preview");
        r.body = HexPreview(content);
        // No language hint for hex — it's not a real source dump.
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    // ── Text branch ──────────────────────────────────────────────
    r.chips.push_back(HumanBytes(totalBytes));
    r.chips.push_back(std::to_string(shownLineCount) +
                      (truncated ? " shown lines" : " lines"));
    if (truncated) r.chips.push_back("truncated");
    r.body     = std::move(content);
    r.bodyLang = InferBodyLang(resolved);
    r.chips.push_back(ElapsedChip(t0));
    return r;
}
