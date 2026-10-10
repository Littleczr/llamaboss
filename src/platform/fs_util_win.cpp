// fs_util_win.cpp — Windows filesystem implementation
#include "fs_util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <optional>
#include <filesystem>

namespace fs_util {

static std::wstring ToWide(std::string_view utf8) {
    if (utf8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), out.data(), n);
    return out;
}

static std::string ToUtf8(const wchar_t* w, int len = -1) {
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, len, out.data(), n, nullptr, nullptr);
    return out;
}

std::optional<FileInfo> GetFileInfo(std::string_view utf8Path) {
    std::wstring wpath = ToWide(utf8Path);
    if (wpath.empty()) return std::nullopt;

    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(wpath.c_str(), GetFileExInfoStandard, &data)) {
        return std::nullopt;
    }

    FileInfo info;
    info.exists = true;
    info.isDirectory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

    ULARGE_INTEGER sz;
    sz.HighPart = data.nFileSizeHigh;
    sz.LowPart = data.nFileSizeLow;
    info.size = sz.QuadPart;

    FILETIME ft = data.ftLastWriteTime;
    uint64_t ticks = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    // Convert Windows FILETIME (100ns since 1601) to std::filesystem::file_time_type
    // This is approximate; for exact conversion use std::chrono::file_clock
    info.mtime = std::filesystem::file_time_type(
        std::chrono::duration_cast<std::filesystem::file_time_type::duration>(
            std::chrono::nanoseconds(ticks * 100)
        )
    );
    return info;
}

bool PathExists(std::string_view p) {
    auto info = GetFileInfo(p);
    return info.has_value() && info->exists;
}

bool IsDirectory(std::string_view p) {
    auto info = GetFileInfo(p);
    return info.has_value() && info->isDirectory;
}

bool IsRegularFile(std::string_view p) {
    auto info = GetFileInfo(p);
    return info.has_value() && !info->isDirectory;
}

uint64_t FileSize(std::string_view p) {
    auto info = GetFileInfo(p);
    return info.has_value() ? info->size : 0;
}

std::string CanonicalizePath(std::string_view utf8Path) {
    std::wstring wpath = ToWide(utf8Path);
    if (wpath.empty()) return {};

    DWORD needed = GetFullPathNameW(wpath.c_str(), 0, nullptr, nullptr);
    if (needed == 0) return {};

    std::wstring buf(needed, L'\0');
    DWORD got = GetFullPathNameW(wpath.c_str(), needed, buf.data(), nullptr);
    if (got == 0 || got >= needed) return {};

    return ToUtf8(buf.c_str(), (int)got);
}

bool IsPathUnderRoot(std::string_view path, std::string_view root) {
    std::string cpath = CanonicalizePath(path);
    std::string croot = CanonicalizePath(root);
    if (cpath.empty() || croot.empty()) return false;

    // Normalize separators to backslash for comparison
    for (char& c : cpath) if (c == '/') c = '\\';
    for (char& c : croot) if (c == '/') c = '\\';

    if (croot.back() != '\\') croot.push_back('\\');
    return cpath.size() > croot.size() && cpath.compare(0, croot.size(), croot) == 0;
}

} // namespace fs_util