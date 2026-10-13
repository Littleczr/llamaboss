#pragma once

// ─── lb_ascii.h ────────────────────────────────────────────────────
// Locale-independent ASCII character classes and case mapping.
//
// <cctype> consults the C locale for bytes >= 0x80.  wxWidgets sets
// LC_CTYPE to "UTF-8" at startup on macOS, and there isspace(0xA0),
// isalpha(0xC3) are true and tolower(0xC3) == 0xE3, so byte-wise use
// corrupts UTF-8 text: U+00E0 'à' is C3 A0, U+26A0 '⚠' is E2 9A A0.
// The Windows "C" locale treats every byte >= 0x80 as a non-letter, which
// is what LlamaBoss's byte-wise parsers, trims and lowercasing assume;
// these helpers give that behaviour on every platform.
//
// Arguments are ints like <cctype>: pass a char, an unsigned char or EOF.
// Anything outside 0..127 is not ASCII and is returned unchanged by
// ToLower/ToUpper, so a negative (signed) char is safe too.

namespace lb_ascii {

inline bool IsSpace(int c)  { return c == ' ' || (c >= '\t' && c <= '\r'); }
inline bool IsDigit(int c)  { return c >= '0' && c <= '9'; }
inline bool IsUpper(int c)  { return c >= 'A' && c <= 'Z'; }
inline bool IsLower(int c)  { return c >= 'a' && c <= 'z'; }
inline bool IsAlpha(int c)  { return IsUpper(c) || IsLower(c); }
inline bool IsAlnum(int c)  { return IsAlpha(c) || IsDigit(c); }
inline bool IsXDigit(int c) { return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
inline bool IsPunct(int c)  { return c > ' ' && c < 0x7F && !IsAlnum(c); }
inline bool IsPrint(int c)  { return c >= ' ' && c < 0x7F; }
inline bool IsGraph(int c)  { return c > ' ' && c < 0x7F; }
inline bool IsCntrl(int c)  { return (c >= 0 && c < ' ') || c == 0x7F; }

inline int ToLower(int c) { return IsUpper(c) ? c + ('a' - 'A') : c; }
inline int ToUpper(int c) { return IsLower(c) ? c - ('a' - 'A') : c; }

} // namespace lb_ascii
