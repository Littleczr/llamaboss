// platform_dirs_linux.cpp — Linux known folders (XDG Base Directory Spec)
#include "platform_dirs.h"

#include <cstdlib>
#include <filesystem>
#include <string>

namespace platform_dirs {

static std::string GetEnvOr(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : fallback;
}

static std::string GetHomeDir() {
    return GetEnvOr("HOME", "");
}

static std::string GetLlamaBossSubdir(const std::string& base, const char* subdir) {
    if (base.empty()) return "";
    std::filesystem::path p = base;
    p /= "LlamaBoss";
    if (subdir && *subdir) p /= subdir;
    return p.u8string();
}

std::string GetDataDir() {
    // $XDG_DATA_HOME or ~/.local/share
    std::string base = GetEnvOr("XDG_DATA_HOME", (GetHomeDir() + "/.local/share").c_str());
    return GetLlamaBossSubdir(base, nullptr);
}

std::string GetConfigDir() {
    // $XDG_CONFIG_HOME or ~/.config
    std::string base = GetEnvOr("XDG_CONFIG_HOME", (GetHomeDir() + "/.config").c_str());
    return GetLlamaBossSubdir(base, nullptr);
}

std::string GetCacheDir() {
    // $XDG_CACHE_HOME or ~/.cache
    std::string base = GetEnvOr("XDG_CACHE_HOME", (GetHomeDir() + "/.cache").c_str());
    return GetLlamaBossSubdir(base, nullptr);
}

std::string GetWorkspaceDir() {
    return GetLlamaBossSubdir(GetDataDir(), "Workspace");
}

std::string GetScriptsDir() {
    return GetLlamaBossSubdir(GetDataDir(), "Scripts");
}

std::string GetDocumentsDir() {
    return GetLlamaBossSubdir(GetDataDir(), "Documents");
}

std::string GetTempDir() {
    return GetEnvOr("TMPDIR", "/tmp");
}

} // namespace platform_dirs