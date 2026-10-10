#pragma once
// ═══════════════════════════════════════════════════════════════════
//  elision_budget.h — self-calibrating bytes-per-token for elision
// ═══════════════════════════════════════════════════════════════════
//
// ChatHistory::BuildChatRequestJson elides old tool results/arguments
// once the request body exceeds
//     contextTokens * bytesPerToken * kBudgetFraction   (bytes).
// A constant bytesPerToken is wrong for most models: measured values run
// from ~1.3 (token-dense models) to ~6.3 bytes per token.  Too high a
// constant elides far too early (a 262k window held at ~40%); too low
// lets requests exceed the window.
//
// AdaptiveBytesPerToken learns the ratio from exact usage reports:
//   * Observe() takes the wire bytes of the last built request and the
//     server's prompt_tokens for it.  Requests under kMinCalibrationBytes
//     are ignored (fixed overhead dominates), as are implausible ratios
//     above kMaxPlausibleBpt (a server reporting only UNCACHED prompt
//     tokens would otherwise inflate the budget) or below
//     kMinPlausibleBpt.
//   * The applied value is clamp(measured * kMargin, kFloorBpt, kCapBpt).
//     kMargin=0.90 covers the observed request-to-request swing (~7%),
//     and kBudgetFraction's 30% headroom remains on top.
//   * Default vs floor are separate: before any measurement the value is
//     kDefaultBpt (3.0); a measured candidate may go down to kFloorBpt
//     (1.0).  Clamping a dense model UP to the default would enlarge its
//     budget and let requests through past the window.  Lowering only
//     shrinks the budget, which is the safe direction.
//   * Hysteresis: lowering (the safety direction) applies when the new
//     candidate is >5% below the applied value; raising needs >10%.
//     A budget that moved every request would move the elision cut
//     every request -- un-eliding content, then eliding it again -- and
//     break the provider's prompt cache each turn.
//   * Elided bodies may LOWER, never RAISE: heavily elided requests
//     measure HIGHER bytes/token (spool markers and shortened arguments
//     tokenize cheaply).  Raising on them un-elides dozens of results in
//     one request, which then measures lower and re-elides -- a sawtooth
//     that breaks the prompt cache twice per cycle.  A raise must come
//     from a body that elided nothing, i.e. one that represents the
//     content a higher budget would restore.
//   * Keyed to the model string: a different model starts at the default.
//   * SeedFromCalibrationTsv() replays the chat folder's
//     ctx_calibration.tsv on load, so a reopened chat does not spend its
//     first request at the default.
//
// Pure arithmetic, no dependencies: tested in elision_budget_tests.cpp.

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace lb_elision {

constexpr double      kDefaultBpt          = 3.0;   // before any measurement
constexpr double      kFloorBpt            = 1.0;   // lowest applied value from a measurement
constexpr double      kCapBpt              = 6.0;
constexpr double      kMargin              = 0.90;
constexpr double      kMaxPlausibleBpt     = 8.0;
constexpr double      kMinPlausibleBpt     = 0.5;   // below: treat the report as bogus
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
        return (m_applied > 0.0 && model == m_model) ? m_applied : kDefaultBpt;
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
        if (!(measured >= kMinPlausibleBpt) || measured > kMaxPlausibleBpt) return false;

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

// ── Image attachments vs. the byte budget ──────────────────────────
// The budget is BYTES (contextTokens * bpt * kBudgetFraction), but an
// attached image travels as a base64 data URI: hundreds of KB on the
// wire for a few thousand tokens.  Two screenshots can be ~90% of a
// request body at a small fraction of the window; counting their raw
// bytes would make the body never fit, so every elidable text tool
// result gets elided on every request and the model loses the very
// reads that verified its claims.
//
// BudgetedBodyBytes() is the size compared against the budget: the body
// with each image data URI's bytes replaced by kImageTokenEstimate
// tokens' worth of bytes at the current bpt.  Eliding text cannot shrink
// an image anyway, so counting its raw bytes only ever cost text.
//
// Only real JSON image values are discounted: the scan requires the
// opening quote of "data:image... to directly follow ':' (a JSON value
// position).  The same text inside a tool result or file body is
// JSON-escaped (\"data:image...), so it is preceded by a backslash and
// keeps counting at full size -- it IS text tokens there.
//
// kImageTokenEstimate is deliberately high (typical provider cost is
// ~1-2k tokens per screenshot); over-estimating only elides a little
// more text, which is the safe direction.
constexpr std::size_t kImageTokenEstimate = 3000;

struct ImageUriScan {
    std::size_t bytes = 0;   // raw bytes of the data-URI values (without quotes)
    std::size_t count = 0;   // number of image data URIs found
};

inline ImageUriScan ScanImageDataUris(const std::string& body)
{
    ImageUriScan out;
    static const std::string kNeedle = "\"data:image";
    std::size_t pos = 0;
    while ((pos = body.find(kNeedle, pos)) != std::string::npos) {
        // Must be a JSON value: `":"data:image` with nothing escaped.
        if (pos == 0 || body[pos - 1] != ':') { pos += kNeedle.size(); continue; }
        const std::size_t valueStart = pos + 1;
        // Base64, the MIME type and ";base64," contain no '"'; a '/' may
        // be emitted as "\/", which is fine.  Stop at the closing quote.
        const std::size_t close = body.find('"', valueStart);
        if (close == std::string::npos) break;   // truncated body: ignore the tail
        out.bytes += close - valueStart;
        ++out.count;
        pos = close + 1;
    }
    return out;
}

// Size to compare against the byte budget.  bytesPerToken is the same
// value the budget was built with, so an image costs exactly
// kImageTokenEstimate tokens of budget.
inline std::size_t BudgetedBodyBytes(const std::string& body, double bytesPerToken)
{
    const ImageUriScan s = ScanImageDataUris(body);
    if (s.count == 0) return body.size();
    const std::size_t text = body.size() - s.bytes;
    const double perImage = (double)kImageTokenEstimate * (bytesPerToken > 0.0 ? bytesPerToken : kDefaultBpt);
    return text + (std::size_t)((double)s.count * perImage);
}

} // namespace lb_elision
