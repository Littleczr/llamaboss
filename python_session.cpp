// python_session.cpp
//
// RLM step 2, phase S1 — persistent Python session.  See
// python_session.h for the architecture and failure-semantics
// contract.  The Windows process model (pipes, Job Object,
// CREATE_SUSPENDED -> AssignProcessToJobObject -> ResumeThread,
// launcher candidates, CancelSynchronousIo unblock) deliberately
// mirrors python_runner.cpp so both Python backends fail the same
// way; the structural difference is that this process stays ALIVE
// between calls and the wait primitive is therefore pipe data
// (PeekNamedPipe tick loop), not process exit.

#include "python_session.h"

#include "path_safety.h"
#include "lb_string_utils.h"   // LbUtf8SafeTruncate
#include "python_resources.h"  // embedded lb_kernel.py (RCDATA)

#include <condition_variable>
#include <cwctype>
#include <iterator>
#include "ui_event_post.h"

#include "lb_windows.h"

wxDEFINE_EVENT(wxEVT_PY_SESSION_COMPLETE, wxCommandEvent);

namespace {

// ═══════════════════════════════════════════════════════════════════
//  Embedded kernel
// ═══════════════════════════════════════════════════════════════════
//
// Written to <session temp dir>\lb_kernel.py at spawn, refreshed every
// spawn so a LlamaBoss update can never run a stale kernel.  Uses only
// the standard library and no syntax newer than Python 3.6; the ast
// REPL split mutates tree.body in place instead of constructing
// ast.Module, so it does not depend on the 3.8 type_ignores signature.
//
// fd discipline (the whole point — see python_session.h):
//   proto_in / proto_out / crash_out are private dup()s of the
//   original stdin/stdout/stderr pipes.  fd 0 is then repointed to
//   NUL and fds 1/2 to the O_APPEND capture files, so user code, C
//   extensions, and grandchildren can never touch the frame channel.
//   crash_out keeps the original stderr pipe open as a crash channel:
//   kernel-internal fatal errors are written there, where the C++
//   drain thread is listening.
//
// O_APPEND matters: sys.stdout gets its own fd onto the same file, and
// per-exec resets use ftruncate; with independent file offsets,
// non-append writers would overwrite each other after a truncate.
// Kernel source: assets/python/lb_kernel.py, embedded as RCDATA LB_KERNEL
// (python_helpers.rc2) and loaded through lb_pyres::Load.

// ═══════════════════════════════════════════════════════════════════
//  Small Win32 helpers (local copies — python_runner.cpp keeps its
//  own in an anonymous namespace, so these cannot be shared without
//  a refactor that S1 deliberately avoids)
// ═══════════════════════════════════════════════════════════════════

std::wstring Utf8ToWide(const std::string& s)
{
    return path_safety::Utf8ToWide(s);
}

double NowSec()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
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

// Same rationale and mechanism as python_runner.cpp / cmd_executor.cpp:
// unblock a drain thread parked in a synchronous ReadFile when the
// job-object kill alone does not deliver EOF (something outside the
// job inherited a write handle).
void CancelThreadSynchronousIoLocal(HANDLE threadHandle)
{
    if (!threadHandle) return;
    using Fn = BOOL (WINAPI *)(HANDLE);
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    if (!kernel) return;
    auto* fn = reinterpret_cast<Fn>(GetProcAddress(kernel, "CancelSynchronousIo"));
    if (fn) fn(threadHandle);
}

// Full Windows CRT argv quoting — byte-identical policy to
// python_runner.cpp's QuoteArg (trailing backslashes doubled so an
// argument ending in \ cannot escape the closing quote).
std::wstring QuoteArg(const std::wstring& arg)
{
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

// Lossy UTF-8 sanitizer for bytes we did NOT receive through the
// (ensure_ascii) frame channel — i.e. salvaged capture files and the
// crash-channel drain.  Invalid sequences become '?' so malformed
// subprocess bytes can never flow into llama-server JSON.
std::string SanitizeUtf8Lossy(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    size_t i = 0;
    const size_t n = in.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        size_t len = 0;
        if      (c < 0x80)           len = 1;
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (len == 0 || i + len > n) { out.push_back('?'); ++i; continue; }
        bool okSeq = true;
        for (size_t k = 1; k < len; ++k) {
            if ((static_cast<unsigned char>(in[i + k]) & 0xC0) != 0x80) {
                okSeq = false;
                break;
            }
        }
        if (!okSeq) { out.push_back('?'); ++i; continue; }
        out.append(in, i, len);
        i += len;
    }
    return out;
}

// Normalized map key for a conversation workspace path: absolute,
// backslashed, lowercase — same spirit as ConversationRegistry's
// Normalize, without pulling wxFileName into worker-thread code.
std::string NormalizeKey(const std::string& cwdUtf8)
{
    std::wstring w = Utf8ToWide(cwdUtf8);
    if (w.empty()) return std::string();
    wchar_t buf[MAX_PATH * 4];
    DWORD got = GetFullPathNameW(w.c_str(),
                                 static_cast<DWORD>(std::size(buf)),
                                 buf, nullptr);
    std::wstring full = (got > 0 && got < std::size(buf))
                            ? std::wstring(buf, got)
                            : w;
    for (wchar_t& ch : full) {
        if (ch == L'/') ch = L'\\';
        else ch = static_cast<wchar_t>(::towlower(ch));
    }
    while (!full.empty() && full.back() == L'\\') full.pop_back();
    return path_safety::WideToUtf8(full);
}

std::string LastErrorString(const char* what)
{
    return std::string(what) + " failed, error=" + std::to_string(GetLastError());
}

// ═══════════════════════════════════════════════════════════════════
//  Per-session temp dir + kernel file
// ═══════════════════════════════════════════════════════════════════

bool EnsureDirectory(const std::wstring& dir)
{
    if (CreateDirectoryW(dir.c_str(), nullptr)) return true;
    const DWORD err = GetLastError();
    if (err == ERROR_ALREADY_EXISTS) return true;
    if (err != ERROR_PATH_NOT_FOUND) return false;
    const size_t slash = dir.find_last_of(L'\\');
    if (slash == std::wstring::npos || slash == 0) return false;
    if (!EnsureDirectory(dir.substr(0, slash))) return false;
    return CreateDirectoryW(dir.c_str(), nullptr) ||
           GetLastError() == ERROR_ALREADY_EXISTS;
}

std::wstring MakeSessionTempDir(std::string& errorOut)
{
    wchar_t tmp[MAX_PATH + 1] = {};
    const DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (n == 0 || n > MAX_PATH) {
        errorOut = LastErrorString("GetTempPath");
        return std::wstring();
    }
    static std::atomic<unsigned> s_counter{0};
    std::wstring base(tmp);
    if (!base.empty() && base.back() != L'\\') base.push_back(L'\\');
    std::wostringstream dir;
    dir << base << L"LlamaBoss\\PySessions\\s" << GetCurrentProcessId()
        << L'_' << s_counter.fetch_add(1)
        << L'_' << static_cast<unsigned long long>(GetTickCount64());
    const std::wstring path = dir.str();
    if (!EnsureDirectory(path)) {
        errorOut = "could not create session temp dir " +
                   path_safety::WideToUtf8(path);
        return std::wstring();
    }
    return path;
}

bool WriteKernelFile(const std::wstring& path, std::string& errorOut)
{
    const char* src  = nullptr;
    size_t      size = 0;
    if (!lb_pyres::Load(lb_pyres::kKernelName, src, size, errorOut))
        return false;
    HandleGuard f(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!f.h || f.h == INVALID_HANDLE_VALUE) {
        errorOut = LastErrorString("CreateFile(lb_kernel.py)");
        return false;
    }
    const DWORD total = static_cast<DWORD>(size);
    DWORD written = 0;
    if (!WriteFile(f.h, src, total, &written, nullptr) || written != total) {
        errorOut = LastErrorString("WriteFile(lb_kernel.py)");
        return false;
    }
    return true;
}

void CleanupSessionTempDir(const std::wstring& dir)
{
    if (dir.empty()) return;
    // Best effort — a straggling grandchild can hold a capture file
    // open briefly after the job kill; leftover temp dirs are inert.
    DeleteFileW((dir + L"\\lb_kernel.py").c_str());
    DeleteFileW((dir + L"\\out.log").c_str());
    DeleteFileW((dir + L"\\err.log").c_str());
    RemoveDirectoryW(dir.c_str());
}

std::string ReadFileCapped(const std::wstring& path, size_t cap,
                           bool& truncatedOut)
{
    truncatedOut = false;
    HandleGuard f(CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE |
                                  FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr));
    if (!f.h || f.h == INVALID_HANDLE_VALUE) return std::string();
    std::string out;
    char buf[8192];
    while (out.size() < cap) {
        DWORD got = 0;
        if (!ReadFile(f.h, buf,
                      static_cast<DWORD>(std::min(sizeof(buf),
                                                  cap - out.size())),
                      &got, nullptr) || got == 0)
            break;
        out.append(buf, got);
    }
    if (out.size() >= cap) {
        DWORD probe = 0;
        char one;
        if (ReadFile(f.h, &one, 1, &probe, nullptr) && probe == 1)
            truncatedOut = true;
    }
    return SanitizeUtf8Lossy(out);
}

