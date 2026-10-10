#pragma once
// process_io.h — Pipe I/O for process communication
// Windows: Anonymous pipes + reader threads
// POSIX: pipe() + poll() + reader threads (Phase 1+)

#include <string>
#include <string_view>
#include <functional>

namespace process_io {

class PipePair {
public:
    virtual ~PipePair() = default;
    // Write to stdin of child process. Returns bytes written, or -1 on error.
    virtual int Write(std::string_view data) = 0;
    // Close stdin (EOF to child).
    virtual void CloseWrite() = 0;
    // Read from stdout. Returns bytes read, 0 = EOF, -1 = error.
    virtual int Read(std::string& out, size_t maxBytes) = 0;
    // Read from stderr.
    virtual int ReadErr(std::string& out, size_t maxBytes) = 0;
};

// Create pipes for a new process. Returns null on failure.
// On Windows, also returns handles suitable for CreateProcessW.
std::unique_ptr<PipePair> CreatePipes();

} // namespace process_io