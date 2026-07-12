#pragma once

#include <string>

namespace lb_input_parsers {

// Typed approval replies are accepted while a tool approval card is
// pending.  The UI layer decides how to render unrecognized input.
enum class ApprovalInputAction {
    Unrecognized,
    ApproveOnce,
    ApproveAlways,
    Deny,
};

// Trims, lowercases ASCII, and collapses internal whitespace to one
// space before matching approval phrases.
ApprovalInputAction ParseApprovalInput(const std::string& userInput);

struct SlashCommandParseResult {
    bool        matched = false;
    std::string toolName;
    std::string args;
};

// Parses user-typed tool-shaped slash commands only.  It does not
// execute anything and intentionally does not handle app-state commands
// such as /cd or /goal.
SlashCommandParseResult TryParseToolSlashCommand(const std::string& userInput);

} // namespace lb_input_parsers
