// cmd_executor.cpp
//
// PowerShell command runner.  See cmd_executor.h for lifetime notes.
//
// Key Windows idioms used:
//   - CreateProcessW with CREATE_SUSPENDED | CREATE_NO_WINDOW so we can
//     assign the child to a Job Object before it runs.
//   - Job Object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE so a single
//     CloseHandle() tears down the child AND any grandchildren cleanly
//     (important — PowerShell readily spawns sub-processes).
//   - Two anonymous pipes (one each for stdout and stderr) with the
//     write ends explicitly whitelisted for inheritance via
//     PROC_THREAD_ATTRIBUTE_HANDLE_LIST; parent reads via two std::threads
//     so neither stream can block the child by filling a pipe buffer.
//   - Fully-qualified Windows PowerShell 5.1 with -NoProfile -NonInteractive
//                    -OutputFormat Text -EncodedCommand <b64>
//     where the encoded payload is UTF-16LE of the UTF-8 console-encoding
//     prefix plus the user's command.  Using -EncodedCommand makes
//     quoting a non-issue for anything the user types.
//
#include "cmd_executor.h"
#include "copy_line_fold.h"   // fold MSBuild/vcpkg copy-progress runs
#include "progress_output_fold.h" // collapse \r progress bars, PS 5.1 NativeCommandError wrappers

#include "ps_command_hints.h"
#include "workspace_delta.h"
#include "command_policy.h"   // LintPowerShellHazards (advisory)
#include "chat_folders.h"     // chat folder recognizer
#include "server_manager.h"    // ConversationLaneDirForCwd

#include "ui_event_post.h"

// Win32
#include "lb_windows.h"
#include <shlobj.h>       // SHGetKnownFolderPath (PowerShell 7 location)
#include <knownfolders.h>

// ─── Event definitions ───────────────────────────────────────────
wxDEFINE_EVENT(wxEVT_CMD_COMPLETE, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_CMD_ERROR,    wxCommandEvent);
wxDEFINE_EVENT(wxEVT_CMD_OUTPUT,   wxCommandEvent);

namespace {

// ─── Helpers ─────────────────────────────────────────────────────

// UTF-8 string -> UTF-16LE bytes.  PowerShell's -EncodedCommand expects
// UTF-16LE, base64-encoded.
std::wstring Utf8ToWide(const std::string& in) {
    if (in.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, in.data(), (int)in.size(),
                                nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, in.data(), (int)in.size(),
                        out.data(), n);
    return out;
}

// Base64-encode the raw bytes of a wide string (UTF-16LE on Windows).
std::string Base64EncodeUtf16LE(const std::wstring& w) {
    const char* bytes = reinterpret_cast<const char*>(w.data());
    size_t nbytes = w.size() * sizeof(wchar_t);  // wchar_t is 2 bytes on Windows

    std::ostringstream out;
    Poco::Base64Encoder enc(out);
    enc.write(bytes, static_cast<std::streamsize>(nbytes));
    enc.close();

    // Poco's encoder inserts line breaks every 72 chars by default;
    // PowerShell doesn't care but we strip CR/LF just to keep the
    // command line short and clean.
    std::string s = out.str();
    s.erase(std::remove_if(s.begin(), s.end(),
                           [](char c) { return c == '\n' || c == '\r'; }),
            s.end());
    return s;
}

// Build the full encoded payload we'll hand to -EncodedCommand.
//
// Prefix:
//   $ProgressPreference = SilentlyContinue
//       Suppresses "Preparing modules for first use" progress record,
//       which otherwise serializes to stderr as CLIXML.
//   [Console]::OutputEncoding = UTF8; $OutputEncoding = UTF8
//       Ensures non-ASCII stdout/stderr come back as UTF-8.
//   $ErrorActionPreference = 'Stop'
//       Promotes non-terminating errors (e.g. Get-ChildItem on a missing
//       path) to terminating so trap catches them.  PS 5.1 -NonInteractive
//       does not route non-terminating errors to stderr reliably on its
//       own.  Per-command `-ErrorAction SilentlyContinue` still wins over
//       the preference variable if the user wants partial-success.
//   trap { stderr write; exit 1 }
//       Uniform handler for every error.  Calls exit 1 directly instead
//       of setting a flag and checking it later — -EncodedCommand appears
//       to run script at global scope, and flag variables don't survive
//       the trap->script scope boundary cleanly.  Calling `exit 1` in
//       trap sidesteps the whole scope-scoping mess and makes the exit
//       code reliably reflect whether anything failed.
//
// Semantic cost: multi-statement commands abort at the first error rather
// than continuing.  That is usually right — you want to see the error and
// know it failed.  Commands that need partial success can pass
// `-ErrorAction SilentlyContinue` on the offending cmdlet.
// Escape a UTF-8 string for embedding inside a PowerShell single-quoted
// literal: the only special character is the single quote, doubled.
std::string EscapePsSingleQuoted(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out += c;
        if (c == '\'') out += '\'';
    }
    return out;
}

std::wstring BuildPowerShellPayload(const std::string& userCommand) {
    const std::string prefix =
        "$ProgressPreference = 'SilentlyContinue';"
        // PowerShell 7 can emit ANSI colour escapes in error/format output;
        // the tool result is plain text.  No-op on Windows PowerShell 5.1.
        "if ($PSVersionTable.PSVersion.Major -ge 7) { $PSStyle.OutputRendering = 'PlainText' };"
        "[Console]::OutputEncoding = [System.Text.Encoding]::UTF8;"
        "$OutputEncoding = [System.Text.Encoding]::UTF8;"
        // MSBuild /m normally retains reusable workers after a successful
        // foreground build. This fresh tool process cannot reuse them on its
        // next call, and the job cleanup would report them as detached work.
        // Disable reuse only in this PowerShell process and its descendants;
        // do not change LlamaBoss's environment or the user's Windows settings.
        "$env:MSBUILDDISABLENODEREUSE = '1';"
        // Windows PowerShell 5.1 does not load the ZIP types by default:
        // [IO.Compression.ZipFile] fails with "Unable to find type" until
        // Add-Type runs, and models otherwise prepend Add-Type by hand.
        // GAC load, milliseconds; silent and before 'Stop' is set, so a
        // missing assembly can never fail the user's command.
        "try { Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem -ErrorAction SilentlyContinue } catch { };"
        // Default every text-file cmdlet to UTF-8.  Windows PowerShell
        // 5.1 otherwise READS as ANSI and WRITES UTF-16LE (Out-File) /
        // ANSI (Set-Content, Add-Content), which double-encodes any
        // UTF-8 file the agent itself wrote on a read-modify-write
        // cycle (em-dashes -> mojibake).  Per-call -Encoding still
        // overrides.  PS 5.1 'UTF8' WRITES include a BOM; the agent
        // prompt directs no-BOM writes through
        // [System.IO.File]::WriteAllText + UTF8Encoding($false).
        // Select-String is deliberately absent: it has no -Encoding
        // parameter in 5.1, and a $PSDefaultParameterValues entry for
        // a nonexistent parameter errors at invocation.
        "$PSDefaultParameterValues['Get-Content:Encoding']='UTF8';"
        "$PSDefaultParameterValues['Set-Content:Encoding']='UTF8';"
        "$PSDefaultParameterValues['Add-Content:Encoding']='UTF8';"
        "$PSDefaultParameterValues['Out-File:Encoding']='utf8';"
        "$PSDefaultParameterValues['Import-Csv:Encoding']='UTF8';"
        "$PSDefaultParameterValues['Export-Csv:Encoding']='UTF8';"
        "$ErrorActionPreference = 'Stop';"
        // Windows PowerShell 5.1 wraps each line a NATIVE program writes
        // to a redirected stderr (2> file, 2>&1) in an ErrorRecord.  Under
        // 'Stop' the first such line is fatal: the script dies mid-call
        // and the job cleanup kills the still-running child.  Printing
        // only that stderr line reads like the program's own failure (an
        // ssh session announcing itself looks like a key-unlock problem),
        // so say what really happened.  Note that the script may have set
        // 'Stop' itself, and the steps before the fatal line DID run
        // (e.g. git's LF/CRLF warning stopping a script that had already
        // appended to a file).
        "trap { "
            "if ($_.FullyQualifiedErrorId -like 'NativeCommandError*') { "
                "[Console]::Error.WriteLine('[native stderr] ' + $_.ToString()); "
                "[Console]::Error.WriteLine('[LlamaBoss] The script stopped because a native program wrote the line above to a redirected stderr (2> or 2>&1). It is not necessarily an error from that program. Windows PowerShell 5.1 turns redirected native stderr into error records, and $ErrorActionPreference was ''Stop'' at that point (the tool default, or set by the script), so the first stderr line ended the script; the program may have been stopped mid-run. Everything before that line already ran: files it wrote or appended are in place, so check them before rerunning (an append would repeat). Set $ErrorActionPreference = ''Continue'' before such a call and judge it by $LASTEXITCODE.'); "
            "} else { "
                "[Console]::Error.WriteLine($_.ToString()); "
            "} "
            "exit 1 "
        "}\r\n";

    // Advisory lint (command_policy.cpp): surface hazardous-but-legal
    // string constructs at the top of stdout so the model sees them
    // alongside its own output and self-corrects on the next call.
    // Injected as Write-Output lines so warnings ride the existing
    // async result plumbing with zero changes to result assembly.
    std::string lintBlock;
    for (const std::string& w : LintPowerShellHazards(userCommand)) {
        lintBlock += "Write-Output '"
                   + EscapePsSingleQuoted("lint: " + w)
                   + "';";
    }
    if (!lintBlock.empty()) lintBlock += "\r\n";

    return Utf8ToWide(prefix + lintBlock + userCommand);
}

