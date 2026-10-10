#pragma once
// atomic_file.h — Atomic file write (write to temp, then rename)
// Windows: MoveFileExW with MOVEFILE_REPLACE_EXISTING / MOVEFILE_WRITE_THROUGH
// POSIX: link() + rename() dance for atomic replace

#include <string>
#include <string_view>

namespace atomic_file {

// Write `content` to `finalPath` atomically.
// If `overwrite` is false and file exists, returns false.
// Returns true on success.
bool Write(std::string_view finalPath, std::string_view content, bool overwrite = true);

} // namespace atomic_file