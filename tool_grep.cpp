// tool_grep.cpp

#include "tool_grep.h"
#include "tool_path.h"

#include <deque>

#include "lb_windows.h"
#ifndef _WIN32
#include <dirent.h>
#include <strings.h>
#endif
#include "lb_utf.h"
#include "ui_event_post.h"

wxDEFINE_EVENT(wxEVT_GREP_COMPLETE, wxCommandEvent);

namespace {

#ifdef _WIN32
constexpr const char* kPathSep = "\\";
#else
constexpr const char* kPathSep = "/";
#endif

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

// Shrink a byte-length clamp back to a UTF-8 character boundary.
// Match lines are truncated at kMaxLineLength BYTES, which splits a
// multi-byte character most of the time on CJK text and occasionally on
// accented Latin text.  The truncated sequence then rides into
// ChatHistory and out through Poco's JSON stringifier, which is called
// with default options and emits non-ASCII verbatim; llama-server's
// parser rejects malformed UTF-8 in a string, failing the whole request.
// Never grows the clamp, and leaves already-malformed input untouched.
size_t ClampToUtf8Boundary(const char* data, size_t len)
{
    if (len == 0) return 0;

    size_t j = len;
    while (j > 0 && ((unsigned char)data[j - 1] & 0xC0) == 0x80) --j;
    if (j == 0) return len;                    // no lead byte in range

    const size_t leadIdx  = j - 1;
    const unsigned char c = (unsigned char)data[leadIdx];

    size_t seqLen;
    if      ((c & 0x80) == 0x00) seqLen = 1;
    else if ((c & 0xE0) == 0xC0) seqLen = 2;
    else if ((c & 0xF0) == 0xE0) seqLen = 3;
    else if ((c & 0xF8) == 0xF0) seqLen = 4;
    else return len;                           // invalid lead byte

    return (leadIdx + seqLen <= len) ? len : leadIdx;
}

// Truncate a match line to kMaxLineLength without splitting a character.
std::string ClampMatchLine(const std::string& text)
{
    if (text.size() <= GrepExecutor::kMaxLineLength) return text;
    const size_t keep = ClampToUtf8Boundary(
        text.data(), GrepExecutor::kMaxLineLength - 3);
    return text.substr(0, keep) + "...";
}

std::wstring Utf8ToWide(const std::string& s)
{
#ifndef _WIN32
    return lb_utf::Utf8ToWide(s);
#else
    if (s.empty()) return L"";
    int len = ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                    (int)s.size(), nullptr, 0);
    if (len <= 0) return L"";
    std::wstring w((size_t)len, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(),
                          &w[0], len);
    return w;
#endif
}

std::string WideToUtf8(const std::wstring& w)
{
#ifndef _WIN32
    return lb_utf::WideToUtf8(w);
#else
    if (w.empty()) return "";
    int len = ::WideCharToMultiByte(CP_UTF8, 0, w.data(),
                                    (int)w.size(),
                                    nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string s((size_t)len, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                          &s[0], len, nullptr, nullptr);
    return s;
#endif
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
#ifdef _WIN32
        if (_stricmp(name.c_str(), b) == 0) return true;
#else
        if (::strcasecmp(name.c_str(), b) == 0) return true;
#endif
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
    size_t                                    contextLines;

    struct Match {
        std::string path;     // relative to search root, or basename for file-mode
        size_t      lineNo;   // 1-based
        std::string line;     // truncated to kMaxLineLength
    };

    std::vector<Match> matches;

    struct ContextOutputLine {
        std::string path;
        size_t      lineNo;
        std::string line;
        bool        isMatch;
        bool        separatorBefore;
    };
    std::vector<ContextOutputLine> contextOutput;
    size_t filesScanned = 0;
    bool   hitMatchCap = false;
    bool   hitFileCap  = false;
    bool   cancelled   = false;
    bool   timedOut    = false;

    // ── Skip accounting: honest negatives ───────────────────────
    // A file we never opened must never be folded into a "(no matches)"
    // body.  That turns an unsearched file into a confident absence, and
    // the caller cannot tell the difference.  Observed in production: a
    // model grepped three DLLs for a build string, got 0 matches each
    // time because the binary sniff skipped them silently, and concluded
    // "the version string isn't greppable in the binary" — it is, the
    // files were simply never read.
    //
    // filesScanned stays as-is because kMaxFilesScanned accounting and
    // the walk's cap checks depend on counting every candidate.
    // filesSearched counts only files actually read as text, and is what
    // the chip now reports.
    size_t filesSearched     = 0;
    size_t skippedBinary     = 0;
    size_t skippedTooLarge   = 0;
    size_t skippedUnreadable = 0;   // open failed: permissions, transient IO
    size_t prunedDirs        = 0;   // ShouldSkipDir: dot-dirs, node_modules, bin, obj, ...
};

bool CheckCancellationOrTimeout(SearchState& s)
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
    return true;
}

