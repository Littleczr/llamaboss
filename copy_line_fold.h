#pragma once
// ═══════════════════════════════════════════════════════════════════
//  copy_line_fold.h — fold runs of "<src> -> <dst> done" lines
// ═══════════════════════════════════════════════════════════════════
//
// MSBuild + vcpkg app-local deployment prints one line per dependency
// copied next to the built binary:
//   C:\...\vcpkg_installed\...\bin\PocoFoundation.dll -> C:\...\out\PocoFoundation.dll done
// 2026-10-01 r16c1 session: 198 such lines, 35.8 KB, ~23% of the whole
// transcript, ~4 KB per build-and-test run.  Each line only says a copy
// SUCCEEDED; the model never needs them individually.
//
// FoldCopyProgressLines() replaces every run of >= kMinRun consecutive
// such lines with ONE summary line:
//   [42 file-copy lines folded: PocoFoundation.dll, pcre2-8.dll, ... -> C:\...\out\]
// Matching is by output SHAPE, never by command: a line qualifies when it
// contains " -> ", ends with " done" (trailing whitespace/CR ignored),
// and contains neither "error" nor "warning" (any case).  So these always
// survive verbatim:
//   * the build-output line "X.vcxproj -> C:\...\X.exe" (no " done"),
//   * copy failures (MSB3021 "Unable to copy ..." etc.),
//   * compiler warnings/errors,
//   * short runs (< kMinRun) -- folding 1-2 lines saves nothing.
// A non-matching line in the middle of a run splits it.
//
// Applied to captured stdout/stderr in cmd_executor before large-output
// handling; logs the command itself wrote (Tee-Object etc.) are untouched.
// No dependencies: tested in copy_line_fold_tests.cpp.

#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

namespace lb_copyfold {

constexpr std::size_t kMinRun       = 3;
constexpr std::size_t kNamesListed  = 6;

namespace detail {

inline std::string Lower(std::string s)
{
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

inline std::string TrimRight(const std::string& s)
{
    std::size_t e = s.size();
    while (e > 0 && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(0, e);
}

inline std::string BaseName(const std::string& p)
{
    std::string t = p;
    while (!t.empty() && (t.back() == ' ' || t.back() == '\\' || t.back() == '/')) t.pop_back();
    const std::size_t s = t.find_last_of("\\/");
    std::string b = (s == std::string::npos) ? t : t.substr(s + 1);
    std::size_t lead = 0;
    while (lead < b.size() && b[lead] == ' ') ++lead;
    return b.substr(lead);
}

inline std::string DirName(const std::string& p)
{
    const std::size_t s = p.find_last_of("\\/");
    return (s == std::string::npos) ? std::string() : p.substr(0, s + 1);
}

// Parses one line; fills src/dstDir and returns true if it qualifies.
inline bool ParseCopyLine(const std::string& rawLine, std::string& src, std::string& dstDir)
{
    const std::string line = TrimRight(rawLine);
    static const std::string kDone = " done";
    if (line.size() <= kDone.size() ||
        line.compare(line.size() - kDone.size(), kDone.size(), kDone) != 0)
        return false;
    const std::size_t arrow = line.find(" -> ");
    if (arrow == std::string::npos) return false;
    const std::string low = Lower(line);
    if (low.find("error") != std::string::npos || low.find("warning") != std::string::npos)
        return false;
    src = line.substr(0, arrow);
    const std::string dst = line.substr(arrow + 4, line.size() - kDone.size() - (arrow + 4));
    dstDir = DirName(dst);
    return !BaseName(src).empty();
}

} // namespace detail

// Folds in place; returns the number of lines removed (0 = unchanged).
// Line endings of surviving lines are preserved; the summary line uses
// the line ending of the run's first line.
inline std::size_t FoldCopyProgressLines(std::string& text)
{
    if (text.find(" done") == std::string::npos) return 0;   // fast path

    // Split keeping terminators.
    std::vector<std::string> lines;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) { lines.push_back(text.substr(pos)); break; }
        lines.push_back(text.substr(pos, nl + 1 - pos));
        pos = nl + 1;
    }

    std::string out;
    out.reserve(text.size());
    std::size_t removed = 0;
    std::size_t i = 0;
    while (i < lines.size()) {
        std::string src, dstDir;
        if (!detail::ParseCopyLine(lines[i], src, dstDir)) { out += lines[i]; ++i; continue; }

        std::size_t j = i;
        std::vector<std::string> names;
        std::string commonDir = dstDir;
        bool sameDir = true;
        while (j < lines.size()) {
            std::string s2, d2;
            if (!detail::ParseCopyLine(lines[j], s2, d2)) break;
            names.push_back(detail::BaseName(s2));
            if (d2 != commonDir) sameDir = false;
            ++j;
        }
        const std::size_t run = j - i;
        if (run < kMinRun) {
            for (std::size_t k = i; k < j; ++k) out += lines[k];
            i = j;
            continue;
        }

        const std::string& first = lines[i];
        std::size_t indent = 0;
        while (indent < first.size() && first[indent] == ' ') ++indent;
        const bool crlf = first.size() >= 2 && first[first.size() - 2] == '\r';
        const bool hasNl = !first.empty() && first.back() == '\n';

        std::string summary(indent, ' ');
        summary += "[" + std::to_string(run) + " file-copy lines folded: ";
        for (std::size_t k = 0; k < names.size() && k < kNamesListed; ++k) {
            if (k) summary += ", ";
            summary += names[k];
        }
        if (names.size() > kNamesListed)
            summary += ", ... (+" + std::to_string(names.size() - kNamesListed) + " more)";
        summary += sameDir && !commonDir.empty() ? " -> " + commonDir : " -> various destinations";
        summary += "]";
        // If the last line of the run had no terminator, keep it that way.
        const bool lastHasNl = !lines[j - 1].empty() && lines[j - 1].back() == '\n';
        if (hasNl && lastHasNl) summary += crlf ? "\r\n" : "\n";
        out += summary;

        removed += run - 1;
        i = j;
    }

    if (removed) text.swap(out);
    return removed;
}

} // namespace lb_copyfold
