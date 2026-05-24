// tool_python_syntax.cpp
//
// Single shared implementation of the Python syntax pre-check.
// See tool_python_syntax.h for the contract and rationale.

#include "tool_python_syntax.h"
#include "path_safety.h"   // Utf8ToWide

#include <string>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

// Quote a single argument for the Windows CRT command-line parser.
// Always quote, and use the full backslash-doubling rule so paths with
// trailing backslashes or embedded quotes cannot reshape argv.
std::wstring QuoteWinArg(const std::wstring& arg)
{
    std::wstring out = L"\"";
    std::size_t backslashes = 0;

    for (wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }

        if (ch == L'\"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'\"');
            backslashes = 0;
            continue;
        }

        if (backslashes > 0) {
            out.append(backslashes, L'\\');
            backslashes = 0;
        }
        out.push_back(ch);
    }

    // Backslashes immediately before the closing quote must be doubled.
    out.append(backslashes * 2, L'\\');
    out.push_back(L'\"');
    return out;
}

// Drain a pipe to EOF.  Accumulates output up to a 64 KiB cap but KEEPS
// READING (and discarding) past the cap, so a child that emits more than
// the pipe buffer can hold never blocks on a full pipe -- it can always
// finish writing and exit, which is what lets the caller's
// WaitForSingleObject return promptly instead of deadlocking.
std::string ReadAllFromPipe(HANDLE h)
{
    constexpr size_t kCap = 64 * 1024;
    std::string out;
    bool truncatedNoted = false;
    char buf[4096];
    for (;;) {
        DWORD got = 0;
        BOOL ok = ::ReadFile(h, buf, sizeof(buf), &got, nullptr);
        if (!ok || got == 0) break;
        if (out.size() < kCap) {
            out.append(buf, buf + got);
            if (out.size() >= kCap && !truncatedNoted) {
                out += "\n[... syntax-check output truncated ...]\n";
                truncatedNoted = true;
            }
        }
        // Past the cap: keep draining, drop the bytes.
    }
    return out;
}

// True if py_compile's output names a genuine source-level error.
// py_compile prints the error class name on the final line, so a
// substring match is sufficient and avoids treating "interpreter
// failed to run" (which also exits non-zero) as a syntax failure.
bool LooksLikeSyntaxError(const std::string& output)
{
    return output.find("SyntaxError")      != std::string::npos ||
           output.find("IndentationError") != std::string::npos ||
           output.find("TabError")         != std::string::npos;
}

} // anonymous namespace

namespace tool_python_syntax {

SyntaxCheckResult CheckFile(const std::string& filePath)
{
    SyntaxCheckResult result;

    std::wstring wPath = path_safety::Utf8ToWide(filePath);
    if (wPath.empty()) {
        result.ok = false;
        result.checked = true;
        result.message = "Path conversion failed during Python syntax check.";
        return result;
    }

    std::vector<std::wstring> commands = {
        L"py.exe -3 -B -m py_compile " + QuoteWinArg(wPath),
        L"python.exe -B -m py_compile " + QuoteWinArg(wPath),
        L"python3.exe -B -m py_compile " + QuoteWinArg(wPath)
    };

    std::string startErrors;
    for (const std::wstring& cmd : commands) {
        SECURITY_ATTRIBUTES sa = {};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE readPipe = nullptr;
        HANDLE writePipe = nullptr;
        if (!::CreatePipe(&readPipe, &writePipe, &sa, 0)) {
            startErrors += "CreatePipe failed for syntax check.\n";
            continue;
        }
        ::SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = writePipe;
        si.hStdError = writePipe;
        si.hStdInput = nullptr;

        PROCESS_INFORMATION pi = {};
        std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
        mutableCmd.push_back(L'\0');
        BOOL ok = ::CreateProcessW(
            nullptr,
            mutableCmd.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &si,
            &pi);

        // The parent must drop its copy of the write end; otherwise the
        // reader below never sees EOF.  Done before any branch so it
        // can't be skipped on a launch failure.
        ::CloseHandle(writePipe);

        if (!ok) {
            DWORD err = ::GetLastError();
            ::CloseHandle(readPipe);
            startErrors += "Could not start syntax check (Win32 error " + std::to_string(err) + ").\n";
            continue;
        }

        // Drain the pipe concurrently with the wait.  If we waited for
        // the process first and only then read, a child whose output
        // overflows the pipe buffer would block on write while we block
        // on the wait -- a classic anonymous-pipe deadlock that resolves
        // only when the 10s timeout fires.  The reader thread keeps the
        // pipe drained so the child can always make progress and exit.
        std::string output;
        std::thread reader([readPipe, &output]() {
            output = ReadAllFromPipe(readPipe);
        });

        DWORD wait = ::WaitForSingleObject(pi.hProcess, 10000);
        const bool timedOut = (wait == WAIT_TIMEOUT);
        const bool waitFailed = (wait == WAIT_FAILED);
        DWORD waitError = waitFailed ? ::GetLastError() : 0;
        if (timedOut || waitFailed) {
            ::TerminateProcess(pi.hProcess, 1);
            ::WaitForSingleObject(pi.hProcess, 2000);
        }

        DWORD exitCode = 1;
        ::GetExitCodeProcess(pi.hProcess, &exitCode);

        // The child has exited (or been terminated), so the last write
        // handle to the pipe is closed and the reader will hit EOF.
        // Join before touching readPipe so we never close a handle the
        // reader is still using.
        reader.join();

        ::CloseHandle(readPipe);
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);

        result.checked = true;

        // Clean compile -- file is good.
        if (!timedOut && exitCode == 0) {
            result.ok = true;
            result.message.clear();
            return result;
        }

        // Genuine source-level error -- block.
        if (!timedOut && LooksLikeSyntaxError(output)) {
            result.ok = false;
            result.message = output;
            return result;
        }

        // Everything else -- timed out, or a non-zero exit that does NOT
        // name a syntax error (e.g. a py launcher with no usable 3.x
        // runtime, or py_compile failing to start).  We couldn't get a
        // real verdict, so we do NOT block; python_health /
        // python_run_script will surface a genuine runtime problem
        // later.  `message` is kept only for diagnostics and is not
        // surfaced because result.ok stays true.
        result.ok = true;
        result.checked = false;
        if (timedOut) {
            result.message = "Python syntax check timed out; not treated as a syntax failure.";
        } else if (waitFailed) {
            result.message = "Python syntax check wait failed (Win32 error " +
                             std::to_string(waitError) +
                             "); not treated as a syntax failure.";
        } else {
            result.message = output.empty()
                ? "Python syntax check could not run (exit " + std::to_string(exitCode) + ")."
                : output;
        }
        return result;
    }

    // No launcher could even start. Don't block; the later
    // python_health / python_run_script step reports the missing runtime.
    result.ok = true;
    result.checked = false;
    result.message = startErrors;
    return result;
}

} // namespace tool_python_syntax
