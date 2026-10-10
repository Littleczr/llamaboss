#pragma once
// dir_walker.h — Directory walk abstraction
// Windows: FindFirstFileW / FindNextFileW
// POSIX: std::filesystem::recursive_directory_iterator

#include <string>
#include <string_view>
#include <functional>

namespace dir_walker {

struct Entry {
    std::string name;       // filename only (UTF-8)
    std::string relPath;    // relative to walk root (UTF-8)
    bool isDirectory = false;
    uint64_t size = 0;
};

// Walk `root` (UTF-8). Calls `fn` for each entry.
// Return false from `fn` to stop early.
// `skipDir` predicate: return true to skip descending into a directory by name.
bool Walk(std::string_view root,
          const std::function<bool(const Entry&)>& fn,
          const std::function<bool(std::string_view dirName)>& skipDir = {});

} // namespace dir_walker