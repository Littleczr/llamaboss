// module_path_linux.cpp — Linux readlink(/proc/self/exe)
#include "module_path.h"

#include <unistd.h>
#include <filesystem>
#include <string>
#include <vector>

namespace module_path {

std::string GetExeDir() {
    std::vector<char> buf(4096);
    ssize_t n = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    std::filesystem::path p = std::filesystem::u8path(buf.data());
    return p.parent_path().u8string();
}

} // namespace module_path