// context_stats.h
//
// Data behind the context details panel (the "HUD" opened by clicking the
// ctx meter in the top bar):
//
//   * RequestBreakdown -- byte sizes of each part of the most recent wire
//     request, recorded by ChatHistory::BuildChatRequestJson.
//   * HudInputs / HudModel / BuildContextHudModel -- turns the breakdown,
//     the meter numbers and the TurnStats history into rows of text that
//     ContextHud (context_hud.h) draws and "Copy" puts on the clipboard.
//
// Pure data + formatting, no wx / Poco, so it is unit-testable anywhere.

#pragma once

#include "turn_stats.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

// Byte sizes of the parts of one built request.  Content bytes only (the
// text the model reads); JSON syntax and base64 image data are counted
// separately so they don't distort the per-section token estimates.
struct RequestBreakdown
{
    size_t totalBytes      = 0;  // whole wire body
    size_t systemBytes     = 0;  // system prompt(s), incl. XML tool catalog when not native
    size_t toolsBytes      = 0;  // native "tools" array (function definitions)
    size_t userBytes       = 0;  // user messages (not tool results)
    size_t assistantBytes  = 0;  // assistant replies, incl. native tool_calls
    size_t toolResultBytes = 0;  // tool results fed back to the model
    size_t imageBytes      = 0;  // base64 image data URLs on the wire
    int    imageCount      = 0;
    int    messageCount    = 0;  // wire messages, system included
    int    elidedCount     = 0;  // tool-result bodies trimmed by the budget pass

    size_t TextBytes() const
    {
        return systemBytes + toolsBytes + userBytes + assistantBytes + toolResultBytes;
    }
    bool empty() const { return totalBytes == 0; }
};

// ── Panel content model ─────────────────────────────────────────────

struct HudRow
{
    enum Tone { Normal, Muted, Warn, Danger, Good };
    std::string label;
    std::string value;
    double      bar  = -1;      // 0..1 share bar, -1 = none
    Tone        tone = Normal;
};

struct HudSection
{
    std::string title;
    std::string rightText;      // shown right-aligned on the title line
    double      headerBar = -1; // full-width occupancy bar under the title
    HudRow::Tone headerTone = HudRow::Normal;
    std::vector<HudRow> rows;
};

struct HudModel
{
    std::vector<HudSection> sections;
    std::string plainText;      // same content, for the clipboard
};

struct HudInputs
{
    // Meter (next request occupancy)
    long long ctxUsed    = 0;
    long long ctxWindow  = 0;
    bool      ctxExact   = false;   // ctxUsed as a whole is a server count
    bool      ctxBaseExact = false; // the history part is (draft rides on top)
    long long draftTokens = 0;
    double    elisionFraction = 0.75;

    // Last built request
    RequestBreakdown request;
    long long requestPromptTokens = -1;   // exact prompt tokens for that request, -1 unknown

    // Replies
    TurnStats   last;
    std::vector<TurnStats> history;       // this chat since the last reset, oldest first
};

