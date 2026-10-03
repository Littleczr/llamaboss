// bench_stats.h
//
// Pure pieces of /bench: argument parsing, the fixed benchmark prompt, and
// the summary math.  No wx / Poco, so it is unit-testable anywhere.
// BenchController (bench_controller.h) does the I/O.
//
// What a run measures (same as llama-bench's pp / tg, as closely as a chat
// endpoint allows):
//   * pp -- prompt processing: a fixed ~512-token (or ~4k with `long`) prompt.
//   * tg -- generation: 128 tokens.  On a local llama-server we pass
//           ignore_eos + max_tokens so every run generates exactly 128;
//           remote models stop when they choose, so their tg varies.
//
// Cache state matters more than anything else for pp and first-token time,
// so every run is labelled cold / warm from what the server reported:
//   * local: cold when llama-server reported reusing 0 prompt tokens from its
//            KV cache (timings.cache_n); "?" when cache_n is not reported.
//            Run 1 is always forced cold (cache_prompt:false); `cold` forces
//            every run cold.
//   * remote: warm when the provider reported cached prompt tokens, else
//            "?" -- providers don't let us force or always report it.

#pragma once

#include "turn_stats.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace bench {

struct Options
{
    int  runs       = 5;      // 1..50
    bool allCold    = false;  // `cold`: disable prompt cache on every run (local)
    bool longPrompt = false;  // `long`: ~4k-token prompt instead of ~512
    int  genTokens  = 128;
};

// "/bench" args: any order of  <runs>  cold  long  stop  help
// Returns false with `error` set (usage text) on anything else.
// `stop` / `help` are reported through the out flags.
inline bool ParseArgs(const std::string& args, Options& out, bool& stop,
                      bool& help, std::string& error)
{
    out = Options{};
    stop = help = false;
    std::string tok;
    auto flush = [&]() -> bool {
        if (tok.empty()) return true;
        std::string t = tok;
        tok.clear();
        for (char& c : t) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        if (t == "cold") { out.allCold = true; return true; }
        if (t == "long") { out.longPrompt = true; return true; }
        if (t == "stop") { stop = true; return true; }
        if (t == "help" || t == "?") { help = true; return true; }
        bool digits = !t.empty() && t.size() <= 3;
        for (char c : t) if (c < '0' || c > '9') digits = false;
        if (digits) {
            const int n = std::stoi(t);
            if (n >= 1 && n <= 50) { out.runs = n; return true; }
            error = "Runs must be between 1 and 50.";
            return false;
        }
        error = "Unknown option \"" + t + "\".";
        return false;
    };
    for (char c : args) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!flush()) return false;
        } else {
            tok.push_back(c);
        }
    }
    return flush();
}

inline const char* Usage()
{
    return "Usage: /bench [runs] [cold] [long]   \xC2\xB7   /bench stop\n"
           "  runs  number of runs, 1-50 (default 5)\n"
           "  cold  disable the prompt cache on every run (local models)\n"
           "  long  ~4k-token prompt instead of ~512 (stresses prompt processing)\n"
           "Each run sends a fixed prompt and generates 128 tokens. Run 1 is "
           "always cold on local models. Results go to Shared\\Benchmarks.";
}

// Deterministic filler: numbered lines of plain prose.  ~17 tokens per line
// with common BPE tokenizers, so 30 lines ~ 540 tokens and 240 ~ 4.1k.
// The final instruction asks for open-ended prose so generation never
// runs out of things to say before the 128-token cap.
inline std::string BuildPrompt(bool longPrompt)
{
    static const char* kLines[] = {
        "The river bends twice before it reaches the old stone bridge.",
        "A lighthouse keeper writes the weather in a leather notebook.",
        "Farmers bring apples, pears and honey to the Saturday market.",
        "The train leaves the valley at dawn and returns after dark.",
        "Children fly paper kites above the harbor on windy afternoons.",
        "A small bakery on the corner sells bread until noon each day.",
        "The museum keeps a map of the coast drawn two centuries ago.",
        "Snow closes the mountain pass for most of the winter months.",
    };
    const int lines = longPrompt ? 240 : 30;
    std::string p = "Reference notes for a speed test (read them, no need to repeat them):\n";
    for (int i = 0; i < lines; ++i) {
        p += std::to_string(i + 1) + ". ";
        p += kLines[i % (sizeof(kLines) / sizeof(kLines[0]))];
        p += '\n';
    }
    p += "\nNow write a long, detailed story about the lighthouse keeper "
         "(about 150 words). Plain prose only, no lists or headings.";
    return p;
}

// Prompt-processing speed only when enough tokens were actually processed
// to mean something.  A warm run re-processes ~1 token, and "1 token in
// 25 ms = 40 tok/s" is noise, not a speed.
inline double MeaningfulPromptRate(const TurnStats& t)
{
    if (t.hasServerTimings && t.serverPromptN >= 0 && t.serverPromptN < 64) return -1;
    return t.PromptTokensPerSec();
}

