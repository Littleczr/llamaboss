#pragma once
//
// lb_background_threads.h — joins fire-and-forget background threads at
// process exit.
//
// Moved out of LlamaBoss.cpp so the About dialog (update check + installer
// download) can use it too.  Behavior is unchanged:
//
// CheckForUpdates() used to std::thread(...).detach().  If the user quit
// while the HTTP check was stalled, the leftover thread kept running
// through static destruction and could touch function-local statics
// (ui_event_post's mutex) mid-teardown -- the classic sporadic
// crash-on-exit that never reproduces under a debugger.
//
// Threads launched through the keeper behave exactly like detached ones
// while the app runs; the keeper's destructor joins them at exit.  The
// singleton is constructed lazily on first use (well after the statics
// those threads depend on), so reverse-order static destruction
// guarantees the join happens while everything they touch is still
// alive.
//
// Anything launched here must be bounded: the update check has an 8 s
// network timeout, and the installer download checks a cancel flag
// between reads (each read also bounded by the WinHTTP timeout), so the
// worst-case exit delay stays small.
//
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

class LbBackgroundThreadKeeper
{
public:
    static LbBackgroundThreadKeeper& Instance()
    {
        static LbBackgroundThreadKeeper keeper;
        return keeper;
    }

    void Launch(std::function<void()> fn)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_threads.emplace_back(std::move(fn));
    }

    ~LbBackgroundThreadKeeper()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& t : m_threads) {
            if (t.joinable())
                t.join();
        }
    }

    LbBackgroundThreadKeeper(const LbBackgroundThreadKeeper&) = delete;
    LbBackgroundThreadKeeper& operator=(const LbBackgroundThreadKeeper&) = delete;

private:
    LbBackgroundThreadKeeper() = default;

    // Update checks/downloads are rare and user-initiated, so the vector
    // holds at most a handful of entries per session; finished threads
    // join instantly at exit.
    std::mutex m_mutex;
    std::vector<std::thread> m_threads;
};
