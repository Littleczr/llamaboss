// ps_command_hints.h
//
// Header-only detectors for well-known PowerShell footguns that
// produce a SILENT no-op (exit 0, no output, no error), which local
// models then misread as success.
//
// First detector (2026-06-11): `Get-ChildItem -Include` selects
// nothing unless -Recurse is used or the -Path value ends with a
// wildcard.  Observed in the wild as:
//
//   Get-ChildItem -Path "C:\dir" -Include "*.h","*.cpp" -File |
//       Compress-Archive -DestinationPath out.zip
//
// -> zero files matched, Compress-Archive's process block never ran,
//    exit 0, no zip, and the model claimed the file was "attached".
//
// The caller decides WHEN to surface a hint (typically: command
// succeeded but produced no output and no workspace changes); this
// header only answers "does the command match the footgun pattern".
//
#pragma once

#include <string>

namespace ps_command_hints {

namespace detail {

inline std::string LowerAscii(std::string s)
{
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

// True if `token` appears in `haystack` delimited by non-identifier
// characters (so "gci" does not match inside "logcint").
inline bool ContainsToken(const std::string& haystack,
                          const std::string& token)
{
    size_t pos = 0;
    auto isWord = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '-' || c == '_';
    };
    while ((pos = haystack.find(token, pos)) != std::string::npos) {
        bool leftOk  = (pos == 0) || !isWord(haystack[pos - 1]);
        size_t end   = pos + token.size();
        bool rightOk = (end >= haystack.size()) || !isWord(haystack[end]);
        if (leftOk && rightOk) return true;
        pos = end;
    }
    return false;
}

// Extracts the first argument value following `-path` (quoted or
// bare).  Empty if -path is absent or has no value.
inline std::string ExtractPathArg(const std::string& lowered,
                                  const std::string& original)
{
    size_t p = lowered.find("-path");
    if (p == std::string::npos) return std::string();
    p += 5;

    // Skip separators (space, colon as in -Path:"x").
    while (p < original.size() &&
           (original[p] == ' ' || original[p] == '\t' || original[p] == ':')) {
        ++p;
    }
    if (p >= original.size()) return std::string();

    char quote = 0;
    if (original[p] == '"' || original[p] == '\'') {
        quote = original[p];
        ++p;
    }

    size_t end = p;
    while (end < original.size()) {
        char c = original[end];
        if (quote) {
            if (c == quote) break;
        } else if (c == ' ' || c == '\t' || c == ',' || c == '|') {
            break;
        }
        ++end;
    }
    return original.substr(p, end - p);
}

} // namespace detail

// Returns a hint string when `command` matches the
// Get-ChildItem -Include-without-wildcard/-Recurse footgun, else
// empty.  Heuristic and fail-safe: a missed detection just means no
// hint; a false positive is only ever a hint, never a behavior change.
//
// Callers should additionally gate on "command succeeded with no
// output / no workspace changes" before surfacing, so correct
// commands that happen to use -Include never get noise.
inline std::string GetChildItemIncludeHint(const std::string& command)
{
    std::string low = detail::LowerAscii(command);

    bool isGci = detail::ContainsToken(low, "get-childitem") ||
                 detail::ContainsToken(low, "gci");
    if (!isGci) return std::string();
    if (low.find("-include") == std::string::npos) return std::string();
    if (low.find("-recurse") != std::string::npos) return std::string();

    // If the -Path value already carries a wildcard, the command is
    // fine — do not hint.  When -Path is absent we conservatively
    // treat the implicit path (current directory, no wildcard) as
    // matching the footgun, because -Include still selects nothing.
    std::string pathArg = detail::ExtractPathArg(low, command);
    if (pathArg.find('*') != std::string::npos ||
        pathArg.find('?') != std::string::npos) {
        return std::string();
    }

    return
        "[hint] Get-ChildItem -Include matches NOTHING unless -Recurse "
        "is used or the -Path value ends with \\*. This command most "
        "likely selected zero files, so anything piped from it (e.g. "
        "Compress-Archive, Copy-Item) did nothing. Rewrite as "
        "-Path \"<dir>\\*\" -Include *.h,*.cpp (top level) or add "
        "-Recurse (subfolders too), or use -Filter for one pattern.";
}

} // namespace ps_command_hints
