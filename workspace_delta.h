// workspace_delta.h
//
// Shared header-only helper: snapshot a workspace directory before a
// run-style tool executes (PowerShell command, script run), diff it
// afterwards, and format a model-facing "[workspace changes]" manifest.
//
// Why this exists (2026-06-11): a PowerShell Compress-Archive command
// silently matched zero files (the Get-ChildItem -Include footgun),
// exited 0 with no output, and a small local model then told the user
// "the file is attached above" — patterned on the write tool's artifact
// cards — when no file existed at all. The harness gave the model no
// ground truth about file-system effects, so it confabulated one.
// The manifest closes that gap in BOTH directions:
//   * files that WERE created are named (and can be attached as
//     PresentedFile cards by the caller), and
//   * when nothing was created or modified, the result says so
//     explicitly, which leaves the model no room to claim otherwise.
//
// Scope is deliberately the conversation workspace folder only:
// cheap, bounded, and where run-style tools are expected to place
// their outputs. Callers must not pass broad roots like %USERPROFILE%.
//
// The python_run_script auto-artifact scanner in python_runner.cpp
// predates this header and keeps its own multi-root snapshot; this
// header is the equivalent for tools that had nothing (PowerShell).
//
// Windows implementation mirrors the proven FindFirstFileW pattern
// from python_runner.cpp. A POSIX branch exists solely so the logic
// can be unit-tested off-Windows; it is not used by the app.
//
#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#else
    #include <dirent.h>
    #include <sys/stat.h>
#endif

namespace workspace_delta {

// ─── Types ────────────────────────────────────────────────────────

struct FileSig {
    unsigned long long sizeBytes  = 0;
    unsigned long long mtimeTicks = 0;   // opaque; only compared for equality

    bool operator==(const FileSig& o) const {
        return sizeBytes == o.sizeBytes && mtimeTicks == o.mtimeTicks;
    }
    bool operator!=(const FileSig& o) const { return !(*this == o); }
};

// Absolute UTF-8 path -> signature.
struct Snapshot {
    std::map<std::string, FileSig> files;
    bool capped = false;   // true if the entry cap was hit while scanning
};

struct Delta {
    std::vector<std::string> created;    // absolute paths, sorted (map order)
    std::vector<std::string> modified;   // absolute paths, sorted
    bool capped = false;                 // either snapshot hit the entry cap
};

// Hard ceiling on scanned entries per snapshot.  Workspace folders are
// per-conversation and normally small; the cap only matters when a
// script dumps thousands of files, and then the manifest says so.
constexpr size_t kMaxScanEntries = 2000;

// ─── Internal helpers ─────────────────────────────────────────────

namespace detail {

inline std::string LowerAscii(std::string s)
{
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

// Directories that are never interesting as tool outputs and can be
// arbitrarily large.  Mirrors ShouldSkipPythonArtifactScanDir in
// python_runner.cpp, plus IDE folders.
inline bool ShouldSkipScanDir(const std::string& name)
{
    std::string key = LowerAscii(name);
    return key == ".git" ||
           key == ".hg" ||
           key == ".svn" ||
           key == ".vs" ||
           key == ".idea" ||
           key == ".cache" ||
           key == ".mypy_cache" ||
           key == ".pytest_cache" ||
           key == "__pycache__" ||
           key == "node_modules" ||
           key == ".venv" ||
           key == "venv" ||
           key == "env" ||
           key == "site-packages" ||
           key == "system";
}

#ifdef _WIN32

inline std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                        static_cast<int>(s.size()), &out[0], n);
    return out;
}

inline std::string WideToUtf8(const std::wstring& s)
{
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(),
                                static_cast<int>(s.size()),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(),
                        static_cast<int>(s.size()),
                        &out[0], n, nullptr, nullptr);
    return out;
}

inline void ScanRecursive(const std::string& dir,
                          Snapshot&          out,
                          size_t&            seen)
{
    if (seen >= kMaxScanEntries) { out.capped = true; return; }

    std::wstring pattern = Utf8ToWide(dir) + L"\\*";
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        if (seen >= kMaxScanEntries) { out.capped = true; break; }

        std::wstring nameW = fd.cFileName;
        if (nameW == L"." || nameW == L"..") continue;

        std::string name = WideToUtf8(nameW);
        std::string path = dir + "\\" + name;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (ShouldSkipScanDir(name)) continue;
            ScanRecursive(path, out, seen);
        } else {
            ++seen;
            FileSig sig;
            sig.sizeBytes =
                (static_cast<unsigned long long>(fd.nFileSizeHigh) << 32) |
                fd.nFileSizeLow;
            ULARGE_INTEGER t;
            t.LowPart  = fd.ftLastWriteTime.dwLowDateTime;
            t.HighPart = fd.ftLastWriteTime.dwHighDateTime;
            sig.mtimeTicks = t.QuadPart;
            out.files.emplace(std::move(path), sig);
        }
    } while (FindNextFileW(h, &fd));

    FindClose(h);
}

#else  // POSIX test branch (not used by the Windows app)

