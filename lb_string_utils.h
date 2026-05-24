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
