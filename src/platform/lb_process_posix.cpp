// lb_process_posix.cpp — see lb_process.h
#include "lb_process.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace lb_process {

namespace {

void SetCloexecNonblock(int fd, bool nonblock)
{
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (nonblock) ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
}

bool MakePipe(int fds[2])
{
    if (::pipe(fds) != 0) return false;
    SetCloexecNonblock(fds[0], false);
    SetCloexecNonblock(fds[1], false);
    return true;
}

void CloseFd(int& fd)
{
    if (fd >= 0) ::close(fd);
    fd = -1;
}

std::vector<std::string> BuildEnv(const Options& o)
{
    std::vector<std::string> env;
    auto nameOf = [](const std::string& kv) { return kv.substr(0, kv.find('=')); };
    auto overridden = [&](const std::string& name) {
        for (const auto& u : o.envUnset) if (u == name) return true;
        for (const auto& s : o.envSet) if (nameOf(s) == name) return true;
        return false;
    };
    for (char** e = environ; e && *e; ++e) {
        std::string kv(*e);
        if (!overridden(nameOf(kv))) env.push_back(std::move(kv));
    }
    for (const auto& s : o.envSet) env.push_back(s);
    return env;
}

void KillGroup(pid_t pgid)
{
    if (pgid <= 0) return;
    ::kill(-pgid, SIGTERM);
    for (int i = 0; i < 20; ++i) {           // up to ~1s for a clean exit
        if (::kill(-pgid, 0) != 0) return;
        ::usleep(50000);
    }
    ::kill(-pgid, SIGKILL);
}

} // namespace

std::string FindExecutable(const std::string& name)
{
    if (name.empty()) return {};
    if (name.find('/') != std::string::npos)
        return ::access(name.c_str(), X_OK) == 0 ? name : std::string();
    const char* path = std::getenv("PATH");
    std::string dirs = path && *path ? path : "/usr/bin:/bin:/usr/sbin:/sbin";
    // GUI apps launched from Finder get a minimal PATH; Homebrew lives here.
    dirs += ":/opt/homebrew/bin:/usr/local/bin";
    size_t start = 0;
    while (start <= dirs.size()) {
        const size_t end = dirs.find(':', start);
        std::string dir = dirs.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!dir.empty()) {
            std::string cand = dir + "/" + name;
            struct stat st {};
            if (::stat(cand.c_str(), &st) == 0 && S_ISREG(st.st_mode) && ::access(cand.c_str(), X_OK) == 0)
                return cand;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return {};
}

Result Run(const Options& o)
{
    Result r;
    const auto t0 = std::chrono::steady_clock::now();
    if (o.argv.empty()) { r.spawnError = EINVAL; return r; }

    const std::string exe = FindExecutable(o.argv[0]);
    if (exe.empty()) { r.spawnError = ENOENT; return r; }

    int outPipe[2] = {-1, -1}, errPipe[2] = {-1, -1}, inPipe[2] = {-1, -1};
    if (!MakePipe(outPipe) || (!o.mergeStderr && !MakePipe(errPipe)) ||
        (!o.stdinData.empty() && !MakePipe(inPipe))) {
        r.spawnError = errno;
        for (int* p : {outPipe, errPipe, inPipe}) { CloseFd(p[0]); CloseFd(p[1]); }
        return r;
    }

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (inPipe[0] >= 0) posix_spawn_file_actions_adddup2(&fa, inPipe[0], STDIN_FILENO);
    else posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, outPipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, o.mergeStderr ? outPipe[1] : errPipe[1], STDERR_FILENO);
    if (!o.cwd.empty()) posix_spawn_file_actions_addchdir_np(&fa, o.cwd.c_str());

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    // New process group (tree kill) and a clean signal state for the child.
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &all);
    posix_spawnattr_setpgroup(&attr, 0);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK |
                                     POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_CLOEXEC_DEFAULT);

    std::vector<std::string> env = BuildEnv(o);
    std::vector<char*> envp;
    for (auto& e : env) envp.push_back(e.data());
    envp.push_back(nullptr);
    std::vector<char*> argv;
    std::vector<std::string> args = o.argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);

    pid_t pid = -1;
    const int rc = ::posix_spawn(&pid, exe.c_str(), &fa, &attr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);

    CloseFd(outPipe[1]);
    CloseFd(errPipe[1]);
    CloseFd(inPipe[0]);
    if (rc != 0) {
        r.spawnError = rc;
        CloseFd(outPipe[0]); CloseFd(errPipe[0]); CloseFd(inPipe[1]);
        return r;
    }
    r.started = true;

    SetCloexecNonblock(outPipe[0], true);
    if (errPipe[0] >= 0) SetCloexecNonblock(errPipe[0], true);
    if (inPipe[1] >= 0) SetCloexecNonblock(inPipe[1], true);

    // A child that exits early must not kill us with SIGPIPE on stdin writes.
    struct sigaction ignore {}, oldPipe {};
    ignore.sa_handler = SIG_IGN;
    ::sigaction(SIGPIPE, &ignore, &oldPipe);

    size_t inOffset = 0;
    char buf[16384];
    auto append = [&](std::string& dst, const char* data, size_t n) {
        if (o.maxCaptureBytes && dst.size() + n > o.maxCaptureBytes) {
            r.truncated = true;
            if (dst.size() < o.maxCaptureBytes) dst.append(data, o.maxCaptureBytes - dst.size());
        } else {
            dst.append(data, n);
        }
    };

    bool killed = false;
    bool reaped = false;
    int status = 0;
    while (outPipe[0] >= 0 || errPipe[0] >= 0) {
        // Once the child itself exits, anything left in its group would
        // only hold the pipes open; take the tree down as a job close does.
        if (!reaped && ::waitpid(pid, &status, WNOHANG) == pid) {
            reaped = true;
            ::kill(-pid, SIGKILL);
        }
        if (!killed && !reaped) {
            const bool cancelled = o.cancel && o.cancel->load();
            const bool timedOut = o.timeoutMs &&
                std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(o.timeoutMs);
            if (cancelled || timedOut) {
                r.cancelled = cancelled;
                r.timedOut = !cancelled && timedOut;
                KillGroup(pid);
                killed = true;
            }
        }

        pollfd fds[3];
        int n = 0;
        int outIdx = -1, errIdx = -1, inIdx = -1;
        if (outPipe[0] >= 0) { outIdx = n; fds[n++] = {outPipe[0], POLLIN, 0}; }
        if (errPipe[0] >= 0) { errIdx = n; fds[n++] = {errPipe[0], POLLIN, 0}; }
        if (inPipe[1] >= 0)  { inIdx = n;  fds[n++] = {inPipe[1], POLLOUT, 0}; }
        const int pr = ::poll(fds, n, 100);
        if (pr < 0 && errno != EINTR) break;
        if (pr <= 0) continue;

        auto drain = [&](int idx, int& fd, std::string& dst, bool isErr) {
            if (idx < 0 || !(fds[idx].revents & (POLLIN | POLLHUP | POLLERR))) return;
            for (;;) {
                const ssize_t got = ::read(fd, buf, sizeof(buf));
                if (got > 0) {
                    append(dst, buf, static_cast<size_t>(got));
                    if (o.onOutput) o.onOutput(buf, static_cast<size_t>(got), isErr);
                    continue;
                }
                if (got < 0 && (errno == EAGAIN || errno == EINTR)) return;
                CloseFd(fd);  // EOF or error
                return;
            }
        };
        drain(outIdx, outPipe[0], r.out, false);
        drain(errIdx, errPipe[0], r.err, true);

        if (inIdx >= 0 && (fds[inIdx].revents & (POLLOUT | POLLERR | POLLHUP))) {
            const ssize_t w = ::write(inPipe[1], o.stdinData.data() + inOffset,
                                      o.stdinData.size() - inOffset);
            if (w > 0) inOffset += static_cast<size_t>(w);
            if ((w < 0 && errno != EAGAIN && errno != EINTR) || inOffset >= o.stdinData.size())
                CloseFd(inPipe[1]);
        }
    }
    CloseFd(inPipe[1]);
    CloseFd(outPipe[0]);
    CloseFd(errPipe[0]);

    if (!reaped)
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    ::sigaction(SIGPIPE, &oldPipe, nullptr);
    // Grandchildren that kept running after the child exited are part of
    // the tree too; a Job Object would have taken them down on close.
    ::kill(-pid, SIGKILL);

    if (WIFEXITED(status)) r.exitCode = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) r.exitCode = 128 + WTERMSIG(status);
    r.elapsedSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