inline void ScanRecursive(const std::string& dir,
                          Snapshot&          out,
                          size_t&            seen)
{
    if (seen >= kMaxScanEntries) { out.capped = true; return; }

    DIR* d = opendir(dir.c_str());
    if (!d) return;

    while (dirent* e = readdir(d)) {
        if (seen >= kMaxScanEntries) { out.capped = true; break; }

        std::string name = e->d_name;
        if (name == "." || name == "..") continue;

        std::string path = dir + "/" + name;
        struct stat st;
        if (lstat(path.c_str(), &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            if (ShouldSkipScanDir(name)) continue;
            ScanRecursive(path, out, seen);
        } else if (S_ISREG(st.st_mode)) {
            ++seen;
            FileSig sig;
            sig.sizeBytes  = static_cast<unsigned long long>(st.st_size);
            sig.mtimeTicks =
                static_cast<unsigned long long>(st.st_mtime) * 1000000000ull +
                static_cast<unsigned long long>(st.st_mtim.tv_nsec);
            out.files.emplace(std::move(path), sig);
        }
    }

    closedir(d);
}

#endif  // _WIN32

inline std::string HumanSize(unsigned long long bytes)
{
    char buf[64];
    if (bytes >= 1024ull * 1024ull * 1024ull) {
        snprintf(buf, sizeof(buf), "%.1f GB",
                 static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ull * 1024ull) {
        snprintf(buf, sizeof(buf), "%.1f MB",
                 static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024ull) {
        snprintf(buf, sizeof(buf), "%.1f KB",
                 static_cast<double>(bytes) / 1024.0);
    } else {
        snprintf(buf, sizeof(buf), "%llu B", bytes);
    }
    return buf;
}

// Strip `root` (plus one separator) from `path` for display; falls
// back to the full path if it is not under root.
inline std::string DisplayRelative(const std::string& path,
                                   const std::string& root)
{
    if (!root.empty() &&
        path.size() > root.size() + 1 &&
        path.compare(0, root.size(), root) == 0 &&
        (path[root.size()] == '\\' || path[root.size()] == '/')) {
        return path.substr(root.size() + 1);
    }
    return path;
}

} // namespace detail

// ─── Public API ───────────────────────────────────────────────────

// Recursively snapshot regular files under `rootUtf8`.  Bounded by
// kMaxScanEntries; junk/VCS/IDE directories are skipped.  Returns an
// empty snapshot for an empty or nonexistent root.
inline Snapshot TakeSnapshot(const std::string& rootUtf8)
{
    Snapshot s;
    if (rootUtf8.empty()) return s;
    size_t seen = 0;
    detail::ScanRecursive(rootUtf8, s, seen);
    return s;
}

inline Delta Diff(const Snapshot& before, const Snapshot& after)
{
    Delta d;
    d.capped = before.capped || after.capped;

    for (const auto& kv : after.files) {
        auto it = before.files.find(kv.first);
        if (it == before.files.end()) {
            d.created.push_back(kv.first);
        } else if (it->second != kv.second) {
            d.modified.push_back(kv.first);
        }
    }
    return d;
}

// Builds the model-and-user-facing manifest text.  `afterSizes` is the
// post-run snapshot (for created-file sizes); `rootUtf8` is used to
// shorten displayed paths.  ALWAYS returns a non-empty section so the
// negative case ("no files were created or modified") is stated
// explicitly — that explicit negative is the anti-hallucination
// signal this header exists for.
inline std::string FormatManifest(const Delta&    d,
                                  const Snapshot& afterSizes,
                                  const std::string& rootUtf8)
{
    constexpr size_t kMaxCreatedListed  = 12;
    constexpr size_t kMaxModifiedListed = 8;

    std::string out = "[workspace changes]\n";

    if (d.created.empty() && d.modified.empty()) {
        out += "no files were created or modified in the workspace folder "
               "by this command.\n";
    } else {
        size_t listed = 0;
        for (const std::string& p : d.created) {
            if (listed >= kMaxCreatedListed) break;
            unsigned long long size = 0;
            auto it = afterSizes.files.find(p);
            if (it != afterSizes.files.end()) size = it->second.sizeBytes;
            out += "created: " + detail::DisplayRelative(p, rootUtf8) +
                   " (" + detail::HumanSize(size) + ")\n";
            ++listed;
        }
        if (d.created.size() > kMaxCreatedListed) {
            out += "...and " +
                   std::to_string(d.created.size() - kMaxCreatedListed) +
                   " more created file(s)\n";
        }

        listed = 0;
        for (const std::string& p : d.modified) {
            if (listed >= kMaxModifiedListed) break;
            out += "modified: " + detail::DisplayRelative(p, rootUtf8) + "\n";
            ++listed;
        }
        if (d.modified.size() > kMaxModifiedListed) {
            out += "...and " +
                   std::to_string(d.modified.size() - kMaxModifiedListed) +
                   " more modified file(s)\n";
        }
    }

    if (d.capped) {
        out += "(scan capped at " + std::to_string(kMaxScanEntries) +
               " entries; changes beyond the cap may be missing)\n";
    }

    return out;
}

} // namespace workspace_delta