// ═══════════════════════════════════════════════════════════════════
//  Crash-channel drain (original stderr pipe)
// ═══════════════════════════════════════════════════════════════════

constexpr size_t kStderrDrainCapBytes = 64 * 1024;

void StderrDrainLoop(HANDLE readEnd,
                     std::string* dest,
                     std::mutex* destMutex)
{
    constexpr DWORD kChunk = 4096;
    char buf[kChunk];
    for (;;) {
        DWORD got = 0;
        BOOL ok = ReadFile(readEnd, buf, kChunk, &got, nullptr);
        if (!ok || got == 0) break;
        std::lock_guard<std::mutex> lk(*destMutex);
        if (dest->size() < kStderrDrainCapBytes) {
            const size_t room = kStderrDrainCapBytes - dest->size();
            dest->append(buf, std::min<size_t>(got, room));
        }
    }
}

// ═══════════════════════════════════════════════════════════════════
//  PySession — one live kernel
// ═══════════════════════════════════════════════════════════════════

struct PySession {
    HANDLE proc    = nullptr;
    HANDLE job     = nullptr;
    HANDLE stdinW  = nullptr;   // frame channel out (we write requests)
    HANDLE stdoutR = nullptr;   // frame channel in  (we read responses)
    HANDLE stderrR = nullptr;   // crash channel     (drain thread reads)
    DWORD  pid     = 0;

    std::thread  drainThread;
    std::string  drainBuf;
    std::mutex   drainMutex;

    std::wstring tempDir;       // holds lb_kernel.py, out.log, err.log
    std::string  pythonCommand; // launcher label
    std::string  pythonVersion; // from the spawn ping
    std::string  key;           // normalized cwd this session belongs to

    unsigned              nextRequestId = 1;  // worker-thread only
    std::atomic<unsigned> execCount{0};
    std::atomic<double>   lastUsedSec{0.0};   // NowSec() at spawn and at each
                                              //   exec completion; ReapIdle reads
    std::atomic<bool>     dead{false};

    void Kill()
    {
        bool expected = false;
        if (!dead.compare_exchange_strong(expected, true)) return;
        if (job) TerminateJobObject(job, 1);
    }

    std::string DrainedStderr()
    {
        std::lock_guard<std::mutex> lk(drainMutex);
        return SanitizeUtf8Lossy(drainBuf);
    }

    bool ProcExited(DWORD* exitCodeOut) const
    {
        if (!proc) return true;
        if (WaitForSingleObject(proc, 0) != WAIT_OBJECT_0) return false;
        if (exitCodeOut) {
            DWORD code = 0;
            *exitCodeOut = GetExitCodeProcess(proc, &code)
                               ? code
                               : static_cast<DWORD>(-1);
        }
        return true;
    }

