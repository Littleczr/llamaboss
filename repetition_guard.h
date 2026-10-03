// repetition_guard.h
//
// Detects a model stuck in a generation loop: the tail of the output is the
// same block of text repeated back-to-back, e.g. (seen with a quantized
// Qwen, inside its thinking):
//
//   - The 2022-2023 homelessness crisis
//   - The 2022-2023 tree felling controversy
//   - The 2024 "Portland Stings"
//   - The 2022-2023 homelessness crisis
//   ...
//
// Without a guard such a stream only ends at the context limit (131k tokens
// in the report above).  ChatWorkerThread checks every ~512 bytes of new
// output and ends the reply as an INTERRUPTED turn when this fires (the
// agent loop stops and no tool call from that reply is dispatched).
//
// This is a safeguard, not the primary limit: it only catches EXACT
// back-to-back repetition.  A loop with any variation (an incrementing
// counter, a changing word) is not detected.
//
// Two policies, because the cost of a false positive differs:
//
//   * Thinking -- reasoning text is never shown as the answer and has no
//     legitimate reason to repeat a block verbatim eight times.  Catches a
//     loop after ~1 KB.
//   * Output   -- visible answer text and native tool-call arguments.  A
//     user can ASK for repetitive content (identical CSV rows, test
//     fixtures, a file body with repeated blocks), so exact repetition has
//     to run for 32 KB before it counts.  That still ends a runaway long
//     before it fills a 131k-token window (~500 KB).
//
// Common rules (both policies):
//   * repeating block of 16..4096 bytes (a 701-byte paragraph loop escaped
//     the old 600-byte ceiling);
//   * at least 8 back-to-back copies, or 4 for blocks of 256+ bytes -- a
//     long block repeated four times verbatim is already a loop;
//   * the block must look like text: at least 6 distinct bytes and at least
//     one letter, where any non-ASCII byte counts as a letter so Chinese,
//     Japanese, Cyrillic etc. loops are caught (the old ASCII-letter rule
//     let them through).  A constant or tiny-unit run ("=====", "0, 0, 0",
//     "----") is periodic at every length, so without this it would match
//     at period 16 even though its real unit is 1-3 bytes.
//
// Pure std::string logic, unit-testable anywhere.

#pragma once

#include <algorithm>
#include <cstddef>
#include <string>

namespace repetition_guard {

struct Policy
{
    std::size_t minPeriod      = 16;
    std::size_t maxPeriod      = 4096;
    std::size_t minRepeats     = 8;     // copies needed for short blocks
    std::size_t longPeriodFrom = 256;   // blocks at least this long ...
    std::size_t minRepeatsLong = 4;     // ... need only this many copies
    std::size_t minSpan        = 1000;  // exactly-repeating tail, bytes
};

inline Policy ThinkingPolicy()
{
    return Policy{};
}

inline Policy OutputPolicy()
{
    Policy p;
    p.minSpan = 32 * 1024;
    return p;
}

// True when text[begin, end) has >= 6 distinct bytes and at least one
// letter.  Any byte >= 0x80 (part of a multi-byte UTF-8 character) counts as
// a letter.  UTF-8 continuation bytes count as distinct, which is fine.
inline bool LooksLikeText(const std::string& text, std::size_t begin, std::size_t end)
{
    bool seen[256] = {};
    int distinct = 0;
    bool letter = false;
    for (std::size_t i = begin; i < end; ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (!seen[c]) { seen[c] = true; ++distinct; }
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80)
            letter = true;
    }
    return distinct >= 6 && letter;
}

inline bool EndsInRepetitionLoop(const std::string& text, const Policy& pol)
{
    const std::size_t n = text.size();
    if (n < pol.minSpan) return false;

    for (std::size_t p = pol.minPeriod; p <= pol.maxPeriod; ++p) {
        const std::size_t repeats =
            p >= pol.longPeriodFrom ? pol.minRepeatsLong : pol.minRepeats;
        const std::size_t span = std::max(p * repeats, pol.minSpan);
        if (span > n) break;                 // longer periods need even more text

        // The last `span` bytes are periodic with period p when every byte
        // equals the byte p positions earlier.  Scan backwards: loops are
        // usually broken near the start of the window, so mismatches in
        // non-looping text are found within the first few comparisons.
        // (span >= 2p, so the lower bound n - span + p is >= p > 0.)
        bool periodic = true;
        for (std::size_t i = n - 1; i >= n - span + p; --i) {
            if (text[i] != text[i - p]) { periodic = false; break; }
        }
        if (!periodic) continue;
        if (LooksLikeText(text, n - p, n)) return true;

        // Periodic but not text-like ("=====", "0, 0, 0").  Every longer
        // period whose block sits inside this run is built from the same
        // bytes and fails the same test, so stop instead of re-scanning the
        // whole span for thousands of periods (~100M compares per check on
        // a long ruler line).  A real loop whose tail happens to be such a
        // run at this moment is caught by a later check, ~512 bytes on.
        return false;
    }
    return false;
}

// Back-compat overload: the thinking policy.
inline bool EndsInRepetitionLoop(const std::string& text)
{
    return EndsInRepetitionLoop(text, ThinkingPolicy());
}

} // namespace repetition_guard
