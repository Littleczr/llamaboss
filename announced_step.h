// announced_step.h
//
// Detects an agent reply that ANNOUNCES its next step but contains no tool
// call -- "Now step 9:" or "Step 2: writing n1.txt with "1"." -- which the
// loop would otherwise treat as the final answer and end the turn on.
//
// Observed with Qwen3.8-27B on multi-step checklists when it is asked to
// narrate between tool calls: the turn ended after an announcement line in
// 2 of 2 narrated runs, and 0 of 1 silent runs (14/14 steps completed).
// The metrics export confirmed the reply really had no call (tool_calls=0,
// finish_reason=stop) rather than a call the parser missed.
//
// Deliberately narrow, because a false positive costs one extra request
// and a false negative is today's behavior:
//   * short replies only (a real final answer is usually longer),
//   * the LAST sentence must read as forward-looking: it ends with ':' or
//     starts with "step N", "now", "next", "let me" (not "let me know"),
//     "let's", "i'll now" / "i will now", "moving on", "proceeding",
//     "continuing", or promises a retry ("retrying", "trying again",
//     "i'll retry", "i will retry", "will retry").  The retry forms came
//     from a Qwen3.8-27B transcript: "That call was malformed on my side.
//     Retrying once with the correct arguments." -- then no call.
//
// Reasoning is removed before any check (VisibleText): the parser hands the
// whole reply, <think> blocks included, to the final-answer path, so a stall
// after 2 KB of thinking used to fail the length test and end the turn.
//
// HasCallInsideLeakedReasoning covers a second Qwen3.8 failure: a complete
// <tool_call> written INSIDE reasoning that leaked into the reply (no <think>
// opener, but a stray </think> after the call).  The parser correctly refuses
// to run calls from reasoning, and the model then reported the never-run call
// as done ("The file size and line count are printed in the tool result
// above").  The loop nudges instead of executing: a quoted example costs one
// extra request; a real intent gets re-issued properly.
//
// Pure std C++, no wx / Poco.  Tested in announced_step_tests.cpp.

#pragma once

#include <cctype>
#include <string>

