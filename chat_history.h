#pragma once
// chat_history.h

#include <vector>
#include <string>
#include <unordered_set>

// Poco headers
#include <Poco/JSON/Object.h>

#include "attachment_manager.h"  // for AttachmentInfo
#include "tool_invocation.h"     // for tool_names::kPythonInstallPackage
#include "goal_state.h"          // Goals Phase 1: per-conversation in-memory goal state

// Chat history management class with file persistence
class ChatHistory
{
public:
    ChatHistory();
    ~ChatHistory() = default;

    // Message management
    // model param on assistant messages: tags which model produced the response.
    // Empty string is valid (single-model legacy behavior).
    // target param on user messages: which model was addressed (empty = group turn).
    // attachments: optional metadata about files attached to this message.
    void AddUserMessage(const std::string& content, const std::string& target = "",
                        const std::vector<AttachmentInfo>& attachments = {});
    void AddAssistantMessage(const std::string& content, const std::string& model = "");
    void AddSystemMessage(const std::string& content);

    // ── Phase 3c-ii: native function-calling sidecar fields ─────
    //
    // Attach a JSON tool_calls array to the most-recent assistant
    // message.  The array is the verbatim payload received from
    // llama-server's structured streaming response (see
    // ChatClient's accumulator in Phase 3c-ii) — id/type/function
    // tuples that the next request must thread back as role:"tool"
    // replies via tool_call_id.
    //
    // Stored as a Poco::JSON::Array on the message object so the
    // request builder can serialize it without re-parsing.  No-op
    // if the last message isn't role:"assistant".  toolCallsJson
    // empty → does nothing (preserves the assistant message as
    // pure prose).
    void SetLastAssistantToolCalls(const std::string& toolCallsJson);

    // Append a tool-result message that's threaded to a specific
    // assistant tool_call by id.  On the wire (under native
    // protocol) it serializes as role:"tool" with tool_call_id;
    // under XML protocol the id is ignored and the message
    // appears as an ordinary user message.  The content is the
    // existing FormatToolBlockAsUserMessage output so the rendered
    // chat looks identical regardless of protocol.
    //
    // toolCallId empty → falls back to AddUserMessage(content)
    // semantics.  Provided as a single entry point so callers
    // don't need to branch on protocol themselves.
    void AddToolResultMessage(const std::string& toolCallId,
                              const std::string& content);

    // History management
    void Clear();
    size_t GetMessageCount() const;
    bool IsEmpty() const;

    // True when the conversation has something worth saving even if it
    // has no chat messages yet.  Projects Phase 1 uses this so attaching
    // a project to a brand-new chat can persist across reloads, and so
    // clearing metadata from an already-saved empty chat can persist too.
    bool HasPersistableContent() const;

    // ── API integration ───────────────────────────────────────────
    // Build a JSON request body for the OpenAI-compatible /v1/chat/completions endpoint.
    // If systemPrompt is non-empty, it is prepended as a system message.
    // If contextTokens > 0 and the body would exceed ~70% of the token budget
    // (estimated at 3 bytes/token), older tool-result bodies are elided with
    // a replay hint so the model can re-issue the call if it needs the data.
    // The most recent 2 tool results are always preserved intact.
    //
    // Phase 3c-i: when toolsArrayJson is non-empty, it is parsed and
    // attached to the body as the OpenAI function-calling "tools"
    // field.  Empty (the default) preserves Phase 1/2 behavior — no
    // tools field, the model sees only chat messages.  Build with
    // BuildToolsArrayJson(GetGlobalRouter()) when the active model
    // is on the Native tool protocol.
    //
    // Phase 3c-ii: when nativeProtocol is true, stored assistant
    // messages with a "tool_calls" sidecar are emitted as proper
    // assistant tool_call messages (with their content cleared per
    // OpenAI spec), and stored user messages tagged with a
    // "tool_call_id" sidecar are emitted as role:"tool" messages
    // referencing that id.  Dangling tool_calls (an assistant call
    // without a matching tool reply remaining after elision) are
    // dropped to satisfy llama-server's strict request validation.
    //
    // Not const because the builder must flush the in-memory streaming
    // buffer onto the JSON object before serialization.
    // Poco::SharedPtr propagates const through operator-> (a const
    // SharedPtr yields a const T*), which makes Object::set()
    // uncallable from a const path even though the vector member
    // wouldn't have to be mutated.
    std::string BuildChatRequestJson(const std::string& model, bool stream = true,
                                     const std::string& systemPrompt = "",
                                     int contextTokens = 0,
                                     const std::string& toolsArrayJson = "",
                                     bool nativeProtocol = false);

