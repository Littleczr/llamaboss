// process_io_posix.cpp — POSIX pipe I/O for process communication
// Uses pipe() + poll() + reader threads

#include "process_io.h"

#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <memory>
#include <cerrno>

namespace process_io {

class PosixPipePair : public PipePair {
public:
    PosixPipePair(int childStdout, int childStderr, int stdinWrite)
        : m_childStdout(childStdout), m_childStderr(childStderr), m_stdinWrite(stdinWrite) {}

    ~PosixPipePair() {
        if (m_childStdout >= 0) close(m_childStdout);
        if (m_childStderr >= 0) close(m_childStderr);
        if (m_stdinWrite >= 0) close(m_stdinWrite);
    }

    int Write(std::string_view data) override {
        if (m_stdinWrite < 0) return -1;
        const char* ptr = data.data();
        size_t remain = data.size();
        while (remain > 0) {
            ssize_t n = write(m_stdinWrite, ptr, remain);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    struct pollfd pfd{m_stdinWrite, POLLOUT, 0};
                    if (poll(&pfd, 1, 100) <= 0) continue;
                    continue;
                }
                return -1;
            }
            ptr += n;
            remain -= n;
        }
        return static_cast<int>(data.size());
    }

    void CloseWrite() override {
        if (m_stdinWrite >= 0) {
            close(m_stdinWrite);
            m_stdinWrite = -1;
        }
    }

    int Read(std::string& out, size_t maxBytes) override {
        if (m_childStdout < 0) return 0;
        return ReadFd(m_childStdout, out, maxBytes);
    }

    int ReadErr(std::string& out, size_t maxBytes) override {
        if (m_childStderr < 0) return 0;
        return ReadFd(m_childStderr, out, maxBytes);
    }

private:
    int m_childStdout = -1;
    int m_childStderr = -1;
    int m_stdinWrite = -1;

    int ReadFd(int fd, std::string& out, size_t maxBytes) {
        char buf[4096];
        size_t toRead = std::min(maxBytes, sizeof(buf));
        struct pollfd pfd{fd, POLLIN, 0};
        int ret = poll(&pfd, 1, 0); // Non-blocking check
        if (ret <= 0) return 0; // No data or error

        if (!(pfd.revents & POLLIN)) return 0;

        ssize_t n = read(fd, buf, toRead);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1; // Error
        }
        if (n == 0) return 0; // EOF
        out.append(buf, n);
        return static_cast<int>(n);
    }
};

std::unique_ptr<PipePair> CreatePipes() {
    int stdinPipe[2] = {-1, -1};
    int stdoutPipe[2] = {-1, -1};
    int stderrPipe[2] = {-1, -1};

    if (pipe(stdinPipe) != 0 || pipe(stdoutPipe) != 0 || pipe(stderrPipe) != 0) {
        if (stdinPipe[0] >= 0) { close(stdinPipe[0]); close(stdinPipe[1]); }
        if (stdoutPipe[0] >= 0) { close(stdoutPipe[0]); close(stdoutPipe[1]); }
        if (stderrPipe[0] >= 0) { close(stderrPipe[0]); close(stderrPipe[1]); }
        return nullptr;
    }

    // Parent: stdinPipe[1] = write to child stdin
    //         stdoutPipe[0] = read from child stdout
    //         stderrPipe[0] = read from child stderr
    // Child gets: stdinPipe[0], stdoutPipe[1], stderrPipe[1]

    // Set non-blocking on read ends
    fcntl(stdoutPipe[0], F_SETFL, O_NONBLOCK);
    fcntl(stderrPipe[0], F_SETFL, O_NONBLOCK);

    return std::make_unique<PosixPipePair>(stdoutPipe[0], stderrPipe[0], stdinPipe[1]);
}

} // namespace process_io