// Resolve %USERPROFILE% for CWD; fall back to empty (= inherit parent CWD)
// if the env var is missing for some reason.
std::wstring ResolveUserProfileDir() {
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", nullptr, 0);
    if (n == 0) return std::wstring();
    std::wstring buf(n, L'\0');
    DWORD got = GetEnvironmentVariableW(L"USERPROFILE", buf.data(), n);
    if (got == 0) return std::wstring();
    buf.resize(got);
    return buf;
}

// Resolve the Windows PowerShell 5.1 executable (always present).
// Passing it as lpApplicationName avoids CreateProcessW's bare-name search
// order (which can include the parent current directory / PATH shims).
std::wstring ResolveWindowsPowerShellExe()
{
    std::wstring sysDir(MAX_PATH, L'\0');
    UINT n = GetSystemDirectoryW(sysDir.data(), static_cast<UINT>(sysDir.size()));
    if (n == 0) return L"powershell.exe";
    if (n >= sysDir.size()) {
        sysDir.resize(n + 1, L'\0');
        n = GetSystemDirectoryW(sysDir.data(), static_cast<UINT>(sysDir.size()));
        if (n == 0) return L"powershell.exe";
    }
    sysDir.resize(n);
    return sysDir + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
}

// PowerShell 7 (pwsh.exe) when installed, else Windows PowerShell 5.1.
//
// Only the machine-wide install location is probed:
// %ProgramFiles%\PowerShell\7\pwsh.exe (MSI / winget).  Like the 5.1 path, it comes from
// a known folder rather than PATH, so a pwsh.exe dropped into the working
// directory or a user-writable PATH entry is never picked up.  The
// Microsoft Store build (WindowsApps alias) is deliberately not used.
// Set LLAMABOSS_WINDOWS_POWERSHELL=1 to force 5.1.  Resolved once per
// process; installing PowerShell 7 takes effect on the next app start.
struct PowerShellChoice {
    std::wstring exe;
    bool         isPwsh = false;
};

const PowerShellChoice& ResolvePowerShell()
{
    static const PowerShellChoice choice = [] {
        PowerShellChoice c;
        wchar_t force[8] = {};
        const DWORD forced = GetEnvironmentVariableW(
            L"LLAMABOSS_WINDOWS_POWERSHELL", force, 8);
        if (!(forced > 0 && forced < 8 && force[0] == L'1')) {
            PWSTR programFiles = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0,
                                               nullptr, &programFiles))) {
                std::wstring pwsh = std::wstring(programFiles) +
                                    L"\\PowerShell\\7\\pwsh.exe";
                CoTaskMemFree(programFiles);
                const DWORD attrs = GetFileAttributesW(pwsh.c_str());
                if (attrs != INVALID_FILE_ATTRIBUTES &&
                    !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                    c.exe = pwsh;
                    c.isPwsh = true;
                    return c;
                }
            }
            else if (programFiles) {
                CoTaskMemFree(programFiles);
            }
        }
        c.exe = ResolveWindowsPowerShellExe();
        return c;
    }();
    return choice;
}

std::string WideToUtf8(const std::wstring& in) {
    if (in.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, in.data(), (int)in.size(),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, in.data(), (int)in.size(),
                        out.data(), n, nullptr, nullptr);
    return out;
}

std::string JoinPathLocal(const std::string& a, const std::string& b)
{
    if (a.empty()) return b;
    const char sep = wxFILE_SEP_PATH;
    if (a.back() == '/' || a.back() == '\\') return a + b;
    return a + std::string(1, sep) + b;
}