    // ── Streaming support methods ─────────────────────────────────
    void AddAssistantPlaceholder(const std::string& model = "");
    void AppendToLastAssistantMessage(const std::string& delta);
    void UpdateLastAssistantMessage(const std::string& content);
    void RemoveLastAssistantMessage();
    bool HasAssistantPlaceholder() const;

    // ── Access methods ────────────────────────────────────────────
    const std::vector<Poco::JSON::Object::Ptr>& GetMessages() const;

    // Read the "model" field from a message (empty string if absent)
    static std::string GetMessageModel(const Poco::JSON::Object::Ptr& msg);

    // Read the "target" field from a user message (empty string if absent/group turn)
    static std::string GetMessageTarget(const Poco::JSON::Object::Ptr& msg);

    // Utility methods
    std::string GetLastUserMessage() const;
    std::string GetLastAssistantMessage() const;

    // ── File persistence ─────────────────────────────────────────
    // Save conversation to a JSON file. If filePath is empty, uses m_filePath.
    // models: list of models used (single-element for normal chat, two for group).
    bool SaveToFile(const std::string& filePath, const std::vector<std::string>& models);
    // Convenience overload for single-model save (wraps into vector).
    bool SaveToFile(const std::string& filePath, const std::string& model);

    // Load conversation from a JSON file.
    // outModels: populated with the model(s) stored in the file.
    bool LoadFromFile(const std::string& filePath, std::vector<std::string>& outModels);
    // Convenience overload: returns first model only (backwards compat).
    bool LoadFromFile(const std::string& filePath, std::string& outModel);

    // File path management
    std::string GetFilePath() const { return m_filePath; }
    void SetFilePath(const std::string& path) { m_filePath = path; }
    bool HasFilePath() const { return !m_filePath.empty(); }

    // Dirty tracking — true when content has changed since last save/load
    bool IsDirty() const { return m_dirty; }

    // Conversation metadata
    std::string GetTitle() const { return m_title; }
    void SetTitle(const std::string& title) { m_title = title; }
    std::string GetCreatedAt() const { return m_createdAt; }

    // ── Project association (Projects Phase 1) ───────────────────
    // Optional long-lived project attached to this conversation.  A
    // missing project keeps legacy chat behavior.  Project folders are
    // never deleted when a chat is deleted; they are durable user data.
    bool HasProject() const { return !m_projectId.empty() && !m_projectRoot.empty(); }
    std::string GetProjectId() const { return m_projectId; }
    std::string GetProjectName() const { return m_projectName; }
    std::string GetProjectRoot() const { return m_projectRoot; }
    void SetProject(const std::string& id,
                    const std::string& name,
                    const std::string& root) {
        if (id != m_projectId || name != m_projectName || root != m_projectRoot) {
            m_projectId = id;
            m_projectName = name;
            m_projectRoot = root;
            m_dirty = true;
        }
    }
    void ClearProject() {
        if (HasProject() || !m_projectName.empty()) {
            m_projectId.clear();
            m_projectName.clear();
            m_projectRoot.clear();
            m_dirty = true;
        }
    }

    // ── Tool execution context (Phase 3) ──────────────────────────
    // Per-conversation working directory for slash-command tools
    // (/cmd, /read, /ls, /grep).  Empty string means "fall back to
    // the app's current directory".  Persisted alongside the
    // conversation so switching chats restores the right context.
    std::string GetToolCwd() const { return m_toolCwd; }
    void        SetToolCwd(const std::string& cwd) {
        if (cwd != m_toolCwd) {
            m_toolCwd = cwd;
            m_dirty = true;
        }
    }