enum class CacheState { Cold, Warm, Unknown };

// Only a REPORTED count classifies a run.  serverCacheN / cachedPromptTokens
// are -1 when the field was absent (older llama-server builds, proxies that
// strip timings, providers without cache detail), and a missing field says
// nothing about the cache -- labelling it "cold" would put warm runs in the
// cold medians.  llama-server's cache_n wins when reported; otherwise fall
// back to the usage detail; otherwise Unknown.
inline CacheState ClassifyCache(const TurnStats& t)
{
    if (t.hasServerTimings && t.serverCacheN >= 0)
        return t.serverCacheN > 0 ? CacheState::Warm : CacheState::Cold;
    if (t.cachedPromptTokens > 0) return CacheState::Warm;
    if (t.cachedPromptTokens == 0) return CacheState::Cold;
    return CacheState::Unknown;
}

inline const char* CacheLabel(CacheState c)
{
    return c == CacheState::Cold ? "cold" : c == CacheState::Warm ? "warm" : "?";
}

struct Run
{
    int        index = 0;     // 1-based
    TurnStats  stats;
    CacheState cache = CacheState::Unknown;
};

inline double Median(std::vector<double> v)
{
    v.erase(std::remove_if(v.begin(), v.end(), [](double x) { return x <= 0; }), v.end());
    if (v.empty()) return -1;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

// One progress line per run, e.g.
//   "run 2/5 · warm · pp 523 tok · tg 128 tok @ 64.9 tok/s · first token 0.08 s"
inline std::string RunLine(const Run& r, int total)
{
    const TurnStats& t = r.stats;
    const char* dot = " \xC2\xB7 ";
    char b[96];
    std::string s = "run " + std::to_string(r.index) + "/" + std::to_string(total) +
                    dot + CacheLabel(r.cache);
    if (t.promptTokens >= 0) s += dot + ("pp " + std::to_string(t.promptTokens) + " tok");
    const double pp = MeaningfulPromptRate(t);
    if (pp > 0) { std::snprintf(b, sizeof(b), " @ %.0f tok/s", pp); s += b; }
    if (t.completionTokens >= 0) s += dot + ("tg " + std::to_string(t.completionTokens) + " tok");
    const double tg = t.GenerationTokensPerSec();
    if (tg > 0) { std::snprintf(b, sizeof(b), " @ %.1f tok/s", tg); s += b; }
    if (t.firstTokenMs >= 0) s += dot + ("first token " + TurnStats::FormatSeconds(t.firstTokenMs));
    return s;
}

// Multi-line summary: medians split by cache state.
inline std::string Summary(const std::vector<Run>& runs, const std::string& model,
                           bool remote, const std::string& tsvPath)
{
    char b[160];
    std::string s = "Benchmark \xC2\xB7 " + model + (remote ? " (remote)" : " (local)") +
                    " \xC2\xB7 " + std::to_string(runs.size()) +
                    (runs.size() == 1 ? " run\n" : " runs\n");

    auto block = [&](CacheState c, const char* name) {
        std::vector<double> tg, pp, ft;
        for (const Run& r : runs) {
            if (r.cache != c) continue;
            tg.push_back(r.stats.GenerationTokensPerSec());
            pp.push_back(MeaningfulPromptRate(r.stats));
            ft.push_back(r.stats.firstTokenMs);
        }
        if (tg.empty()) return;
        std::snprintf(b, sizeof(b), "  %-5s (%zu):", name, tg.size());
        s += b;
        const double mtg = Median(tg), mpp = Median(pp), mft = Median(ft);
        if (mtg > 0) { std::snprintf(b, sizeof(b), "  tg %.1f tok/s", mtg); s += b; }
        if (mpp > 0) { std::snprintf(b, sizeof(b), "  \xC2\xB7  pp %.0f tok/s", mpp); s += b; }
        if (mft > 0) s += "  \xC2\xB7  first token " + TurnStats::FormatSeconds(mft);
        s += "\n";
    };
    block(CacheState::Cold, "cold");
    block(CacheState::Warm, "warm");
    block(CacheState::Unknown, "cache?");
    s += "  (medians";
    if (remote) s += "; remote speeds include network and provider time";
    s += ")\n";
    if (!tsvPath.empty()) s += "Saved: " + tsvPath;
    return s;
}

inline const char* TsvHeader()
{
    static const std::string h = std::string("run\tcache\tprompt_size\t") + TurnStats::TsvHeader();
    return h.c_str();
}

inline std::string TsvRow(const Run& r, bool longPrompt, const std::string& time,
                          const std::string& model)
{
    return std::to_string(r.index) + '\t' + CacheLabel(r.cache) + '\t' +
           (longPrompt ? "long" : "short") + '\t' + r.stats.TsvRow(time, model);
}

} // namespace bench
