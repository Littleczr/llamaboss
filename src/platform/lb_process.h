// lb_process.h
//
// Synchronous child-process runner for non-Windows builds.
//
// Covers what the Win32 code does with CreatePipe + CreateProcessW +
// WaitForSingleObject + Job Objects: argv-based launch (no shell
// quoting), optional working directory, optional stdin bytes, stdout and
// stderr captured concurrently so a chatty child can never deadlock on a
// full pipe, a timeout, cooperative cancellation, and whole-tree kill
// (the child gets its own process group, so kill(-pgid) reaches every
// grandchild just as closing a KILL_ON_JOB_CLOSE job does).
//
// Run() blocks; call it from a worker thread, as the Win32 paths do.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <sys/types.h>
#include <string>
#include <vector>

namespace lb_process {

struct Options {
    std::vector<std::string> argv;       // argv[0] is looked up on PATH if it has no '/'
    std::string cwd;                     // empty = inherit
    std::vector<std::string> envSet;     // "NAME=value" entries added/overriding the parent env
    std::vector<std::string> envUnset;   // names removed from the child env
    std::string stdinData;               // written then closed; empty = stdin is /dev/null
    bool mergeStderr = false;            // stderr goes into `out` as well
    unsigned long timeoutMs = 0;         // 0 = no timeout
    const std::atomic<bool>* cancel = nullptr;
    size_t maxCaptureBytes = 0;          // per stream, 0 = unlimited; excess is drained and dropped
    // Optional live output; called on the calling thread between polls.
    std::function<void(const char* data, size_t size, bool isStderr)> onOutput;
};

struct Result {
    bool started = false;
    int spawnError = 0;       // errno when !started
    int exitCode = -1;        // 128+signal when killed by a signal
    bool timedOut = false;
    bool cancelled = false;
    bool truncated = false;
    std::string out;
    std::string err;
    double elapsedSec = 0.0;
};

Result Run(const Options& options);

// ── Long-running children (servers) ─────────────────────────────────
//
// A detached child in its own process group with stdout/stderr sent to a
// log file. The Child object is the ONLY place that reaps the process, so
// several owners (the manager and a health-check thread) can share it the
// way the Windows code shares duplicated process handles.
struct SpawnOptions {
    std::vector<std::string> argv;
    std::string cwd;          // empty = inherit
    std::string logPath;      // stdout+stderr, truncated; empty = /dev/null
};

class Child {
public:
    explicit Child(pid_t pid) : m_pid(pid) {}
    pid_t Pid() const { return m_pid; }
    // Non-blocking; true once the process has exited (exit code cached).
    bool Exited(int* exitCode = nullptr);
    // SIGTERM to the group, wait up to graceMs, then SIGKILL and reap.
    // Returns true if the process is gone afterwards.
    bool Terminate(unsigned long graceMs);
private:
    std::mutex m_mutex;
    pid_t m_pid;
    bool m_exited = false;
    int m_exitCode = -1;
};

// Returns null on failure with errno-style code in `error`.
std::shared_ptr<Child> Spawn(const SpawnOptions& options, int& error);

// Resolve a program name on PATH (or return it unchanged when it already
// contains a '/'). Empty when not found or not executable.
std::string FindExecutable(const std::string& name);

} // namespace lb_process
