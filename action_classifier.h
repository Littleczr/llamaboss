// action_classifier.h
// Determines whether a user message is a normal chat message or a
// workspace action request (e.g. /search).
//
// This is the first stage of the message pipeline — it runs before
// model routing (RouteMessage / ParseTurnRoute).  If the result is
// anything other than Chat, the message never reaches the LLM.
//
// Supported slash-commands:
//   /search <query>       → WorkspaceSearch
//   /savechat [name]      → WorkspaceSaveChat  (name is optional)
//
// Everything else is Chat.  Matching is case-insensitive on the
// command keyword.  The workspace feature must be enabled for any
// command to be recognized; when disabled the classifier always
// returns Chat.

#pragma once

#include <string>
#include <vector>

// ── Action types ─────────────────────────────────────────────────
enum class ActionType
{
    Chat,               // Default — pass through to model routing
    WorkspaceSearch,    // /search <query>
    WorkspaceSaveChat   // /savechat [optional name]
};

// ── Classifier output ────────────────────────────────────────────
struct ActionIntent
{
    ActionType  action;         // What kind of request this is
    std::string cleanedBody;    // Message with command prefix stripped
    std::string rawQuery;       // Extracted query text (empty for Chat)
    std::string command;        // The matched command name (empty for Chat)
};

// ── Main classification function ─────────────────────────────────
// Inspect a user message and classify it as Chat or a workspace action.
//
//   message          : the raw user input
//   workspaceEnabled : whether the workspace feature is turned on
//   supportedActions : list of recognized command names (e.g. {"search"})
//                      Pass empty to use the built-in default set.
//
// Returns an ActionIntent describing how the message should be handled.
ActionIntent ClassifyAction(const std::string& message,
                            bool workspaceEnabled,
                            const std::vector<std::string>& supportedActions = {});
