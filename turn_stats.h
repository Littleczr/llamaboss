// turn_stats.h
//
// Per-turn performance numbers for one streamed model reply: token counts,
// timings and the speeds derived from them.  Filled by ChatWorkerThread
// (chat_client.cpp) while it reads the stream, carried to the UI on
// AssistantCompletePayload, and consumed by the context meter tooltip,
// the per-chat turn_stats.tsv log and (later) the context HUD / /bench.
//
// Two sources, preferred in this order:
//   1. Server-reported timings.  llama-server adds a "timings" object to
//      its final streamed chunk: prompt_n / prompt_ms / prompt_per_second
//      (prompt processing) and predicted_n / predicted_ms /
//      predicted_per_second (generation), plus cache_n.  These are measured
//      inside the server and exclude network time -- the right numbers for
//      benchmarking a local model.
//   2. Client wall clock.  Measured here for every lane (remote providers
//      never send timings).  Includes network and provider queue time, so
//      remote speeds are "what you experienced", not a model property.
//
// Pure data + formatting, no wx / Poco, so it is unit-testable anywhere.

#pragma once

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

struct TurnStats
{
    // Identity captured by the worker from this request's resolved target.
    // Never substitute the currently selected model for a historical turn.
    std::string modelId;
    int modelRemote = -1; // -1 unknown (older logs), 0 local, 1 remote

    // ── Client wall clock, milliseconds from the request being sent ──
    // -1 = never happened (e.g. no token before the stream ended).
    double firstByteMs    = -1;  // first SSE data event of any kind
    double firstTokenMs   = -1;  // first reasoning / content / tool-call delta
    double firstContentMs = -1;  // first visible content or tool-call delta
    double lastTokenMs    = -1;  // last delta of any kind
    double totalMs        = -1;  // terminal event (reply complete)

    // ── Usage reported by the server/provider (-1 = not reported) ──
    long promptTokens       = -1;
    long completionTokens   = -1;  // includes reasoning tokens when reported
    long cachedPromptTokens = -1;  // prompt tokens served from the provider's cache
    long reasoningTokens    = -1;  // hidden/visible thinking tokens (OpenAI-style detail)

    // ── llama-server "timings" (only local llama-server lanes) ──
    bool   hasServerTimings     = false;
    long   serverPromptN        = -1;   // prompt tokens actually processed (cache misses)
    long   serverCacheN         = -1;   // prompt tokens reused from the KV cache
    long   serverPredictedN     = -1;   // tokens generated
    double serverPromptMs       = -1;
    double serverPredictedMs    = -1;
    double serverPromptPerSec   = -1;
    double serverPredictedPerSec = -1;

    // The reply was cut off by LlamaBoss because the model was repeating
    // the same block of text (repetition_guard.h), not ended by the model.
    bool stoppedForRepetition = false;

    // Generation speed read back from turn_stats.tsv (FromTsvRow) for a
    // remote reply, whose speed is derived from per-token times the log
    // doesn't keep.  -1 for live replies.
    double loggedGenTokPerSec = -1;

    bool empty() const
    {
        return totalMs < 0 && promptTokens < 0 && !hasServerTimings;
    }

    // Generation speed, tokens per second.  -1 when it can't be computed
    // honestly.
    //  * llama-server: its own predicted_per_second.
    //  * Remote with reasoning tokens reported: hidden reasoning happens
    //    before the first visible token, so counting those tokens against
    //    visible streaming time would overstate the speed.  Use visible
    //    output only: (completion - reasoning) over first content -> last.
    //  * Otherwise: completion tokens over first token -> last token.
    double GenerationTokensPerSec() const
    {
        if (hasServerTimings && serverPredictedPerSec > 0)
            return serverPredictedPerSec;
        if (lastTokenMs < 0 && loggedGenTokPerSec > 0)
            return loggedGenTokPerSec;
        if (completionTokens <= 0 || lastTokenMs < 0) return -1;

        long tokens = completionTokens;
        double startMs = firstTokenMs;
        if (reasoningTokens > 0) {
            tokens = completionTokens - reasoningTokens;
            startMs = firstContentMs;
        }
        if (tokens <= 1 || startMs < 0) return -1;
        const double windowMs = lastTokenMs - startMs;
        if (windowMs < 50) return -1;   // too short to mean anything
        // N tokens arrive over N-1 gaps after the first one.
        return (double)(tokens - 1) * 1000.0 / windowMs;
    }

