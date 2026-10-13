// tool_copy.cpp -- see tool_copy.h

#include "tool_copy.h"
#include "lb_ascii.h"
#include "tool_hash.h"
#include "tool_write.h"
#include "lb_sha256.h"
#include "tool_path.h"
#include "tool_path_safety.h"
#include "path_safety.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <sstream>

namespace {

std::string Trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string Unquote(std::string s)
{
    s = Trim(s);
    if (s.size() >= 2 &&
        ((s.front() == '"' && s.back() == '"') ||
         (s.front() == '\'' && s.back() == '\''))) {
        s = Trim(s.substr(1, s.size() - 2));
    }
    return s;
}

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(lb_ascii::ToLower(c)); });
    return s;
}

std::string ElapsedChip(std::chrono::steady_clock::time_point t0)
{
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();
    std::ostringstream ts;
    ts << std::fixed;
    ts.precision(elapsed < 10.0 ? 2 : 1);
    ts << elapsed << "s";
    return ts.str();
}

std::string HumanBytes(size_t b)
{
    std::ostringstream ss;
    ss << std::fixed;
    if (b < 1024)              { ss << b << " B"; }
    else if (b < 1024 * 1024)  { ss.precision(1); ss << (b / 1024.0) << " KB"; }
    else                       { ss.precision(2); ss << (b / (1024.0 * 1024.0)) << " MB"; }
    return ss.str();
}

int CountLines(const std::string& s)
{
    if (s.empty()) return 0;
    int n = 0;
    for (char c : s) if (c == '\n') ++n;
    if (s.back() != '\n') ++n;
    return n;
}

bool ReadAllBytes(const std::string& absPath, size_t cap,
                  std::string& out, std::string& error)
{
#ifdef _WIN32
    std::ifstream f(path_safety::Utf8ToWide(absPath), std::ios::binary | std::ios::ate);
#else
    std::ifstream f(absPath, std::ios::binary | std::ios::ate);
#endif
    if (!f) {
        error = "Could not open source for reading (it may be locked or inaccessible): " + absPath;
        return false;
    }
    const std::streamoff size = f.tellg();
    if (size < 0) {
        error = "Could not determine source size: " + absPath;
        return false;
    }
    if (static_cast<unsigned long long>(size) > cap) {
        error = "Source is " + HumanBytes(static_cast<size_t>(size)) +
                ", above the copy limit of " + HumanBytes(cap) +
                ". Use PowerShell Copy-Item for large files (it needs approval).";
        return false;
    }
    out.assign(static_cast<size_t>(size), '\0');
    f.seekg(0);
    if (size > 0 && !f.read(&out[0], size)) {
        error = "Read error on source: " + absPath;
        return false;
    }
    return true;
}

} // namespace

