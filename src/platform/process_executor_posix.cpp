// process_executor_posix.cpp — POSIX process spawn + tree kill
// Uses posix_spawn + process group (setpgid + kill(-pgid))

#include "process_executor.h"

#include <spawn.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <poll.h>
#include <fcntl.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <memory>
#include <chrono>
#include <cerrno>
#include <cstring>

extern char** environ;

namespace process_executor {

class PosixHandle : public Handle {
public:
    PosixHandle(pid_t pid, int pgid, std::thread&& stdoutThread, std::thread&& stderrThread,
                std::shared_ptr<std::atomic<bool>> cancelled,
                CompletionCallback onComplete, Result&& result)
        : m_pid(pid), m_pgid(pgid),
          m_stdoutThread(std::move(stdoutThread)), m_stderrThread(std::move(stderrThread)),
          m_cancelled(cancelled), m_onComplete(std::move(onComplete)), m_result(std::move(result)) {}

    ~PosixHandle() {
        if (m_stdoutThread.joinable()) m_stdoutThread.join();
        if (m_stderrThread.joinable()) m_stderrThread.join();
    }

    bool Cancel() override {
        m_cancelled->store(true);
        if (m_pgid > 0) {
            // Kill entire process group
            kill(-m_pgid, SIGKILL);
        }
        return true;
    }

    bool IsRunning() const override {
        if (m_pid <= 0) return false;
        int status = 0;
        pid_t r = waitpid(m_pid, &status, WNOHANG);
        if (r == 0) return true;  // Still running
        if (r == -1) return false; // Error or already reaped
        return false; // Exited
    }

private:
    pid_t m_pid = -1;
    pid_t m_pgid = -1;
    std::thread m_stdoutThread, m_stderrThread;
    std::shared_ptr<std::atomic<bool>> m_cancelled;
    CompletionCallback m_onComplete;
    Result m_result;
};

static void CloseFds(int* fds, int count) {
    for (int i = 0; i < count; ++i) {
        if (fds[i] >= 0) close(fds[i]);
    }
}

static void ReaderLoop(int fd, std::string& out, std::atomic<bool>& cancelled,
                       OutputCallback cb, bool isStdout) {
    constexpr size_t kBufSize = 4096;
    char buf[kBufSize];

    while (true) {
        struct pollfd pfd{fd, POLLIN, 0};
        int ret = poll(&pfd, 1, 100); // 100ms timeout for cancel check
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ret == 0) {
            if (cancelled.load()) break;
            continue;
        }
        if (!(pfd.revents & POLLIN)) break;

        ssize_t n = read(fd, buf, kBufSize);
        if (n <= 0) break; // EOF or error
        if (cancelled.load()) break;

        out.append(buf, n);
        if (cb) {
            if (isStdout) cb(std::string_view(buf, n), std::string_view());
            else cb(std::string_view(), std::string_view(buf, n));
        }
    }
}

std::unique_ptr<Handle> Start(std::string_view command,
                              const std::vector<std::string>& args,
                              std::string_view cwd,
                              unsigned long timeoutMs,
                              OutputCallback onOutput,
                              CompletionCallback onComplete) {

    // Build argv
    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char*>(command.data()));
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    // Pipes for stdout/stderr
    int stdoutPipe[2] = {-1, -1};
    int stderrPipe[2] = {-1, -1};
    if (pipe(stdoutPipe) != 0 || pipe(stderrPipe) != 0) {
        CloseFds(stdoutPipe, 2);
        CloseFds(stderrPipe, 2);
        return nullptr;
    }

    // Set non-blocking on read ends
    fcntl(stdoutPipe[0], F_SETFL, O_NONBLOCK);
    fcntl(stderrPipe[0], F_SETFL, O_NONBLOCK);

    // posix_spawn attributes
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0); // New process group

    // File actions: redirect stdout/stderr
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, stdoutPipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, stderrPipe[1], STDERR_FILENO);
    // Close write ends in child (they're duplicated above)
    posix_spawn_file_actions_addclose(&actions, stdoutPipe[0]);
    posix_spawn_file_actions_addclose(&actions, stdoutPipe[1]);
    posix_spawn_file_actions_addclose(&actions, stderrPipe[0]);
    posix_spawn_file_actions_addclose(&actions, stderrPipe[1]);

    pid_t pid = -1;
    int spawnResult = posix_spawnp(&pid, command.data(), &actions, &attr,
                                    argv.data(), environ);

    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);

    // Parent closes write ends immediately
    close(stdoutPipe[1]); stdoutPipe[1] = -1;
    close(stderrPipe[1]); stderrPipe[1] = -1;

    if (spawnResult != 0 || pid <= 0) {
        CloseFds(stdoutPipe, 2);
        CloseFds(stderrPipe, 2);
        return nullptr;
    }

    // Process group ID = pid (since we setpgroup=0)
    pid_t pgid = pid;

    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    Result result;

    std::thread outThread(ReaderLoop, stdoutPipe[0], std::ref(result.stdoutText),
                          std::ref(*cancelled), onOutput, true);
    std::thread errThread(ReaderLoop, stderrPipe[0], std::ref(result.stderrText),
                          std::ref(*cancelled), onOutput, false);

    // Wait thread
    auto waitThread = std::thread([pid, pgid, timeoutMs,
                                    cancelled = std::move(cancelled),
                                    onComplete = std::move(onComplete),
                                    result = std::move(result),
                                    outThread = std::move(outThread),
                                    errThread = std::move(errThread)]() mutable {
        auto t0 = std::chrono::steady_clock::now();
        double deadline = timeoutMs ? (std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count() + timeoutMs / 1000.0) : 0;

        int status = 0;
        bool killIt = false;

        while (true) {
            pid_t r = waitpid(pid, &status, WNOHANG);
            if (r == pid) break; // Exited
            if (r == -1) { killIt = true; break; } // Error

            if (cancelled->load()) { result.cancelled = true; killIt = true; break; }
            if (deadline && std::chrono::duration<double>(
                    std::chrono::steady_clock::now().time_since_epoch()).count() >= deadline) {
                result.timedOut = true; killIt = true; break;
            }
            usleep(200000); // 200ms
        }

        if (killIt) {
            if (pgid > 0) kill(-pgid, SIGKILL);
            // Reap
            waitpid(pid, &status, 0);
        }

        if (outThread.joinable()) outThread.join();
        if (errThread.joinable()) errThread.join();

        if (WIFEXITED(status)) {
            result.exitCode = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            result.exitCode = 128 + WTERMSIG(status);
        } else {
            result.exitCode = -1;
        }

        result.elapsedSec = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count();

        if (onComplete) onComplete(result);
    });

    waitThread.detach(); // Handle owns the reader threads

    return std::make_unique<PosixHandle>(pid, pgid, std::move(outThread), std::move(errThread),
                                         cancelled, std::move(onComplete), std::move(result));
}

} // namespace process_executor