namespace lb_announced_step {

// Replies longer than this are never treated as an announcement.
constexpr size_t kMaxAnnouncementBytes = 600;

inline std::string LowerAscii(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline bool StartsWith(const std::string& s, const char* prefix)
{
    return s.compare(0, std::char_traits<char>::length(prefix), prefix) == 0;
}

// The last sentence of |text| (trailing whitespace and closing punctuation
// removed), lowercased.  Sentences end at ". ", "! ", "? " or a newline.
inline std::string LastSentenceLower(const std::string& text, bool* endsWithColon)
{
    size_t end = text.find_last_not_of(" \t\r\n");
    if (end == std::string::npos) { if (endsWithColon) *endsWithColon = false; return {}; }
    std::string s = text.substr(0, end + 1);

    // Markdown emphasis / code around the tail does not change the meaning.
    while (!s.empty() && (s.back() == '*' || s.back() == '_' || s.back() == '`'))
        s.pop_back();
    if (endsWithColon) *endsWithColon = !s.empty() && s.back() == ':';
    while (!s.empty() && (s.back() == '.' || s.back() == '!' || s.back() == ':' ||
                          s.back() == '"' || s.back() == '\''))
        s.pop_back();

    // Clause boundaries too: "That call was malformed on my end -- retrying
    // with the correct path." puts the promise after a dash, and "; " or an
    // em/en dash does the same (U+2014 / U+2013 are E2 80 94 / E2 80 93).
    size_t start = 0;
    for (size_t i = 0; i + 1 < s.size(); ++i) {
        const char c = s[i];
        if (c == '\n') start = i + 1;
        else if ((c == '.' || c == '!' || c == '?' || c == ';') &&
                 (s[i + 1] == ' ' || s[i + 1] == '\t'))
            start = i + 2;
        else if (c == ' ' && s[i + 1] == '-' && i + 3 < s.size() &&
                 s[i + 2] == '-' && s[i + 3] == ' ')
            start = i + 4;                                   // " -- "
        else if (c == ' ' && s[i + 1] == '-' && i + 2 < s.size() && s[i + 2] == ' ')
            start = i + 3;                                   // " - "
        else if (static_cast<unsigned char>(c) == 0xE2 && i + 2 < s.size() &&
                 static_cast<unsigned char>(s[i + 1]) == 0x80 &&
                 (static_cast<unsigned char>(s[i + 2]) == 0x94 ||
                  static_cast<unsigned char>(s[i + 2]) == 0x93))
            start = i + 3;                                   // em / en dash
    }
    std::string last = s.substr(start);
    const size_t first = last.find_first_not_of(" \t\r\n*_>-#");
    last = first == std::string::npos ? std::string() : last.substr(first);
    return LowerAscii(last);
}

// The reply with reasoning removed: wrapped <think>...</think> blocks, an
// unterminated <think> to the end, and everything up to the last orphan
// </think> (leaked reasoning has no opener).
inline std::string VisibleText(const std::string& text)
{
    static const std::string kOpen = "<think>", kClose = "</think>";
    std::string out = text;
    for (;;) {
        const size_t open = out.find(kOpen);
        if (open == std::string::npos) break;
        const size_t close = out.find(kClose, open + kOpen.size());
        if (close == std::string::npos) { out.erase(open); break; }
        out.erase(open, close + kClose.size() - open);
    }
    const size_t stray = out.rfind(kClose);
    if (stray != std::string::npos) out.erase(0, stray + kClose.size());
    return out;
}

// A complete tool-call block (<tool_call> ... </tool_call> with a <name>)
// that sits before an orphan </think>, i.e. inside leaked reasoning.
inline bool HasCallInsideLeakedReasoning(const std::string& text)
{
    static const std::string kClose = "</think>";
    // Wrapped blocks are real reasoning the model meant to hide; only an
    // orphan closer (no <think> before it) marks leaked reasoning.
    const size_t stray = text.rfind(kClose);
    if (stray == std::string::npos) return false;
    const size_t opener = text.rfind("<think>", stray);
    if (opener != std::string::npos) return false;
    const std::string reasoning = text.substr(0, stray);
    const size_t open = reasoning.find("<tool_call>");
    if (open == std::string::npos) return false;
    const size_t name = reasoning.find("<name>", open);
    const size_t close = reasoning.find("</tool_call>", open);
    return name != std::string::npos && close != std::string::npos && name < close;
}

inline bool LooksLikeAnnouncedNextStep(const std::string& rawReply)
{
    const std::string prose = VisibleText(rawReply);
    const size_t first = prose.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return false;
    const size_t last = prose.find_last_not_of(" \t\r\n");
    if (last - first + 1 > kMaxAnnouncementBytes) return false;

    bool endsWithColon = false;
    const std::string s = LastSentenceLower(prose, &endsWithColon);
    if (s.empty()) return false;

    // A trailing colon promises content that never came.  Exclude the one
    // common case where a colon ends a question to the user.
    if (endsWithColon && s.find('?') == std::string::npos) return true;

    if (StartsWith(s, "step ") && s.size() > 5 &&
        std::isdigit(static_cast<unsigned char>(s[5])))
        return true;
    if (StartsWith(s, "let me ") && !StartsWith(s, "let me know"))
        return true;
    static const char* const kCues[] = {
        "now ", "now,", "next ", "next,", "i'll now", "i will now",
        "moving on", "proceeding", "continuing",
        "let's ", "retrying", "trying again", "i'll retry", "i will retry",
        "will retry",
    };
    for (const char* cue : kCues)
        if (StartsWith(s, cue)) return true;
    return false;
}

} // namespace lb_announced_step
