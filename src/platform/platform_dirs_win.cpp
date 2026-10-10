// platform_dirs_win.cpp — Windows known folders
#include "platform_dirs.h"
#include "str_util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <filesystem>

namespace platform_dirs {

static std::string GetKnownFolder(REFKNOWNFOLDERID rfid) {
    PWSTR pszPath = nullptr;
    HRESULT hr = SHGetKnownFolderPath(rfid, 0, nullptr, &pszPath);
    if (FAILED(hr) || !pszPath) return {};
    std::string utf8 = str_util::WideToUtf8(pszPath);
    CoTaskMemFree(pszPath);
    return utf8;
}

static std::string GetLlamaBossSubdir(const char* subdir) {
    std::string base = GetKnownFolder(FOLDERID_LocalAppData);
    if (base.empty()) return {};
    std::filesystem::path p = base;
    p /= "LlamaBoss";
    if (subdir && *subdir) p /= subdir;
    return p.u8string();
}

std::string GetDataDir() {
    return GetLlamaBossSubdir(nullptr);
}

std::string GetConfigDir() {
    std::string base = GetKnownFolder(FOLDERID_RoamingAppData);
    if (base.empty()) return {};
    std::filesystem::path p = base;
    p /= "LlamaBoss";
    return p.u8string();
}

std::string GetCacheDir() {
    return GetLlamaBossSubdir("Cache");
}

std::string GetWorkspaceDir() {
    return GetLlamaBossSubdir("Workspace");
}

std::string GetScriptsDir() {
    return GetLlamaBossSubdir("Scripts");
}

std::string GetDocumentsDir() {
    return GetLlamaBossSubdir("Documents");
}

std::string GetTempDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, buf);
    if (n == 0 || n > MAX_PATH) return {};
    return str_util::WideToUtf8(buf, (int)n);
}

} // namespace platform_dirs