// fs_util_posix.cpp — POSIX filesystem implementation (std::filesystem)
#include "fs_util.h"

#include <filesystem>
#include <system_error>

namespace fs_util {

static std::filesystem::path ToPath(std::string_view utf8) {
    return std::filesystem::path(std::string(utf8));
}

std::optional<FileInfo> GetFileInfo(std::string_view utf8Path) {
    std::error_code ec;
    std::filesystem::path p = ToPath(utf8Path);
    std::filesystem::file_status status = std::filesystem::status(p, ec);

    if (ec || !std::filesystem::exists(status)) {
        return std::nullopt;
    }

    FileInfo info;
    info.exists = true;
    info.isDirectory = std::filesystem::is_directory(status);

    if (!info.isDirectory) {
        info.size = std::filesystem::file_size(p, ec);
        if (ec) info.size = 0;
    }

    info.mtime = std::filesystem::last_write_time(p, ec);
    if (ec) info.mtime = std::filesystem::file_time_type{};

    return info;
}

bool PathExists(std::string_view p) {
    std::error_code ec;
    return std::filesystem::exists(ToPath(p), ec);
}

bool IsDirectory(std::string_view p) {
    std::error_code ec;
    return std::filesystem::is_directory(ToPath(p), ec);
}

bool IsRegularFile(std::string_view p) {
    std::error_code ec;
    return std::filesystem::is_regular_file(ToPath(p), ec);
}

uint64_t FileSize(std::string_view p) {
    std::error_code ec;
    return std::filesystem::file_size(ToPath(p), ec);
}

std::string CanonicalizePath(std::string_view utf8Path) {
    std::error_code ec;
    std::filesystem::path p = std::filesystem::weakly_canonical(ToPath(utf8Path), ec);
    if (ec) return {};
    // Return as UTF-8 string
    return p.string();
}

bool IsPathUnderRoot(std::string_view path, std::string_view root) {
    std::string cpath = CanonicalizePath(path);
    std::string croot = CanonicalizePath(root);
    if (cpath.empty() || croot.empty()) return false;

    // Ensure root ends with separator
    if (!croot.empty() && croot.back() != '/' && croot.back() != '\\') {
        croot.push_back('/');
    }
    return cpath.size() > croot.size() && cpath.compare(0, croot.size(), croot) == 0;
}

} // namespace fs_util