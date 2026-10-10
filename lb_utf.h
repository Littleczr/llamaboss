// lb_utf.h
//
// Portable UTF-8 <-> std::wstring conversion for non-Windows builds.
//
// On Windows, wchar_t is UTF-16 and the Win32 MultiByteToWideChar /
// WideCharToMultiByte calls do this job. On macOS/Linux wchar_t is
// 32 bits, so a wide string holds one Unicode code point per element.
// Malformed input is replaced with U+FFFD, matching what the Win32 calls
// do when given no flags.
#pragma once

#include <string>

namespace lb_utf {

inline std::wstring Utf8ToWide(const std::string& in)
{
    std::wstring out;
    out.reserve(in.size());
    const auto* s = reinterpret_cast<const unsigned char*>(in.data());
    const std::size_t n = in.size();
    std::size_t i = 0;
    while (i < n) {
        const unsigned char c = s[i];
        char32_t cp = 0;
        std::size_t len = 0;
        if (c < 0x80)                { cp = c;        len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else { out.push_back(L'�'); ++i; continue; }

        bool ok = i + len <= n;
        for (std::size_t k = 1; ok && k < len; ++k) {
            if ((s[i + k] & 0xC0) != 0x80) ok = false;
            else cp = (cp << 6) | (s[i + k] & 0x3F);
        }
        // Reject overlong forms, surrogates and out-of-range values.
        if (ok && ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
                   (len == 4 && cp < 0x10000) || cp > 0x10FFFF ||
                   (cp >= 0xD800 && cp <= 0xDFFF)))
            ok = false;
        if (!ok) { out.push_back(L'�'); ++i; continue; }
        out.push_back(static_cast<wchar_t>(cp));
        i += len;
    }
    return out;
}

inline std::string WideToUtf8(const std::wstring& in)
{
    std::string out;
    out.reserve(in.size());
    for (wchar_t wc : in) {
        auto cp = static_cast<char32_t>(wc);
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

} // namespace lb_utf