bool Child::Exited(int* exitCode)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_exited && m_pid > 0) {
        int status = 0;
        const pid_t r = ::waitpid(m_pid, &status, WNOHANG);
        if (r == m_pid) {
            m_exited = true;
            m_exitCode = WIFEXITED(status) ? WEXITSTATUS(status)
                       : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
        } else if (r < 0 && errno == ECHILD) {
            m_exited = true;   // already reaped elsewhere; treat as gone
        }
    }
    if (exitCode) *exitCode = m_exitCode;
    return m_exited;
}

bool Child::Terminate(unsigned long graceMs)
{
    if (Exited()) return true;
    ::kill(-m_pid, SIGTERM);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(graceMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (Exited()) { ::kill(-m_pid, SIGKILL); return true; }  // sweep stragglers in the group
        ::usleep(50000);
    }
    ::kill(-m_pid, SIGKILL);
    for (int i = 0; i < 40; ++i) {          // up to ~2s for the kernel to reap
        if (Exited()) return true;
        ::usleep(50000);
    }
    return Exited();
}

std::shared_ptr<Child> Spawn(const SpawnOptions& o, int& error)
{
    error = 0;
    if (o.argv.empty()) { error = EINVAL; return nullptr; }
    const std::string exe = FindExecutable(o.argv[0]);
    if (exe.empty()) { error = ENOENT; return nullptr; }

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (o.logPath.empty()) {
        posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    } else {
        posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, o.logPath.c_str(),
                                         O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    posix_spawn_file_actions_adddup2(&fa, STDOUT_FILENO, STDERR_FILENO);
    if (!o.cwd.empty()) posix_spawn_file_actions_addchdir_np(&fa, o.cwd.c_str());

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &all);
    posix_spawnattr_setpgroup(&attr, 0);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK |
                                     POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_CLOEXEC_DEFAULT);

    std::vector<std::string> args = o.argv;
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);

    pid_t pid = -1;
    const int rc = ::posix_spawn(&pid, exe.c_str(), &fa, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) { error = rc; return nullptr; }
    return std::make_shared<Child>(pid);
}

} // namespace lb_process
