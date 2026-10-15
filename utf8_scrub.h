// utf8_scrub.h
//
// Make an arbitrary byte string valid UTF-8 by replacing every invalid
// sequence with U+FFFD (EF BF BD).  Header-only, no dependencies.
//
// Why this exists
// ---------------
// Child-process output is not guaranteed to be UTF-8.  A Python started
// without -X utf8 writes stderr in the ANSI code page, so a curly
// apostrophe (U+2019) arrives as the single cp1252 byte 0x92.  One such
// byte in a tool result poisons the whole chat history: Poco's JSON
// parser rejects it ("JSON Exception: invalid UTF-8 character"), and the
// Responses conversion re-parses the history on every request, so every
// later send -- including `continue` -- fails the same way.
//
// Rules (RFC 3629 / Unicode "maximal subpart" substitution):
//   * overlong encodings, UTF-16 surrogates (U+D800..U+DFFF) and code
//     points above U+10FFFF are invalid;
//   * each maximal invalid subpart becomes exactly one U+FFFD, so a
//     truncated 3-byte sequence yields one replacement, not three.
// Valid input is returned unchanged (fast path: no copy is built until
// the first bad byte is found).
#pragma once

#include <cstddef>
#include <string>

namespace lb_utf8 {

// Length of the valid UTF-8 sequence starting at s[i], or 0 if the bytes
// at s[i] do not start a complete valid sequence.  When 0 is returned,
// *subpart receives the length of the maximal invalid subpart (>= 1).
inline std::size_t ValidSequenceLength(const std::string& s, std::size_t i,
                                       std::size_t* subpart)
{
    const std::size_t n = s.size();
    const unsigned char b0 = static_cast<unsigned char>(s[i]);
    *subpart = 1;
    if (b0 < 0x80) return 1;

    std::size_t len = 0;
    unsigned char lo = 0x80, hi = 0xBF;   // allowed range for the 2nd byte
    if (b0 >= 0xC2 && b0 <= 0xDF)      { len = 2; }
    else if (b0 == 0xE0)               { len = 3; lo = 0xA0; }
    else if (b0 >= 0xE1 && b0 <= 0xEC) { len = 3; }
    else if (b0 == 0xED)               { len = 3; hi = 0x9F; }   // no surrogates
    else if (b0 >= 0xEE && b0 <= 0xEF) { len = 3; }
    else if (b0 == 0xF0)               { len = 4; lo = 0x90; }
    else if (b0 >= 0xF1 && b0 <= 0xF3) { len = 4; }
    else if (b0 == 0xF4)               { len = 4; hi = 0x8F; }   // <= U+10FFFF
    else return 0;                     // 0x80..0xC1, 0xF5..0xFF

    for (std::size_t k = 1; k < len; ++k) {
        if (i + k >= n) { *subpart = k; return 0; }
        const unsigned char b = static_cast<unsigned char>(s[i + k]);
        const unsigned char min = (k == 1) ? lo : 0x80;
        const unsigned char max = (k == 1) ? hi : 0xBF;
        if (b < min || b > max) { *subpart = k; return 0; }
    }
    return len;
}

inline bool IsValid(const std::string& s)
{
    std::size_t sub = 0;
    for (std::size_t i = 0; i < s.size();) {
        const std::size_t len = ValidSequenceLength(s, i, &sub);
        if (len == 0) return false;
        i += len;
    }
    return true;
}

// Returns `s` with every invalid subpart replaced by U+FFFD.
// If `replaced` is non-null it receives the number of replacements.
inline std::string Scrub(const std::string& s, std::size_t* replaced = nullptr)
{
    if (replaced) *replaced = 0;
    std::size_t i = 0, sub = 0;
    // Fast path: scan until the first invalid byte.
    while (i < s.size()) {
        const std::size_t len = ValidSequenceLength(s, i, &sub);
        if (len == 0) break;
        i += len;
    }
    if (i == s.size()) return s;

    std::string out;
    out.reserve(s.size() + 16);
    out.append(s, 0, i);
    std::size_t count = 0;
    while (i < s.size()) {
        const std::size_t len = ValidSequenceLength(s, i, &sub);
        if (len) {
            out.append(s, i, len);
            i += len;
        } else {
            out += "\xEF\xBF\xBD";
            ++count;
            i += sub;
        }
    }
    if (replaced) *replaced = count;
    return out;
}

} // namespace lb_utf8
