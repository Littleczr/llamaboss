#pragma once
// platform_dirs.h — Known folder locations (AppData, Documents, etc.)
// Windows: SHGetKnownFolderPath (FOLDERID_LocalAppData, FOLDERID_RoamingAppData)
// Linux: XDG Base Directory Specification
// macOS: ~/Library/Application Support, ~/Library/Caches

#include <string>
#include <string_view>
#include <optional>

namespace platform_dirs {

// Get the LlamaBoss data directory (where settings, secrets, workspace live)
std::string GetDataDir();           // %LOCALAPPDATA%\LlamaBoss (Win), ~/.local/share/LlamaBoss (Linux), ~/Library/Application Support/LlamaBoss (macOS)
std::string GetConfigDir();         // %APPDATA%\LlamaBoss (Win), ~/.config/LlamaBoss (Linux), ~/Library/Application Support/LlamaBoss (macOS)
std::string GetCacheDir();          // %LOCALAPPDATA%\LlamaBoss\Cache (Win), ~/.cache/LlamaBoss (Linux), ~/Library/Caches/LlamaBoss (macOS)
std::string GetWorkspaceDir();      // DataDir/Workspace
std::string GetScriptsDir();        // DataDir/Scripts
std::string GetDocumentsDir();      // DataDir/Documents
std::string GetTempDir();           // System temp (GetTempPath / /tmp / NSTemporaryDirectory)

} // namespace platform_dirs