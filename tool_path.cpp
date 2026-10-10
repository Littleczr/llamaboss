// tool_path.cpp

#include "tool_path.h"

#include "lb_windows.h"

#ifdef _WIN32

namespace {

// UTF-8 → UTF-16 via MultiByteToWideChar.  Empty input → empty output.
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

// UTF-16 → UTF-8 via WideCharToMultiByte.  Empty input → empty output.
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


std::string TrimAscii(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string StripMatchingQuotes(std::string s)
{
    s = TrimAscii(s);
    if (s.size() >= 2 &&
        ((s.front() == '"'  && s.back() == '"') ||
         (s.front() == '\'' && s.back() == '\''))) {
        s = s.substr(1, s.size() - 2);
    }
    return s;
}

bool IsPathSeparator(wchar_t ch)
{
    return ch == L'\\' || ch == L'/';
}

bool IsDriveAbsolutePath(const std::wstring& p)
{
    return p.size() >= 3 &&
           iswalpha(p[0]) &&
           p[1] == L':' &&
           IsPathSeparator(p[2]);
}

bool IsDriveRelativePath(const std::wstring& p)
{
    return p.size() >= 2 &&
           iswalpha(p[0]) &&
           p[1] == L':' &&
           (p.size() == 2 || !IsPathSeparator(p[2]));
}

bool IsUncLikePath(const std::wstring& p)
{
    return p.size() >= 2 && IsPathSeparator(p[0]) && IsPathSeparator(p[1]);
}

bool IsRootRelativePath(const std::wstring& p)
{
    return p.size() >= 1 && IsPathSeparator(p[0]) && !IsUncLikePath(p);
}

// Accepts classic drive-letter absolutes ("C:\..."), UNC roots
// ("\\server\share"), and Win32 device paths ("\\?\C:\...").
// Root-relative paths ("\foo") are not absolute because they need a
// drive prefix from cwd before canonicalization.
bool IsAbsolutePath(const std::wstring& p)
{
    return IsDriveAbsolutePath(p) || IsUncLikePath(p);
}

std::wstring DrivePrefixFromAbsoluteCwd(const std::wstring& cwd)
{
    if (IsDriveAbsolutePath(cwd)) {
        return cwd.substr(0, 2); // "C:"
    }
    return L"";
}

} // anonymous namespace

std::string ResolveToolPath(const std::string& input,
                            const std::string& cwd)
{
    std::string normalized = StripMatchingQuotes(input);
    if (normalized.empty()) return "";

    std::wstring wInput = Utf8ToWide(normalized);
    std::wstring wCwd   = Utf8ToWide(cwd);
    if (wInput.empty()) return "";

    // ── Step 1: expand %VAR% environment variables.
    // ExpandEnvironmentStringsW returns required buffer size (including
    // terminating NUL) or 0 on error.  On success the call with the
    // real buffer writes the expanded string plus a NUL; we trim it.
    {
        DWORD needed = ::ExpandEnvironmentStringsW(
            wInput.c_str(), nullptr, 0);
        if (needed > 0) {
            std::wstring expanded((size_t)needed, L'\0');
            DWORD written = ::ExpandEnvironmentStringsW(
                wInput.c_str(), &expanded[0], needed);
            if (written > 0 && written <= needed) {
                expanded.resize(written - 1);   // drop NUL
                wInput = std::move(expanded);
            }
        }
    }

    if (wInput.empty()) return "";

    // ── Step 2: make relative/rooted shapes deterministic.
    //
    // Windows has two ambiguous path forms:
    //   * "\foo" / "/foo" is rooted on the current drive.
    //   * "C:foo" is relative to the process's hidden per-drive cwd.
    //
    // The first should resolve against the supplied cwd's drive, not the
    // LlamaBoss process cwd. The second is too surprising for tool paths, so
    // fail closed instead of letting GetFullPathNameW consult process state.
    if (IsDriveRelativePath(wInput)) {
        return "";
    }

    if (IsRootRelativePath(wInput)) {
        std::wstring drive = DrivePrefixFromAbsoluteCwd(wCwd);
        if (drive.empty()) return "";
        wInput = drive + wInput;
    }
    else if (!IsAbsolutePath(wInput) && !wCwd.empty()) {
        wchar_t last = wCwd.back();
        if (last != L'\\' && last != L'/')
            wCwd.push_back(L'\\');
        wInput = wCwd + wInput;
    }

    // ── Step 3: canonicalize (normalize `.` and `..`, unify separators
    // to `\`).  GetFullPathNameW with a zero-length buffer returns the
    // required size in chars including NUL.
    DWORD bufLen = ::GetFullPathNameW(wInput.c_str(), 0, nullptr, nullptr);
    if (bufLen == 0) return "";

    std::wstring out((size_t)bufLen, L'\0');
    DWORD written = ::GetFullPathNameW(wInput.c_str(),
                                       bufLen, &out[0], nullptr);
    if (written == 0 || written >= bufLen) return "";
    out.resize(written);

    return WideToUtf8(out);
}

bool IsDirectory(const std::string& absPath)
{
    std::wstring w = Utf8ToWide(absPath);
    if (w.empty()) return false;
    DWORD attrs = ::GetFileAttributesW(w.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool IsFile(const std::string& absPath)
{
    std::wstring w = Utf8ToWide(absPath);
    if (w.empty()) return false;
    DWORD attrs = ::GetFileAttributesW(w.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

#else  // ── macOS / POSIX ──────────────────────────────────────────────

#include <cstdlib>
#include <filesystem>
#include <sys/stat.h>

namespace {

std::string TrimAscii(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string StripMatchingQuotes(std::string s)
{
    s = TrimAscii(s);
    if (s.size() >= 2 &&
        ((s.front() == '"'  && s.back() == '"') ||
         (s.front() == '\'' && s.back() == '\''))) {
        s = s.substr(1, s.size() - 2);
    }
    return s;
}

bool IsVarChar(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}

// Expands a leading "~" plus $VAR, ${VAR} and %VAR% references. Unknown
// variables are left as written, as ExpandEnvironmentStringsW does.
std::string ExpandEnv(const std::string& in)
{
    std::string s = in;
    if (!s.empty() && s[0] == '~' && (s.size() == 1 || s[1] == '/')) {
        if (const char* home = std::getenv("HOME")) s = home + s.substr(1);
    }
    std::string out;
    for (size_t i = 0; i < s.size();) {
        const char c = s[i];
        if (c == '$' && i + 1 < s.size() && s[i + 1] == '{') {
            const size_t close = s.find('}', i + 2);
            if (close != std::string::npos) {
                const std::string name = s.substr(i + 2, close - i - 2);
                if (const char* v = std::getenv(name.c_str())) {
                    out += v; i = close + 1; continue;
                }
            }
        } else if (c == '$' && i + 1 < s.size() && IsVarChar(s[i + 1])) {
            size_t end = i + 1;
            while (end < s.size() && IsVarChar(s[end])) ++end;
            const std::string name = s.substr(i + 1, end - i - 1);
            if (const char* v = std::getenv(name.c_str())) {
                out += v; i = end; continue;
            }
        } else if (c == '%') {
            const size_t close = s.find('%', i + 1);
            if (close != std::string::npos && close > i + 1) {
                const std::string name = s.substr(i + 1, close - i - 1);
                if (const char* v = std::getenv(name.c_str())) {
                    out += v; i = close + 1; continue;
                }
            }
        }
        out += c;
        ++i;
    }
    return out;
}

} // anonymous namespace

std::string ResolveToolPath(const std::string& input,
                            const std::string& cwd)
{
    std::string normalized = ExpandEnv(StripMatchingQuotes(input));
    if (normalized.empty()) return "";

    namespace fs = std::filesystem;
    fs::path p(normalized);
    if (p.is_relative()) {
        if (cwd.empty()) return "";
        fs::path base(cwd);
        if (base.is_relative()) return "";
        p = base / p;
    }
    p = p.lexically_normal();
    std::string out = p.string();
    while (out.size() > 1 && out.back() == '/') out.pop_back();
    return out;
}

bool IsDirectory(const std::string& absPath)
{
    struct stat st {};
    return !absPath.empty() && ::stat(absPath.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool IsFile(const std::string& absPath)
{
    struct stat st {};
    return !absPath.empty() && ::stat(absPath.c_str(), &st) == 0 && !S_ISDIR(st.st_mode);
}

#endif