std::string TrimTrailingSeparatorsLocal(std::string s)
{
    while (!s.empty() && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    return s;
}

std::string BaseNameLocal(const std::string& path)
{
    std::string clean = TrimTrailingSeparatorsLocal(path);
    size_t pos = clean.find_last_of("/\\");
    return (pos == std::string::npos) ? clean : clean.substr(pos + 1);
}

std::string LowerForOutputLocal(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return s;
}

bool StartsWithLocal(const std::string& s, const std::string& prefix)
{
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

std::string WxToUtf8Local(const wxString& value)
{
    wxScopedCharBuffer utf8 = value.ToUTF8();
    return utf8 ? std::string(utf8.data()) : std::string();
}

std::string NormalizePathForCompareLocal(const std::string& path)
{
    if (path.empty()) return std::string();

    wxFileName fn(wxString::FromUTF8(path));
    fn.Normalize(wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE);

    std::string normalized = WxToUtf8Local(fn.GetFullPath());
    std::replace(normalized.begin(), normalized.end(), '/', '\\');
    normalized = LowerForOutputLocal(normalized);
    return TrimTrailingSeparatorsLocal(normalized);
}

bool IsPathUnderRootLocal(const std::string& path, const std::string& root)
{
    std::string p = NormalizePathForCompareLocal(path);
    std::string r = NormalizePathForCompareLocal(root);
    if (p.empty() || r.empty()) return false;
    if (p == r) return true;
    if (r.back() != '\\') r.push_back('\\');
    return p.size() > r.size() && p.compare(0, r.size(), r) == 0;
}

std::string ChatFolderFromCwdLocal(const std::string& cwd)
{
    // Shared recognizer (chat_folders.h): ...\Chats\<chat folder>\Workspace.
    return chat_folders::ChatFolderFromWorkspaceCwd(cwd);
}

std::string ToolOutputsDirForCwdLocal(const std::string& cwd)
{
    // Chat folder ToolOutputs lane, else LlamaBoss\Shared\ToolOutputs.
    std::string dir = ServerManager::ConversationLaneDirForCwd(cwd, "ToolOutputs");
    return dir.empty() ? JoinPathLocal(".", "ToolOutputs") : dir;
}

std::string SafeOutputStemLocal(const std::string& text, const std::string& fallback)
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

std::string UniqueOutputPathLocal(const std::string& dir,
                                  const std::string& stem,
                                  const std::string& suffix,
                                  std::string& displayNameOut)
{
    std::string safeStem = SafeOutputStemLocal(stem, "tool_output");
    for (int i = 0; i < 1000; ++i) {
        displayNameOut = (i == 0)
            ? (safeStem + suffix)
            : (safeStem + "_" + std::to_string(i + 1) + suffix);
        std::string path = JoinPathLocal(dir, displayNameOut);
        DWORD attrs = GetFileAttributesW(Utf8ToWide(path).c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) return path;
    }
    displayNameOut.clear();
    return std::string();
}

size_t CountLinesLocal(const std::string& s)
{
    if (s.empty()) return 0;
    size_t n = 0;
    for (char c : s) if (c == '\n') ++n;
    if (s.back() != '\n') ++n;
    return n;
}

std::vector<std::string> SplitLinesLocal(const std::string& s)
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

std::string BuildHeadTailPreviewLocal(const std::string& text,
                                      const std::string& displayName,
                                      bool streamWasCapped)
{
    constexpr size_t kHeadLines = 80;
    constexpr size_t kTailLines = 30;

    std::vector<std::string> lines = SplitLinesLocal(text);
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

size_t FileSizeLocal(const std::string& path)
{
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(Utf8ToWide(path).c_str(), GetFileExInfoStandard, &data)) return 0;
    ULARGE_INTEGER u;
    u.HighPart = data.nFileSizeHigh;
    u.LowPart = data.nFileSizeLow;
    return static_cast<size_t>(u.QuadPart);
}

bool WriteUtf8TextFileLocal(const std::string& path, const std::string& content)
{
    try {
        std::ofstream f(Utf8ToWide(path), std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
        return f.good();
    } catch (...) {
        return false;
    }
}

bool ShouldExternalizeOutputLocal(const std::string& text)
{
    constexpr size_t kMaxInlineBytes = 16 * 1024;
    constexpr size_t kMaxInlineLines = 120;
    return text.size() > kMaxInlineBytes || CountLinesLocal(text) > kMaxInlineLines;
}

void ExternalizeOneStreamLocal(CmdResult& result,
                               std::string& streamText,
                               const std::string& cwd,
                               const std::string& stem,
                               const std::string& streamName,
                               bool streamWasCapped)
{
    if (!ShouldExternalizeOutputLocal(streamText)) return;

    std::string dir = ToolOutputsDirForCwdLocal(cwd);
    wxFileName::Mkdir(wxString::FromUTF8(dir.c_str()), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);

    std::string displayName;
    std::string path = UniqueOutputPathLocal(dir, stem + "_" + streamName, ".txt", displayName);
    if (path.empty()) return;

    std::string fileBody = streamText;
    if (streamWasCapped) {
        if (!fileBody.empty() && fileBody.back() != '\n') fileBody += "\n";
        fileBody += "\n[LlamaBoss output capture cap reached; additional output was discarded.]\n";
    }

    if (!WriteUtf8TextFileLocal(path, fileBody)) return;

    PresentedFile f;
    f.displayName = displayName;
    f.language    = "text";
    f.diskPath    = path;
    f.sizeBytes   = FileSizeLocal(path);
    f.lineCount   = static_cast<int>(CountLinesLocal(fileBody));
    result.presentedFiles.push_back(std::move(f));

    streamText = BuildHeadTailPreviewLocal(streamText, displayName, streamWasCapped);
}

void ApplyLargeOutputHandlingLocal(CmdResult& result, const std::string& cwd)
{
    if (!ShouldExternalizeOutputLocal(result.stdoutText) &&
        !ShouldExternalizeOutputLocal(result.stderrText)) {
        return;
    }

    std::string stem = SafeOutputStemLocal(result.command, "powershell_output");
    ExternalizeOneStreamLocal(result, result.stdoutText, cwd, stem, "stdout", result.stdoutTruncated);
    ExternalizeOneStreamLocal(result, result.stderrText, cwd, stem, "stderr", result.stderrTruncated);
}

// ─── Workspace-delta support (see workspace_delta.h) ─────────────
// PowerShell commands routinely create files (Compress-Archive,
// Out-File, Copy-Item, redirects).  Attaching the result as a card and
// telling the model whether the file system actually changed stops it
// from claiming a file was "attached above" when a silently-empty
// pipeline created nothing and exited 0.

std::string LanguageForCreatedFileLocal(const std::string& path)
{
    std::string name = LowerForOutputLocal(BaseNameLocal(path));
    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return std::string();
    std::string ext = name.substr(dot + 1);

    if (ext == "ps1")  return "powershell";
    if (ext == "py")   return "python";
    if (ext == "cpp" || ext == "h" || ext == "hpp" || ext == "c")
                       return "cpp";
    if (ext == "md")   return "markdown";
    if (ext == "json") return "json";
    if (ext == "csv")  return "csv";
    if (ext == "txt" || ext == "log") return "text";
    if (ext == "html" || ext == "htm") return "html";
    if (ext == "xml")  return "xml";
    return std::string();   // binary / unknown: no language chip
}

bool IsBlankTextLocal(const std::string& s)
{
    for (char c : s) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') return false;
    }
    return true;
}

bool IsRegularFileLocal(const std::string& path)
{
    DWORD attrs = GetFileAttributesW(Utf8ToWide(path).c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::string TrimArtifactValueLocal(const std::string& raw)
{
    size_t first = raw.find_first_not_of(" \t\r\n`\"'");
    if (first == std::string::npos) return std::string();
    size_t last = raw.find_last_not_of(" \t\r\n`\"'");
    return raw.substr(first, last - first + 1);
}

bool ResolveExplicitArtifactPathLocal(const std::string& raw,
                                      const std::string& cwd,
                                      const std::string& activeProjectRoot,
                                      std::string& pathOut)
{
    std::string value = TrimArtifactValueLocal(raw);
    if (value.empty()) return false;

    wxFileName fn(wxString::FromUTF8(value));
    if (!fn.IsAbsolute()) {
        const std::string& base = !activeProjectRoot.empty()
            ? activeProjectRoot : cwd;
        if (base.empty()) return false;
        fn.MakeAbsolute(wxString::FromUTF8(base));
    }
    fn.Normalize(wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE);
    pathOut = WxToUtf8Local(fn.GetFullPath());
    if (!IsRegularFileLocal(pathOut)) return false;

    const std::string chatFolder = ChatFolderFromCwdLocal(cwd);
    return IsPathUnderRootLocal(pathOut, cwd) ||
           IsPathUnderRootLocal(pathOut, chatFolder) ||
           IsPathUnderRootLocal(pathOut, activeProjectRoot);
}

bool PresentedFileAlreadyAddedLocal(const CmdResult& result,
                                    const std::string& path)
{
    const std::string wanted = NormalizePathForCompareLocal(path);
    for (const auto& file : result.presentedFiles) {
        if (!file.diskPath.empty() &&
            NormalizePathForCompareLocal(file.diskPath) == wanted) {
            return true;
        }
    }
    return false;
}

// Successful PowerShell workflows may explicitly nominate user-facing files
// with one line per file:
//
//   ARTIFACT_FILE: C:\\absolute\\path\\to\\result.mp4
//
// The marker is deliberately narrow: only existing regular files under the
// conversation or active-project roots are accepted. Explicit artifacts are
// disk-backed cards, so large media files do not need to be read into memory
// and are not subject to the automatic workspace-delta 25 MiB noise cap.
void AttachExplicitPowerShellArtifactsLocal(CmdResult& result,
                                             const std::string& cwd,
                                             const std::string& activeProjectRoot)
{
    if (result.exitCode != 0 || result.timedOut || result.cancelled ||
        result.stdoutText.empty()) {
        return;
    }

    constexpr int kMaxExplicitArtifacts = 12;
    int attached = 0;
    std::ostringstream cleaned;
    bool firstOutputLine = true;

    for (const std::string& originalLine : SplitLinesLocal(result.stdoutText)) {
        std::string probe = originalLine;
        size_t first = probe.find_first_not_of(" \t");
        if (first != std::string::npos) probe.erase(0, first);
        std::string lower = LowerForOutputLocal(probe);

        const std::string marker = "artifact_file:";
        const bool isMarker = StartsWithLocal(lower, marker);
        bool consumed = false;

        if (isMarker && attached < kMaxExplicitArtifacts) {
            std::string path;
            if (ResolveExplicitArtifactPathLocal(
                    probe.substr(marker.size()), cwd, activeProjectRoot, path)) {
                if (!PresentedFileAlreadyAddedLocal(result, path)) {
                    PresentedFile file;
                    file.displayName = BaseNameLocal(path);
                    file.language    = LanguageForCreatedFileLocal(path);
                    file.diskPath    = path;
                    file.sizeBytes   = FileSizeLocal(path);
                    file.lineCount   = 0;
                    result.presentedFiles.push_back(std::move(file));
                    ++attached;
                }
                consumed = true;
            }
        }

        // Valid markers are control records, not model-facing output. Keep an
        // invalid or disallowed marker visible so the agent can diagnose it.
        if (!consumed) {
            if (!firstOutputLine) cleaned << "\r\n";
            cleaned << originalLine;
            firstOutputLine = false;
        }
    }

    result.stdoutText = cleaned.str();
}

// Attaches PresentedFile cards for files CREATED by the command and
// returns the manifest text to append to stdout after externalization.
// Modified files are reported by name only (no card) — re-attaching a
// file the user already has open as a card on every touch is noise.
std::string ApplyWorkspaceDeltaLocal(CmdResult&                       result,
                                     const workspace_delta::Snapshot& before,
                                     const std::string&               cwd)
{
    constexpr int    kMaxAttach    = 12;
    constexpr size_t kMaxCardBytes = 25 * 1024 * 1024;   // mirror python_runner

    workspace_delta::Snapshot after = workspace_delta::TakeSnapshot(cwd);
    workspace_delta::Delta    delta = workspace_delta::Diff(before, after);

    int attached = 0;
    for (const std::string& path : delta.created) {
        if (attached >= kMaxAttach) break;

        auto it = after.files.find(path);
        const unsigned long long size =
            (it != after.files.end()) ? it->second.sizeBytes : 0;
        if (size == 0 || size > kMaxCardBytes) continue;   // still listed in manifest

        if (PresentedFileAlreadyAddedLocal(result, path)) continue;

        PresentedFile f;
        f.displayName = BaseNameLocal(path);
        f.language    = LanguageForCreatedFileLocal(path);
        f.diskPath    = path;
        f.sizeBytes   = static_cast<size_t>(size);
        f.lineCount   = 0;   // size is shown on the chip; counting lines
                             // would mean re-reading arbitrary files here
        result.presentedFiles.push_back(std::move(f));
        ++attached;
    }

    return workspace_delta::FormatManifest(delta, after, cwd);
}

// Monotonic clock snapshot in seconds since an arbitrary epoch.
double NowSec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Strip PowerShell CLIXML serialization blocks from captured output.
// PowerShell sometimes serializes progress/error records to stderr as
//   #< CLIXML
//   <Objs ...>...</Objs>
// even when we set $ProgressPreference = SilentlyContinue.  It's noise
// to the user and burns context budget downstream.
// Handles multiple blocks in a single buffer and trims a single trailing
// newline per block so the surrounding text doesn't gain blank lines.
//
// ERROR records are kept.  A block can carry
//   <S S="Error">message_x000D__x000A_</S>
// strings, which is how PowerShell reports failures that happen before
// the payload's trap is installed — most importantly PARSE errors, where
// nothing runs at all.  Deleting the whole block would turn those into a
// bare "exit 1" with no text.  Error strings are decoded and left in
// place of the block; progress and other records are dropped.

// Decode CLIXML string content: XML entities plus _xHHHH_ escapes
// (CLIXML encodes CR/LF and other control characters that way).
std::string DecodeClixmlString(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c == '&') {
            static const struct { const char* ent; char ch; } kEnts[] = {
                { "&lt;", '<' }, { "&gt;", '>' }, { "&amp;", '&' },
                { "&quot;", '"' }, { "&apos;", '\'' } };
            bool matched = false;
            for (const auto& e : kEnts) {
                const size_t n = std::char_traits<char>::length(e.ent);
                if (in.compare(i, n, e.ent) == 0) {
                    out += e.ch; i += n - 1; matched = true; break;
                }
            }
            if (!matched) out += c;
            continue;
        }
        if (c == '_' && i + 6 < in.size() && in[i + 1] == 'x' && in[i + 6] == '_') {
            unsigned v = 0;
            bool hex = true;
            for (size_t k = i + 2; k < i + 6; ++k) {
                const char h = in[k];
                v <<= 4;
                if (h >= '0' && h <= '9')      v |= unsigned(h - '0');
                else if (h >= 'a' && h <= 'f') v |= unsigned(h - 'a' + 10);
                else if (h >= 'A' && h <= 'F') v |= unsigned(h - 'A' + 10);
                else { hex = false; break; }
            }
            if (hex) {
                // Encode as UTF-8 (BMP only, which is all _xHHHH_ carries).
                if (v < 0x80) {
                    out += char(v);
                } else if (v < 0x800) {
                    out += char(0xC0 | (v >> 6));
                    out += char(0x80 | (v & 0x3F));
                } else {
                    out += char(0xE0 | (v >> 12));
                    out += char(0x80 | ((v >> 6) & 0x3F));
                    out += char(0x80 | (v & 0x3F));
                }
                i += 6;
                continue;
            }
        }
        out += c;
    }
    return out;
}

// Collect the decoded text of every <S S="Error"> string in [begin, end).
std::string ExtractClixmlErrors(const std::string& s, size_t begin, size_t end) {
    const std::string open  = "<S S=\"Error\">";
    const std::string shut  = "</S>";
    std::string errors;
    size_t p = begin;
    while ((p = s.find(open, p)) != std::string::npos && p < end) {
        const size_t textStart = p + open.size();
        const size_t textEnd   = s.find(shut, textStart);
        if (textEnd == std::string::npos || textEnd > end) break;
        errors += DecodeClixmlString(s.substr(textStart, textEnd - textStart));
        p = textEnd + shut.size();
    }
    return errors;
}

void StripClixmlInPlace(std::string& s) {
    const std::string marker = "#< CLIXML";
    const std::string close  = "</Objs>";
    size_t pos = 0;
    while ((pos = s.find(marker, pos)) != std::string::npos) {
        size_t end = s.find(close, pos);
        if (end == std::string::npos) break;  // malformed — leave alone
        end += close.size();

        std::string errors = ExtractClixmlErrors(s, pos, end);
        if (!errors.empty() && errors.back() != '\n') errors += "\r\n";

        // Consume one trailing \r and/or \n so we don't leave blank gaps.
        if (end < s.size() && s[end] == '\r') ++end;
        if (end < s.size() && s[end] == '\n') ++end;
        s.replace(pos, end - pos, errors);
        pos += errors.size();
        // `pos` now points at whatever followed the block — keep scanning.
    }
}

// RAII for Win32 HANDLEs.
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

struct ProcThreadAttrGuard {
    std::vector<BYTE> buffer;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = nullptr;

    ~ProcThreadAttrGuard() {
        if (attrs) DeleteProcThreadAttributeList(attrs);
    }

    bool InitHandleList(HANDLE* handles, SIZE_T bytes, std::string& error) {
        SIZE_T attrSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
        if (attrSize == 0) {
            error = "InitializeProcThreadAttributeList(size) failed, error=" +
                    std::to_string(GetLastError());
            return false;
        }

        buffer.resize(attrSize);
        attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer.data());
        if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize)) {
            error = "InitializeProcThreadAttributeList failed, error=" +
                    std::to_string(GetLastError());
            attrs = nullptr;
            return false;
        }

        if (!UpdateProcThreadAttribute(attrs,
                                       0,
                                       PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       handles,
                                       bytes,
                                       nullptr,
                                       nullptr)) {
            error = "UpdateProcThreadAttribute(handle list) failed, error=" +
                    std::to_string(GetLastError());
            return false;
        }
        return true;
    }
};

