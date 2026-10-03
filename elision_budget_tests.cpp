// elision_budget_tests.cpp
//
// Regression harness for elision_budget.h (2026-10-01).  No dependencies:
//   g++ -std=c++17 -I . elision_budget_tests.cpp && ./a.out
//
// Includes a replay of the 67-row GPT-6 Luna ctx_calibration.tsv from
// the r16c2 session (req_bytes, prompt_tokens): the adaptive budget must
// never let a filled request exceed the intended window fraction by more
// than the margin allows, must settle (few changes), and must raise the
// usable window well above the old 3.0 constant's ~40%.
#include "elision_budget.h"
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>

using namespace lb_elision;
static int fails = 0, passes = 0;
static void check(bool ok, const std::string& w)
{ (ok ? passes : fails)++; std::cout << (ok ? "PASS " : "FAIL ") << w << "\n"; }

struct Row { long long bytes, tokens; };
struct ERow { long long bytes, tokens; bool elided; };
// Second Luna log (new build, 21:43-22:02): 109 rows incl. the elided
// flag.  The old rule raised on elided rows 11 times (sawtooth).
static const ERow kLuna2[] = {
    {542364, 103280, true},
    {864536, 145045, false},
    {967016, 155841, false},
    {981642, 157786, false},
    {981043, 157998, false},
    {967667, 160316, false},
    {970314, 160567, false},
    {976834, 162046, false},
    {978523, 165858, false},
    {977213, 167802, false},
    {980015, 168086, false},
    {978480, 168344, false},
    {955674, 168250, false},
    {968874, 172074, false},
    {920114, 171876, false},
    {926836, 174996, false},
    {866886, 171427, false},
    {873371, 172878, false},
    {846341, 170709, false},
    {816897, 167326, true},
    {814921, 166316, true},
    {815450, 166526, true},
    {815142, 165958, true},
    {804937, 163476, true},
    {806770, 153406, true},
    {810787, 154088, true},
    {813147, 153860, true},
    {803812, 148350, true},
    {800221, 133887, true},
    {982592, 182251, true},
    {878758, 156525, true},
    {883504, 157123, true},
    {882841, 157047, true},
    {883826, 156812, true},
    {883804, 156405, true},
    {882949, 155353, true},
    {874458, 149688, true},
    {880493, 150441, true},
    {881044, 150957, true},
    {879762, 151801, true},
    {870922, 147300, true},
    {881295, 148472, true},
    {972610, 176397, true},
    {895190, 154416, true},
    {897362, 154711, true},
    {900844, 155124, true},
    {900773, 154307, true},
    {896417, 150885, true},
    {895353, 148777, true},
    {889933, 145659, true},
    {1001144, 177393, true},
    {911749, 150690, true},
    {919461, 151976, true},
    {911794, 142224, true},
    {1052318, 184830, true},
    {919307, 151611, true},
    {928904, 153164, true},
    {919454, 147644, true},
    {916057, 147809, true},
    {927677, 152585, true},
    {917240, 147893, true},
    {925238, 149245, true},
    {924732, 144109, true},
    {1046980, 183235, true},
    {924497, 147496, true},
    {923946, 144775, true},
    {1041232, 178185, true},
    {943597, 147561, true},
    {945209, 151313, true},
    {938722, 147681, true},
    {949009, 149274, true},
    {948432, 148992, true},
    {948054, 150197, true},
    {947627, 146578, true},
    {1054012, 178276, true},
    {949525, 147433, true},
    {952048, 148054, true},
    {950468, 148308, true},
    {954606, 148790, true},
    {956238, 149199, true},
    {960395, 149686, true},
    {956078, 147303, true},
    {944194, 143922, true},
    {1064956, 179700, true},
    {959382, 145659, true},
    {1064732, 177667, true},
    {970809, 147699, true},
    {951204, 143348, true},
    {1077117, 177745, true},
    {968714, 145699, true},
    {971040, 146291, true},
    {970391, 146535, true},
    {978837, 147754, true},
    {981878, 145650, true},
    {1079112, 171573, true},
    {1016705, 156435, true},
    {1006558, 152226, true},
    {1018219, 153692, true},
    {1018761, 152842, true},
    {1020639, 154249, true},
    {1011619, 151275, true},
    {1019188, 152457, true},
    {1020508, 150219, true},
    {1020830, 149318, true},
    {1018161, 151450, true},
    {1018813, 152977, true},
    {1017466, 153184, true},
    {1019182, 153274, true},
    {1021645, 152645, true}
};
static const Row kLuna[] = {
    {91468, 18213},
    {97654, 19556},
    {108083, 21935},
    {117123, 23536},
    {144762, 29449},
    {193166, 40237},
    {222783, 43406},
    {229339, 44080},
    {233790, 44523},
    {247167, 46355},
    {277162, 50545},
    {296664, 53160},
    {319888, 56622},
    {328147, 57487},
    {338304, 59137},
    {348139, 60935},
    {388913, 65551},
    {392812, 65958},
    {401663, 67577},
    {406255, 68243},
    {424513, 70846},
    {433683, 72460},
    {453132, 74669},
    {458928, 75378},
    {467464, 76531},
    {473102, 77274},
    {479829, 78123},
    {485556, 78814},
    {495257, 80083},
    {504797, 81135},
    {513472, 82031},
    {527586, 88759},
    {541935, 90510},
    {545747, 91231},
    {550202, 93961},
    {544814, 93957},
    {548406, 94472},
    {537697, 93538},
    {541530, 93972},
    {536702, 93770},
    {549104, 95252},
    {545147, 96879},
    {535719, 98005},
    {546342, 100567},
    {546882, 101063},
    {549247, 102086},
    {549417, 102402},
    {535812, 100985},
    {546091, 102269},
    {545370, 102754},
    {524724, 100393},
    {534808, 102288},
    {546050, 103648},
    {549768, 104051},
    {547825, 104189},
    {544403, 104484},
    {544447, 104750},
    {528313, 104699},
    {546176, 106602},
    {547254, 106079},
    {545998, 105153},
    {548142, 105130},
    {548500, 104201},
    {549199, 103891},
    {534420, 98626},
    {534484, 98907},
    {533152, 101463}
};

