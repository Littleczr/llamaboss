#include "lb_input_parsers.h"

#include "tool_invocation.h"

namespace lb_input_parsers {
namespace {

bool IsAsciiWhitespace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string TrimAsciiWhitespace(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();

    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string NormalizeApprovalForMatch(const std::string& s)
{
    const std::string trimmed = TrimAsciiWhitespace(s);

    std::string out;
    out.reserve(trimmed.size());

    bool inWs = false;
    for (char c : trimmed) {
        char lc = (c >= 'A' && c <= 'Z')
                  ? static_cast<char>(c - 'A' + 'a')
                  : c;

        if (IsAsciiWhitespace(lc)) {
            if (!out.empty() && !inWs) out.push_back(' ');
            inWs = true;
        } else {
            out.push_back(lc);
            inWs = false;
        }
    }

    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

struct SlashEntry {
    std::string_view prefix;
    const char*      toolName;
};

// Typed tool commands kept for the user.  The model still has every
// tool; these are the only ones a person can run by typing, because
// reminders have no other screen for listing or cancelling them.
static const SlashEntry kToolSlashTable[] = {
    { "/reminder_create",        tool_names::kReminderCreate },
    { "/reminder_list",          tool_names::kReminderList },
    { "/reminder_cancel",        tool_names::kReminderCancel },
};

} // namespace

ApprovalInputAction ParseApprovalInput(const std::string& userInput)
{
    const std::string normalized = NormalizeApprovalForMatch(userInput);

    if (normalized == "/approve once" ||
        normalized == "approve once" ||
        normalized == "allow once" ||
        normalized == "just once") {
        return ApprovalInputAction::ApproveOnce;
    }

    if (normalized == "/approve" ||
        normalized == "approve" ||
        normalized == "allow" ||
        normalized == "run it" ||
        normalized == "go ahead" ||
        normalized == "/approve always" ||
        normalized == "/approve all" ||
        normalized == "/approve chat" ||
        normalized == "/trust chat" ||
        normalized == "approve always" ||
        normalized == "approve all" ||
        normalized == "approve chat" ||
        normalized == "approve conversation" ||
        normalized == "allow always" ||
        normalized == "trust chat") {
        return ApprovalInputAction::ApproveAlways;
    }

    if (normalized == "/deny" ||
        normalized == "deny" ||
        normalized == "cancel" ||
        normalized == "no") {
        return ApprovalInputAction::Deny;
    }

    return ApprovalInputAction::Unrecognized;
}

SlashCommandParseResult TryParseToolSlashCommand(const std::string& userInput)
{
    SlashCommandParseResult result;

    if (userInput.empty() || userInput[0] != '/') return result;

    for (const SlashEntry& e : kToolSlashTable) {
        const size_t plen = e.prefix.size();

        if (userInput.size() < plen) continue;
        if (userInput.compare(0, plen, e.prefix.data(), plen) != 0) continue;

        // Require whitespace or EOS after the verb so "/lsfoo" falls
        // through as normal chat instead of being parsed as "/ls foo".
        if (userInput.size() != plen && !IsAsciiWhitespace(userInput[plen])) {
            continue;
        }

        result.matched = true;
        result.toolName = e.toolName;

        if (userInput.size() > plen) {
            // Preserve existing behavior: the caller already proved the
            // next byte is a whitespace delimiter, and the old parser
            // skipped exactly one delimiter before trimming both ends.
            result.args = userInput.substr(plen + 1);
            result.args = TrimAsciiWhitespace(result.args);
        }

        return result;
    }

    return result;
}

} // namespace lb_input_parsers
