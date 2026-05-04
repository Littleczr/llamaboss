// path_safety.cpp
#include "path_safety.h"

#include <algorithm>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace path_safety {

namespace {

// ─── Single-pass character substitution ────────────────────────────
// UTF-8 continuation bytes (>= 0x80) pass through untouched so non-
// ASCII filenames stay readable on disk.  Replacement char is '_'
// rather than removal so the output preserves intent visually
// (e.g. "path/to/file.txt" -> "path_to_file.txt").
std::string SubstituteUnsafeChars(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (unsigned char c : in) {
        // Path separators -- the most important class to strip.
        if (c == '/' || c == '\\') { out += '_'; continue; }
        // Windows-forbidden punctuation in filenames.
        if (c == '<' || c == '>' || c == ':' || c == '"' ||
            c == '|' || c == '?' || c == '*') { out += '_'; continue; }
        // ASCII control characters (incl. NUL, tab, newline, CR, DEL).
        if (c < 0x20 || c == 0x7F) { out += '_'; continue; }
        out += static_cast<char>(c);
    }
    return out;
}

// Replace any '.' run of length >= 2 anywhere in the string with '_'
// of the same length.  This neutralizes path-traversal tokens like
// ".." and ".." segments left over after slash substitution
// ("../../../foo" -> "______foo"), while preserving single dots
// (".env", "archive.tar.gz", "v1.2.3").
void NeutralizeDotRuns(std::string& s)
{
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] != '.') { ++i; continue; }
        size_t run = i;
        while (run < s.size() && s[run] == '.') ++run;
        size_t len = run - i;
        if (len >= 2) {
            for (size_t k = i; k < run; ++k) s[k] = '_';
        }
        i = run;
    }
}

// Strip trailing '.' and ' ' runs.  Windows silently strips these on
// file creation, which would desync the on-disk name from whatever
// the UI showed -- strip explicitly so the chip label matches the
// file we wrote.
void StripTrailingDotsAndSpaces(std::string& s)
{
    while (!s.empty() && (s.back() == '.' || s.back() == ' '))
        s.pop_back();
}

// ASCII-only, locale-free uppercase.  Avoids std::toupper's locale
// dependence and UB on signed-char inputs.
char UpperAscii(char c)
{
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

// Does the stem match a Windows reserved device name?  Reserved
// names are checked WITHOUT extension -- "CON.txt" is reserved
// because its stem is "CON".  All reserved names are 3 or 4 chars,
// so we fast-exit on length.
bool StemEqualsReserved(const std::string& stem)
{
    if (stem.size() != 3 && stem.size() != 4) return false;

    char u[5] = { 0 };
    for (size_t i = 0; i < stem.size(); ++i) u[i] = UpperAscii(stem[i]);

    if (stem.size() == 3) {
        return (u[0] == 'C' && u[1] == 'O' && u[2] == 'N') ||  // CON
               (u[0] == 'P' && u[1] == 'R' && u[2] == 'N') ||  // PRN
               (u[0] == 'A' && u[1] == 'U' && u[2] == 'X') ||  // AUX
               (u[0] == 'N' && u[1] == 'U' && u[2] == 'L');    // NUL
    }
    // size == 4: COMn or LPTn for n = 1..9
    bool isCom = (u[0] == 'C' && u[1] == 'O' && u[2] == 'M');
    bool isLpt = (u[0] == 'L' && u[1] == 'P' && u[2] == 'T');
    if (!isCom && !isLpt) return false;
    return u[3] >= '1' && u[3] <= '9';
}

// Split a filename at the LAST '.'.  Treats a SINGLE leading dot as
// part of the stem so dotfiles like ".env" don't get parsed as
// (stem="", ext=".env").
//   "foo.tar.gz" -> ("foo.tar", ".gz")
//   "foo"        -> ("foo", "")
//   ".env"       -> (".env", "")
std::pair<std::string, std::string> SplitExtension(const std::string& s)
{
    if (s.empty()) return { s, std::string() };
    size_t pos = s.rfind('.');
    if (pos == std::string::npos || pos == 0) return { s, std::string() };
    return { s.substr(0, pos), s.substr(pos) };
}

// Returns the segment of `s` BEFORE the first '.' (skipping a single
// leading dot, so ".env" -> ".env").  Used for the reserved-device-
// name check: Windows treats "CON.anything" as reserved, looking at
// the FIRST dot, not the last.  This differs from SplitExtension's
// last-dot semantics, which are right for length-preserving
// truncation.
std::string FirstDotStem(const std::string& s)
{
    if (s.empty()) return s;
    size_t startIdx = (s[0] == '.') ? 1 : 0;
    size_t pos = s.find('.', startIdx);
    if (pos == std::string::npos) return s;
    return s.substr(0, pos);
}

// Truncate to maxLen, preserving the extension when reasonable.  An
// "extension" longer than 16 chars is treated as not-really-an-
// extension and we just hard-truncate (defends against pathological
// inputs like "data.thisisnotanextension_aaaaaa...").
std::string TruncatePreservingExt(const std::string& s, size_t maxLen)
{
    if (s.size() <= maxLen) return s;

    auto parts = SplitExtension(s);
    const std::string& stem = parts.first;
    const std::string& ext  = parts.second;

    if (ext.size() > 16 || ext.size() >= maxLen) {
        return s.substr(0, maxLen);
    }

    size_t stemMax = maxLen - ext.size();
    if (stemMax == 0) return s.substr(0, maxLen);
    return stem.substr(0, stemMax) + ext;
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════
//  Public API
// ═══════════════════════════════════════════════════════════════════

std::string SanitizeFilename(const std::string& raw,
                             const std::string& fallback)
{
    if (raw.empty()) return fallback;

    std::string out = SubstituteUnsafeChars(raw);
    NeutralizeDotRuns(out);
    StripTrailingDotsAndSpaces(out);

    if (out.empty()) return fallback;

    // Reserved-device-name guard: examine the FIRST-dot stem (not the
    // last-dot one), because Windows treats "CON.anything" as reserved
    // regardless of further extensions.  Prefix with '_' to keep the
    // visual association with the original name while making it
    // creatable on Windows.
    if (StemEqualsReserved(FirstDotStem(out))) {
        out = "_" + out;
    }

    out = TruncatePreservingExt(out, kMaxFilenameLen);

    // Re-strip trailing dots/spaces in case truncation exposed any.
    StripTrailingDotsAndSpaces(out);
    if (out.empty()) return fallback;

    return out;
}

// ═══════════════════════════════════════════════════════════════════
//  UTF-8 <-> wide conversion (see header for rationale)
// ═══════════════════════════════════════════════════════════════════
//
// Note: identical helpers exist (file-locally) in cmd_executor.cpp,
// server_manager.cpp, tool_grep.cpp, tool_ls.cpp, tool_open.cpp, and
// tool_path.cpp.  Those duplicates can be migrated to call these in
// a future cleanup pass; new code should use these directly.

std::wstring Utf8ToWide(const std::string& in)
{
    if (in.empty()) return std::wstring();
    int n = ::MultiByteToWideChar(CP_UTF8, 0, in.data(), (int)in.size(),
                                  nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out((size_t)n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, in.data(), (int)in.size(),
                          &out[0], n);
    return out;
}

std::string WideToUtf8(const std::wstring& in)
{
    if (in.empty()) return std::string();
    int n = ::WideCharToMultiByte(CP_UTF8, 0, in.data(), (int)in.size(),
                                  nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out((size_t)n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, in.data(), (int)in.size(),
                          &out[0], n, nullptr, nullptr);
    return out;
}

} // namespace path_safety
