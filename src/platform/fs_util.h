#pragma once
// fs_util.h — Filesystem metadata abstraction
// Windows: GetFileAttributesExW, GetFullPathNameW
// POSIX: std::filesystem

#include <string>
#include <string_view>
#include <optional>
#include <filesystem>

namespace fs_util {

struct FileInfo {
    bool exists = false;
    bool isDirectory = false;
    uint64_t size = 0;
    std::filesystem::file_time_type mtime;
};

// Get file info (exists, directory, size, mtime). Returns nullopt on hard error.
std::optional<FileInfo> GetFileInfo(std::string_view utf8Path);

// Convenience helpers
bool PathExists(std::string_view utf8Path);
bool IsDirectory(std::string_view utf8Path);
bool IsRegularFile(std::string_view utf8Path);
uint64_t FileSize(std::string_view utf8Path);

// Canonicalize path: resolve .., symlinks, make absolute
std::string CanonicalizePath(std::string_view utf8Path);

// Check if path is under root (after canonicalization)
bool IsPathUnderRoot(std::string_view path, std::string_view root);

} // namespace fs_util