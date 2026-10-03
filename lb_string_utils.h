#pragma once

// ─── lb_string_utils.h ─────────────────────────────────────────────
// Tiny ASCII-only string helpers shared across the file-local support
// modules extracted out of LlamaBoss.cpp.  These intentionally avoid
// wxWidgets, Poco, and locale-aware behaviour so they can live in any
// translation unit without dragging UI/network headers along.
//
// Use these instead of std::tolower / locale-aware trim helpers when
// the input is known to be ASCII (control protocol tokens, file
// extensions, etc.) so behaviour stays stable regardless of the
// current C locale.

#include <cctype>
#include <string>

// Human-readable byte formatter used by project/source prompt summaries.
// Kept out of LlamaBoss.cpp so extracted controllers can share it without
// depending on frame-local anonymous-namespace helpers.
std::string ProjectSource_HumanBytes(unsigned long long bytes);

inline std::string LbLowerAscii(std::string s)
{
    for (char& ch : s)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

inline std::string LbTrimAscii(std::string s)
{
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// ── Prompt clipping ───────────────────────────────────────────────
// Clip long text for a prompt by keeping the head (2/3) and tail (1/3)
// with a "[middle truncated]" marker between them.  Cut points never
// split a UTF-8 multi-byte sequence.  Used by the Skill draft builder.
namespace lb_string_utils_detail {

inline bool IsUtf8ContinuationByte(unsigned char c)
{
    return (c & 0xC0) == 0x80;
}

inline size_t Utf8SafePrefixLen(const std::string& text, size_t maxBytes)
{
    if (maxBytes >= text.size()) return text.size();
    size_t cut = maxBytes;
    while (cut > 0 &&
           IsUtf8ContinuationByte(static_cast<unsigned char>(text[cut]))) {
        --cut;
    }
    return cut;
}

inline size_t Utf8SafeSuffixStart(const std::string& text, size_t maxBytes)
{
    if (maxBytes >= text.size()) return 0;
    size_t start = text.size() - maxBytes;
    while (start < text.size() &&
           IsUtf8ContinuationByte(static_cast<unsigned char>(text[start]))) {
        ++start;
    }
    return start;
}

} // namespace lb_string_utils_detail

// Byte-budget prefix that never ends mid-character.  Use this for every
// fixed-size cut of UTF-8 text (echo lines, titles, summaries, previews):
// a split multi-byte sequence that reaches ChatHistory makes llama-server
// reject the whole request, and wxString::FromUTF8 turns it into "".
inline size_t LbUtf8SafePrefixLen(const std::string& text, size_t maxBytes)
{
    return lb_string_utils_detail::Utf8SafePrefixLen(text, maxBytes);
}

inline std::string LbUtf8SafeTruncate(const std::string& text, size_t maxBytes)
{
    if (text.size() <= maxBytes) return text;
    return text.substr(0, LbUtf8SafePrefixLen(text, maxBytes));
}

inline std::string LbClipForPrompt(const std::string& text, size_t maxBytes)
{
    using namespace lb_string_utils_detail;
    if (text.size() <= maxBytes) return text;

    const std::string marker = "\n... [middle truncated] ...\n";
    if (maxBytes <= marker.size() + 2) {
        return text.substr(0, Utf8SafePrefixLen(text, maxBytes));
    }

    const size_t keepBytes = maxBytes - marker.size();
    const size_t headBytes = (keepBytes * 2) / 3;
    const size_t tailBytes = keepBytes - headBytes;

    return text.substr(0, Utf8SafePrefixLen(text, headBytes))
        + marker
        + text.substr(Utf8SafeSuffixStart(text, tailBytes));
}
