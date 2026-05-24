// ─── goal_verifier_support.cpp ─────────────────────────────────────
// Implementation of the goal verifier / contract-builder helpers
// extracted from LlamaBoss.cpp.  See goal_verifier_support.h for the
// public API contract and notes on the underlying protocol.

#include "goal_verifier_support.h"
#include "lb_string_utils.h"

#include <sstream>
#include <utility>

namespace {

// Keep the command recognizer conservative while still accepting
// everyday polite wrappers such as "please continue the goal" or
// "can you verify the goal?".  The remainder still has to match one
// of the explicit command phrases below exactly.
std::string LbNormalizeNaturalLanguageGoalControl(std::string text)
{
    text = LbLowerAscii(LbTrimAscii(std::move(text)));
    while (!text.empty()) {
        const char ch = text.back();
        if (ch != '.' && ch != '?' && ch != '!') break;
        text.pop_back();
        text = LbTrimAscii(std::move(text));
    }

    static const char* kOptionalPrefixes[] = {
        "please ",
        "can you ",
        "could you ",
        "would you "
    };

    for (const char* prefix : kOptionalPrefixes) {
        const std::string prefixText(prefix);
        if (text.rfind(prefixText, 0) == 0) {
            text = LbTrimAscii(text.substr(prefixText.size()));
            break;
        }
    }

    return text;
}

// Helper for ParseGoalContractDraft: trim, cap to kMaxItemBytes,
// drop empties, and respect a per-category maxItems cap.
void LbAppendGoalContractDraftItem(std::vector<std::string>& target,
                                   std::string               item,
                                   size_t                    maxItems)
{
    item = LbTrimAscii(item);
    if (item.empty() || target.size() >= maxItems) return;

    constexpr size_t kMaxItemBytes = 320;
    if (item.size() > kMaxItemBytes)
        item = item.substr(0, kMaxItemBytes - 3) + "...";

    target.push_back(std::move(item));
}

} // anonymous namespace

// ─── Text shaping helpers ──────────────────────────────────────────

std::string LbCompactGoalStripText(std::string s, size_t maxBytes)
{
    for (char& ch : s) {
        if (ch == '\r' || ch == '\n' || ch == '\t')
            ch = ' ';
    }

    std::string collapsed;
    collapsed.reserve(s.size());
    bool previousWasSpace = false;
    for (char ch : s) {
        const bool isSpace = (ch == ' ');
        if (isSpace) {
            if (!previousWasSpace)
                collapsed.push_back(' ');
        } else {
            collapsed.push_back(ch);
        }
        previousWasSpace = isSpace;
    }

    collapsed = LbTrimAscii(std::move(collapsed));
    if (collapsed.size() <= maxBytes) return collapsed;
    if (maxBytes <= 3) return collapsed.substr(0, maxBytes);
    return collapsed.substr(0, maxBytes - 3) + "...";
}

std::string LbClipForGoalVerifier(const std::string& text, size_t maxBytes)
{
    if (text.size() <= maxBytes) return text;

    const std::string marker = "\n... [middle truncated] ...\n";
    if (maxBytes <= marker.size() + 2) {
        return text.substr(0, maxBytes);
    }

    const size_t keepBytes = maxBytes - marker.size();
    const size_t headBytes = (keepBytes * 2) / 3;
    const size_t tailBytes = keepBytes - headBytes;

    return text.substr(0, headBytes)
        + marker
        + text.substr(text.size() - tailBytes);
}

// ─── Natural-language goal control recognizers ─────────────────────

bool LbTryParseNaturalLanguageGoalStart(const std::string& userInput,
                                        std::string&       objective)
{
    objective.clear();

    const std::string trimmed = LbTrimAscii(userInput);
    if (trimmed.empty()) return false;

    const std::string lower = LbLowerAscii(trimmed);

    // Phase 15 intentionally stays conservative. These are explicit
    // "this is a Goal" command-like phrases, not a fuzzy intent classifier.
    // That keeps normal conversation from unexpectedly becoming autonomous.
    static const char* kPrefixes[] = {
        "goal:",
        "make this a goal:",
        "start a goal:",
        "set a goal:",
        "create a goal:",
        "turn this into a goal:"
    };

    for (const char* prefix : kPrefixes) {
        const std::string prefixText(prefix);
        if (lower.rfind(prefixText, 0) != 0) continue;

        objective = LbTrimAscii(trimmed.substr(prefixText.size()));
        return true;
    }

    return false;
}

