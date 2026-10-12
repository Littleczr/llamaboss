// background_work.h
//
// Process-wide count of detached background threads that are still running.
//
// Why this exists: LlamaBoss starts several fire-and-forget std::threads
// (generic tool worker, HTTPS probes, ChatGPT token revoke, slot-action
// queue, py session calls) and detaches them.  When the last window closes,
// MyApp::OnExit returns, main returns, and the C++ runtime destroys statics
// -- the global tool router, Poco's SSL manager and logger registry -- while
// those threads may still be running on Windows.  A thread that touches one
// of those objects at that moment is a use-after-free.
//
// The rule: every detached thread owns a BackgroundWorkToken for its whole
// lifetime (init-capture it into the thread lambda).  MyApp::OnExit calls
// DrainBackgroundWork() with a cap, so short work finishes before statics
// are destroyed.  The wait is CAPPED on purpose: synchronous tools (a hash
// of a huge file) cannot be interrupted, and exit must never hang on them.
//
// Header-only so no project-file change is needed.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace lb {

namespace detail {

struct BackgroundWorkState {
    std::mutex              mutex;
    std::condition_variable idle;
    int                     active = 0;
};

// Intentionally leaked: the state must outlive every static destructor,
// because a thread that overran the drain cap still decrements it.
inline BackgroundWorkState& GetBackgroundWorkState()
{
    static BackgroundWorkState* state = new BackgroundWorkState();
    return *state;
}

}  // namespace detail

// Movable RAII token.  Counted from construction (on the spawning thread,
// so a thread not yet scheduled is already counted) until destruction
// (when std::thread destroys the finished lambda, or immediately if the
// thread failed to start and the lambda was discarded).
class BackgroundWorkToken {
public:
    BackgroundWorkToken() : m_active(true)
    {
        auto& s = detail::GetBackgroundWorkState();
        std::lock_guard<std::mutex> lock(s.mutex);
        ++s.active;
    }

    BackgroundWorkToken(BackgroundWorkToken&& other) noexcept
        : m_active(other.m_active)
    {
        other.m_active = false;
    }

    BackgroundWorkToken& operator=(BackgroundWorkToken&&) = delete;
    BackgroundWorkToken(const BackgroundWorkToken&) = delete;
    BackgroundWorkToken& operator=(const BackgroundWorkToken&) = delete;

    ~BackgroundWorkToken()
    {
        if (!m_active) return;
        auto& s = detail::GetBackgroundWorkState();
        bool nowIdle = false;
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            nowIdle = (--s.active == 0);
        }
        if (nowIdle) s.idle.notify_all();
    }

private:
    bool m_active;
};

inline BackgroundWorkToken BeginBackgroundWork()
{
    return BackgroundWorkToken();
}

// Waits until no background work is running or the timeout expires.
// Returns the number of threads still running (0 means fully drained).
inline int DrainBackgroundWork(std::chrono::milliseconds timeout)
{
    auto& s = detail::GetBackgroundWorkState();
    std::unique_lock<std::mutex> lock(s.mutex);
    s.idle.wait_for(lock, timeout, [&s]() { return s.active == 0; });
    return s.active;
}

}  // namespace lb
