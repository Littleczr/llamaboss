// desktop_open_macos.cpp — macOS `open` command
#include "desktop_open.h"

#include <cstdlib>
#include <string>
#include <memory>

namespace desktop_open {

bool Open(std::string_view pathOrUrl) {
    std::string cmd = "open ";
    // Escape for shell
    cmd.reserve(pathOrUrl.size() + 8);
    cmd.push_back('"');
    for (char c : pathOrUrl) {
        if (c == '"' || c == '\\' || c == '$' || c == '`') cmd.push_back('\\');
        cmd.push_back(c);
    }
    cmd.push_back('"');
    return std::system(cmd.c_str()) == 0;
}

} // namespace desktop_open