    // Prompt processing speed, tokens per second.  Only meaningful with
    // server timings: a remote first-token time mixes network, queueing,
    // hidden reasoning and prefill, so it is reported as a time, not a rate.
    double PromptTokensPerSec() const
    {
        if (hasServerTimings && serverPromptPerSec > 0)
            return serverPromptPerSec;
        return -1;
    }

    // Compact tokens: 612, 17.2k, 1.35M.
    static std::string FormatCount(long n)
    {
        char buf[32];
        if (n < 0) return "?";
        if (n < 1000) std::snprintf(buf, sizeof(buf), "%ld", n);
        else if (n < 1000000) std::snprintf(buf, sizeof(buf), "%.1fk", n / 1000.0);
        else std::snprintf(buf, sizeof(buf), "%.2fM", n / 1000000.0);
        return buf;
    }

    static std::string FormatSeconds(double ms)
    {
        char buf[32];
        if (ms < 0) return "?";
        std::snprintf(buf, sizeof(buf), ms < 10000 ? "%.2f s" : "%.1f s", ms / 1000.0);
        return buf;
    }

    // One line for tooltips / status:
    //   "78.4 tok/s · first token 0.84 s · 8.65 s total · 612 out / 17.2k in (12.3k cached)"
    // Separator is U+00B7 (UTF-8).
    std::string OneLineSummary() const
    {
        const char* dot = " \xC2\xB7 ";
        std::string s;
        char buf[64];

        const double gen = GenerationTokensPerSec();
        if (gen > 0) {
            std::snprintf(buf, sizeof(buf), "%.1f tok/s", gen);
            s += buf;
        }
        const double pp = PromptTokensPerSec();
        if (pp > 0) {
            std::snprintf(buf, sizeof(buf), "prompt %.0f tok/s", pp);
            if (!s.empty()) s += dot;
            s += buf;
        }
        if (firstTokenMs >= 0) {
            if (!s.empty()) s += dot;
            s += "first token " + FormatSeconds(firstTokenMs);
        }
        if (totalMs >= 0) {
            if (!s.empty()) s += dot;
            s += FormatSeconds(totalMs) + " total";
        }
        if (completionTokens >= 0 || promptTokens >= 0) {
            if (!s.empty()) s += dot;
            s += FormatCount(completionTokens) + " out / " + FormatCount(promptTokens) + " in";
            if (cachedPromptTokens > 0)
                s += " (" + FormatCount(cachedPromptTokens) + " cached)";
            else if (hasServerTimings && serverCacheN > 0)
                s += " (" + FormatCount(serverCacheN) + " cached)";
            if (reasoningTokens > 0)
                s += dot + FormatCount(reasoningTokens) + " reasoning";
        }
        return s;
    }

    // TSV row for turn_stats.tsv; keep in step with TsvHeader().
    static const char* TsvHeader()
    {
        return "time\tmodel\tprompt_tokens\tcompletion_tokens\tcached_tokens"
               "\treasoning_tokens\tfirst_byte_ms\tfirst_token_ms\ttotal_ms"
               "\tgen_tok_s\tprompt_tok_s\tserver_timings"
               "\tserver_prompt_n\tserver_cache_n\tserver_predicted_n"
               "\tserver_prompt_ms\tserver_predicted_ms\tmodel_remote";
    }

    std::string TsvRow(const std::string& time, const std::string& model) const
    {
        auto num = [](double v, const char* fmt) {
            if (v < 0) return std::string();
            char b[32];
            std::snprintf(b, sizeof(b), fmt, v);
            return std::string(b);
        };
        auto cnt = [](long v) { return v < 0 ? std::string() : std::to_string(v); };

        std::string m = modelId.empty() ? model : modelId;
        for (char& c : m) if (c == '\t' || c == '\n' || c == '\r') c = ' ';

        return time + '\t' + m + '\t' +
               cnt(promptTokens) + '\t' + cnt(completionTokens) + '\t' +
               cnt(cachedPromptTokens) + '\t' + cnt(reasoningTokens) + '\t' +
               num(firstByteMs, "%.0f") + '\t' + num(firstTokenMs, "%.0f") + '\t' +
               num(totalMs, "%.0f") + '\t' +
               num(GenerationTokensPerSec(), "%.2f") + '\t' +
               num(PromptTokensPerSec(), "%.2f") + '\t' +
               (hasServerTimings ? "1" : "0") + '\t' +
               cnt(serverPromptN) + '\t' + cnt(serverCacheN) + '\t' +
               cnt(serverPredictedN) + '\t' +
               num(serverPromptMs, "%.1f") + '\t' + num(serverPredictedMs, "%.1f") + '\t' +
               (modelRemote == 0 ? "0" : modelRemote == 1 ? "1" : "");
    }

