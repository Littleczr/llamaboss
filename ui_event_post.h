#pragma once

// ui_event_post.h
//
// Thread-safe guard for posting worker-thread events back to wx UI objects.
//
// The old pattern was:
//   if (alive && alive->load()) wxQueueEvent(handler, ev);
//
// That leaves a small but real race: the UI object can be destroyed after the
// alive check but before wxQueueEvent runs.  LbMarkUiEventTargetDead() and
// LbQueueEventIfAlive() share the same mutex, so once close/destruction starts
// and marks the target dead, no worker can pass the check-and-queue window.

#include <wx/event.h>

#include <atomic>
#include <memory>
#include <mutex>

namespace llamaboss_ui_event_post {

inline std::mutex& UiPostMutex()
{
    static std::mutex m;
    return m;
}

// ── Payload reclamation on the rejected-event path ───────────────
// wxCommandEvent does NOT own its client object: wx/event.h keeps a
// raw m_clientObject pointer and no wx destructor touches it (it
// exists to point at control-owned listbox item data).  So when we
// reject an event because the target is gone, `delete ev` frees the
// event but leaks whatever the producer attached via
// SetClientObject() -- a full CmdResult, PythonRunResult,
// WebFetchResult, conversation SaveSnapshot, etc.
//
// Every producer in LlamaBoss attaches payloads to wxCommandEvent
// (or a subclass), so a single downcast covers all of them.  Events
// with no client object are unaffected: GetClientObject() returns
// nullptr and `delete nullptr` is a no-op.
inline void ReclaimClientObject(wxEvent* ev)
{
    if (auto* cmd = dynamic_cast<wxCommandEvent*>(ev)) {
        delete cmd->GetClientObject();
        cmd->SetClientObject(nullptr);
    }
}

inline void MarkDead(const std::shared_ptr<std::atomic<bool>>& aliveToken)
{
    std::lock_guard<std::mutex> lock(UiPostMutex());
    if (aliveToken) {
        aliveToken->store(false, std::memory_order_release);
    }
}

inline bool QueueIfAlive(wxEvtHandler* handler,
                         const std::weak_ptr<std::atomic<bool>>& aliveToken,
                         wxEvent* ev)
{
    if (!ev) return false;
    if (!handler) {
        ReclaimClientObject(ev);
        delete ev;
        return false;
    }

    std::lock_guard<std::mutex> lock(UiPostMutex());

    auto alive = aliveToken.lock();
    if (!alive || !alive->load(std::memory_order_acquire)) {
        ReclaimClientObject(ev);
        delete ev;
        return false;
    }

    wxQueueEvent(handler, ev);
    return true;
}

} // namespace llamaboss_ui_event_post

inline void LbMarkUiEventTargetDead(const std::shared_ptr<std::atomic<bool>>& aliveToken)
{
    llamaboss_ui_event_post::MarkDead(aliveToken);
}

inline bool LbQueueEventIfAlive(wxEvtHandler* handler,
                                const std::weak_ptr<std::atomic<bool>>& aliveToken,
                                wxEvent* ev)
{
    return llamaboss_ui_event_post::QueueIfAlive(handler, aliveToken, ev);
}
