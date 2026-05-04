// tool_ls.cpp

#include "tool_ls.h"
#include "tool_path.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <sstream>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {

// ─── Limits ──────────────────────────────────────────────────────
// kLsEntryCap is a fixed UX ceiling on how many entries we emit in a
// single listing.  Directories with more than this are rare in dev
// workflows but common in system paths (C:\Windows\System32 has
// ~3000 entries); capping here keeps output readable and bounded.
constexpr size_t kLsEntryCap = 500;

// Same byte-cap idea as /read — prevents pathological cases (500
// entries × 200-char names × three columns) from overflowing the
// model's context window on top of the entry cap.
constexpr int    kReservedTokens = 3000;
constexpr size_t kLsByteCapFloor   =   4 * 1024;
constexpr size_t kLsByteCapCeiling = 512 * 1024;

size_t ComputeLsByteCap(int ctxTokens)
{
    int usable = (ctxTokens > kReservedTokens)
                 ? (ctxTokens - kReservedTokens) : 0;
    size_t cap = (size_t)usable * 3;  // ~3 chars/token
    if (cap < kLsByteCapFloor)   cap = kLsByteCapFloor;
    if (cap > kLsByteCapCeiling) cap = kLsByteCapCeiling;
    return cap;
}

// Layout constants for the aligned listing.
constexpr size_t kMinNameColWidth = 20;   // soft floor — ensures tidy look on short-named dirs
constexpr size_t kSizeColWidth    = 10;   // enough for "1023.9 MB"

// ─── Helpers ─────────────────────────────────────────────────────
// Local UTF-8 ↔ UTF-16 helpers, duplicated from tool_path.cpp for
// file-private use.  Will be extracted to a shared header once /grep
// needs them too (YAGNI until then).
std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return L"";
    int len = ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                    (int)s.size(), nullptr, 0);
    if (len <= 0) return L"";
    std::wstring w((size_t)len, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(),
                          &w[0], len);
    return w;
}

std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return "";
    int len = ::WideCharToMultiByte(CP_UTF8, 0, w.data(),
                                    (int)w.size(),
                                    nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string s((size_t)len, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                          &s[0], len, nullptr, nullptr);
    return s;
}

// Human-readable byte count — duplicated from tool_read.cpp for the
// same reason as the UTF-8 helpers.
std::string HumanBytes(uint64_t b)
{
    std::ostringstream ss;
    ss << std::fixed;
    if (b < 1024) {
        ss << b << " B";
    } else if (b < 1024ULL * 1024ULL) {
        ss.precision(1);
        ss << (b / 1024.0) << " KB";
    } else if (b < 1024ULL * 1024ULL * 1024ULL) {
        ss.precision(2);
        ss << (b / (1024.0 * 1024.0)) << " MB";
    } else {
        ss.precision(2);
        ss << (b / (1024.0 * 1024.0 * 1024.0)) << " GB";
    }
    return ss.str();
}

// FILETIME → "YYYY-MM-DD HH:MM" in local time.  Zero FILETIME
// (e.g. root special dirs) collapses to "—" so the column stays
// aligned.
std::string FormatFileTime(const FILETIME& ft)
{
    if (ft.dwLowDateTime == 0 && ft.dwHighDateTime == 0) return "\xE2\x80\x94"; // em-dash

    FILETIME local{};
    if (!::FileTimeToLocalFileTime(&ft, &local)) return "\xE2\x80\x94";

    SYSTEMTIME st{};
    if (!::FileTimeToSystemTime(&local, &st)) return "\xE2\x80\x94";

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u",
                  (unsigned)st.wYear, (unsigned)st.wMonth,
                  (unsigned)st.wDay,  (unsigned)st.wHour,
                  (unsigned)st.wMinute);
    return std::string(buf);
}

// Case-insensitive < on UTF-8 strings.  Falls back to byte-wise
// compare after lowercasing ASCII; non-ASCII entries sort in byte
// order, which matches Explorer's behaviour well enough for dev
// use.  Keeps dirs and files separately sorted.
bool NameLess(const std::string& a, const std::string& b)
{
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca = ca + ('a' - 'A');
        if (cb >= 'A' && cb <= 'Z') cb = cb + ('a' - 'A');
        if (ca != cb) return ca < cb;
    }
    return a.size() < b.size();
}

// Elapsed-time chip — "0.02s" / "12.3s", same format as /cmd and /read.
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

struct LsEntry {
    std::string  name;
    bool         isDirectory;
    uint64_t     sizeBytes;
    std::string  mtimeFormatted;
};

} // anonymous namespace

