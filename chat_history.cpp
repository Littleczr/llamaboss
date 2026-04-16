#define _CRT_SECURE_NO_WARNINGS

// chat_history.cpp
#include "chat_history.h"

// Poco headers for JSON
#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Stringifier.h>
#include <Poco/Timestamp.h>
#include <Poco/DateTimeFormatter.h>
#include <Poco/DateTimeFormat.h>
#include <Poco/UUIDGenerator.h>

// wxWidgets for paths and file system
#include <wx/stdpaths.h>
#include <wx/filename.h>

#include <sstream>
#include <fstream>

// File format version: 3 adds per-message "attachments" array
static const int CONVERSATION_FORMAT_VERSION = 3;

ChatHistory::ChatHistory()
{
}

Poco::JSON::Object::Ptr ChatHistory::CreateMessage(const std::string& role,
                                                    const std::string& content,
                                                    const std::string& model)
{
    Poco::JSON::Object::Ptr msg = new Poco::JSON::Object;
    msg->set("role", role);
    msg->set("content", content);
    if (!model.empty()) {
        msg->set("model", model);
    }
    return msg;
}

void ChatHistory::AddUserMessage(const std::string& content, const std::string& target,
                                  const std::vector<AttachmentInfo>& attachments)
{
    auto msg = CreateMessage("user", content);
    if (!target.empty()) {
        msg->set("target", target);
    }

    // Store structured attachment metadata on the message (v3 format).
    // This is separate from the content string — it records what was
    // attached so future code can render file chips, image previews, etc.
    if (!attachments.empty()) {
        Poco::JSON::Array::Ptr arr = new Poco::JSON::Array;
        for (const auto& a : attachments) {
            Poco::JSON::Object::Ptr obj = new Poco::JSON::Object;
            obj->set("kind", a.kind == AttachmentInfo::Kind::Image ? "image" : "text_file");
            obj->set("filename", a.filename);
            obj->set("mime_type", a.mimeType);
            obj->set("byte_size", static_cast<Poco::Int64>(a.byteSize));
            if (!a.storagePath.empty())
                obj->set("storage_path", a.storagePath);
            arr->add(obj);
        }
        msg->set("attachments", arr);
    }

    m_messages.push_back(msg);
    m_dirty = true;
}

void ChatHistory::AddAssistantMessage(const std::string& content, const std::string& model)
{
    m_messages.push_back(CreateMessage("assistant", content, model));
    m_dirty = true;
}

void ChatHistory::AddSystemMessage(const std::string& content)
{
    m_messages.push_back(CreateMessage("system", content));
    m_dirty = true;
}

void ChatHistory::Clear()
{
    m_messages.clear();
    m_streamBuffer.clear();
    m_filePath.clear();
    m_title.clear();
    m_createdAt.clear();
    m_updatedAt.clear();
    m_dirty = false;
}

size_t ChatHistory::GetMessageCount() const
{
    return m_messages.size();
}

bool ChatHistory::IsEmpty() const
{
    return m_messages.empty();
}

// ═══════════════════════════════════════════════════════════════════
//  API Request Builders
// ═══════════════════════════════════════════════════════════════════

std::string ChatHistory::BuildChatRequestJson(const std::string& model, bool stream,
                                               const std::string& systemPrompt) const
{
    Poco::JSON::Object::Ptr root = new Poco::JSON::Object;
    root->set("model", model);
    root->set("stream", stream);

    Poco::JSON::Array::Ptr messagesArray = new Poco::JSON::Array;

    // Prepend system prompt if provided
    if (!systemPrompt.empty()) {
        Poco::JSON::Object::Ptr sysMsg = new Poco::JSON::Object;
        sysMsg->set("role", "system");
        sysMsg->set("content", systemPrompt);
        messagesArray->add(sysMsg);
    }

    for (const auto& msg : m_messages) {
        // Skip empty messages (e.g. the assistant placeholder added before streaming)
        std::string content = msg->getValue<std::string>("content");
        if (content.empty()) continue;
        // Strip per-message metadata ("model", "target", "attachments") from
        // the wire format — only "role" + "content" go to the server.
        Poco::JSON::Object::Ptr wireMsg = new Poco::JSON::Object;
        wireMsg->set("role", msg->getValue<std::string>("role"));
        wireMsg->set("content", content);
        messagesArray->add(wireMsg);
    }
    root->set("messages", messagesArray);

    std::ostringstream oss;
    Poco::JSON::Stringifier::stringify(root, oss);
    return oss.str();
}

