// process_executor_win.cpp — Windows CreateProcessW + Job Object
#include "process_executor.h"
#include "str_util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <memory>
#include <chrono>

namespace process_executor {

class WinHandle : public Handle {
public:
    WinHandle(HANDLE job, HANDLE proc, std::thread&& stdoutThread, std::thread&& stderrThread,
              std::shared_ptr<std::atomic<bool>> cancelled,
              CompletionCallback onComplete, Result&& result)
        : m_job(job), m_proc(proc),
          m_stdoutThread(std::move(stdoutThread)), m_stderrThread(std::move(stderrThread)),
          m_cancelled(cancelled), m_onComplete(std::move(onComplete)), m_result(std::move(result)) {}

    ~WinHandle() {
        if (m_job) CloseHandle(m_job);
        if (m_proc) CloseHandle(m_proc);
        if (m_stdoutThread.joinable()) m_stdoutThread.join();
        if (m_stderrThread.joinable()) m_stderrThread.join();
    }

    bool Cancel() override {
        m_cancelled->store(true);
        // Closing job handle triggers KILL_ON_JOB_CLOSE
        if (m_job) { CloseHandle(m_job); m_job = nullptr; }
        return true;
    }

    bool IsRunning() const override {
        if (!m_proc) return false;
        DWORD code = 0;
        if (!GetExitCodeProcess(m_proc, &code)) return false;
        return code == STILL_ACTIVE;
    }

private:
    HANDLE m_job = nullptr;
    HANDLE m_proc = nullptr;
    std::thread m_stdoutThread, m_stderrThread;
    std::shared_ptr<std::atomic<bool>> m_cancelled;
    CompletionCallback m_onComplete;
    Result m_result;
};

static std::wstring BuildCmdLine(std::string_view command, const std::vector<std::string>& args) {
    std::string line = std::string(command);
    for (const auto& a : args) {
        line += " ";
        // Simple quoting for args with spaces
        if (a.find(' ') != std::string::npos) {
            line += "\"";
            line += a;
            line += "\"";
        } else {
            line += a;
        }
    }
    return str_util::Utf8ToWide(line);
}

std::unique_ptr<Handle> Start(std::string_view command,
                              const std::vector<std::string>& args,
                              std::string_view cwd,
                              unsigned long timeoutMs,
                              OutputCallback onOutput,
                              CompletionCallback onComplete) {

    std::wstring wCmdLine = BuildCmdLine(command, args);
    if (wCmdLine.empty()) return nullptr;

    std::wstring wCwd;
    if (!cwd.empty()) wCwd = str_util::Utf8ToWide(cwd);

    // Create pipes for stdout/stderr
    SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    HANDLE outR = nullptr, outW = nullptr, errR = nullptr, errW = nullptr;
    if (!CreatePipe(&outR, &outW, &sa, 0) || !CreatePipe(&errR, &errW, &sa, 0)) {
        if (outR) CloseHandle(outR);
        if (outW) CloseHandle(outW);
        if (errR) CloseHandle(errR);
        if (errW) CloseHandle(errW);
        return nullptr;
    }
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);

    // Create Job Object
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) {
        CloseHandle(outR); CloseHandle(outW); CloseHandle(errR); CloseHandle(errW);
        return nullptr;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));

    // Spawn process
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nullptr;
    si.hStdOutput = outW;
    si.hStdError = errW;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdBuf(wCmdLine.begin(), wCmdLine.end());
    cmdBuf.push_back(L'\0');

    BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_SUSPENDED,
                             nullptr, wCwd.empty() ? nullptr : wCwd.c_str(), &si, &pi);
    if (!ok) {
        CloseHandle(job); CloseHandle(outR); CloseHandle(outW); CloseHandle(errR); CloseHandle(errW);
        return nullptr;
    }
    CloseHandle(outW); CloseHandle(errW); // Parent doesn't need write ends

    AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    Result result;

    auto reader = [](HANDLE h, std::string& out, std::atomic<bool>& cancelled,
                     OutputCallback cb, bool isStdout) {
        constexpr DWORD kChunk = 4096;
        char buf[kChunk];
        while (true) {
            DWORD got = 0;
            if (!ReadFile(h, buf, kChunk, &got, nullptr) || got == 0) break;
            if (cancelled.load()) break;
            out.append(buf, got);
            if (cb) cb(isStdout ? std::string_view(buf, got) : std::string_view(),
                       isStdout ? std::string_view() : std::string_view(buf, got));
        }
    };

    std::mutex outMu, errMu;
    std::thread outThread(reader, outR, std::ref(result.stdoutText), std::ref(*cancelled),
                          onOutput, true);
    std::thread errThread(reader, errR, std::ref(result.stderrText), std::ref(*cancelled),
                          onOutput, false);

    auto waitThread = std::thread([=, cancelled = std::move(cancelled),
                                    onComplete = std::move(onComplete),
                                    result = std::move(result),
                                    job, proc = pi.hProcess,
                                    outThread = std::move(outThread),
                                    errThread = std::move(errThread)]() mutable {
        auto t0 = std::chrono::steady_clock::now();
        constexpr DWORD kTickMs = 200;
        double deadline = timeoutMs ? (std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count() + timeoutMs / 1000.0) : 0;

        bool killIt = false;
        for (;;) {
            DWORD wr = WaitForSingleObject(proc, kTickMs);
            if (wr == WAIT_OBJECT_0) break;
            if (wr == WAIT_FAILED) { killIt = true; break; }
            if (cancelled->load()) { result.cancelled = true; killIt = true; break; }
            if (deadline && std::chrono::duration<double>(
                    std::chrono::steady_clock::now().time_since_epoch()).count() >= deadline) {
                result.timedOut = true; killIt = true; break;
            }
        }

        if (killIt) {
            CloseHandle(job); // KILL_ON_JOB_CLOSE
            WaitForSingleObject(proc, 2000);
        }

        if (outThread.joinable()) outThread.join();
        if (errThread.joinable()) errThread.join();

        DWORD code = 0;
        GetExitCodeProcess(proc, &code);
        result.exitCode = static_cast<int>(code);
        result.elapsedSec = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count();

        if (onComplete) onComplete(result);
    });

    waitThread.detach(); // Handle owns the threads via destructor

    return std::make_unique<WinHandle>(job, proc, std::move(outThread), std::move(errThread),
                                       cancelled, std::move(onComplete), std::move(result));
}

} // namespace process_executor