// cmd_executor.cpp
//
// Phase 1 PowerShell /cmd runner.  See cmd_executor.h for lifetime notes.
//
// Key Windows idioms used:
//   - CreateProcessW with CREATE_SUSPENDED | CREATE_NO_WINDOW so we can
//     assign the child to a Job Object before it runs.
//   - Job Object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE so a single
//     CloseHandle() tears down the child AND any grandchildren cleanly
//     (important — PowerShell readily spawns sub-processes).
//   - Two anonymous pipes (one each for stdout and stderr) with the
//     write ends marked inheritable; parent reads via two std::threads
//     so neither stream can block the child by filling a pipe buffer.
//   - powershell.exe -NoProfile -NonInteractive
//                    -OutputFormat Text -EncodedCommand <b64>
//     where the encoded payload is UTF-16LE of the UTF-8 console-encoding
//     prefix plus the user's command.  Using -EncodedCommand makes
//     quoting a non-issue for anything the user types.
//
#include "cmd_executor.h"

// wx
#include <wx/log.h>
#include <wx/filename.h>

// std
#include <algorithm>
#include <fstream>
#include <chrono>
#include <cctype>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// Poco (Base64 — already linked via existing usage in LlamaBoss.cpp)
#include <Poco/Base64Encoder.h>

// Win32
#define NOMINMAX
#include <windows.h>