void TrimIncompleteUtf8Tail(std::string& s)
{
    if (s.empty()) return;

    const size_t n = s.size();
    size_t start = n - 1;
    while (start > 0 &&
           (static_cast<unsigned char>(s[start]) & 0xC0) == 0x80) {
        --start;
    }

    unsigned char lead = static_cast<unsigned char>(s[start]);
    size_t expected = 0;
    if ((lead & 0x80) == 0x00) return;          // ASCII
    else if ((lead & 0xE0) == 0xC0) expected = 2;
    else if ((lead & 0xF0) == 0xE0) expected = 3;
    else if ((lead & 0xF8) == 0xF0) expected = 4;
    else {
        // Invalid trailing byte sequence.  Since we only call this after
        // a byte cap fired, prefer losing a few tail bytes over poisoning
        // wxString::FromUTF8 for the whole captured stream.
        s.resize(start);
        return;
    }

    if (n - start < expected)
        s.resize(start);
}

void CancelThreadSynchronousIoLocal(HANDLE threadHandle)
{
    if (!threadHandle) return;
    using Fn = BOOL (WINAPI *)(HANDLE);
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    if (!kernel) return;
    auto* fn = reinterpret_cast<Fn>(GetProcAddress(kernel, "CancelSynchronousIo"));
    if (fn) fn(threadHandle);
}

// Pipe reader: drains `readEnd` into `dest`, stopping early after
// kMaxOutputBytes have been accepted.  Further bytes are discarded but the
// pipe is still drained so the child cannot block on a full pipe buffer.
// Each stream has exactly one writer thread, and the worker reads `dest`
// only after join(), so no per-stream mutex is needed.
// Shared "latest line" slot fed by both pipe readers and drained by the
// worker's wait loop.  Both readers write, so this one needs a mutex.
struct LiveTail {
    std::mutex  mutex;
    std::string partial;    // bytes since the last terminator
    std::string lastLine;   // most recent complete non-empty line
    bool        dirty = false;

