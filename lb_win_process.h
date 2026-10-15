#pragma once

// ─── lb_win_process.h ──────────────────────────────────────────────
// Small Win32 process primitives shared by the three child-process
// runners: cmd_executor.cpp (PowerShell), python_runner.cpp (helpers and
// scripts) and python_session.cpp (the persistent py kernel).  Each file
// used to carry its own identical copy of these.
//
// Only the leaf helpers live here.  Process launch, job-object limits,
// pipe draining and descendant cleanup stay in each runner on purpose:
// their semantics differ (py session memory cap and framed IPC, PowerShell
// descendant wait, helper output caps) and were debugged separately.
//
// Header-only so no project file needs a new .cpp entry.

#ifdef _WIN32

#include "lb_windows.h"

#include <cstddef>
#include <string>

namespace lb_win {

// RAII for Win32 HANDLEs.  Treats both nullptr and INVALID_HANDLE_VALUE
// as "no handle".
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

// Unblock a drain thread parked in a synchronous ReadFile when the
// job-object kill alone does not deliver EOF (something outside the job
// inherited a write handle).  Resolved dynamically, as the original
// copies did.
inline void CancelThreadSynchronousIo(HANDLE threadHandle)
{
    if (!threadHandle) return;
    using Fn = BOOL (WINAPI *)(HANDLE);
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    if (!kernel) return;
    auto* fn = reinterpret_cast<Fn>(GetProcAddress(kernel, "CancelSynchronousIo"));
    if (fn) fn(threadHandle);
}

// Full Windows CRT argv quoting: quotes are escaped and trailing
// backslashes doubled, so an argument ending in \ cannot escape the
// closing quote and merge argv items.
inline std::wstring QuoteArg(const std::wstring& arg)
{
    std::wstring out = L"\"";
    std::size_t backslashes = 0;

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

} // namespace lb_win

#endif // _WIN32
