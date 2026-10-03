// prompt_prewarm.h
//
// Pure logic for pre-warming llama-server's prompt cache on New Chat.
// No wx, no Poco, no I/O -- tested standalone by prompt_prewarm_tests.cpp.
//
// Why: the agent system prompt is ~13k tokens and byte-identical across
// chats up to its "WORKING CONTEXT" section (agent_prompt_builder.cpp).
// On hybrid/recurrent models (Qwen3.8) llama-server cannot reuse a
// PARTIAL prefix of what the slot holds -- it can only extend the slot's
// whole cached sequence -- so a new chat reprocesses the full prompt
// (~4-8 s before the first token).  If the slot holds exactly the stable
// prefix when the first real request arrives, that request extends it and
// only the per-chat tail + the user message are processed.
//
// How (ServerManager does the I/O on its serialized slot-action worker):
//   1. POST /apply-template with the SAME body the first real request would
//      send (same system prompt, tools, template kwargs) plus a throwaway
//      user message.  The server renders it with the model's own template,
//      so the primed text matches the real prompt byte-for-byte.
//   2. CutStablePrefix(): keep everything before the per-chat section.
//   3. POST /completion {prompt: prefix, n_predict: 1, cache_prompt: true}.
//      The one sampled token is never decoded into the KV (llama-server
//      stops at the limit before the next batch), so the slot ends holding
//      exactly the prefix tokens.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace prompt_prewarm {

// Priming less than this is not worth a request (and means the marker was
// found somewhere unexpected).
constexpr size_t kMinPrefixBytes = 2048;

enum class Cut { Ok, MarkerMissing, MarkerRepeated, TooShort };

inline const char* CutName(Cut c)
{
    switch (c) {
        case Cut::Ok:             return "ok";
        case Cut::MarkerMissing:  return "working-context heading not found in rendered prompt";
        case Cut::MarkerRepeated: return "working-context heading appears more than once";
        case Cut::TooShort:       return "stable prefix too short to be worth priming";
    }
    return "?";
}

// The per-chat section starts with "\n\n" + heading.  The cut goes AFTER
// the two newlines, not before them: BPE pre-tokenizers (Qwen, Llama 3)
// glue trailing newlines onto preceding punctuation (".\n\n" is one
// pre-token), so cutting before them would tokenize the prefix differently
// from the full prompt and the cache would miss on its last token -- which
// on a recurrent model means a full reprocess.  After the newlines the
// next character is a letter, which always starts a new pre-token.
inline Cut CutStablePrefix(const std::string& rendered,
                           const std::string& heading,
                           std::string& prefix)
{
    prefix.clear();
    if (heading.empty()) return Cut::MarkerMissing;
    const std::string needle = "\n\n" + heading;
    const size_t pos = rendered.find(needle);
    if (pos == std::string::npos) return Cut::MarkerMissing;
    if (rendered.find(needle, pos + 1) != std::string::npos) return Cut::MarkerRepeated;
    const size_t cut = pos + 2;
    if (cut < kMinPrefixBytes) return Cut::TooShort;
    prefix = rendered.substr(0, cut);
    return Cut::Ok;
}

// The part of a system prompt that is shared across chats (everything
// before the per-chat section), or the whole prompt if it has none.  Used
// for the dedupe key, so re-clicking New Chat doesn't re-prime a slot that
// already holds this prefix (on a recurrent model an exact-match prompt is
// NOT free: llama-server must re-evaluate the last token, which it can't
// roll back without a checkpoint, and may reprocess everything).
inline std::string StablePart(const std::string& systemPrompt,
                              const std::string& heading)
{
    const size_t pos = systemPrompt.find("\n\n" + heading);
    return pos == std::string::npos ? systemPrompt : systemPrompt.substr(0, pos);
}

inline uint64_t Fnv1a64(const std::string& s, uint64_t h = 1469598103934665603ULL)
{
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    return h;
}

// Identity of "what would be primed": model, protocol, think mode, the
// stable system text and the tool catalog (the template may render tools
// before the system text on some models).
inline std::string MakeKey(const std::string& model, const std::string& protocol,
                           const std::string& think, const std::string& stableSystem,
                           const std::string& toolsJson)
{
    uint64_t h = Fnv1a64(model);
    const std::string parts[] = { protocol, think, stableSystem, toolsJson };
    for (const auto& p : parts) { h = Fnv1a64("\x1f", h); h = Fnv1a64(p, h); }
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

// JSON string escaping for the /completion body (the prompt holds quotes,
// backslashes, newlines and possibly control characters from tool docs).
inline void AppendJsonString(std::string& out, const std::string& s)
{
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (c < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));   // UTF-8 passes through
                }
        }
    }
    out.push_back('"');
}

// n_predict 1 rather than 0: 0 is interpreted inconsistently across
// llama-server versions; 1 reliably stops after sampling without decoding
// the token.  temperature 0 keeps the throwaway sample deterministic.
inline std::string BuildCompletionBody(const std::string& prefix)
{
    std::string body;
    body.reserve(prefix.size() + 128);
    body += "{\"prompt\":";
    AppendJsonString(body, prefix);
    body += ",\"n_predict\":1,\"cache_prompt\":true,\"stream\":false,"
            "\"temperature\":0}";
    return body;
}

// Result of one prime, for the log line.
struct Outcome {
    bool        ok = false;
    bool        skipped = false;  // deliberately not run (superseded), not a failure
    std::string error;          // why it didn't run / failed
    long long   promptTokens = -1;   // tokens_evaluated: size of the primed prefix
    long long   cachedTokens = -1;   // timings.cache_n: already in the slot
    long long   processed    = -1;   // timings.prompt_n
    double      promptMs     = -1;   // timings.prompt_ms
    double      wallMs       = -1;   // both requests, client side
    size_t      prefixBytes  = 0;
};

inline std::string WithCommas(long long v)
{
    std::string s = std::to_string(v < 0 ? -v : v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), ",");
    return v < 0 ? "-" + s : s;
}

inline std::string Describe(const Outcome& o)
{
    if (!o.ok) return std::string(o.skipped ? "skipped: " : "failed: ") +
                      (o.error.empty() ? std::string("unknown error") : o.error);
    std::string s = "primed ";
    s += o.promptTokens >= 0 ? WithCommas(o.promptTokens) + " tokens" : std::string("? tokens");
    if (o.processed >= 0) {
        s += " (processed " + WithCommas(o.processed);
        if (o.cachedTokens > 0) s += ", reused " + WithCommas(o.cachedTokens);
        if (o.promptMs >= 0) {
            char buf[32];
            std::snprintf(buf, sizeof buf, " in %.1f s", o.promptMs / 1000.0);
            s += buf;
        }
        s += ")";
    }
    if (o.wallMs >= 0) {
        char buf[48];
        std::snprintf(buf, sizeof buf, ", %.1f s total", o.wallMs / 1000.0);
        s += buf;
    }
    return s;
}

} // namespace prompt_prewarm