    void Feed(const char* data, size_t len)
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (size_t i = 0; i < len; ++i) {
            const char c = data[i];
            if (c == '\n' || c == '\r') {
                if (!partial.empty()) {
                    // Trim trailing whitespace; skip pure-whitespace lines.
                    size_t end = partial.find_last_not_of(" \t");
                    if (end != std::string::npos) {
                        lastLine.assign(partial, 0, end + 1);
                        dirty = true;
                    }
                    partial.clear();
                }
            } else {
                partial.push_back(c);
                // A pathological writer with no terminators (progress
                // rendered with backspaces, or a binary dump) must not grow
                // this without bound.  Keep the tail; it is display-only.
                if (partial.size() > 512) {
                    // Erase to a codepoint boundary: the line is later
                    // handed to wxString::FromUTF8, which returns EMPTY
                    // for a leading partial sequence — the whole live
                    // line would vanish, not just one character.
                    size_t cut = partial.size() - 512;
                    while (cut < partial.size() &&
                           (static_cast<unsigned char>(partial[cut]) & 0xC0) == 0x80)
                        ++cut;
                    partial.erase(0, cut);
                }
            }
        }
    }

    // Returns true and fills |out| if a new line arrived since last take.
    bool Take(std::string& out)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!dirty) return false;
        dirty = false;
        out = lastLine;
        return true;
    }
};

void ReaderLoop(HANDLE readEnd, std::string& dest,
                std::atomic<bool>& truncatedFlag,
                std::atomic<bool>& stopFlag,
                std::atomic<bool>& doneFlag,
                LiveTail* liveTail)
{
    constexpr DWORD kChunk = 4096;
    char buf[kChunk];
    for (;;) {
        if (stopFlag.load()) break;

        DWORD got = 0;
        BOOL ok = ReadFile(readEnd, buf, kChunk, &got, nullptr);
        if (!ok || got == 0) break;  // pipe closed/cancelled/error -> done

        if (liveTail) liveTail->Feed(buf, got);

        if (dest.size() < CmdExecutor::kMaxOutputBytes) {
            size_t room = CmdExecutor::kMaxOutputBytes - dest.size();
            size_t take = std::min<size_t>(got, room);
            dest.append(buf, take);
            if (take < got)
                truncatedFlag.store(true);
        } else {
            truncatedFlag.store(true);
            // keep reading to drain — discard the bytes
        }
    }
    doneFlag.store(true);
}

enum class JobDescendantState {
    None,
    Present,
    Unknown
};

// Presentation classification only, never permission to survive job cleanup.
// Require the full MSVC tool-directory layout as well as the executable name;
// a same-named executable in the workspace is not a compiler helper.
bool IsMsvcCleanupHelperLocal(const std::wstring& imagePath)
{
    std::string path = LowerForOutputLocal(WideToUtf8(imagePath));
    std::replace(path.begin(), path.end(), '/', '\\');
    if (path.size() < 3 || path[1] != ':' || path[2] != '\\') return false;
    const std::string marker = "\\vc\\tools\\msvc\\";
    const size_t start = path.find(marker);
    if (start == std::string::npos) return false;
    const size_t versionStart = start + marker.size();
    const size_t versionEnd = path.find('\\', versionStart);
    if (versionEnd == std::string::npos || versionEnd == versionStart) return false;
    const std::string version = path.substr(versionStart, versionEnd - versionStart);
    if (version.find_first_not_of("0123456789.") != std::string::npos ||
        version.find_first_of("0123456789") == std::string::npos) return false;
    const std::string suffix = path.substr(versionEnd);
    for (const char* host : { "hostx86", "hostx64", "hostarm64" }) {
        for (const char* target : { "x86", "x64", "arm", "arm64", "arm64ec" }) {
            const std::string dir = std::string("\\bin\\") + host + "\\" + target + "\\";
            if (suffix == dir + "vctip.exe" || suffix == dir + "mspdbsrv.exe")
                return true;
        }
    }
    return false;
}

// Query the process IDs that are still active in the Job Object and ignore
// the already-signalled top-level PowerShell process.  The accounting-only
// ActiveProcesses value can briefly lag process-handle signalling, which made
// a normal foreground command look like it had escaped into the background.
JobDescendantState QueryJobDescendantsLocal(
    HANDLE job, DWORD rootProcessId, std::string* details = nullptr,
    bool* buildHelpersOnly = nullptr)
{
    if (details) details->clear();
    if (buildHelpersOnly) *buildHelpersOnly = false;
    size_t capacity = 8;
    for (int attempt = 0; attempt < 6; ++attempt) {
        const size_t bytes =
            sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) +
            (capacity - 1) * sizeof(ULONG_PTR);
        std::vector<unsigned char> storage(bytes, 0);
        auto* ids = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(
            storage.data());

        if (QueryInformationJobObject(
                job, JobObjectBasicProcessIdList,
                ids, static_cast<DWORD>(storage.size()), nullptr)) {
            bool anyLive = false;
            bool anyUnknown = false;
            bool allLiveAreBuildHelpers = true;
            size_t reportedCount = 0;
            auto report = [&](DWORD pid, const std::string& description) {
                if (!details) return;
                if (reportedCount < 8) {
                    if (!details->empty()) *details += "; ";
                    *details += "PID " + std::to_string(pid) + " " + description;
                }
                ++reportedCount;
            };
            for (DWORD i = 0; i < ids->NumberOfProcessIdsInList; ++i) {
                const DWORD pid = static_cast<DWORD>(ids->ProcessIdList[i]);
                if (pid == rootProcessId) continue;

                HandleGuard child(OpenProcess(
                    SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                    FALSE, pid));
                if (!child.h) {
                    const DWORD error = GetLastError();
                    anyUnknown = true;
                    report(pid, "(state unavailable; OpenProcess error " +
                        std::to_string(error) + ")");
                    continue;
                }

                // A PID may disappear or be reused between the job snapshot
                // and OpenProcess. Never identify an unrelated process as a
                // survivor, or an already-signalled child as live work.
                BOOL inJob = FALSE;
                if (!IsProcessInJob(child.h, job, &inJob)) {
                    const DWORD error = GetLastError();
                    anyUnknown = true;
                    report(pid, "(job membership unavailable; error " +
                        std::to_string(error) + ")");
                    continue;
                }
                if (!inJob) continue;
                const DWORD state = WaitForSingleObject(child.h, 0);
                if (state == WAIT_OBJECT_0) continue;
                if (state != WAIT_TIMEOUT) {
                    const DWORD error = GetLastError();
                    anyUnknown = true;
                    report(pid, "(process state unavailable; error " +
                        std::to_string(error) + ")");
                    continue;
                }
                anyLive = true;
                if (details || buildHelpersOnly) {
                    std::wstring path(32768, L'\0');
                    DWORD length = static_cast<DWORD>(path.size());
                    if (QueryFullProcessImageNameW(child.h, 0, &path[0], &length)) {
                        path.resize(length);
                        if (!IsMsvcCleanupHelperLocal(path))
                            allLiveAreBuildHelpers = false;
                        const size_t slash = path.find_last_of(L"\\/");
                        report(pid, WideToUtf8(slash == std::wstring::npos
                            ? path : path.substr(slash + 1)) + " (running)");
                    } else {
                        allLiveAreBuildHelpers = false;
                        report(pid, "(running; executable name unavailable)");
                    }
                }
            }
            if (details && reportedCount > 8)
                *details += "; " + std::to_string(reportedCount - 8) + " more";
            if (buildHelpersOnly)
                *buildHelpersOnly = anyLive && !anyUnknown && allLiveAreBuildHelpers;
            return anyLive ? JobDescendantState::Present :
                (anyUnknown ? JobDescendantState::Unknown : JobDescendantState::None);
        }

        if (GetLastError() != ERROR_MORE_DATA)
            return JobDescendantState::Unknown;

        const size_t reported =
            static_cast<size_t>(ids->NumberOfAssignedProcesses);
        capacity = std::max(capacity * 2, reported + 1);
    }
    return JobDescendantState::Unknown;
}

// Short-lived foreground tools can finish their visible work just before a
// helper process or Windows Job Object bookkeeping settles.  Give those
// descendants a small, bounded opportunity to exit naturally.  A genuinely
// detached child remains in the job, is reported to the caller, and is killed
// when JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE takes effect immediately afterward.
bool WaitForJobDescendantsLocal(
    HANDLE job, DWORD rootProcessId, std::string& details,
    bool& buildHelpersOnly)
{
    buildHelpersOnly = false;
    constexpr DWORD kNaturalExitGraceMs = 1500;
    constexpr DWORD kPollMs = 25;
    const ULONGLONG deadline = GetTickCount64() + kNaturalExitGraceMs;

    JobDescendantState state = JobDescendantState::Unknown;
    for (;;) {
        state = QueryJobDescendantsLocal(job, rootProcessId);
        if (state == JobDescendantState::None)
            return false;

        if (GetTickCount64() >= deadline)
            break;

        Sleep(kPollMs);
    }

    // Capture names only once, immediately before job cleanup. Recheck the
    // state too, so a child that exited at the grace boundary is not reported.
    state = QueryJobDescendantsLocal(job, rootProcessId, &details, &buildHelpersOnly);
    if (state == JobDescendantState::None)
        return false;
    if (state == JobDescendantState::Present)
        return true;

    // The PID-list query should normally succeed.  Preserve the old safe
    // behavior if Windows refuses it: after the grace period, an active job
    // still gets closed and surfaced rather than being allowed to escape.
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting = {};
    if (details.empty())
        details = "Process list unavailable; using job accounting (live state unconfirmed).";
    return QueryInformationJobObject(
               job, JobObjectBasicAccountingInformation,
               &accounting, sizeof(accounting), nullptr) &&
           accounting.ActiveProcesses > 0;
}

