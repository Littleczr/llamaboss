// tool_grep.cpp

#include "tool_grep.h"
#include "tool_path.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

wxDEFINE_EVENT(wxEVT_GREP_COMPLETE, wxCommandEvent);

namespace {

// ─── Limits ──────────────────────────────────────────────────────
// Same ctx-aware byte-cap formula as /read and /ls.  Applied after
// kMaxMatches — if we hit the match cap first, body is bounded by
// that; if we hit the byte cap first, body is bounded by that.
constexpr int    kReservedTokens    = 3000;
constexpr size_t kGrepByteCapFloor   =   4 * 1024;
constexpr size_t kGrepByteCapCeiling = 512 * 1024;

size_t ComputeGrepByteCap(int ctxTokens)
{
    int usable = (ctxTokens > kReservedTokens)
                 ? (ctxTokens - kReservedTokens) : 0;
    size_t cap = (size_t)usable * 3;
    if (cap < kGrepByteCapFloor)   cap = kGrepByteCapFloor;
    if (cap > kGrepByteCapCeiling) cap = kGrepByteCapCeiling;
    return cap;
}

// ─── UTF-8 helpers (local copies, same as other tools) ──────────
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

// ─── Dir-skip policy ─────────────────────────────────────────────
// Recursive walks skip any directory that:
//   - starts with "." (covers .git, .vs, .vscode, .idea, .hg, .svn,
//     .gradle, .pytest_cache, …)
//   - matches a small blacklist of common build/cache dirs by
//     name (case-insensitive).
// Dot-prefix FILES are NOT skipped — .gitconfig, .env, .dockerfile
// are legitimate grep targets.
bool ShouldSkipDir(const std::string& name)
{
    if (!name.empty() && name[0] == '.') return true;

    static const char* const kBlacklist[] = {
        "node_modules",
        "x64", "x86", "Win32",
        "Debug", "Release",
        "bin", "obj",
        "vcpkg_installed",
        "target",         // Rust
        "build", "cmake-build-debug", "cmake-build-release",
        "dist", "out",
        "__pycache__",
    };
    for (const char* b : kBlacklist) {
        if (_stricmp(name.c_str(), b) == 0) return true;
    }
    return false;
}

// ─── Binary sniff (same rule as /read) ───────────────────────────
bool IsLikelyBinary(const char* data, size_t n)
{
    size_t check = std::min(n, (size_t)4096);
    for (size_t i = 0; i < check; ++i) {
        if (data[i] == '\0') return true;
    }
    return false;
}

// ─── Elapsed-time chip ───────────────────────────────────────────
std::string FormatElapsed(double elapsed)
{
    std::ostringstream ts;
    ts << std::fixed;
    ts.precision(elapsed < 10.0 ? 2 : 1);
    ts << elapsed << "s";
    return ts.str();
}

// ─── Search state passed through the recursive walk ─────────────
struct SearchState {
    const std::string&                        pattern;
    const std::shared_ptr<std::atomic<bool>>& cancelFlag;
    unsigned long                             timeoutMs;
    std::chrono::steady_clock::time_point     t0;

    struct Match {
        std::string path;     // relative to search root, or basename for file-mode
        size_t      lineNo;   // 1-based
        std::string line;     // truncated to kMaxLineLength
    };

    std::vector<Match> matches;
    size_t filesScanned = 0;
    bool   hitMatchCap = false;
    bool   hitFileCap  = false;
    bool   cancelled   = false;
    bool   timedOut    = false;
};

// Returns true to continue; false if we should stop (cancel, timeout,
// or a hard cap fired).  Called before every file open and every ~256
// lines inside a file.
bool CheckLimits(SearchState& s)
{
    if (s.cancelFlag->load()) { s.cancelled = true; return false; }

    if (s.timeoutMs > 0) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - s.t0).count();
        if ((unsigned long)ms >= s.timeoutMs) {
            s.timedOut = true;
            return false;
        }
    }

    if (s.matches.size() >= GrepExecutor::kMaxMatches) {
        s.hitMatchCap = true;
        return false;
    }
    if (s.filesScanned >= GrepExecutor::kMaxFilesScanned) {
        s.hitFileCap = true;
        return false;
    }
    return true;
}