LsResult ListDirectory(const std::string& inputPath, const ToolContext& ctx)
{
    LsResult r;
    auto t0 = std::chrono::steady_clock::now();

    // ── Path resolution ──────────────────────────────────────────
    // Empty arg means "list ctx.cwd itself".  ResolveToolPath with
    // the cwd as both input and reference canonicalizes it.
    const std::string target = inputPath.empty() ? ctx.cwd : inputPath;
    std::string resolved = ResolveToolPath(target, ctx.cwd);
    if (resolved.empty()) {
        r.chips.push_back("failed");
        r.errorBody = "Could not resolve path: " + target;
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    // ── Existence / type check ───────────────────────────────────
    if (!IsDirectory(resolved)) {
        r.chips.push_back("failed");
        if (IsFile(resolved)) {
            r.errorBody = "Not a directory (is a file): " + resolved;
        } else {
            r.errorBody = "Directory not found: " + resolved;
        }
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    // ── Enumerate via FindFirstFileW ─────────────────────────────
    // Pattern "<dir>\*" yields every entry; we filter . and .. as
    // we go.  UNC paths ("\\server\share\*") work identically.
    std::wstring wPattern = Utf8ToWide(resolved) + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = ::FindFirstFileW(wPattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        DWORD err = ::GetLastError();
        r.chips.push_back("failed");
        r.errorBody = "Could not read directory: " + resolved;
        if (err == ERROR_ACCESS_DENIED)       r.errorBody += " (access denied)";
        else if (err == ERROR_FILE_NOT_FOUND) r.errorBody += " (no entries)";
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    std::vector<LsEntry> dirs, files;
    bool hitEntryCap = false;
    size_t totalScanned = 0;

    do {
        std::wstring wname(fd.cFileName);
        if (wname == L"." || wname == L"..") continue;

        ++totalScanned;
        if (dirs.size() + files.size() >= kLsEntryCap) {
            hitEntryCap = true;
            // Keep scanning so we can report an accurate total in
            // the "showing X of Y+" marker.
            continue;
        }

        LsEntry e;
        e.name = WideToUtf8(wname);
        e.isDirectory = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.sizeBytes = e.isDirectory ? 0
            : (((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
        e.mtimeFormatted = FormatFileTime(fd.ftLastWriteTime);

        if (e.isDirectory) dirs.push_back(std::move(e));
        else               files.push_back(std::move(e));
    } while (::FindNextFileW(hFind, &fd));
    ::FindClose(hFind);

    // Keep counting beyond the cap so "X of Y+" is honest.
    // (FindNextFileW loop exited at EOF, not at the cap.)  The
    // totalScanned counter already reflects everything seen.

    // ── Sort each group alphabetically (case-insensitive) ────────
    auto byName = [](const LsEntry& a, const LsEntry& b) {
        return NameLess(a.name, b.name);
    };
    std::sort(dirs.begin(),  dirs.end(),  byName);
    std::sort(files.begin(), files.end(), byName);

    // ── Compute name-column width ────────────────────────────────
    // Dir names get a trailing "/" in the display, so their display
    // length is +1.  Width is max(floor, longest display length).
    size_t maxNameLen = 0;
    for (const auto& e : dirs)
        maxNameLen = std::max(maxNameLen, e.name.size() + 1); // +1 for "/"
    for (const auto& e : files)
        maxNameLen = std::max(maxNameLen, e.name.size());
    const size_t nameColWidth = std::max(kMinNameColWidth, maxNameLen);

    // ── Emit rows, bounded by byte cap ───────────────────────────
    const size_t byteCap = ComputeLsByteCap(ctx.ctxTokens);
    std::ostringstream ss;
    size_t emitted = 0;
    bool hitByteCap = false;

    auto emitRow = [&](const LsEntry& e, bool isDir) -> bool {
        std::string displayName = e.name + (isDir ? "/" : "");

        std::ostringstream line;
        // Name, left-padded to column width.  Long names overflow
        // the column; alignment on *that* row breaks but subsequent
        // rows stay aligned.
        line << std::left << std::setw((int)nameColWidth) << displayName;
        line << "  ";
        // Size, right-aligned.  Directories leave the column blank.
        if (isDir) {
            line << std::setw((int)kSizeColWidth) << std::right << "";
        } else {
            line << std::setw((int)kSizeColWidth) << std::right
                 << HumanBytes(e.sizeBytes);
        }
        line << "  " << e.mtimeFormatted << "\n";

        std::string lineStr = line.str();
        // Check byte cap BEFORE emitting.  streampos on an
        // ostringstream tracks cumulative bytes written.
        if ((size_t)ss.tellp() + lineStr.size() > byteCap) {
            hitByteCap = true;
            return false;
        }
        ss << lineStr;
        ++emitted;
        return true;
    };

    for (const auto& e : dirs) {
        if (!emitRow(e, /*isDir*/ true)) break;
    }
    if (!hitByteCap) {
        for (const auto& e : files) {
            if (!emitRow(e, /*isDir*/ false)) break;
        }
    }

    // ── Empty directory ──────────────────────────────────────────
    if (emitted == 0 && !hitEntryCap && !hitByteCap) {
        ss << "(empty directory)\n";
    }

    // ── Truncation marker ────────────────────────────────────────
    const size_t totalEntries = totalScanned;
    if (hitEntryCap || hitByteCap) {
        ss << "\n[... truncated: showing " << emitted
           << " of " << totalEntries << " entries";
        if (hitByteCap)
            ss << " to fit model context (ctx=" << ctx.ctxTokens
               << " tokens)";
        ss << " ...]\n";
    }

    // ── Chips ────────────────────────────────────────────────────
    // Primary chip: entry count.  Then truncated (if any).  Then
    // elapsed, always last for consistency with /cmd and /read.
    {
        std::ostringstream countChip;
        countChip << totalEntries
                  << (totalEntries == 1 ? " entry" : " entries");
        r.chips.push_back(countChip.str());
    }
    if (hitEntryCap || hitByteCap) r.chips.push_back("truncated");

    r.body = ss.str();
    r.bodyLang = "";  // plain fixed-width text
    r.chips.push_back(ElapsedChip(t0));
    return r;
}
