// tool_staged_write.h
//
// Shared atomic-write staging used by tool_write and tool_edit.
//
// Both tools use the same crash-safe pattern:
//   1. Open a unique sibling temp file with CREATE_NEW (never clobber
//      an existing user-owned file).
//   2. Write the payload in chunks.
//   3. FlushFileBuffers, then CloseHandle.
//   4. PromoteSiblingTempFile to atomically rename the temp within its
//      existing directory, without reopening the pinned parent for writes.
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
#include <cstring>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winternl.h>

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

// Native tools hold parent directories open without share-write/delete to
// prevent directory replacement and reparse-point changes. A full-path
// MoveFileEx rename asks Windows to reopen the destination directory with
// FILE_WRITE_DATA, conflicting with those very pins (ERROR_SHARING_VIOLATION).
//
// Staging is always in the target's own directory. The native rename contract
// supports a simple filename with a null RootDirectory: rename in the source
// directory, with no destination-directory open. Use NtSetInformationFile
// directly because SetFileInformationByHandle resolves relative Win32 paths
// against the process CWD. Never change CWD or release the directory pins.
// https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_rename_information
//
// Caller must close the staging writer, validate the target, and release its
// target handle before calling. Failure leaves the staging file available.
// Existing call sites have already flushed the staged bytes. This remains a
// same-volume atomic rename; it is not a cross-volume copy/delete fallback.
inline BOOL PromoteSiblingTempFile(const std::wstring& stagedPath,
                                  const std::wstring& finalPath,
                                  bool replaceExisting)
{
    std::wstring source = stagedPath;
    std::wstring target = finalPath;
    for (auto& c : source) if (c == L'/') c = L'\\';
    for (auto& c : target) if (c == L'/') c = L'\\';
    const auto sourceSep = source.find_last_of(L'\\');
    const auto targetSep = target.find_last_of(L'\\');
    if (sourceSep == std::wstring::npos || targetSep == std::wstring::npos ||
        sourceSep == 0 || sourceSep != targetSep ||
        source.compare(0, sourceSep + 1, target, 0, targetSep + 1) != 0 ||
        sourceSep + 1 == source.size()) {
        ::SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const std::wstring leaf = target.substr(targetSep + 1);
    if (leaf.empty() || leaf == L"." || leaf == L".." ||
        leaf.back() == L'.' || leaf.back() == L' ' ||
        leaf.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) {
        ::SetLastError(ERROR_INVALID_NAME);
        return FALSE;
    }
    for (wchar_t c : leaf) {
        if (c < 32) {
            ::SetLastError(ERROR_INVALID_NAME);
            return FALSE;
        }
    }
    if (leaf.size() > (MAXDWORD - sizeof(FILE_RENAME_INFO)) / sizeof(wchar_t) - 1) {
        ::SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return FALSE;
    }

    using NtSetInformationFileFn = NTSTATUS (NTAPI *)(
        HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
    using RtlNtStatusToDosErrorFn = ULONG (WINAPI *)(NTSTATUS);
    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    const auto renameFile = ntdll ? reinterpret_cast<NtSetInformationFileFn>(
        ::GetProcAddress(ntdll, "NtSetInformationFile")) : nullptr;
    const auto statusToError = ntdll ? reinterpret_cast<RtlNtStatusToDosErrorFn>(
        ::GetProcAddress(ntdll, "RtlNtStatusToDosError")) : nullptr;
    if (!renameFile || !statusToError) {
        ::SetLastError(ERROR_PROC_NOT_FOUND);
        return FALSE;
    }

    // FILE_RENAME_INFO has the same layout as native FILE_RENAME_INFORMATION
    // for the classic FileRenameInformation class (10). Allocate before the
    // handle is opened so an allocation exception cannot leak the handle.
    const DWORD nameBytes = static_cast<DWORD>(leaf.size() * sizeof(wchar_t));
    const DWORD bufferBytes = static_cast<DWORD>(sizeof(FILE_RENAME_INFO) +
                                               nameBytes + sizeof(wchar_t));
    std::vector<unsigned char> buffer(bufferBytes, 0);
    auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    info->ReplaceIfExists = replaceExisting ? TRUE : FALSE;
    info->RootDirectory = nullptr;
    info->FileNameLength = nameBytes;
    std::memcpy(info->FileName, leaf.data(), nameBytes);

    HANDLE file = ::CreateFileW(source.c_str(), DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    BY_HANDLE_FILE_INFORMATION identity{};
    DWORD error = ERROR_SUCCESS;
    BOOL ok = ::GetFileInformationByHandle(file, &identity);
    if (!ok) {
        error = ::GetLastError();
    } else if (identity.dwFileAttributes &
               (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
        ok = FALSE;
        error = ERROR_ACCESS_DENIED;
    } else {
        IO_STATUS_BLOCK io{};
        const NTSTATUS status = renameFile(file, &io, info, bufferBytes,
            static_cast<FILE_INFORMATION_CLASS>(10)); // FileRenameInformation
        ok = status >= 0;
        if (!ok) error = statusToError(status);
    }
    ::CloseHandle(file);
    // CloseHandle must not hide the actual rename/validation failure.
    ::SetLastError(error);
    return ok;
}

} // namespace tool_staged_write