// ─── Event definitions ───────────────────────────────────────────
wxDEFINE_EVENT(wxEVT_CMD_COMPLETE, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_CMD_ERROR,    wxCommandEvent);

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
// than continuing.  For Phase 1 user-typed one-liners, this is correct —
// you want to see the error and know it failed.  Users who need partial-
// success can pass `-ErrorAction SilentlyContinue` on the offending cmdlet.
std::wstring BuildPowerShellPayload(const std::string& userCommand) {
    const std::string prefix =
        "$ProgressPreference = 'SilentlyContinue';"
        "[Console]::OutputEncoding = [System.Text.Encoding]::UTF8;"
        "$OutputEncoding = [System.Text.Encoding]::UTF8;"
        "$ErrorActionPreference = 'Stop';"
        "trap { "
            "[Console]::Error.WriteLine($_.ToString()); "
            "exit 1 "
        "}\r\n";
    return Utf8ToWide(prefix + userCommand);
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

std::string ParentDirLocal(const std::string& path)
{
    std::string clean = TrimTrailingSeparatorsLocal(path);
    size_t pos = clean.find_last_of("/\\");
    return (pos == std::string::npos) ? std::string() : clean.substr(0, pos);
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

std::string WorkflowRootFromCwdLocal(const std::string& cwd)
{
    std::string clean = TrimTrailingSeparatorsLocal(cwd);
    if (clean.empty()) return std::string();
    if (LowerForOutputLocal(BaseNameLocal(clean)) != "workspace") return std::string();

    std::string chatRoot = ParentDirLocal(clean);
    std::string workflows = ParentDirLocal(chatRoot);
    if (chatRoot.empty() || workflows.empty()) return std::string();
    if (!StartsWithLocal(BaseNameLocal(chatRoot), "chat_")) return std::string();
    if (LowerForOutputLocal(BaseNameLocal(workflows)) != "workflows") return std::string();
    return chatRoot;
}

std::string UserProfileUtf8Local()
{
    return WideToUtf8(ResolveUserProfileDir());
}

std::string ToolOutputsDirForCwdLocal(const std::string& cwd)
{
    std::string root = WorkflowRootFromCwdLocal(cwd);
    if (root.empty()) {
        std::string profile = UserProfileUtf8Local();
        root = profile.empty() ? std::string(".") : JoinPathLocal(profile, "LlamaBoss");
    }
    return JoinPathLocal(root, "ToolOutputs");
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
    ExternalizeOneStreamLocal(result, result.stdoutText, cwd, stem, "stdout", result.truncated);
    ExternalizeOneStreamLocal(result, result.stderrText, cwd, stem, "stderr", result.truncated);
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
// to the user and burns context budget downstream (harness phase).
// Handles multiple blocks in a single buffer and trims a single trailing
// newline per block so the surrounding text doesn't gain blank lines.
void StripClixmlInPlace(std::string& s) {
    const std::string marker = "#< CLIXML";
    const std::string close  = "</Objs>";
    size_t pos = 0;
    while ((pos = s.find(marker, pos)) != std::string::npos) {
        size_t end = s.find(close, pos);
        if (end == std::string::npos) break;  // malformed — leave alone
        end += close.size();
        // Consume one trailing \r and/or \n so we don't leave blank gaps.
        if (end < s.size() && s[end] == '\r') ++end;
        if (end < s.size() && s[end] == '\n') ++end;
        s.erase(pos, end - pos);
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

// Pipe reader: drains `readEnd` into `dest` (under `mutex`), stopping
// early after kMaxOutputBytes have been accepted (further bytes are
// discarded but the pipe is still drained so the child doesn't block).
void ReaderLoop(HANDLE readEnd, std::string& dest, std::mutex& destMutex,
                std::atomic<bool>& truncatedFlag)
{
    constexpr DWORD kChunk = 4096;
    char buf[kChunk];
    for (;;) {
        DWORD got = 0;
        BOOL ok = ReadFile(readEnd, buf, kChunk, &got, nullptr);
        if (!ok || got == 0) break;  // pipe closed or error -> done

        std::lock_guard<std::mutex> lk(destMutex);
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
}

// ─── Worker thread ───────────────────────────────────────────────

class CmdWorkerThread : public wxThread {
public:
    CmdWorkerThread(wxEvtHandler* evtHandler,
                    const std::string& command,
                    const std::string& cwd,
                    unsigned long      timeoutMs,
                    std::shared_ptr<std::atomic<bool>> cancelFlag,
                    std::shared_ptr<std::atomic<bool>> runningFlag,
                    std::weak_ptr<std::atomic<bool>> aliveToken)
        : wxThread(wxTHREAD_DETACHED)
        , m_evtHandler(evtHandler)
        , m_command(command)
        , m_cwd(cwd)
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
        // sees us as idle.  This is safe because nobody else writes
        // to this flag.
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
        std::wstring wCmdLine =
            L"powershell.exe -NoProfile -NonInteractive "
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
        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput  = nullptr;             // no stdin for /cmd
        si.hStdOutput = outW.h;
        si.hStdError  = errW.h;

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

        BOOL ok = CreateProcessW(
            nullptr,
            cmdBuf.data(),
            nullptr, nullptr,
            TRUE,                                           // bInheritHandles
            CREATE_NO_WINDOW | CREATE_SUSPENDED,
            nullptr,                                        // inherit env
            cwdArg,
            &si,
            &pi
        );
        if (!ok) {
            DWORD err = GetLastError();
            result.stderrText = "CreateProcess(powershell.exe) failed, "
                                "error=" + std::to_string(err);
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
        std::mutex outMu, errMu;
        std::atomic<bool> truncated(false);
        std::thread outThread(ReaderLoop,
            outR.h, std::ref(result.stdoutText), std::ref(outMu),
            std::ref(truncated));
        std::thread errThread(ReaderLoop,
            errR.h, std::ref(result.stderrText), std::ref(errMu),
            std::ref(truncated));

        // 6. Wait loop: poll process with timeout, observe cancel
        //    flag, enforce the overall deadline.
        constexpr DWORD kTickMs = 200;
        double deadline = NowSec() + (m_timeoutMs / 1000.0);

        bool killIt = false;
        for (;;) {
            DWORD wr = WaitForSingleObject(proc.h, kTickMs);
            if (wr == WAIT_OBJECT_0) break;           // child exited
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
            // Closing the job handle triggers KILL_ON_JOB_CLOSE on
            // the entire process tree.
            CloseHandle(job.release());
            // Give the OS a beat to deliver the kill so readers unblock.
            WaitForSingleObject(proc.h, 2000);
        }

        // Drain readers.
        if (outThread.joinable()) outThread.join();
        if (errThread.joinable()) errThread.join();

        // 7. Exit code.
        DWORD code = 0;
        if (GetExitCodeProcess(proc.h, &code))
            result.exitCode = static_cast<int>(code);
        else
            result.exitCode = -1;

        result.truncated = truncated.load();

        // Strip PowerShell CLIXML serialization noise from both streams.
        // Must happen BEFORE the cancel/timeout breadcrumb below — we
        // don't want to scrub those.
        StripClixmlInPlace(result.stdoutText);
        StripClixmlInPlace(result.stderrText);

        // If we killed and nothing was written to stderr, leave a
        // breadcrumb the display layer can style.
        if (result.cancelled && result.stderrText.empty())
            result.stderrText = "[cancelled by user]\r\n";
        else if (result.timedOut && result.stderrText.empty())
            result.stderrText = "[timed out]\r\n";

        ApplyLargeOutputHandlingLocal(result, m_cwd);
    }

    // Post the completion event back to the UI thread iff the frame
    // is still alive.  We intentionally don't deliver on destruction
    // — the handler would dereference freed UI state.
    void PostCompletion(CmdResult result) {
        auto alive = m_aliveToken.lock();
        if (!alive || !alive->load()) return;

        auto* ev = new wxCommandEvent(wxEVT_CMD_COMPLETE);
        ev->SetClientObject(new CmdResultClientData(std::move(result)));
        wxQueueEvent(m_evtHandler, ev);
    }

    wxEvtHandler*                      m_evtHandler;
    std::string                        m_command;
    std::string                        m_cwd;        // UTF-8; empty -> %USERPROFILE%
    unsigned long                      m_timeoutMs;  // resolved (>0)
    std::shared_ptr<std::atomic<bool>> m_cancelFlag;
    std::shared_ptr<std::atomic<bool>> m_runningFlag;
    std::weak_ptr<std::atomic<bool>>   m_aliveToken;
};

} // namespace

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
    return Start(command, std::string{}, kDefaultTimeoutMs);
}

bool CmdExecutor::Start(const std::string& command,
                        const std::string& cwd,
                        unsigned long      timeoutMs)
{
    if (IsRunning()) return false;

    // Reject empty / whitespace-only input.
    bool anyNonWs = false;
    for (char c : command) {
        if (!std::isspace((unsigned char)c)) { anyNonWs = true; break; }
    }
    if (!anyNonWs) return false;

    // Reset cancel flag for the new run; mark running up-front so a
    // double-click on Send can't race us into two workers.
    m_cancelFlag->store(false);
    m_isRunning->store(true);

    auto* worker = new CmdWorkerThread(
        m_eventHandler,
        command,
        cwd,
        timeoutMs,
        m_cancelFlag,
        m_isRunning,
        m_aliveToken);

    if (worker->Run() != wxTHREAD_NO_ERROR) {
        delete worker;
        m_isRunning->store(false);

        // Surface the failure as a synthetic error event so the UI
        // follows the same completion path as a real failure.
        auto alive = m_aliveToken.lock();
        if (alive && alive->load()) {
            auto* ev = new wxCommandEvent(wxEVT_CMD_ERROR);
            ev->SetString("Failed to start command worker thread.");
            wxQueueEvent(m_eventHandler, ev);
        }
        return false;
    }

    return true;
}

void CmdExecutor::Cancel() {
    if (m_cancelFlag) m_cancelFlag->store(true);
}
