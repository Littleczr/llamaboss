#pragma once
// desktop_open.h — Open file/URL with default application
// Windows: ShellExecuteExW
// macOS: open(1) command
// Linux: xdg-open(1) command

#include <string>
#include <string_view>

namespace desktop_open {

// Open `pathOrUrl` with the default system handler.
// Returns true if the launch succeeded (not whether the app opened successfully).
bool Open(std::string_view pathOrUrl);

} // namespace desktop_open