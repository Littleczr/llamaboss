// tool_delete.cpp

#include "tool_delete.h"
#include "tool_path.h"
#include "tool_path_safety.h"   // IsUnderCwd, NormalizeForCompare, Basename
#include "tool_open.h"          // ClassifyForOpen, FileRisk
#include "path_safety.h"

#include <chrono>
#include <sstream>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {

std::string HumanBytes(size_t b)
{
    std::ostringstream ss;
    ss << std::fixed;
    if (b < 1024) {
        ss << b << " B";
    } else if (b < 1024 * 1024) {
        ss.precision(1);
        ss << (b / 1024.0) << " KB";
    } else {
        ss.precision(2);
        ss << (b / (1024.0 * 1024.0)) << " MB";
    }
    return ss.str();
}

std::string ElapsedChip(std::chrono::steady_clock::time_point t0)
{
    double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();
    std::ostringstream ts;
    ts << std::fixed;
    ts.precision(elapsed < 10.0 ? 2 : 1);
    ts << elapsed << "s";
    return ts.str();
}

std::string Trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// ─── Directory inspection ────────────────────────────────────────
//
// Walk the directory enumerating non-"." / non-".." entries and
// stop at the first one we find OR after `cap` entries.  Returns
// the count discovered (0 = empty, >=1 = non-empty).  The cap is
// just to keep the diagnostic display readable when a model tries
// to delete a directory with thousands of files; we stop counting
// after a small ceiling and add a "+" to the count chip.
size_t CountDirEntries(const std::string& absDir, size_t cap)
{
    std::wstring wDir = path_safety::Utf8ToWide(absDir);
    if (wDir.empty()) return 0;

    // FindFirstFileW takes a path with a wildcard at the end.
    // Append \\* without re-canonicalizing -- absDir is already
    // a resolved Windows path with backslashes.
    if (!wDir.empty() && wDir.back() != L'\\' && wDir.back() != L'/')
        wDir.push_back(L'\\');
    wDir += L'*';

    WIN32_FIND_DATAW fd;
    HANDLE h = ::FindFirstFileW(wDir.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;

    size_t count = 0;
    do {
        // Skip "." and ".." special entries.
        if (fd.cFileName[0] == L'.' &&
            (fd.cFileName[1] == L'\0' ||
             (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0'))) {
            continue;
        }
        ++count;
        if (count >= cap) break;
    } while (::FindNextFileW(h, &fd));

    ::FindClose(h);
    return count;
}

// File size for the deletion confirmation body.  Returns 0 on any
// error; only used for cosmetic "Deleted N.M KB" reporting, so an
// inaccurate read here doesn't affect correctness of the delete.
size_t FileSize(const std::string& absPath)
{
    std::wstring wPath = path_safety::Utf8ToWide(absPath);
    if (wPath.empty()) return 0;

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!::GetFileAttributesExW(wPath.c_str(),
                                GetFileExInfoStandard,
                                &fad)) {
        return 0;
    }
    // Cap at SIZE_MAX -- anything bigger we treat as 4 GiB+ for
    // display purposes since we don't emit those numbers anyway.
    ULARGE_INTEGER li;
    li.LowPart  = fad.nFileSizeLow;
    li.HighPart = fad.nFileSizeHigh;
    return (li.QuadPart > (ULONGLONG)((size_t)-1))
               ? (size_t)-1
               : (size_t)li.QuadPart;
}

} // anonymous namespace

DeleteResult DeleteEntry(const std::string& pathIn,
                         const ToolContext& ctx)
{
    DeleteResult r;
    auto t0 = std::chrono::steady_clock::now();

    const std::string requested = Trim(pathIn);
    if (requested.empty()) {
        r.chips.push_back("failed");
        r.errorBody = "delete requires a path in <args>.";
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    if (ctx.cwd.empty()) {
        r.chips.push_back("blocked");
        r.errorBody = "No working directory set; refuse to delete.";
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    std::string resolved = ResolveToolPath(requested, ctx.cwd);
    if (resolved.empty()) {
        r.chips.push_back("failed");
        r.errorBody = "Could not resolve path: " + requested;
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    // ── Containment ──────────────────────────────────────────────
    if (!tool_path_safety::IsUnderCwd(resolved, ctx.cwd)) {
        r.chips.push_back("blocked");
        r.errorBody = "Refuses to delete outside the working "
                      "directory.\n  resolved: " + resolved +
                      "\n  cwd:      " + ctx.cwd;
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    // ── Refuse to delete the cwd itself ──────────────────────────
    // Containment passes for the cwd (size_match branch), but
    // deleting your own working directory mid-conversation is the
    // kind of thing nobody wants to happen by accident.  Force the
    // user to leave first.
    {
        std::string a = tool_path_safety::NormalizeForCompare(resolved);
        std::string b = tool_path_safety::NormalizeForCompare(ctx.cwd);
        while (b.size() > 3 && b.back() == '\\') b.pop_back();
        if (a == b) {
            r.chips.push_back("blocked");
            r.errorBody = "Refuses to delete the working directory "
                          "itself: " + resolved;
            r.chips.push_back(ElapsedChip(t0));
            return r;
        }
    }

    // ── Filename sanitization ────────────────────────────────────
    std::string basename  = tool_path_safety::Basename(resolved);
    std::string sanitized = path_safety::SanitizeFilename(basename, "");
    if (sanitized.empty() || sanitized != basename) {
        r.chips.push_back("blocked");
        r.errorBody = "Filename '" + basename +
                      "' has characters or a shape that aren't "
                      "safe on Windows; refuse to operate on it.";
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    // ── Existence + dispatch on file-vs-dir ──────────────────────
    const bool isFile = IsFile(resolved);
    const bool isDir  = IsDirectory(resolved);

    if (!isFile && !isDir) {
        r.chips.push_back("not found");
        r.errorBody = "Path does not exist: " + resolved;
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    // ── Risky-extension block (file branch only) ─────────────────
    // Directories don't have a meaningful extension classification.
    // For files, mirror write/edit's kill-list.
    if (isFile && ClassifyForOpen(resolved) == FileRisk::Risky) {
        r.chips.push_back("blocked");
        r.errorBody = "Refuses to delete files with executable or "
                      "scriptable extensions. If the user wants this, "
                      "they can delete the file manually.";
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    std::wstring wPath = path_safety::Utf8ToWide(resolved);
    if (wPath.empty()) {
        r.chips.push_back("failed");
        r.errorBody = "Path conversion failed: " + resolved;
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    // ── File deletion ────────────────────────────────────────────
    if (isFile) {
        const size_t bytes = FileSize(resolved);

        if (!::DeleteFileW(wPath.c_str())) {
            DWORD err = ::GetLastError();
            r.chips.push_back("failed");
            r.errorBody = "DeleteFile failed (Win32 error " +
                          std::to_string(err) + ").";
            r.chips.push_back(ElapsedChip(t0));
            return r;
        }

        r.chips.push_back("deleted");
        if (bytes != (size_t)-1 && bytes > 0) {
            r.chips.push_back(HumanBytes(bytes));
        }
        r.body = "Deleted " +
                 (bytes != (size_t)-1 && bytes > 0
                      ? HumanBytes(bytes) + " file: "
                      : std::string("file: ")) +
                 resolved + "\n";
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    // ── Directory deletion (must be empty) ───────────────────────
    constexpr size_t kCountCap = 16;
    size_t entries = CountDirEntries(resolved, kCountCap);
    if (entries > 0) {
        r.chips.push_back("not empty");
        r.chips.push_back(std::to_string(entries) +
                          (entries >= kCountCap ? "+" : "") +
                          (entries == 1 ? " entry" : " entries"));
        r.errorBody = "Directory is not empty: " + resolved +
                      ".\nDelete its contents first (one ls + one "
                      "delete per entry), then retry deleting the "
                      "directory.";
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    if (!::RemoveDirectoryW(wPath.c_str())) {
        DWORD err = ::GetLastError();
        // Race: another process / user added a file to the dir
        // between CountDirEntries and now, making it non-empty.
        // Treat that as "not empty" with a recoverable diagnostic
        // rather than a generic failure.
        if (err == ERROR_DIR_NOT_EMPTY) {
            r.chips.push_back("not empty");
            r.errorBody = "Directory became non-empty during the "
                          "operation: " + resolved;
        } else {
            r.chips.push_back("failed");
            r.errorBody = "RemoveDirectory failed (Win32 error " +
                          std::to_string(err) + ").";
        }
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    r.chips.push_back("deleted");
    r.chips.push_back("directory");
    r.body = "Deleted directory: " + resolved + "\n";
    r.chips.push_back(ElapsedChip(t0));
    return r;
}
