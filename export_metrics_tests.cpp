// export_metrics_tests.cpp -- standalone checks for export_metrics.h.
// Build: cl /std:c++17 /EHsc export_metrics_tests.cpp   (or g++ -std=c++17)
//
// Rows below are shaped like the real logs: turn_stats.tsv in the
// turn_stats.h schema, ctx_calibration.tsv first in the older 8-column
// schema, then (same file) a repeated header with the request-makeup
// columns.  Numbers are from a real chat.
#include "export_metrics.h"
#include <cstdio>
#include <string>

using namespace lb_export_metrics;
static int fails = 0, passes = 0;
static void check(bool ok, const char* what)
{
    (ok ? passes : fails)++;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
}
static bool Has(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

static const char* kTurnHeader =
    "time\tmodel\tprompt_tokens\tcompletion_tokens\tcached_tokens"
    "\treasoning_tokens\tfirst_byte_ms\tfirst_token_ms\ttotal_ms"
    "\tgen_tok_s\tprompt_tok_s\tserver_timings"
    "\tserver_prompt_n\tserver_cache_n\tserver_predicted_n"
    "\tserver_prompt_ms\tserver_predicted_ms\tmodel_remote\n";
static const char* kCalOld =
    "time\tmodel\treq_bytes\tprompt_tokens\test_tokens\tbytes_per_token\test_error_pct\telided\n";
static const char* kCalNew =
    "time\tmodel\treq_bytes\tprompt_tokens\test_tokens\tbytes_per_token\test_error_pct\telided"
    "\targs_elided\tsystem_bytes\ttools_bytes\tuser_bytes\tassistant_bytes\ttool_result_bytes"
    "\timage_bytes\treasoning_replay_bytes\n";

int main()
{
    // ── Parsing ──
    {
        auto rows = ParseTsv(std::string(kCalOld) +
                             "2026-10-05 09:34:38\tm\t237344\t170297\t79114\t1.394\t-53.5\t0\r\n\n" +
                             kCalNew +
                             "2026-10-05 09:36:22\tm\t285473\t197732\t95157\t1.444\t-51.9\t0\t2\t30000\t40000\t5000\t60000\t140000\t0\t9000\n");
        check(rows.size() == 2, "old + repeated new header: 2 rows");
        check(L(rows[0], "system_bytes") == -1 && L(rows[0], "elided") == 0, "old row: makeup columns absent, elided read");
        check(L(rows[1], "reasoning_replay_bytes") == 9000 && L(rows[1], "args_elided") == 2, "new row: makeup columns read");
        check(ParseTsv("garbage\nmore\n").empty(), "no header -> nothing");
        check(ParseTsv("").empty(), "empty -> nothing");
    }

    // ── Merge ──
    {
        const std::string turns = std::string(kTurnHeader) +
            "2026-10-05 09:30:45\topenai/gpt-6-luna-pro\t159801\t258\t71860\t161\t3900\t4069\t4194\t\t\t0\t\t\t\t\t\t1\n"
            "2026-10-05 09:30:50\topenai/gpt-6-luna-pro\t164725\t594\t111670\t209\t6800\t6906\t7005\t\t\t0\t\t\t\t\t\t1\n"
            "2026-10-05 09:31:00\topenai/gpt-6-luna-pro\t\t\t\t\t\t\t500\t\t\t0\t\t\t\t\t\t1\n";   // no usage
        const std::string cal = std::string(kCalOld) +
            "2026-10-05 09:30:45\topenai/gpt-6-luna-pro\t228000\t159801\t76000\t1.427\t-52.4\t0\n"
            "2026-10-05 09:30:50\topenai/gpt-6-luna-pro\t236000\t164725\t78666\t1.433\t-52.2\t0\n"
            "2026-10-05 09:31:30\topenai/gpt-6-luna-pro\t240000\t170000\t80000\t1.411\t-52.9\t0\n";   // only here
        int unpaired = -1;
        auto r = MergeLogs(ParseTsv(turns), ParseTsv(cal), &unpaired);
        check(r.size() == 4, "3 replies + 1 calibration-only row");
        check(unpaired == 1, "one unpaired calibration row counted");
        check(r[0].reqBytes == 228000 && r[1].reqBytes == 236000, "pairs by order + prompt_tokens");
        check(r[2].prompt == -1 && r[2].reqBytes == -1, "reply without usage stays unpaired");
        check(r[3].prompt == 170000 && r[3].totalMs < 0, "calibration-only row kept, in time order");
        auto onlyCal = MergeLogs({}, ParseTsv(cal));
        check(onlyCal.size() == 3 && onlyCal[0].reqBytes == 228000, "calibration log alone still reports");
    }

    // ── Markdown ──
    {
        const std::string turns = std::string(kTurnHeader) +
            "2026-10-05 09:22:11\topenai/gpt-6-luna-pro\t308018\t1555\t119355\t289\t11500\t11736\t12970\t\t\t0\t\t\t\t\t\t1\n"
            "2026-10-05 09:23:00\topenai/gpt-6-luna-pro\t43877\t354\t16610\t283\t5000\t5102\t5167\t\t\t0\t\t\t\t\t\t1\n";
        const std::string cal = std::string(kCalNew) +
            "2026-10-05 09:22:11\topenai/gpt-6-luna-pro\t389520\t308018\t129840\t1.265\t-57.8\t0\t0\t30000\t40000\t20000\t90000\t200000\t0\t0\n"
            "2026-10-05 09:23:00\topenai/gpt-6-luna-pro\t70000\t43877\t23333\t1.595\t-46.8\t3\t1\t30000\t40000\t0\t0\t0\t0\t0\n";
        const std::string md = BuildMetricsMarkdown(turns, cal);
        check(Has(md, "## Metrics") && Has(md, "### Summary") && Has(md, "### Requests"), "sections present");
        check(Has(md, "| Largest prompt | 308,018 tokens, 380.4 KB sent"), "largest prompt with thousands + KB");
        check(Has(md, "351,895 (135,965 cached, 39%)"), "prompt total + cached share");
        check(Has(md, "**low:"), "low bytes/token warning for luna-pro");
        check(Has(md, "### Largest request makeup") && Has(md, "| Tool results | 195.3 | 51% |"), "makeup table");
        check(Has(md, "| JSON structure and other |"), "remainder row");
        check(!Has(md, "| Reasoning replay |"), "zero replay omitted from makeup");
        check(Has(md, "| 1 | 09:22:11 |"), "single-day times show clock only");
        check(Has(md, "| 3 +1 args |"), "elided results + args");
        check(!Has(md, " Model |"), "single model: no model column");
        check(Has(md, "Replay KB"), "breakdown columns present");
        check(Has(md, "| Requests with elision | 1 (up to 4 items"), "elision summary");
    }
    {
        // Old logs only: no makeup, explanatory note, no breakdown columns.
        const std::string cal = std::string(kCalOld) +
            "2026-10-04 10:00:00\tgpt-6-luna\t500000\t80000\t166666\t6.250\t+108.3\t0\n"
            "2026-10-05 10:00:00\tanthropic/claude|x\t300000\t110000\t100000\t2.727\t-9.1\t0\n";
        const std::string md = BuildMetricsMarkdown("", cal);
        check(Has(md, "predate it"), "old logs: makeup note");
        check(!Has(md, "Replay KB"), "old logs: no breakdown columns");
        check(Has(md, " Model |") && Has(md, "anthropic/claude/x"), "multi-model column, pipe escaped");
        check(Has(md, "| 2026-10-04 10:00:00 |"), "multi-day keeps full timestamp");
        check(!Has(md, "**low:"), "2.7 B/tok is not flagged");
    }
    {
        const std::string md = BuildMetricsMarkdown("", "");
        check(Has(md, "No per-request metrics"), "no logs -> explicit note");
    }

    std::printf("\n%d/%d passed\n", passes, passes + fails);
    return fails ? 1 : 0;
}
