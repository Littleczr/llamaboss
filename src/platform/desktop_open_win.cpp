// desktop_open_win.cpp — Windows ShellExecuteExW
#include "desktop_open.h"
#include "str_util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

namespace desktop_open {

bool Open(std::string_view pathOrUrl) {
    std::wstring wpath = str_util::Utf8ToWide(pathOrUrl);
    if (wpath.empty()) return false;

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"open";
    sei.lpFile = wpath.c_str();
    sei.nShow = SW_SHOWNORMAL;

    return ShellExecuteExW(&sei) != FALSE;
}

} // namespace desktop_open