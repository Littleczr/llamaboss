// tool_grep_args.h
//
// UI-free parsing for grep's freeform/native argument projection. Keeping this
// small parser outside tool_router.cpp lets the native regression runner test
// the exact production behavior without linking every tool implementation.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>

namespace tool_grep_args {

struct ParsedGrepArgs {
    std::string pattern;
    std::string path;
    size_t      contextLines = 0;
    std::string error;
};

inline std::string Trim(const std::string& value)
{
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

inline bool ParseQuotedPattern(const std::string& text,
                               std::string& patternOut,
                               std::string& remainderOut,
                               std::string& errorOut)
{
    if (text.empty() || (text.front() != '"' && text.front() != '\''))
        return false;

    const char quote = text.front();
    size_t close = 1;
    for (; close < text.size(); ++close) {
        if (text[close] == quote && text[close - 1] != '\\') break;
    }
    if (close >= text.size()) {
        errorOut = "Unterminated quoted grep pattern.";
        return true;
    }

    if (close + 1 < text.size() &&
        !std::isspace(static_cast<unsigned char>(text[close + 1]))) {
        errorOut = "Quoted grep pattern must be followed by whitespace.";
        return true;
    }

    patternOut = text.substr(1, close - 1);
    remainderOut = Trim(close + 1 < text.size()
        ? text.substr(close + 1) : std::string());
    return true;
}

inline ParsedGrepArgs Parse(const std::string& args,
                            size_t maxContextLines)
{
    ParsedGrepArgs out;
    std::string s = Trim(args);
    bool patternWasLineDelimited = false;

    // True only when the private "@context=" marker was consumed, which
    // means these args were PROJECTED from a structured native tool call
    // ({pattern, path, context}) rather than typed freeform.  On that
    // path the pattern is the JSON value verbatim, so a leading quote is
    // a literal byte to search for ("path":, 'use strict', ...) and the
    // shell-style quote heuristic below must not run.  Applying it there
    // rejected valid patterns outright ("Quoted grep pattern must be
    // followed by whitespace") and, in the no-path shape, silently
    // stripped the quotes so "alpha" searched for alpha instead.
    bool nativeStructured = false;

    if (s.empty()) {
        out.error = "grep requires a non-empty pattern";
        return out;
    }

    auto parseContext = [&](const std::string& token) -> bool {
        if (token.empty() || !std::all_of(token.begin(), token.end(),
                [](unsigned char c){ return std::isdigit(c); })) {
            out.error = "grep -C context must be an integer from 0 to "
                + std::to_string(maxContextLines);
            return false;
        }
        try {
            const unsigned long long value = std::stoull(token);
            if (value > maxContextLines) {
                out.error = "grep -C context exceeds maximum "
                    + std::to_string(maxContextLines);
                return false;
            }
            out.contextLines = static_cast<size_t>(value);
            return true;
        } catch (...) {
            out.error = "grep -C context must be an integer from 0 to "
                + std::to_string(maxContextLines);
            return false;
        }
    };

    constexpr const char* kNativeMarker = "@context=";
    if (s.compare(0, 9, kNativeMarker) == 0) {
        const size_t nl = s.find_first_of("\r\n");
        if (nl == std::string::npos ||
            !parseContext(Trim(s.substr(9, nl - 9)))) {
            if (out.error.empty())
                out.error = "native grep context marker requires a pattern on the next line";
            return out;
        }
        size_t rest = nl;
        while (rest < s.size() && (s[rest] == '\r' || s[rest] == '\n')) ++rest;
        s = Trim(rest < s.size() ? s.substr(rest) : std::string());
        patternWasLineDelimited = true;
        nativeStructured = true;
    } else if (s.size() >= 2 && s[0] == '-' && s[1] == 'C' &&
               (s.size() == 2 || std::isdigit(static_cast<unsigned char>(s[2])) ||
                std::isspace(static_cast<unsigned char>(s[2])) ||
                s[2] == '-' || s[2] == '+')) {
        size_t pos = 2;
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) ++pos;
        size_t end = pos;
        while (end < s.size() && std::isdigit(static_cast<unsigned char>(s[end]))) ++end;
        if (!parseContext(s.substr(pos, end - pos))) return out;
        if (end < s.size() && !std::isspace(static_cast<unsigned char>(s[end]))) {
            out.error = "grep -C context must be followed by whitespace";
            return out;
        }
        patternWasLineDelimited = end < s.size() &&
            (s[end] == '\r' || s[end] == '\n');
        s = Trim(end < s.size() ? s.substr(end) : std::string());
    }

    const size_t nl = s.find_first_of("\r\n");
    if (nl != std::string::npos) {
        const std::string firstLine = Trim(s.substr(0, nl));
        size_t rest = nl;
        while (rest < s.size() && (s[rest] == '\r' || s[rest] == '\n')) ++rest;
        out.path = Trim(rest < s.size() ? s.substr(rest) : std::string());

        if (nativeStructured) {
            // Projected native call: the pattern occupies this whole
            // line by construction and the path is on the next one.
            out.pattern = firstLine;
        } else {
            std::string quotedRemainder;
            if (ParseQuotedPattern(firstLine, out.pattern, quotedRemainder, out.error)) {
                if (out.error.empty() && !quotedRemainder.empty())
                    out.error = "Quoted grep pattern must occupy its complete line.";
            } else {
                out.pattern = firstLine;
            }
        }
    } else {
        if (nativeStructured) {
            // Projected native call with the path omitted: everything
            // after the marker line is the pattern.
            out.pattern = s;
        } else {
            std::string quotedRemainder;
            if (ParseQuotedPattern(s, out.pattern, quotedRemainder, out.error)) {
                out.path = quotedRemainder;
            } else {
                const size_t sep = patternWasLineDelimited
                    ? std::string::npos : s.find_first_of(" \t");
                if (sep == std::string::npos) {
                    out.pattern = Trim(s);
                } else {
                    out.pattern = Trim(s.substr(0, sep));
                    out.path = Trim(s.substr(sep + 1));
                }
            }
        }
    }

    if (out.error.empty() && out.pattern.empty())
        out.error = "Empty grep pattern.";
    return out;
}

} // namespace tool_grep_args
