// module_path_macos.cpp — macOS _NSGetExecutablePath
#include "module_path.h"

#include <mach-o/dyld.h>
#include <filesystem>
#include <string>
#include <vector>

namespace module_path {

std::string GetExeDir() {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size);
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    std::filesystem::path p(buf.data());
    return p.parent_path().string();
}

} // namespace module_path