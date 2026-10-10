// module_path_win.cpp — Windows GetModuleFileNameW
#include "module_path.h"
#include "str_util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <filesystem>

namespace module_path {

std::string GetExeDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return {};
    std::string utf8 = str_util::WideToUtf8(buf, (int)n);
    std::filesystem::path p = std::filesystem::u8path(utf8);
    return p.parent_path().u8string();
}

} // namespace module_path