    // Per-conversation timeout override for tool execution, in
    // milliseconds.  0 means "use the global default"
    // (kDefaultToolTimeoutMs in tool_context.h).  Kept on ChatHistory
    // so agent-harness conversations can carry their own limits.
    unsigned long GetToolTimeoutMs() const { return m_toolTimeoutMs; }
    void          SetToolTimeoutMs(unsigned long ms) {
        if (ms != m_toolTimeoutMs) {
            m_toolTimeoutMs = ms;
            m_dirty = true;
        }
    }

    // Per-conversation approval memory.  In this polish pass, plain
    // approve enables one-approval mode for this chat; approve once
    // remains available for a single-card approval.  After trust is
    // enabled, subsequent approval-required tools skip the approval
    // card until the conversation is cleared or reloaded.
    //
    // Exception: python_install_package is intentionally excluded from
    // the chat-wide trust flag.  Each install names a distinct package
    // that touches the user's Python environment, so it always renders
    // its own approval card with the exact package name even when
    // one-approval mode is on.  An explicit "approve once" of an
    // install request still individually remembers that single tool
    // name (via RememberToolApproval), but installs cannot be
    // wholesale-approved for the rest of the chat.
    //
    // This is intentionally in-memory only.  It is not persisted with
    // the conversation file, so reopening an older conversation starts
    // with a clean trust state.
    bool IsToolChatApproved(const std::string& name) const {
        if (name == tool_names::kPythonInstallPackage) {
            // Per-package review is the safety boundary now that the
            // package allowlist has been retired.  Never bypass.
            return false;
        }
        return m_chatApprovalTrustEnabled ||
               (!name.empty() && m_chatApprovedTools.count(name) > 0);
    }
    bool IsChatApprovalTrustEnabled() const {
        return m_chatApprovalTrustEnabled;
    }
    void RememberToolApproval(const std::string& name) {
        if (!name.empty()) m_chatApprovedTools.insert(name);
    }
    void RememberAllToolApprovalsForChat() {
        m_chatApprovalTrustEnabled = true;
    }
    void ClearChatApprovedTools() {
        m_chatApprovedTools.clear();
        m_chatApprovalTrustEnabled = false;
    }

    // ── Goals Phase 12: durable mission state + awaiting-user pauses ──
    // Goal state saves with the conversation JSON: objective, status,
    // verifier bookkeeping, continuation budget state, drafted verification
    // contract, and compact structured AgentEvent evidence.  The
    // explicitly-cleared anti-ghosting tombstone remains runtime-only and is
    // intentionally not persisted.
    bool HasGoal() const { return m_goalState.HasGoal(); }
    bool HasActiveGoal() const { return m_goalState.IsActive(); }
    bool HasPausedGoal() const { return m_goalState.IsPaused(); }
    bool HasAwaitingUserGoal() const { return m_goalState.IsAwaitingUser(); }
    bool HasCompletedGoal() const { return m_goalState.IsCompleted(); }
    bool HasBudgetReachedGoal() const { return m_goalState.IsBudgetReached(); }
    bool GoalContractReady() const { return m_goalState.contract.IsReady(); }
    bool GoalContractDrafting() const { return m_goalState.contract.IsDrafting(); }
    const GoalState& GetGoalState() const { return m_goalState; }

    void StartGoal(const std::string& objective) {
        m_goalState.Start(objective);
        m_dirty = true;
    }
    bool PauseGoal() {
        if (!m_goalState.HasGoal() || m_goalState.IsPaused()) return false;
        m_goalState.Pause();
        m_dirty = true;
        return true;
    }
    bool ResumeGoal() {
        if (!m_goalState.HasGoal() || m_goalState.IsActive()) return false;
        m_goalState.Resume();
        m_dirty = true;
        return true;
    }
    void ClearGoal() {
        if (!m_goalState.HasGoal()) return;
        m_goalState.Clear();
        m_dirty = true;
    }

    void BeginGoalContractDrafting() {
        if (!m_goalState.HasGoal()) return;
        m_goalState.BeginContractDrafting();
        m_dirty = true;
    }
    void SetGoalContractReady(const std::vector<std::string>& successCriteria,
                              const std::vector<std::string>& constraints,
                              const std::vector<std::string>& evidenceChecks,
                              const std::string& builderReason) {
        m_goalState.SetContractReady(successCriteria, constraints, evidenceChecks, builderReason);
        m_dirty = true;
    }
    void MarkGoalContractFailed(const std::string& reason) {
        m_goalState.MarkContractFailed(reason);
        m_dirty = true;
    }

