// markdown_image_line.h
//
// Parser for assistant lines that consist only of Markdown images:
//
//   ![Before](C:\Users\Cesar\LlamaBoss\Chats\...\Workspace\before.png)
//   ![Before](before.png) ![After](after.png)
//   ![chart](<C:\Program Files (x86)\Some App\chart.png> "optional title")
//
// MarkdownRenderer turns such lines into inline thumbnails (via a
// ChatDisplay callback that resolves and loads the file).  Lines that mix
// images with other prose are NOT matched and render as ordinary text, so
// nothing a model writes mid-sentence changes shape.
//
// Tolerances, because models write Windows paths into Markdown without
// escaping: backslashes, spaces, and parentheses inside the target are
// accepted.  A target ends at the ')' that is followed only by whitespace
// and either the end of the line or the next "![".  <...> wrapping, a
// trailing "title", file:/// prefixes and %XX escapes are handled.
//
// Header-only, no wx dependency (unit-tested off Windows).
//
#pragma once
#include "lb_ascii.h"

#include <cctype>
#include <string>
#include <vector>

namespace markdown_image_line {

struct ImageRef {
    std::string alt;
    std::string src;    // cleaned target: unwrapped, title removed, decoded
};

inline std::string Trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

inline std::string PercentDecode(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0) {
            out.push_back(static_cast<char>(hex(s[i + 1]) * 16 + hex(s[i + 2])));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

inline std::string CleanTarget(std::string t)
{
    t = Trim(t);
    if (t.size() >= 2 && t.front() == '<') {
        const size_t close = t.find('>');
        if (close != std::string::npos) t = t.substr(1, close - 1);
    } else {
        // Optional title: whitespace + "..." or '...' at the end.
        if (t.size() >= 3 && (t.back() == '"' || t.back() == '\'')) {
            const char q = t.back();
            const size_t open = t.rfind(q, t.size() - 2);
            if (open != std::string::npos && open > 0 &&
                (t[open - 1] == ' ' || t[open - 1] == '\t')) {
                t = Trim(t.substr(0, open));
            }
        }
    }
    t = Trim(t);
    // file:///C:/x or file://C:/x -> C:/x
    std::string lower = t;
    for (char& c : lower) c = static_cast<char>(lb_ascii::ToLower(static_cast<unsigned char>(c)));
    if (lower.rfind("file:///", 0) == 0)      t = t.substr(8);
    else if (lower.rfind("file://", 0) == 0)  t = t.substr(7);
    if (t.find('%') != std::string::npos) t = PercentDecode(t);
    return t;
}

// True when the whole (trimmed) line is one or more images and nothing else.
inline bool Parse(const std::string& line, std::vector<ImageRef>& out)
{
    out.clear();
    const std::string s = Trim(line);
    if (s.size() < 5 || s.compare(0, 2, "![") != 0) return false;

    size_t pos = 0;
    while (pos < s.size()) {
        if (s.compare(pos, 2, "![") != 0) { out.clear(); return false; }
        const size_t altEnd = s.find("](", pos + 2);
        if (altEnd == std::string::npos) { out.clear(); return false; }
        const std::string alt = s.substr(pos + 2, altEnd - pos - 2);
        if (alt.find('\n') != std::string::npos) { out.clear(); return false; }

        const size_t targetStart = altEnd + 2;
        size_t close = std::string::npos;
        for (size_t i = targetStart; i < s.size(); ++i) {
            if (s[i] != ')') continue;
            size_t j = i + 1;
            while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) ++j;
            if (j == s.size() || s.compare(j, 2, "![") == 0) { close = i; break; }
        }
        if (close == std::string::npos) { out.clear(); return false; }

        ImageRef ref;
        ref.alt = Trim(alt);
        ref.src = CleanTarget(s.substr(targetStart, close - targetStart));
        if (ref.src.empty()) { out.clear(); return false; }
        out.push_back(std::move(ref));

        pos = close + 1;
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) ++pos;
    }
    return !out.empty();
}

// http(s)/data/ftp targets are never fetched or decoded by the transcript.
inline bool IsRemoteOrInline(const std::string& src)
{
    std::string lower = src;
    for (char& c : lower) c = static_cast<char>(lb_ascii::ToLower(static_cast<unsigned char>(c)));
    return lower.rfind("http://", 0) == 0 || lower.rfind("https://", 0) == 0 ||
           lower.rfind("data:", 0) == 0   || lower.rfind("ftp://", 0) == 0;
}

} // namespace markdown_image_line