namespace tool_copy {

Args ParseArgs(const std::string& raw)
{
    Args a;
    std::vector<std::string> lines;
    {
        std::string s = Trim(raw);
        size_t pos = 0;
        while (pos <= s.size()) {
            size_t nl = s.find('\n', pos);
            std::string line = Trim(s.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
            if (!line.empty()) lines.push_back(line);
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
    }

    if (lines.size() < 2) {
        a.error = "copy needs the source path on line 1 and the destination path on "
                  "line 2 (optional line 3: overwrite). Native calls use "
                  "{source, destination, overwrite}.";
        return a;
    }
    if (lines.size() > 3) {
        a.error = "copy takes exactly one source and one destination (plus an optional "
                  "\"overwrite\" line). Copy files one call at a time.";
        return a;
    }
    a.source      = Unquote(lines[0]);
    a.destination = Unquote(lines[1]);
    if (lines.size() == 3) {
        const std::string flag = Lower(lines[2]);
        if (flag == "overwrite" || flag == "true" || flag == "overwrite=true") {
            a.overwrite = true;
        } else if (flag == "false" || flag == "overwrite=false") {
            a.overwrite = false;
        } else {
            a.error = "copy line 3 may only be \"overwrite\"; got: " + lines[2];
            return a;
        }
    }
    if (a.source.empty() || a.destination.empty())
        a.error = "copy needs both a source and a destination path.";
    return a;
}

} // namespace tool_copy

CopyResult CopyFileExact(const std::string& args, const ToolContext& ctx)
{
    CopyResult r;
    const auto t0 = std::chrono::steady_clock::now();

    auto fail = [&](const char* chip, const std::string& why) {
        r.chips = { chip, ElapsedChip(t0) };
        r.errorBody = why;
        return r;
    };

    const tool_copy::Args a = tool_copy::ParseArgs(args);
    if (!a.error.empty()) return fail("failed", a.error);
    if (ctx.cwd.empty()) return fail("blocked", "No working directory set; refuse to copy.");

    // ── Source: read-style resolution ────────────────────────────
    const std::string src = tool_path_safety::ResolveReadOnlyToolPath(
        a.source, ctx.cwd, ctx.activeProjectRoot);
    if (src.empty()) return fail("failed", "Could not resolve source path: " + a.source);
    if (IsDirectory(src))
        return fail("failed", "copy takes one file, not a directory: " + src);
    if (!IsFile(src)) return fail("failed", "Source file not found: " + src);

    // Refuse a self-copy before touching anything: with overwrite it would
    // be a pointless rewrite, without it a confusing "exists" failure.
    const std::string dstPreview = tool_path_safety::ResolveProjectAwareToolPath(
        a.destination, ctx.cwd, ctx.activeProjectRoot);
    if (!dstPreview.empty() && Lower(dstPreview) == Lower(src))
        return fail("failed", "Source and destination are the same file: " + src);

    std::string bytes, why;
    if (!ReadAllBytes(src, kExactWriteMaxBytes, bytes, why))
        return fail("failed", why);
    const std::string srcDigest = lb_sha256::HexOf(bytes);

    // ── Destination: every overwrite_file gate, exact bytes ──────
    WriteResult w = WriteExactBytes(a.destination, bytes, ctx, a.overwrite);
    if (!w.errorBody.empty() || w.createdPath.empty()) {
        r.chips = w.chips.empty() ? std::vector<std::string>{ "failed" } : w.chips;
        r.errorBody = w.errorBody.empty() ? std::string("Copy failed.") : w.errorBody;
        return r;
    }

    // ── Verify what actually landed on disk ──────────────────────
    std::string dstDigest;
    std::uint64_t dstSize = 0;
    if (!tool_hash::HashFileAt(w.createdPath, dstDigest, dstSize, why)) {
        return fail("failed", "The copy was written but could not be re-read for "
                              "verification: " + why);
    }
    if (dstDigest != srcDigest || dstSize != bytes.size()) {
        return fail("failed",
            "The copy was written but its SHA-256 does not match the source.\n"
            "  source:      " + srcDigest + "  (" + std::to_string(bytes.size()) + " bytes)\n"
            "  destination: " + dstDigest + "  (" + std::to_string(dstSize) + " bytes)\n"
            "  path: " + w.createdPath);
    }

    const bool replaced = std::find(w.chips.begin(), w.chips.end(), "overwritten") != w.chips.end();

    std::ostringstream body;
    body << (replaced ? "Replaced " : "Copied ") << bytes.size() << " bytes\n"
         << "From: " << src << "\n"
         << "To:   " << w.createdPath << "\n"
         << "SHA-256: " << srcDigest << "\n"
         << "Verified: the destination's SHA-256 matches the source (byte-identical).\n";

    r.body        = body.str();
    r.createdPath = w.createdPath;
    r.displayName = w.displayName;
    r.sizeBytes   = bytes.size();
    r.lineCount   = CountLines(bytes);
    r.chips = { replaced ? "overwritten" : "copied",
                bytes.empty() ? std::string("empty") : HumanBytes(bytes.size()),
                "verified", ElapsedChip(t0) };
    return r;
}
