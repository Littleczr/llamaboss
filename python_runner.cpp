// python_runner.cpp
//
// Controlled Python backend foundation.  See python_runner.h for the
// user-facing safety contract.  This intentionally mirrors CmdExecutor's
// Windows process model but narrows the command surface to fixed helpers.

#include "python_runner.h"

#include "python_arg_policy.h"

#include "path_safety.h"
#include "project_manager.h"
#include "secrets_store.h"
#include "server_manager.h"
#include "tool_path.h"
#include "tool_path_safety.h"

#include <cwchar>
#include <iterator>
#include "ui_event_post.h"

#include "progress_output_fold.h" // collapse \r progress bars (tqdm, pip)
#include "python_resources.h"     // built-in helper scripts (RCDATA)

#include "lb_windows.h"

wxDEFINE_EVENT(wxEVT_PYTHON_COMPLETE, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_PYTHON_ERROR,    wxCommandEvent);

#ifdef _WIN32
namespace {

std::wstring Utf8ToWide(const std::string& s)
{
    return path_safety::Utf8ToWide(s);
}

std::string WideToUtf8(const std::wstring& s)
{
    return path_safety::WideToUtf8(s);
}

// Strict UTF-8 probe used by NormalizeProcessOutputUtf8().
bool IsStrictUtf8(const std::string& bytes)
{
    if (bytes.empty()) return true;
    const int inputLen = static_cast<int>(bytes.size());
    return ::MultiByteToWideChar(CP_UTF8,
                                 MB_ERR_INVALID_CHARS,
                                 bytes.data(),
                                 inputLen,
                                 nullptr,
                                 0) > 0;
}

size_t CountValidUtf8MultibyteSequences(const std::string& bytes)
{
    size_t count = 0;
    size_t i = 0;
    while (i < bytes.size()) {
        unsigned char c0 = static_cast<unsigned char>(bytes[i]);
        if (c0 < 0x80) {
            ++i;
            continue;
        }

        size_t len = 0;
        if (c0 >= 0xC2 && c0 <= 0xDF) {
            len = 2;
        } else if (c0 >= 0xE0 && c0 <= 0xEF) {
            len = 3;
        } else if (c0 >= 0xF0 && c0 <= 0xF4) {
            len = 4;
        } else {
            ++i;
            continue;
        }

        if (i + len > bytes.size()) break;

        unsigned char c1 = static_cast<unsigned char>(bytes[i + 1]);
        if ((c1 & 0xC0) != 0x80) {
            ++i;
            continue;
        }

        bool valid = true;
        if (len >= 3) {
            unsigned char c2 = static_cast<unsigned char>(bytes[i + 2]);
            if ((c2 & 0xC0) != 0x80) valid = false;

            // Reject overlong forms and UTF-16 surrogate halves.
            if (valid && c0 == 0xE0 && c1 < 0xA0) valid = false;
            if (valid && c0 == 0xED && c1 >= 0xA0) valid = false;
        }
        if (valid && len == 4) {
            unsigned char c3 = static_cast<unsigned char>(bytes[i + 3]);
            if ((c3 & 0xC0) != 0x80) valid = false;

            // Reject overlong 4-byte forms and values above U+10FFFF.
            if (valid && c0 == 0xF0 && c1 < 0x90) valid = false;
            if (valid && c0 == 0xF4 && c1 > 0x8F) valid = false;
        }

        if (valid) {
            ++count;
            i += len;
        } else {
            ++i;
        }
    }
    return count;
}

std::string DecodeWithCodePageToUtf8(UINT codePage,
                                     DWORD flags,
                                     const std::string& bytes)
{
    if (bytes.empty()) return bytes;

    const int inputLen = static_cast<int>(bytes.size());
    int wideLen = ::MultiByteToWideChar(codePage,
                                        flags,
                                        bytes.data(),
                                        inputLen,
                                        nullptr,
                                        0);
    if (wideLen <= 0) return std::string();

    std::wstring wide(static_cast<size_t>(wideLen), L'\0');
    if (::MultiByteToWideChar(codePage,
                              flags,
                              bytes.data(),
                              inputLen,
                              wide.data(),
                              wideLen) <= 0) {
        return std::string();
    }

    return WideToUtf8(wide);
}

std::string DropTrailingIncompleteUtf8Codepoint(const std::string& bytes)
{
    if (bytes.empty()) return bytes;

    const size_t n = bytes.size();
    size_t lead = n - 1;

    while (lead > 0) {
        unsigned char ch = static_cast<unsigned char>(bytes[lead]);
        if ((ch & 0xC0) != 0x80)
            break;
        --lead;
    }

    unsigned char first = static_cast<unsigned char>(bytes[lead]);
    size_t expected = 0;
    if ((first & 0x80) == 0x00) expected = 1;
    else if (first >= 0xC2 && first <= 0xDF) expected = 2;
    else if (first >= 0xE0 && first <= 0xEF) expected = 3;
    else if (first >= 0xF0 && first <= 0xF4) expected = 4;
    else return bytes;

    const size_t have = n - lead;
    if (expected > 1 && have < expected) {
        return bytes.substr(0, lead);
    }
    return bytes;
}

// Python helpers and user Python scripts are expected to speak UTF-8 back
// to the model.  On Windows, however, a subprocess can still inherit a legacy
// console/text encoding and return bytes such as CP-1252 smart punctuation.
// Those bytes are harmless for the UI, but they make the JSON request sent to
// llama-server invalid and can poison the rest of the chat transcript.
//
// Keep already-valid UTF-8 untouched.  If the stream was captured exactly at
// the byte cap, ReaderLoop may have split the final UTF-8 code point; try that
// one safe trim.  If the capture contains valid multibyte UTF-8 plus damage,
// preserve the UTF-8 and replace only the bad spans.  CP_ACP is reserved for
// legacy-ANSI-looking captures with no valid multibyte UTF-8 at all; decoding
// the whole stream as ANSI after one corrupt byte would mojibake otherwise-clean
// UTF-8 output.
std::string NormalizeProcessOutputUtf8(const std::string& bytes)
{
    if (bytes.empty()) return bytes;

    if (IsStrictUtf8(bytes)) {
        return bytes;
    }

    std::string trimmed = DropTrailingIncompleteUtf8Codepoint(bytes);
    if (trimmed.size() != bytes.size() && IsStrictUtf8(trimmed)) {
        return trimmed;
    }

    const bool looksLikeDamagedUtf8 = CountValidUtf8MultibyteSequences(bytes) > 0;

    if (looksLikeDamagedUtf8) {
        std::string recoded = DecodeWithCodePageToUtf8(CP_UTF8, 0, bytes);
        if (!recoded.empty()) return recoded;
    }

    std::string ansiRecoded = DecodeWithCodePageToUtf8(CP_ACP, 0, bytes);
    if (!ansiRecoded.empty()) return ansiRecoded;

    std::string forgivingRecoded = DecodeWithCodePageToUtf8(CP_UTF8, 0, bytes);
    if (!forgivingRecoded.empty()) return forgivingRecoded;

    // Extremely defensive last resort.  This branch should be practically
    // unreachable, but ASCII replacement still guarantees the downstream JSON
    // serializer receives safe text instead of preserving corrupt bytes.
    std::string safe;
    safe.reserve(bytes.size());
    for (unsigned char ch : bytes) {
        safe.push_back(ch < 0x80 ? static_cast<char>(ch) : '?');
    }
    return safe;
}
double NowSec()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string JoinPath(const std::string& a, const std::string& b)
{
    if (a.empty()) return b;
    const char sep = wxFILE_SEP_PATH;
    if (a.back() == '/' || a.back() == '\\') return a + b;
    return a + std::string(1, sep) + b;
}

std::string ParentDirOf(const std::string& path)
{
    std::string s = path;
    while (!s.empty() && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    size_t pos = s.find_last_of("/\\");
    if (pos == std::string::npos) return std::string();
    return s.substr(0, pos);
}

std::string LowerLocal(std::string s);

std::string BuiltInHelperScriptsDir()
{
    // Keep generated built-in helper scripts out of the user-facing
    // Scripts lane.  python_create_script / python_run_script use the
    // Scripts folder for reviewable user/model-authored scripts; helpers
    // such as csv_inspect.py and xlsx_create_workbook.py are LlamaBoss
    // implementation details and should not collide with those files.
    //
    // Default layout:
    //   %USERPROFILE%\LlamaBoss\Chats\<chat>\Scripts         (user scripts)
    //   %USERPROFILE%\LlamaBoss\Shared\Scripts                (no-chat fallback)
    //   %USERPROFILE%\LlamaBoss\System\PythonHelpers          (built-in helpers)
    std::string root = ServerManager::GetLlamaBossRootDir();
    if (root.empty()) root = ParentDirOf(ServerManager::GetWorkspaceDir());
    return JoinPath(JoinPath(root, "System"), "PythonHelpers");
}

std::string SharedLanesRootDir()
{
    // Delegates to the shared layout owner.  agent_controller.cpp's
    // one-shot-bypass shadow check resolves the SAME lanes through the
    // SAME ServerManager functions, so the two cannot diverge.
    return ServerManager::GetSharedLanesRootDir();
}

std::string TrimTrailingSeparators(std::string s)
{
    while (!s.empty() && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    return s;
}

std::string PathBaseNameLocal(const std::string& path)
{
    std::string s = TrimTrailingSeparators(path);
    size_t pos = s.find_last_of("/\\");
    return (pos == std::string::npos) ? s : s.substr(pos + 1);
}


std::string ChatFolderFromCwd(const std::string& cwd)
{
    // Shared recognizer for the per-conversation folder layout created
    // by ChatHistory::EnsureChatFolder():
    //   %USERPROFILE%\LlamaBoss\Chats\<date>_<slug>_<id>\Workspace
    // Implementation lives in chat_folders.h (via ServerManager) so this
    // file and agent_controller.cpp resolve lanes identically by
    // construction.
    return ServerManager::ChatFolderFromCwd(cwd);
}

std::string LaneDirForCwd(const std::string& cwd, const std::string& lane)
{
    return ServerManager::ConversationLaneDirForCwd(cwd, lane);
}

std::string DocumentsDirForCwd(const std::string& cwd)
{
    return LaneDirForCwd(cwd, "Documents");
}

std::string SpreadsheetsDirForCwd(const std::string& cwd)
{
    return LaneDirForCwd(cwd, "Spreadsheets");
}

std::string PdfsDirForCwd(const std::string& cwd)
{
    return LaneDirForCwd(cwd, "PDFs");
}

std::string WordDirForCwd(const std::string& cwd)
{
    return LaneDirForCwd(cwd, "Word");
}

std::string FilledFormsDirForCwd(const std::string& cwd)
{
    return LaneDirForCwd(cwd, "Filled Forms");
}

std::string ExtractedDirForCwd(const std::string& cwd)
{
    return LaneDirForCwd(cwd, "Extracted");
}

std::string UserScriptsDirForCwd(const std::string& cwd)
{
    return LaneDirForCwd(cwd, "Scripts");
}

std::string ScanRootForCwd(const std::string& cwd)
{
    std::string chatFolder = ChatFolderFromCwd(cwd);
    return chatFolder.empty() ? SharedLanesRootDir() : chatFolder;
}

bool FileExistsRegular(const std::string& path)
{
    DWORD attrs = GetFileAttributesW(Utf8ToWide(path).c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}


std::string UniqueSidecarPathNear(const std::string& targetPath,
                                  const std::string& suffix)
{
    static std::atomic<unsigned long> s_counter{0};

    std::string dir = ParentDirOf(targetPath);
    std::string base = PathBaseNameLocal(targetPath);
    if (dir.empty() || base.empty()) return std::string();

    for (int i = 0; i < 100; ++i) {
        unsigned long n = ++s_counter;
        std::ostringstream name;
        name << base << ".tmp."
             << GetCurrentProcessId() << "."
             << GetTickCount64() << "."
             << n << suffix;
        std::string candidate = JoinPath(dir, name.str());
        if (!FileExistsRegular(candidate))
            return candidate;
    }
    return std::string();
}

bool CreateUniqueSidecarForWrite(const std::string& targetPath,
                                 const std::string& suffix,
                                 const std::string& purpose,
                                 std::string& pathOut,
                                 HANDLE& handleOut,
                                 std::string& errorOut)
{
    pathOut.clear();
    handleOut = INVALID_HANDLE_VALUE;

    constexpr DWORD attrs = FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
    for (int attempt = 0; attempt < 100; ++attempt) {
        std::string candidate = UniqueSidecarPathNear(targetPath, suffix);
        if (candidate.empty()) {
            errorOut = "Could not allocate temporary path for " + purpose + ": " + targetPath;
            return false;
        }

        HANDLE h = CreateFileW(Utf8ToWide(candidate).c_str(),
                               GENERIC_WRITE,
                               0,
                               nullptr,
                               CREATE_NEW,
                               attrs,
                               nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            pathOut = std::move(candidate);
            handleOut = h;
            return true;
        }

        DWORD err = GetLastError();
        if (err == ERROR_FILE_EXISTS || err == ERROR_ALREADY_EXISTS) {
            continue;  // check-to-create race; try another sidecar name
        }

        errorOut = "Could not create temporary file for " + purpose + ": " +
                   targetPath + ", error=" + std::to_string(err);
        return false;
    }

    errorOut = "Could not create a unique temporary file for " + purpose +
               " after multiple attempts: " + targetPath;
    return false;
}

bool WriteBytesAtomic(const std::string& targetPath,
                      const char* data,
                      size_t size,
                      std::string& errorOut)
{
    std::string tmpPath;
    HANDLE h = INVALID_HANDLE_VALUE;
    if (!CreateUniqueSidecarForWrite(targetPath,
                                     ".tmp",
                                     "atomic write",
                                     tmpPath,
                                     h,
                                     errorOut)) {
        return false;
    }

    bool ok = true;
    size_t offset = 0;
    while (offset < size) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(size - offset, 1024 * 1024));
        DWORD written = 0;
        if (!WriteFile(h, data + offset, chunk, &written, nullptr) || written != chunk) {
            ok = false;
            errorOut = "Failed while writing temporary file for: " + targetPath +
                       ", error=" + std::to_string(GetLastError());
            break;
        }
        offset += written;
    }

    if (ok && !FlushFileBuffers(h)) {
        ok = false;
        errorOut = "Failed to flush temporary file for: " + targetPath +
                   ", error=" + std::to_string(GetLastError());
    }

    CloseHandle(h);

    if (!ok) {
        DeleteFileW(Utf8ToWide(tmpPath).c_str());
        return false;
    }

    if (!MoveFileExW(Utf8ToWide(tmpPath).c_str(),
                     Utf8ToWide(targetPath).c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD err = GetLastError();
        DeleteFileW(Utf8ToWide(tmpPath).c_str());
        errorOut = "Could not install file atomically: " + targetPath +
                   ", error=" + std::to_string(err);
        return false;
    }

    return true;
}

bool WriteUtf8ArgTempFile(const std::string& prefix,
                          const std::string& content,
                          std::string& pathOut,
                          std::string& errorOut)
{
    std::string argDir = JoinPath(BuiltInHelperScriptsDir(), "Args");
    wxFileName::Mkdir(wxString::FromUTF8(argDir.c_str()),
                      wxS_DIR_DEFAULT,
                      wxPATH_MKDIR_FULL);

    std::string target = JoinPath(argDir, prefix + ".json");
    HANDLE h = INVALID_HANDLE_VALUE;
    if (!CreateUniqueSidecarForWrite(target,
                                     ".json",
                                     "helper-argument file",
                                     pathOut,
                                     h,
                                     errorOut)) {
        pathOut.clear();
        return false;
    }

    bool ok = true;
    size_t offset = 0;
    while (offset < content.size()) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(content.size() - offset, 1024 * 1024));
        DWORD written = 0;
        if (!WriteFile(h, content.data() + offset, chunk, &written, nullptr) || written != chunk) {
            ok = false;
            errorOut = "Failed while writing helper-argument file: " + pathOut +
                       ", error=" + std::to_string(GetLastError());
            break;
        }
        offset += written;
    }

    if (ok && !FlushFileBuffers(h)) {
        ok = false;
        errorOut = "Failed to flush helper-argument file: " + pathOut +
                   ", error=" + std::to_string(GetLastError());
    }

    CloseHandle(h);

    if (!ok) {
        DeleteFileW(Utf8ToWide(pathOut).c_str());
        pathOut.clear();
        return false;
    }

    return true;
}

std::string LowerLocal(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string ExtensionLower(const std::string& path)
{
    size_t slash = path.find_last_of("/\\");
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    if (dot == std::string::npos) return std::string();
    return LowerLocal(base.substr(dot));
}

std::string LanguageForFile(const std::string& path)
{
    std::string ext = ExtensionLower(path);
    if (ext == ".py") return "python";
    if (ext == ".md" || ext == ".markdown") return "markdown";
    if (ext == ".txt" || ext == ".log") return "text";
    if (ext == ".csv") return "csv";
    if (ext == ".tsv") return "tsv";
    if (ext == ".json") return "json";
    if (ext == ".html" || ext == ".htm") return "html";
    if (ext == ".css") return "css";
    if (ext == ".cpp" || ext == ".cc" || ext == ".cxx") return "cpp";
    if (ext == ".h" || ext == ".hpp") return "cpp";
    return std::string();
}

// These helpers are defined later in this file and are also used by the
// Python-run artifact attachment path below. Keep declarations here so the
// earlier helper code builds cleanly on MSVC.
size_t FileSizeBytes(const std::string& path);
int CountFileLines(const std::string& path);
std::string BaseNameOf(const std::string& path);

std::string NormalizePathForCompare(std::string path)
{
    // Compare canonical Windows paths, not raw strings.  Normalizing
    // only slashes/case would let a path such as
    // C:\Users\Cesar\LlamaBoss\Workspace\..\..\outside.txt appear to be
    // under the LlamaBoss root during prefix checks.  GetFullPathNameW
    // collapses . and .. segments before we do the case-insensitive
    // boundary comparison.
    if (!path.empty()) {
        std::wstring w = Utf8ToWide(path);
        DWORD needed = GetFullPathNameW(w.c_str(), 0, nullptr, nullptr);
        if (needed > 0) {
            std::vector<wchar_t> buf(static_cast<size_t>(needed) + 1, L'\0');
            DWORD len = GetFullPathNameW(w.c_str(),
                                         static_cast<DWORD>(buf.size()),
                                         buf.data(),
                                         nullptr);
            if (len > 0 && len < buf.size()) {
                path = WideToUtf8(std::wstring(buf.data(), len));
            }
        }
    }

    std::replace(path.begin(), path.end(), '/', '\\');
    while (!path.empty() && (path.back() == '\\' || path.back() == '/')) path.pop_back();
    std::transform(path.begin(), path.end(), path.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return path;
}

bool IsAbsoluteWindowsPath(const std::string& path)
{
    if (path.size() >= 3 && std::isalpha(static_cast<unsigned char>(path[0])) &&
        path[1] == ':' && (path[2] == '\\' || path[2] == '/')) {
        return true;
    }
    return path.size() >= 2 &&
           ((path[0] == '\\' && path[1] == '\\') ||
            (path[0] == '/'  && path[1] == '/'));
}

bool IsPathUnderRootForCompare(const std::string& path, const std::string& root)
{
    if (path.empty() || root.empty()) return false;

    std::string p = NormalizePathForCompare(path);
    std::string r = NormalizePathForCompare(root);
    if (p == r) return true;
    if (r.empty()) return false;

    const char sep = '\\';
    if (r.back() != sep) r.push_back(sep);
    return p.size() > r.size() && p.compare(0, r.size(), r) == 0;
}

std::vector<std::string> PythonArtifactRoots(const std::string& cwd,
                                             const std::string& activeProjectRoot)
{
    std::vector<std::string> roots;

    auto addRoot = [&](const std::string& root) {
        if (root.empty()) return;
        std::string norm = NormalizePathForCompare(root);
        for (const auto& existing : roots) {
            if (NormalizePathForCompare(existing) == norm) return;
        }
        roots.push_back(root);
    };

    addRoot(ScanRootForCwd(cwd));
    addRoot(activeProjectRoot);
    return roots;
}

std::vector<std::string> PythonAutoArtifactScanRoots(const std::string& cwd,
                                                     const std::string& activeProjectRoot)
{
    // Snapshotting the entire LlamaBoss root plus the active project
    // before and after every run gets expensive once Projects, Sources,
    // Templates, or cache folders grow.  Keep explicit ARTIFACT: paths
    // fully flexible via IsAllowedPythonArtifactPath(); for automatic
    // discovery, scan only the places scripts are expected to create
    // user-facing files.
    std::vector<std::string> roots;

    auto addRoot = [&](const std::string& root) {
        if (root.empty()) return;
        std::string norm = NormalizePathForCompare(root);
        for (const auto& existing : roots) {
            if (NormalizePathForCompare(existing) == norm) return;
        }
        roots.push_back(root);
    };

    std::string effectiveCwd = cwd.empty() ? ServerManager::GetWorkspaceDir() : cwd;
    addRoot(effectiveCwd);
    addRoot(DocumentsDirForCwd(cwd));
    addRoot(SpreadsheetsDirForCwd(cwd));
    addRoot(PdfsDirForCwd(cwd));
    addRoot(WordDirForCwd(cwd));
    addRoot(FilledFormsDirForCwd(cwd));

    std::string chatFolder = ChatFolderFromCwd(cwd);
    if (!chatFolder.empty()) {
        addRoot(chatFolder);
    }

    if (!activeProjectRoot.empty()) {
        addRoot(JoinPath(activeProjectRoot, "Outputs"));
        addRoot(JoinPath(activeProjectRoot, "Documents"));
        addRoot(JoinPath(activeProjectRoot, "Spreadsheets"));
        addRoot(JoinPath(activeProjectRoot, "PDFs"));
        addRoot(JoinPath(activeProjectRoot, "Word"));
        addRoot(JoinPath(activeProjectRoot, "Filled Forms"));
    }

    return roots;
}

bool IsAllowedPythonArtifactPath(const std::string& path,
                                 const std::string& cwd,
                                 const std::string& activeProjectRoot)
{
    if (!FileExistsRegular(path)) return false;
    for (const auto& root : PythonArtifactRoots(cwd, activeProjectRoot)) {
        if (IsPathUnderRootForCompare(path, root)) return true;
    }
    return false;
}

std::string TrimArtifactValue(const std::string& raw)
{
    std::string s = raw;
    size_t a = s.find_first_not_of(" \t\r\n`\"'");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t\r\n`\"'");
    s = s.substr(a, b - a + 1);

    // Keep a plain path if a script prints a Markdown-ish label like:
    //   ARTIFACT: Outputs\file.md (ready)
    // The suffix is UI prose, not part of the path.  Strip only a trailing
    // parenthesized status token after a space so filenames such as
    // "report (final).md" remain untouched.
    if (!s.empty() && s.back() == ')') {
        size_t open = s.find_last_of('(');
        if (open != std::string::npos && open > 0 && s[open - 1] == ' ') {
            std::string token = s.substr(open + 1, s.size() - open - 2);
            std::string key = LowerLocal(token);
            bool statusLike = !key.empty() && key.size() <= 32;
            for (char ch : key) {
                if (!((ch >= 'a' && ch <= 'z') || ch == ' ' || ch == '-' || ch == '_')) {
                    statusLike = false;
                    break;
                }
            }
            if (statusLike) {
                s = s.substr(0, open - 1);
                while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
            }
        }
    }

    return s;
}

std::string ResolvePythonArtifactPathCandidate(const std::string& value,
                                               const std::string& cwd,
                                               const std::string& activeProjectRoot)
{
    std::string candidate = TrimArtifactValue(value);
    if (candidate.empty()) return std::string();

    if (IsAbsoluteWindowsPath(candidate)) {
        return FileExistsRegular(candidate) ? candidate : std::string();
    }

    std::vector<std::string> bases;
    if (!activeProjectRoot.empty()) bases.push_back(activeProjectRoot);
    if (!cwd.empty()) bases.push_back(cwd);
    std::string scanRoot = ScanRootForCwd(cwd);
    if (!scanRoot.empty()) bases.push_back(scanRoot);

    for (const auto& base : bases) {
        std::string path = JoinPath(base, candidate);
        if (FileExistsRegular(path)) return path;
    }

    return std::string();
}

bool PresentedFileAlreadyAdded(const PythonRunResult& result, const std::string& path)
{
    std::string norm = NormalizePathForCompare(path);
    for (const auto& f : result.presentedFiles) {
        if (!f.diskPath.empty() && NormalizePathForCompare(f.diskPath) == norm) return true;
    }
    return false;
}

void AddPresentedDiskFile(PythonRunResult& result, const std::string& path)
{
    if (path.empty() || PresentedFileAlreadyAdded(result, path)) return;

    PresentedFile f;
    f.displayName = BaseNameOf(path);
    f.language    = LanguageForFile(path);
    f.diskPath    = path;
    f.sizeBytes   = FileSizeBytes(path);
    f.lineCount   = CountFileLines(path);
    result.presentedFiles.push_back(std::move(f));
}

bool AttachExplicitPythonRunArtifacts(PythonRunResult& result,
                                      const std::string& cwd,
                                      const std::string& activeProjectRoot)
{
    if (result.exitCode != 0 || result.stdoutText.empty()) return false;

    bool attachedAny = false;
    std::istringstream lines(result.stdoutText);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();

        std::string key = LowerLocal(line);
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;

        const bool isExplicitArtifact =
            key.rfind("artifact:", 0) == 0 ||
            key.rfind("artifact_file:", 0) == 0 ||
            key.rfind("artifact path:", 0) == 0 ||
            key.rfind("output artifact:", 0) == 0 ||
            key.rfind("user-facing markdown transcript:", 0) == 0;

        if (!isExplicitArtifact) continue;

        std::string path = ResolvePythonArtifactPathCandidate(line.substr(colon + 1), cwd, activeProjectRoot);
        if (path.empty()) continue;
        if (!IsAllowedPythonArtifactPath(path, cwd, activeProjectRoot)) continue;

        size_t size = FileSizeBytes(path);
        if (size == 0 || size > 25 * 1024 * 1024) continue;

        AddPresentedDiskFile(result, path);
        attachedAny = true;
    }

    return attachedAny;
}

std::string HelperScriptPath(const std::string& helperName)
{
    return JoinPath(BuiltInHelperScriptsDir(), helperName + ".py");
}

bool WriteHelperScript(const std::string& helperName,
                       const char*        data,
                       size_t             size,
                       std::string&       scriptPathOut,
                       std::string&       errorOut)
{
    scriptPathOut = HelperScriptPath(helperName);

    // Helper scripts are embedded resources — they never change
    // within a process.  Pre-fix this function truncated and rewrote
    // the .py file on every helper invocation, which churned the disk
    // for no benefit (and opened a small "partial script on disk
    // during write" window every call).  Track which helper paths have
    // already been written this process, but verify the file still exists
    // before short-circuiting.  If the helper folder is cleaned/deleted
    // while LlamaBoss is still open, the stale cache entry is discarded
    // and the helper is recreated below.
    {
        static std::mutex            s_writtenMutex;
        static std::set<std::string> s_writtenScriptPaths;
        std::lock_guard<std::mutex> lock(s_writtenMutex);

        const std::string cacheKey = LowerLocal(scriptPathOut);
        if (s_writtenScriptPaths.count(cacheKey) > 0) {
            if (FileExistsRegular(scriptPathOut)) {
                return true;   // already written and still present
            }
            s_writtenScriptPaths.erase(cacheKey);   // stale cache entry; rewrite below
        }

        // Hold the lock across the actual write.  Helper invocations
        // are infrequent and the writes are tiny (kilobytes), so the
        // serialization cost is negligible.  Holding the lock prevents
        // two threads from racing to write the same script in the
        // unlikely case of concurrent helper kicks.

        wxFileName::Mkdir(wxString::FromUTF8(BuiltInHelperScriptsDir().c_str()),
                          wxS_DIR_DEFAULT,
                          wxPATH_MKDIR_FULL);

        if (!WriteBytesAtomic(scriptPathOut, data, size, errorOut)) {
            return false;
        }

        s_writtenScriptPaths.insert(cacheKey);
    }

    return true;
}

// Writes built-in helper `helperName` from its RCDATA resource (source:
// assets/python/<helperName>.py, see python_resources.h) and returns the
// on-disk path.  Only names in lb_pyres::kHelperNames are accepted.
bool EnsureBuiltinHelperScript(const std::string& helperName,
                               std::string&       scriptPathOut,
                               std::string&       errorOut)
{
    if (!lb_pyres::IsHelperName(helperName)) {
        errorOut = "Unknown built-in Python helper: " + helperName;
        return false;
    }
    const char* data = nullptr;
    size_t      size = 0;
    if (!lb_pyres::Load(helperName, data, size, errorOut)) return false;
    return WriteHelperScript(helperName, data, size, scriptPathOut, errorOut);
}

bool JsonStringField(const std::string& json,
                     const std::string& field,
                     std::string& out)
{
    std::string key = "\"" + field + "\"";
    size_t k = json.find(key);
    if (k == std::string::npos) return false;
    size_t colon = json.find(':', k + key.size());
    if (colon == std::string::npos) return false;
    size_t q = json.find('"', colon + 1);
    if (q == std::string::npos) return false;

    std::string value;
    bool esc = false;
    for (size_t i = q + 1; i < json.size(); ++i) {
        char ch = json[i];
        if (esc) {
            switch (ch) {
            case '"': value.push_back('"'); break;
            case '\\': value.push_back('\\'); break;
            case '/': value.push_back('/'); break;
            case 'b': value.push_back('\b'); break;
            case 'f': value.push_back('\f'); break;
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            default: value.push_back(ch); break;
            }
            esc = false;
        } else if (ch == '\\') {
            esc = true;
        } else if (ch == '"') {
            out = value;
            return true;
        } else {
            value.push_back(ch);
        }
    }
    return false;
}

size_t FileSizeBytes(const std::string& path)
{
    try {
        std::ifstream f(std::filesystem::path(Utf8ToWide(path)), std::ios::binary | std::ios::ate);
        if (!f) return 0;
        return static_cast<size_t>(f.tellg());
    } catch (...) {
        return 0;
    }
}

int CountFileLines(const std::string& path)
{
    try {
        std::ifstream f(std::filesystem::path(Utf8ToWide(path)), std::ios::binary);
        if (!f) return 0;
        std::string data((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        if (data.empty()) return 0;
        int n = 0;
        for (char c : data) if (c == '\n') ++n;
        if (data.back() != '\n') ++n;
        return n;
    } catch (...) {
        return 0;
    }
}

std::string BaseNameOf(const std::string& path)
{
    size_t p = path.find_last_of("/\\");
    return (p == std::string::npos) ? path : path.substr(p + 1);
}


std::string SafeOutputStemForTool(const std::string& text, const std::string& fallback)
{
    std::string out;
    out.reserve(std::min<size_t>(text.size(), 64));
    for (char ch : text) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c)) out.push_back(static_cast<char>(std::tolower(c)));
        else if (c == '_' || c == '-' || c == '.') out.push_back(ch);
        else if (std::isspace(c) || c == ':' || c == '\\' || c == '/' || c == '|') {
            if (!out.empty() && out.back() != '_') out.push_back('_');
        }
        if (out.size() >= 48) break;
    }
    while (!out.empty() && (out.back() == '_' || out.back() == '.')) out.pop_back();
    if (out.empty()) out = fallback;
    return out;
}

std::string ToolOutputsDirForCwd(const std::string& cwd)
{
    return LaneDirForCwd(cwd, "ToolOutputs");
}

std::string UniqueToolOutputPath(const std::string& dir,
                                 const std::string& stem,
                                 const std::string& suffix,
                                 std::string& displayNameOut)
{
    std::string safeStem = SafeOutputStemForTool(stem, "tool_output");
    for (int i = 0; i < 1000; ++i) {
        displayNameOut = (i == 0)
            ? (safeStem + suffix)
            : (safeStem + "_" + std::to_string(i + 1) + suffix);
        std::string path = JoinPath(dir, displayNameOut);
        DWORD attrs = GetFileAttributesW(Utf8ToWide(path).c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) return path;
    }
    displayNameOut.clear();
    return std::string();
}

size_t CountTextLinesForOutput(const std::string& s)
{
    if (s.empty()) return 0;
    size_t n = 0;
    for (char c : s) if (c == '\n') ++n;
    if (s.back() != '\n') ++n;
    return n;
}

std::vector<std::string> SplitLinesForOutput(const std::string& s)
{
    std::vector<std::string> lines;
    std::istringstream in(s);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

std::string BuildHeadTailPreviewForOutput(const std::string& text,
                                          const std::string& displayName,
                                          bool streamWasCapped)
{
    constexpr size_t kHeadLines = 80;
    constexpr size_t kTailLines = 30;

    std::vector<std::string> lines = SplitLinesForOutput(text);
    const size_t totalLines = lines.empty() && !text.empty() ? 1 : lines.size();

    std::ostringstream out;
    out << "Large output saved to " << displayName << ".\n";
    out << "Showing preview";
    if (totalLines > 0) out << " of " << totalLines << " captured line" << (totalLines == 1 ? "" : "s");
    out << ".";
    if (streamWasCapped) out << " Output hit the capture cap; the saved file contains the captured portion.";
    out << "\n\n";

    if (lines.empty()) {
        out << text;
        return out.str();
    }

    if (lines.size() <= kHeadLines + kTailLines) {
        for (const auto& line : lines) out << line << "\n";
        return out.str();
    }

    for (size_t i = 0; i < kHeadLines; ++i) out << lines[i] << "\n";
    out << "\n[... " << (lines.size() - kHeadLines - kTailLines)
        << " lines omitted; open " << displayName << " for full captured output ...]\n\n";
    for (size_t i = lines.size() - kTailLines; i < lines.size(); ++i) out << lines[i] << "\n";
    return out.str();
}

bool WriteUtf8TextFileForOutput(const std::string& path, const std::string& content)
{
    try {
        std::ofstream f(std::filesystem::path(Utf8ToWide(path)), std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
        return f.good();
    } catch (...) {
        return false;
    }
}

bool ShouldExternalizeOutput(const std::string& text)
{
    constexpr size_t kMaxInlineBytes = 16 * 1024;
    constexpr size_t kMaxInlineLines = 120;
    return text.size() > kMaxInlineBytes || CountTextLinesForOutput(text) > kMaxInlineLines;
}

void ExternalizeOnePythonStream(PythonRunResult& result,
                                std::string& streamText,
                                const std::string& cwd,
                                const std::string& stem,
                                const std::string& streamName,
                                bool streamWasCapped)
{
    if (!ShouldExternalizeOutput(streamText)) return;

    std::string dir = ToolOutputsDirForCwd(cwd);
    wxFileName::Mkdir(wxString::FromUTF8(dir.c_str()), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);

    std::string displayName;
    std::string path = UniqueToolOutputPath(dir, stem + "_" + streamName, ".txt", displayName);
    if (path.empty()) return;

    std::string fileBody = streamText;
    if (streamWasCapped) {
        if (!fileBody.empty() && fileBody.back() != '\n') fileBody += "\n";
        fileBody += "\n[LlamaBoss output capture cap reached; additional output was discarded.]\n";
    }

    if (!WriteUtf8TextFileForOutput(path, fileBody)) return;

    PresentedFile f;
    f.displayName = displayName;
    f.language    = "text";
    f.diskPath    = path;
    f.sizeBytes   = FileSizeBytes(path);
    f.lineCount   = static_cast<int>(CountTextLinesForOutput(fileBody));
    result.presentedFiles.push_back(std::move(f));

    streamText = BuildHeadTailPreviewForOutput(streamText, displayName, streamWasCapped);
}

void ApplyLargePythonRunOutputHandling(PythonRunResult& result, const std::string& cwd)
{
    if (!ShouldExternalizeOutput(result.stdoutText) &&
        !ShouldExternalizeOutput(result.stderrText)) {
        return;
    }

    std::string stem = SafeOutputStemForTool(result.commandEcho.empty()
        ? std::string("python_run_output")
        : result.commandEcho,
        "python_run_output");
    ExternalizeOnePythonStream(result, result.stdoutText, cwd, stem, "stdout", result.truncated);
    ExternalizeOnePythonStream(result, result.stderrText, cwd, stem, "stderr", result.truncated);
}

void AttachCsvReportArtifact(PythonRunResult& result)
{
    if (result.exitCode != 0 || result.stdoutText.empty()) return;

    std::string outputPath;
    if (!JsonStringField(result.stdoutText, "output_path", outputPath)) return;
    if (outputPath.empty()) return;

    std::string outputName;
    if (!JsonStringField(result.stdoutText, "output_filename", outputName) || outputName.empty()) {
        outputName = BaseNameOf(outputPath);
    }

    size_t size = FileSizeBytes(outputPath);
    if (size == 0) return;

    PresentedFile f;
    f.displayName = outputName.empty() ? std::string("csv_report.md") : outputName;
    f.language    = "markdown";
    f.diskPath    = outputPath;
    f.sizeBytes   = size;
    f.lineCount   = CountFileLines(outputPath);
    result.presentedFiles.push_back(std::move(f));
}


void AttachXlsxReportArtifact(PythonRunResult& result)
{
    if (result.exitCode != 0 || result.stdoutText.empty()) return;

    std::string outputPath;
    if (!JsonStringField(result.stdoutText, "output_path", outputPath)) return;
    if (outputPath.empty()) return;

    std::string outputName;
    if (!JsonStringField(result.stdoutText, "output_filename", outputName) || outputName.empty()) {
        outputName = BaseNameOf(outputPath);
    }

    size_t size = FileSizeBytes(outputPath);
    if (size == 0) return;

    PresentedFile f;
    f.displayName = outputName.empty() ? std::string("xlsx_report.md") : outputName;
    f.language    = "markdown";
    f.diskPath    = outputPath;
    f.sizeBytes   = size;
    f.lineCount   = CountFileLines(outputPath);
    result.presentedFiles.push_back(std::move(f));
}



void AttachXlsxWorkbookArtifact(PythonRunResult& result)
{
    if (result.exitCode != 0 || result.stdoutText.empty()) return;

    std::string outputPath;
    if (!JsonStringField(result.stdoutText, "output_path", outputPath)) return;
    if (outputPath.empty()) return;

    std::string outputName;
    if (!JsonStringField(result.stdoutText, "output_filename", outputName) || outputName.empty()) {
        outputName = BaseNameOf(outputPath);
    }

    size_t size = FileSizeBytes(outputPath);
    if (size == 0) return;

    PresentedFile f;
    f.displayName = outputName.empty() ? std::string("workbook.xlsx") : outputName;
    f.language    = "xlsx";
    f.diskPath    = outputPath;
    f.sizeBytes   = size;
    f.lineCount   = 0;
    result.presentedFiles.push_back(std::move(f));
}

void AttachPdfExtractTextArtifact(PythonRunResult& result)
{
    if (result.exitCode != 0 || result.stdoutText.empty()) return;

    std::string outputPath;
    if (!JsonStringField(result.stdoutText, "output_path", outputPath)) return;
    if (outputPath.empty()) return;

    std::string outputName;
    if (!JsonStringField(result.stdoutText, "output_filename", outputName) || outputName.empty()) {
        outputName = BaseNameOf(outputPath);
    }

    size_t size = FileSizeBytes(outputPath);
    if (size == 0) return;

    PresentedFile f;
    f.displayName = outputName.empty() ? std::string("pdf_extracted_text.md") : outputName;
    f.language    = "markdown";
    f.diskPath    = outputPath;
    f.sizeBytes   = size;
    f.lineCount   = CountFileLines(outputPath);
    result.presentedFiles.push_back(std::move(f));
}

void AttachDocxExtractTextArtifact(PythonRunResult& result)
{
    if (result.exitCode != 0 || result.stdoutText.empty()) return;

    std::string outputPath;
    if (!JsonStringField(result.stdoutText, "output_path", outputPath)) return;
    if (outputPath.empty()) return;

    std::string outputName;
    if (!JsonStringField(result.stdoutText, "output_filename", outputName) || outputName.empty()) {
        outputName = BaseNameOf(outputPath);
    }

    size_t size = FileSizeBytes(outputPath);
    if (size == 0) return;

    PresentedFile f;
    f.displayName = outputName.empty() ? std::string("docx_extracted_text.md") : outputName;
    f.language    = "markdown";
    f.diskPath    = outputPath;
    f.sizeBytes   = size;
    f.lineCount   = CountFileLines(outputPath);
    result.presentedFiles.push_back(std::move(f));
}

void AttachPdfFillFormArtifact(PythonRunResult& result)
{
    if (result.exitCode != 0 || result.stdoutText.empty()) return;

    std::string outputPath;
    if (!JsonStringField(result.stdoutText, "output_path", outputPath)) return;
    if (outputPath.empty()) return;

    std::string outputName;
    if (!JsonStringField(result.stdoutText, "output_filename", outputName) || outputName.empty()) {
        outputName = BaseNameOf(outputPath);
    }

    size_t size = FileSizeBytes(outputPath);
    if (size == 0) return;

    PresentedFile f;
    f.displayName = outputName.empty() ? std::string("filled.pdf") : outputName;
    // Empty language hint -- this is a binary PDF, not source.  The
    // renderer treats this as "no syntax highlight" and the [Open]
    // button hands off to the OS default app (Acrobat, Edge, etc.)
    // because IsLikelyCodeFile() returns false for .pdf.
    f.language    = "";
    f.diskPath    = outputPath;
    f.sizeBytes   = size;
    f.lineCount   = 0;
    result.presentedFiles.push_back(std::move(f));
}

void SplitPythonRunScriptInvocation(const std::string& raw,
                                    std::string&       scriptRequestOut,
                                    std::vector<std::string>& argvOut)
{
    scriptRequestOut.clear();
    argvOut.clear();

    auto trimCopy = [](const std::string& s) -> std::string {
        const size_t first = s.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return std::string();
        const size_t last = s.find_last_not_of(" \t\r\n");
        return s.substr(first, last - first + 1);
    };

    const size_t firstBreak = raw.find_first_of("\r\n");
    if (firstBreak != std::string::npos) {
        scriptRequestOut = trimCopy(raw.substr(0, firstBreak));

        size_t restStart = firstBreak;
        while (restStart < raw.size() &&
               (raw[restStart] == '\r' || raw[restStart] == '\n')) {
            ++restStart;
        }

        std::string rest = restStart < raw.size() ? raw.substr(restStart) : std::string();
        size_t lineStart = 0;
        while (lineStart <= rest.size()) {
            size_t lineEnd = rest.find('\n', lineStart);
            std::string line = (lineEnd == std::string::npos)
                ? rest.substr(lineStart)
                : rest.substr(lineStart, lineEnd - lineStart);

            std::string arg = trimCopy(line);
            if (!arg.empty()) argvOut.push_back(std::move(arg));

            if (lineEnd == std::string::npos) break;
            lineStart = lineEnd + 1;
        }
        return;
    }

    const std::string oneLine = trimCopy(raw);
    if (oneLine.empty()) return;

    // Preferred contract is multi-line args: script on line 1, each argv
    // value on a later line. This fallback keeps the runner forgiving for the
    // common model form "helper.py C:\\path\\to\\input" by treating everything
    // after a terminal ".py" plus whitespace as one argument. It remains
    // safe because ResolveRunnableScriptPath still validates the script lane.
    std::string lower = LowerLocal(oneLine);
    size_t search = 0;
    while (true) {
        size_t py = lower.find(".py", search);
        if (py == std::string::npos) break;
        const size_t afterPy = py + 3;
        const bool terminalScriptMarker =
            afterPy == oneLine.size() ||
            (afterPy < oneLine.size() &&
             (oneLine[afterPy] == ' ' || oneLine[afterPy] == '\t'));
        if (terminalScriptMarker) {
            scriptRequestOut = trimCopy(oneLine.substr(0, afterPy));
            if (afterPy < oneLine.size()) {
                std::string inlineArg = trimCopy(oneLine.substr(afterPy));
                if (!inlineArg.empty()) argvOut.push_back(std::move(inlineArg));
            }
            return;
        }
        search = afterPy;
    }

    scriptRequestOut = oneLine;
}

bool ResolveRunnableScriptPath(const std::string& requested,
                               const std::string& cwd,
                               const std::string& activeProjectRoot,
                               std::string&       pathOut,
                               std::string&       displayNameOut,
                               std::string&       errorOut)
{
    std::string name = requested;
    size_t a = name.find_first_not_of(" \t\r\n");
    size_t b = name.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) {
        errorOut = "python_run_script requires a script filename or script path.";
        return false;
    }
    name = name.substr(a, b - a + 1);

    // Friendly project-relative form: Workflows\helper.py pins the active
    // project's Workflows lane.  Do not strip this to a bare filename, because
    // bare-name lookup checks the conversation Scripts lane first; stripping
    // would let Scripts\helper.py shadow the project workflow the caller
    // explicitly requested.
    {
        std::string pathish = name;
        std::replace(pathish.begin(), pathish.end(), '/', '\\');
        std::string key = LowerLocal(pathish);
        const std::string p1 = "workflows\\";
        const std::string p2 = "project\\workflows\\";

        size_t prefixLen = std::string::npos;
        if (key.rfind(p1, 0) == 0) {
            prefixLen = p1.size();
        } else if (key.rfind(p2, 0) == 0) {
            prefixLen = p2.size();
        }

        if (prefixLen != std::string::npos) {
            if (activeProjectRoot.empty()) {
                errorOut = "python_run_script Workflows\\... paths require an active project.";
                return false;
            }

            std::string workflowName = pathish.substr(prefixLen);
            size_t wa = workflowName.find_first_not_of(" \t\r\n");
            size_t wb = workflowName.find_last_not_of(" \t\r\n");
            if (wa == std::string::npos) {
                errorOut = "python_run_script Workflows\\... path must include a script filename.";
                return false;
            }
            workflowName = workflowName.substr(wa, wb - wa + 1);

            // Drive-letter forms don't belong in the lane selector.
            if (workflowName.find(':') != std::string::npos) {
                errorOut = "python_run_script Workflows\\... does not accept "
                           "drive-letter paths. Use Workflows\\<script>.py, "
                           "Workflows\\<folder>\\<script>.py, or the script's "
                           "absolute path.";
                return false;
            }

            // Nested forms (Workflows\<folder>\<script>.py) are handled by
            // ResolveProjectWorkflowScript, which resolves the last
            // component and verifies the pinned folder.  Sanitize only the
            // final filename component here.
            std::string workflowLeaf = workflowName;
            {
                const size_t lastSep = workflowLeaf.find_last_of("\\/");
                if (lastSep != std::string::npos) {
                    workflowLeaf = workflowLeaf.substr(lastSep + 1);
                }
            }
            if (workflowLeaf.empty()) {
                errorOut = "python_run_script Workflows\\... path must end "
                           "in a script filename.";
                return false;
            }

            size_t dot = workflowLeaf.find_last_of('.');
            if (dot == std::string::npos) {
                workflowName += ".py";
                workflowLeaf += ".py";
            } else if (LowerLocal(workflowLeaf.substr(dot)) != ".py") {
                errorOut = "python_run_script Workflows\\... paths must point to a .py file.";
                return false;
            }

            std::string safe = path_safety::SanitizeFilename(workflowLeaf, "");
            if (safe.empty() || safe != workflowLeaf) {
                errorOut = "Unsafe Python workflow script filename: " + workflowLeaf;
                return false;
            }
            if (python_arg_policy::IsReservedBuiltInPythonHelperName(workflowLeaf)) {
                errorOut = "Reserved Python helper filename: " + workflowLeaf +
                           ". Built-in helpers are run through their tool names, not through python_run_script.";
                return false;
            }

            ProjectWorkflowScriptInfo script;
            std::string projectError;
            if (!ProjectManager::ResolveProjectWorkflowScript(activeProjectRoot, workflowName, script, projectError)) {
                errorOut = projectError.empty()
                    ? ("Python project workflow script not found: " + workflowName)
                    : projectError;
                return false;
            }

            pathOut = script.path;
            displayNameOut = script.name;
            return true;
        }
    }

    // Friendly global-skill form: Skills\helper.py pins the LlamaBoss Skills
    // lane.  This mirrors Workflows\helper.py so a same-named conversation
    // script cannot shadow a skill the caller explicitly requested.
    {
        std::string pathish = name;
        std::replace(pathish.begin(), pathish.end(), '/', '\\');
        std::string key = LowerLocal(pathish);
        const std::string p1 = "skills\\";

        if (key.rfind(p1, 0) == 0) {
            std::string skillName = pathish.substr(p1.size());
            size_t sa = skillName.find_first_not_of(" \t\r\n");
            size_t sb = skillName.find_last_not_of(" \t\r\n");
            if (sa == std::string::npos) {
                errorOut = "python_run_script Skills\\... path must include a script filename.";
                return false;
            }
            skillName = skillName.substr(sa, sb - sa + 1);

            // Nested skill paths are the natural on-disk form
            // (Skills\runPod\scripts\runpod_ops.py) and the form models
            // copy out of SKILL.md files, so accept it: remember the
            // FIRST component as the pinned skill folder, resolve the
            // LAST component through the normal skill-script search,
            // then verify the resolved script actually lives under the
            // pinned folder — keeping the lane selector's
            // explicit-pinning promise intact.
            if (skillName.find(':') != std::string::npos) {
                errorOut = "python_run_script Skills\\... does not accept "
                           "drive-letter paths. Use Skills\\<skill>\\<script>.py, "
                           "Skills\\<script>.py, or the script's absolute path.";
                return false;
            }
            std::string pinnedSkillFolder;
            {
                const size_t firstSep = skillName.find_first_of("\\/");
                if (firstSep != std::string::npos) {
                    pinnedSkillFolder = skillName.substr(0, firstSep);
                    const size_t lastSep = skillName.find_last_of("\\/");
                    skillName = skillName.substr(lastSep + 1);
                    size_t na = skillName.find_first_not_of(" \t\r\n");
                    if (pinnedSkillFolder.empty() || na == std::string::npos) {
                        errorOut = "python_run_script Skills\\... path must end "
                                   "in a script filename, e.g. "
                                   "Skills\\<skill>\\<script>.py.";
                        return false;
                    }
                }
            }

            size_t dot = skillName.find_last_of('.');
            if (dot == std::string::npos) {
                skillName += ".py";
            } else if (LowerLocal(skillName.substr(dot)) != ".py") {
                errorOut = "python_run_script Skills\\... paths must point to a .py file.";
                return false;
            }

            std::string safe = path_safety::SanitizeFilename(skillName, "");
            if (safe.empty() || safe != skillName) {
                errorOut = "Unsafe Python skill script filename: " + skillName;
                return false;
            }
            if (python_arg_policy::IsReservedBuiltInPythonHelperName(skillName)) {
                errorOut = "Reserved Python helper filename: " + skillName +
                           ". Built-in helpers are run through their tool names, not through python_run_script.";
                return false;
            }

            SkillScriptInfo script;
            std::string globalError;
            if (!ProjectManager::ResolveSkillScript(skillName, script, globalError)) {
                errorOut = globalError.empty()
                    ? ("Python skill script not found: " + skillName)
                    : globalError;
                return false;
            }

            // Nested-form pinning check: Skills\runPod\...\x.py must
            // resolve into the runPod skill folder, not a same-named
            // script that happens to live in another skill.
            if (!pinnedSkillFolder.empty()) {
                std::string resolvedLower = LowerLocal(script.path);
                std::replace(resolvedLower.begin(), resolvedLower.end(), '/', '\\');
                const std::string needle =
                    "\\" + LowerLocal(pinnedSkillFolder) + "\\";
                if (resolvedLower.find(needle) == std::string::npos) {
                    errorOut = "Skill script " + skillName +
                               " resolved outside the requested Skills\\" +
                               pinnedSkillFolder + "\\ folder (found: " +
                               script.path + "). Use the bare form Skills\\" +
                               skillName + " or the script's absolute path.";
                    return false;
                }
            }

            pathOut = script.path;
            displayNameOut = script.name;
            return true;
        }
    }

    // Friendly conversation-lane form: Scripts\helper.py pins the
    // conversation Scripts lane.  This is the lane python_create_script
    // writes to and the one models most naturally name.  Without this
    // selector the path-shaped branch below would resolve it against
    // the cwd (the Workspace folder), while the Scripts lane is the
    // Workspace's SIBLING.  The selector mirrors the Workflows\ and
    // Skills\ selectors exactly.
    {
        std::string pathish = name;
        std::replace(pathish.begin(), pathish.end(), '/', '\\');
        std::string key = LowerLocal(pathish);
        const std::string p1 = "scripts\\";

        if (key.rfind(p1, 0) == 0) {
            std::string scriptName = pathish.substr(p1.size());
            size_t ca = scriptName.find_first_not_of(" \t\r\n");
            size_t cb = scriptName.find_last_not_of(" \t\r\n");
            if (ca == std::string::npos) {
                errorOut = "python_run_script Scripts\\... path must include a script filename.";
                return false;
            }
            scriptName = scriptName.substr(ca, cb - ca + 1);

            // Prefix form is a lane selector, not an arbitrary nested path.
            if (scriptName.find('\\') != std::string::npos ||
                scriptName.find('/')  != std::string::npos ||
                scriptName.find(':')  != std::string::npos) {
                errorOut = "python_run_script Scripts\\... takes one filename "
                           "(Scripts\\<script>.py) \xE2\x80\x94 the conversation "
                           "Scripts folder is flat. For a script elsewhere on "
                           "disk, pass its absolute path.";
                return false;
            }

            size_t dot = scriptName.find_last_of('.');
            if (dot == std::string::npos) {
                scriptName += ".py";
            } else if (LowerLocal(scriptName.substr(dot)) != ".py") {
                errorOut = "python_run_script Scripts\\... paths must point to a .py file.";
                return false;
            }

            std::string safe = path_safety::SanitizeFilename(scriptName, "");
            if (safe.empty() || safe != scriptName) {
                errorOut = "Unsafe Python script filename: " + scriptName;
                return false;
            }
            if (python_arg_policy::IsReservedBuiltInPythonHelperName(scriptName)) {
                errorOut = "Reserved Python helper filename: " + scriptName +
                           ". Built-in helpers are run through their tool names, not through python_run_script.";
                return false;
            }

            const std::string scriptsRoot = UserScriptsDirForCwd(cwd);
            if (scriptsRoot.empty()) {
                errorOut = "Conversation Scripts folder could not be resolved.";
                return false;
            }
            std::string fullPath = JoinPath(scriptsRoot, scriptName);
            if (!FileExistsRegular(fullPath)) {
                errorOut = "Python script not found in the conversation Scripts folder: " +
                           scriptName;
                return false;
            }

            pathOut = fullPath;
            displayNameOut = scriptName;
            return true;
        }
    }

    const bool pathShaped =
        name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos ||
        name.find(':') != std::string::npos;

    if (pathShaped) {
        size_t dot = name.find_last_of('.');
        if (dot == std::string::npos || LowerLocal(name.substr(dot)) != ".py") {
            errorOut = "python_run_script path arguments must point to a .py file.";
            return false;
        }

        std::string resolved = ResolveToolPath(name, cwd);
        if (resolved.empty()) {
            errorOut = "Could not resolve Python script path: " + name;
            return false;
        }

        const std::string basename = PathBaseNameLocal(resolved);
        std::string safe = path_safety::SanitizeFilename(basename, "");
        if (safe.empty() || safe != basename) {
            errorOut = "Unsafe Python script filename in path: " + basename;
            return false;
        }
        if (python_arg_policy::IsReservedBuiltInPythonHelperName(basename)) {
            errorOut = "Reserved Python helper filename: " + basename +
                       ". Built-in helpers are run through their tool names, not through python_run_script.";
            return false;
        }

        const std::string scriptsRoot = UserScriptsDirForCwd(cwd);
        const std::string projectWorkflowRoot = activeProjectRoot.empty()
            ? std::string()
            : ProjectManager::ProjectWorkflowsPath(activeProjectRoot);
        const std::string skillsRoot = ProjectManager::GetSkillsDir();

        // ResolveToolPath already canonicalizes . and .. segments; use the
        // local canonicalizing comparator here too so path-shaped
        // python_run_script requests share one containment rule with Python
        // artifact attachment below.
        const bool inScripts = !scriptsRoot.empty() &&
            IsPathUnderRootForCompare(resolved, scriptsRoot);
        const bool inProjectWorkflows = !projectWorkflowRoot.empty() &&
            IsPathUnderRootForCompare(resolved, projectWorkflowRoot);
        const bool inSkills = !skillsRoot.empty() &&
            IsPathUnderRootForCompare(resolved, skillsRoot);

        if (!inScripts && !inProjectWorkflows && !inSkills) {
            // Common model recovery seam: write/overwrite_file create files
            // in the conversation Workspace (cwd), while python_run_script
            // intentionally runs only reviewed script lanes.  If the requested
            // path exists in the Workspace, say that explicitly so the model
            // repairs by recreating it with python_create_script instead of
            // repeating the same failing run.
            if (!cwd.empty() &&
                IsPathUnderRootForCompare(resolved, cwd) &&
                FileExistsRegular(resolved)) {
                errorOut = "Python script exists in the conversation Workspace, but python_run_script does not run Workspace files created by write/overwrite_file. Recreate the runnable script with python_create_script, then run python_run_script with that script filename. Workspace path: " + resolved;
                return false;
            }

            errorOut = "python_run_script only runs .py files from the conversation Scripts folder, the active project Workflows folder, or the LlamaBoss Skills folder. Use the bare filename (e.g. report.py), or pin a lane with Scripts\\name.py, Workflows\\name.py (active project), or Skills\\name.py.";
            return false;
        }
        if (!FileExistsRegular(resolved)) {
            errorOut = "Python script not found at: " + resolved;
            return false;
        }

        pathOut = resolved;
        displayNameOut = basename;
        return true;
    }

    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) {
        name += ".py";
    } else if (LowerLocal(name.substr(dot)) != ".py") {
        errorOut = "python_run_script only runs .py files from the conversation Scripts folder, the active project Workflows folder, or the LlamaBoss Skills folder. Use the bare filename (e.g. report.py), or pin a lane with Scripts\\name.py, Workflows\\name.py (active project), or Skills\\name.py.";
        return false;
    }

    // Reuse the same filename sanitizer semantics as script creation.
    std::string safe = path_safety::SanitizeFilename(name, "");
    if (safe.empty() || safe != name) {
        errorOut = "Unsafe Python script filename: " + name;
        return false;
    }

    if (python_arg_policy::IsReservedBuiltInPythonHelperName(name)) {
        errorOut = "Reserved Python helper filename: " + name +
                   ". Built-in helpers are run through their tool names, not through python_run_script. "
                   "Use a project-specific filename such as custom_" + name + " for user scripts.";
        return false;
    }

    std::string fullPath = JoinPath(UserScriptsDirForCwd(cwd), name);
    if (FileExistsRegular(fullPath)) {
        pathOut = fullPath;
        displayNameOut = name;
        return true;
    }

    if (!activeProjectRoot.empty()) {
        ProjectWorkflowScriptInfo script;
        std::string projectError;
        if (ProjectManager::ResolveProjectWorkflowScript(activeProjectRoot, name, script, projectError)) {
            pathOut = script.path;
            displayNameOut = script.name;
            return true;
        }
    }

    // Skills fallback: project scope wins on filename collision because
    // we try project resolution first; this lane runs only when the
    // script wasn't found in the conversation Scripts folder or in any
    // attached project's Workflows folder.
    {
        SkillScriptInfo script;
        std::string globalError;
        if (ProjectManager::ResolveSkillScript(name, script, globalError)) {
            pathOut = script.path;
            displayNameOut = script.name;
            return true;
        }
    }

    // If the model wrote the file with write/overwrite_file, it will be in
    // the Workspace cwd rather than the Scripts lane.  Keep the safety
    // boundary, but make the recovery action model-visible.
    {
        const std::string workspacePath = JoinPath(cwd, name);
        if (!cwd.empty() && FileExistsRegular(workspacePath)) {
            errorOut = "Python script not found in runnable script lanes. A file named " + name +
                       " exists in the conversation Workspace, but python_run_script does not run Workspace files created by write/overwrite_file. Recreate the runnable script with python_create_script " + name +
                       ", then run python_run_script with that filename. Workspace path: " + workspacePath +
                       ". Conversation Scripts path checked: " + fullPath;
            return false;
        }
    }

    errorOut = "Python script not found in this conversation's Scripts folder, the active project Workflows folder, or the LlamaBoss Skills folder. Conversation Scripts path checked: " + fullPath;
    return false;
}

bool ShouldSkipPythonArtifactScanDir(const std::string& name)
{
    std::string key = LowerLocal(name);
    return key == ".git" ||
           key == ".hg" ||
           key == ".svn" ||
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

void CollectFilesRecursive(const std::string& dir,
                           std::vector<std::string>& out,
                           size_t& seenCount,
                           size_t maxSeen)
{
    if (dir.empty() || seenCount >= maxSeen) return;

    std::wstring pattern = Utf8ToWide(JoinPath(dir, "*"));
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        if (seenCount >= maxSeen) break;

        std::wstring nameW = fd.cFileName;
        if (nameW == L"." || nameW == L"..") continue;

        std::string name = WideToUtf8(nameW);
        std::string path = JoinPath(dir, name);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (ShouldSkipPythonArtifactScanDir(name)) continue;
            CollectFilesRecursive(path, out, seenCount, maxSeen);
        } else {
            ++seenCount;
            out.push_back(path);
        }
    } while (FindNextFileW(h, &fd));

    FindClose(h);
}

std::set<std::string> SnapshotPythonRunArtifactFiles(const std::string& cwd,
                                                       const std::string& activeProjectRoot)
{
    std::set<std::string> files;

    for (const auto& root : PythonAutoArtifactScanRoots(cwd, activeProjectRoot)) {
        std::vector<std::string> paths;
        size_t seen = 0;
        CollectFilesRecursive(root, paths, seen, 2000);
        for (const auto& p : paths) files.insert(p);
    }

    return files;
}

void AttachNewFilesUnderLlamaBoss(PythonRunResult& result,
                                  const std::set<std::string>& before,
                                  const std::string& cwd,
                                  const std::string& activeProjectRoot)
{
    std::set<std::string> after = SnapshotPythonRunArtifactFiles(cwd, activeProjectRoot);

    int attached = 0;
    int extra = 0;
    size_t createdTotal = 0;
    constexpr int kMaxAttach = 12;
    constexpr size_t kMaxFileBytes = 25 * 1024 * 1024;
    constexpr size_t kMaxManifestListed = 12;
    std::string manifestLines;

    for (const auto& path : after) {
        if (before.find(path) != before.end()) continue;

        ++createdTotal;
        size_t size = FileSizeBytes(path);

        if (createdTotal <= kMaxManifestListed) {
            manifestLines += "created: " + path + "\r\n";
        }

        if (size == 0 || size > kMaxFileBytes) continue;

        if (attached >= kMaxAttach) {
            ++extra;
            continue;
        }

        AddPresentedDiskFile(result, path);
        ++attached;
    }

    if (extra > 0) {
        if (!result.stderrText.empty() && result.stderrText.back() != '\n')
            result.stderrText += "\r\n";
        result.stderrText += "[LlamaBoss detected additional created files but only attached the first " +
                             std::to_string(kMaxAttach) + " artifact cards.]\r\n";
    }

    // Workspace-change manifest, INCLUDING the explicit negative.
    // This function only runs when the script exited 0 and printed no
    // explicit ARTIFACT: line — exactly the situation where a small
    // model is most tempted to claim an output file exists anyway.
    // Stating "no new files" outright leaves it no room to confabulate
    // one (companion to the PowerShell manifest in cmd_executor.cpp).
    if (!result.stdoutText.empty() && result.stdoutText.back() != '\n')
        result.stdoutText += "\r\n";
    if (!result.stdoutText.empty()) result.stdoutText += "\r\n";
    result.stdoutText += "[workspace changes]\r\n";
    if (createdTotal == 0) {
        result.stdoutText +=
            "no new files were detected in the conversation workspace after "
            "this run. If this script was supposed to create a file in the "
            "workspace, it did not. Files written to directories outside the "
            "workspace are not tracked by this manifest; if the script's own "
            "output above names a destination path it wrote to, trust that "
            "printed path instead of spending another tool call re-checking "
            "it.\r\n";
    } else {
        result.stdoutText += manifestLines;
        if (createdTotal > kMaxManifestListed) {
            result.stdoutText += "...and " +
                std::to_string(createdTotal - kMaxManifestListed) +
                " more created file(s)\r\n";
        }
    }
}

struct HandleGuard {
    HANDLE h = nullptr;
    HandleGuard() = default;
    explicit HandleGuard(HANDLE handle) : h(handle) {}
    ~HandleGuard() {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
    HANDLE release() { HANDLE r = h; h = nullptr; return r; }
};

// Unblock a reader thread that is parked in a synchronous ReadFile.
//
// The job-object kill normally closes the last pipe writer and the
// reader returns cleanly.  It does not when something outside the job
// still holds an inherited write handle -- a grandchild the script
// spawned and detached, most often.  Without this the join() below
// never returns, the detached tool worker wedges, and because
// ToolWorkerExecutor clears m_isRunning only after that join, every
// later worker-dispatched tool in the frame is refused for the rest of
// the session.
//
// Resolved dynamically for the same reason cmd_executor.cpp does it:
// keeps the import table clean and degrades to a no-op rather than a
// load failure if the export is ever absent.
void CancelThreadSynchronousIoLocal(HANDLE threadHandle)
{
    if (!threadHandle) return;
    using Fn = BOOL (WINAPI *)(HANDLE);
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    if (!kernel) return;
    auto* fn = reinterpret_cast<Fn>(GetProcAddress(kernel, "CancelSynchronousIo"));
    if (fn) fn(threadHandle);
}

void ReaderLoop(HANDLE readEnd,
                std::string& dest,
                std::mutex& destMutex,
                std::atomic<bool>& truncatedFlag)
{
    constexpr DWORD kChunk = 4096;
    char buf[kChunk];
    for (;;) {
        DWORD got = 0;
        BOOL ok = ReadFile(readEnd, buf, kChunk, &got, nullptr);
        if (!ok || got == 0) break;

        std::lock_guard<std::mutex> lk(destMutex);
        if (dest.size() < PythonRunner::kMaxOutputBytes) {
            size_t room = PythonRunner::kMaxOutputBytes - dest.size();
            size_t take = std::min<size_t>(got, room);
            dest.append(buf, take);
            if (take < got) truncatedFlag.store(true);
        } else {
            truncatedFlag.store(true);
        }
    }
}

std::wstring QuoteArg(const std::wstring& arg)
{
    // Full Windows CRT argv quoting.  The old minimal wrapper escaped
    // quotes but did not double trailing backslashes.  A rare argument
    // ending in \ could escape the closing quote and merge argv items.
    std::wstring out = L"\"";
    size_t backslashes = 0;

    for (wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
        } else if (ch == L'\"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'\"');
            backslashes = 0;
        } else {
            out.append(backslashes, L'\\');
            backslashes = 0;
            out.push_back(ch);
        }
    }

    out.append(backslashes * 2, L'\\');
    out.push_back(L'\"');
    return out;
}

struct PythonCandidate {
    std::string label;
    std::wstring commandLine;
};

std::vector<PythonCandidate> BuildCandidates(const std::string& scriptPath,
                                             const std::vector<std::string>& helperArgs)
{
    std::wstring script = QuoteArg(Utf8ToWide(scriptPath));
    std::wstring argText;
    for (const std::string& a : helperArgs) {
        argText += L" ";
        argText += QuoteArg(Utf8ToWide(a));
    }

    // -B  : skip writing .pyc files so the bundled helper directory stays
    //       clean across runs.
    // -u  : unbuffer stdout/stderr so timeout/cancel failures still preserve
    //       the latest useful Python output instead of losing buffered text.
    //
    // We deliberately do NOT pass -I (isolated mode).  -I implies -s, which
    // disables the per-user site-packages directory.  python_install_package
    // installs user-approved dependencies via `pip install --user`, so the
    // wheels land in %APPDATA%\Python\Python3xx\site-packages.  Under -I,
    // helpers cannot import packages they themselves told the user to
    // install -- the missing-package recovery flow would loop forever.
    //
    // The helpers are bundled trusted scripts written by LlamaBoss, not
    // arbitrary model code, so the loss of isolation is acceptable; the only
    // realistic threat is PYTHONPATH redirection by an attacker who already
    // has write access to the user's environment, which is a strictly
    // bigger compromise than what -I would have prevented.
    // python_run_script (user/model-authored scripts) also runs without -I.
    // -X utf8 : force Python stdio/text handling to use UTF-8 even on
    //           Windows systems whose inherited process code page is still
    //           legacy ANSI.  This prevents smart punctuation copied from
    //           PDFs/DOCX files from leaving Python as non-UTF-8 bytes.
    const std::wstring pyFlags     = L" -3 -X utf8 -B -u ";
    const std::wstring pythonFlags = L" -X utf8 -B -u ";

    return {
        { "py -3",   L"py.exe" + pyFlags + script + argText },
        { "python",  L"python.exe" + pythonFlags + script + argText },
        { "python3", L"python3.exe" + pythonFlags + script + argText },
    };
}

std::vector<PythonCandidate> BuildPipInstallCandidates(const std::string& packageName)
{
    const std::wstring pkg = QuoteArg(Utf8ToWide(packageName));
    const std::wstring pyCommon = L" -3 -X utf8 -B -m pip install --user --disable-pip-version-check " + pkg;
    const std::wstring common   = L" -X utf8 -B -m pip install --user --disable-pip-version-check " + pkg;

    return {
        { "py -3",   L"py.exe" + pyCommon },
        { "python",  L"python.exe" + common },
        { "python3", L"python3.exe" + common },
    };
}

class PythonWorkerThread : public wxThread {
public:
    PythonWorkerThread(wxEvtHandler* evtHandler,
                       const std::string& helperName,
                       const std::string& helperArg,
                       const std::string& cwd,
                       unsigned long      timeoutMs,
                       std::string        activeProjectRoot,
                       std::shared_ptr<std::atomic<bool>> cancelFlag,
                       std::shared_ptr<std::atomic<bool>> runningFlag,
                       std::weak_ptr<std::atomic<bool>> aliveToken,
                       std::vector<std::pair<std::string, std::string>> envInjections = {})
        : wxThread(wxTHREAD_DETACHED)
        , m_evtHandler(evtHandler)
        , m_helperName(helperName)
        , m_helperArg(helperArg)
        , m_cwd(cwd)
        , m_timeoutMs(timeoutMs ? timeoutMs : PythonRunner::kDefaultTimeoutMs)
        , m_activeProjectRoot(std::move(activeProjectRoot))
        , m_cancelFlag(std::move(cancelFlag))
        , m_runningFlag(std::move(runningFlag))
        , m_aliveToken(std::move(aliveToken))
        , m_envInjections(std::move(envInjections))
    {}

protected:
    ExitCode Entry() override
    {
        PythonRunResult result;
        result.toolName    = m_helperName;
        result.helperName  = m_helperName;
        result.commandEcho = m_helperName;

        double t0 = NowSec();
        RunOne(result);
        if (m_helperName == "python_run_script") {
            ApplyLargePythonRunOutputHandling(result, m_cwd);
        }
        result.elapsedSec = NowSec() - t0;

        if (m_runningFlag) m_runningFlag->store(false);
        PostCompletion(std::move(result));
        return (ExitCode)0;
    }

private:
    void RunOne(PythonRunResult& result)
    {
        std::string scriptPath, scriptError;
        std::vector<std::string> helperArgs;
        // Must live outside the python_run_script branch because the
        // post-run argparse recovery hint is emitted after the process exits.
        std::vector<std::string> pythonRunScriptArgvForHint;
        std::vector<std::string> tempArgFiles;
        auto cleanupTempArgFiles = [&]() {
            for (const auto& path : tempArgFiles) {
                DeleteFileW(Utf8ToWide(path).c_str());
            }
            tempArgFiles.clear();
        };

        const bool isPackageInstall = (m_helperName == "python_install_package");
        std::set<std::string> beforeFiles;

        // Fixed helpers: write the embedded script once, up front; the
        // branches below only build each helper's argv.
        if (lb_pyres::IsHelperName(m_helperName) &&
            !EnsureBuiltinHelperScript(m_helperName, scriptPath, scriptError)) {
            result.stderrText = scriptError;
            result.exitCode = -1;
            return;
        }

        if (m_helperName == "python_install_package") {
            std::string packageName;
            if (!python_arg_policy::NormalizeAllowedPythonPackage(m_helperArg, packageName, scriptError)) {
                result.stderrText = scriptError;
                result.exitCode = -1;
                return;
            }
            helperArgs.push_back(packageName);
            result.commandEcho = "python_install_package " + packageName;
        } else if (m_helperName == "python_health") {
            result.commandEcho = "python_health";
        } else if (m_helperName == "csv_inspect") {
            helperArgs.push_back(m_helperArg);
            result.commandEcho = m_helperArg.empty()
                ? std::string("csv_inspect")
                : std::string("csv_inspect ") + m_helperArg;
        } else if (m_helperName == "csv_report") {
            helperArgs.push_back(m_helperArg);
            helperArgs.push_back(DocumentsDirForCwd(m_cwd));
            result.commandEcho = m_helperArg.empty()
                ? std::string("csv_report")
                : std::string("csv_report ") + m_helperArg;
        } else if (m_helperName == "csv_to_xlsx") {
            helperArgs.push_back(m_helperArg);
            helperArgs.push_back(SpreadsheetsDirForCwd(m_cwd));
            result.commandEcho = m_helperArg.empty()
                ? std::string("csv_to_xlsx")
                : std::string("csv_to_xlsx ") + m_helperArg;
        } else if (m_helperName == "xlsx_create_workbook") {
            std::string specArg = m_helperArg;
            if (specArg.size() > 24 * 1024) {
                std::string argFile;
                if (!WriteUtf8ArgTempFile("xlsx_create_workbook_spec", specArg, argFile, scriptError)) {
                    result.stderrText = scriptError;
                    result.exitCode = -1;
                    cleanupTempArgFiles();
                    return;
                }
                tempArgFiles.push_back(argFile);
                specArg = "@" + argFile;
            }
            helperArgs.push_back(specArg);
            helperArgs.push_back(SpreadsheetsDirForCwd(m_cwd));
            result.commandEcho = "xlsx_create_workbook";
        } else if (m_helperName == "xlsx_inspect") {
            helperArgs.push_back(m_helperArg);
            result.commandEcho = m_helperArg.empty()
                ? std::string("xlsx_inspect")
                : std::string("xlsx_inspect ") + m_helperArg;
        } else if (m_helperName == "xlsx_report") {
            helperArgs.push_back(m_helperArg);
            helperArgs.push_back(DocumentsDirForCwd(m_cwd));
            result.commandEcho = m_helperArg.empty()
                ? std::string("xlsx_report")
                : std::string("xlsx_report ") + m_helperArg;
        } else if (m_helperName == "pdf_extract_text") {
            helperArgs.push_back(m_helperArg);
            helperArgs.push_back(PdfsDirForCwd(m_cwd));
            result.commandEcho = m_helperArg.empty()
                ? std::string("pdf_extract_text")
                : std::string("pdf_extract_text ") + m_helperArg;
        } else if (m_helperName == "pdf_inspect_form") {
            helperArgs.push_back(m_helperArg);
            // No output directory argument: pdf_inspect_form is a
            // read-only inspector, sibling to csv_inspect/xlsx_inspect.
            // It emits JSON to stdout and produces no artifact file.
            result.commandEcho = m_helperArg.empty()
                ? std::string("pdf_inspect_form")
                : std::string("pdf_inspect_form ") + m_helperArg;
        } else if (m_helperName == "pdf_fill_form") {
            // Multi-line args contract: first line is the input PDF
            // path, remaining lines are a JSON {field: value} object.
            // Splitting here keeps the helper's argv shape clean
            // (path, json, out_dir) and preserves the established
            // first-line-is-path convention used by /write and /edit.
            std::string blob = m_helperArg;
            size_t nl = blob.find('\n');
            std::string pathPart;
            std::string jsonPart;
            if (nl == std::string::npos) {
                pathPart = blob;
                jsonPart = "";
            } else {
                pathPart = blob.substr(0, nl);
                jsonPart = blob.substr(nl + 1);
            }
            // Trim CR from CRLF endings on Windows-pasted content.
            while (!pathPart.empty() &&
                   (pathPart.back() == '\r' || pathPart.back() == ' ' ||
                    pathPart.back() == '\t'))
                pathPart.pop_back();
            // Leading whitespace on path
            size_t ps = pathPart.find_first_not_of(" \t");
            if (ps != std::string::npos && ps > 0)
                pathPart = pathPart.substr(ps);

            if (jsonPart.size() > 24 * 1024) {
                std::string argFile;
                if (!WriteUtf8ArgTempFile("pdf_fill_form_fields", jsonPart, argFile, scriptError)) {
                    result.stderrText = scriptError;
                    result.exitCode = -1;
                    cleanupTempArgFiles();
                    return;
                }
                tempArgFiles.push_back(argFile);
                jsonPart = "@" + argFile;
            }

            helperArgs.push_back(pathPart);
            helperArgs.push_back(jsonPart);
            helperArgs.push_back(FilledFormsDirForCwd(m_cwd));

            result.commandEcho = pathPart.empty()
                ? std::string("pdf_fill_form")
                : std::string("pdf_fill_form ") + pathPart;
        } else if (m_helperName == "docx_extract_text") {
            helperArgs.push_back(m_helperArg);
            helperArgs.push_back(WordDirForCwd(m_cwd));
            result.commandEcho = m_helperArg.empty()
                ? std::string("docx_extract_text")
                : std::string("docx_extract_text ") + m_helperArg;
        } else if (m_helperName == "docx_inspect") {
            helperArgs.push_back(m_helperArg);
            // No output directory argument: docx_inspect is read-only,
            // sibling to pdf_inspect_form / xlsx_inspect / csv_inspect.
            result.commandEcho = m_helperArg.empty()
                ? std::string("docx_inspect")
                : std::string("docx_inspect ") + m_helperArg;
        } else if (m_helperName == "zip_inspect") {
            helperArgs.push_back(m_helperArg);
            // No output directory argument: zip_inspect is read-only,
            // sibling to docx_inspect / pdf_inspect_form / csv_inspect.
            result.commandEcho = m_helperArg.empty()
                ? std::string("zip_inspect")
                : std::string("zip_inspect ") + m_helperArg;
        } else if (m_helperName == "zip_extract") {
            helperArgs.push_back(m_helperArg);
            // Output directory: the conversation "Extracted" lane.  The
            // helper creates a per-archive subfolder under it and confines
            // every extracted entry beneath that subfolder.
            helperArgs.push_back(ExtractedDirForCwd(m_cwd));
            result.commandEcho = m_helperArg.empty()
                ? std::string("zip_extract")
                : std::string("zip_extract ") + m_helperArg;
        } else if (m_helperName == "python_run_script") {
            std::string scriptRequest;
            SplitPythonRunScriptInvocation(m_helperArg, scriptRequest, pythonRunScriptArgvForHint);

            std::string displayName;
            if (!ResolveRunnableScriptPath(scriptRequest, m_cwd, m_activeProjectRoot, scriptPath, displayName, scriptError)) {
                result.stderrText = scriptError;
                result.exitCode = -1;
                return;
            }

            helperArgs.insert(helperArgs.end(),
                              pythonRunScriptArgvForHint.begin(),
                              pythonRunScriptArgvForHint.end());
            result.commandEcho = displayName.empty()
                ? std::string("python_run_script")
                : std::string("python_run_script ") + displayName;
            if (!pythonRunScriptArgvForHint.empty()) {
                result.commandEcho += "  ·  " + std::to_string(pythonRunScriptArgvForHint.size()) +
                    (pythonRunScriptArgvForHint.size() == 1 ? " arg" : " args");
            }
            beforeFiles = SnapshotPythonRunArtifactFiles(m_cwd, m_activeProjectRoot);
        } else {
            result.stderrText = "Unknown built-in Python helper/script runner: " + m_helperName;
            result.exitCode = -1;
            return;
        }

        std::string cwd = m_cwd.empty() ? ServerManager::GetWorkspaceDir() : m_cwd;
        std::wstring wCwd = Utf8ToWide(cwd);
        LPCWSTR cwdArg = wCwd.empty() ? nullptr : wCwd.c_str();

        std::vector<PythonCandidate> candidates = isPackageInstall
            ? BuildPipInstallCandidates(helperArgs.empty() ? std::string() : helperArgs.front())
            : BuildCandidates(scriptPath, helperArgs);
        std::ostringstream startErrors;

        for (const PythonCandidate& c : candidates) {
            PythonRunResult attempt = result;
            attempt.pythonCommand = c.label;

            std::wstring mutableCmd = c.commandLine;
            std::vector<wchar_t> cmdBuf(mutableCmd.begin(), mutableCmd.end());
            cmdBuf.push_back(L'\0');

            if (RunProcess(cmdBuf.data(), cwdArg, attempt, startErrors)) {
                if (m_helperName == "csv_report") {
                    AttachCsvReportArtifact(attempt);
                } else if (m_helperName == "csv_to_xlsx" ||
                           m_helperName == "xlsx_create_workbook") {
                    AttachXlsxWorkbookArtifact(attempt);
                } else if (m_helperName == "xlsx_report") {
                    AttachXlsxReportArtifact(attempt);
                } else if (m_helperName == "pdf_extract_text") {
                    AttachPdfExtractTextArtifact(attempt);
                } else if (m_helperName == "pdf_fill_form") {
                    AttachPdfFillFormArtifact(attempt);
                } else if (m_helperName == "docx_extract_text") {
                    AttachDocxExtractTextArtifact(attempt);
                } else if (m_helperName == "python_run_script") {
                    const bool completedSuccessfully =
                        attempt.exitCode == 0 && !attempt.timedOut && !attempt.cancelled;
                    if (completedSuccessfully &&
                        !AttachExplicitPythonRunArtifacts(attempt, m_cwd, m_activeProjectRoot)) {
                        AttachNewFilesUnderLlamaBoss(attempt, beforeFiles, m_cwd, m_activeProjectRoot);
                    }

                    // Deterministic recovery hint for the most common argv
                    // mistake: a flag and its value on ONE args line, which
                    // the one-token-per-line contract passes to the script as
                    // a single argv element ("--format-index 1").  argparse
                    // then rejects the call with a usage error the model
                    // cannot diagnose, and small models retry the identical
                    // call until the loop guard stops the agent.  Name the
                    // exact bad token and both accepted fixes so one retry
                    // can succeed.
                    if (!completedSuccessfully &&
                        !attempt.cancelled && !attempt.timedOut) {
                        std::string suspicious;
                        for (const std::string& tok : pythonRunScriptArgvForHint) {
                            if (tok.size() > 1 && tok[0] == '-' &&
                                tok.find_first_of(" \t") != std::string::npos) {
                                suspicious = tok;
                                break;
                            }
                        }
                        if (!suspicious.empty()) {
                            if (!attempt.stderrText.empty() &&
                                attempt.stderrText.back() != '\n') {
                                attempt.stderrText += "\n";
                            }
                            attempt.stderrText +=
                                "[hint] Each python_run_script args line is passed to the "
                                "script as ONE argv token. The line \"" + suspicious +
                                "\" became a single token, which argument parsers reject. "
                                "Put the flag and its value on separate lines, or write "
                                "it as one token with '=' (e.g. --flag=value).";
                        }
                    }
                }
                result = std::move(attempt);
                cleanupTempArgFiles();
                return;
            }
        }

        cleanupTempArgFiles();
        result.stderrText =
            "Could not start Python. Tried py -3, python, and python3.\n" +
            startErrors.str();
        result.exitCode = -1;
    }

    bool RunProcess(wchar_t* cmdLine,
                    LPCWSTR cwdArg,
                    PythonRunResult& result,
                    std::ostringstream& startErrors)
    {
        constexpr size_t kMaxWindowsCommandLineChars = 32767;
        size_t cmdChars = cmdLine ? std::wcslen(cmdLine) : 0;
        if (cmdChars >= kMaxWindowsCommandLineChars) {
            startErrors << result.pythonCommand
                        << ": command line too long (" << cmdChars
                        << " UTF-16 chars). Large helper JSON is normally passed through a temp file; "
                           "if this was python_run_script, reduce argv size or write arguments to a file.\n";
            return false;
        }

        SECURITY_ATTRIBUTES sa = {};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        sa.lpSecurityDescriptor = nullptr;

        HANDLE outR_raw = nullptr, outW_raw = nullptr;
        HANDLE errR_raw = nullptr, errW_raw = nullptr;
        if (!CreatePipe(&outR_raw, &outW_raw, &sa, 0)) {
            result.stderrText = "CreatePipe(stdout) failed, error=" +
                                std::to_string(GetLastError());
            result.exitCode = -1;
            return true; // fatal infrastructure error; do not try candidates
        }
        HandleGuard outR(outR_raw), outW(outW_raw);
        if (!CreatePipe(&errR_raw, &errW_raw, &sa, 0)) {
            result.stderrText = "CreatePipe(stderr) failed, error=" +
                                std::to_string(GetLastError());
            result.exitCode = -1;
            return true;
        }
        HandleGuard errR(errR_raw), errW(errW_raw);

        SetHandleInformation(outR.h, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(errR.h, HANDLE_FLAG_INHERIT, 0);

        HandleGuard job(CreateJobObjectW(nullptr, nullptr));
        if (!job.h) {
            result.stderrText = "CreateJobObject failed, error=" +
                                std::to_string(GetLastError());
            result.exitCode = -1;
            return true;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job.h,
                                     JobObjectExtendedLimitInformation,
                                     &jeli,
                                     sizeof(jeli))) {
            result.stderrText = "SetInformationJobObject failed, error=" +
                                std::to_string(GetLastError());
            result.exitCode = -1;
            return true;
        }

        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput  = nullptr;
        si.hStdOutput = outW.h;
        si.hStdError  = errW.h;

        // ── Env-var injection for skill scripts ──────────────────
        // Build a UTF-16 environment block when m_envInjections has
        // entries (only python_run_script populates this).  The
        // block starts as a copy of our own process environment so
        // PATH / PYTHONHOME / SystemRoot etc. are preserved, then
        // each injected NAME=VALUE entry is merged in.  The explicit
        // block is rebuilt in a stable sorted order and duplicate
        // variable names are replaced rather than appended.
        //
        // For built-in helpers (csv_inspect, xlsx_report, etc.)
        // m_envInjections is empty and we pass nullptr — the child
        // simply inherits our environment as before.
        std::vector<wchar_t> envBlock;
        void*  envPtr   = nullptr;
        DWORD  envFlags = 0;
        if (!m_envInjections.empty()) {
            std::vector<std::wstring> envEntries;

            LPWCH parentEnv = GetEnvironmentStringsW();
            if (parentEnv) {
                // Copy parent entries one by one.  Walk until the
                // double-null sentinel so we know its true length.
                LPWCH p = parentEnv;
                while (*p) {
                    size_t len = wcslen(p);
                    envEntries.emplace_back(p, len);
                    p += len + 1;
                }
                FreeEnvironmentStringsW(parentEnv);
            }

            auto envNameFromEntry = [](const std::wstring& entry) {
                // Normal entries are NAME=VALUE.  Windows can also
                // carry hidden drive-current-directory entries such
                // as =C:=C:\Path; for those, the separator is the
                // second '=' rather than the first character.
                const size_t searchFrom =
                    (!entry.empty() && entry.front() == L'=') ? 1u : 0u;
                const size_t eq = entry.find(L'=', searchFrom);
                return (eq == std::wstring::npos) ? entry : entry.substr(0, eq);
            };

            auto sameEnvName = [&](const std::wstring& entry,
                                   const std::wstring& wantedName) {
                const std::wstring existingName = envNameFromEntry(entry);
                return CompareStringOrdinal(existingName.c_str(),
                                            static_cast<int>(existingName.size()),
                                            wantedName.c_str(),
                                            static_cast<int>(wantedName.size()),
                                            TRUE) == CSTR_EQUAL;
            };

            // Merge/replace each injected variable rather than leaving
            // duplicate names in the child environment.
            for (const auto& kv : m_envInjections) {
                std::wstring name = path_safety::Utf8ToWide(kv.first);
                if (name.empty()) continue;

                envEntries.erase(
                    std::remove_if(envEntries.begin(), envEntries.end(),
                                   [&](const std::wstring& entry) {
                                       return sameEnvName(entry, name);
                                   }),
                    envEntries.end());

                std::wstring entry = std::move(name);
                entry.push_back(L'=');
                entry.append(path_safety::Utf8ToWide(kv.second));
                envEntries.push_back(std::move(entry));
            }

            // Windows expects an explicit environment block to be
            // sorted case-insensitively.  CompareStringOrdinal(...,
            // TRUE) gives us a locale-independent Windows comparison.
            std::sort(envEntries.begin(), envEntries.end(),
                      [&](const std::wstring& a, const std::wstring& b) {
                          const std::wstring nameA = envNameFromEntry(a);
                          const std::wstring nameB = envNameFromEntry(b);
                          const int nameCmp =
                              CompareStringOrdinal(nameA.c_str(),
                                                   static_cast<int>(nameA.size()),
                                                   nameB.c_str(),
                                                   static_cast<int>(nameB.size()),
                                                   TRUE);
                          if (nameCmp != CSTR_EQUAL)
                              return nameCmp == CSTR_LESS_THAN;

                          return CompareStringOrdinal(a.c_str(),
                                                      static_cast<int>(a.size()),
                                                      b.c_str(),
                                                      static_cast<int>(b.size()),
                                                      TRUE) == CSTR_LESS_THAN;
                      });

            for (const auto& entry : envEntries) {
                envBlock.insert(envBlock.end(), entry.begin(), entry.end());
                envBlock.push_back(L'\0');
            }
            // Final sentinel.
            envBlock.push_back(L'\0');

            envPtr   = envBlock.data();
            envFlags = CREATE_UNICODE_ENVIRONMENT;
        }

        PROCESS_INFORMATION pi = {};
        BOOL ok = CreateProcessW(
            nullptr,
            cmdLine,
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED | envFlags,
            envPtr,
            cwdArg,
            &si,
            &pi);

        if (!ok) {
            DWORD err = GetLastError();
            startErrors << result.pythonCommand << ": CreateProcess failed, error="
                        << err << "\n";
            return false; // try next launcher candidate
        }

        HandleGuard proc(pi.hProcess);
        HandleGuard thr(pi.hThread);

        if (!AssignProcessToJobObject(job.h, proc.h)) {
            DWORD err = GetLastError();
            TerminateProcess(proc.h, 1);
            result.stderrText = "AssignProcessToJobObject failed, error=" +
                                std::to_string(err);
            result.exitCode = -1;
            return true;
        }

        // An unchecked ResumeThread left the child suspended, which then
        // burned the entire m_timeoutMs before surfacing as a generic
        // "timed out" -- hiding the real failure.  Match server_manager's
        // handling and report it directly.
        if (ResumeThread(thr.h) == (DWORD)-1) {
            DWORD err = GetLastError();
            TerminateProcess(proc.h, 1);
            result.stderrText = "ResumeThread failed, error=" +
                                std::to_string(err);
            result.exitCode = -1;
            return true;
        }

        CloseHandle(outW.release());
        CloseHandle(errW.release());

        std::mutex outMu, errMu;
        std::atomic<bool> truncated(false);
        std::thread outThread(ReaderLoop,
            outR.h,
            std::ref(result.stdoutText),
            std::ref(outMu),
            std::ref(truncated));
        std::thread errThread(ReaderLoop,
            errR.h,
            std::ref(result.stderrText),
            std::ref(errMu),
            std::ref(truncated));

        constexpr DWORD kTickMs = 100;
        double deadline = NowSec() + (m_timeoutMs / 1000.0);
        bool killIt = false;

        for (;;) {
            DWORD wr = WaitForSingleObject(proc.h, kTickMs);
            if (wr == WAIT_OBJECT_0) break;
            if (wr == WAIT_FAILED) { killIt = true; break; }

            if (m_cancelFlag && m_cancelFlag->load()) {
                result.cancelled = true;
                killIt = true;
                break;
            }
            if (NowSec() >= deadline) {
                result.timedOut = true;
                killIt = true;
                break;
            }
        }

        if (killIt) {
            // Closing the job handle triggers KILL_ON_JOB_CLOSE across
            // the whole process tree.
            CloseHandle(job.release());
            // Give the OS a beat to deliver the kill so the readers see
            // EOF and unblock on their own.
            DWORD killed = WaitForSingleObject(proc.h, 2000);
            if (killed == WAIT_TIMEOUT) {
                // A stubborn process or a leaked writer is still holding a
                // pipe open.  Cancel the in-flight ReadFile calls so the
                // joins below cannot park this detached worker forever.
                if (outThread.joinable())
                    CancelThreadSynchronousIoLocal(outThread.native_handle());
                if (errThread.joinable())
                    CancelThreadSynchronousIoLocal(errThread.native_handle());
            }
        }

        if (outThread.joinable()) outThread.join();
        if (errThread.joinable()) errThread.join();

        // Guard the chat/model request boundary.  Helpers are supposed to
        // return UTF-8, but Windows subprocess output is not trustworthy
        // enough to let malformed bytes flow straight into llama-server JSON.
        result.stdoutText = NormalizeProcessOutputUtf8(result.stdoutText);
        result.stderrText = NormalizeProcessOutputUtf8(result.stderrText);

        // tqdm/pip/huggingface_hub progress bars redraw with bare '\r' on
        // stderr.  Collapse to the final frame; progress-only stderr moves
        // to a one-line stdout summary.  Before the breadcrumbs below.
        lb_progressfold::TidyCapturedStreams(result.stdoutText, result.stderrText);

        DWORD code = 0;
        if (GetExitCodeProcess(proc.h, &code)) {
            if (code == STILL_ACTIVE) {
                result.exitCode = -1;
                if (!result.stderrText.empty() && result.stderrText.back() != '\n')
                    result.stderrText += "\r\n";
                result.stderrText += "[process did not exit after the kill request]\r\n";
            } else {
                result.exitCode = static_cast<int>(code);
            }
        } else {
            result.exitCode = -1;
        }

        result.truncated = truncated.load();

        if (result.cancelled && result.stderrText.empty())
            result.stderrText = "[cancelled by user]\r\n";
        else if (result.timedOut && result.stderrText.empty())
            result.stderrText =
                "[timed out after " + std::to_string(m_timeoutMs / 1000) +
                "s" +
                (result.stdoutText.empty()
                     ? " with no output. The script printed nothing before "
                       "the deadline. When scanning or zipping directories, "
                       "exclude build/IDE folders such as .vs, .git, x64, "
                       "Debug, Release, node_modules, __pycache__, and print "
                       "progress as the script works so partial output "
                       "survives a timeout"
                     : "") +
                "]\r\n";

        return true;
    }

    void PostCompletion(PythonRunResult result)
    {
        auto* ev = new wxCommandEvent(wxEVT_PYTHON_COMPLETE);
        ev->SetClientObject(new PythonRunResultClientData(std::move(result)));
        LbQueueEventIfAlive(m_evtHandler, m_aliveToken, ev);
    }

    wxEvtHandler*                      m_evtHandler;
    std::string                        m_helperName;
    std::string                        m_helperArg;
    std::string                        m_cwd;
    unsigned long                      m_timeoutMs;
    std::string                        m_activeProjectRoot;
    std::shared_ptr<std::atomic<bool>> m_cancelFlag;
    std::shared_ptr<std::atomic<bool>> m_runningFlag;
    std::weak_ptr<std::atomic<bool>>   m_aliveToken;

    // Environment variables to inject into the child process.  Only
    // populated for python_run_script (the one helper that runs
    // user-authored skill scripts).  Empty for every built-in
    // helper, so csv_inspect / xlsx_report / pdf_extract_text etc.
    // never see API keys.
    std::vector<std::pair<std::string, std::string>> m_envInjections;
};

} // namespace
#endif // _WIN32

PythonRunner::PythonRunner(wxEvtHandler* eventHandler,
                           std::weak_ptr<std::atomic<bool>> aliveToken)
    : m_eventHandler(eventHandler)
    , m_aliveToken(std::move(aliveToken))
    , m_cancelFlag(std::make_shared<std::atomic<bool>>(false))
    , m_isRunning(std::make_shared<std::atomic<bool>>(false))
{}

PythonRunner::~PythonRunner()
{
    if (m_cancelFlag) m_cancelFlag->store(true);
}

bool PythonRunner::StartWorker(const std::string& helperName,
                               const std::string& helperArg,
                               const std::string& cwd,
                               unsigned long      timeoutMs,
                               unsigned long      defaultTimeoutMs,
                               const std::string& activeProjectRoot)
{
#ifndef _WIN32
    // macOS: Python helpers arrive in Phase 3.  Complete asynchronously
    // with a clear message so the tool card and agent loop finish normally.
    (void)helperArg; (void)cwd; (void)timeoutMs; (void)defaultTimeoutMs; (void)activeProjectRoot;
    PythonRunResult r;
    r.toolName = helperName;
    r.helperName = helperName;
    r.stderrText = "Python tools are not available on macOS yet.";
    r.exitCode = -1;
    auto* ev = new wxCommandEvent(wxEVT_PYTHON_COMPLETE);
    ev->SetClientObject(new PythonRunResultClientData(std::move(r)));
    LbQueueEventIfAlive(m_eventHandler, m_aliveToken, ev);
    return true;
#else
    bool expected = false;
    if (!m_isRunning->compare_exchange_strong(expected, true)) {
        return false;
    }

    m_cancelFlag->store(false);

    // Per-tool fallback wins over the worker's own kDefaultTimeoutMs
    // when both are non-zero.  Caller's explicit timeoutMs always
    // wins over both when it's non-zero.
    const unsigned long effectiveTimeout =
        timeoutMs ? timeoutMs : defaultTimeoutMs;

    // Build the env-var injection snapshot on the UI thread.  Only
    // python_run_script (the one helper that runs user-authored
    // skill scripts) sees Connections; every built-in helper gets
    // an empty list so API keys never leak into csv/xlsx/pdf paths.
    std::vector<std::pair<std::string, std::string>> envInjections;
    if (helperName == "python_run_script" && m_secretsStore) {
        envInjections = m_secretsStore->BuildEnvInjections();
    }

    auto* worker = new PythonWorkerThread(
        m_eventHandler,
        helperName,
        helperArg,
        cwd,
        effectiveTimeout,
        activeProjectRoot,
        m_cancelFlag,
        m_isRunning,
        m_aliveToken,
        std::move(envInjections));

    if (worker->Run() != wxTHREAD_NO_ERROR) {
        delete worker;
        m_isRunning->store(false);

        auto* ev = new wxCommandEvent(wxEVT_PYTHON_ERROR);
        ev->SetString("Failed to start Python worker thread.");
        LbQueueEventIfAlive(m_eventHandler, m_aliveToken, ev);
        return false;
    }

    return true;
#endif
}

bool PythonRunner::StartHealth(const std::string& cwd,
                               unsigned long      timeoutMs)
{
    return StartWorker("python_health", std::string(), cwd, timeoutMs);
}

bool PythonRunner::StartCsvInspect(const std::string& pathArg,
                                   const std::string& cwd,
                                   unsigned long      timeoutMs)
{
    return StartWorker("csv_inspect", pathArg, cwd, timeoutMs);
}

bool PythonRunner::StartCsvReport(const std::string& pathArg,
                                  const std::string& cwd,
                                  unsigned long      timeoutMs)
{
    return StartWorker("csv_report", pathArg, cwd, timeoutMs);
}

bool PythonRunner::StartCsvToXlsx(const std::string& pathArg,
                                  const std::string& cwd,
                                  unsigned long      timeoutMs)
{
    return StartWorker("csv_to_xlsx", pathArg, cwd, timeoutMs);
}


bool PythonRunner::StartXlsxInspect(const std::string& pathArg,
                                    const std::string& cwd,
                                    unsigned long      timeoutMs)
{
    return StartWorker("xlsx_inspect", pathArg, cwd, timeoutMs);
}

bool PythonRunner::StartXlsxReport(const std::string& pathArg,
                                   const std::string& cwd,
                                   unsigned long      timeoutMs)
{
    return StartWorker("xlsx_report", pathArg, cwd, timeoutMs);
}

bool PythonRunner::StartXlsxCreateWorkbook(const std::string& jsonSpecArg,
                                           const std::string& cwd,
                                           unsigned long      timeoutMs)
{
    // Workbook creation can be a little slower than inspection for
    // larger tables, so allow a slightly longer helper default.
    return StartWorker("xlsx_create_workbook", jsonSpecArg, cwd, timeoutMs, 30000);
}


bool PythonRunner::StartPdfExtractText(const std::string& pathArg,
                                       const std::string& cwd,
                                       unsigned long      timeoutMs)
{
    return StartWorker("pdf_extract_text", pathArg, cwd, timeoutMs);
}

bool PythonRunner::StartPdfInspectForm(const std::string& pathArg,
                                       const std::string& cwd,
                                       unsigned long      timeoutMs)
{
    return StartWorker("pdf_inspect_form", pathArg, cwd, timeoutMs);
}

bool PythonRunner::StartPdfFillForm(const std::string& argsBlob,
                                    const std::string& cwd,
                                    unsigned long      timeoutMs)
{
    return StartWorker("pdf_fill_form", argsBlob, cwd, timeoutMs);
}

bool PythonRunner::StartDocxExtractText(const std::string& pathArg,
                                        const std::string& cwd,
                                        unsigned long      timeoutMs)
{
    return StartWorker("docx_extract_text", pathArg, cwd, timeoutMs);
}

bool PythonRunner::StartDocxInspect(const std::string& pathArg,
                                    const std::string& cwd,
                                    unsigned long      timeoutMs)
{
    return StartWorker("docx_inspect", pathArg, cwd, timeoutMs);
}

bool PythonRunner::StartZipInspect(const std::string& pathArg,
                                   const std::string& cwd,
                                   unsigned long      timeoutMs)
{
    return StartWorker("zip_inspect", pathArg, cwd, timeoutMs);
}

bool PythonRunner::StartZipExtract(const std::string& pathArg,
                                   const std::string& cwd,
                                   unsigned long      timeoutMs)
{
    return StartWorker("zip_extract", pathArg, cwd, timeoutMs);
}

bool PythonRunner::StartPythonRunScript(const std::string& scriptArg,
                                        const std::string& cwd,
                                        unsigned long      timeoutMs,
                                        const std::string& activeProjectRoot)
{
    return StartWorker("python_run_script", scriptArg, cwd, timeoutMs,
                       /*defaultTimeoutMs*/ 30000,
                       activeProjectRoot);
}

bool PythonRunner::StartPythonInstallPackage(const std::string& packageArg,
                                             const std::string& cwd,
                                             unsigned long      timeoutMs)
{
    if (IsRunning()) return false;

    // Pre-flight validation runs on the calling thread.  Rejecting an
    // unsafe package name here means we never spin up a worker for it
    // and we surface a synthetic completion event so the agent loop /
    // slash UI can render the failure card the same way it would
    // render a completed install with a non-zero exit code.
    std::string packageName;
    std::string err;
    if (!python_arg_policy::NormalizeAllowedPythonPackage(packageArg, packageName, err)) {
        PythonRunResult r;
        r.toolName    = "python_install_package";
        r.helperName  = "python_install_package";
        r.commandEcho = "python_install_package";
        r.stderrText  = err;
        r.exitCode    = -1;
        auto* ev = new wxCommandEvent(wxEVT_PYTHON_COMPLETE);
        ev->SetClientObject(new PythonRunResultClientData(std::move(r)));
        LbQueueEventIfAlive(m_eventHandler, m_aliveToken, ev);
        return true;
    }

    return StartWorker("python_install_package", packageName, cwd, timeoutMs,
                       /*defaultTimeoutMs*/ 300000);
}

void PythonRunner::Cancel()
{
    if (m_cancelFlag) m_cancelFlag->store(true);
}