    ~PySession()
    {
        Kill();
        // Closing our write end delivers EOF on the (already dead)
        // kernel's stdin; harmless, and required so no handle leaks.
        if (stdinW)  { CloseHandle(stdinW);  stdinW  = nullptr; }
        // Give the OS a beat to finish the job kill so the drain
        // thread sees EOF on its own; unblock it explicitly otherwise
        // (leaked-writer paranoia, same as python_runner.cpp).
        if (proc) WaitForSingleObject(proc, 2000);
        if (drainThread.joinable()) {
            CancelThreadSynchronousIoLocal(
                static_cast<HANDLE>(drainThread.native_handle()));
            drainThread.join();
        }
        if (stdoutR) { CloseHandle(stdoutR); stdoutR = nullptr; }
        if (stderrR) { CloseHandle(stderrR); stderrR = nullptr; }
        if (proc)    { CloseHandle(proc);    proc    = nullptr; }
        if (job)     { CloseHandle(job);     job     = nullptr; }
        CleanupSessionTempDir(tempDir);
    }
};

// ═══════════════════════════════════════════════════════════════════
//  Frame I/O (C++ side)
// ═══════════════════════════════════════════════════════════════════

enum class FrameReadStatus { Ok, TimedOut, Cancelled, Broken, Desync };

bool WriteFrame(PySession& s, const std::string& jsonPayload,
                std::string& errorOut)
{
    const std::string wire =
        "LBPY1 " + std::to_string(jsonPayload.size()) + "\n" + jsonPayload;
    DWORD written = 0;
    if (!WriteFile(s.stdinW, wire.data(), static_cast<DWORD>(wire.size()),
                   &written, nullptr) ||
        written != wire.size()) {
        errorOut = LastErrorString("WriteFile(frame)");
        return false;
    }
    return true;
}

// Must match CAP in the embedded kernel.
constexpr unsigned long long kKernelCaptureCapBytes = 4ull * 1024 * 1024;

// Keeps a capture file from growing past what the kernel will ever read.
// The kernel reads CAP + 1 bytes (the extra byte is how it detects
// truncation), so anything beyond that is disk use with no reader.
// fds 1/2 are O_APPEND, so writers simply continue at the new end of
// file; this is called every read tick, which bounds a runaway script
// (or its subprocesses) to the cap plus one tick's worth of output
// instead of letting it fill the temp drive before the timeout.
void CapCaptureFile(const std::wstring& path)
{
    const unsigned long long keep = kKernelCaptureCapBytes + 1;
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
        return;
    const unsigned long long size =
        (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) |
        fad.nFileSizeLow;
    if (size <= keep) return;

    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    // Re-check through the handle immediately before cutting.  The size
    // above came from a path query; in between, the kernel may have
    // ftruncate'd the file to 0 for a new exec (run_exec resets both
    // captures).  SetEndOfFile at `keep` would then EXTEND the fresh
    // capture to ~4 MiB of zero bytes, which read_capture returns as the
    // new run's output, flagged truncated.  This only ever shrinks.
    // A residual window of a few instructions remains between this check
    // and SetEndOfFile; closing it fully would need the kernel and this
    // watchdog to coordinate resets, which the risk does not justify.
    LARGE_INTEGER cur = {};
    if (GetFileSizeEx(h, &cur) &&
        static_cast<unsigned long long>(cur.QuadPart) > keep) {
        LARGE_INTEGER pos;
        pos.QuadPart = static_cast<LONGLONG>(keep);
        if (SetFilePointerEx(h, pos, nullptr, FILE_BEGIN))
            SetEndOfFile(h);
    }
    CloseHandle(h);
}

// Reads exactly one response frame with a deadline, checking the
// cancel flag every tick.  No blocking reads: PeekNamedPipe gates
// every ReadFile, so a wedged kernel can never park this thread —
// the timeout path is always reachable.
FrameReadStatus ReadFrame(PySession& s,
                          const std::shared_ptr<std::atomic<bool>>& cancelFlag,
                          double deadlineSec,
                          std::string& payloadOut,
                          std::string& errorOut)
{
    constexpr DWORD  kTickMs        = 50;
    constexpr size_t kMaxFrameBytes = 32 * 1024 * 1024;  // sanity ceiling;
        // the kernel budgets each response to ~20 MiB of serialized
        // JSON (FIELD_BUDGET), so a header above this is desync, not data.

    std::string buf;
    size_t headerEnd  = std::string::npos;
    size_t payloadLen = 0;

    for (;;) {
        // Drain whatever is available right now.
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(s.stdoutR, nullptr, 0, nullptr, &avail,
                               nullptr)) {
                errorOut = LastErrorString("PeekNamedPipe");
                return FrameReadStatus::Broken;
            }
            if (avail == 0) break;
            char chunk[8192];
            DWORD got = 0;
            if (!ReadFile(s.stdoutR, chunk,
                          static_cast<DWORD>(std::min<DWORD>(
                              avail, sizeof(chunk))),
                          &got, nullptr) || got == 0) {
                errorOut = LastErrorString("ReadFile(frame)");
                return FrameReadStatus::Broken;
            }
            buf.append(chunk, got);
        }

        // Parse the header once we have a full line.
        if (headerEnd == std::string::npos) {
            const size_t nl = buf.find('\n');
            if (nl != std::string::npos) {
                if (nl > 32 || buf.compare(0, 6, "LBPY1 ") != 0) {
                    errorOut = "protocol desync: bad frame header";
                    return FrameReadStatus::Desync;
                }
                const std::string lenText = buf.substr(6, nl - 6);
                char* end = nullptr;
                const unsigned long long len =
                    std::strtoull(lenText.c_str(), &end, 10);
                if (!end || *end != '\0' || lenText.empty() ||
                    len > kMaxFrameBytes) {
                    errorOut = "protocol desync: bad frame length '" +
                               lenText + "'";
                    return FrameReadStatus::Desync;
                }
                headerEnd  = nl + 1;
                payloadLen = static_cast<size_t>(len);
            } else if (buf.size() > 64) {
                errorOut =
                    "protocol desync: no frame header in first 64 bytes";
                return FrameReadStatus::Desync;
            }
        }

        if (headerEnd != std::string::npos &&
            buf.size() >= headerEnd + payloadLen) {
            payloadOut = buf.substr(headerEnd, payloadLen);
            // Final cap before the result goes back; from here on the
            // manager's capture watchdog keeps the files bounded.
            if (!s.tempDir.empty()) {
                CapCaptureFile(s.tempDir + L"\\out.log");
                CapCaptureFile(s.tempDir + L"\\err.log");
            }
            return FrameReadStatus::Ok;
        }

        if (cancelFlag && cancelFlag->load())
            return FrameReadStatus::Cancelled;
        if (NowSec() >= deadlineSec)
            return FrameReadStatus::TimedOut;

        // A dead kernel with an empty pipe would otherwise spin until
        // the deadline; surface it as Broken immediately.  (Dead with
        // bytes still buffered: loop again and read them first.)
        if (s.ProcExited(nullptr)) {
            DWORD avail = 0;
            if (!PeekNamedPipe(s.stdoutR, nullptr, 0, nullptr, &avail,
                               nullptr) || avail == 0) {
                errorOut = "kernel process exited mid-request";
                return FrameReadStatus::Broken;
            }
            continue;
        }

        if (!s.tempDir.empty()) {
            CapCaptureFile(s.tempDir + L"\\out.log");
            CapCaptureFile(s.tempDir + L"\\err.log");
        }

        Sleep(kTickMs);
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Spawn
// ═══════════════════════════════════════════════════════════════════

