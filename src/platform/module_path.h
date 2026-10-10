#pragma once
// module_path.h — Get executable directory
// Windows: GetModuleFileNameW
// Linux: readlink(/proc/self/exe)
// macOS: _NSGetExecutablePath

#include <string>

namespace module_path {

// Get the directory containing the running executable.
// Returns empty string on failure.
std::string GetExeDir();

} // namespace module_path