    void AppendGoalStructuredAgentEvidence(const std::string& chunk) {
        if (!m_goalState.HasGoal()) return;
        m_goalState.AppendStructuredAgentEvidence(chunk);
        m_dirty = true;
    }
    const std::vector<std::string>& GetGoalStructuredAgentEvidence() const {
        return m_goalState.structuredAgentEvidence;
    }

    void NoteGoalTurnInterrupted(const std::string& reason) {
        if (!m_goalState.HasGoal()) return;
        m_goalState.MarkInterrupted(reason);
        m_dirty = true;
    }

    void MarkGoalAwaitingUser(const std::string& reason,
                              const std::string& promptEvidence) {
        m_goalState.MarkAwaitingUser(reason, promptEvidence);
        m_dirty = true;
    }

    void RecordGoalAwaitingUserReply(const std::string& replyEvidence) {
        if (!m_goalState.IsAwaitingUser()) return;
        m_goalState.RecordAwaitingUserReply(replyEvidence);
        m_dirty = true;
    }

    void NoteGoalVerifierContinue(const std::string& reason) {
        m_goalState.MarkVerifierContinue(reason);
        m_dirty = true;
    }
    void NoteGoalVerifierUnclear(const std::string& reason) {
        m_goalState.MarkVerifierUnclear(reason);
        m_dirty = true;
    }
    void MarkGoalVerifiedComplete(const std::string& reason) {
        m_goalState.MarkVerifiedComplete(reason);
        m_dirty = true;
    }
    bool CanGoalAutoContinue(int maxContinuations) const {
        return m_goalState.CanAutoContinue(maxContinuations);
    }
    void ConsumeGoalAutoContinuation() {
        if (!m_goalState.IsActive()) return;
        m_goalState.ConsumeAutoContinuation();
        m_dirty = true;
    }
    void MarkGoalBudgetReached(const std::string& reason) {
        m_goalState.MarkBudgetReached(reason);
        m_dirty = true;
    }

    // Generate a title from the first user message
    std::string GenerateTitle() const;

    // Get the default conversations directory (creates it if needed)
    static std::string GetConversationsDir();
    // Generate a unique filename for a new conversation
    static std::string GenerateFilePath();

    // ── Per-conversation workflow helpers ─────────────────────────
    // User-visible working area for files created/imported during a
    // single conversation.  Conversation JSON stays under
    // %LOCALAPPDATA%\LlamaBoss\conversations; workflow files live
    // under %USERPROFILE%\LlamaBoss\Workflows\chat_xxxxxxxx.
    //
    // This keeps each chat's files together, lets new chats reuse clean
    // filenames, and makes deleting a chat able to delete its files too.
    static std::string GetWorkflowsDir();
    static std::string GetWorkflowDir(const std::string& conversationPath);
    static std::string GetConversationWorkspaceDir(const std::string& conversationPath);
    static bool        EnsureWorkflowDir(const std::string& conversationPath);

    // ── Attachment sidecar helpers (Phase 3) ──────────────────────
    // User-uploaded attachments now live inside the conversation
    // workflow folder instead of one global sidecar area.
    static std::string GetAttachmentDir(const std::string& conversationPath);

    // Relative path prefix stored in message attachment metadata.  This
    // remains relative for history portability, but is now relative to
    // the conversation workflow root.
    static std::string GetAttachmentRelDir(const std::string& conversationPath);

    // ── Generated-files sidecar helpers ───────────────────────────
    // Files generated from assistant code blocks/artifacts.
    static std::string GetGeneratedFilesDir(const std::string& conversationPath);
    static std::string GetGeneratedFilesRelDir(const std::string& conversationPath);

