#pragma once
// Cross-platform case-insensitive string compare.
// Windows: _stricmp / _strnicmp
// POSIX: strcasecmp / strncasecmp

#include <string>
#include <string_view>

namespace str_case {

inline int icmp(std::string_view a, std::string_view b) {
#ifdef _WIN32
    return _stricmp(a.data(), b.data());
#else
    return strcasecmp(a.data(), b.data());
#endif
}

inline int nicmp(std::string_view a, std::string_view b, size_t n) {
#ifdef _WIN32
    return _strnicmp(a.data(), b.data(), n);
#else
    return strncasecmp(a.data(), b.data(), n);
#endif
}

inline bool iequals(std::string_view a, std::string_view b) {
    return icmp(a, b) == 0;
}

inline bool istarts_with(std::string_view a, std::string_view prefix) {
    return a.size() >= prefix.size() && nicmp(a.substr(0, prefix.size()), prefix, prefix.size()) == 0;
}

} // namespace str_case