// Opens absPath and scans for literal matches.  Skips binary files
// silently.  Returns false to stop the walk (hard cap / cancel).
bool SearchFile(const std::string& absPath,
                const std::string& relPath,
                SearchState& s)
{
    if (!CheckLimits(s)) return false;

    // Open in binary mode so we see \r explicitly (and can strip it
    // reliably from getline output on Windows line endings).  MSVC
    // accepts std::wstring paths as an extension.
    std::ifstream f(Utf8ToWide(absPath), std::ios::binary);
    if (!f) return true;   // permissions, transient IO — skip, keep walking

    // Size check — bail on huge files (binaries, minified bundles)
    f.seekg(0, std::ios::end);
    std::streamoff fsize = f.tellg();
    f.seekg(0, std::ios::beg);
    if (fsize <= 0) {
        ++s.filesScanned;
        return true;
    }
    if ((uint64_t)fsize > GrepExecutor::kMaxFileBytes) {
        ++s.filesScanned;
        return true;
    }

    // Sniff first 4 KiB for NULs — same binary rule as /read.
    char sniff[4096];
    std::streamsize sniffLen = std::min<std::streamsize>(
        (std::streamsize)fsize, (std::streamsize)sizeof(sniff));
    f.read(sniff, sniffLen);
    std::streamsize gotten = f.gcount();
    if (IsLikelyBinary(sniff, (size_t)gotten)) {
        ++s.filesScanned;
        return true;
    }
    f.clear();
    f.seekg(0, std::ios::beg);

    ++s.filesScanned;

    std::string line;
    size_t lineNo = 0;
    while (std::getline(f, line)) {
        ++lineNo;

        // Cheap periodic limit check — avoids a syscall/atomic load
        // per line on large files.  256 lines ≈ 10 KB text — plenty
        // responsive to cancellation.
        if ((lineNo & 0xFF) == 0) {
            if (!CheckLimits(s)) return false;
        }

        // Strip trailing \r from Windows line endings.
        if (!line.empty() && line.back() == '\r') line.pop_back();

        // Literal substring search.  std::string::find is fine here —
        // optimized impls use Boyer-Moore or similar on most libcxx.
        if (line.find(s.pattern) == std::string::npos) continue;

        // Truncate long match lines.  Minified JS / generated code
        // can have 10K+ char lines; we'd obliterate the output.
        SearchState::Match m;
        m.path   = relPath;
        m.lineNo = lineNo;
        if (line.size() > GrepExecutor::kMaxLineLength) {
            m.line = line.substr(0, GrepExecutor::kMaxLineLength - 3);
            m.line += "...";
        } else {
            m.line = std::move(line);
        }
        s.matches.push_back(std::move(m));

        if (s.matches.size() >= GrepExecutor::kMaxMatches) {
            s.hitMatchCap = true;
            return false;
        }
    }
    return true;
}