// ─── Worker thread ───────────────────────────────────────────────

class CmdWorkerThread : public wxThread {
public:
    CmdWorkerThread(wxEvtHandler* evtHandler,
                    const std::string& command,
                    const std::string& cwd,
                    const std::string& activeProjectRoot,
                    unsigned long      timeoutMs,
                    std::shared_ptr<std::atomic<bool>> cancelFlag,
                    std::shared_ptr<std::atomic<bool>> runningFlag,
                    std::weak_ptr<std::atomic<bool>> aliveToken)
        : wxThread(wxTHREAD_DETACHED)
        , m_evtHandler(evtHandler)
        , m_command(command)
        , m_cwd(cwd)
        , m_activeProjectRoot(activeProjectRoot)
        , m_timeoutMs(timeoutMs ? timeoutMs : CmdExecutor::kDefaultTimeoutMs)
        , m_cancelFlag(std::move(cancelFlag))
        , m_runningFlag(std::move(runningFlag))
        , m_aliveToken(std::move(aliveToken))
    {}

protected:
    ExitCode Entry() override {
        CmdResult result;
        result.command = m_command;

        double t0 = NowSec();
        RunOne(result);
        result.elapsedSec = NowSec() - t0;

        // Clear running flag BEFORE posting event so that an event
        // handler that immediately tries to Start() another command
        // sees us as idle.  A later completion event from run A can therefore
        // arrive after run B has already started; UI handlers should consult
        // IsRunning() rather than treating event arrival as an idle guarantee.
        if (m_runningFlag) m_runningFlag->store(false);

        PostCompletion(std::move(result));
        return (ExitCode)0;
    }

private:
    // Fills `result` with stdout/stderr/exit/flags.  Never throws.
    void RunOne(CmdResult& result) {
        // 1. Build the command line:
        //    powershell.exe -NoProfile -NonInteractive
        //                   -OutputFormat Text -EncodedCommand <b64>
        //
        //    -OutputFormat Text forces plain-text stdout/stderr.  Without
        //    it, PowerShell serializes error/warning records (and some
        //    other object streams) to stderr as CLIXML.  That's ugly for
        //    the user and — more importantly — eats real error messages:
        //    'ThisIsNotARealCmdlet' was landing inside <S S="Error">...</S>
        //    and getting swallowed by the CLIXML scrubber below.
        std::wstring payload = BuildPowerShellPayload(m_command);
        if (payload.empty()) {
            result.stderrText = "Failed to encode command as UTF-16.";
            result.exitCode = -1;
            return;
        }
        std::string b64 = Base64EncodeUtf16LE(payload);
        std::wstring psExe = ResolvePowerShell().exe;
        std::wstring wCmdLine =
            L"\"" + psExe + L"\" -NoProfile -NonInteractive "
            L"-OutputFormat Text -EncodedCommand " +
            Utf8ToWide(b64);

        // 2. Create two inheritable pipes (stdout + stderr).
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
            return;
        }
        HandleGuard outR(outR_raw), outW(outW_raw);
        if (!CreatePipe(&errR_raw, &errW_raw, &sa, 0)) {
            result.stderrText = "CreatePipe(stderr) failed, error=" +
                                std::to_string(GetLastError());
            result.exitCode = -1;
            return;
        }
        HandleGuard errR(errR_raw), errW(errW_raw);

        // Ensure the READ ends are NOT inherited by the child.
        SetHandleInformation(outR.h, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(errR.h, HANDLE_FLAG_INHERIT, 0);

        // 3. Create the Job Object we'll pin the child to.
        HandleGuard job(CreateJobObjectW(nullptr, nullptr));
        if (!job.h) {
            result.stderrText = "CreateJobObject failed, error=" +
                                std::to_string(GetLastError());
            result.exitCode = -1;
            return;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
        jeli.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job.h,
                                     JobObjectExtendedLimitInformation,
                                     &jeli, sizeof(jeli))) {
            result.stderrText = "SetInformationJobObject failed, error=" +
                                std::to_string(GetLastError());
            result.exitCode = -1;
            return;
        }

        // 4. Spawn PowerShell (suspended), wire stdio, assign to job.
        // The handle list keeps inheritance scoped to the two pipe write
        // handles even though CreateProcessW still needs bInheritHandles=TRUE
        // for STARTF_USESTDHANDLES to work.  This avoids cross-tool pipe
        // leaks when another child process is spawned concurrently.
        HANDLE inheritHandles[2] = { outW.h, errW.h };
        ProcThreadAttrGuard attrList;
        std::string attrError;
        if (!attrList.InitHandleList(inheritHandles, sizeof(inheritHandles), attrError)) {
            result.stderrText = attrError;
            result.exitCode = -1;
            return;
        }

        STARTUPINFOEXW si = {};
        si.StartupInfo.cb = sizeof(si);
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput  = nullptr;             // no stdin for /cmd
        si.StartupInfo.hStdOutput = outW.h;
        si.StartupInfo.hStdError  = errW.h;
        si.lpAttributeList = attrList.attrs;

        PROCESS_INFORMATION pi = {};

        // CreateProcessW requires a MUTABLE wide buffer.
        std::vector<wchar_t> cmdBuf(wCmdLine.begin(), wCmdLine.end());
        cmdBuf.push_back(L'\0');

        std::wstring cwd;
        if (!m_cwd.empty()) {
            // Caller-provided CWD wins.  We do not validate the path's
            // existence here — CreateProcessW will fail with a clear
            // error if the directory is bogus, and the worker surfaces
            // that as a stderr line in the result.
            cwd = Utf8ToWide(m_cwd);
        }
        if (cwd.empty()) cwd = ResolveUserProfileDir();
        LPCWSTR cwdArg = cwd.empty() ? nullptr : cwd.c_str();

        // Workspace-delta snapshot (see workspace_delta.h).  Taken only
        // when a caller-provided CWD exists: agent/tool invocations pass
        // the per-conversation Workspace folder, which is small and
        // bounded.  The empty-CWD path falls back to %USERPROFILE%,
        // which is far too broad to scan — skip it there.
        workspace_delta::Snapshot wsBefore;
        const bool trackWorkspace = !m_cwd.empty();
        if (trackWorkspace) {
            wsBefore = workspace_delta::TakeSnapshot(m_cwd);
        }

        BOOL ok = CreateProcessW(
            psExe.c_str(),
            cmdBuf.data(),
            nullptr, nullptr,
            TRUE,                                           // required for whitelisted std handles
            CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
            nullptr,                                        // inherit env
            cwdArg,
            reinterpret_cast<LPSTARTUPINFOW>(&si),
            &pi
        );
        if (!ok) {
            DWORD err = GetLastError();
            result.stderrText = "CreateProcess(PowerShell) failed for " +
                                WideToUtf8(psExe) + ", error=" + std::to_string(err);
            result.exitCode = -1;
            return;
        }
        HandleGuard proc(pi.hProcess);
        HandleGuard thr(pi.hThread);

        // Pin child to job BEFORE it runs.  If assignment fails we
        // fall back to TerminateProcess for cleanup (imperfect, but
        // still better than leaking the process).
        if (!AssignProcessToJobObject(job.h, proc.h)) {
            DWORD err = GetLastError();
            TerminateProcess(proc.h, 1);
            result.stderrText = "AssignProcessToJobObject failed, error=" +
                                std::to_string(err);
            result.exitCode = -1;
            return;
        }

        ResumeThread(thr.h);

        // Close the write ends in the parent so ReadFile returns 0
        // (pipe closed) once the child exits.
        CloseHandle(outW.release());
        CloseHandle(errW.release());