bool LbTryParseNaturalLanguageGoalControl(const std::string& userInput,
                                          std::string&       command)
{
    command.clear();

    const std::string normalized =
        LbNormalizeNaturalLanguageGoalControl(userInput);
    if (normalized.empty()) return false;

    struct GoalControlPhrase {
        const char* phrase;
        const char* command;
    };

    // Phase 16 natural-language Goal controls.  Keep these as explicit
    // full-message phrases so normal discussion about goals does not
    // accidentally mutate goal state.
    static const GoalControlPhrase kPhrases[] = {
        { "show goal status", "status" },
        { "show the goal status", "status" },
        { "show the current goal", "status" },
        { "what is the current goal", "status" },
        { "what is the current goal status", "status" },
        { "what's the current goal", "status" },
        { "what's the current goal status", "status" },

        { "pause the goal", "pause" },
        { "pause goal", "pause" },

        { "resume the goal", "resume" },
        { "resume goal", "resume" },

        { "continue the goal", "continue" },
        { "continue goal", "continue" },
        { "keep working on the goal", "continue" },
        { "keep working on goal", "continue" },
        { "keep going on the goal", "continue" },

        { "verify the goal", "verify" },
        { "verify goal", "verify" },
        { "check if the goal is complete", "verify" },
        { "check whether the goal is complete", "verify" },

        { "rebuild the goal contract", "rebuild" },
        { "rebuild goal contract", "rebuild" },
        { "rebuild the contract", "rebuild" },

        { "clear the goal", "clear" },
        { "clear goal", "clear" },
        { "cancel the goal", "clear" },
        { "cancel goal", "clear" }
    };

    for (const GoalControlPhrase& phrase : kPhrases) {
        if (normalized == phrase.phrase) {
            command = phrase.command;
            return true;
        }
    }

    return false;
}

// ─── Verifier verdict parser ───────────────────────────────────────

GoalVerifierVerdict ParseGoalVerifierVerdict(const std::string& response)
{
    GoalVerifierVerdict verdict;
    std::string verdictValue;

    std::istringstream lines(response);
    std::string line;
    while (std::getline(lines, line)) {
        const std::string trimmed = LbTrimAscii(line);
        const std::string lower = LbLowerAscii(trimmed);

        if (lower.rfind("verdict:", 0) == 0) {
            verdictValue = LbLowerAscii(LbTrimAscii(trimmed.substr(8)));
        }
        else if (lower.rfind("reason:", 0) == 0 && verdict.reason.empty()) {
            verdict.reason = LbTrimAscii(trimmed.substr(7));
        }
    }

    if (verdictValue.empty()) {
        // Small fallback for local models that omit the label but still begin
        // with the requested verdict token.
        const std::string whole = LbLowerAscii(LbTrimAscii(response));
        if (whole.rfind("complete", 0) == 0) verdictValue = "complete";
        else if (whole.rfind("continue", 0) == 0) verdictValue = "continue";
        else if (whole.rfind("await_user", 0) == 0 ||
                 whole.rfind("await user", 0) == 0 ||
                 whole.rfind("await-user", 0) == 0) verdictValue = "await_user";
    }

    if (verdictValue == "continue" || verdictValue == "continue.") {
        verdict.kind = GoalVerifierVerdictKind::Continue;
    }
    else if (verdictValue == "complete" || verdictValue == "complete.") {
        verdict.kind = GoalVerifierVerdictKind::Complete;
    }
    else if (verdictValue == "await_user" ||
             verdictValue == "await_user." ||
             verdictValue == "await user" ||
             verdictValue == "await user." ||
             verdictValue == "await-user" ||
             verdictValue == "await-user.") {
        verdict.kind = GoalVerifierVerdictKind::AwaitUser;
    }

    return verdict;
}

// ─── Contract builder draft parser ─────────────────────────────────

GoalContractDraft ParseGoalContractDraft(const std::string& response)
{
    GoalContractDraft draft;

    std::istringstream lines(response);
    std::string line;
    while (std::getline(lines, line)) {
        std::string trimmed = LbTrimAscii(line);
        while (!trimmed.empty()) {
            const unsigned char c =
                static_cast<unsigned char>(trimmed.front());
            if (c == '-' || c == '*') {
                trimmed = LbTrimAscii(trimmed.substr(1));
                continue;
            }
            // Common UTF-8 bullet/dash glyphs a local model may emit instead
            // of the requested plain labels (• ‣ ◦ – — …) are all encoded as
            // 0xE2 followed by two continuation bytes.  Strip the whole glyph;
            // stripping only the lead byte would leave the two continuation
            // bytes in front of the label and break the rfind() matches below.
            if (c == 0xE2 && trimmed.size() >= 3) {
                trimmed = LbTrimAscii(trimmed.substr(3));
                continue;
            }
            break;
        }

        const std::string lower = LbLowerAscii(trimmed);

        if (lower.rfind("success:", 0) == 0) {
            LbAppendGoalContractDraftItem(
                draft.successCriteria, trimmed.substr(8), 5);
        }
        else if (lower.rfind("criterion:", 0) == 0) {
            LbAppendGoalContractDraftItem(
                draft.successCriteria, trimmed.substr(10), 5);
        }
        else if (lower.rfind("constraint:", 0) == 0) {
            LbAppendGoalContractDraftItem(
                draft.constraints, trimmed.substr(11), 4);
        }
        else if (lower.rfind("evidence:", 0) == 0) {
            LbAppendGoalContractDraftItem(
                draft.evidenceChecks, trimmed.substr(9), 5);
        }
        else if (lower.rfind("reason:", 0) == 0 && draft.reason.empty()) {
            draft.reason = LbTrimAscii(trimmed.substr(7));
        }
    }

    return draft;
}
