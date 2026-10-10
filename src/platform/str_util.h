#pragma once
// str_util.h — UTF-8 ↔ UTF-16 conversion
// Windows: MultiByteToWideChar / WideCharToMultiByte (CP_UTF8)
// POSIX: no-op (UTF-8 native)

#include <string>
#include <string_view>

namespace str_util {

// Convert UTF-8 string to UTF-16 (wstring). On POSIX, returns empty (unused).
std::wstring Utf8ToWide(std::string_view in);

// Convert UTF-16 (wstring) to UTF-8 string. On POSIX, returns empty (unused).
std::string  WideToUtf8(std::wstring_view in);

// POSIX no-op passthroughs — UTF-8 is native
inline std::string Utf8ToNative(std::string_view in) { return std::string(in); }
inline std::string NativeToUtf8(std::string_view in) { return std::string(in); }

} // namespace str_util