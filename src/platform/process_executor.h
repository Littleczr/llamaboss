#pragma once
// process_executor.h — Process spawn + tree kill abstraction
// Windows: CreateProcessW + Job Object (JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE)
// POSIX: posix_spawn + process group (setpgid + kill(-pgid))

#include <string>
#include <string_view>
#include <functional>
#include <atomic>
#include <memory>

namespace process_executor {

struct Result {
    int exitCode = -1;
    std::string stdoutText;
    std::string stderrText;
    bool timedOut = false;
    bool cancelled = false;
    bool truncated = false;
    double elapsedSec = 0.0;
};

using OutputCallback = std::function<void(std::string_view stdoutChunk, std::string_view stderrChunk)>;
using CompletionCallback = std::function<void(const Result&)>;

class Handle {
public:
    virtual ~Handle() = default;
    virtual bool Cancel() = 0;
    virtual bool IsRunning() const = 0;
};

// Start a process.
// `command` — full command line (Windows) or argv[0] (POSIX)
// `args` — arguments (empty on Windows, used on POSIX)
// `cwd` — working directory (UTF-8), empty = inherit
// `timeoutMs` — 0 = no timeout
// `onOutput` — called with stdout/stderr chunks (may be called from worker thread)
// `onComplete` — called when process exits (may be called from worker thread)
// Returns null on immediate failure (e.g., process couldn't start).
std::unique_ptr<Handle> Start(std::string_view command,
                              const std::vector<std::string>& args,
                              std::string_view cwd,
                              unsigned long timeoutMs,
                              OutputCallback onOutput,
                              CompletionCallback onComplete);

} // namespace process_executor