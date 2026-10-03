// tool_context.h
//
// Phase 3 foundation: the resolved execution context passed into every
// tool handler (/cmd, /read, /ls, /grep, and future tools).
//
// A ToolContext is built at command-dispatch time by resolving two
// layers:
//   1. Per-conversation overrides from ChatHistory (m_toolCwd,
//      m_toolTimeoutMs).  Empty / 0 means "fall back".
//   2. Global defaults — kDefaultToolTimeoutMs for timeout, wxGetCwd()
//      for working directory.
//
// Handlers never read from ChatHistory or AppState directly; everything
// they need is on the context.  This makes them straightforward to
// exercise from the Phase 4 agent harness, where the agent loop will
// synthesise a ToolContext and hand it to the same handlers the user
// drives today via slash commands.
//
#pragma once

#include <wx/event.h>

#include <functional>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

class ChatHistory;   // forward — tools that opt into history-aware
                     // resolution (open, future view/edit/delete)
                     // dereference through this; others ignore it.

// Global fallback timeout — used when no per-conversation override is
// set on ChatHistory.  Five minutes keeps finite foreground builds and
// dependency installs inside the controlled executor instead of killing
// them at the former 60-second ceiling.  Stop/cancel still tears the
// process tree down immediately.
constexpr unsigned long kDefaultToolTimeoutMs = 5UL * 60UL * 1000UL;

// A PowerShell invocation may request a different bounded timeout.  The
// override is deliberately constrained: short calls can fail fast, while a
// long build can receive enough time without creating an unbounded process.
constexpr unsigned long kMinPowerShellTimeoutSeconds = 15UL;
constexpr unsigned long kMaxPowerShellTimeoutSeconds = 30UL * 60UL;

struct ToolContext {
    // UI-thread-only guided setup. Accepts provider/model query, never a secret.
    std::function<std::string(const std::string&, const std::string&)> setupConnection;

    // Resolved working directory — absolute path, guaranteed non-empty
    // when built through MakeToolContext().  Caller resolves relative
    // argument paths against this.  Stored as UTF-8.
    std::string cwd;

    // Resolved timeout for tool execution.  Per-conversation override
    // wins over kDefaultToolTimeoutMs; 0 is never a valid resolved
    // value.
    unsigned long timeoutMs = kDefaultToolTimeoutMs;

    // Active model context length in tokens.  Used by tools that emit
    // potentially-large bodies (/read today; /ls and /grep soon) to
    // cap their output so it fits alongside system prompt, chat
    // template, prior history, and assistant output headroom.  A
    // sensible baseline when unset is 8192 — the ServerConfig default
    // and Anthropic-era minimum for chat-friendly ctx.
    int ctxTokens = 8192;

    // wxEvtHandler that will receive completion events from threaded
    // tool handlers (grep, future commands).  Sync handlers ignore.
    wxEvtHandler* eventHandler = nullptr;

    // Weak alive token — guards against posting events to a frame that
    // has been destroyed.  Same pattern as CmdExecutor / ChatClient.
    std::weak_ptr<std::atomic<bool>> aliveToken;

    // Optional, non-owning.  When set, history-aware tools (e.g.
    // /open's fuzzy-match against recent /ls results) can walk back
    // through prior messages to resolve names against context the
    // user established earlier in the conversation.  Tools that do
    // not need history ignore this field.  Lifetime is the caller's
    // responsibility — typically a member of MyFrame.
    const ChatHistory* history = nullptr;

    // Optional active project metadata.  File mutation tools treat
    // activeProjectRoot as an additional allowed write root when set,
    // so project chats can create durable Inputs, Outputs, Notes, and
    // Workflows without changing the conversation cwd.
    std::string activeProjectId;
    std::string activeProjectName;
    std::string activeProjectRoot;

    // Global reusable Skills folder.  Skill contracts and helper scripts
    // are user-authored LlamaBoss assets, so mutation tools may update
    // files beneath this root through the normal approval flow instead of
    // hard-blocking Skill development outside the chat workspace.
    std::string skillsRoot;

    // Additional user-approved write roots for this chat.  These grants are
    // created by the folder-access approval card when a native mutation
    // targets an absolute path outside cwd/project/Skills.  ChatHistory owns
    // the session-scoped list; BuildToolContext snapshots it here so every
    // native mutation tool applies the same boundary.
    std::vector<std::string> additionalWriteRoots;
};