    // ── Reading turn_stats.tsv back (HUD after loading an old chat) ──
    // Columns are looked up by header name, so rows written by an older
    // or newer build still parse (unknown columns ignored, missing ones
    // stay -1).  Speeds come back exactly as logged: llama-server rows
    // restore predicted/prompt per-second as the server reported them;
    // remote rows keep the logged gen_tok_s (loggedGenTokPerSec).
    static std::vector<std::string> SplitTabs(const std::string& line)
    {
        std::vector<std::string> out(1);
        for (char c : line) {
            if (c == '\t') out.emplace_back();
            else if (c != '\r' && c != '\n') out.back().push_back(c);
        }
        return out;
    }

    static bool FromTsvRow(const std::vector<std::string>& header,
                           const std::string& line, TurnStats& out)
    {
        out = TurnStats{};
        const std::vector<std::string> v = SplitTabs(line);
        if (v.size() < 3) return false;
        auto col = [&](const char* name) -> const std::string* {
            for (size_t i = 0; i < header.size() && i < v.size(); ++i)
                if (header[i] == name) return v[i].empty() ? nullptr : &v[i];
            return nullptr;
        };
        auto L = [&](const char* name) -> long {
            const std::string* s = col(name);
            if (!s) return -1;
            char* end = nullptr;
            const long n = std::strtol(s->c_str(), &end, 10);
            return (end && *end == '\0') ? n : -1;
        };
        auto D = [&](const char* name) -> double {
            const std::string* s = col(name);
            if (!s) return -1;
            char* end = nullptr;
            const double d = std::strtod(s->c_str(), &end);
            return (end && *end == '\0') ? d : -1;
        };
        if (const std::string* model = col("model")) out.modelId = *model;
        if (const std::string* remote = col("model_remote")) {
            if (*remote == "0") out.modelRemote = 0;
            else if (*remote == "1") out.modelRemote = 1;
        }
        out.promptTokens       = L("prompt_tokens");
        out.completionTokens   = L("completion_tokens");
        out.cachedPromptTokens = L("cached_tokens");
        out.reasoningTokens    = L("reasoning_tokens");
        out.firstByteMs        = D("first_byte_ms");
        out.firstTokenMs       = D("first_token_ms");
        out.totalMs            = D("total_ms");
        const std::string* st  = col("server_timings");
        out.hasServerTimings   = st && *st == "1";
        if (out.hasServerTimings) {
            out.serverPromptN         = L("server_prompt_n");
            out.serverCacheN          = L("server_cache_n");
            out.serverPredictedN      = L("server_predicted_n");
            out.serverPromptMs        = D("server_prompt_ms");
            out.serverPredictedMs     = D("server_predicted_ms");
            out.serverPredictedPerSec = D("gen_tok_s");
            out.serverPromptPerSec    = D("prompt_tok_s");
        } else {
            out.loggedGenTokPerSec    = D("gen_tok_s");
        }
        return !out.empty();
    }

    // Whole file -> rows, oldest first, keeping at most the newest
    // |maxRows|.  A file without the expected header yields nothing.
    static std::vector<TurnStats> ParseTsv(const std::string& content, size_t maxRows)
    {
        std::vector<TurnStats> rows;
        std::vector<std::string> header;
        size_t pos = 0;
        while (pos < content.size()) {
            size_t nl = content.find('\n', pos);
            if (nl == std::string::npos) nl = content.size();
            const std::string line = content.substr(pos, nl - pos);
            pos = nl + 1;
            if (line.empty() || line == "\r") continue;
            if (header.empty()) {
                header = SplitTabs(line);
                if (header.empty() || header[0] != "time") return {};
                continue;
            }
            if (line.compare(0, 5, "time\t") == 0) {   // header repeated (appended by another build)
                header = SplitTabs(line);
                continue;
            }
            TurnStats t;
            if (FromTsvRow(header, line, t)) rows.push_back(t);
        }
        if (maxRows && rows.size() > maxRows)
            rows.erase(rows.begin(), rows.end() - static_cast<std::ptrdiff_t>(maxRows));
        return rows;
    }
};