// Returns true to continue; false if we should stop (cancel, timeout,
// or a hard cap fired).  Called before every file open and every ~256
// lines inside a file.
// grep is a LITERAL substring search (by design: predictable, fast, and
// documented as such in the tool description and prompt).  Models
// trained on rg/grep still write regex, and a regex pattern searched
// literally comes back "(no matches)" -- a confident false negative.
// One Luna turn lost about a fifth of its tool budget this way
// (`a|b|c`, `Name\(`), retrying names that a plain search found at
// once.  When a zero-match pattern carries regex syntax, say so and
// name the literal terms to search instead.  Matching is unchanged.
std::string RegexAttemptHint(const std::string& pattern)
{
    bool looksRegex = false;
    if (pattern.find('|') != std::string::npos) looksRegex = true;
    if (pattern.find(".*") != std::string::npos ||
        pattern.find(".+") != std::string::npos) looksRegex = true;
    if (!pattern.empty() && (pattern.front() == '^' || pattern.back() == '$'))
        looksRegex = true;
    for (size_t i = 0; i + 1 < pattern.size(); ++i) {
        if (pattern[i] != '\\') continue;
        const char n = pattern[i + 1];
        if (n == '(' || n == ')' || n == '.' || n == '[' || n == ']' ||
            n == 's' || n == 'b' || n == 'w' || n == 'd' || n == '|' ||
            n == '*' || n == '+' || n == '?') { looksRegex = true; break; }
    }
    if (!looksRegex) return {};

    std::string hint =
        "[grep matches literal text only; regex syntax is not interpreted, so "
        "'|' is not alternation and an escape like '\\(' searches for a "
        "backslash. This \"(no matches)\" applies only to that exact text.";

    // Name the alternatives, with regex escapes removed, so the next call
    // is obvious.  Only when '|' actually separates non-empty terms.
    std::vector<std::string> terms;
    std::string cur;
    for (size_t i = 0; i < pattern.size(); ++i) {
        const char c = pattern[i];
        if (c == '\\' && i + 1 < pattern.size()) { cur += pattern[++i]; continue; }
        if (c == '|') { terms.push_back(cur); cur.clear(); continue; }
        cur += c;
    }
    terms.push_back(cur);
    // An empty term means '||' or a leading/trailing '|': most likely a
    // literal C++/shell operator search, so don't propose a split.
    for (const std::string& t : terms)
        if (t.empty()) { hint += "]\n"; return hint; }

    std::vector<std::string> named;
    for (std::string t : terms) {
        if (!t.empty() && t.front() == '^') t.erase(0, 1);
        if (!t.empty() && t.back() == '$') t.pop_back();
        if (!t.empty()) named.push_back(t);
    }
    if (named.size() >= 2 && named.size() <= 8) {
        hint += " Search the terms separately:";
        for (size_t i = 0; i < named.size(); ++i)
            hint += (i ? ", '" : " '") + named[i] + "'";
        hint += ".";
    } else if (named.size() == 1 && named[0] != pattern) {
        hint += " Literal form: '" + named[0] + "'.";
    }
    hint += "]\n";
    return hint;
}

