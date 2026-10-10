// dir_walker_win.cpp — Windows directory walk (FindFirstFileW)
#include "dir_walker.h"
#include "str_util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vector>
#include <algorithm>

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

    auto skip = skipDir ? skipDir : ShouldSkipDirDefault;

    std::wstring wroot = str_util::Utf8ToWide(root);
    if (wroot.empty()) return false;

    // Ensure trailing backslash for FindFirstFileW pattern
    if (!wroot.empty() && wroot.back() != L'\\') wroot.push_back(L'\\');
    std::wstring pattern = wroot + L"*";

    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return false;

    // Collect entries for sorting (files first, then dirs, alpha)
    struct Item { std::string name; bool isDir; uint64_t size; };
    std::vector<Item> files, dirs;

    do {
        std::wstring wname(fd.cFileName);
        if (wname == L"." || wname == L"..") continue;

        std::string name = str_util::WideToUtf8(wname);
        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

        ULARGE_INTEGER sz;
        sz.HighPart = fd.nFileSizeHigh;
        sz.LowPart = fd.nFileSizeLow;

        if (isDir) {
            if (skip(name)) continue;
            dirs.push_back({std::move(name), true, 0});
        } else {
            files.push_back({std::move(name), false, sz.QuadPart});
        }
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);

    auto cmp = [](const Item& a, const Item& b) { return a.name < b.name; };
    std::sort(files.begin(), files.end(), cmp);
    std::sort(dirs.begin(), dirs.end(), cmp);

    // Files first
    for (const auto& f : files) {
        Entry e{f.name, f.name, false, f.size};
        if (!fn(e)) return true;
    }
    // Then recurse into dirs
    for (const auto& d : dirs) {
        Entry e{d.name, d.name, true, 0};
        if (!fn(e)) return true;

        std::string childRel = d.name;
        std::string childAbs = std::string(root);
        if (!childAbs.empty() && childAbs.back() != '/' && childAbs.back() != '\\')
            childAbs.push_back('\\');
        childAbs += d.name;

        if (!Walk(childAbs, [&](const Entry& child) {
            Entry e{child.name, childRel + "\\" + child.relPath, child.isDirectory, child.size};
            return fn(e);
        }, skip)) {
            return false;
        }
    }
    return true;
}

} // namespace dir_walker