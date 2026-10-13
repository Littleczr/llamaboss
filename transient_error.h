#pragma once
#include "lb_ascii.h"
// transient_error.h -- classify model-request errors that are worth an
// automatic retry inside a running agent loop.
//
// Why this exists: an agent turn used to end on the FIRST transport hiccup.
// A dropped socket or a 502/503 from a cloud provider in the middle of a
// 40-step task showed one system line, ended the turn, and left the user to
// type "continue".  That line is not part of the saved transcript, so the
// exported chat simply stopped after a successful tool result.
//
// The rule here is deliberately conservative: retry only failures that say
// the request never produced a usable reply AND are known to clear on their
// own (network drop, timeout, gateway/overload status, a stream that ended
// before its terminal event).  Anything that a retry cannot fix -- bad
// request, auth/policy refusal, model not found, context too long,
// repetition stop, oversized stream -- is NOT transient.
//
// Pure string logic, header-only, no wx/Poco: tested by
// transient_error_tests.cpp (g++ -std=c++17 transient_error_tests.cpp).

#include <string>
#include <cctype>

namespace lb_transient_error {

inline std::string Lower(const std::string& s)
{
    std::string out = s;
    for (char& c : out)
        c = static_cast<char>(lb_ascii::ToLower(static_cast<unsigned char>(c)));
    return out;
}

inline bool Has(const std::string& hay, const char* needle)
{
    return hay.find(needle) != std::string::npos;
}

// Maximum automatic retries for one model request, and the wait before
// each one.  Attempt numbers are 1-based.
constexpr int kMaxRetries = 2;

inline int RetryDelayMs(int attempt)
{
    return attempt <= 1 ? 3000 : 10000;
}

inline bool IsTransient(const std::string& error)
{
    if (error.empty()) return false;
    const std::string e = Lower(error);

    // ── Never retry: a retry would send the same doomed request ──
    if (Has(e, "stopped this reply") ||          // repetition / tool-call loop stop
        Has(e, "exceeded the maximum") ||        // oversized stream / event
        Has(e, "single line") ||                 // line overflow abort
        Has(e, "not found") ||                   // model / endpoint missing
        Has(e, "context length") ||
        Has(e, "context_length") ||
        Has(e, "maximum context") ||
        Has(e, "too many tokens") ||
        Has(e, "prompt is too long") ||
        Has(e, "invalid_request") ||
        Has(e, "sign-in") || Has(e, "sign in") ||
        Has(e, "blocked by an account") ||
        Has(e, "api key") ||
        Has(e, "unauthorized") ||
        Has(e, "forbidden") ||
        Has(e, "insufficient") ||                // credits / quota exhausted
        Has(e, "quota") ||
        Has(e, "usage limit") ||
        Has(e, "usage_limit") ||                 // plan codes: usage_limit_reached, ...
        Has(e, "limit_exceeded") ||              // subscription_sharing_usage_limit_exceeded
        Has(e, "not eligible") ||
        Has(e, "invalid json") ||
        Has(e, "malformed event") ||
        Has(e, "failed to start chat request") ||
        Has(e, "instead of a stream"))
        return false;

    // ── HTTP status codes that clear on their own ─────────────────
    // "API Error (503): ..." and "... request failed (HTTP 502)."
    static const char* kStatus[] = { "408", "409", "425", "429", "500",
                                     "502", "503", "504", "520", "522",
                                     "524", "529" };
    for (const char* code : kStatus) {
        if (Has(e, (std::string("api error (") + code).c_str()) ||
            Has(e, (std::string("(http ") + code).c_str()))
            return true;
    }
    if (Has(e, "api error (")) return false;   // any other HTTP status

    // ── Transport / stream failures ───────────────────────────────
    return Has(e, "network error") ||
           Has(e, "connection reset") ||
           Has(e, "connection aborted") ||
           Has(e, "connection refused") ||
           Has(e, "no message received") ||
           Has(e, "timeout") ||
           Has(e, "timed out") ||
           Has(e, "stream ended unexpectedly") ||
           Has(e, "stream ended before the server sent") ||
           Has(e, "server returned an error mid-stream") ||
           Has(e, "overloaded") ||
           Has(e, "server_error") ||
           Has(e, "internal server error") ||
           Has(e, "bad gateway") ||
           Has(e, "service unavailable") ||
           Has(e, "temporarily unavailable") ||
           Has(e, "rate limit") ||
           Has(e, "try again");
}

// One-line form of an error for the retry notice (first line, bounded).
inline std::string ShortForm(const std::string& error, size_t maxChars = 160)
{
    std::string s = error.substr(0, error.find('\n'));
    if (s.size() > maxChars) s = s.substr(0, maxChars) + "...";
    return s;
}

} // namespace lb_transient_error
