// tool_staged_write.h
//
// Shared atomic-write staging used by tool_write and tool_edit.
//
// Both tools use the same crash-safe pattern:
//   1. Open a unique sibling temp file with CREATE_NEW (never clobber
//      an existing user-owned file).
//   2. Write the payload in chunks.
//   3. CloseHandle.
//   4. MoveFileExW with MOVEFILE_WRITE_THROUGH (and, for tool_edit,
//      MOVEFILE_REPLACE_EXISTING) to atomically promote the temp to
//      the final path.
//
// Steps 1 and the temp-path construction in particular were copy-
// pasted between tool_write.cpp and tool_edit.cpp.  Lifting them here
// keeps the two tools using the exact same staging logic.
//
// The HANDLE returned in StagedTempFile is owned by the caller; the
// caller is responsible for CloseHandle and (on failure) DeleteFileW.
// Higher-level RAII would be nice; deferred to a later cleanup pass
// because the existing call sites already have explicit error paths
// that handle deletion correctly.

#pragma once

#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "path_safety.h"   // path_safety::Utf8ToWide

namespace tool_staged_write {

struct StagedTempFile {
    std::string  path;
    std::wstring wPath;
    HANDLE       handle = INVALID_HANDLE_VALUE;
    DWORD        error  = ERROR_SUCCESS;
};

// Returns the parent directory of `absPath` with a trailing separator
// preserved only for drive roots ("C:\\").  Empty when the input has
// no separator (shouldn't happen for canonical absolute paths).
inline std::string ParentDirForTemp(const std::string& absPath)
{
    size_t p = absPath.find_last_of("\\/");
    if (p == std::string::npos) return {};

    if (p == 2 && absPath.size() >= 3 &&
        absPath[1] == ':' &&
        (absPath[2] == '\\' || absPath[2] == '/')) {
        // Parent of "C:\\foo" is "C:\\" with trailing slash.
        return absPath.substr(0, 3);
    }

    return absPath.substr(0, p);
}

// Create a unique temp file in the same directory as the target using
// CREATE_NEW.  This avoids clobbering a real user-owned "<target>.tmp"
// file and keeps the later MoveFileEx on the same volume (so the move
// is a metadata-only rename instead of a copy+delete).
//
// On success, StagedTempFile.handle is a writable HANDLE the caller
// owns.  On failure the handle is INVALID_HANDLE_VALUE and `error`
// carries the Win32 error code from the last attempt.
inline StagedTempFile CreateStagedTempFile(const std::string& finalPath)
{
    StagedTempFile out;

    const std::string parent = ParentDirForTemp(finalPath);
    if (parent.empty()) {
        out.error = ERROR_INVALID_NAME;
        return out;
    }

    const bool parentHasSlash =
        !parent.empty() && (parent.back() == '\\' || parent.back() == '/');

    const DWORD pid = ::GetCurrentProcessId();
    const ULONGLONG tick = ::GetTickCount64();

    for (DWORD attempt = 0; attempt < 64; ++attempt) {
        std::string tmpPath = parent;
        if (!parentHasSlash) tmpPath += "\\";
        tmpPath += ".llamaboss-" +
                   std::to_string(pid) + "-" +
                   std::to_string(tick) + "-" +
                   std::to_string(attempt) + ".tmp";

        std::wstring wTmp = path_safety::Utf8ToWide(tmpPath);
        if (wTmp.empty()) {
            out.path  = tmpPath;
            out.error = ERROR_INVALID_NAME;
            return out;
        }

        HANDLE hFile = ::CreateFileW(
            wTmp.c_str(),
            GENERIC_WRITE,
            0,                    // no sharing while we write
            nullptr,
            CREATE_NEW,           // never overwrite any existing file
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

        if (hFile != INVALID_HANDLE_VALUE) {
            out.path   = std::move(tmpPath);
            out.wPath  = std::move(wTmp);
            out.handle = hFile;
            return out;
        }

        DWORD err = ::GetLastError();
        if (err != ERROR_FILE_EXISTS && err != ERROR_ALREADY_EXISTS) {
            out.path  = tmpPath;
            out.wPath = std::move(wTmp);
            out.error = err;
            return out;
        }
    }

    out.error = ERROR_ALREADY_EXISTS;
    return out;
}

} // namespace tool_staged_write
