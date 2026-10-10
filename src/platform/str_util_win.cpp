// str_util_win.cpp — Windows UTF-8 ↔ UTF-16 implementation
#include "str_util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace str_util {

std::wstring Utf8ToWide(std::string_view in) {
    if (in.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, in.data(), (int)in.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, in.data(), (int)in.size(), out.data(), n);
    return out;
}

std::string WideToUtf8(std::wstring_view in) {
    if (in.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, in.data(), (int)in.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, in.data(), (int)in.size(), out.data(), n, nullptr, nullptr);
    return out;
}

} // namespace str_util