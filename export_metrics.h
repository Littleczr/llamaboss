// export_metrics.h
//
// "Export with metrics...": the Markdown section appended to an exported
// conversation, built from the chat folder's two per-request logs:
//
//   turn_stats.tsv       one row per completed reply (turn_stats.h): tokens,
//                        cached / reasoning tokens, first-token and total time;
//                        from the 2026-10-11 build also cache writes, cost,
//                        finish_reason, think/reply bytes and tool-call count;
//                        from the 2026-10-25 build also stopped_by
//                        ("repetition" when LlamaBoss cut a looping reply).
//   ctx_calibration.tsv  one row per reply with an exact usage report: wire
//                        request bytes, prompt tokens, bytes/token, elided
//                        count and what the request was made of -- system
//                        prompt, tool definitions, messages by role, images,
//                        replayed reasoning; later builds add prefix_hash (a
//                        fingerprint of system prompt + tools, the bytes every
//                        provider cache keys on) and cache_control (Anthropic
//                        caching requested).
//
// Both logs are written from the same completion handler, in the same order,
// so rows are paired by order + equal prompt_tokens (a short look-ahead skips
// a row present in only one log).  Columns are looked up by header name and a
// repeated "time\t..." header switches schema mid-file, so logs written by
// older builds still read; missing columns print as "-".
//
// Pure std C++: no wx / Poco.  Tested in export_metrics_tests.cpp.

#pragma once

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace lb_export_metrics {

using Row = std::map<std::string, std::string>;

// Whole TSV -> rows keyed by header name.  First line must start with
// "time\t"; a later line starting with "time\t" replaces the header.
inline std::vector<Row> ParseTsv(const std::string& content)
{
    auto split = [](const std::string& line) {
        std::vector<std::string> out(1);
        for (char c : line) {
            if (c == '\t') out.emplace_back();
            else if (c != '\r' && c != '\n') out.back().push_back(c);
        }
        return out;
    };
    std::vector<Row> rows;
    std::vector<std::string> header;
    size_t pos = 0;
    while (pos < content.size()) {
        size_t nl = content.find('\n', pos);
        if (nl == std::string::npos) nl = content.size();
        std::string line = content.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line.compare(0, 5, "time\t") == 0) { header = split(line); continue; }
        if (header.empty()) return {};          // not a log we know
        const auto v = split(line);
        Row r;
        for (size_t i = 0; i < header.size() && i < v.size(); ++i)
            if (!v[i].empty()) r[header[i]] = v[i];
        if (!r.empty()) rows.push_back(std::move(r));
    }
    return rows;
}

// One request as the export reports it.  -1 = not logged.
struct Request {
    std::string time, model;
    long long prompt = -1, cached = -1, completion = -1, reasoning = -1;
    double firstTokenMs = -1, totalMs = -1;
    long long reqBytes = -1, elided = -1, argsElided = -1;
    long long sysBytes = -1, toolsBytes = -1, userBytes = -1, asstBytes = -1,
              resultBytes = -1, imageBytes = -1, replayBytes = -1;
    // Newer columns (blank in older logs).
    long long cacheWrite = -1, thinkBytes = -1, replyBytes = -1, toolCalls = -1;
    double cost = -1;
    std::string finish, prefix;
    std::string stoppedBy;   // turn_stats stopped_by: "repetition" or empty
    int cacheCtl = -1;   // -1 not logged, 0 off, 1 Anthropic cache_control sent

    bool HasBreakdown() const { return sysBytes >= 0; }
    bool StoppedForRepetition() const { return stoppedBy == "repetition"; }
    // What the Finish column / tally shows: the provider's finish_reason,
    // or LlamaBoss's own stop when it cut the reply before one arrived.
    std::string FinishLabel() const
    {
        if (StoppedForRepetition()) return "repetition stop";
        return finish;
    }
    double Bpt() const { return (reqBytes > 0 && prompt > 0) ? (double)reqBytes / (double)prompt : -1; }
};