// ═══════════════════════════════════════════════════════════════════
//  Streaming Support
// ═══════════════════════════════════════════════════════════════════

void ChatHistory::AddAssistantPlaceholder(const std::string& model)
{
    m_streamBuffer.clear();
    AddAssistantMessage("", model);
}

void ChatHistory::AppendToLastAssistantMessage(const std::string& delta)
{
    if (delta.empty()) return;

    if (!m_messages.empty() && IsLastMessageRole("assistant")) {
        // Accumulate in the buffer (amortized O(1) per delta)
        // and sync to the JSON object so auto-save captures partial content.
        m_streamBuffer += delta;
        m_messages.back()->set("content", m_streamBuffer);
        m_dirty = true;
    }
}

void ChatHistory::UpdateLastAssistantMessage(const std::string& content)
{
    if (!m_messages.empty() && IsLastMessageRole("assistant")) {
        m_messages.back()->set("content", content);
        m_dirty = true;
    }
    m_streamBuffer.clear();
}

void ChatHistory::RemoveLastAssistantMessage()
{
    if (!m_messages.empty() && IsLastMessageRole("assistant")) {
        m_messages.pop_back();
        m_dirty = true;
    }
}

bool ChatHistory::HasAssistantPlaceholder() const
{
    if (!m_messages.empty() && IsLastMessageRole("assistant")) {
        return m_messages.back()->getValue<std::string>("content").empty();
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════════
//  Access Methods
// ═══════════════════════════════════════════════════════════════════

const std::vector<Poco::JSON::Object::Ptr>& ChatHistory::GetMessages() const
{
    return m_messages;
}

std::string ChatHistory::GetMessageModel(const Poco::JSON::Object::Ptr& msg)
{
    if (msg && msg->has("model")) {
        return msg->getValue<std::string>("model");
    }
    return "";
}

std::string ChatHistory::GetMessageTarget(const Poco::JSON::Object::Ptr& msg)
{
    if (msg && msg->has("target")) {
        return msg->getValue<std::string>("target");
    }
    return "";
}

std::string ChatHistory::GetLastUserMessage() const
{
    for (auto it = m_messages.rbegin(); it != m_messages.rend(); ++it) {
        if ((*it)->getValue<std::string>("role") == "user") {
            return (*it)->getValue<std::string>("content");
        }
    }
    return "";
}

std::string ChatHistory::GetLastAssistantMessage() const
{
    for (auto it = m_messages.rbegin(); it != m_messages.rend(); ++it) {
        if ((*it)->getValue<std::string>("role") == "assistant") {
            return (*it)->getValue<std::string>("content");
        }
    }
    return "";
}

bool ChatHistory::IsLastMessageRole(const std::string& role) const
{
    if (m_messages.empty()) {
        return false;
    }
    return m_messages.back()->getValue<std::string>("role") == role;
}

// ═══════════════════════════════════════════════════════════════════
//  File Persistence
// ═══════════════════════════════════════════════════════════════════

bool ChatHistory::SaveToFile(const std::string& filePath, const std::vector<std::string>& models)
{
    std::string savePath = filePath.empty() ? m_filePath : filePath;
    if (savePath.empty()) return false;
    if (m_messages.empty()) return false;

    try {
        // Set timestamps
        if (m_createdAt.empty()) {
            m_createdAt = CurrentTimestamp();
        }
        m_updatedAt = CurrentTimestamp();

        // Auto-generate title if empty
        if (m_title.empty()) {
            m_title = GenerateTitle();
        }

        // Build the JSON document
        // preserveInsertOrder=true keeps title/metadata before messages,
        // which enables fast title extraction in the sidebar.
        Poco::JSON::Object::Ptr root = new Poco::JSON::Object(true);
        root->set("version", CONVERSATION_FORMAT_VERSION);
        root->set("title", m_title);
        root->set("created_at", m_createdAt);
        root->set("updated_at", m_updatedAt);

        // Write models array (v2 format)
        Poco::JSON::Array::Ptr modelsArray = new Poco::JSON::Array;
        for (const auto& m : models) {
            modelsArray->add(m);
        }
        root->set("models", modelsArray);

        // Also write single "model" for v1 backwards compat (first model)
        if (!models.empty()) {
            root->set("model", models.front());
        }

        Poco::JSON::Array::Ptr messagesArray = new Poco::JSON::Array;
        for (const auto& msg : m_messages) {
            // Only save messages with content (skip empty placeholders)
            std::string content = msg->getValue<std::string>("content");
            if (!content.empty()) {
                // Write the full message including "model" field if present
                Poco::JSON::Object::Ptr saveMsg = new Poco::JSON::Object;
                saveMsg->set("role", msg->getValue<std::string>("role"));
                saveMsg->set("content", content);
                std::string msgModel = GetMessageModel(msg);
                if (!msgModel.empty()) {
                    saveMsg->set("model", msgModel);
                }
                std::string msgTarget = GetMessageTarget(msg);
                if (!msgTarget.empty()) {
                    saveMsg->set("target", msgTarget);
                }
                // Preserve attachment metadata (v3 format)
                if (msg->has("attachments")) {
                    saveMsg->set("attachments", msg->getArray("attachments"));
                }
                messagesArray->add(saveMsg);
            }
        }
        root->set("messages", messagesArray);

        // Write to file with pretty formatting
        std::ostringstream oss;
        Poco::JSON::Stringifier::stringify(root, oss, 2);

        std::ofstream file(savePath, std::ios::out | std::ios::trunc);
        if (!file.is_open()) return false;

        file << oss.str();
        file.close();

        m_filePath = savePath;
        m_dirty = false;
        return true;
    }
    catch (...) {
        return false;
    }
}

bool ChatHistory::SaveToFile(const std::string& filePath, const std::string& model)
{
    return SaveToFile(filePath, std::vector<std::string>{ model });
}

bool ChatHistory::LoadFromFile(const std::string& filePath, std::vector<std::string>& outModels)
{
    try {
        std::ifstream file(filePath);
        if (!file.is_open()) return false;

        std::string content((std::istreambuf_iterator<char>(file)),
            std::istreambuf_iterator<char>());
        file.close();

        if (content.empty()) return false;

        Poco::JSON::Parser parser;
        auto result = parser.parse(content);
        auto root = result.extract<Poco::JSON::Object::Ptr>();

        // Read metadata
        if (root->has("title")) {
            m_title = root->getValue<std::string>("title");
        }
        if (root->has("created_at")) {
            m_createdAt = root->getValue<std::string>("created_at");
        }
        if (root->has("updated_at")) {
            m_updatedAt = root->getValue<std::string>("updated_at");
        }

        // Read models — prefer v2 "models" array, fall back to v1 "model" string
        outModels.clear();
        if (root->has("models")) {
            auto arr = root->getArray("models");
            for (size_t i = 0; i < arr->size(); ++i) {
                outModels.push_back(arr->get(i).convert<std::string>());
            }
        }
        else if (root->has("model")) {
            outModels.push_back(root->getValue<std::string>("model"));
        }

        // Read messages (with optional per-message "model" field)
        m_messages.clear();
        if (root->has("messages")) {
            auto messagesArray = root->getArray("messages");
            for (size_t i = 0; i < messagesArray->size(); ++i) {
                auto msgObj = messagesArray->getObject(i);
                std::string role = msgObj->getValue<std::string>("role");
                std::string msgContent = msgObj->getValue<std::string>("content");
                std::string msgModel;
                if (msgObj->has("model")) {
                    msgModel = msgObj->getValue<std::string>("model");
                }
                auto loadedMsg = CreateMessage(role, msgContent, msgModel);
                if (msgObj->has("target")) {
                    loadedMsg->set("target", msgObj->getValue<std::string>("target"));
                }
                // Restore attachment metadata (v3 format; absent in v1/v2 files)
                if (msgObj->has("attachments")) {
                    loadedMsg->set("attachments", msgObj->getArray("attachments"));
                }
                m_messages.push_back(loadedMsg);
            }
        }

        m_filePath = filePath;
        m_dirty = false;
        return true;
    }
    catch (...) {
        return false;
    }
}

bool ChatHistory::LoadFromFile(const std::string& filePath, std::string& outModel)
{
    std::vector<std::string> models;
    bool ok = LoadFromFile(filePath, models);
    outModel = models.empty() ? "" : models.front();
    return ok;
}

std::string ChatHistory::GenerateTitle() const
{
    // Use the first user message, truncated to ~60 chars
    for (const auto& msg : m_messages) {
        if (msg->getValue<std::string>("role") == "user") {
            std::string content = msg->getValue<std::string>("content");
            if (content.empty()) continue;

            // Strip newlines
            for (auto& c : content) {
                if (c == '\n' || c == '\r') c = ' ';
            }

            // Truncate
            if (content.size() > 60) {
                content = content.substr(0, 57) + "...";
            }
            return content;
        }
    }
    return "Untitled conversation";
}

std::string ChatHistory::GetConversationsDir()
{
    // Use GetUserLocalDataDir (%LOCALAPPDATA%) to match ServerManager::GetDataDir().
    // Keeps conversations alongside models, logs, and config in one location.
    wxString userDataDir = wxStandardPaths::Get().GetUserLocalDataDir();
    wxFileName dir(userDataDir + wxFileName::GetPathSeparator() + "conversations"
        + wxFileName::GetPathSeparator());

    if (!dir.DirExists()) {
        dir.Mkdir(wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    }

    return dir.GetPath().ToUTF8().data();
}

std::string ChatHistory::GenerateFilePath()
{
    std::string dir = GetConversationsDir();

    // Generate a unique filename using UUID
    std::string uuid = Poco::UUIDGenerator::defaultGenerator().createRandom().toString();

    // Use just the first 8 chars for a shorter filename
    std::string shortId = uuid.substr(0, 8);

    return dir + std::string(1, (char)wxFileName::GetPathSeparator()) + "chat_" + shortId + ".json";
}

// ═══════════════════════════════════════════════════════════════════
//  Attachment Sidecar Helpers (Phase 3)
// ═══════════════════════════════════════════════════════════════════

std::string ChatHistory::GetAttachmentDir(const std::string& conversationPath)
{
    // Extract the filename stem (e.g. "chat_abc12345" from ".../chat_abc12345.json")
    wxFileName fn(wxString::FromUTF8(conversationPath));
    std::string stem(fn.GetName().ToUTF8().data());
    std::string convDir = GetConversationsDir();

    return convDir + "/attachments/" + stem;
}

std::string ChatHistory::GetAttachmentRelDir(const std::string& conversationPath)
{
    wxFileName fn(wxString::FromUTF8(conversationPath));
    std::string stem(fn.GetName().ToUTF8().data());

    // Forward slashes for cross-platform JSON portability
    return "attachments/" + stem;
}

std::string ChatHistory::CurrentTimestamp()
{
    Poco::Timestamp now;
    return Poco::DateTimeFormatter::format(now, Poco::DateTimeFormat::ISO8601_FORMAT);
}
