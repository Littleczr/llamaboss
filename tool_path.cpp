// tool_path.cpp

#include "tool_path.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

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

// Accepts classic drive-letter absolutes ("C:\...") and UNC roots
// ("\\server\share").  Everything else is treated as relative so it
// gets resolved against the supplied cwd.
bool IsAbsolutePath(const std::wstring& p)
{
    if (p.size() >= 3 && iswalpha(p[0]) && p[1] == L':' &&
        (p[2] == L'\\' || p[2] == L'/'))
        return true;
    if (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\')
        return true;
    return false;
}

} // anonymous namespace

std::string ResolveToolPath(const std::string& input,
                            const std::string& cwd)
{
    std::string normalized = StripMatchingQuotes(input);
    if (normalized.empty()) return "";

    std::wstring wInput = Utf8ToWide(normalized);
    std::wstring wCwd   = Utf8ToWide(cwd);

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

    // ── Step 2: prepend cwd if the path is relative.
    if (!IsAbsolutePath(wInput) && !wCwd.empty()) {
        wchar_t last = wCwd.back();
        if (last != L'\\' && last != L'/')
            wCwd.push_back(L'\\');
        wInput = wCwd + wInput;
    }

    // ── Step 3: canonicalize (normalize `.` and `..`, unify separators
    // to `\`, resolve drive-relative paths).  GetFullPathNameW with a
    // zero-length buffer returns the required size in chars including NUL.
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
    DWORD attrs = ::GetFileAttributesW(w.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool IsFile(const std::string& absPath)
{
    std::wstring w = Utf8ToWide(absPath);
    DWORD attrs = ::GetFileAttributesW(w.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}
