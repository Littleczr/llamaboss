#pragma once

// Native file mutations only. Read-only tools and shell/Python permissions
// retain their existing policy. The lexical root check must pass FIRST.
#include "lb_windows.h"
#include <mutex>
#include <string>
#include <vector>
#include "path_safety.h"

#ifndef _WIN32
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace tool_mutation_guard {

// Shared by all translation units/windows in this process. Try-lock avoids
// queuing a mutation that could run later after its chat has been cancelled.
inline std::mutex& NativeMutationMutex()
{
    static std::mutex mutex;
    return mutex;
}

#ifdef _WIN32

class Handle {
public:
    explicit Handle(HANDLE h = INVALID_HANDLE_VALUE) : m_handle(h) {}
    ~Handle() { Reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE Get() const { return m_handle; }
    bool Valid() const { return m_handle != INVALID_HANDLE_VALUE; }
    HANDLE Release() { HANDLE h = m_handle; m_handle = INVALID_HANDLE_VALUE; return h; }
    void Reset(HANDLE h = INVALID_HANDLE_VALUE) {
        if (Valid()) ::CloseHandle(m_handle);
        m_handle = h;
    }
private:
    HANDLE m_handle;
};

// Parse only ordinary absolute drive/UNC paths, including their extended
// spellings. Device namespaces, alternate streams and ambiguous components
// must not reach the path-based Win32 mutation calls.
inline bool DirectoryPrefixes(const std::wstring& input,
                              std::vector<std::wstring>& parents,
                              std::wstring& target)
{
    parents.clear();
    target = input;
    for (auto& c : target) if (c == L'/') c = L'\\';
    const bool extended = target.compare(0, 4, L"\\\\?\\") == 0;
    if (target.compare(0, 8, L"\\\\?\\UNC\\") == 0)
        target = L"\\\\" + target.substr(8);
    else if (target.compare(0, 4, L"\\\\?\\") == 0)
        target.erase(0, 4);

    std::size_t rootEnd = 0;
    if (target.size() >= 3 && target[1] == L':' && target[2] == L'\\' &&
        ((target[0] >= L'A' && target[0] <= L'Z') ||
         (target[0] >= L'a' && target[0] <= L'z'))) {
        rootEnd = 3;
    } else if (target.compare(0, 2, L"\\\\") == 0) {
        const auto serverEnd = target.find(L'\\', 2);
        if (serverEnd == std::wstring::npos || serverEnd == 2) return false;
        const auto shareEnd = target.find(L'\\', serverEnd + 1);
        if (shareEnd == std::wstring::npos || shareEnd == serverEnd + 1) return false;
        const std::wstring server = target.substr(2, serverEnd - 2);
        const std::wstring share = target.substr(serverEnd + 1, shareEnd - serverEnd - 1);
        if (server == L"." || server == L"?" || server == L".." ||
            share == L"." || share == L".." ||
            server.back() == L'.' || server.back() == L' ' ||
            share.back() == L'.' || share.back() == L' ' ||
            server.find_first_of(L":*?\"<>|") != std::wstring::npos ||
            share.find_first_of(L":*?\"<>|") != std::wstring::npos) return false;
        for (auto c : server) if (c < 32) return false;
        for (auto c : share) if (c < 32) return false;
        rootEnd = shareEnd + 1;
    } else return false;

    while (target.size() > rootEnd && target.back() == L'\\') target.pop_back();
    if (target.size() <= rootEnd) return false; // Never mutate a volume/share root.
    parents.push_back(target.substr(0, rootEnd));
    std::size_t start = rootEnd;
    while (start < target.size()) {
        const auto sep = target.find(L'\\', start);
        const auto end = sep == std::wstring::npos ? target.size() : sep;
        const auto component = target.substr(start, end - start);
        if (component.empty() || component == L"." || component == L".." ||
            component.back() == L'.' || component.back() == L' ' ||
            component.find_first_of(L":*?\"<>|") != std::wstring::npos) return false;
        for (auto c : component) if (c < 32) return false;
        if (sep == std::wstring::npos) break;
        parents.push_back(target.substr(0, sep));
        start = sep + 1;
    }
    // Preserve an explicitly requested extended path for the handle calls;
    // dropping this prefix would regress paths longer than MAX_PATH.
    if (extended) {
        auto extend = [](const std::wstring& p) {
            return p.compare(0, 2, L"\\\\") == 0
                ? std::wstring(L"\\\\?\\UNC\\") + p.substr(2)
                : std::wstring(L"\\\\?\\") + p;
        };
        for (auto& p : parents) p = extend(p);
        target = extend(target);
    }
    return true;
}

class Guard {
public:
    Guard() : m_lock(NativeMutationMutex(), std::try_to_lock) {}
    ~Guard() {
        m_targetHandle.Reset();
        for (auto it = m_parents.rbegin(); it != m_parents.rend(); ++it)
            ::CloseHandle(*it);
    }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

    const std::string& Error() const { return m_error; }
    bool TargetExisted() const { return m_existed; }

    bool Begin(const std::string& path, bool allowMissingParents = false) {
        if (!m_lock.owns_lock()) {
            m_error = "Another native file change is in progress. Retry after it finishes; "
                      "the retry will read the latest file. No file was changed by this tool.";
            return false;
        }
        std::vector<std::wstring> parents;
        if (!DirectoryPrefixes(path_safety::Utf8ToWide(path), parents, m_target)) {
            m_error = "Unsafe or unsupported mutation path. Use an ordinary absolute "
                      "drive or UNC path without device names, streams or ambiguous components.";
            return false;
        }
        for (const auto& parent : parents) {
            if (!PinDirectory(parent)) {
                if (allowMissingParents && Missing(m_lastError)) {
                    m_error.clear();
                    break; // mkdir pins each new directory before descending into it.
                }
                return false;
            }
        }
        return Inspect(m_target, m_existed, m_initial, nullptr);
    }

    // Used by mkdir immediately after each create/already-exists result.
    // Its parent is already pinned; reject a concurrently inserted link.
    bool PinCreatedDirectory(const std::string& path) {
        return PinDirectory(path_safety::Utf8ToWide(path));
    }

    // Hold the verified target against writes/deletes while checking. edit
    // additionally compares the exact original bytes, not just timestamps.
    bool VerifyUnchanged(const std::string* originalBytes = nullptr) {
        bool exists = false;
        BY_HANDLE_FILE_INFORMATION current{};
        if (!Inspect(m_target, exists, current, originalBytes)) return false;
        if (exists != m_existed || (exists && !SameVersion(m_initial, current))) {
            m_targetHandle.Reset();
            m_error = "File conflict: the target changed, disappeared or was replaced "
                      "during this operation. Read the latest file and retry.";
            return false;
        }
        return true;
    }

    // MoveFileEx/DeleteFile cannot replace a target whose open handle denies
    // FILE_SHARE_DELETE, including our own. Release only at the final call.
    // Parent pins and the native-mutation mutex remain held through commit.
    // External programs do not honor our mutex: a small OS handoff window
    // remains between this close and the path-based commit (see patch notes).
    void ReleaseTargetForCommit() { m_targetHandle.Reset(); }

private:
    static bool Missing(DWORD err) {
        return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
    }
    static bool SameVersion(const BY_HANDLE_FILE_INFORMATION& a,
                            const BY_HANDLE_FILE_INFORMATION& b) {
        return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber &&
               a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow &&
               a.nFileSizeHigh == b.nFileSizeHigh && a.nFileSizeLow == b.nFileSizeLow &&
               a.ftLastWriteTime.dwHighDateTime == b.ftLastWriteTime.dwHighDateTime &&
               a.ftLastWriteTime.dwLowDateTime == b.ftLastWriteTime.dwLowDateTime &&
               a.ftCreationTime.dwHighDateTime == b.ftCreationTime.dwHighDateTime &&
               a.ftCreationTime.dwLowDateTime == b.ftCreationTime.dwLowDateTime &&
               a.dwFileAttributes == b.dwFileAttributes;
    }
    bool Fail(const char* what, DWORD err) {
        m_lastError = err;
        m_error = std::string(what) + " (Win32 error " + std::to_string(err) + ").";
        return false;
    }
    bool RejectLink() {
        m_lastError = ERROR_ACCESS_DENIED;
        m_error = "Native file changes through junctions, symbolic links or other "
                  "reparse points are blocked. Use the real destination path and "
                  "grant that folder access if needed.";
        return false;
    }
    bool PinDirectory(const std::wstring& path) {
        // No share-write/delete: ancestors cannot be moved or opened for
        // reparse-point modification until this operation releases its pins.
        Handle h(::CreateFileW(path.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!h.Valid()) return Fail("Cannot safely lock a parent directory", ::GetLastError());
        BY_HANDLE_FILE_INFORMATION info{};
        if (!::GetFileInformationByHandle(h.Get(), &info))
            return Fail("Cannot inspect a parent directory", ::GetLastError());
        if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return RejectLink();
        if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            return Fail("A parent path is not a directory", ERROR_DIRECTORY);
        m_parents.push_back(h.Get());
        h.Release();
        m_lastError = ERROR_SUCCESS;
        return true;
    }
    bool Inspect(const std::wstring& path, bool& exists,
                 BY_HANDLE_FILE_INFORMATION& info, const std::string* bytes) {
        m_targetHandle.Reset();
        exists = false;
        m_targetHandle.Reset(::CreateFileW(path.c_str(), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!m_targetHandle.Valid()) {
            const DWORD err = ::GetLastError();
            if (Missing(err)) return true;
            return Fail("Cannot safely inspect target (it may be in use)", err);
        }
        exists = true;
        if (!::GetFileInformationByHandle(m_targetHandle.Get(), &info))
            return Fail("Cannot inspect target identity", ::GetLastError());
        if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return RejectLink();
        if (bytes) {
            if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || info.nFileSizeHigh != 0 ||
                static_cast<unsigned long long>(info.nFileSizeLow) != bytes->size())
                return Fail("File conflict: original content changed; read again and retry", ERROR_INVALID_DATA);
            char block[65536];
            std::size_t offset = 0;
            while (offset < bytes->size()) {
                const auto remaining = bytes->size() - offset;
                const DWORD want = static_cast<DWORD>(remaining < sizeof(block) ? remaining : sizeof(block));
                DWORD got = 0;
                if (!::ReadFile(m_targetHandle.Get(), block, want, &got, nullptr))
                    return Fail("Cannot verify original file content", ::GetLastError());
                if (got != want || bytes->compare(offset, got, block, got) != 0)
                    return Fail("File conflict: original content changed; read again and retry", ERROR_INVALID_DATA);
                offset += got;
            }
        }
        return true;
    }

    std::unique_lock<std::mutex> m_lock;
    std::vector<HANDLE> m_parents;
    Handle m_targetHandle;
    std::wstring m_target;
    bool m_existed = false;
    BY_HANDLE_FILE_INFORMATION m_initial{};
    DWORD m_lastError = ERROR_SUCCESS;
    std::string m_error;
};

#else  // ── macOS / POSIX ──────────────────────────────────────────────

// Same contract as the Win32 guard above: pin every ancestor directory,
// refuse symbolic links, and detect a target that changed or was replaced
// between Begin() and commit. POSIX cannot deny other processes a rename
// the way a Win32 share mode can; walking with openat() relative to the
// pinned parent ensures every check applies to the directory actually held.
class Guard {
public:
    Guard() : m_lock(NativeMutationMutex(), std::try_to_lock) {}
    ~Guard() {
        CloseTarget();
        for (auto it = m_parents.rbegin(); it != m_parents.rend(); ++it) ::close(*it);
    }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

    const std::string& Error() const { return m_error; }
    bool TargetExisted() const { return m_existed; }

    bool Begin(const std::string& path, bool allowMissingParents = false) {
        if (!m_lock.owns_lock()) {
            m_error = "Another native file change is in progress. Retry after it finishes; "
                      "the retry will read the latest file. No file was changed by this tool.";
            return false;
        }
        std::vector<std::string> parts;
        if (!SplitAbsolute(path, parts)) {
            m_error = "Unsafe or unsupported mutation path. Use an ordinary absolute "
                      "path without ambiguous components.";
            return false;
        }
        m_target = path;
        m_name = parts.back();
        const int root = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (root < 0) return Fail("Cannot safely lock a parent directory", errno);
        m_parents.push_back(root);
        m_parentsComplete = true;
        for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
            if (!PinChild(parts[i], m_parents.size() == 1)) {
                if (allowMissingParents && Missing(m_lastError)) {
                    m_error.clear();
                    m_parentsComplete = false;
                    break; // mkdir pins each new directory before descending into it.
                }
                return false;
            }
        }
        return Inspect(m_existed, m_initial, nullptr);
    }

    // Used by mkdir immediately after each create/already-exists result.
    bool PinCreatedDirectory(const std::string& path) {
        const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) return errno == ELOOP ? RejectLink()
                                          : Fail("Cannot safely lock a parent directory", errno);
        m_parents.push_back(fd);
        return true;
    }

    bool VerifyUnchanged(const std::string* originalBytes = nullptr) {
        bool exists = false;
        struct stat current {};
        if (!Inspect(exists, current, originalBytes)) return false;
        if (exists != m_existed || (exists && !SameVersion(m_initial, current))) {
            CloseTarget();
            m_error = "File conflict: the target changed, disappeared or was replaced "
                      "during this operation. Read the latest file and retry.";
            return false;
        }
        return true;
    }

    void ReleaseTargetForCommit() { CloseTarget(); }

private:
    static bool Missing(int err) { return err == ENOENT; }

    static bool SplitAbsolute(const std::string& path, std::vector<std::string>& parts) {
        if (path.empty() || path[0] != '/') return false;
        std::size_t start = 1;
        while (start <= path.size()) {
            const auto sep = path.find('/', start);
            const auto end = sep == std::string::npos ? path.size() : sep;
            std::string part = path.substr(start, end - start);
            if (!part.empty()) {
                if (part == "." || part == "..") return false;
                for (unsigned char c : part) if (c < 32) return false;
                parts.push_back(std::move(part));
            }
            if (sep == std::string::npos) break;
            start = sep + 1;
        }
        return !parts.empty(); // Never mutate the filesystem root.
    }

    static bool SameVersion(const struct stat& a, const struct stat& b) {
        return a.st_dev == b.st_dev && a.st_ino == b.st_ino &&
               a.st_size == b.st_size && a.st_mode == b.st_mode &&
               a.st_mtimespec.tv_sec == b.st_mtimespec.tv_sec &&
               a.st_mtimespec.tv_nsec == b.st_mtimespec.tv_nsec &&
               a.st_birthtimespec.tv_sec == b.st_birthtimespec.tv_sec &&
               a.st_birthtimespec.tv_nsec == b.st_birthtimespec.tv_nsec;
    }
    bool Fail(const char* what, int err) {
        m_lastError = err;
        m_error = std::string(what) + " (" + std::strerror(err) + ").";
        return false;
    }
    bool RejectLink() {
        m_lastError = EACCES;
        m_error = "Native file changes through symbolic links are blocked. Use the "
                  "real destination path and grant that folder access if needed.";
        return false;
    }
    void CloseTarget() {
        if (m_targetFd >= 0) ::close(m_targetFd);
        m_targetFd = -1;
    }
    bool PinChild(const std::string& name, bool underRoot) {
        const int parent = m_parents.back();
        int fd = ::openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0 && (errno == ELOOP || errno == ENOTDIR)) {
            struct stat lst {};
            if (::fstatat(parent, name.c_str(), &lst, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(lst.st_mode)) {
                // macOS's own root-level links (/tmp, /var, /etc -> /private/...)
                // are owned by root and are part of every path under them.
                if (!underRoot || lst.st_uid != 0) return RejectLink();
                fd = ::openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            } else {
                return Fail("A parent path is not a directory", ENOTDIR);
            }
        }
        if (fd < 0) return Fail("Cannot safely lock a parent directory", errno);
        m_parents.push_back(fd);
        m_lastError = 0;
        return true;
    }
    bool Inspect(bool& exists, struct stat& info, const std::string* bytes) {
        CloseTarget();
        exists = false;
        const int flags = O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC;
        m_targetFd = m_parentsComplete
            ? ::openat(m_parents.back(), m_name.c_str(), flags)
            : ::open(m_target.c_str(), flags);
        if (m_targetFd < 0) {
            const int err = errno;
            if (Missing(err)) return true;
            if (err == ELOOP) return RejectLink();
            return Fail("Cannot safely inspect target (it may be in use)", err);
        }
        exists = true;
        if (::fstat(m_targetFd, &info) != 0)
            return Fail("Cannot inspect target identity", errno);
        if (bytes) {
            if (S_ISDIR(info.st_mode) || static_cast<unsigned long long>(info.st_size) != bytes->size())
                return Fail("File conflict: original content changed; read again and retry", EIO);
            char block[65536];
            std::size_t offset = 0;
            while (offset < bytes->size()) {
                const auto remaining = bytes->size() - offset;
                const auto want = remaining < sizeof(block) ? remaining : sizeof(block);
                const auto got = ::read(m_targetFd, block, want);
                if (got < 0) return Fail("Cannot verify original file content", errno);
                if (static_cast<std::size_t>(got) != want ||
                    bytes->compare(offset, want, block, want) != 0)
                    return Fail("File conflict: original content changed; read again and retry", EIO);
                offset += want;
            }
        }
        return true;
    }

    std::unique_lock<std::mutex> m_lock;
    std::vector<int> m_parents;
    bool m_parentsComplete = false;
    int m_targetFd = -1;
    std::string m_target;
    std::string m_name;
    bool m_existed = false;
    struct stat m_initial {};
    int m_lastError = 0;
    std::string m_error;
};

#endif

} // namespace tool_mutation_guard
