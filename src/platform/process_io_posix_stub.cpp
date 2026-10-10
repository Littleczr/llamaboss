// process_io_posix_stub.cpp — POSIX stub (Phase 0)
#include "process_io.h"

namespace process_io {

class StubPipes : public PipePair {
public:
    int Write(std::string_view) override { return -1; }
    void CloseWrite() override {}
    int Read(std::string&, size_t) override { return 0; }
    int ReadErr(std::string&, size_t) override { return 0; }
};

std::unique_ptr<PipePair> CreatePipes() {
    return nullptr;
}

} // namespace process_io