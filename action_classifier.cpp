// action_classifier.cpp
// Implementation of workspace action classification.

#include "action_classifier.h"
#include <algorithm>
#include <cctype>

// ═══════════════════════════════════════════════════════════════════
//  Internal helpers
// ═══════════════════════════════════════════════════════════════════

// Default actions recognized when no explicit list is provided.
static const std::vector<std::string> kDefaultActions = { "search", "savechat" };

// Map a command name to its ActionType.
// Returns Chat if the command is not recognized.
static ActionType CommandToAction(const std::string& cmd)
{
    // Case-insensitive comparison
    std::string lower = cmd;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (lower == "search") return ActionType::WorkspaceSearch;
    if (lower == "savechat") return ActionType::WorkspaceSaveChat;

    return ActionType::Chat;
}

// Check if a command name appears in the supported actions list.
// Comparison is case-insensitive.
static bool IsSupported(const std::string& cmd,
                        const std::vector<std::string>& supported)
{
    std::string lowerCmd = cmd;
    std::transform(lowerCmd.begin(), lowerCmd.end(), lowerCmd.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    for (const auto& s : supported) {
        std::string lowerS = s;
        std::transform(lowerS.begin(), lowerS.end(), lowerS.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (lowerCmd == lowerS) return true;
    }
    return false;
}

// Skip leading whitespace, return position of first non-space char.
static size_t SkipSpaces(const std::string& s, size_t pos = 0)
{
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos])))
        ++pos;
    return pos;
}

// ═══════════════════════════════════════════════════════════════════
//  Main classification function
// ═══════════════════════════════════════════════════════════════════

ActionIntent ClassifyAction(const std::string& message,
                            bool workspaceEnabled,
                            const std::vector<std::string>& supportedActions)
{
    ActionIntent result;
    result.action      = ActionType::Chat;
    result.cleanedBody = message;
    // rawQuery and command default to empty strings

    // ── Fast exit: workspace feature is off ──────────────────────
    if (!workspaceEnabled)
        return result;

    // ── Look for slash-command at the start of the message ───────
    size_t pos = SkipSpaces(message);

    // Must start with '/'
    if (pos >= message.size() || message[pos] != '/')
        return result;

    // Extract the command word after '/'
    size_t cmdStart = pos + 1;  // skip the '/'
    size_t cmdEnd = cmdStart;
    while (cmdEnd < message.size() &&
           !std::isspace(static_cast<unsigned char>(message[cmdEnd])))
    {
        ++cmdEnd;
    }

    // Empty command (just "/" with nothing after)
    if (cmdEnd == cmdStart)
        return result;

    std::string command = message.substr(cmdStart, cmdEnd - cmdStart);

    // ── Check if this command is in the supported list ───────────
    const auto& actions = supportedActions.empty() ? kDefaultActions : supportedActions;
    if (!IsSupported(command, actions))
        return result;

    // ── Map to ActionType ────────────────────────────────────────
    ActionType actionType = CommandToAction(command);
    if (actionType == ActionType::Chat)
        return result;  // Command name recognized but no ActionType mapping

    // ── Extract the query / body after the command ───────────────
    size_t bodyStart = SkipSpaces(message, cmdEnd);
    std::string query = (bodyStart < message.size())
                        ? message.substr(bodyStart)
                        : "";

    // ── Require non-empty query for WorkspaceSearch ──────────────
    // "/search" with no terms is treated as regular chat (safe fallback).
    // Other actions like /savechat are valid without arguments.
    if (actionType == ActionType::WorkspaceSearch && query.empty())
        return result;

    // ── Build the classified result ──────────────────────────────
    result.action      = actionType;
    result.cleanedBody = query;
    result.rawQuery    = query;
    result.command     = command;

    return result;
}