inline long long L(const Row& r, const char* k)
{
    auto it = r.find(k);
    if (it == r.end()) return -1;
    char* end = nullptr;
    const long long v = std::strtoll(it->second.c_str(), &end, 10);
    return (end && *end == '\0' && v >= 0) ? v : -1;
}
inline double D(const Row& r, const char* k)
{
    auto it = r.find(k);
    if (it == r.end()) return -1;
    char* end = nullptr;
    const double v = std::strtod(it->second.c_str(), &end);
    return (end && *end == '\0' && v >= 0) ? v : -1;
}
inline std::string S(const Row& r, const char* k)
{
    auto it = r.find(k);
    return it == r.end() ? std::string() : it->second;
}

inline void FillFromCalibration(Request& q, const Row& c)
{
    if (q.time.empty())  q.time  = S(c, "time");
    if (q.model.empty()) q.model = S(c, "model");
    if (q.prompt < 0)    q.prompt = L(c, "prompt_tokens");
    q.reqBytes    = L(c, "req_bytes");
    q.elided      = L(c, "elided");
    q.argsElided  = L(c, "args_elided");
    q.sysBytes    = L(c, "system_bytes");
    q.toolsBytes  = L(c, "tools_bytes");
    q.userBytes   = L(c, "user_bytes");
    q.asstBytes   = L(c, "assistant_bytes");
    q.resultBytes = L(c, "tool_result_bytes");
    q.imageBytes  = L(c, "image_bytes");
    q.replayBytes = L(c, "reasoning_replay_bytes");
    q.prefix      = S(c, "prefix_hash");
    const std::string cc = S(c, "cache_control");
    if (cc == "1") q.cacheCtl = 1; else if (cc == "0") q.cacheCtl = 0;
}

// Pair the two logs.  *unpairedCalibration counts calibration rows that
// matched no reply row (they are still reported, as their own requests).
inline std::vector<Request> MergeLogs(const std::vector<Row>& turns,
                                      const std::vector<Row>& calib,
                                      int* unpairedCalibration = nullptr)
{
    constexpr size_t kLookAhead = 4;
    std::vector<Request> out;
    std::vector<bool> used(calib.size(), false);
    size_t j = 0;
    for (const Row& t : turns) {
        Request q;
        q.time         = S(t, "time");
        q.model        = S(t, "model");
        q.prompt       = L(t, "prompt_tokens");
        q.cached       = L(t, "cached_tokens");
        q.completion   = L(t, "completion_tokens");
        q.reasoning    = L(t, "reasoning_tokens");
        q.firstTokenMs = D(t, "first_token_ms");
        q.totalMs      = D(t, "total_ms");
        q.cacheWrite   = L(t, "cache_write_tokens");
        q.cost         = D(t, "cost_usd");
        q.finish       = S(t, "finish_reason");
        q.thinkBytes   = L(t, "think_bytes");
        q.replyBytes   = L(t, "reply_bytes");
        q.toolCalls    = L(t, "tool_calls");
        q.stoppedBy    = S(t, "stopped_by");
        if (q.prompt > 0) {
            for (size_t k = j; k < calib.size() && k < j + kLookAhead; ++k) {
                if (!used[k] && L(calib[k], "prompt_tokens") == q.prompt) {
                    FillFromCalibration(q, calib[k]);
                    used[k] = true;
                    j = k + 1;
                    break;
                }
            }
        }
        out.push_back(std::move(q));
    }
    int unpaired = 0;
    for (size_t k = 0; k < calib.size(); ++k) {
        if (used[k]) continue;
        ++unpaired;
        Request q;
        FillFromCalibration(q, calib[k]);
        out.push_back(std::move(q));
    }
    if (unpaired && !turns.empty())   // keep time order when both logs exist
        std::stable_sort(out.begin(), out.end(),
                         [](const Request& a, const Request& b) { return a.time < b.time; });
    if (unpairedCalibration) *unpairedCalibration = unpaired;
    return out;
}

