#pragma once
// chat_history.h

#include <vector>
#include <string>

// Poco headers
#include <Poco/JSON/Object.h>

#include "attachment_manager.h"  // for AttachmentInfo

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

    // History management
    void Clear();
    size_t GetMessageCount() const;
    bool IsEmpty() const;

    // ── API integration ───────────────────────────────────────────
    // Build a JSON request body for the OpenAI-compatible /v1/chat/completions endpoint.
    // If systemPrompt is non-empty, it is prepended as a system message.
    std::string BuildChatRequestJson(const std::string& model, bool stream = true,
                                     const std::string& systemPrompt = "") const;

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

    // Generate a title from the first user message
    std::string GenerateTitle() const;

    // Get the default conversations directory (creates it if needed)
    static std::string GetConversationsDir();
    // Generate a unique filename for a new conversation
    static std::string GenerateFilePath();

    // ── Attachment sidecar helpers (Phase 3) ──────────────────────
    // Absolute path to sidecar directory for a conversation's attachments.
    // Does NOT create the directory — caller creates when saving.
    // e.g. ".../conversations/attachments/chat_abc12345/"
    static std::string GetAttachmentDir(const std::string& conversationPath);

    // Relative path prefix (from conversations dir) for storagePath values.
    // Uses forward slashes for cross-platform JSON portability.
    // e.g. "attachments/chat_abc12345"
    static std::string GetAttachmentRelDir(const std::string& conversationPath);

private:
    std::vector<Poco::JSON::Object::Ptr> m_messages;

    // File persistence state
    std::string m_filePath;     // Current file path (empty = unsaved)
    std::string m_title;        // Conversation title
    std::string m_createdAt;    // ISO timestamp of creation
    std::string m_updatedAt;    // ISO timestamp of last save
    bool        m_dirty = false; // True when content changed since last save/load

    // Streaming accumulation buffer — avoids O(n²) string concatenation
    // in AppendToLastAssistantMessage by accumulating here instead of
    // repeatedly reading/writing the Poco JSON object per delta.
    std::string m_streamBuffer;

    // Helper methods
    Poco::JSON::Object::Ptr CreateMessage(const std::string& role,
                                          const std::string& content,
                                          const std::string& model = "");
    bool IsLastMessageRole(const std::string& role) const;
    static std::string CurrentTimestamp();
};

