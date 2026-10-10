// dir_walker_posix.cpp — POSIX directory walk (std::filesystem)
#include "dir_walker.h"
#include "str_util.h"
#include "str_case.h"

#include <filesystem>
#include <vector>
#include <algorithm>
#include <system_error>

namespace dir_walker {

static bool ShouldSkipDirDefault(std::string_view name) {
    if (!name.empty() && name[0] == '.') return true;
    static const char* const kBlacklist[] = {
        "node_modules", "x64", "x86", "Win32", "Debug", "Release",
        "bin", "obj", "vcpkg_installed", "target", "build",
        "cmake-build-debug", "cmake-build-release", "dist", "out", "__pycache__"
    };
    for (const char* b : kBlacklist) {
        if (str_case::iequals(name, b)) return true;
    }
    return false;
}

bool Walk(std::string_view root,
          const std::function<bool(const Entry&)>& fn,
          const std::function<bool(std::string_view dirName)>& skipDir) {

    using SkipDirFn = std::function<bool(std::string_view)>;
    SkipDirFn skip = skipDir ? SkipDirFn(skipDir) : SkipDirFn(ShouldSkipDirDefault);
    std::filesystem::path rootPath{std::string(root)};

    std::error_code ec;
    if (!std::filesystem::exists(rootPath, ec) || !std::filesystem::is_directory(rootPath, ec)) {
        return false;
    }

    // Use directory_iterator with manual control for sorting
    std::vector<std::filesystem::directory_entry> entries;
    for (auto it = std::filesystem::directory_iterator(rootPath, ec); it != std::filesystem::directory_iterator{}; ++it) {
        if (ec) break;
        entries.push_back(*it);
    }
    if (ec) return false;

    // Sort: files first, then dirs, alpha
    std::sort(entries.begin(), entries.end(),
        [](const auto& a, const auto& b) {
            bool aDir = a.is_directory();
            bool bDir = b.is_directory();
            if (aDir != bDir) return !aDir; // files first
            return a.path().filename() < b.path().filename();
        });

    for (const auto& entry : entries) {
        std::string name = entry.path().filename().string();
        bool isDir = entry.is_directory();
        uint64_t size = 0;

        if (!isDir) {
            size = entry.file_size(ec);
            if (ec) size = 0; ec.clear();
        } else if (skip(name)) {
            continue;
        }

        Entry e{name, name, isDir, size};
        if (!fn(e)) return true;

        if (isDir) {
            std::string childAbs = entry.path().string();
            std::function<bool(const Entry&)> childFn = [&](const Entry& child) {
                Entry e2{child.name, name + "/" + child.relPath, child.isDirectory, child.size};
                return fn(e2);
            };
            if (!Walk(childAbs, childFn, skip)) {
                return false;
            }
        }
    }
    return true;
}

} // namespace dir_walker
