// str_util_posix.cpp — POSIX UTF-8 native (no-op)
#include "str_util.h"

namespace str_util {

std::wstring Utf8ToWide(std::string_view) { return {}; }
std::string  WideToUtf8(std::wstring_view) { return {}; }

} // namespace str_util