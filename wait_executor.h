// wait_executor.h
//
// Backend for the agent `wait` tool: a timed pause in the agent loop
// that lets the model monitor long-running external work (downloads,
// builds, remote jobs) with wait -> single-poll cycles instead of
// embedding sleep loops inside powershell payloads that then hit the
// command timeout.
//
// Unlike the other executors there is NO worker thread and NO child
// process: Start() arms a one-shot wxTimer on the main thread, the
// timer fires on the main thread, and completion is posted as
// wxEVT_WAIT_COMPLETE through the same LbQueueEventIfAlive guard
// every other executor uses -- so the completion always arrives as a
// queued event (never re-entrantly inside Start's caller) and never
// lands on a destroyed frame.
//
// Lifetime model mirrors CmdExecutor:
//   - WaitExecutor is owned by MyFrame.
//   - The alive token is the frame's m_alive.
//   - Cancel() stops the timer and posts the completion event with
//     cancelled=true, so the Stop button drives the exact same
//     teardown path as a natural expiry.
//
// Main-thread only: Start/Cancel are called from wx event handlers
// and the timer callback runs on the main thread.  No locking.
//
#pragma once

#include <wx/wx.h>
#include <wx/timer.h>
#include <wx/stopwatch.h>

#include <atomic>
#include <memory>
#include <string>

// ─── Custom event for completion ─────────────────────────────────
wxDECLARE_EVENT(wxEVT_WAIT_COMPLETE, wxCommandEvent);

// ─── Result payload ──────────────────────────────────────────────
struct WaitResult {
    int         requestedSeconds = 0;
    double      elapsedSec       = 0.0;
    std::string reason;              // optional model-supplied label
    bool        cancelled        = false;
};

// wxClientData wrapper -- same ownership contract as
// CmdResultClientData (see cmd_executor.h): the receiving handler
// must claim the client object before any early return.
class WaitResultClientData : public wxClientData {
public:
    explicit WaitResultClientData(WaitResult r) : m_result(std::move(r)) {}
    const WaitResult& GetResult() const { return m_result; }
private:
    WaitResult m_result;
};

// ─── Executor ────────────────────────────────────────────────────
class WaitExecutor : public wxEvtHandler {
public:
    WaitExecutor(wxEvtHandler* eventHandler,
                 std::weak_ptr<std::atomic<bool>> aliveToken);
    ~WaitExecutor() override;

    // Arm a one-shot wait.  Returns false if a wait is already
    // running or the timer could not start.  `seconds` must already
    // be validated/clamped by the caller (AgentController enforces
    // the per-call bounds and the per-turn budget).
    bool Start(int seconds, const std::string& reason);

    // Stop the timer (if running) and post the completion event
    // with cancelled=true.  Safe to call when idle (no-op).
    void Cancel();

    bool IsRunning() const { return m_running; }

private:
    void OnTimer(wxTimerEvent& evt);
    void PostResult(bool cancelled);

    wxEvtHandler*                    m_eventHandler;
    std::weak_ptr<std::atomic<bool>> m_aliveToken;
    wxTimer                          m_timer;

    bool        m_running          = false;
    int         m_requestedSeconds = 0;
    std::string m_reason;
    wxStopWatch m_stopwatch;
};