        // 5. Spin up reader threads.
        std::atomic<bool> stdoutTruncated(false);
        std::atomic<bool> stderrTruncated(false);
        std::atomic<bool> stopReaders(false);
        std::atomic<bool> stdoutReaderDone(false);
        std::atomic<bool> stderrReaderDone(false);
        LiveTail liveTail;
        std::thread outThread(ReaderLoop,
            outR.h, std::ref(result.stdoutText),
            std::ref(stdoutTruncated), std::ref(stopReaders),
            std::ref(stdoutReaderDone), &liveTail);
        std::thread errThread(ReaderLoop,
            errR.h, std::ref(result.stderrText),
            std::ref(stderrTruncated), std::ref(stopReaders),
            std::ref(stderrReaderDone), &liveTail);

        // 6. Wait loop: poll process with timeout, observe cancel
        //    flag, enforce the overall deadline.  The same 200 ms tick
        //    doubles as the throttle for live-output events: at most one
        //    wxEVT_CMD_OUTPUT per kLiveTailIntervalMs, and only when a
        //    new line actually arrived.
        constexpr DWORD kTickMs = 200;
        double deadline = NowSec() + (m_timeoutMs / 1000.0);
        double nextLivePost = 0.0;

        auto postLiveTail = [&]() {
            std::string line;
            if (!liveTail.Take(line)) return;
            auto* ev = new wxCommandEvent(wxEVT_CMD_OUTPUT);
            ev->SetString(wxString::FromUTF8(line));
            LbQueueEventIfAlive(m_evtHandler, m_aliveToken, ev);
        };

        bool killIt = false;
        for (;;) {
            DWORD wr = WaitForSingleObject(proc.h, kTickMs);
            if (wr == WAIT_OBJECT_0) break;           // child exited
            if (wr == WAIT_FAILED) { killIt = true; break; }

            const double now = NowSec();
            if (now >= nextLivePost) {
                postLiveTail();
                nextLivePost = now + CmdExecutor::kLiveTailIntervalMs / 1000.0;
            }

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

        std::string descendantDetails;
        bool buildHelpersOnly = false;
        if (!killIt) {
            // The top-level PowerShell process can exit while either a
            // short-lived foreground helper is still winding down or a real
            // detached child remains alive.  Wait briefly for the former,
            // ignoring PowerShell's already-signalled PID, then classify only
            // a persistent descendant as unsupported background work.
            result.descendantsTerminated =
                WaitForJobDescendantsLocal(job.h, pi.dwProcessId,
                    descendantDetails, buildHelpersOnly);
        }

        // Always close the job before joining the pipe readers. On timeout or
        // cancellation this kills the whole process tree as before. On a
        // normal PowerShell exit it also closes unsupported lingering
        // descendants, guaranteeing that their inherited pipe writers cannot
        // keep this worker pending indefinitely.
        CloseHandle(job.release());

        if (killIt) {
            // Give Windows a moment to deliver the job termination. Reader
            // completion is checked independently below because the direct
            // process can exit while another leaked writer still owns a pipe.
            WaitForSingleObject(proc.h, 2000);
        }

        // Give buffered stdout/stderr a bounded drain window.  An
        // unconditional join would let one inherited writer handle
        // bypass the command timeout forever. If either reader is still
        // blocked after the grace period, ask its synchronous ReadFile to
        // return and make the loop observe stopReaders before another read.
        constexpr DWORD kReaderDrainGraceMs = 2000;
        const ULONGLONG drainDeadline =
            GetTickCount64() + kReaderDrainGraceMs;
        while ((!stdoutReaderDone.load() || !stderrReaderDone.load()) &&
               GetTickCount64() < drainDeadline) {
            Sleep(10);
        }

        const bool forcedReaderStop =
            !stdoutReaderDone.load() || !stderrReaderDone.load();
        if (forcedReaderStop) {
            stopReaders.store(true);
            if (outThread.joinable() && !stdoutReaderDone.load())
                CancelThreadSynchronousIoLocal(outThread.native_handle());
            if (errThread.joinable() && !stderrReaderDone.load())
                CancelThreadSynchronousIoLocal(errThread.native_handle());
        }

        // Drain readers. They have either seen EOF after the job closed or
        // had their outstanding synchronous read cancelled above.
        if (outThread.joinable()) outThread.join();
        if (errThread.joinable()) errThread.join();

        // 7. Exit code.
        DWORD code = 0;
        if (GetExitCodeProcess(proc.h, &code))
            result.exitCode = static_cast<int>(code);
        else
            result.exitCode = -1;

        result.stdoutTruncated = stdoutTruncated.load();
        result.stderrTruncated = stderrTruncated.load();
        result.truncated = result.stdoutTruncated || result.stderrTruncated;
        if (result.stdoutTruncated) TrimIncompleteUtf8Tail(result.stdoutText);
        if (result.stderrTruncated) TrimIncompleteUtf8Tail(result.stderrText);

        // Strip PowerShell CLIXML serialization noise from both streams.
        // Must happen BEFORE the cancel/timeout breadcrumb below — we
        // don't want to scrub those.
        StripClixmlInPlace(result.stdoutText);
        StripClixmlInPlace(result.stderrText);

        // Fold runs of "<src> -> <dst> done" lines (MSBuild/vcpkg
        // app-local DLL copies: ~4 KB per build run) into one summary
        // line each.  Shape-matched, never folds errors/warnings/the
        // .vcxproj -> .exe line; see copy_line_fold.h.  Before the
        // breadcrumbs and large-output handling, so neither sees the
        // noise.
        lb_copyfold::FoldCopyProgressLines(result.stdoutText);
        lb_copyfold::FoldCopyProgressLines(result.stderrText);

        // Progress bars (curl/git/pip/tqdm) redraw with bare '\r' on
        // stderr; PS 5.1 wraps redirected native stderr in multi-line
        // NativeCommandError records.  Collapse both, and when stderr is
        // nothing but progress, move a one-line summary to stdout so a
        // successful download is not rendered (or scored) as a failure.
        // See progress_output_fold.h.
        lb_progressfold::TidyCapturedStreams(result.stdoutText, result.stderrText);

        // Lingering MSVC helpers (mspdbsrv, vctip) are identified by
        // process name and outlive FAILED builds exactly as they outlive
        // successful ones, so the exit code does not decide the
        // classification, only the wording.  Otherwise every failed
        // MSBuild run would get a misleading "[background process
        // stopped]" warning next to the real compiler or test failure.
        result.buildHelpersCleaned = result.descendantsTerminated &&
            buildHelpersOnly && !result.timedOut &&
            !result.cancelled && !killIt && !forcedReaderStop;

        if (result.buildHelpersCleaned && result.exitCode == 0 &&
            result.stderrText.empty()) {
            if (!result.stdoutText.empty() && result.stdoutText.back() != '\n')
                result.stdoutText += "\r\n";
            result.stdoutText +=
                "[build helper cleanup] Command exited with code 0. "
                "LlamaBoss stopped remaining MSVC compiler helpers after "
                "the build command ended. This is informational cleanup, "
                "not a tool failure. Continue the requested work when the "
                "expected build outputs have been verified.\r\n"
                "[remaining-process check] " + descendantDetails + "\r\n";
        } else if (result.buildHelpersCleaned) {
            // Failed (or stderr-producing) build: one neutral line in
            // stdout, so stderr carries only the command's own errors.
            if (!result.stdoutText.empty() && result.stdoutText.back() != '\n')
                result.stdoutText += "\r\n";
            result.stdoutText +=
                "[build helper cleanup] LlamaBoss stopped leftover MSVC "
                "compiler helpers (" + descendantDetails + ") after the "
                "command ended. Unrelated to the exit code " +
                std::to_string(result.exitCode) + "; diagnose the "
                "command's own output.\r\n";
        } else if (result.descendantsTerminated) {
            if (!result.stderrText.empty() && result.stderrText.back() != '\n')
                result.stderrText += "\r\n";
            result.stderrText +=
                "[background process stopped] PowerShell exited, but the "
                "job still reported child processes after the 1.5s cleanup "
                "grace period. LlamaBoss closed the job to stop remaining "
                "children. This warning alone does not mean the command "
                "failed; check its exit code, output and expected files.\r\n"
                "[remaining-process check] " + descendantDetails + "\r\n";
        }

        if (forcedReaderStop) {
            if (!result.stderrText.empty() && result.stderrText.back() != '\n')
                result.stderrText += "\r\n";
            result.stderrText +=
                "[output capture warning] A process kept an output pipe open; "
                "LlamaBoss stopped waiting for that pipe after 2s.\r\n";
        }

        // If we killed and nothing was written to stderr, leave a
        // breadcrumb the display layer can style.  On timeout, make the
        // breadcrumb CORRECTIVE for the model, not just diagnostic.  Two
        // common failure modes:
        //   * a script that silently scans far more than asked (build
        //     trees, IDE caches) and emits nothing before the deadline;
        //   * a process blocked on input nobody can see: an interactive
        //     prompt, or a GUI dialog (e.g. a debug assert box in a test
        //     executable turning a test run into a silent timeout).
        // The timeout breadcrumb is appended even when stderr already
        // has text, so a process that printed warnings and then hung
        // still gets an explanation.
        if (result.cancelled && result.stderrText.empty()) {
            result.stderrText = "[cancelled by user]\r\n";
        } else if (result.timedOut) {
            const bool noOutput = IsBlankTextLocal(result.stdoutText) &&
                                  IsBlankTextLocal(result.stderrText);
            if (!result.stderrText.empty() && result.stderrText.back() != '\n')
                result.stderrText += "\r\n";
            result.stderrText +=
                "[timed out after " +
                std::to_string(m_timeoutMs / 1000) + "s" +
                (noOutput ? " with no output" : "") +
                "; LlamaBoss killed the process tree. Likely causes: "
                "(1) it was waiting for input this tool cannot see or "
                "answer: an interactive prompt (Read-Host, a y/n or "
                "credential prompt) or a GUI window such as a debug "
                "assert, crash or error dialog (common for Debug-built "
                "test executables). Run it non-interactively (-y, --batch, "
                "-NonInteractive, -Confirm:$false, redirected stdin), use "
                "a Release build or disable assert/crash dialogs, and do "
                "not rerun the same command unchanged. (2) it did far more "
                "work than intended" +
                (noOutput
                     ? ": if it scans or compresses directories, exclude "
                       "build/IDE folders such as .vs, .git, x64, Debug, "
                       "Release, node_modules, __pycache__, and emit progress "
                       "as it works so partial output survives a timeout"
                     : "; the last output above shows where it stopped") +
                ".]\r\n";
        }

        // Workspace delta: compute BEFORE large-output externalization
        // (the externalized .txt lands under the chat root and must not
        // self-report as a created file), but append the manifest text
        // AFTER it (so the manifest stays in the inline preview instead
        // of being swept into the saved output file).
        std::string wsManifest;
        bool stdoutWasBlank = IsBlankTextLocal(result.stdoutText);
        if (trackWorkspace) {
            wsManifest = ApplyWorkspaceDeltaLocal(result, wsBefore, m_cwd);
        }

        AttachExplicitPowerShellArtifactsLocal(
            result, m_cwd, m_activeProjectRoot);

        ApplyLargeOutputHandlingLocal(result, m_cwd);

        if (!wsManifest.empty()) {
            if (!result.stdoutText.empty() && result.stdoutText.back() != '\n')
                result.stdoutText += "\r\n";
            if (!result.stdoutText.empty()) result.stdoutText += "\r\n";
            result.stdoutText += wsManifest;
        }

        // Silent-no-op footgun hint (see ps_command_hints.h).  Only when
        // the command "succeeded" with nothing to show for it: exit 0,
        // no output, and no workspace changes — exactly the shape that
        // makes a local model hallucinate success.
        const bool noWorkspaceEffect =
            wsManifest.empty() ||
            wsManifest.find("no files were created") != std::string::npos;
        if (result.exitCode == 0 && !result.timedOut && !result.cancelled &&
            stdoutWasBlank && noWorkspaceEffect) {
            std::string hint =
                ps_command_hints::GetChildItemIncludeHint(result.command);
            if (!hint.empty()) {
                if (!result.stdoutText.empty() && result.stdoutText.back() != '\n')
                    result.stdoutText += "\r\n";
                result.stdoutText += hint;
                result.stdoutText += "\r\n";
            }
        }

        // Wildcard -Path + -Recurse -File: surfaced on any successful
        // run, not only blank ones -- the form can return a partial
        // result, and a plain folder path is always the fix.
        if (result.exitCode == 0 && !result.timedOut && !result.cancelled) {
            std::string hint =
                ps_command_hints::GetChildItemWildcardRecurseHint(result.command);
            if (!hint.empty()) {
                if (!result.stdoutText.empty() && result.stdoutText.back() != '\n')
                    result.stdoutText += "\r\n";
                result.stdoutText += hint;
                result.stdoutText += "\r\n";
            }
        }
    }

