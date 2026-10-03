#pragma once

// server_log_hints.h
//
// Turns the most common llama-server startup failures into one plain
// sentence shown ABOVE the raw log tail.  The log stays for anyone who
// wants it; the sentence is for everyone else.
// Pure logic, no wx / Poco -- tested standalone by server_log_hints_tests.cpp.

#include <cctype>
#include <string>

namespace server_log_hints {

namespace detail {
inline std::string Lower(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
} // namespace detail

// Returns an empty string when nothing recognizable is in the tail.
inline std::string Explain(const std::string& logTail)
{
    const std::string lower = detail::Lower(logTail);

    // "unknown model architecture: 'spark2_5'"
    const std::string archKey = "unknown model architecture: '";
    const size_t at = lower.find(archKey);
    if (at != std::string::npos) {
        const size_t nameStart = at + archKey.size();
        const size_t nameEnd = logTail.find('\'', nameStart);
        const std::string arch = nameEnd == std::string::npos
            ? std::string() : logTail.substr(nameStart, nameEnd - nameStart);
        return "This model uses the '" + arch + "' architecture, which the "
               "llama.cpp engine bundled with LlamaBoss doesn't support yet. "
               "Models built on brand-new architectures need a newer llama.cpp "
               "release. The file itself is fine: keep it for a future "
               "LlamaBoss update, or choose another model for now.";
    }

    if (lower.find("out of memory") != std::string::npos ||
        lower.find("failed to allocate") != std::string::npos ||
        lower.find("unable to allocate") != std::string::npos) {
        return "The model didn't fit in memory. Try a smaller download of the "
               "same model (a lower quant such as Q4 instead of Q8) or a "
               "shorter context length in Settings.";
    }

    if (lower.find("invalid magic") != std::string::npos ||
        lower.find("failed to read magic") != std::string::npos ||
        lower.find("tensor data is not within the file bounds") != std::string::npos) {
        return "The model file looks damaged or incomplete. Delete it in "
               "Manage Models and download it again.";
    }

    return std::string();
}

} // namespace server_log_hints