    // ── Tool-result formatting (Phase 3) ──────────────────────────
    // Unified formatter for any tool invocation round-tripped into the
    // conversation history as a user message.  Emits:
    //
    //   [tool: <toolTag>]
    //   > <commandEcho>
    //
    //   ```<bodyLang>
    //   <body>
    //   ```
    //
    //   [error]
    //   ```
    //   <errorBody>
    //   ```
    //
    //   [status: <chip1>, <chip2>, ...]
    //
    // The body fence length is computed as N+1 backticks where N is
    // the longest backtick run in the body — this keeps the fence
    // robust when /read dumps a markdown file or a code sample that
    // itself contains ``` fences.  Error body uses the same rule.
    // Any section whose input string is empty is skipped; an empty
    // body is common for "did nothing useful" commands like
    // `$null = 1+1`.
    static std::string FormatToolBlockAsUserMessage(
        const std::string& toolTag,
        const std::string& commandEcho,
        const std::string& body,
        const std::string& errorBody,
        const std::vector<std::string>& statusChips,
        const std::string& bodyLang = "");

private:
    std::vector<Poco::JSON::Object::Ptr> m_messages;

    // File persistence state
    std::string m_filePath;     // Current file path (empty = unsaved)
    std::string m_title;        // Conversation title
    std::string m_createdAt;    // ISO timestamp of creation
    std::string m_updatedAt;    // ISO timestamp of last save
    bool        m_dirty = false; // True when content changed since last save/load

    // Tool execution context (Phase 3) — persisted per-conversation.
    // Empty / 0 means "use the global default" at resolution time.
    std::string   m_toolCwd;
    unsigned long m_toolTimeoutMs = 0;

    // Optional project association (Projects Phase 1).
    std::string m_projectId;
    std::string m_projectName;
    std::string m_projectRoot;

    // Per-conversation approval memory (in-memory only; cleared on
    // Clear() and on conversation load).  Lives here rather than on
    // the agent or frame so loading a different chat naturally drops
    // the prior session's approval choices.
    bool m_chatApprovalTrustEnabled = false;
    std::unordered_set<std::string> m_chatApprovedTools;

    // Goals Phase 1 state is also in-memory only.  Keeping it on
    // ChatHistory rather than MyFrame prevents one chat's goal from leaking
    // into another chat when the conversation controller swaps histories.
    GoalState m_goalState;

    // Streaming accumulation buffer — avoids O(n²) string concatenation
    // in AppendToLastAssistantMessage by accumulating here instead of
    // repeatedly reading/writing the Poco JSON object per delta.
    //
    // The Poco JSON object's "content" field is the source of truth on disk
    // and on the wire; m_streamBuffer is the source of truth in memory while
    // a response is streaming.  The two are reconciled lazily by
    // FlushStreamBuffer() — called automatically before any reader (Save,
    // BuildChatRequestJson) and on stream completion via
    // UpdateLastAssistantMessage().  Per-delta sync used to be unconditional
    // and made streaming O(n²) in body bytes; now we sync only after enough
    // bytes have accumulated to amortize the cost.
    //
    // m_streamBufferDirty is the byte count buffered since the last sync to
    // the JSON object.  It only ticks under non-const flush paths, so it
    // does not need to be mutable; FlushStreamBuffer() is non-const because
    // Poco::SharedPtr propagates const through operator-> (a const
    // SharedPtr yields a const T*), preventing Object::set() from a const
    // member.  See FlushStreamBuffer() for the matching rationale.
    std::string m_streamBuffer;
    std::size_t m_streamBufferDirty = 0;

    // Helper methods
    Poco::JSON::Object::Ptr CreateMessage(const std::string& role,
                                          const std::string& content,
                                          const std::string& model = "");
    bool IsLastMessageRole(const std::string& role) const;
    static std::string CurrentTimestamp();

    // Sync the in-memory streaming buffer onto the last assistant message's
    // JSON "content" field.  No-op if the buffer is clean or the last
    // message is not an assistant message.  See m_streamBuffer comment for
    // the full lifetime / consistency contract.
    //
    // Not const: Poco::SharedPtr's operator->() const returns a const T*,
    // so calling Object::set() through m_messages.back() requires non-const
    // access even though the vector itself is not modified.  Callers that
    // need to flush from an otherwise-const reader path should drop const
    // on themselves rather than reach for const_cast here.
    void FlushStreamBuffer();
};

