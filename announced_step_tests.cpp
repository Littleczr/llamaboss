// announced_step_tests.cpp -- standalone checks for announced_step.h.
// Build: cl /std:c++17 /EHsc announced_step_tests.cpp   (or g++ -std=c++17)
#include "announced_step.h"
#include <cstdio>

using lb_announced_step::LooksLikeAnnouncedNextStep;
using lb_announced_step::HasCallInsideLeakedReasoning;
static int fails = 0, passes = 0;
static void expect(bool want, const char* text)
{
    const bool got = LooksLikeAnnouncedNextStep(text);
    (got == want ? passes : fails)++;
    std::printf("%s %-5s %s\n", got == want ? "PASS" : "FAIL", want ? "yes" : "no", text);
}

int main()
{
    // Real stalls from Qwen3.8-27B transcripts (2026-10-11).
    expect(true,  "Step 8: `ls` -- tool result said: both files are 38 bytes.\n\nNow step 9:");
    expect(true,  "Step 1 returned: created directory `lbtest1b`. Step 2: writing `n1.txt` with \"1\".");
    // Other forward-looking endings.
    expect(true,  "Created the folder. Next, I'll write the config file.");
    expect(true,  "Let me read the file to confirm.");
    expect(true,  "Moving on to the tests.");
    expect(true,  "Done with the copy. **Now hashing b.txt:**");
    expect(true,  "Proceeding with step 4.");
    expect(true,  "That call was malformed on my side (I sent the tool name as the pattern instead of the actual pattern). Retrying once with the correct arguments.");
    expect(true,  "Step 2 done (not found, as expected). Now step 3: grep for the quoted pattern.");
    expect(true,  "Let's read n3.txt.");
    expect(true,  "That call was malformed on my end \xE2\x80\x94" " retrying with the correct path.");
    expect(true,  "The mkdir failed (empty args); retrying with the path included.");
    expect(true,  "Step 1: creating the folder \xE2\x80\x94" " retrying with the path included.");
    expect(true,  "Write failed -- let me fix the args.");
    // Long reasoning before a short stall must not hide it (2026-10-11 Q3/Q4 misses).
    {
        std::string withThink = "<think>";
        for (int i = 0; i < 80; ++i) withThink += "Considering the grep quoting rule again. ";
        withThink += "</think>\nStep 3 failed with a validation error. Let me retry once with the exact same pattern.";
        const bool got = LooksLikeAnnouncedNextStep(withThink);
        (got ? passes : fails)++;
        std::printf("%s yes   (3 KB <think> block, then a short 'Let me retry' stall)\n", got ? "PASS" : "FAIL");
    }
    expect(true,  "leaked reasoning about the counting bug...</think>\nMy error tally came back 0. Let me redo the analysis pass cleanly.");

    // Final answers and questions must NOT trigger.
    expect(false, "STEPS_DONE=14\n\nRemaining files: n1.txt (1), n3.txt (3), n4.txt (4) -> sum = 8");
    expect(false, "All ten steps completed. No disagreements.");
    expect(false, "Let me know if you want the remaining files deleted.");
    expect(false, "Which option do you prefer: A or B?");
    expect(false, "Retried twice and it still fails, so I stopped. The file is locked by another process.");
    expect(false, "All 14 steps done \xE2\x80\x94" " STEPS_DONE=14, sum = 8.");
    expect(false, "Edit 3 matched the final file - every diff was accurate.");
    expect(false, "");
    expect(false, "   \n  ");
    {
        std::string longAnswer = "Here is the full report. ";
        while (longAnswer.size() < 700) longAnswer += "Every check passed and the files match. ";
        longAnswer += "Now step 9:";
        const bool got = LooksLikeAnnouncedNextStep(longAnswer);
        (got == false ? passes : fails)++;
        std::printf("%s no    (700+ byte reply ending in an announcement)\n", got ? "FAIL" : "PASS");
    }

    // Calls written inside leaked reasoning (orphan </think> after them).
    auto expectLeak = [&](bool want, const char* text, const char* what) {
        const bool got = HasCallInsideLeakedReasoning(text);
        (got == want ? passes : fails)++;
        std::printf("%s %-5s leaked-call: %s\n", got == want ? "PASS" : "FAIL", want ? "yes" : "no", what);
    };
    expectLeak(true,  "I'll run a snippet.\n<tool_call>\n<name>py</name>\n<args>print(1)</args>\n</tool_call>\n</think>\n\nDone -- printed above.",
               "Q4 turn 1: py call before a stray </think>");
    expectLeak(false, "<think>plan <tool_call><name>py</name></tool_call></think>Answer",
               "call inside a WRAPPED think block is ordinary hidden reasoning");
    expectLeak(false, "<tool_call>\n<name>ls</name>\n<args>.</args>\n</tool_call>",
               "normal call, no think tags at all");
    expectLeak(false, "reasoning mentions <tool_call> but no complete block</think>Final",
               "a mention without a complete block");

    std::printf("\n%d/%d passed\n", passes, passes + fails);
    return fails ? 1 : 0;
}
