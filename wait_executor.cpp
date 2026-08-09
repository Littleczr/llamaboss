// wait_executor.cpp
// See wait_executor.h.
#include "wait_executor.h"

#include "ui_event_post.h"   // LbQueueEventIfAlive

wxDEFINE_EVENT(wxEVT_WAIT_COMPLETE, wxCommandEvent);

WaitExecutor::WaitExecutor(wxEvtHandler* eventHandler,
                           std::weak_ptr<std::atomic<bool>> aliveToken)
    : m_eventHandler(eventHandler)
    , m_aliveToken(std::move(aliveToken))
    , m_timer(this)
{
    Bind(wxEVT_TIMER, &WaitExecutor::OnTimer, this);
}

WaitExecutor::~WaitExecutor()
{
    // Silent teardown: the frame is going away, so there is no one
    // to receive a cancelled completion.  Just stop the timer.
    if (m_timer.IsRunning()) m_timer.Stop();
}

bool WaitExecutor::Start(int seconds, const std::string& reason)
{
    wxASSERT(wxIsMainThread());
    if (m_running) return false;
    if (seconds <= 0) return false;

    m_running          = true;
    m_requestedSeconds = seconds;
    m_reason           = reason;
    m_stopwatch.Start();

    // One-shot; wxTimer takes milliseconds.  seconds is bounded by
    // the caller (<= 600), so the int multiply cannot overflow.
    if (!m_timer.StartOnce(seconds * 1000)) {
        m_running = false;
        return false;
    }
    return true;
}

void WaitExecutor::Cancel()
{
    wxASSERT(wxIsMainThread());
    if (!m_running) return;
    if (m_timer.IsRunning()) m_timer.Stop();
    PostResult(/*cancelled=*/true);
}

void WaitExecutor::OnTimer(wxTimerEvent& WXUNUSED(evt))
{
    if (!m_running) return;    // stale fire after Cancel; ignore
    PostResult(/*cancelled=*/false);
}

void WaitExecutor::PostResult(bool cancelled)
{
    m_running = false;

    WaitResult result;
    result.requestedSeconds = m_requestedSeconds;
    result.reason           = m_reason;
    result.cancelled        = cancelled;
    result.elapsedSec = m_stopwatch.Time() / 1000.0;

    auto* evt = new wxCommandEvent(wxEVT_WAIT_COMPLETE);
    evt->SetClientObject(new WaitResultClientData(std::move(result)));

    // Queued (not processed inline) on purpose: the completion must
    // never run re-entrantly inside Cancel()'s caller, and the alive
    // token guards a frame mid-close.  LbQueueEventIfAlive deletes
    // the event (and its client object) itself when the target is
    // already dead.
    LbQueueEventIfAlive(m_eventHandler, m_aliveToken, evt);
}
