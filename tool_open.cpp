// tool_open.cpp

#include "tool_open.h"

#include "tool_path.h"
#include "tool_read.h"
#include "chat_history.h"

#include <Poco/JSON/Object.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <set>
#include <sstream>
#include <regex>
#include <functional>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

namespace {

// ─── ASCII-only locale-free helpers ─────────────────────────────
// Reused pattern from path_safety.cpp -- avoids std::tolower's locale
// dependency and keeps behaviour deterministic across machines.
char LowerAscii(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string ToLowerAscii(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) out += LowerAscii(c);
    return out;
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


bool IsFuzzyWordChar(char c)
{
    unsigned char u = static_cast<unsigned char>(c);
    return (u >= 'a' && u <= 'z') ||
           (u >= 'A' && u <= 'Z') ||
           (u >= '0' && u <= '9');
}

std::string NormalizeForFuzzyText(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    bool lastWasSpace = true;

    for (char c : s) {
        if (IsFuzzyWordChar(c)) {
            out.push_back(LowerAscii(c));
            lastWasSpace = false;
        } else if (!lastWasSpace) {
            out.push_back(' ');
            lastWasSpace = true;
        }
    }

    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

bool IsFuzzyFillerToken(const std::string& t)
{
    static const std::set<std::string> kFillers = {
        "a", "an", "and", "app", "application", "audio", "by",
        "default", "directory", "file", "folder", "for", "from",
        "in", "into", "launch", "local", "me", "media", "mp3s",
        "music", "my", "of", "on", "open", "play", "playing",
        "please", "powershell", "pull", "run", "shell", "show",
        "song", "songs", "start", "the", "this", "to", "track",
        "tracks", "up", "use", "using", "view", "with"
    };
    return kFillers.count(t) != 0;
}

std::vector<std::string> TokenizeFuzzyQuery(const std::string& query)
{
    std::vector<std::string> raw;
    std::istringstream iss(NormalizeForFuzzyText(query));
    std::string t;
    while (iss >> t) {
        if (!t.empty()) raw.push_back(t);
    }

    if (raw.empty()) return {};

    std::vector<std::string> filtered;
    for (const auto& tok : raw) {
        if (!IsFuzzyFillerToken(tok)) filtered.push_back(tok);
    }

    // If the user typed only action words ("play music"), keep the raw
    // tokens so the caller can still return a normal not-found/ambiguous
    // result rather than silently matching everything.
    return filtered.empty() ? raw : filtered;
}

std::string ParentPartRaw(const std::string& path)
{
    std::string s = StripMatchingQuotes(path);
    while (!s.empty() && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    size_t pos = s.find_last_of("/\\");
    if (pos == std::string::npos) return {};

    // Preserve drive roots.  For "D:\\Movie.mkv", the last slash is
    // index 2; returning only "D:" produces Windows drive-relative semantics
    // instead of the absolute root.
    if (pos == 2 && s.size() >= 3 &&
        ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z')) &&
        s[1] == ':') {
        return s.substr(0, 3);
    }

    return s.substr(0, pos);
}

std::string LeafPartRaw(const std::string& path)
{
    std::string s = StripMatchingQuotes(path);
    while (!s.empty() && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    size_t pos = s.find_last_of("/\\");
    return (pos == std::string::npos) ? s : s.substr(pos + 1);
}

bool IsDriveRootPath(const std::string& path)
{
    std::string s = StripMatchingQuotes(path);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                          s.back() == '\r' || s.back() == '\n')) {
        s.pop_back();
    }
    return s.size() == 3 &&
           ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z')) &&
           s[1] == ':' &&
           (s[2] == '\\' || s[2] == '/');
}

std::string BuildLooseWildcardFromQuery(const std::string& query)
{
    std::vector<std::string> tokens = TokenizeFuzzyQuery(query);
    if (tokens.empty()) return "*";

    std::string out = "*";
    for (const auto& t : tokens) {
        if (t.empty()) continue;
        out += t;
        out += "*";
    }
    return out == "*" ? "*" : out;
}

bool HasWildcardChar(const std::string& s)
{
    return s.find('*') != std::string::npos || s.find('?') != std::string::npos;
}

// Models sometimes copy an entire PowerShell Get-ChildItem table row into
// /open, for example:
//
//   D:\music\7/31/2011 11:09 AM 6277824 Big Sean Ft ... .mp3
//
// That is not a real path; the date/size columns became part of the
// filename.  Normalize that back to the path or basename the user meant.
std::string StripCopiedPowerShellListingColumns(const std::string& input)
{
    std::string s = StripMatchingQuotes(input);
    s = TrimAscii(s);
    if (s.empty()) return s;

    std::smatch m;

    // Path + copied row metadata.  Keep the path prefix, drop the metadata,
    // and keep only the Name column.
    static const std::regex kPathRow(
        R"(^\s*([A-Za-z]:[\\/].*?[\\/])\s*(?:[A-Za-z\-]{5,}\s+)?\d{1,2}/\d{1,2}/\d{2,4}\s+\d{1,2}:\d{2}(?::\d{2})?\s*(?:AM|PM|am|pm)?\s+\d+\s+(.+?)\s*$)"
    );
    if (std::regex_match(s, m, kPathRow) && m.size() >= 3) {
        return TrimAscii(m[1].str() + m[2].str());
    }

    // Bare copied row metadata with no path prefix.  Return only the Name
    // column so recent-listing fuzzy matching can do its job.
    static const std::regex kBareRow(
        R"(^\s*(?:[A-Za-z\-]{5,}\s+)?\d{1,2}/\d{1,2}/\d{2,4}\s+\d{1,2}:\d{2}(?::\d{2})?\s*(?:AM|PM|am|pm)?\s+\d+\s+(.+?)\s*$)"
    );
    if (std::regex_match(s, m, kBareRow) && m.size() >= 2) {
        return TrimAscii(m[1].str());
    }

    return s;
}

// Same Utf8/Wide helpers as elsewhere in the tool layer.  Local copy
// rather than reaching across translation units; the eventual
// tool_util consolidation pass picks up these duplicates.
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
    std::string s((size_t)len, char(0));
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                          &s[0], len, nullptr, nullptr);
    return s;
}

// Returns the lowercase extension (no leading dot) or empty if none.
// "foo.tar.gz" -> "gz".  Splits at the LAST dot only -- consistent
// with Windows filesystem semantics (the extension that determines
// the file association is the last one).
std::string LowerExt(const std::string& path)
{
    // Strip directory part so dots in the path don't confuse us.
    size_t slash = path.find_last_of("/\\");
    std::string base = (slash == std::string::npos)
                       ? path : path.substr(slash + 1);

    size_t dot = base.rfind('.');
    if (dot == std::string::npos || dot == 0) return "";
    return ToLowerAscii(base.substr(dot + 1));
}

std::string BasenameLower(const std::string& path)
{
    size_t slash = path.find_last_of("/\\");
    std::string base = (slash == std::string::npos)
                       ? path : path.substr(slash + 1);
    return ToLowerAscii(base);
}

// Returns the basename without extension (and without directory).
// Used as the canonical match target for fuzzy matching: the user
// types "the eagles", not "the eagles - hotel california.mp3", so
// the extension contributes noise without information.
std::string BasenameStemLower(const std::string& path)
{
    size_t slash = path.find_last_of("/\\");
    std::string base = (slash == std::string::npos)
                       ? path : path.substr(slash + 1);
    size_t dot = base.rfind('.');
    if (dot != std::string::npos && dot > 0) base = base.substr(0, dot);
    return ToLowerAscii(base);
}

// ─── Risky / text-like extension lists ───────────────────────────
// The risky list is the security floor: anything here is blocked from
// ShellExecute even if the user explicitly asked for it.  Phase 2b
// will add a click-confirm affordance to override; for now, blocked
// means blocked.
//
// Keep this synced with the user's threat model.  When in doubt,
// add it -- false positives just mean the user opens it manually
// in File Explorer; false negatives mean the agent runs code on
// the user's behalf without consent.
const std::set<std::string>& RiskyExtensions()
{
    static const std::set<std::string> kSet = {
        // Native executables and their cousins
        "exe", "com", "scr", "cpl", "msi", "msp", "msu",
        // Batch / Command
        "bat", "cmd",
        // PowerShell
        "ps1", "psm1", "psd1",
        // VBScript / JScript / WSH
        "vbs", "vbe", "js", "jse",
        "wsf", "wsh", "hta",
        // Registry edit
        "reg",
        // Shortcuts (can run arbitrary commands)
        "lnk", "pif",
        // ClickOnce
        "application", "appref-ms",
        // Sidebar gadget (legacy, but still recognized)
        "gadget",
        // Java
        "jar",
        // URL shortcut (can fetch remote content)
        "url",
        // Macro Office documents
        "docm", "dotm", "xlsm", "xltm", "pptm", "potm",
    };
    return kSet;
}

// Text-like extensions -- the open-as-read path.  Same family as
// AttachmentManager::IsTextFile but kept independent so changes to
// the attachment classifier don't unexpectedly retarget the open
// tool.  The tool_read implementation does its own binary detection
// (null-byte scan), so even if this list misclassifies a binary,
// ReadFile produces a readable hex preview instead of trash.
const std::set<std::string>& TextLikeExtensions()
{
    static const std::set<std::string> kSet = {
        "txt", "md", "log", "json", "xml", "yaml", "yml", "toml",
        "csv", "tsv", "ini", "cfg", "env",
        // Code
        "c", "h", "cpp", "hpp", "cc", "cxx", "hh",
        "py", "rs", "go", "java", "kt", "swift",
        "rb", "php", "sql", "pl", "lua",
        "ts", "tsx", "js", "jsx",
        // Wait — js / jsx are also in the risky list above.  Risky
        // wins (see ClassifyForOpen below) so this duplication is
        // benign; .js as a script gets blocked, but the ext is
        // listed here too in case the policy ever changes.
        "html", "htm", "css", "scss", "sass", "less",
        "sh", "ps1",  // ps1 also risky; same rationale as js
        "rs", "go",
        "diff", "patch",
        "dockerfile", "gitignore", "gitattributes",
        "rc", "vcxproj", "filters", "slnx", "sln",
    };
    return kSet;
}

// Text-like filenames that do not have a normal extension.
// LowerExt("Dockerfile") and LowerExt(".gitignore") both return empty,
// so extension-only classification would incorrectly send them to
// ShellExecuteW. Keep these as exact lowercase basename matches.
const std::set<std::string>& TextLikeFilenames()
{
    static const std::set<std::string> kSet = {
        "dockerfile",
        "makefile",
        ".env",
        ".gitignore",
        ".gitattributes",
        ".editorconfig",
        ".clang-format",
        ".clang-tidy",
    };
    return kSet;
}

// ─── Language hint by extension ──────────────────────────────────
// Used to populate OpenResult::bodyLang so the rendered tool block
// gets syntax highlighting for the inlined content, matching how
// /read presents code files.
std::string LangForExtImpl(const std::string& ext)
{
    static const std::set<std::string> kCpp     = { "c","h","cpp","hpp","cc","cxx","hh" };
    static const std::set<std::string> kPy      = { "py" };
    static const std::set<std::string> kJs      = { "js","jsx","ts","tsx" };
    static const std::set<std::string> kJson    = { "json" };
    static const std::set<std::string> kYaml    = { "yaml","yml" };
    static const std::set<std::string> kXml     = { "xml","html","htm","rc","vcxproj","filters","slnx","sln" };
    static const std::set<std::string> kMd      = { "md" };
    static const std::set<std::string> kSh      = { "sh" };
    static const std::set<std::string> kPs      = { "ps1","psm1","psd1" };
    static const std::set<std::string> kCss     = { "css","scss","sass","less" };
    static const std::set<std::string> kGo      = { "go" };
    static const std::set<std::string> kRust    = { "rs" };
    static const std::set<std::string> kJava    = { "java","kt" };
    static const std::set<std::string> kSql     = { "sql" };
    static const std::set<std::string> kRuby    = { "rb" };
    static const std::set<std::string> kPhp     = { "php" };
    static const std::set<std::string> kIni     = { "ini","cfg","toml","env" };
    static const std::set<std::string> kCsv     = { "csv","tsv" };
    static const std::set<std::string> kDiff    = { "diff","patch" };

    if (kCpp.count(ext))  return "cpp";
    if (kPy.count(ext))   return "python";
    if (kJs.count(ext))   return "javascript";
    if (kJson.count(ext)) return "json";
    if (kYaml.count(ext)) return "yaml";
    if (kXml.count(ext))  return "xml";
    if (kMd.count(ext))   return "markdown";
    if (kSh.count(ext))   return "bash";
    if (kPs.count(ext))   return "powershell";
    if (kCss.count(ext))  return "css";
    if (kGo.count(ext))   return "go";
    if (kRust.count(ext)) return "rust";
    if (kJava.count(ext)) return "java";
    if (kSql.count(ext))  return "sql";
    if (kRuby.count(ext)) return "ruby";
    if (kPhp.count(ext))  return "php";
    if (kIni.count(ext))  return "ini";
    if (kCsv.count(ext))  return "csv";
    if (kDiff.count(ext)) return "diff";
    return "";  // plain text — no fence language
}

// ─── ShellExecute wrapper ─────────────────────────────────────────
// Returns a human-readable error string on failure, empty on success.
// Uses the W variant with explicit UTF-8 conversion; same rationale
// as the server_manager fix from earlier work.
std::string ShellOpenFile(const std::string& absPath)
{
    std::wstring wPath = Utf8ToWide(absPath);

    // ShellExecuteW returns an HINSTANCE that's actually an integer
    // status >32 == success, <=32 == failure, with documented codes.
    HINSTANCE rc = ::ShellExecuteW(
        nullptr,        // hwnd — null is fine, no parent UI
        nullptr,        // lpOperation — null = use default verb
                        // (typically "open"; for media types, "play")
        wPath.c_str(),  // lpFile
        nullptr,        // lpParameters
        nullptr,        // lpDirectory — let the shell pick
        SW_SHOWNORMAL);

    INT_PTR code = reinterpret_cast<INT_PTR>(rc);
    if (code > 32) return "";  // success

    // Translate the documented failure codes to user-facing text.
    switch (code) {
        case 0:                       return "Out of memory.";
        case ERROR_FILE_NOT_FOUND:    return "File not found at launch time.";
        case ERROR_PATH_NOT_FOUND:    return "Path not found at launch time.";
        case ERROR_BAD_FORMAT:        return "Executable image is invalid.";
        case SE_ERR_ACCESSDENIED:     return "Access denied.";
        case SE_ERR_ASSOCINCOMPLETE:  return "File association is incomplete or invalid.";
        case SE_ERR_DDEBUSY:          return "DDE transaction busy; the application is busy.";
        case SE_ERR_DDEFAIL:          return "DDE transaction failed.";
        case SE_ERR_DDETIMEOUT:       return "DDE transaction timed out.";
        case SE_ERR_DLLNOTFOUND:      return "Required DLL not found.";
        case SE_ERR_NOASSOC:          return "No application is associated with this file type. "
                                             "Open it manually from File Explorer or set a default app.";
        case SE_ERR_OOM:              return "Out of memory.";
        case SE_ERR_SHARE:            return "Sharing violation.";
        default:                      return "Launch failed (Windows error " +
                                              std::to_string((int)code) + ").";
    }
}

// ─── Listing-from-history parser ─────────────────────────────────
// FormatToolBlockAsUserMessage produces:
//
//     [tool: ls]
//     > /ls D:\Music
//     <blank>
//     ```
//     <body lines>
//     ```
//     <blank>
//     [status: ...]
//
// We need (a) the directory from the echo line, (b) the entry names
// from the body lines.  The body is fixed-width-aligned; the entry
// name occupies the first column and ends at the first run of two or
// more spaces (the column separator is "  ").  Directories have a
// trailing "/" we strip.
//
// Rejects truncation markers ("[... truncated ...]") and the
// "(empty directory)" sentinel so we don't try to fuzzy-match
// against them.

std::vector<std::string> TokenizePowerShellLike(const std::string& command)
{
    std::vector<std::string> tokens;
    std::string cur;
    bool inSingle = false;
    bool inDouble = false;

    for (char c : command) {
        if (c == static_cast<char>(39) && !inDouble) {
            inSingle = !inSingle;
            continue;
        }
        if (c == '"' && !inSingle) {
            inDouble = !inDouble;
            continue;
        }
        if (!inSingle && !inDouble && (c == ' ' || c == '\t' || c == '\r' || c == '\n')) {
            if (!cur.empty()) {
                tokens.push_back(cur);
                cur.clear();
            }
            continue;
        }
        cur += c;
    }
    if (!cur.empty()) tokens.push_back(cur);
    return tokens;
}

std::string FirstPipelineStage(const std::string& command)
{
    bool inSingle = false;
    bool inDouble = false;
    for (size_t i = 0; i < command.size(); ++i) {
        char c = command[i];
        if (c == static_cast<char>(39) && !inDouble) inSingle = !inSingle;
        else if (c == '"' && !inSingle) inDouble = !inDouble;
        else if (c == '|' && !inSingle && !inDouble) {
            return TrimAscii(command.substr(0, i));
        }
    }
    return TrimAscii(command);
}

struct PowerShellGciSpec {
    std::string dirRaw;
    std::string filterRaw;
    bool        recurse = false;
};

bool IsParameterThatConsumesValue(const std::string& lt)
{
    static const std::set<std::string> kConsumes = {
        "-path", "-literalpath", "-filter", "-include", "-exclude",
        "-erroraction", "-ea", "-warningaction", "-wa",
        "-informationaction", "-ia", "-depth"
    };
    return kConsumes.count(lt) != 0;
}

bool ExtractGetChildItemSpec(const std::string& command,
                             PowerShellGciSpec& outSpec)
{
    std::string first = FirstPipelineStage(command);
    if (first.empty()) return false;

    std::vector<std::string> tokens = TokenizePowerShellLike(first);
    if (tokens.empty()) return false;

    std::string verb = ToLowerAscii(tokens[0]);
    if (verb != "get-childitem" && verb != "gci" && verb != "dir")
        return false;

    outSpec = PowerShellGciSpec{}; // bare Get-ChildItem means current tool cwd
    bool pathSeen = false;

    for (size_t i = 1; i < tokens.size(); ++i) {
        std::string t  = tokens[i];
        std::string lt = ToLowerAscii(t);

        if ((lt == "-path" || lt == "-literalpath") && i + 1 < tokens.size()) {
            outSpec.dirRaw = tokens[++i];
            pathSeen = true;
            continue;
        }

        if (lt == "-filter" && i + 1 < tokens.size()) {
            outSpec.filterRaw = tokens[++i];
            continue;
        }

        if (lt == "-recurse") {
            outSpec.recurse = true;
            continue;
        }

        const std::string pathEq = "-path:";
        const std::string litEq  = "-literalpath:";
        const std::string filtEq = "-filter:";
        if (lt.rfind(pathEq, 0) == 0) {
            outSpec.dirRaw = t.substr(pathEq.size());
            pathSeen = true;
            continue;
        }
        if (lt.rfind(litEq, 0) == 0) {
            outSpec.dirRaw = t.substr(litEq.size());
            pathSeen = true;
            continue;
        }
        if (lt.rfind(filtEq, 0) == 0) {
            outSpec.filterRaw = t.substr(filtEq.size());
            continue;
        }

        if (!t.empty() && t[0] == '-') {
            if (IsParameterThatConsumesValue(lt) && i + 1 < tokens.size())
                ++i;
            continue;
        }

        // First non-switch token after Get-ChildItem is the positional path.
        // Avoid misreading values from later options as paths after a path has
        // already been found.
        if (!pathSeen && !t.empty()) {
            outSpec.dirRaw = t;
            pathSeen = true;
        }
    }

    return true;
}

bool ExtractGetChildItemPathArg(const std::string& command,
                                std::string&       outDirRaw)
{
    PowerShellGciSpec spec;
    if (!ExtractGetChildItemSpec(command, spec)) return false;
    outDirRaw = spec.dirRaw;
    return true;
}

bool ExtractPowerShellGetChildItemSpec(const std::string& content,
                                       PowerShellGciSpec& outSpec)
{
    if (content.compare(0, 19, "[tool: powershell]\n") != 0)
        return false;

    size_t firstNl = content.find('\n');
    if (firstNl == std::string::npos) return false;

    size_t echoStart = firstNl + 1;
    size_t echoEnd   = content.find('\n', echoStart);
    if (echoEnd == std::string::npos) return false;

    std::string echo = content.substr(echoStart, echoEnd - echoStart);
    if (echo.size() < 2 || echo[0] != '>' || echo[1] != ' ')
        return false;

    std::string command = TrimAscii(echo.substr(2));
    return ExtractGetChildItemSpec(command, outSpec);
}

bool ExtractPowerShellGetChildItemDir(const std::string& content,
                                      std::string&       outDirRaw)
{
    PowerShellGciSpec spec;
    if (!ExtractPowerShellGetChildItemSpec(content, spec)) return false;
    outDirRaw = spec.dirRaw;
    return true;
}

bool EnumerateDirectoryEntryNames(const std::string& directory,
                                  std::vector<std::string>& outEntries)
{
    if (directory.empty() || !IsDirectory(directory)) return false;

    std::wstring pattern = Utf8ToWide(directory);
    if (pattern.empty()) return false;
    wchar_t last = pattern.back();
    if (last != static_cast<wchar_t>(92) && last != L'/')
        pattern += static_cast<wchar_t>(92);
    pattern += L'*';

    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;

    do {
        std::wstring nameW(fd.cFileName);
        if (nameW == L"." || nameW == L"..") continue;
        std::string name = WideToUtf8(nameW);
        if (!name.empty()) outEntries.push_back(name);
    } while (::FindNextFileW(h, &fd));

    ::FindClose(h);
    return true;
}

std::wstring JoinWidePath(const std::wstring& dir, const std::wstring& leaf)
{
    if (dir.empty()) return leaf;
    std::wstring out = dir;
    wchar_t last = out.back();
    if (last != static_cast<wchar_t>(92) && last != L'/')
        out += static_cast<wchar_t>(92);
    out += leaf;
    return out;
}

bool EnumerateDirectoryEntryNamesRecursiveFiltered(
    const std::string&        rootDirectory,
    const std::string&        filterRaw,
    std::vector<std::string>& outEntries)
{
    outEntries.clear();
    if (rootDirectory.empty() || !IsDirectory(rootDirectory)) return false;

    std::string filter = StripMatchingQuotes(filterRaw);
    filter = TrimAscii(filter);
    if (filter.empty()) return false;

    // PowerShell -Filter is a leaf-name wildcard.  Do not allow an
    // accidental path pattern to turn this helper into an arbitrary glob.
    if (filter.find('/') != std::string::npos || filter.find('\\') != std::string::npos)
        return false;

    std::wstring rootW = Utf8ToWide(rootDirectory);
    std::wstring filterW = Utf8ToWide(filter);
    if (rootW.empty() || filterW.empty()) return false;

    // Safety/perf caps: this only supports the friendly media/document
    // discovery path, not unbounded filesystem indexing.
    constexpr size_t kMaxDirs    = 20000;
    constexpr size_t kMaxMatches = 500;
    constexpr size_t kMaxDepth   = 64;

    size_t dirsVisited = 0;

    std::function<void(const std::wstring&, const std::string&, size_t)> walk;
    walk = [&](const std::wstring& dirW, const std::string& relPrefix, size_t depth) {
        if (dirsVisited++ >= kMaxDirs || outEntries.size() >= kMaxMatches || depth > kMaxDepth)
            return;

        // Match entries in this directory using the same wildcard style as
        // PowerShell -Filter / Win32 FindFirstFile.
        WIN32_FIND_DATAW fd{};
        std::wstring matchPattern = JoinWidePath(dirW, filterW);
        HANDLE mh = ::FindFirstFileW(matchPattern.c_str(), &fd);
        if (mh != INVALID_HANDLE_VALUE) {
            do {
                std::wstring nameW(fd.cFileName);
                if (nameW == L"." || nameW == L"..") continue;
                std::string name = WideToUtf8(nameW);
                if (!name.empty()) {
                    outEntries.push_back(relPrefix + name);
                    if (outEntries.size() >= kMaxMatches) break;
                }
            } while (::FindNextFileW(mh, &fd));
            ::FindClose(mh);
        }

        if (outEntries.size() >= kMaxMatches) return;

        // Recurse into normal directories.  Skip reparse points so junctions
        // and symlinks cannot create cycles or wander outside the tree.
        WIN32_FIND_DATAW dd{};
        std::wstring childPattern = JoinWidePath(dirW, L"*");
        HANDLE dh = ::FindFirstFileW(childPattern.c_str(), &dd);
        if (dh == INVALID_HANDLE_VALUE) return;

        do {
            std::wstring nameW(dd.cFileName);
            if (nameW == L"." || nameW == L"..") continue;
            if ((dd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
            if ((dd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) continue;

            std::string name = WideToUtf8(nameW);
            if (name.empty()) continue;

            std::wstring childDir = JoinWidePath(dirW, nameW);
            walk(childDir, relPrefix + name + "\\", depth + 1);
            if (dirsVisited >= kMaxDirs || outEntries.size() >= kMaxMatches) break;
        } while (::FindNextFileW(dh, &dd));

        ::FindClose(dh);
    };

    walk(rootW, "", 0);
    return true;
}

std::vector<std::string> RecursiveFiltersForPowerShellFilter(const std::string& filterRaw)
{
    std::string filter = TrimAscii(StripMatchingQuotes(filterRaw));
    if (filter.empty()) return {};

    std::vector<std::string> filters;
    filters.push_back(filter);

    // Common model mistake: -Filter "Hotel California*" misses
    // "The Eagles - Hotel California.mp3".  When a recursive search is
    // being used as discovery context for /open, add the friendly contains
    // form too.
    if (!filter.empty() && filter.front() != '*') {
        std::string contains = "*" + filter;
        if (std::find(filters.begin(), filters.end(), contains) == filters.end())
            filters.push_back(contains);
    }

    if (!HasWildcardChar(filter)) {
        std::string containsBoth = "*" + filter + "*";
        if (std::find(filters.begin(), filters.end(), containsBoth) == filters.end())
            filters.push_back(containsBoth);
    }

    return filters;
}

bool ExtractLsDirAndEntries(const std::string& content,
                            std::string&       outDirRaw,
                            std::vector<std::string>& outEntries)
{
    // Quick header check.
    if (content.compare(0, 7, "[tool: ") != 0) return false;
    size_t firstNl = content.find('\n');
    if (firstNl == std::string::npos) return false;
    std::string header = content.substr(0, firstNl);
    if (header != "[tool: ls]") return false;

    // Echo line: "> /ls <dir>" or "> /ls".
    size_t echoStart = firstNl + 1;
    size_t echoEnd   = content.find('\n', echoStart);
    if (echoEnd == std::string::npos) return false;
    std::string echo = content.substr(echoStart, echoEnd - echoStart);

    // Strip leading "> " then "/ls" prefix; rest is the directory arg.
    if (echo.size() < 2 || echo[0] != '>' || echo[1] != ' ') return false;
    std::string args = echo.substr(2);
    if (args.compare(0, 3, "/ls") != 0) return false;
    if (args.size() == 3) {
        outDirRaw.clear();   // empty arg means "list cwd"
    } else if (args[3] == ' ' || args[3] == '\t') {
        // Trim leading whitespace after "/ls"
        size_t a = args.find_first_not_of(" \t", 3);
        outDirRaw = (a == std::string::npos) ? "" : args.substr(a);
        // Right-trim
        size_t b = outDirRaw.find_last_not_of(" \t\r");
        if (b != std::string::npos) outDirRaw = outDirRaw.substr(0, b + 1);
        else                        outDirRaw.clear();
    } else {
        return false;  // looked like a different command
    }

    // Find the body fence.  FormatToolBlockAsUserMessage uses
    // dynamic backtick lengths; for /ls bodyLang is empty so the
    // fence is a bare "```" or longer.  Scan for the opening fence
    // (a line that's all backticks).
    size_t pos = echoEnd + 1;
    auto isFenceLine = [](const std::string& s) -> size_t {
        if (s.size() < 3) return 0;
        for (char c : s) if (c != '`') return 0;
        return s.size();
    };

    size_t fenceLen = 0;
    while (pos < content.size()) {
        size_t lineEnd = content.find('\n', pos);
        std::string line = content.substr(pos, lineEnd == std::string::npos
                                              ? std::string::npos
                                              : lineEnd - pos);
        // Right-trim CR for Windows line endings.
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if ((fenceLen = isFenceLine(line)) > 0) {
            pos = (lineEnd == std::string::npos) ? content.size() : lineEnd + 1;
            break;
        }
        if (lineEnd == std::string::npos) return false;
        pos = lineEnd + 1;
    }
    if (fenceLen == 0) return false;

    // Read body lines until matching closing fence.
    while (pos < content.size()) {
        size_t lineEnd = content.find('\n', pos);
        std::string line = content.substr(pos, lineEnd == std::string::npos
                                              ? std::string::npos
                                              : lineEnd - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();

        size_t thisFence = isFenceLine(line);
        if (thisFence == fenceLen) break;  // closing fence

        // Skip empty lines and the truncation / empty-dir markers.
        if (line.empty() ||
            line.compare(0, 4, "[...") == 0 ||
            line == "(empty directory)")
        {
            // continue
        } else {
            // Entry name is the first column up to the first run of
            // 2+ spaces (the column separator we use in tool_ls).
            size_t sep = line.find("  ");
            std::string name = (sep == std::string::npos)
                               ? line : line.substr(0, sep);
            // Right-trim trailing whitespace
            size_t b = name.find_last_not_of(" \t");
            if (b != std::string::npos) name = name.substr(0, b + 1);
            // Strip trailing "/" that marks dirs in the listing.
            if (!name.empty() && name.back() == '/') name.pop_back();
            if (!name.empty()) outEntries.push_back(name);
        }

        if (lineEnd == std::string::npos) break;
        pos = lineEnd + 1;
    }

    return true;
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════
//  Public API
// ═══════════════════════════════════════════════════════════════════

FileRisk ClassifyForOpen(const std::string& path)
{
    std::string ext = LowerExt(path);

    // Risky ALWAYS wins -- a file with an extension on the kill list
    // is risky even if it also appears in TextLike (e.g. .js).
    if (!ext.empty() && RiskyExtensions().count(ext)) return FileRisk::Risky;

    // Normal text/code extensions.
    if (!ext.empty() && TextLikeExtensions().count(ext)) return FileRisk::TextLike;

    // Special no-extension / dotfile text names: Dockerfile, .env,
    // .gitignore, etc. These should open inline instead of being
    // handed to the Windows shell.
    if (TextLikeFilenames().count(BasenameLower(path))) return FileRisk::TextLike;

    // Unknown or no extension -- ShellExecuteW will fail with NO_ASSOC
    // for unknown types, surfacing a clear error instead of executing
    // anything by itself.
    return FileRisk::Safe;
}

std::string LanguageForExt(const std::string& path)
{
    return LangForExtImpl(LowerExt(path));
}

std::vector<size_t> FuzzyMatchBasenames(
    const std::string&              query,
    const std::vector<std::string>& candidates)
{
    if (query.empty() || candidates.empty()) return {};

    // Tokenize the user's request as fuzzy words, not literal shell words.
    // This intentionally ignores common action/filler words so requests like
    // "play hotel california", "run the hotel california song", and
    // "use powershell to play hotel california" can still resolve to the
    // visible basename "The Eagles - Hotel California.mp3".
    std::vector<std::string> tokens = TokenizeFuzzyQuery(query);
    if (tokens.empty()) return {};

    // Score: a candidate matches iff EVERY useful query token is a substring
    // of its normalized lowercase basename. Length is the tiebreaker
    // (shortest wins). The extension is kept so exact filename queries still
    // work after the model copies a visible listing line.
    struct M { size_t idx; size_t stemLen; };
    std::vector<M> matched;

    for (size_t i = 0; i < candidates.size(); ++i) {
        std::string stem = NormalizeForFuzzyText(BasenameLower(candidates[i]));
        bool allHit = true;
        for (const auto& t : tokens) {
            if (stem.find(t) == std::string::npos) { allHit = false; break; }
        }
        if (allHit) matched.push_back({ i, stem.size() });
    }

    if (matched.empty()) return {};

    std::sort(matched.begin(), matched.end(),
              [](const M& a, const M& b) {
                  if (a.stemLen != b.stemLen) return a.stemLen < b.stemLen;
                  return a.idx < b.idx;
              });

    // Cap at 5 to keep an ambiguity message readable.
    constexpr size_t kCap = 5;
    std::vector<size_t> out;
    for (size_t i = 0; i < matched.size() && i < kCap; ++i)
        out.push_back(matched[i].idx);
    return out;
}

// ─── Recent file-listing collector (/ls + safe Get-ChildItem) ─────
std::vector<LsListing> CollectRecentLsListings(
    const ChatHistory& history,
    const std::string& fallbackCwd,
    size_t             maxListings)
{
    std::vector<LsListing> out;
    if (maxListings == 0) return out;

    const auto& msgs = history.GetMessages();
    // Walk backward.  Skip non-user messages; stop after we've
    // collected `maxListings`.
    for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) {
        const auto& msg = *it;
        std::string role    = msg->getValue<std::string>("role");
        if (role != "user") continue;

        std::string content = msg->getValue<std::string>("content");
        if (content.empty()) continue;

        std::string dirRaw;
        std::vector<std::string> entries;
        bool fromPowerShell = false;
        PowerShellGciSpec psSpec;

        if (content.compare(0, 11, "[tool: ls]\n") == 0) {
            if (!ExtractLsDirAndEntries(content, dirRaw, entries)) continue;
        }
        else if (content.compare(0, 19, "[tool: powershell]\n") == 0) {
            // Natural-language browsing may use the PowerShell tool for:
            //   Get-ChildItem -Path 'D:\\Music'
            // Treat that as a list/search context too, so a later open call
            // can fuzzy-match against what the user just saw.  For ordinary
            // Get-ChildItem we enumerate that directory.  For recursive
            // filtered searches ("D: somewhere"), we keep relative paths to
            // matches under the searched root so /open can launch the actual
            // nested file even if the model selected only Name instead of
            // FullName.
            if (!ExtractPowerShellGetChildItemSpec(content, psSpec)) continue;
            dirRaw = psSpec.dirRaw;
            fromPowerShell = true;
        }
        else {
            continue;
        }

        // Resolve the directory. Empty arg means "list cwd"; in that
        // case fall back to fallbackCwd directly.
        std::string resolvedDir;
        if (dirRaw.empty()) {
            resolvedDir = fallbackCwd;
        } else {
            resolvedDir = ResolveToolPath(dirRaw, fallbackCwd);
        }
        if (resolvedDir.empty() || !IsDirectory(resolvedDir)) continue;

        // /ls entries are parsed from the rendered listing. For recognized
        // PowerShell Get-ChildItem calls, populate entries now by scanning
        // the resolved directory. This keeps fuzzy /open working even when
        // the model used PowerShell instead of the native ls tool.
        if (entries.empty()) {
            if (fromPowerShell && psSpec.recurse && !psSpec.filterRaw.empty()) {
                bool any = false;
                for (const auto& filt : RecursiveFiltersForPowerShellFilter(psSpec.filterRaw)) {
                    std::vector<std::string> found;
                    if (EnumerateDirectoryEntryNamesRecursiveFiltered(resolvedDir, filt, found) &&
                        !found.empty()) {
                        entries.insert(entries.end(), found.begin(), found.end());
                        any = true;
                    }
                }

                // De-duplicate while preserving discovery order.
                if (any) {
                    std::set<std::string> seen;
                    std::vector<std::string> deduped;
                    for (const auto& e : entries) {
                        std::string key = ToLowerAscii(e);
                        if (seen.insert(key).second) deduped.push_back(e);
                    }
                    entries.swap(deduped);
                }

                if (!any) continue;
            } else {
                if (!EnumerateDirectoryEntryNames(resolvedDir, entries)) continue;
            }
        }

        LsListing l;
        l.directory = std::move(resolvedDir);
        l.entries   = std::move(entries);
        out.push_back(std::move(l));

        if (out.size() >= maxListings) break;
    }
    return out;
}

// ─── Main entry ──────────────────────────────────────────────────
OpenResult OpenFile(const std::string&                  inputPath,
                    const ToolContext&                  ctx,
                    const std::vector<LsListing>&       recentListings)
{
    OpenResult r;
    auto t0 = std::chrono::steady_clock::now();
    auto elapsedChip = [&]() -> std::string {
        auto t1 = std::chrono::steady_clock::now();
        double sec = std::chrono::duration<double>(t1 - t0).count();
        std::ostringstream ts;
        ts.setf(std::ios::fixed);
        ts.precision(sec < 10.0 ? 2 : 1);
        ts << sec << "s";
        return ts.str();
    };

    if (inputPath.empty()) {
        r.chips.push_back("failed");
        r.errorBody = "Usage: /open <path or fuzzy name>";
        r.chips.push_back(elapsedChip());
        return r;
    }

    const std::string openInput = StripCopiedPowerShellListingColumns(inputPath);

    // ── Resolution: direct path first ────────────────────────────
    std::string resolved;
    {
        std::string r0 = ResolveToolPath(openInput, ctx.cwd);
        if (!r0.empty() && (IsFile(r0) || IsDirectory(r0))) {
            resolved = std::move(r0);
        }
    }

    // ── Resolution: partial filename inside a real directory ──────
    // Direct path resolution cannot find "D:\music\Hotel California" when
    // the real filename is "The Eagles - Hotel California.mp3". If the parent
    // directory exists, enumerate it and fuzzy-match the leaf before falling
    // back to history. This makes open useful when the user gives a folder and
    // a human title, without requiring a separate list step.
    if (resolved.empty()) {
        std::string parentRaw = ParentPartRaw(openInput);
        std::string leafRaw   = LeafPartRaw(openInput);
        if (!parentRaw.empty() && !leafRaw.empty()) {
            std::string parentDir = ResolveToolPath(parentRaw, ctx.cwd);
            std::vector<std::string> entries;
            if (!parentDir.empty() && IsDirectory(parentDir)) {
                if (EnumerateDirectoryEntryNames(parentDir, entries)) {
                    std::vector<size_t> matches = FuzzyMatchBasenames(leafRaw, entries);

                    // Friendly fallback for "D:\\Terminator 2" / "D:\\some movie.mkv"
                    // when the user says the file is somewhere on the drive.  We keep
                    // this narrow: only drive roots, only after direct/root listing
                    // failed, and with bounded recursive search caps.
                    if (matches.empty() && IsDriveRootPath(parentDir)) {
                        std::vector<std::string> recursiveEntries;
                        std::string loose = BuildLooseWildcardFromQuery(leafRaw);
                        if (loose != "*" &&
                            EnumerateDirectoryEntryNamesRecursiveFiltered(parentDir, loose, recursiveEntries) &&
                            !recursiveEntries.empty()) {
                            entries.swap(recursiveEntries);
                            matches = FuzzyMatchBasenames(leafRaw, entries);
                        }
                    }

                    if (matches.size() == 1) {
                        std::string joined = parentDir;
                        if (!joined.empty() && joined.back() != '\\' && joined.back() != '/')
                            joined += '\\';
                        joined += entries[matches[0]];
                        resolved = ResolveToolPath(joined, ctx.cwd);
                        if (resolved.empty() || (!IsFile(resolved) && !IsDirectory(resolved))) {
                            r.chips.push_back("failed");
                            r.errorBody = "Resolved \"" + inputPath + "\" to \"" +
                                          joined + "\" but the item is no longer there.";
                            r.chips.push_back(elapsedChip());
                            return r;
                        }
                    } else if (matches.size() > 1) {
                        r.chips.push_back("ambiguous");
                        std::ostringstream ss;
                        ss << "Multiple files match \"" << inputPath << "\":\n\n";
                        for (size_t mi : matches) {
                            ss << "  " << parentDir << "\\" << entries[mi] << "\n";
                        }
                        ss << "\nAsk the user which one to open, or call /open with "
                              "a more specific name.";
                        r.errorBody = ss.str();
                        r.chips.push_back(elapsedChip());
                        return r;
                    }
                }
            }
        }
    }

    // ── Resolution: fuzzy match against recent file listings ─────
    if (resolved.empty() && !recentListings.empty()) {
        // Collect ALL candidates across listings, remembering which
        // listing each came from so we can rebuild the full path.
        std::vector<std::string> allBasenames;
        std::vector<size_t>      basenameToListing;
        for (size_t li = 0; li < recentListings.size(); ++li) {
            for (const auto& e : recentListings[li].entries) {
                allBasenames.push_back(e);
                basenameToListing.push_back(li);
            }
        }

        // Direct path resolution already strips surrounding quotes via
        // ResolveToolPath().  Do the same for the fuzzy query. Also strip
        // accidental PowerShell table metadata so a copied row like
        // "7/31/2011 11:09 AM 6277824 Big Sean ... .mp3" still resolves
        // through recent-listing fuzzy matching.
        std::string fuzzyQuery = StripCopiedPowerShellListingColumns(openInput);
        std::vector<size_t> matches = FuzzyMatchBasenames(fuzzyQuery, allBasenames);

        if (matches.empty()) {
            r.chips.push_back("not found");
            std::ostringstream ss;
            ss << "Could not resolve \"" << inputPath << "\".\n"
               << "Direct path resolution failed and no fuzzy match was "
               << "found in the most recent file listing.\n"
               << "List the folder first, then open a more specific name.";
            r.errorBody = ss.str();
            r.chips.push_back(elapsedChip());
            return r;
        }

        if (matches.size() > 1) {
            r.chips.push_back("ambiguous");
            std::ostringstream ss;
            ss << "Multiple files match \"" << inputPath << "\":\n\n";
            for (size_t mi : matches) {
                size_t li = basenameToListing[mi];
                ss << "  " << recentListings[li].directory
                   << "\\" << allBasenames[mi] << "\n";
            }
            ss << "\nAsk the user which one to open, or call /open with "
                  "a more specific name.";
            r.errorBody = ss.str();
            r.chips.push_back(elapsedChip());
            return r;
        }

        // Unique match.
        size_t mi = matches[0];
        size_t li = basenameToListing[mi];
        // Path join: directory + separator + basename.  ResolveToolPath
        // canonicalises so any double-separator is collapsed.
        std::string joined = recentListings[li].directory;
        if (!joined.empty() &&
            joined.back() != '\\' && joined.back() != '/')
            joined += '\\';
        joined += allBasenames[mi];

        resolved = ResolveToolPath(joined, ctx.cwd);
        if (resolved.empty() || (!IsFile(resolved) && !IsDirectory(resolved))) {
            r.chips.push_back("failed");
            r.errorBody = "Resolved \"" + inputPath + "\" to \"" +
                          joined + "\" but the item is no longer there.";
            r.chips.push_back(elapsedChip());
            return r;
        }
    }

    if (resolved.empty()) {
        r.chips.push_back("not found");
        r.errorBody = "Could not resolve \"" + inputPath + "\". "
                      "It is not a direct path, and no recent file "
                      "listing is available to fuzzy-match against. "
                      "List the folder first, then open a more specific name.";
        r.chips.push_back(elapsedChip());
        return r;
    }

    // Directories are safe to open in File Explorer/default shell and
    // should never be classified by extension-like dots in the folder name.
    if (IsDirectory(resolved)) {
        std::string shellErr = ShellOpenFile(resolved);
        if (!shellErr.empty()) {
            r.chips.push_back("failed");
            r.errorBody = "Could not launch \"" + resolved + "\": " + shellErr;
            r.chips.push_back(elapsedChip());
            return r;
        }

        r.chips.push_back("launched");
        r.body = "Opened " + resolved + " in the default application.\n";
        r.chips.push_back(elapsedChip());
        return r;
    }

    // ── Classify ─────────────────────────────────────────────────
    FileRisk risk = ClassifyForOpen(resolved);

    if (risk == FileRisk::Risky) {
        r.chips.push_back("blocked");
        std::ostringstream ss;
        ss << "Refused to open \"" << resolved << "\".\n"
           << "This file type can execute code (extension: ."
           << LowerExt(resolved) << "). Tools cannot launch executable "
           << "or scriptable files for safety reasons.\n"
           << "Ask the user to open it manually from File Explorer if "
           << "they want to run it.";
        r.errorBody = ss.str();
        r.chips.push_back(elapsedChip());
        return r;
    }

    if (risk == FileRisk::TextLike) {
        // Inline read.  Reuse ReadFile so the size cap, binary
        // detection, language hint, and chip composition all match
        // exactly what /read produces.
        ReadResult rr = ReadFile(resolved, ctx);
        r.chips     = rr.chips;
        r.body      = rr.body;
        r.errorBody = rr.errorBody;
        r.bodyLang  = rr.bodyLang;
        // Replace the trailing elapsed chip with our own so the
        // displayed elapsed time covers the resolution + read,
        // not just the read.  ReadFile always appends an elapsed
        // chip last; pop it and replace.
        if (!r.chips.empty()) r.chips.pop_back();
        r.chips.push_back(elapsedChip());
        return r;
    }

    // ── Safe branch: ShellExecuteW ───────────────────────────────
    std::string shellErr = ShellOpenFile(resolved);
    if (!shellErr.empty()) {
        r.chips.push_back("failed");
        r.errorBody = "Could not launch \"" + resolved + "\": " + shellErr;
        r.chips.push_back(elapsedChip());
        return r;
    }

    // Successful launch.  The body summarises what happened so the
    // model has something concrete to acknowledge to the user.
    r.chips.push_back("launched");
    r.body = "Opened " + resolved + " in the default application.\n";
    r.chips.push_back(elapsedChip());
    return r;
}
