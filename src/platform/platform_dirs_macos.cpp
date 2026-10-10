// platform_dirs_macos.cpp — macOS known folders
#include "platform_dirs.h"

#include <cstdlib>
#include <filesystem>
#include <string>

namespace platform_dirs {

static std::string GetHomeDir() {
    const char* home = std::getenv("HOME");
    return home ? std::string(home) : "";
}

static std::string GetLlamaBossSubdir(const std::string& base, const char* subdir) {
    if (base.empty()) return "";
    std::filesystem::path p = base;
    p /= "LlamaBoss";
    if (subdir && *subdir) p /= subdir;
    return p.string();
}

std::string GetDataDir() {
    std::string base = GetHomeDir();
    if (base.empty()) return "";
    return GetLlamaBossSubdir(base + "/Library/Application Support", nullptr);
}

std::string GetConfigDir() {
    // On macOS, config and data are the same (Application Support)
    return GetDataDir();
}

std::string GetCacheDir() {
    std::string base = GetHomeDir();
    if (base.empty()) return "";
    return GetLlamaBossSubdir(base + "/Library/Caches", nullptr);
}

std::string GetWorkspaceDir() {
    std::string base = GetHomeDir();
    if (base.empty()) return "";
    return GetLlamaBossSubdir(base + "/Library/Application Support", "Workspace");
}

std::string GetScriptsDir() {
    std::string base = GetHomeDir();
    if (base.empty()) return "";
    return GetLlamaBossSubdir(base + "/Library/Application Support", "Scripts");
}

std::string GetDocumentsDir() {
    std::string base = GetHomeDir();
    if (base.empty()) return "";
    return GetLlamaBossSubdir(base + "/Library/Application Support", "Documents");
}

std::string GetTempDir() {
    const char* tmp = std::getenv("TMPDIR");
    return tmp ? std::string(tmp) : "/tmp";
}

} // namespace platform_dirs