    // Post the completion event back to the UI thread iff the frame
    // is still alive.  We intentionally don't deliver on destruction
    // — the handler would dereference freed UI state.
    void PostCompletion(CmdResult result) {
        auto* ev = new wxCommandEvent(wxEVT_CMD_COMPLETE);
        ev->SetClientObject(new CmdResultClientData(std::move(result)));
        LbQueueEventIfAlive(m_evtHandler, m_aliveToken, ev);
    }

    wxEvtHandler*                      m_evtHandler;
    std::string                        m_command;
    std::string                        m_cwd;        // UTF-8; empty -> %USERPROFILE%
    std::string                        m_activeProjectRoot; // trusted artifact root
    unsigned long                      m_timeoutMs;  // resolved (>0)
    std::shared_ptr<std::atomic<bool>> m_cancelFlag;
    std::shared_ptr<std::atomic<bool>> m_runningFlag;
    std::weak_ptr<std::atomic<bool>>   m_aliveToken;
};

} // namespace

// ── Shell version (for the agent prompt) ─────────────────────────
bool LbToolUsesPowerShell7()
{
    return ResolvePowerShell().isPwsh;
}

std::string LbPowerShellPromptNote()
{
    if (LbToolUsesPowerShell7()) {
        return "The powershell tool runs PowerShell 7 (pwsh.exe, modern .NET), "
               "not Windows PowerShell 5.1: newer .NET APIs such as "
               "[IO.Path]::GetRelativePath are available, && and || chain "
               "commands, Get-WmiObject does not exist (use Get-CimInstance), "
               "use -AsByteStream instead of -Encoding Byte, and UTF8 file "
               "writes have no BOM.";
    }
    return "The powershell tool runs Windows PowerShell 5.1 (.NET Framework): "
           "newer .NET APIs such as [IO.Path]::GetRelativePath are "
           "unavailable, and && / || chaining and the ?: ternary do not exist.";
}


// ─── CmdExecutor ─────────────────────────────────────────────────

CmdExecutor::CmdExecutor(wxEvtHandler* eventHandler,
                         std::weak_ptr<std::atomic<bool>> aliveToken)
    : m_eventHandler(eventHandler)
    , m_aliveToken(std::move(aliveToken))
    , m_cancelFlag(std::make_shared<std::atomic<bool>>(false))
    , m_isRunning(std::make_shared<std::atomic<bool>>(false))
{}

CmdExecutor::~CmdExecutor() {
    // Best-effort: signal cancel so any live worker tears down its
    // job object quickly.  The worker itself is detached — it will
    // NOT post completion because m_aliveToken will already be false
    // by the time the frame is gone.
    if (m_cancelFlag) m_cancelFlag->store(true);
}

bool CmdExecutor::Start(const std::string& command) {
    return Start(command, std::string{}, kDefaultTimeoutMs, std::string{});
}

bool CmdExecutor::Start(const std::string& command,
                        const std::string& cwd,
                        unsigned long      timeoutMs,
                        const std::string& activeProjectRoot)
{
    // Reject empty / whitespace-only input.
    bool anyNonWs = false;
    for (char c : command) {
        if (!std::isspace((unsigned char)c)) { anyNonWs = true; break; }
    }
    if (!anyNonWs) return false;

    // Test-and-set atomically so two callers cannot race into two workers
    // if Start() is ever reached off the UI thread.
    bool expected = false;
    if (!m_isRunning->compare_exchange_strong(expected, true))
        return false;

    // Reset cancel flag for the new run after we own the running slot.
    m_cancelFlag->store(false);

    auto* worker = new CmdWorkerThread(
        m_eventHandler,
        command,
        cwd,
        activeProjectRoot,
        timeoutMs,
        m_cancelFlag,
        m_isRunning,
        m_aliveToken);

    if (worker->Run() != wxTHREAD_NO_ERROR) {
        delete worker;
        m_isRunning->store(false);

        // Surface the failure as a synthetic error event so the UI
        // follows the same completion path as a real failure.
        auto* ev = new wxCommandEvent(wxEVT_CMD_ERROR);
        ev->SetString("Failed to start command worker thread.");
        LbQueueEventIfAlive(m_eventHandler, m_aliveToken, ev);
        return false;
    }

    return true;
}

void CmdExecutor::Cancel() {
    if (m_cancelFlag) m_cancelFlag->store(true);
}