bool CheckLimits(SearchState& s)
{
    if (!CheckCancellationOrTimeout(s)) return false;

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
    std::ifstream f(std::filesystem::path(Utf8ToWide(absPath)), std::ios::binary);
    if (!f) {
        ++s.skippedUnreadable;   // permissions, transient IO — skip, keep walking
        return true;
    }

    // Size check — bail on huge files (binaries, minified bundles)
    f.seekg(0, std::ios::end);
    std::streamoff fsize = f.tellg();
    f.seekg(0, std::ios::beg);
    if (fsize <= 0) {
        // Empty file: opened successfully and genuinely contains no
        // match, so this IS a true negative — counts as searched.
        ++s.filesScanned;
        ++s.filesSearched;
        return true;
    }
    if ((uint64_t)fsize > GrepExecutor::kMaxFileBytes) {
        ++s.filesScanned;
        ++s.skippedTooLarge;
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
        ++s.skippedBinary;
        return true;
    }
    f.clear();
    f.seekg(0, std::ios::beg);

    ++s.filesScanned;
    ++s.filesSearched;

    // Context mode is deliberately separate from the context=0 path below,
    // preserving the original match-only output byte-for-byte.  Stream the
    // file with a bounded deque of preceding lines and an after-match
    // countdown; overlapping neighborhoods merge without duplicate lines.
    if (s.contextLines > 0) {
        struct RecentLine {
            size_t      lineNo;
            std::string line;
        };

        std::deque<RecentLine> recent;
        size_t lineNo = 0;
        size_t lastOutputLine = 0;
        size_t afterRemaining = 0;
        bool producedForFile = false;
        bool stoppingAtMatchCap = false;

        auto clampLine = [](const std::string& text) {
            return ClampMatchLine(text);   // UTF-8-boundary safe
        };

        auto appendOutput = [&](size_t number,
                                const std::string& text,
                                bool isMatch,
                                bool separatorBefore) {
            SearchState::ContextOutputLine item;
            item.path = relPath;
            item.lineNo = number;
            item.line = clampLine(text);
            item.isMatch = isMatch;
            item.separatorBefore = separatorBefore;
            s.contextOutput.push_back(std::move(item));
            lastOutputLine = number;
            producedForFile = true;
        };

        std::string line;
        while (std::getline(f, line)) {
            ++lineNo;
            if ((lineNo & 0xFF) == 0 &&
                !CheckCancellationOrTimeout(s)) {
                return false;
            }
            if (!line.empty() && line.back() == '\r') line.pop_back();

            const bool isMatch = !stoppingAtMatchCap &&
                line.find(s.pattern) != std::string::npos;

            if (isMatch) {
                const size_t contextStart = lineNo > s.contextLines
                    ? lineNo - s.contextLines : 1;
                bool separatorPending =
                    (!producedForFile && !s.contextOutput.empty()) ||
                    (producedForFile && contextStart > lastOutputLine + 1);

                for (const RecentLine& prev : recent) {
                    if (prev.lineNo < contextStart ||
                        prev.lineNo <= lastOutputLine) {
                        continue;
                    }
                    appendOutput(prev.lineNo, prev.line, false,
                                 separatorPending);
                    separatorPending = false;
                }

                appendOutput(lineNo, line, true, separatorPending);

                SearchState::Match m;
                m.path = relPath;
                m.lineNo = lineNo;
                m.line = clampLine(line);
                s.matches.push_back(std::move(m));
                afterRemaining = s.contextLines;

                if (s.matches.size() >= GrepExecutor::kMaxMatches) {
                    s.hitMatchCap = true;
                    stoppingAtMatchCap = true;
                }
            } else if (afterRemaining > 0) {
                if (lineNo > lastOutputLine) {
                    appendOutput(lineNo, line, false, false);
                }
                --afterRemaining;
            }

            recent.push_back({ lineNo, line });
            while (recent.size() > s.contextLines) recent.pop_front();

            // Once the match cap fires, retain only the requested trailing
            // context for the final accepted match, then stop the whole walk.
            if (stoppingAtMatchCap && afterRemaining == 0) return false;
        }
        return !stoppingAtMatchCap;
    }

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
            m.line = ClampMatchLine(line);   // UTF-8-boundary safe
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

#ifdef _WIN32
    std::wstring wPat = Utf8ToWide(absDir) + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = ::FindFirstFileW(wPat.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        ++s.skippedUnreadable;   // unreadable dir — counted, no longer silent
        return true;
    }

    std::vector<std::pair<std::string, bool>> fileEntries; // name, dummy false
    std::vector<std::string>                   dirEntries; // name

    do {
        std::wstring wname(fd.cFileName);
        if (wname == L"." || wname == L"..") continue;

        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        std::string name = WideToUtf8(wname);

        if (isDir) {
            if (ShouldSkipDir(name)) { ++s.prunedDirs; continue; }
            dirEntries.push_back(std::move(name));
        } else {
            fileEntries.emplace_back(std::move(name), false);
        }
    } while (::FindNextFileW(hFind, &fd));
    ::FindClose(hFind);

#else
    DIR* dirp = ::opendir(absDir.c_str());
    if (!dirp) {
        ++s.skippedUnreadable;   // unreadable dir — counted, no longer silent
        return true;
    }

    std::vector<std::pair<std::string, bool>> fileEntries; // name, dummy false
    std::vector<std::string>                   dirEntries; // name

    while (struct dirent* de = ::readdir(dirp)) {
        std::string name = de->d_name;
        if (name == "." || name == "..") continue;

        bool isDir = de->d_type == DT_DIR;
        if (de->d_type == DT_UNKNOWN || de->d_type == DT_LNK) {
            // Symlinked files are searched; symlinked directories are not
            // followed, so a link cycle cannot trap the walk.
            struct stat st {};
            const std::string full = absDir + "/" + name;
            if (::lstat(full.c_str(), &st) != 0) continue;
            if (S_ISLNK(st.st_mode)) {
                if (::stat(full.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) continue;
            }
            isDir = S_ISDIR(st.st_mode);
        }

        if (isDir) {
            if (ShouldSkipDir(name)) { ++s.prunedDirs; continue; }
            dirEntries.push_back(std::move(name));
        } else {
            fileEntries.emplace_back(std::move(name), false);
        }
    }
    ::closedir(dirp);
#endif

    std::sort(fileEntries.begin(), fileEntries.end(),
              [](const auto& a, const auto& b){ return a.first < b.first; });
    std::sort(dirEntries.begin(), dirEntries.end());

    // Files first — this directory's matches land contiguous in the
    // output before we dive into subdirs.
    for (const auto& [name, _] : fileEntries) {
        std::string childAbs = absDir + kPathSep + name;
        std::string childRel = relPrefix.empty() ? name : (relPrefix + kPathSep + name);
        if (!SearchFile(childAbs, childRel, s)) return false;
    }
    for (const auto& name : dirEntries) {
        std::string childAbs = absDir + kPathSep + name;
        std::string childRel = relPrefix.empty() ? name : (relPrefix + kPathSep + name);
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
               ToolContext ctx,
               size_t contextLines)
        : wxThread(wxTHREAD_DETACHED)
        , m_handler(handler)
        , m_alive(std::move(alive))
        , m_cancel(std::move(cancel))
        , m_running(std::move(running))
        , m_pattern(std::move(pattern))
        , m_resolvedPath(std::move(resolvedPath))
        , m_commandEcho(std::move(commandEcho))
        , m_ctx(std::move(ctx))
        , m_contextLines(contextLines)
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
            m_contextLines,
            /*matches*/    {},
            /*contextOutput*/ {},
            /*filesScanned*/ 0,
            /*hitMatchCap*/  false,
            /*hitFileCap*/   false,
            /*cancelled*/    false,
            /*timedOut*/     false,
            /*filesSearched*/     0,
            /*skippedBinary*/     0,
            /*skippedTooLarge*/   0,
            /*skippedUnreadable*/ 0,
            /*prunedDirs*/        0,
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
        // matches/context we collected.  If we hit the byte cap we still
        // truncate at a whole output line, never mid-line.
        std::ostringstream body;
        const size_t byteCap = ComputeGrepByteCap(m_ctx.ctxTokens);
        size_t emittedMatches = 0;
        bool   hitByteCap = false;

        if (m_contextLines == 0) {
            // Compatibility path: preserve the original match-only format.
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
        } else {
            // GNU-grep-style distinction: ':' marks a matching line and '-'
            // marks surrounding context.  "--" separates disjoint groups
            // and files; overlapping neighborhoods were already merged.
            std::string group;
            size_t groupMatches = 0;
            bool emittedAnyGroup = false;

            auto flushGroup = [&]() -> bool {
                if (group.empty()) return true;
                const size_t separatorBytes = emittedAnyGroup ? 3 : 0; // "--\n"
                if ((size_t)body.tellp() + separatorBytes + group.size()
                    > byteCap) {
                    hitByteCap = true;
                    return false;
                }
                if (emittedAnyGroup) body << "--\n";
                body << group;
                emittedMatches += groupMatches;
                emittedAnyGroup = true;
                group.clear();
                groupMatches = 0;
                return true;
            };

            for (const auto& item : s.contextOutput) {
                if (item.separatorBefore && !flushGroup()) break;
                std::ostringstream line;
                const char sep = item.isMatch ? ':' : '-';
                line << item.path << sep << item.lineNo << sep << ' '
                     << item.line << "\n";
                group += line.str();
                if (item.isMatch) ++groupMatches;
            }
            if (!hitByteCap) flushGroup();
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

        // ── Skip summary ─────────────────────────────────────────
        // Built before the "(no matches)" marker because it decides
        // whether that marker is even truthful.  Reasons are listed
        // individually: "binary" tells the caller to reach for a
        // different tool, "too large" tells them to narrow the target,
        // and the two need different remedies.
        const size_t skippedFiles =
            s.skippedBinary + s.skippedTooLarge + s.skippedUnreadable;

        std::string skipReasons;
        {
            auto addReason = [&](size_t n, const char* label) {
                if (n == 0) return;
                if (!skipReasons.empty()) skipReasons += ", ";
                skipReasons += std::to_string(n);
                skipReasons += " ";
                skipReasons += label;
            };
            addReason(s.skippedBinary,     "binary");
            addReason(s.skippedTooLarge,   "over the 10 MiB per-file limit");
            addReason(s.skippedUnreadable, "unreadable");
        }

        // Nothing was actually read, so there is no negative to report --
        // only a failure.  A confident "(no matches)" would be wrong for
        // a file grep can't search at all.
        std::string skipError;
        if (skippedFiles > 0 && s.filesSearched == 0 && s.matches.empty() &&
            !s.cancelled && !s.timedOut) {
            std::ostringstream e;
            if (isFile) {
                e << "Nothing was searched: " << Basename(m_resolvedPath);
                if (s.skippedBinary > 0) {
                    e << " looks like a binary file (a NUL byte within the first "
                         "4 KiB), and grep only scans text. This is NOT a "
                         "\"no matches\" result -- the file was never read. To read "
                         "strings embedded in a binary, run it with a version or "
                         "help flag via powershell, or extract them with py.";
                } else if (s.skippedTooLarge > 0) {
                    e << " is larger than grep's "
                      << (GrepExecutor::kMaxFileBytes / (1024 * 1024))
                      << " MiB per-file limit, so it was never read. Narrow the "
                         "target or slice it with read_range.";
                } else {
                    e << " could not be opened (permissions or transient IO), so "
                         "it was never read.";
                }
            } else {
                e << "Nothing was searched under " << m_resolvedPath
                  << ": every candidate file was skipped (" << skipReasons
                  << "). This is NOT a \"no matches\" result -- no file was read.";
                if (s.prunedDirs > 0) {
                    e << " " << s.prunedDirs
                      << (s.prunedDirs == 1 ? " directory was" : " directories were")
                      << " also pruned (dot-directories and build/cache names such "
                         "as node_modules, bin, obj, Debug, Release).";
                }
            }
            skipError = e.str();
        }

        if (s.matches.empty() && !s.cancelled && !s.timedOut &&
            skipError.empty()) {
            // Distinguish "no matches" from "you got nothing because
            // we bailed out" — cancelled/timedOut cases fall through
            // to the chips layer without a body marker.
            body << "(no matches)\n";
            body << RegexAttemptHint(s.pattern);
        }

        // Partial searches still get the caveat inline: some files were
        // read and some were not, so a match count alone is misleading.
        if (skipError.empty() && skippedFiles > 0) {
            body << "\n[" << skippedFiles
                 << (skippedFiles == 1 ? " file was" : " files were")
                 << " skipped and NOT searched: " << skipReasons
                 << ". A match count says nothing about skipped content.]\n";
        }
        if (skipError.empty() && s.prunedDirs > 0 && s.matches.empty()) {
            body << "\n[" << s.prunedDirs
                 << (s.prunedDirs == 1 ? " directory was" : " directories were")
                 << " pruned from the walk (dot-directories and build/cache "
                    "names such as node_modules, bin, obj, Debug, Release). "
                    "Point grep at one directly if the target lives there.]\n";
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
        if (m_contextLines > 0) {
            result.chips.push_back(std::to_string(m_contextLines)
                                   + " context lines");
        }
        if (isDir) {
            // filesSearched, not filesScanned: the old chip counted
            // skipped files as scanned, which overstated coverage
            // exactly when coverage mattered most.
            std::ostringstream c;
            c << s.filesSearched
              << (s.filesSearched == 1 ? " file searched" : " files searched");
            result.chips.push_back(c.str());
        }
        if (skippedFiles > 0) {
            result.chips.push_back(std::to_string(skippedFiles) + " skipped");
        }
        if (anyTrunc)       result.chips.push_back("truncated");
        if (s.cancelled)    result.chips.push_back("cancelled");
        if (s.timedOut)     result.chips.push_back("timed out");
        result.chips.push_back(FormatElapsed(elapsed));

        // A search that read no files reports a failure, not an absence.
        // errorBody is never demoted to a variable handle and marks the
        // call failed for the repeat guard, both of which are correct
        // here: the model must not build on this as evidence.
        if (!skipError.empty()) {
            result.errorBody = skipError;
            result.body.clear();
            result.chips.clear();
            result.chips.push_back("failed");
            result.chips.push_back(std::to_string(skippedFiles) + " skipped");
            result.chips.push_back(FormatElapsed(elapsed));
        }

        // ── Post back to UI thread, if it's still alive ──────────
        auto* evt = new wxCommandEvent(wxEVT_GREP_COMPLETE);
        evt->SetClientObject(new GrepResultClientData(std::move(result)));
        LbQueueEventIfAlive(m_handler, m_alive, evt);

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
    size_t                             m_contextLines;
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
                         const ToolContext& ctx,
                         size_t contextLines)
{
    if (IsRunning())     return false;
    if (pattern.empty()) return false;
    if (contextLines > kMaxContextLines) return false;

    m_cancelFlag->store(false);
    m_isRunning->store(true);

    // wxThread::Create allocates the OS thread; Run schedules it.
    // Detached, so we don't own the pointer after Run returns.
    auto* worker = new GrepWorker(
        m_eventHandler, m_aliveToken, m_cancelFlag, m_isRunning,
        pattern, resolvedPath, commandEcho, ctx, contextLines);

    if (worker->Create() != wxTHREAD_NO_ERROR) {
        m_isRunning->store(false);
        delete worker;
        return false;
    }
    if (worker->Run() != wxTHREAD_NO_ERROR) {
        m_isRunning->store(false);
        // A detached wxThread only deletes itself after its entry function
        // runs; if Run() fails the thread never starts, so the object must
        // be deleted here or it leaks.
        delete worker;
        return false;
    }
    return true;
}

void GrepExecutor::Cancel()
{
    if (m_cancelFlag) m_cancelFlag->store(true);
}
