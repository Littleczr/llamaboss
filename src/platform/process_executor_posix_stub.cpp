// process_executor_posix_stub.cpp — POSIX stub (Phase 0: returns "unimplemented")
#include "process_executor.h"

#include <memory>

namespace process_executor {

class StubHandle : public Handle {
public:
    bool Cancel() override { return true; }
    bool IsRunning() const override { return false; }
};

std::unique_ptr<Handle> Start(std::string_view /*command*/,
                              const std::vector<std::string>& /*args*/,
                              std::string_view /*cwd*/,
                              unsigned long /*timeoutMs*/,
                              OutputCallback /*onOutput*/,
                              CompletionCallback onComplete) {
    if (onComplete) {
        Result r;
        r.exitCode = -1;
        r.stderrText = "process_executor not implemented on this platform (Phase 0 stub)";
        onComplete(r);
    }
    return std::make_unique<StubHandle>();
}

} // namespace process_executor