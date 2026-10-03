#pragma once
// ═══════════════════════════════════════════════════════════════════
//  elision_budget.h — self-calibrating bytes-per-token for elision
// ═══════════════════════════════════════════════════════════════════
//
// ChatHistory::BuildChatRequestJson elides old tool results/arguments
// once the request body exceeds
//     contextTokens * bytesPerToken * kBudgetFraction   (bytes).
// bytesPerToken used to be the constant 3.0.  Measured on GPT-6 Luna
// (ctx_calibration.tsv, 2026-10-01, 67 rows): 4.8-6.3 real bytes per
// token, so the 550 KB budget for a 262k window held the conversation
// at ~89-107k real tokens -- about 40% of the window -- and elision ran
// for most of a long agent session with ~150k tokens unused.
//
// AdaptiveBytesPerToken learns the ratio from exact usage reports:
//   * Observe() takes the wire bytes of the last built request and the
//     server's prompt_tokens for it.  Requests under kMinCalibrationBytes
//     are ignored (fixed overhead dominates), as are implausible ratios
//     above kMaxPlausibleBpt (a server reporting only UNCACHED prompt
//     tokens would otherwise inflate the budget).
//   * The applied value is clamp(measured * kMargin, kFloorBpt, kCapBpt).
//     Largest consecutive swing in the Luna log was 6.9%; kMargin=0.90
//     covers that, and kBudgetFraction's 30% headroom remains on top.
//   * Hysteresis: lowering (the safety direction) applies when the new
//     candidate is >5% below the applied value; raising needs >10%.
//     A budget that moved every request would move the elision cut
//     every request -- un-eliding content, then eliding it again -- and
//     break the provider's prompt cache each turn.
//   * Elided bodies may LOWER, never RAISE (2026-10-01, second Luna log,
//     109 rows): heavily elided requests measure HIGHER bytes/token
//     (median 6.20 vs 5.83 unelided -- spool markers and shortened
//     arguments tokenize cheaply).  Raising on them un-elided 30-50
//     results in one request (to 1.08 MB / 184.8k tokens), which then
//     measured lower and re-elided: 11 such sawtooth cycles in 16
//     minutes, each breaking the prompt cache twice.  A raise must come
//     from a body that elided nothing, i.e. one that represents the
//     content a higher budget would restore.
//   * Keyed to the model string: a different model starts at the floor.
//   * SeedFromCalibrationTsv() replays the chat folder's
//     ctx_calibration.tsv on load, so a reopened chat does not spend its
//     first request at the 3.0 floor (observed: 542 KB, 13 results
//     elided, then 865 KB one report later).
//   * Before the first usable measurement: kFloorBpt (the old 3.0),
//     i.e. exactly the previous behaviour.
//
// Pure arithmetic, no dependencies: tested in elision_budget_tests.cpp.

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace lb_elision {

constexpr double      kFloorBpt            = 3.0;
constexpr double      kCapBpt              = 6.0;
constexpr double      kMargin              = 0.90;
constexpr double      kMaxPlausibleBpt     = 8.0;
constexpr std::size_t kMinCalibrationBytes = 32 * 1024;
constexpr double      kLowerThreshold      = 0.95;   // apply if candidate < applied*0.95
constexpr double      kRaiseThreshold      = 1.10;   // apply if candidate > applied*1.10

inline double CandidateFromMeasured(double measuredBpt)
{
    return std::clamp(measuredBpt * kMargin, kFloorBpt, kCapBpt);
}

class AdaptiveBytesPerToken {
public:
    // Bytes-per-token to use for a request to `model`.
    double Current(const std::string& model) const
    {
        return (m_applied > 0.0 && model == m_model) ? m_applied : kFloorBpt;
    }

    // Feed one exact usage report.  `bodyWasElided`: the measured request
    // had any tool result or tool-call argument elided (it may then only
    // lower the applied value).  Returns true when the applied value
    // changed.  Never throws.
    bool Observe(const std::string& model, std::size_t requestBytes,
                 long long promptTokens, bool bodyWasElided = false)
    {
        if (model.empty() || promptTokens <= 0 ||
            requestBytes < kMinCalibrationBytes)
            return false;
        const double measured = (double)requestBytes / (double)promptTokens;
        if (!(measured > 0.0) || measured > kMaxPlausibleBpt) return false;

        m_lastMeasured = measured;
        const double candidate = CandidateFromMeasured(measured);

        if (model != m_model || m_applied <= 0.0) {
            m_model   = model;
            m_applied = candidate;
            return true;
        }
        if (candidate < m_applied * kLowerThreshold ||
            (!bodyWasElided && candidate > m_applied * kRaiseThreshold)) {
            m_applied = candidate;
            return true;
        }
        return false;
    }

    void Reset() { m_applied = 0.0; m_lastMeasured = 0.0; m_model.clear(); }

    double LastMeasured() const { return m_lastMeasured; }

    // Measured bytes-per-token for `model` (unmargined), or 0 if unknown.
    double MeasuredFor(const std::string& model) const
    {
        return (m_applied > 0.0 && model == m_model) ? m_lastMeasured : 0.0;
    }

    const std::string& Model() const { return m_model; }

private:
    std::string m_model;
    double      m_applied      = 0.0;
    double      m_lastMeasured = 0.0;
};

// Replays the tail of a ctx_calibration.tsv (header: time, model,
// req_bytes, prompt_tokens, ..., elided) into `a`.  Only rows for the
// model of the LAST valid row are used, oldest first, at most `maxRows`.
// Columns are located by header name; malformed rows are skipped.
// Returns the number of rows fed.  Never throws.
inline int SeedFromCalibrationTsv(AdaptiveBytesPerToken& a, const std::string& tsv,
                                  std::size_t maxRows = 64)
{
    try {
        auto splitTabs = [](const std::string& line) {
            std::vector<std::string> out;
            std::size_t p = 0;
            while (true) {
                std::size_t t = line.find('\t', p);
                out.push_back(line.substr(p, t == std::string::npos ? std::string::npos : t - p));
                if (t == std::string::npos) break;
                p = t + 1;
            }
            for (auto& f : out) while (!f.empty() && (f.back() == '\r' || f.back() == ' ')) f.pop_back();
            return out;
        };
        std::vector<std::string> lines;
        std::size_t p = 0;
        while (p < tsv.size()) {
            std::size_t nl = tsv.find('\n', p);
            lines.push_back(tsv.substr(p, nl == std::string::npos ? std::string::npos : nl - p));
            if (nl == std::string::npos) break;
            p = nl + 1;
        }
        if (lines.empty()) return 0;
        const auto header = splitTabs(lines[0]);
        int cModel = -1, cBytes = -1, cTokens = -1, cElided = -1;
        for (std::size_t i = 0; i < header.size(); ++i) {
            if (header[i] == "model")         cModel  = (int)i;
            if (header[i] == "req_bytes")     cBytes  = (int)i;
            if (header[i] == "prompt_tokens") cTokens = (int)i;
            if (header[i] == "elided")        cElided = (int)i;
        }
        if (cModel < 0 || cBytes < 0 || cTokens < 0) return 0;

        struct Row { std::string model; unsigned long long bytes; long long tokens; bool elided; };
        std::vector<Row> rows;
        for (std::size_t i = 1; i < lines.size(); ++i) {
            const auto f = splitTabs(lines[i]);
            const int need = std::max(cModel, std::max(cBytes, std::max(cTokens, cElided)));
            if ((int)f.size() <= need) continue;
            try {
                std::size_t used = 0;
                Row r;
                r.model  = f[cModel];
                r.bytes  = std::stoull(f[cBytes], &used);  if (used != f[cBytes].size()) continue;
                r.tokens = std::stoll(f[cTokens], &used);  if (used != f[cTokens].size()) continue;
                r.elided = cElided >= 0 && !f[cElided].empty() && f[cElided] != "0";
                if (r.model.empty() || r.tokens <= 0) continue;
                rows.push_back(r);
            } catch (...) { continue; }
        }
        if (rows.empty()) return 0;
        const std::string model = rows.back().model;
        std::vector<Row> mine;
        for (const Row& r : rows) if (r.model == model) mine.push_back(r);
        const std::size_t start = mine.size() > maxRows ? mine.size() - maxRows : 0;
        int fed = 0;
        for (std::size_t i = start; i < mine.size(); ++i) {
            a.Observe(mine[i].model, (std::size_t)mine[i].bytes, mine[i].tokens, mine[i].elided);
            ++fed;
        }
        return fed;
    } catch (...) {
        return 0;
    }
}

} // namespace lb_elision