// ── Formatting ──────────────────────────────────────────────────────
inline std::string Thousands(long long n)
{
    if (n < 0) return "-";
    std::string s = std::to_string(n), out;
    int c = 0;
    for (auto it = s.rbegin(); it != s.rend(); ++it) {
        if (c && c % 3 == 0) out.push_back(',');
        out.push_back(*it);
        ++c;
    }
    return std::string(out.rbegin(), out.rend());
}
inline std::string Fixed(double v, const char* fmt)
{
    if (v < 0) return "-";
    char b[48];
    std::snprintf(b, sizeof b, fmt, v);
    return b;
}
inline std::string Kb(long long bytes) { return bytes < 0 ? "-" : Fixed(bytes / 1024.0, "%.1f"); }
inline std::string Pct(double part, double whole)
{
    return whole > 0 ? Fixed(part * 100.0 / whole, "%.0f%%") : "-";
}
inline std::string Cell(std::string s)
{
    for (char& c : s) if (c == '|') c = '/';
    return s.empty() ? "-" : s;
}
inline double Median(std::vector<double> v)
{
    if (v.empty()) return -1;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

// Below this, a model's reported prompt tokens are far above what the
// request text normally tokenizes to (text/code is ~3.5-6.5 bytes/token).
constexpr double kLowBytesPerToken = 2.5;

// The "## Metrics" section.  Never empty: says so when nothing was logged.
inline std::string BuildMetricsMarkdown(const std::string& turnStatsTsv,
                                        const std::string& calibrationTsv)
{
    const auto turns = ParseTsv(turnStatsTsv);
    const auto calib = ParseTsv(calibrationTsv);
    int unpaired = 0;
    const auto reqs = MergeLogs(turns, calib, &unpaired);

    std::string md = "## Metrics\n\n";
    if (reqs.empty()) {
        md += "_No per-request metrics were logged for this conversation "
              "(turn_stats.tsv and ctx_calibration.tsv are missing or empty)._\n";
        return md;
    }
    md += "_From this chat's turn_stats.tsv and ctx_calibration.tsv. Token counts are as the "
          "provider reported them; request sizes are the bytes LlamaBoss sent._\n\n";

    // ── Totals ──
    long long withUsage = 0, sumPrompt = 0, sumCached = 0, sumOut = 0, sumReason = 0;
    long long elidedReqs = 0, maxElided = 0;
    const Request* largest = nullptr;
    std::vector<double> ttft;
    double sumTotalMs = 0;
    std::map<std::string, int> modelCount;
    std::map<std::string, std::vector<double>> bptByModel;
    bool anyBreakdown = false, multiModel = false, multiDay = false;
    bool anyCacheWrite = false, anyCost = false, anyFinish = false, anyShape = false,
         anyPrefix = false;
    long long sumCacheWrite = 0, cacheCtlOn = 0, cacheCtlLogged = 0;
    // Replies in turn_stats.tsv with no usage report (so no calibration row
    // and no request makeup), and how many of those LlamaBoss stopped.
    long long noUsageReplies = 0, repetitionStops = 0;
    double sumCost = 0;
    std::map<std::string, int> finishCount;
    std::map<std::string, std::vector<std::string>> prefixByModel;   // distinct, in order
    const std::string firstDay = reqs.front().time.substr(0, 10);
    for (const Request& q : reqs) {
        if (!q.model.empty()) ++modelCount[q.model];
        if (q.prompt >= 0) {
            ++withUsage;
            sumPrompt += q.prompt;
            if (!largest || q.prompt > largest->prompt) largest = &q;
        }
        if (q.cached > 0)     sumCached += q.cached;
        if (q.completion > 0) sumOut    += q.completion;
        if (q.reasoning > 0)  sumReason += q.reasoning;
        const long long el = std::max(0LL, q.elided) + std::max(0LL, q.argsElided);
        if (el > 0) { ++elidedReqs; maxElided = std::max(maxElided, el); }
        if (q.firstTokenMs >= 0) ttft.push_back(q.firstTokenMs);
        if (q.totalMs > 0) sumTotalMs += q.totalMs;
        if (q.Bpt() > 0 && !q.model.empty()) bptByModel[q.model].push_back(q.Bpt());
        anyBreakdown |= q.HasBreakdown();
        if (q.cacheWrite >= 0) { anyCacheWrite = true; sumCacheWrite += q.cacheWrite; }
        if (q.cost >= 0)       { anyCost = true; sumCost += q.cost; }
        const std::string fin = q.FinishLabel();
        if (!fin.empty()) { anyFinish = true; ++finishCount[fin]; }
        if (q.StoppedForRepetition()) ++repetitionStops;
        if (q.prompt < 0 && q.totalMs >= 0) ++noUsageReplies;   // reply row, no usage
        if (q.toolCalls >= 0 || q.thinkBytes >= 0 || q.replyBytes >= 0) anyShape = true;
        if (!q.prefix.empty()) {
            anyPrefix = true;
            auto& v = prefixByModel[q.model];
            if (std::find(v.begin(), v.end(), q.prefix) == v.end()) v.push_back(q.prefix);
        }
        if (q.cacheCtl >= 0) { ++cacheCtlLogged; if (q.cacheCtl == 1) ++cacheCtlOn; }
        if (q.time.size() >= 10 && q.time.substr(0, 10) != firstDay) multiDay = true;
    }
    multiModel = modelCount.size() > 1;

    md += "### Summary\n\n| | |\n|---|---|\n";
    md += "| Requests logged | " + Thousands((long long)reqs.size()) +
          (withUsage != (long long)reqs.size() ? " (" + Thousands(withUsage) + " with token usage)" : "") + " |\n";
    {
        std::string m;
        for (const auto& [name, n] : modelCount) {
            if (!m.empty()) m += ", ";
            m += Cell(name) + " (" + std::to_string(n) + ")";
        }
        md += "| Models | " + (m.empty() ? std::string("-") : m) + " |\n";
    }
    if (largest) {
        md += "| Largest prompt | " + Thousands(largest->prompt) + " tokens";
        if (largest->reqBytes > 0) md += ", " + Kb(largest->reqBytes) + " KB sent";
        md += " (" + Cell(largest->time) + ") |\n";
    }
    md += "| Prompt tokens, all requests | " + Thousands(sumPrompt) +
          (sumCached > 0 ? " (" + Thousands(sumCached) + " cached, " + Pct((double)sumCached, (double)sumPrompt) + ")" : "") + " |\n";
    if (sumCached > 0 || anyCacheWrite || cacheCtlOn > 0)
        md += "| Uncached prompt tokens | " + Thousands(sumPrompt - sumCached) + " (" +
              Pct((double)(sumPrompt - sumCached), (double)sumPrompt) +
              " of prompt tokens, billed at the full input rate) |\n";
    if (anyCacheWrite)
        md += "| Cache writes | " + Thousands(sumCacheWrite) + " tokens"
              + (sumCacheWrite > 0 ? " (Anthropic bills writes at 1.25x input)" : "") + " |\n";
    if (cacheCtlLogged > 0 && cacheCtlOn > 0)
        md += "| Anthropic cache_control | sent on " + Thousands(cacheCtlOn) + " of " +
              Thousands(cacheCtlLogged) + " requests |\n";
    if (anyCost)
        md += "| Cost, provider-reported | $" + Fixed(sumCost, "%.4f") + " total, $" +
              Fixed(sumCost / (double)reqs.size(), "%.4f") + " per request |\n";
    if (anyPrefix) {
        for (const auto& [name, v] : prefixByModel) {
            md += "| Cacheable prefix (system + tools)";
            if (multiModel) md += ", " + Cell(name);
            md += " | " + std::to_string(v.size()) + (v.size() == 1 ? " version" : " versions");
            if (v.size() > 1)
                md += " **changed mid-chat: every change is a full cache miss**";
            md += " |\n";
        }
    }
    if (anyFinish) {
        std::string f;
        for (const auto& [why, n] : finishCount) {
            if (!f.empty()) f += ", ";
            f += Cell(why) + " " + std::to_string(n);
        }
        md += "| Finish reasons | " + f + " |\n";
    }
    if (repetitionStops > 0)
        md += "| Stopped by LlamaBoss | " + Thousands(repetitionStops) +
              " (the model kept repeating the same text; the reply was cut off "
              "before the server reported token usage) |\n";
    md += "| Output tokens, all requests | " + Thousands(sumOut) +
          (sumReason > 0 ? " (" + Thousands(sumReason) + " reasoning)" : "") + " |\n";
    md += "| Requests with elision | " + Thousands(elidedReqs) +
          (elidedReqs ? " (up to " + Thousands(maxElided) + " items trimmed in one request)" : "") + " |\n";
    if (!ttft.empty())
        md += "| Time to first token | median " + Fixed(Median(ttft) / 1000.0, "%.1f") + " s |\n";
    if (sumTotalMs > 0)
        md += "| Time waiting on replies | " + Fixed(sumTotalMs / 1000.0, "%.0f") + " s total |\n";
    for (const auto& [name, v] : bptByModel) {
        const double med = Median(v);
        md += "| Bytes per token, " + Cell(name) + " | median " + Fixed(med, "%.2f") +
              " (" + Fixed(*std::min_element(v.begin(), v.end()), "%.2f") + "-" +
              Fixed(*std::max_element(v.begin(), v.end()), "%.2f") + ", " + std::to_string(v.size()) + " requests)";
        if (med > 0 && med < kLowBytesPerToken)
            md += " **low: the provider reported far more prompt tokens than this much text usually produces**";
        md += " |\n";
    }
    md += "\n";

    // ── Largest request makeup ──
    const Request* mk = nullptr;
    for (const Request& q : reqs)
        if (q.HasBreakdown() && q.reqBytes > 0 && (!mk || q.reqBytes > mk->reqBytes)) mk = &q;
    if (mk) {
        md += "### Largest request makeup (" + Cell(mk->time) + ", " + Kb(mk->reqBytes) + " KB)\n\n"
              "| Part | KB | Share |\n|---|---:|---:|\n";
        const double total = (double)mk->reqBytes;
        long long known = 0;
        auto part = [&](const char* label, long long b) {
            if (b <= 0) return;
            known += b;
            md += std::string("| ") + label + " | " + Kb(b) + " | " + Pct((double)b, total) + " |\n";
        };
        part("System prompt", mk->sysBytes);
        part("Tool definitions", mk->toolsBytes);
        part("Your messages", mk->userBytes);
        part("Model replies", mk->asstBytes);
        part("Tool results", mk->resultBytes);
        part("Images", mk->imageBytes);
        part("Reasoning replay", mk->replayBytes);
        if (mk->reqBytes > known)
            md += "| JSON structure and other | " + Kb(mk->reqBytes - known) + " | " +
                  Pct((double)(mk->reqBytes - known), total) + " |\n";
        md += "\n";
    } else if (withUsage == 0 && noUsageReplies > 0) {
        // Not an old build: makeup is only logged alongside a usage report,
        // and no reply here had one.
        md += "_No request makeup: LlamaBoss logs it only for replies that end with the "
              "server's token-usage report, and no reply in this chat had one";
        if (repetitionStops > 0)
            md += " (" + Thousands(repetitionStops) + " stopped by LlamaBoss because the model "
                  "kept repeating itself)";
        md += "._\n\n";
    } else {
        md += "_Request makeup (system prompt, tools, messages, replayed reasoning) is logged "
              "from the 2026-10-05 build on; this chat's requests predate it._\n\n";
    }

    // ── Per-request table ──
    md += "### Requests\n\n| # | Time |";
    if (multiModel) md += " Model |";
    md += " Prompt | Cached |";
    if (anyCacheWrite) md += " Cache write |";
    md += " Out | Reasoning | Sent KB | B/tok | Elided | First token s | Total s |";
    if (anyFinish) md += " Finish |";
    if (anyShape)  md += " Calls | Think KB | Reply KB |";
    if (anyCost)   md += " Cost $ |";
    if (anyPrefix) md += " Prefix |";
    if (anyBreakdown) md += " System KB | Tools KB | User KB | Replies KB | Results KB | Replay KB |";
    md += "\n|---:|---|";
    if (multiModel) md += "---|";
    md += "---:|---:|";
    if (anyCacheWrite) md += "---:|";
    md += "---:|---:|---:|---:|---:|---:|---:|";
    if (anyFinish) md += "---|";
    if (anyShape)  md += "---:|---:|---:|";
    if (anyCost)   md += "---:|";
    if (anyPrefix) md += "---|";
    if (anyBreakdown) md += "---:|---:|---:|---:|---:|---:|";
    md += "\n";
    int n = 0;
    for (const Request& q : reqs) {
        std::string t = q.time;
        if (!multiDay && t.size() > 11) t = t.substr(11);
        std::string el = "-";
        if (q.elided >= 0 || q.argsElided >= 0) {
            el = Thousands(std::max(0LL, q.elided));
            if (q.argsElided > 0) el += " +" + Thousands(q.argsElided) + " args";
        }
        md += "| " + std::to_string(++n) + " | " + Cell(t) + " |";
        if (multiModel) md += " " + Cell(q.model) + " |";
        md += " " + Thousands(q.prompt) + " | " + Thousands(q.cached) + " |";
        if (anyCacheWrite) md += " " + Thousands(q.cacheWrite) + " |";
        md += " " + Thousands(q.completion) +
              " | " + Thousands(q.reasoning) + " | " + Kb(q.reqBytes) + " | " + Fixed(q.Bpt(), "%.2f") +
              " | " + el + " | " + Fixed(q.firstTokenMs / 1000.0, "%.1f") +
              " | " + Fixed(q.totalMs / 1000.0, "%.1f") + " |";
        if (anyFinish) md += " " + Cell(q.FinishLabel()) + " |";
        if (anyShape)
            md += " " + Thousands(q.toolCalls) + " | " + Kb(q.thinkBytes) + " | " + Kb(q.replyBytes) + " |";
        if (anyCost)   md += " " + Fixed(q.cost, "%.5f") + " |";
        if (anyPrefix) md += " " + Cell(q.prefix) + " |";
        if (anyBreakdown)
            md += " " + Kb(q.sysBytes) + " | " + Kb(q.toolsBytes) + " | " + Kb(q.userBytes) + " | " +
                  Kb(q.asstBytes) + " | " + Kb(q.resultBytes) + " | " + Kb(q.replayBytes) + " |";
        md += "\n";
    }
    if (unpaired > 0 && !turns.empty())
        md += "\n_" + std::to_string(unpaired) + " request(s) appear only in ctx_calibration.tsv "
              "(no matching reply row); timing columns are blank for them._\n";
    // Mixed chats: say why some rows have no tokens or makeup.  (When no
    // row has usage, the makeup note above already explains it.)
    if (noUsageReplies > 0 && withUsage > 0) {
        md += "\n_" + Thousands(noUsageReplies) + " request(s) have no token counts or request "
              "makeup: the server sent no usage report";
        if (repetitionStops > 0)
            md += " (" + Thousands(repetitionStops) + " stopped by LlamaBoss because the model "
                  "kept repeating itself)";
        md += "._\n";
    }
    return md;
}

} // namespace lb_export_metrics