// Recursive descent.  Dirs are ordered alphabetically (deterministic
// output across runs).  Files are processed before subdirs within
// each directory — matches cluster by depth, which tends to read
// more naturally than a purely depth-first interleaving.
bool WalkAndSearch(const std::string& absDir,
                   const std::string& relPrefix,
                   SearchState& s)
{
    if (!CheckLimits(s)) return false;

    std::wstring wPat = Utf8ToWide(absDir) + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = ::FindFirstFileW(wPat.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return true;   // silently skip unreadable dir

    std::vector<std::pair<std::string, bool>> fileEntries; // name, dummy false
    std::vector<std::string>                   dirEntries; // name

    do {
        std::wstring wname(fd.cFileName);
        if (wname == L"." || wname == L"..") continue;

        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        std::string name = WideToUtf8(wname);

        if (isDir) {
            if (ShouldSkipDir(name)) continue;
            dirEntries.push_back(std::move(name));
        } else {
            fileEntries.emplace_back(std::move(name), false);
        }
    } while (::FindNextFileW(hFind, &fd));
    ::FindClose(hFind);

    std::sort(fileEntries.begin(), fileEntries.end(),
              [](const auto& a, const auto& b){ return a.first < b.first; });
    std::sort(dirEntries.begin(), dirEntries.end());

    // Files first — this directory's matches land contiguous in the
    // output before we dive into subdirs.
    for (const auto& [name, _] : fileEntries) {
        std::string childAbs = absDir + "\\" + name;
        std::string childRel = relPrefix.empty() ? name : (relPrefix + "\\" + name);
        if (!SearchFile(childAbs, childRel, s)) return false;
    }
    for (const auto& name : dirEntries) {
        std::string childAbs = absDir + "\\" + name;
        std::string childRel = relPrefix.empty() ? name : (relPrefix + "\\" + name);
        if (!WalkAndSearch(childAbs, childRel, s)) return false;
    }
    return true;
}

// Extract basename for single-file searches — display uses just the
// filename, not the full absolute path, in match-line prefixes.
std::string Basename(const std::string& path)
{
    size_t a = path.find_last_of("\\/");
    return (a == std::string::npos) ? path : path.substr(a + 1);
}

// ─── Worker thread ───────────────────────────────────────────────
class GrepWorker : public wxThread {
public:
    GrepWorker(wxEvtHandler* handler,
               std::weak_ptr<std::atomic<bool>>   alive,
               std::shared_ptr<std::atomic<bool>> cancel,
               std::shared_ptr<std::atomic<bool>> running,
               std::string pattern,
               std::string resolvedPath,
               std::string commandEcho,
               ToolContext ctx)
        : wxThread(wxTHREAD_DETACHED)
        , m_handler(handler)
        , m_alive(std::move(alive))
        , m_cancel(std::move(cancel))
        , m_running(std::move(running))
        , m_pattern(std::move(pattern))
        , m_resolvedPath(std::move(resolvedPath))
        , m_commandEcho(std::move(commandEcho))
        , m_ctx(std::move(ctx))
    {}

    ExitCode Entry() override
    {
        GrepResult result;
        result.commandEcho = m_commandEcho;

        auto t0 = std::chrono::steady_clock::now();
        SearchState s{
            m_pattern,
            m_cancel,
            m_ctx.timeoutMs,
            t0,
            /*matches*/    {},
            /*filesScanned*/ 0,
            /*hitMatchCap*/  false,
            /*hitFileCap*/   false,
            /*cancelled*/    false,
            /*timedOut*/     false,
        };

        const bool isFile = IsFile(m_resolvedPath);
        const bool isDir  = IsDirectory(m_resolvedPath);

        if (isFile) {
            // Single-file mode: use basename in match prefixes.
            SearchFile(m_resolvedPath, Basename(m_resolvedPath), s);
        } else if (isDir) {
            WalkAndSearch(m_resolvedPath, /*relPrefix*/ "", s);
        }
        // No-else: invalid path should have been caught by caller;
        // we'd emit 0 matches in that case, harmless.

        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count();
        result.elapsedSec = elapsed;
        result.cancelled  = s.cancelled;
        result.timedOut   = s.timedOut;

        // ── Build body ───────────────────────────────────────────
        // Byte-cap bounds the output string regardless of how many
        // matches we collected.  If we hit the byte cap we still
        // truncate mid-output (match-granular, never mid-line).
        std::ostringstream body;
        const size_t byteCap = ComputeGrepByteCap(m_ctx.ctxTokens);
        size_t emittedMatches = 0;
        bool   hitByteCap = false;

        for (const auto& m : s.matches) {
            std::ostringstream line;
            line << m.path << ":" << m.lineNo << ": " << m.line << "\n";
            std::string ls = line.str();
            if ((size_t)body.tellp() + ls.size() > byteCap) {
                hitByteCap = true;
                break;
            }
            body << ls;
            ++emittedMatches;
        }

        const bool anyTrunc = s.hitMatchCap || s.hitFileCap || hitByteCap;
        if (anyTrunc) {
            body << "\n[... truncated: showing " << emittedMatches
                 << " of " << s.matches.size() << " collected matches";
            if (s.hitFileCap)
                body << " (file-scan cap " << GrepExecutor::kMaxFilesScanned
                     << " hit — some subtrees skipped)";
            if (s.hitMatchCap && !s.hitFileCap)
                body << " (match cap " << GrepExecutor::kMaxMatches << " hit)";
            if (hitByteCap)
                body << " (ctx byte cap hit — ctx=" << m_ctx.ctxTokens
                     << " tokens)";
            body << " ...]\n";
        }

        if (emittedMatches == 0 && !s.cancelled && !s.timedOut) {
            // Distinguish "no matches" from "you got nothing because
            // we bailed out" — cancelled/timedOut cases fall through
            // to the chips layer without a body marker.
            body << "(no matches)\n";
        }

        result.body = body.str();
        result.bodyLang = "";

        // ── Build chips ──────────────────────────────────────────
        // Order: match count, files-scanned (dir mode only),
        // truncated, cancelled/timed-out, elapsed (always last).
        {
            std::ostringstream c;
            c << s.matches.size()
              << (s.matches.size() == 1 ? " match" : " matches");
            result.chips.push_back(c.str());
        }
        if (isDir) {
            std::ostringstream c;
            c << s.filesScanned
              << (s.filesScanned == 1 ? " file scanned" : " files scanned");
            result.chips.push_back(c.str());
        }
        if (anyTrunc)       result.chips.push_back("truncated");
        if (s.cancelled)    result.chips.push_back("cancelled");
        if (s.timedOut)     result.chips.push_back("timed out");
        result.chips.push_back(FormatElapsed(elapsed));

        // ── Post back to UI thread, if it's still alive ──────────
        auto alive = m_alive.lock();
        if (alive && alive->load()) {
            auto* evt = new wxCommandEvent(wxEVT_GREP_COMPLETE);
            evt->SetClientObject(new GrepResultClientData(std::move(result)));
            m_handler->QueueEvent(evt);
        }

        m_running->store(false);
        return (ExitCode)0;
    }

private:
    wxEvtHandler*                      m_handler;
    std::weak_ptr<std::atomic<bool>>   m_alive;
    std::shared_ptr<std::atomic<bool>> m_cancel;
    std::shared_ptr<std::atomic<bool>> m_running;
    std::string                        m_pattern;
    std::string                        m_resolvedPath;
    std::string                        m_commandEcho;
    ToolContext                        m_ctx;
};

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════
//  GrepExecutor
// ═══════════════════════════════════════════════════════════════════

GrepExecutor::GrepExecutor(wxEvtHandler* eventHandler,
                           std::weak_ptr<std::atomic<bool>> aliveToken)
    : m_eventHandler(eventHandler)
    , m_aliveToken(std::move(aliveToken))
    , m_cancelFlag(std::make_shared<std::atomic<bool>>(false))
    , m_isRunning(std::make_shared<std::atomic<bool>>(false))
{}

GrepExecutor::~GrepExecutor()
{
    // Cancel any in-flight worker so it checks the flag and exits
    // promptly.  Worker holds its own shared_ptr copies of the
    // cancel/running flags, so state outlives us cleanly.
    Cancel();
}

bool GrepExecutor::Start(const std::string& pattern,
                         const std::string& resolvedPath,
                         const std::string& commandEcho,
                         const ToolContext& ctx)
{
    if (IsRunning())     return false;
    if (pattern.empty()) return false;

    m_cancelFlag->store(false);
    m_isRunning->store(true);

    // wxThread::Create allocates the OS thread; Run schedules it.
    // Detached, so we don't own the pointer after Run returns.
    auto* worker = new GrepWorker(
        m_eventHandler, m_aliveToken, m_cancelFlag, m_isRunning,
        pattern, resolvedPath, commandEcho, ctx);

    if (worker->Create() != wxTHREAD_NO_ERROR) {
        m_isRunning->store(false);
        delete worker;
        return false;
    }
    if (worker->Run() != wxTHREAD_NO_ERROR) {
        m_isRunning->store(false);
        // Detached thread deletes itself on Entry exit; Run failure
        // is rare and leaves ownership ambiguous — don't double-delete.
        return false;
    }
    return true;
}

void GrepExecutor::Cancel()
{
    if (m_cancelFlag) m_cancelFlag->store(true);
}