struct SpawnCandidate {
    std::string  label;
    std::wstring exeAndFlags;
};

// Flag policy matches python_runner.cpp's BuildCandidates: -X utf8
// for codepage sanity, -B to keep the temp dir free of .pyc, -u so
// the crash channel is unbuffered.  No -I for the same reason
// documented there (user-site packages must stay importable — the
// whole point of a session is doing real work with installed
// packages).
const SpawnCandidate kSpawnCandidates[] = {
    { "py -3",   L"py.exe -3 -X utf8 -B -u " },
    { "python",  L"python.exe -X utf8 -B -u " },
    { "python3", L"python3.exe -X utf8 -B -u " },
};

// Spawns a kernel for `cwd` and validates it with a handshake ping.
// Each launcher candidate gets its own fresh temp dir (the failed
// session's destructor cleans its dir, so per-candidate dirs are the
// simplest correct ownership).  Returns nullptr with `errorOut`
// filled on failure.
std::shared_ptr<PySession> SpawnSession(
    const std::string& cwdUtf8,
    const std::string& key,
    const std::shared_ptr<std::atomic<bool>>& cancelFlag,
    std::string& errorOut)
{
    const std::wstring cwdW = Utf8ToWide(cwdUtf8);
    LPCWSTR cwdArg = cwdW.empty() ? nullptr : cwdW.c_str();

    std::ostringstream startErrors;

    for (const SpawnCandidate& cand : kSpawnCandidates) {
        if (cancelFlag && cancelFlag->load()) {
            errorOut = "cancelled before the session could start";
            return nullptr;
        }

        std::string dirError;
        const std::wstring tempDir = MakeSessionTempDir(dirError);
        if (tempDir.empty()) {
            errorOut = dirError;
            return nullptr;
        }
        const std::wstring kernelPath = tempDir + L"\\lb_kernel.py";
        std::string writeError;
        if (!WriteKernelFile(kernelPath, writeError)) {
            errorOut = writeError;
            CleanupSessionTempDir(tempDir);
            return nullptr;
        }

        SECURITY_ATTRIBUTES sa = {};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        // stdin: kernel reads, we write.  4 MiB buffer so a maximal
        // request frame (kMaxCodeBytes of code, worst-case JSON
        // escaping) fits in one never-blocking WriteFile even if the
        // kernel has wedged — the timeout path stays reachable.
        HANDLE inR_raw = nullptr, inW_raw = nullptr;
        if (!CreatePipe(&inR_raw, &inW_raw, &sa, 4 * 1024 * 1024)) {
            errorOut = LastErrorString("CreatePipe(stdin)");
            CleanupSessionTempDir(tempDir);
            return nullptr;
        }
        HandleGuard inR(inR_raw), inW(inW_raw);

        HANDLE outR_raw = nullptr, outW_raw = nullptr;
        if (!CreatePipe(&outR_raw, &outW_raw, &sa, 0)) {
            errorOut = LastErrorString("CreatePipe(stdout)");
            CleanupSessionTempDir(tempDir);
            return nullptr;
        }
        HandleGuard outR(outR_raw), outW(outW_raw);

        HANDLE errR_raw = nullptr, errW_raw = nullptr;
        if (!CreatePipe(&errR_raw, &errW_raw, &sa, 0)) {
            errorOut = LastErrorString("CreatePipe(stderr)");
            CleanupSessionTempDir(tempDir);
            return nullptr;
        }
        HandleGuard errR(errR_raw), errW(errW_raw);

        // Parent-side ends must not leak into the child.
        SetHandleInformation(inW.h,  HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(outR.h, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(errR.h, HANDLE_FLAG_INHERIT, 0);

        HandleGuard job(CreateJobObjectW(nullptr, nullptr));
        if (!job.h) {
            errorOut = LastErrorString("CreateJobObject");
            CleanupSessionTempDir(tempDir);
            return nullptr;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
        jeli.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
            JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        // Per-process cap, not a kill switch: past the limit, commits
        // FAIL, Python raises MemoryError, and the session survives
        // with its state — the failure surfaces as a normal exception
        // card instead of a state-lost kill.
        jeli.ProcessMemoryLimit = static_cast<SIZE_T>(
            PythonSessionManager::kSessionMemoryCapBytes);
        if (!SetInformationJobObject(job.h,
                                     JobObjectExtendedLimitInformation,
                                     &jeli, sizeof(jeli))) {
            errorOut = LastErrorString("SetInformationJobObject");
            CleanupSessionTempDir(tempDir);
            return nullptr;
        }

        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput  = inR.h;
        si.hStdOutput = outW.h;
        si.hStdError  = errW.h;

        std::wstring cmd = cand.exeAndFlags + QuoteArg(kernelPath) + L" " +
                           QuoteArg(tempDir);
        std::vector<wchar_t> cmdLine(cmd.begin(), cmd.end());
        cmdLine.push_back(L'\0');

        PROCESS_INFORMATION pi = {};
        BOOL ok = CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr,
                                 TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                                 nullptr, cwdArg, &si, &pi);
        if (!ok) {
            startErrors << cand.label << ": CreateProcess failed, error="
                        << GetLastError() << "\n";
            CleanupSessionTempDir(tempDir);
            continue;  // next launcher candidate
        }

        HandleGuard proc(pi.hProcess);
        HandleGuard thr(pi.hThread);

        if (!AssignProcessToJobObject(job.h, proc.h)) {
            const DWORD err = GetLastError();
            TerminateProcess(proc.h, 1);
            errorOut = "AssignProcessToJobObject failed, error=" +
                       std::to_string(err);
            CleanupSessionTempDir(tempDir);
            return nullptr;
        }
        if (ResumeThread(thr.h) == (DWORD)-1) {
            const DWORD err = GetLastError();
            TerminateProcess(proc.h, 1);
            errorOut = "ResumeThread failed, error=" + std::to_string(err);
            CleanupSessionTempDir(tempDir);
            return nullptr;
        }

        // Child-side pipe ends close NOW, not at scope end: for the
        // session's lifetime only the kernel may hold them, or EOF
        // semantics (and the drain thread's exit) break.
        CloseHandle(inR.release());
        CloseHandle(outW.release());
        CloseHandle(errW.release());

        auto session = std::make_shared<PySession>();
        session->proc    = proc.release();
        session->job     = job.release();
        session->stdinW  = inW.release();
        session->stdoutR = outR.release();
        session->stderrR = errR.release();
        session->pid     = pi.dwProcessId;
        session->tempDir = tempDir;
        session->pythonCommand = cand.label;
        session->key     = key;
        session->lastUsedSec.store(NowSec());

        session->drainThread = std::thread(StderrDrainLoop,
                                           session->stderrR,
                                           &session->drainBuf,
                                           &session->drainMutex);

        // Handshake ping — proves the launcher is a working Python 3,
        // the kernel initialized its fd discipline, and the frame
        // channel round-trips.
        Poco::JSON::Object ping;
        ping.set("id", 0);
        ping.set("op", "ping");
        std::ostringstream pingJson;
        ping.stringify(pingJson);

        std::string ioError;
        bool handshakeOk = false;
        if (WriteFrame(*session, pingJson.str(), ioError)) {
            std::string payload;
            const double deadline =
                NowSec() +
                PythonSessionManager::kSpawnPingTimeoutMs / 1000.0;
            const FrameReadStatus st = ReadFrame(*session, cancelFlag,
                                                 deadline, payload, ioError);
            if (st == FrameReadStatus::Ok) {
                try {
                    Poco::JSON::Parser parser;
                    auto obj = parser.parse(payload)
                                   .extract<Poco::JSON::Object::Ptr>();
                    if (obj && obj->optValue<bool>("ok", false)) {
                        session->pythonVersion =
                            obj->optValue<std::string>("python", "");
                        handshakeOk = true;
                    } else {
                        ioError = "handshake ping was rejected by the kernel";
                    }
                } catch (const std::exception& e) {
                    ioError = std::string("handshake parse error: ") +
                              e.what();
                }
            } else if (st == FrameReadStatus::Cancelled) {
                errorOut = "cancelled during session startup";
                return nullptr;  // destructor kills + cleans up
            } else if (ioError.empty()) {
                ioError = (st == FrameReadStatus::TimedOut)
                              ? "handshake timed out"
                              : "handshake failed";
            }
        }

        if (handshakeOk) return session;

        const std::string drained = session->DrainedStderr();
        startErrors << cand.label << ": " << ioError;
        if (!drained.empty()) startErrors << " | stderr: " << drained;
        startErrors << "\n";
        session.reset();  // kill + drain-join + temp-dir cleanup
    }

    errorOut = "Could not start a Python session. Tried py -3, python, "
               "and python3.\n" + startErrors.str();
    return nullptr;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════
//  PythonSessionManager::Impl
// ═══════════════════════════════════════════════════════════════════
//
// Held by shared_ptr and captured BY shared_ptr into every worker, so
// a worker outliving the manager (frame closed mid-exec) keeps the
// mutex/map/flags alive until it finishes.  The final PostResult is
// absorbed by the alive-token guard in that case.

struct PythonSessionManager::Impl {
    wxEvtHandler*                      eventHandler = nullptr;
    std::weak_ptr<std::atomic<bool>>   aliveToken;
    std::shared_ptr<std::atomic<bool>> cancelFlag =
        std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> isRunning =
        std::make_shared<std::atomic<bool>>(false);

    // Guards `sessions` and `shutdown`.  Held only for map operations
    // and liveness checks — never across the frame exchange.
    mutable std::mutex mutex;
    std::map<std::string, std::shared_ptr<PySession>> sessions;
    bool shutdown = false;

    // Why the last session for a key died (one line: "timed out after
    // 60s at exec #3", "was reset by the user", ...).  Consumed by the
    // next exec on that key and surfaced as the restart notice, so the
    // model learns WHY its variables are gone.  Guarded by `mutex`.
    std::map<std::string, std::string> deathNotes;

    // Key of the session an exec worker currently holds; ReapIdle must
    // never destroy that one.  Empty when idle.  Guarded by `mutex`.
    std::string inFlightKey;

    // ── Capture watchdog ─────────────────────────────────────────
    // ReadFrame caps the capture files only while a request is being
    // read.  Background threads and subprocesses started by user code
    // keep writing after the result is returned, and an idle session can
    // live for kIdleReapAfterMs (30 min) -- long enough to fill the temp
    // drive.  This thread caps every live session's files on a short
    // cadence for the whole life of the manager, idle or not.
    // Started/stopped by PythonSessionManager's ctor/dtor, which own the
    // thread's lifetime (it holds a raw Impl*, never a shared_ptr, so it
    // cannot keep Impl alive).
    std::thread             capThread;
    std::mutex              capMutex;
    std::condition_variable capCv;
    bool                    capStop = false;

    void CapLoop()
    {
        constexpr auto kCapInterval = std::chrono::milliseconds(100);
        std::unique_lock<std::mutex> lk(capMutex);
        while (!capStop) {
            capCv.wait_for(lk, kCapInterval, [this] { return capStop; });
            if (capStop) break;
            lk.unlock();

            std::vector<std::wstring> dirs;
            {
                std::lock_guard<std::mutex> mlk(mutex);
                dirs.reserve(sessions.size());
                for (const auto& kv : sessions)
                    if (kv.second && !kv.second->tempDir.empty())
                        dirs.push_back(kv.second->tempDir);
            }
            // Paths only, no session refs: a session destroyed meanwhile
            // just makes these calls no-ops (the file is gone).
            for (const std::wstring& d : dirs) {
                CapCaptureFile(d + L"\\out.log");
                CapCaptureFile(d + L"\\err.log");
            }
            lk.lock();
        }
    }

    void RecordDeath(const std::string& key, const std::string& note)
    {
        std::lock_guard<std::mutex> lk(mutex);
        deathNotes[key] = note;
    }

    // Returns and erases the pending note for `key`, or empty.
    std::string TakeDeathNote(const std::string& key)
    {
        std::lock_guard<std::mutex> lk(mutex);
        auto it = deathNotes.find(key);
        if (it == deathNotes.end()) return std::string();
        std::string note = std::move(it->second);
        deathNotes.erase(it);
        return note;
    }

    void PostResult(PySessionResult r)
    {
        auto* ev = new wxCommandEvent(wxEVT_PY_SESSION_COMPLETE);
        ev->SetClientObject(new PySessionResultClientData(std::move(r)));
        LbQueueEventIfAlive(eventHandler, aliveToken, ev);
    }

    // Removes `s` from the map iff it is still the registered session
    // for its key (Shutdown or a conversation close may have removed it first).
    void ForgetSession(const std::shared_ptr<PySession>& s)
    {
        std::lock_guard<std::mutex> lk(mutex);
        auto it = sessions.find(s->key);
        if (it != sessions.end() && it->second == s) sessions.erase(it);
    }
};

PythonSessionManager::PythonSessionManager(
        wxEvtHandler* eventHandler,
        std::weak_ptr<std::atomic<bool>> aliveToken)
    : m_impl(std::make_shared<Impl>())
{
    m_impl->eventHandler = eventHandler;
    m_impl->aliveToken   = std::move(aliveToken);
    Impl* impl = m_impl.get();
    m_impl->capThread = std::thread([impl]() { impl->CapLoop(); });
}

PythonSessionManager::~PythonSessionManager()
{
    Shutdown();
    {
        std::lock_guard<std::mutex> lk(m_impl->capMutex);
        m_impl->capStop = true;
    }
    m_impl->capCv.notify_all();
    if (m_impl->capThread.joinable()) m_impl->capThread.join();
}

void PythonSessionManager::Cancel()
{
    m_impl->cancelFlag->store(true);
}

bool PythonSessionManager::IsRunning() const
{
    return m_impl->isRunning->load();
}

void PythonSessionManager::Shutdown()
{
    m_impl->cancelFlag->store(true);
    std::map<std::string, std::shared_ptr<PySession>> doomed;
    {
        std::lock_guard<std::mutex> lk(m_impl->mutex);
        m_impl->shutdown = true;
        doomed.swap(m_impl->sessions);
    }
    // Destructors run outside the lock: each kills its Job Object
    // (whole process tree), joins its drain thread, and cleans its
    // temp dir.  A session an in-flight worker still holds survives
    // via its shared_ptr until that worker notices the cancel flag.
    doomed.clear();
}

bool PythonSessionManager::CloseSessionFor(const std::string& cwd)
{
    const std::string key = NormalizeKey(cwd);
    std::shared_ptr<PySession> doomed;
    {
        std::lock_guard<std::mutex> lk(m_impl->mutex);
        auto it = m_impl->sessions.find(key);
        if (it == m_impl->sessions.end()) return false;
        doomed = it->second;
        m_impl->sessions.erase(it);
        m_impl->deathNotes[key] = "was reset";
    }
    doomed.reset();  // kill + drain-join + temp cleanup, outside the lock
    return true;
}

size_t PythonSessionManager::ReapIdle(unsigned long maxIdleMs)
{
    const double cutoff = NowSec() - maxIdleMs / 1000.0;
    std::vector<std::shared_ptr<PySession>> doomed;
    {
        std::lock_guard<std::mutex> lk(m_impl->mutex);
        for (auto it = m_impl->sessions.begin();
             it != m_impl->sessions.end();) {
            const bool inFlight = (it->first == m_impl->inFlightKey);
            if (!inFlight && it->second->lastUsedSec.load() < cutoff) {
                m_impl->deathNotes[it->first] =
                    "was closed after " +
                    std::to_string(maxIdleMs / 60000) +
                    " minutes idle";
                doomed.push_back(it->second);
                it = m_impl->sessions.erase(it);
            } else {
                ++it;
            }
        }
    }
    const size_t n = doomed.size();
    doomed.clear();  // destructors (kill + cleanup) outside the lock
    return n;
}

// ═══════════════════════════════════════════════════════════════════
//  Shared card presentation
// ═══════════════════════════════════════════════════════════════════

void BuildPySessionCardParts(const PySessionResult&    r,
                             std::string&              bodyOut,
                             std::string&              errorBodyOut,
                             std::vector<std::string>& chipsOut)
{
    bodyOut = r.stdoutText;

    // stderr and the exception report render together in the error
    // lane; the traceback already carries type + message.
    errorBodyOut = r.stderrText;
    if (!r.traceback.empty()) {
        if (!errorBodyOut.empty() && errorBodyOut.back() != '\n')
            errorBodyOut += "\n";
        errorBodyOut += r.traceback;
    }

    chipsOut.clear();
    if (r.cancelled)      chipsOut.push_back("cancelled");
    else if (r.timedOut)  chipsOut.push_back("timed out");
    else if (r.ok)        chipsOut.push_back("ok");
    else                  chipsOut.push_back(r.excType.empty()
                                                 ? std::string("error")
                                                 : r.excType);
    if (r.execCount > 0)
        chipsOut.push_back("#" + std::to_string(r.execCount));
    {
        std::ostringstream ts;
        ts << std::fixed;
        ts.precision(r.elapsedSec < 10.0 ? 2 : 1);
        ts << r.elapsedSec << "s";
        chipsOut.push_back(ts.str());
    }
    if (r.sessionDiedEarlier ||
        !r.restartNote.empty())   chipsOut.push_back("restarted");
    else if (r.freshSession)      chipsOut.push_back("new session");
    if (r.stateLost)              chipsOut.push_back("state lost");
    if (!r.pythonCommand.empty()) chipsOut.push_back(r.pythonCommand);
    if (r.truncated)              chipsOut.push_back("truncated");
}

bool PythonSessionManager::StartExec(const std::string& code,
                                     const std::string& cwd,
                                     unsigned long      timeoutMs)
{
    if (m_impl->isRunning->load()) return false;
    if (code.size() > kMaxCodeBytes) return false;
    {
        std::lock_guard<std::mutex> lk(m_impl->mutex);
        if (m_impl->shutdown) return false;
    }

    m_impl->isRunning->store(true);
    m_impl->cancelFlag->store(false);

    std::shared_ptr<Impl> impl = m_impl;
    const unsigned long effTimeout =
        (timeoutMs == 0) ? kDefaultExecTimeoutMs : timeoutMs;

    std::thread([impl, code, cwd, effTimeout]() {
        auto running = impl->isRunning;
        auto cancel  = impl->cancelFlag;

        PySessionResult result;
        result.code = code;
        {
            const size_t nl = code.find('\n');
            std::string firstLine =
                (nl == std::string::npos) ? code : code.substr(0, nl);
            firstLine = LbUtf8SafeTruncate(firstLine, 120);
            result.commandEcho = "/py " + firstLine +
                                 (nl != std::string::npos ? " ..." : "");
        }

        const double t0 = NowSec();
        const std::string key = NormalizeKey(cwd);

        std::shared_ptr<PySession> s;
        auto finish = [&](PySessionResult&& r) {
            // Restart notice: this call spawned a fresh session because
            // an earlier one was killed/reset/reaped — lead with WHY,
            // so the model knows its variables are gone by cause, not
            // by NameError surprise.
            if (!r.restartNote.empty()) {
                r.stderrText =
                    "[fresh session — the previous Python session " +
                    r.restartNote +
                    "; variables from earlier py calls are lost]\n" +
                    r.stderrText;
            }
            if (s && !r.stateLost) s->lastUsedSec.store(NowSec());
            {
                std::lock_guard<std::mutex> lk(impl->mutex);
                if (impl->inFlightKey == key) impl->inFlightKey.clear();
            }
            r.elapsedSec = NowSec() - t0;
            impl->PostResult(std::move(r));
            running->store(false);
        };

        // ── Find or create the session ──────────────────────────
        {
            std::lock_guard<std::mutex> lk(impl->mutex);
            auto it = impl->sessions.find(key);
            if (it != impl->sessions.end()) {
                DWORD exitCode = 0;
                if (it->second->ProcExited(&exitCode)) {
                    // Died between calls — replace it, run the code
                    // anyway, and say why the variables are gone.
                    result.sessionDiedEarlier = true;
                    result.priorExitCode = static_cast<int>(exitCode);
                    impl->sessions.erase(it);
                } else {
                    s = it->second;
                    impl->inFlightKey = key;  // ReapIdle must skip us
                }
            }
        }

        if (!s) {
            std::string spawnError;
            s = SpawnSession(cwd, key, cancel, spawnError);
            if (!s) {
                result.stderrText = spawnError;
                result.cancelled  = cancel->load();
                finish(std::move(result));
                return;
            }
            result.freshSession  = true;
            result.pythonCommand = s->pythonCommand;
            result.pythonVersion = s->pythonVersion;
            // A pending note means an earlier call killed / reset /
            // idle-reaped this key's session; consume it so finish()
            // can lead the result with the restart notice.  Taken only
            // AFTER a successful spawn — a spawn failure preserves the
            // note for the next attempt.
            result.restartNote = impl->TakeDeathNote(key);

            bool shutdownAfterSpawn = false;
            {
                std::lock_guard<std::mutex> lk(impl->mutex);
                if (impl->shutdown) {
                    // Shutdown raced our spawn.  Do not register the
                    // freshly-created session.  Finish only AFTER releasing
                    // impl->mutex: finish() takes that mutex itself while it
                    // clears inFlightKey, so calling it under this lock would
                    // self-deadlock during frame/app shutdown.
                    shutdownAfterSpawn = true;
                } else {
                    impl->sessions[key] = s;
                    impl->inFlightKey = key;  // ReapIdle must skip us
                }
            }

            if (shutdownAfterSpawn) {
                result.cancelled  = true;
                result.stderrText = "[cancelled — LlamaBoss is closing]";
                finish(std::move(result));
                return;  // s destructs: kill + cleanup
            }
        } else {
            result.pythonCommand = s->pythonCommand;
            result.pythonVersion = s->pythonVersion;
        }

        // ── Exchange ────────────────────────────────────────────
        const unsigned requestId = s->nextRequestId++;
        result.execCount = s->execCount.fetch_add(1) + 1;

        Poco::JSON::Object req;
        req.set("id", requestId);
        req.set("op", "exec");
        req.set("code", code);
        std::ostringstream reqJson;
        req.stringify(reqJson);

        // Kill the session, drop it from the map, and salvage partial
        // output from the capture files.  `banner` is the explicit
        // state-lost message every kill path must carry.
        auto killAndSalvage = [&](const std::string& banner,
                                  const std::string& deathNote) {
            s->Kill();
            impl->ForgetSession(s);
            if (!deathNote.empty()) {
                impl->RecordDeath(
                    key, deathNote + " at exec #" +
                             std::to_string(result.execCount));
            }
            result.stateLost = true;

            bool soTrunc = false, seTrunc = false;
            const std::string so = ReadFileCapped(
                s->tempDir + L"\\out.log",
                PythonSessionManager::kMaxSalvageBytes, soTrunc);
            const std::string se = ReadFileCapped(
                s->tempDir + L"\\err.log",
                PythonSessionManager::kMaxSalvageBytes, seTrunc);
            result.stdoutText = so;
            result.stderrText = se;
            result.truncated  = soTrunc || seTrunc;
            if (!so.empty() || !se.empty()) {
                result.stderrText +=
                    "\n[partial output above was salvaged from the capture "
                    "files after the kill — it may be incomplete]";
            }
            if (!banner.empty()) {
                result.stderrText += "\n";
                result.stderrText += banner;
            }
        };

        std::string ioError;
        if (!WriteFrame(*s, reqJson.str(), ioError)) {
            DWORD exitCode = 0;
            const bool exited = s->ProcExited(&exitCode);
            const std::string drained = s->DrainedStderr();
            killAndSalvage(std::string(),
                           exited ? "crashed (exit " +
                                        std::to_string(exitCode) + ")"
                                  : "died mid-request");
            std::ostringstream msg;
            msg << "\n[the Python session died"
                << (exited ? " (exit " + std::to_string(exitCode) + ")" : "")
                << " — all session variables are lost. The next py call "
                   "starts a fresh session.]";
            if (!drained.empty()) msg << "\n[kernel stderr] " << drained;
            if (!ioError.empty()) msg << "\n" << ioError;
            result.stderrText += msg.str();
            finish(std::move(result));
            return;
        }

        std::string payload;
        const double deadline = NowSec() + effTimeout / 1000.0;
        const FrameReadStatus st =
            ReadFrame(*s, cancel, deadline, payload, ioError);

        switch (st) {
        case FrameReadStatus::Ok:
            break;

        case FrameReadStatus::TimedOut:
            result.timedOut = true;
            killAndSalvage(
                "[timed out after " + std::to_string(effTimeout / 1000) +
                "s — the Python session was killed; all session variables "
                "are lost. The next py call starts a fresh session.]",
                "timed out after " + std::to_string(effTimeout / 1000) +
                    "s and was killed");
            finish(std::move(result));
            return;

        case FrameReadStatus::Cancelled:
            result.cancelled = true;
            killAndSalvage(
                "[cancelled — the Python session was killed; all session "
                "variables are lost. The next py call starts a fresh "
                "session.]",
                "was cancelled and killed");
            finish(std::move(result));
            return;

        case FrameReadStatus::Broken: {
            DWORD exitCode = 0;
            s->ProcExited(&exitCode);
            const std::string drained = s->DrainedStderr();
            killAndSalvage(
                "[the Python session died (exit " +
                std::to_string(exitCode) +
                ") — all session variables are lost. The next py call "
                "starts a fresh session.]",
                "crashed (exit " + std::to_string(exitCode) + ")");
            if (!drained.empty())
                result.stderrText += "\n[kernel stderr] " + drained;
            if (!ioError.empty()) result.stderrText += "\n" + ioError;
            finish(std::move(result));
            return;
        }

        case FrameReadStatus::Desync: {
            const std::string drained = s->DrainedStderr();
            killAndSalvage(
                "[the session's frame protocol desynced — the session was "
                "killed rather than resynced; all session variables are "
                "lost. The next py call starts a fresh session.]",
                "hit the protocol desync guard and was killed");
            if (!drained.empty())
                result.stderrText += "\n[kernel stderr] " + drained;
            if (!ioError.empty()) result.stderrText += "\n" + ioError;
            finish(std::move(result));
            return;
        }
        }

        // ── Parse the response ──────────────────────────────────
        try {
            Poco::JSON::Parser parser;
            auto obj =
                parser.parse(payload).extract<Poco::JSON::Object::Ptr>();
            if (!obj)
                throw std::runtime_error("response is not a JSON object");
            const Poco::UInt64 respId =
                obj->optValue<Poco::UInt64>("id", 0);
            if (respId != requestId) {
                throw std::runtime_error(
                    "response id " + std::to_string(respId) +
                    " does not match request id " +
                    std::to_string(requestId));
            }
            result.ok         = obj->optValue<bool>("ok", false);
            result.stdoutText = obj->optValue<std::string>("stdout", "");
            result.stderrText = obj->optValue<std::string>("stderr", "");
            result.excType    = obj->optValue<std::string>("exc_type", "");
            result.excMessage = obj->optValue<std::string>("exc_message", "");
            result.traceback  = obj->optValue<std::string>("traceback", "");
            result.truncated  =
                obj->optValue<bool>("stdout_truncated", false) ||
                obj->optValue<bool>("stderr_truncated", false);
        } catch (const std::exception& e) {
            const std::string drained = s->DrainedStderr();
            killAndSalvage(
                "[the session sent an unreadable response — the session was "
                "killed; all session variables are lost. The next py call "
                "starts a fresh session.]",
                "sent an unreadable response and was killed");
            result.stderrText += std::string("\nparse error: ") + e.what();
            if (!drained.empty())
                result.stderrText += "\n[kernel stderr] " + drained;
            finish(std::move(result));
            return;
        }

        if (result.sessionDiedEarlier) {
            result.stderrText =
                "[the previous Python session had exited (code " +
                std::to_string(result.priorExitCode) +
                ") — a fresh session was started; variables from earlier "
                "py calls are gone.]\n" + result.stderrText;
        }

        finish(std::move(result));
    }).detach();

    return true;
}
