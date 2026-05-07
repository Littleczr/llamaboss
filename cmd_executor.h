// cmd_executor.h
//
// Phase 1 of the LlamaBoss Pro tool-executor harness.
//
// Runs a single user-issued PowerShell command on a worker thread,
// captures stdout + stderr, enforces a timeout and an output cap,
// and reports completion back to the UI via a wxCommandEvent.
//
// Lifetime model mirrors ChatClient:
//   - CmdExecutor is owned by MyFrame.
//   - Worker threads are fire-and-forget (wxThread joinable=false).
//   - A weak_ptr<atomic<bool>> alive token guards against posting
//     events to a MyFrame that has been destroyed.
//   - A shared_ptr<atomic<bool>> cancel flag lets the UI kill the
//     running process tree via the Job Object handle held by the
//     worker.
//
#pragma once

#include <wx/wx.h>
#include <wx/thread.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "presented_file.h"

// ─── Custom events for thread -> UI communication ────────────────
wxDECLARE_EVENT(wxEVT_CMD_COMPLETE, wxCommandEvent);
wxDECLARE_EVENT(wxEVT_CMD_ERROR,    wxCommandEvent);

// ─── Result payload ──────────────────────────────────────────────
struct CmdResult {
    std::string command;      // original user-typed command (no /cmd prefix)
    std::string stdoutText;   // captured stdout, post-cap (UTF-8)
    std::string stderrText;   // captured stderr, post-cap (UTF-8)
    int         exitCode   = -1;
    double      elapsedSec = 0.0;
    bool        timedOut   = false;
    bool        truncated  = false;   // true if either stream hit the byte cap
    bool        cancelled  = false;   // true if the user cancelled

    // Large-output UX: if stdout/stderr would flood the chat, the
    // executor saves captured output to the conversation ToolOutputs
    // lane and returns a compact preview in stdoutText/stderrText.
    std::vector<PresentedFile> presentedFiles;
};

// wxClientData wrapper so we can pack a full CmdResult into a
// wxCommandEvent without manual heap lifecycle management.
// wxCommandEvent's destructor will delete the client data for us.
class CmdResultClientData : public wxClientData {
public:
    explicit CmdResultClientData(CmdResult r) : m_result(std::move(r)) {}
    const CmdResult& GetResult() const { return m_result; }
private:
    CmdResult m_result;
};

// ─── Executor facade ─────────────────────────────────────────────
class CmdExecutor {
public:
    // kDefaultTimeoutMs is the hard ceiling for a single /cmd invocation.
    // kMaxOutputBytes caps stdout AND stderr independently; anything past
    // the cap is discarded and `truncated` is set on the result.
    static constexpr unsigned long kDefaultTimeoutMs = 60000;     // 60 s
    static constexpr size_t        kMaxOutputBytes   = 4 * 1024 * 1024;  // 4 MiB per stream; display is previewed if large

    CmdExecutor(wxEvtHandler* eventHandler,
                std::weak_ptr<std::atomic<bool>> aliveToken);
    ~CmdExecutor();

    // Starts `command` on a detached worker thread.
    // Returns false if a command is already running, if `command` is
    // empty/whitespace, or if the worker could not be spawned.
    //
    // Convenience overload — uses %USERPROFILE% as CWD and the default
    // 60s timeout.  Equivalent to Start(command, "", kDefaultTimeoutMs).
    bool Start(const std::string& command);

    // Full overload.  `cwd` is a UTF-8 absolute path; pass empty to fall
    // back to %USERPROFILE% (legacy behaviour).  `timeoutMs` of 0 falls
    // back to kDefaultTimeoutMs.
    //
    // Used by the agent path to honour the per-conversation tool CWD set
    // by /cd, which the model expects PowerShell calls to respect.
    bool Start(const std::string& command,
               const std::string& cwd,
               unsigned long      timeoutMs);

    // Requests cancellation of any running command. Safe to call even
    // if nothing is running. The process tree is killed via its Job
    // Object inside the worker as soon as it observes the cancel flag.
    void Cancel();

    // True while a worker is alive and before its Entry() has returned.
    bool IsRunning() const {
        return m_isRunning && m_isRunning->load();
    }

private:
    wxEvtHandler*                        m_eventHandler;
    std::weak_ptr<std::atomic<bool>>     m_aliveToken;
    std::shared_ptr<std::atomic<bool>>   m_cancelFlag;
    std::shared_ptr<std::atomic<bool>>   m_isRunning;
};
