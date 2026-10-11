// tool_hash.cpp -- see tool_hash.h

#include "tool_hash.h"
#include "lb_sha256.h"
#include "tool_path.h"
#include "tool_path_safety.h"
#include "path_safety.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

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

std::string HumanBytes(std::uint64_t b)
{
    std::ostringstream ss;
    ss << std::fixed;
    if (b < 1024)                     { ss << b << " B"; }
    else if (b < 1024ull * 1024)      { ss.precision(1); ss << (b / 1024.0) << " KB"; }
    else if (b < 1024ull * 1024 * 1024) { ss.precision(1); ss << (b / (1024.0 * 1024.0)) << " MB"; }
    else                              { ss.precision(2); ss << (b / (1024.0 * 1024.0 * 1024.0)) << " GB"; }
    return ss.str();
}

bool IsHexChar(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool AllHex(const std::string& s)
{
    if (s.empty()) return false;
    for (char c : s) if (!IsHexChar(c)) return false;
    return true;
}

// A value that is plainly meant as a digest but is not a SHA-256: long,
// all hex, wrong length.  Reported as such instead of being treated as a
// second file path that "does not exist".
bool LooksLikeWrongLengthDigest(const std::string& s)
{
    return s.size() >= 32 && s.size() != 64 && AllHex(s);
}

} // namespace

namespace tool_hash {

Args ParseArgs(const std::string& raw)
{
    Args a;
    const std::string s = Trim(raw);
    if (s.empty()) return a;

    const size_t nl = s.find('\n');
    if (nl != std::string::npos) {
        a.path     = Unquote(s.substr(0, nl));
        a.expected = Unquote(s.substr(nl + 1));
        // Only the first line after the path is the comparand.
        const size_t nl2 = a.expected.find('\n');
        if (nl2 != std::string::npos) a.expected = Trim(a.expected.substr(0, nl2));
        return a;
    }

    // One-line "path <hex>": split only when the trailing token is a
    // digest-shaped hex run, so paths containing spaces stay intact.
    const size_t sp = s.find_last_of(" \t");
    if (sp != std::string::npos) {
        const std::string tail = Trim(s.substr(sp + 1));
        if (lb_sha256::IsSha256Hex(tail) || LooksLikeWrongLengthDigest(tail)) {
            a.path     = Unquote(s.substr(0, sp));
            a.expected = tail;
            return a;
        }
    }
    a.path = Unquote(s);
    return a;
}

bool HashFileAt(const std::string& absPath,
                std::string& hexOut,
                std::uint64_t& sizeOut,
                std::string& error)
{
    hexOut.clear();
    sizeOut = 0;
#ifdef _WIN32
    std::ifstream f(path_safety::Utf8ToWide(absPath), std::ios::binary);
#else
    std::ifstream f(absPath, std::ios::binary);
#endif
    if (!f) {
        error = "Could not open file for reading (it may be locked or inaccessible): " + absPath;
        return false;
    }
    lb_sha256::Hasher h;
    std::vector<char> buf(1 << 20);
    while (f) {
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize got = f.gcount();
        if (got > 0) {
            h.Update(buf.data(), static_cast<size_t>(got));
            sizeOut += static_cast<std::uint64_t>(got);
        }
    }
    if (f.bad()) {
        error = "Read error while hashing: " + absPath;
        return false;
    }
    hexOut = h.HexDigest();
    return true;
}

} // namespace tool_hash

HashResult HashFile(const std::string& args, const ToolContext& ctx)
{
    HashResult r;
    const auto t0 = std::chrono::steady_clock::now();

    auto fail = [&](const std::string& why) {
        r.chips = { "failed", ElapsedChip(t0) };
        r.errorBody = why;
        return r;
    };

    const tool_hash::Args a = tool_hash::ParseArgs(args);
    if (a.path.empty())
        return fail("hash needs a file path (line 1). Optional line 2: an expected "
                    "SHA-256 (64 hex characters) or a second file path to compare.");

    auto resolveExisting = [&](const std::string& in, std::string& out,
                               std::string& why) -> bool {
        out = tool_path_safety::ResolveReadOnlyToolPath(in, ctx.cwd, ctx.activeProjectRoot);
        if (out.empty()) { why = "Could not resolve path: " + in; return false; }
        if (IsDirectory(out)) { why = "hash takes a file, not a directory: " + out; return false; }
        if (!IsFile(out)) { why = "File not found: " + out; return false; }
        return true;
    };

    std::string resolved, why;
    if (!resolveExisting(a.path, resolved, why)) return fail(why);

    std::string digest;
    std::uint64_t size = 0;
    if (!tool_hash::HashFileAt(resolved, digest, size, why)) return fail(why);

    std::ostringstream body;
    body << "File: " << resolved << "\n"
         << "Size: " << size << " bytes\n"
         << "SHA-256: " << digest << "\n";

    std::string verdictChip;
    if (!a.expected.empty()) {
        body << "\n";
        if (lb_sha256::IsSha256Hex(a.expected)) {
            const bool match = lb_sha256::LowerHex(a.expected) == digest;
            body << "Expected: " << lb_sha256::LowerHex(a.expected) << "\n"
                 << "Match: " << (match ? "True" : "False")
                 << " (compared case-insensitively)\n";
            verdictChip = match ? "match" : "MISMATCH";
        } else if (LooksLikeWrongLengthDigest(a.expected)) {
            body << "Expected value is " << a.expected.size()
                 << " hex characters; a SHA-256 has exactly 64, so it cannot match.\n"
                 << "Expected (as given): " << a.expected << "\n"
                 << "Match: False\n";
            verdictChip = "MISMATCH";
        } else {
            std::string other;
            if (!resolveExisting(a.expected, other, why)) {
                body << "Comparison skipped: " << why << "\n";
                verdictChip = "no compare";
            } else {
                std::string otherDigest;
                std::uint64_t otherSize = 0;
                if (!tool_hash::HashFileAt(other, otherDigest, otherSize, why)) {
                    body << "Comparison skipped: " << why << "\n";
                    verdictChip = "no compare";
                } else {
                    const bool match = otherDigest == digest && otherSize == size;
                    body << "Compared with: " << other << "\n"
                         << "  Size: " << otherSize << " bytes\n"
                         << "  SHA-256: " << otherDigest << "\n"
                         << "Match: " << (match ? "True (identical content)" : "False") << "\n";
                    verdictChip = match ? "match" : "MISMATCH";
                }
            }
        }
    }

    body << "\nQuote the Match line or copy the digest from this result; do not retype hashes from memory.\n";

    r.body = body.str();
    r.chips.push_back(HumanBytes(size));
    if (!verdictChip.empty()) r.chips.push_back(verdictChip);
    r.chips.push_back(ElapsedChip(t0));
    return r;
}