namespace context_stats {

// Use the reply's recorded identity, including after a chat is reopened.
// Keep provider-qualified remote ids intact; shorten GGUF paths for display.
inline std::string ReplyModelLabel(const TurnStats& t)
{
    if (t.modelId.empty()) return "Unknown (not recorded)";
    std::string name = t.modelId;
    if (t.modelRemote != 1 && name.size() >= 5) {
        std::string ext = name.substr(name.size() - 5);
        for (char& c : ext) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (ext == ".gguf") {
            const size_t sep = name.find_last_of("/\\");
            if (sep != std::string::npos) name.erase(0, sep + 1);
            name.resize(name.size() - 5);
        }
    }
    if (t.modelRemote == 0) name += " (local)";
    else if (t.modelRemote == 1) name += " (remote)";
    return name;
}

// 17214 -> "17,214"
inline std::string Thousands(long long n)
{
    if (n < 0) return "?";
    std::string d = std::to_string(n), out;
    int c = 0;
    for (auto it = d.rbegin(); it != d.rend(); ++it) {
        if (c && c % 3 == 0) out.push_back(',');
        out.push_back(*it);
        ++c;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

inline std::string Compact(long long n) { return TurnStats::FormatCount((long)n); }

inline std::string Fixed(double v, const char* fmt)
{
    char b[48];
    std::snprintf(b, sizeof(b), fmt, v);
    return b;
}

inline HudModel BuildContextHudModel(const HudInputs& in)
{
    HudModel m;
    const char* dot = " \xC2\xB7 ";

    // ── Context ────────────────────────────────────────────────
    {
        HudSection s;
        s.title = "Context";
        s.rightText = std::string("ctx ") + (in.ctxExact ? "" : "~") +
                      Compact(in.ctxUsed) + " / " + Compact(in.ctxWindow);
        const double frac = in.ctxWindow > 0 ? (double)in.ctxUsed / (double)in.ctxWindow : 0.0;
        s.headerBar = std::min(1.0, std::max(0.0, frac));
        s.headerTone = frac >= 0.90 ? HudRow::Danger
                     : frac >= in.elisionFraction ? HudRow::Warn : HudRow::Good;

        s.rows.push_back({ "Used", Fixed(frac * 100.0, "%.1f%%") + " of the window", -1,
                           s.headerTone == HudRow::Good ? HudRow::Normal : s.headerTone });

        const RequestBreakdown& r = in.request;
        if (!r.empty()) {
            // Spread the request's exact prompt count across sections by
            // text share.  With images aboard, their token cost is unknown
            // (provider-specific), so fall back to the ~3 bytes/token
            // heuristic for text and give images whatever is left.
            const double textBytes = (double)r.TextBytes();
            double tokPerByte = 1.0 / 3.0;
            bool   exactSplit = false;
            if (in.requestPromptTokens > 0 && textBytes > 0 && r.imageCount == 0) {
                tokPerByte = (double)in.requestPromptTokens / textBytes;
                exactSplit = true;
            }
            const double shareBase = in.requestPromptTokens > 0
                ? (double)in.requestPromptTokens
                : textBytes * tokPerByte;

            auto addPart = [&](const char* label, size_t bytes) {
                if (bytes == 0) return;
                const double tok = (double)bytes * tokPerByte;
                s.rows.push_back({ label, "~" + Compact((long long)(tok + 0.5)),
                                   shareBase > 0 ? std::min(1.0, tok / shareBase) : -1,
                                   HudRow::Normal });
            };
            addPart("System prompt",    r.systemBytes);
            addPart("Tool definitions", r.toolsBytes);
            addPart("Your messages",    r.userBytes);
            addPart("Model replies",    r.assistantBytes);
            addPart("Tool results",     r.toolResultBytes);
            if (r.imageCount > 0) {
                std::string v = std::to_string(r.imageCount) + (r.imageCount == 1 ? " image" : " images");
                if (in.requestPromptTokens > 0) {
                    const long long rest = in.requestPromptTokens - (long long)(textBytes * tokPerByte + 0.5);
                    if (rest > 0) v += dot + ("~" + Compact(rest));
                }
                s.rows.push_back({ "Images", v, -1, HudRow::Normal });
            }
            if (!exactSplit)
                s.rows.push_back({ "", "(section sizes estimated from text length)", -1, HudRow::Muted });
        }
        if (in.draftTokens > 0)
            s.rows.push_back({ "Draft (unsent)", "~" + Compact(in.draftTokens), -1, HudRow::Muted });

        if (in.ctxWindow > 0) {
            const long long trimAt = (long long)(in.ctxWindow * in.elisionFraction);
            std::string v = "~" + Compact(trimAt) + " (" +
                            Fixed(in.elisionFraction * 100.0, "%.0f%%") + ")";
            v += dot + (r.elidedCount > 0
                        ? std::to_string(r.elidedCount) + " trimmed now"
                        : std::string("nothing trimmed"));
            s.rows.push_back({ "Trims old output at", v, -1,
                               r.elidedCount > 0 ? HudRow::Warn : HudRow::Muted });
        }

        std::string src = in.ctxExact     ? "exact (server count)"
                        : in.ctxBaseExact ? "server count + estimated draft"
                                          : "estimate (no server count yet)";
        if (in.requestPromptTokens > 0 && r.totalBytes > 0 && r.imageCount == 0)
            src += dot + Fixed((double)r.totalBytes / (double)in.requestPromptTokens, "%.1f") +
                   " bytes/token";
        s.rows.push_back({ "Source", src, -1, HudRow::Muted });
        m.sections.push_back(std::move(s));
    }

    // ── Last reply ─────────────────────────────────────────────
    if (!in.last.empty()) {
        const TurnStats& t = in.last;
        HudSection s;
        s.title = "Last reply";
        s.rows.push_back({ "Model", ReplyModelLabel(t), -1, HudRow::Normal });

        if (t.promptTokens >= 0) {
            std::string v = Thousands(t.promptTokens) + " tok";
            if (t.cachedPromptTokens > 0)
                v += " (cached " + Thousands(t.cachedPromptTokens) + ")";
            else if (t.hasServerTimings && t.serverCacheN > 0)
                v += " (from cache " + Thousands(t.serverCacheN) + ", processed " +
                     Thousands(t.serverPromptN) + ")";
            s.rows.push_back({ "Prompt", v, -1, HudRow::Normal });
        }
        if (t.completionTokens >= 0) {
            std::string v = Thousands(t.completionTokens) + " tok";
            if (t.reasoningTokens > 0) v += " (reasoning " + Thousands(t.reasoningTokens) + ")";
            s.rows.push_back({ "Output", v, -1, HudRow::Normal });
        }
        if (t.firstTokenMs >= 0)
            s.rows.push_back({ "First token", TurnStats::FormatSeconds(t.firstTokenMs), -1, HudRow::Normal });
        const double gen = t.GenerationTokensPerSec();
        if (gen > 0)
            s.rows.push_back({ "Generation", Fixed(gen, "%.1f tok/s"), -1, HudRow::Good });
        const double pp = t.PromptTokensPerSec();
        if (pp > 0)
            s.rows.push_back({ "Prompt processing", Fixed(pp, "%.0f tok/s"), -1, HudRow::Normal });
        if (t.totalMs >= 0)
            s.rows.push_back({ "Total", TurnStats::FormatSeconds(t.totalMs), -1, HudRow::Normal });
        s.rows.push_back({ "Measured by",
            t.hasServerTimings ? "llama-server (no network time)"
                               : "LlamaBoss (includes network and provider time)",
            -1, HudRow::Muted });
        m.sections.push_back(std::move(s));
    }

    // ── This chat ──────────────────────────────────────────────
    if (in.history.size() >= 2) {
        HudSection s;
        s.title = "This chat";
        s.rightText = std::to_string(in.history.size()) + " replies";
        double genSum = 0, genMin = 0, genMax = 0, ftSum = 0;
        int genN = 0, ftN = 0;
        long long outSum = 0;
        for (const TurnStats& t : in.history) {
            const double g = t.GenerationTokensPerSec();
            if (g > 0) {
                genMin = genN ? std::min(genMin, g) : g;
                genMax = genN ? std::max(genMax, g) : g;
                genSum += g;
                ++genN;
            }
            if (t.firstTokenMs >= 0) { ftSum += t.firstTokenMs; ++ftN; }
            if (t.completionTokens > 0) outSum += t.completionTokens;
        }
        if (genN > 0) {
            std::string v = Fixed(genSum / genN, "avg %.1f tok/s");
            if (genN > 1) v += dot + Fixed(genMin, "%.1f") + "\xE2\x80\x93" + Fixed(genMax, "%.1f");
            s.rows.push_back({ "Generation", v, -1, HudRow::Normal });
        }
        if (ftN > 0)
            s.rows.push_back({ "First token", "avg " + TurnStats::FormatSeconds(ftSum / ftN), -1, HudRow::Normal });
        s.rows.push_back({ "Output total", Thousands(outSum) + " tok", -1, HudRow::Normal });
        m.sections.push_back(std::move(s));
    }

    // ── Plain text for Copy ────────────────────────────────────
    for (const HudSection& s : m.sections) {
        m.plainText += "## " + s.title;
        if (!s.rightText.empty()) m.plainText += "  (" + s.rightText + ")";
        m.plainText += "\n";
        for (const HudRow& r : s.rows) {
            if (r.label.empty()) m.plainText += "  " + r.value + "\n";
            else m.plainText += "  " + r.label + ": " + r.value + "\n";
        }
        m.plainText += "\n";
    }
    return m;
}

} // namespace context_stats