int main()
{
    // Defaults and guards.
    {
        AdaptiveBytesPerToken a;
        check(a.Current("m") == kFloorBpt, "no measurement -> floor 3.0 (old behaviour)");
        check(!a.Observe("m", 20000, 4000), "tiny request ignored");
        check(!a.Observe("m", 600000, 50000), "implausible ratio (12/token, uncached-only report) ignored");
        check(!a.Observe("m", 600000, 0), "zero prompt tokens ignored");
        check(!a.Observe("", 600000, 100000), "empty model ignored");
        check(a.Current("m") == kFloorBpt, "still floor after rejected reports");
        check(a.Observe("m", 500000, 100000), "first valid report applies");
        check(std::abs(a.Current("m") - 4.5) < 1e-9, "5.0 measured -> 4.5 applied");
        check(a.Current("other") == kFloorBpt, "different model -> floor");
        check(!a.Observe("m", 520000, 100000), "+4% swing: no change (hysteresis)");
        check(!a.Observe("m", 480000, 100000), "-4% measured (candidate 4.32 > 4.5*0.95=4.275): no change");
        check(a.Observe("m", 460000, 100000), "-8% measured (candidate 4.14): lowers");
        check(a.Observe("m2", 400000, 100000) && std::abs(a.Current("m2") - 3.6) < 1e-9 && a.Current("m") == kFloorBpt,
              "model switch re-keys");
        AdaptiveBytesPerToken b; b.Observe("m", 700000, 100000);
        check(b.Current("m") == kCapBpt, "7.0 measured -> capped 6.0");
        AdaptiveBytesPerToken c; c.Observe("m", 250000, 100000);
        check(c.Current("m") == kFloorBpt, "2.5 measured -> floor 3.0");
    }

    // Replay the Luna session.
    {
        const double window = 262144, fraction = 0.70;
        AdaptiveBytesPerToken a;
        int changes = 0;
        double worstFill = 0, lastApplied = 0;
        for (const Row& r : kLuna) {
            const double applied = a.Current("gpt-6-luna");
            // If the body were filled exactly to this request's budget, at
            // THIS request's real ratio, what share of the window is used?
            const double budgetBytes = window * applied * fraction;
            const double realBpt = (double)r.bytes / (double)r.tokens;
            const double fill = budgetBytes / realBpt / window;
            if (fill > worstFill) worstFill = fill;
            if (a.Observe("gpt-6-luna", (size_t)r.bytes, r.tokens)) ++changes;
            lastApplied = a.Current("gpt-6-luna");
        }
        char buf[200];
        std::snprintf(buf, sizeof buf, "worst filled-budget share of window %.1f%% (<= 70%%)", worstFill * 100);
        check(worstFill <= 0.70 + 1e-9, buf);
        std::snprintf(buf, sizeof buf, "applied value settled: %d changes over %zu reports", changes,
                      sizeof(kLuna) / sizeof(kLuna[0]));
        check(changes <= 6, buf);
        const double usable = window * lastApplied * fraction / 5.255 / window;   // last row's ratio
        std::snprintf(buf, sizeof buf, "final applied %.2f B/tok -> elision at ~%.0f%% of window (was ~40%%)",
                      lastApplied, usable * 100);
        check(usable > 0.55, buf);
    }


    // Elided bodies may lower, never raise.
    {
        AdaptiveBytesPerToken a;
        a.Observe("m", 500000, 100000);                       // 4.5
        check(!a.Observe("m", 640000, 100000, true) && std::abs(a.Current("m") - 4.5) < 1e-9,
              "elided body at 6.4 does not raise");
        check(a.Observe("m", 640000, 100000, false) && std::abs(a.Current("m") - 5.76) < 1e-9,
              "unelided body at 6.4 raises");
        check(a.Observe("m", 450000, 100000, true) && std::abs(a.Current("m") - 4.05) < 1e-9,
              "elided body at 4.5 still lowers");
    }
    // Replay log 2 with the new rule: once elision starts, no raises.
    {
        AdaptiveBytesPerToken a;
        int raises = 0, raisesFromElided = 0, changes = 0;
        for (const ERow& r : kLuna2) {
            const double before = a.Current("gpt-6-luna");
            if (a.Observe("gpt-6-luna", (size_t)r.bytes, r.tokens, r.elided)) ++changes;
            const double after = a.Current("gpt-6-luna");
            if (after > before && before != kFloorBpt) { ++raises; if (r.elided) ++raisesFromElided; }
        }
        check(raisesFromElided == 0, "log 2 replay: 0 raises from elided bodies (old rule: 11 sawtooth spikes)");
        char buf[160];
        std::snprintf(buf, sizeof buf, "log 2 replay settles: %d changes, final %.2f B/tok", changes, a.Current("gpt-6-luna"));
        check(changes <= 6 && a.Current("gpt-6-luna") >= 4.4, buf);
    }
    // TSV seeding.
    {
        const std::string hdr = "time\tmodel\treq_bytes\tprompt_tokens\test_tokens\tbytes_per_token\test_error_pct\telided\n";
        AdaptiveBytesPerToken a;
        int fed = SeedFromCalibrationTsv(a, hdr +
            "t\tgpt-6-luna\t500000\t100000\t1\t5\t1\t0\r\n"
            "t\tother\t300000\t100000\t1\t3\t1\t0\n"
            "t\tgpt-6-luna\t520000\t100000\t1\t5.2\t1\t13\n");
        check(fed == 2 && std::abs(a.Current("gpt-6-luna") - 4.5) < 1e-9 && a.Model() == "gpt-6-luna",
              "seed uses only the last row's model; elided row cannot raise");
        AdaptiveBytesPerToken b;
        check(SeedFromCalibrationTsv(b, "") == 0 && b.Current("x") == kFloorBpt, "empty file -> floor");
        check(SeedFromCalibrationTsv(b, "garbage\nno\theader\n") == 0, "no header -> nothing");
        check(SeedFromCalibrationTsv(b, hdr + "t\tm\tabc\t100\t1\t1\t1\t0\nt\tm\t500000\n") == 0,
              "malformed rows skipped");
        AdaptiveBytesPerToken c;
        check(SeedFromCalibrationTsv(c, "time\tmodel\treq_bytes\tprompt_tokens\nt\tm\t500000\t100000\n") == 1 &&
              std::abs(c.Current("m") - 4.5) < 1e-9, "minimal header (no elided column) works");
        std::string many = hdr;
        for (int i = 0; i < 200; ++i) many += "t\tm\t" + std::to_string(400000 + i * 1000) + "\t100000\t1\t1\t1\t0\n";
        AdaptiveBytesPerToken d;
        check(SeedFromCalibrationTsv(d, many) == 64, "seeding capped to last 64 rows");
        check(d.MeasuredFor("m") > 5.9 && d.MeasuredFor("other") == 0.0, "MeasuredFor reports last measurement per model");
    }

    std::cout << "\n" << passes << "/" << (passes + fails) << " passed\n";
    return fails ? 1 : 0;
}
