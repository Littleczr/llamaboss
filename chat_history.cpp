#define _CRT_SECURE_NO_WARNINGS

// chat_history.cpp
#include "chat_history.h"
#include "path_safety.h"
#include "tool_staged_write.h"   // CreateStagedTempFile for atomic save

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
#include <wx/utils.h>

#include <sstream>
#include <fstream>
#include <algorithm>

// File format version: 3 adds per-message "attachments" array
static const int CONVERSATION_FORMAT_VERSION = 4;

// Forward declaration — defined further down with the other workflow
// helpers.  Hoisted here so methods earlier in the file (notably
// SaveToFile, which writes the per-conversation _title.txt marker) can
// call it without reordering the entire helpers block.
static std::string JoinWorkflowPath(const std::string& a, const std::string& b);

namespace {

// ─── Tool-result compaction helpers ──────────────────────────────
//
// Recognize and elide message bodies produced by
// FormatToolBlockAsUserMessage so long conversations stay under the
// model's context window.  The rendered format is stable:
//
//   [tool: NAME]
//   > COMMAND ECHO
//
//   ```LANG
//   BODY
//   ```
//
//   [error]
//   ```
//   ERROR BODY
//   ```
//
//   [status: chip1, chip2, ...]
//
// Elision preserves header, command echo, and status chips; replaces
// the fenced BODY and optional ERROR BODY with a marker.  Some tools
// have side effects (write/edit/mkdir/delete/open, and anything not
// explicitly read-only), so the marker must not invite the model to
// rerun them just to recover omitted output.

constexpr double kBytesPerToken       = 3.0;   // conservative for code
constexpr double kBudgetFraction      = 0.70;  // 30% headroom for response
constexpr size_t kMinPreservedResults = 2;     // last N tool results stay intact

// Does the content of a user message look like a formatted tool
// result?  Single-line prefix check — cheap.
bool IsToolResultMessage(const std::string& content)
{
    return content.compare(0, 7, "[tool: ") == 0;
}

// Extract the tool tag from a header like "[tool: read]".
// Returns empty string if the header is malformed.
std::string ToolTagFromHeader(const std::string& header)
{
    constexpr const char* kPrefix = "[tool: ";
    constexpr size_t      kPrefixLen = 7;

    if (header.compare(0, kPrefixLen, kPrefix) != 0) return {};
    if (header.size() <= kPrefixLen + 1 || header.back() != ']') return {};
    return header.substr(kPrefixLen, header.size() - kPrefixLen - 1);
}

// Only these tools are safe to mention as replayable in compacted
// context.  Everything else is treated as side-effecting or not safe
// for automatic replay.
bool IsReplaySafeTool(const std::string& tag)
{
    return tag == "read" ||
           tag == "ls"   ||
           tag == "grep" ||
           tag == "pwd";
}

bool ContainsString(const std::vector<std::string>& values, const std::string& needle)
{
    return std::find(values.begin(), values.end(), needle) != values.end();
}

std::vector<std::string> ExtractToolCallIds(const Poco::JSON::Array::Ptr& toolCalls)
{
    std::vector<std::string> ids;
    if (!toolCalls) return ids;

    for (size_t i = 0; i < toolCalls->size(); ++i) {
        try {
            auto callObj = toolCalls->getObject(i);
            if (callObj && callObj->has("id")) {
                std::string id = callObj->getValue<std::string>("id");
                if (!id.empty() && !ContainsString(ids, id)) {
                    ids.push_back(id);
                }
            }
        } catch (...) {
            // Skip malformed call entries; caller decides whether an
            // empty/partial id list is usable.
        }
    }
    return ids;
}

// Replace the fenced BODY (and optional ERROR BODY) with an elision
// marker.  Returns the compacted form.  If the input doesn't look
// like a tool result, returns it unchanged.
//
// The marker embeds the echoed command as plain text for auditability,
// but only read-only tools get a replay hint.  Mutating tools must not
// be rerun just because the old output was compacted away.
std::string ElideToolResultBody(const std::string& content)
{
    if (!IsToolResultMessage(content)) return content;

    // Parse out header line ([tool: X]) and echo line (> ...).
    size_t headerEnd = content.find('\n');
    if (headerEnd == std::string::npos) return content;
    std::string header = content.substr(0, headerEnd);

    size_t echoStart = headerEnd + 1;
    size_t echoEnd   = content.find('\n', echoStart);
    if (echoEnd == std::string::npos) return content;
    std::string echo = content.substr(echoStart, echoEnd - echoStart);

    // Status line lives at the end — the footer we want to keep.
    // FormatToolBlockAsUserMessage always ends with `\n[status: ...]`.
    size_t statusStart = content.rfind("\n[status:");
    std::string statusLine;
    if (statusStart != std::string::npos) {
        statusLine = content.substr(statusStart + 1);  // drop leading \n
    }

    // Rebuild: header + echo + elision marker + status.
    // Echo has the ">" prefix baked in, strip it so the marker reads
    // naturally.  This remains plain text and will not be parsed as a
    // tool call.
    std::string callShown = echo;
    if (callShown.size() >= 2 && callShown[0] == '>' && callShown[1] == ' ')
        callShown = callShown.substr(2);

    const std::string tag = ToolTagFromHeader(header);

    std::ostringstream out;
    out << header << "\n"
        << echo << "\n"
        << "\n";

    if (IsReplaySafeTool(tag)) {
        out << "[body elided to fit context. This was a read-only tool result. "
            << "Repeat `" << callShown
            << "` only if the user needs the omitted output again.]\n";
    } else {
        out << "[body elided to fit context. This tool may have side effects "
            << "or is not approved for automatic replay. Do not rerun `"
            << callShown
            << "` just to recover omitted output.]\n";
    }

    if (!statusLine.empty()) {
        out << "\n" << statusLine;
    }
    return out.str();
}

} // anonymous namespace

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
            std::string kind = "text_file";
            if (a.kind == AttachmentInfo::Kind::Image)
                kind = "image";
            else if (a.kind == AttachmentInfo::Kind::PdfFile)
                kind = "pdf_file";
            else if (a.kind == AttachmentInfo::Kind::SpreadsheetFile)
                kind = "spreadsheet_file";
            obj->set("kind", kind);
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

// ── Phase 3c-ii: native sidecar fields ─────────────────────────

void ChatHistory::SetLastAssistantToolCalls(const std::string& toolCallsJson)
{
    if (toolCallsJson.empty()) return;
    if (m_messages.empty()) return;

    auto& last = m_messages.back();
    if (last->getValue<std::string>("role") != "assistant") return;

    // Parse the structured tool_calls payload exactly as it came
    // off the wire.  We store the JSON Array on the message object
    // so the request builder can splice it back into the next
    // outbound request without re-parsing.  On parse failure we
    // silently drop the field — defensive against a malformed
    // accumulator output; the assistant message remains as prose.
    try {
        Poco::JSON::Parser parser;
        auto var = parser.parse(toolCallsJson);
        Poco::JSON::Array::Ptr arr = var.extract<Poco::JSON::Array::Ptr>();
        if (arr && arr->size() > 0) {
            last->set("tool_calls", arr);
            m_dirty = true;
        }
    } catch (...) {
        // Drop silently — request builder treats this assistant
        // message as plain prose.
    }
}

void ChatHistory::AddToolResultMessage(const std::string& toolCallId,
                                        const std::string& content)
{
    auto msg = CreateMessage("user", content);
    if (!toolCallId.empty()) {
        // Sidecar field — read by BuildChatRequestJson when the
        // active protocol is Native, ignored under XML.  Both
        // protocols render the same way in chat (the [tool: NAME]
        // text in `content` drives display).
        msg->set("tool_call_id", toolCallId);
    }
    m_messages.push_back(msg);
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
    m_toolCwd.clear();
    m_toolTimeoutMs = 0;
    m_projectId.clear();
    m_projectName.clear();
    m_projectRoot.clear();
    m_chatApprovedTools.clear();
    m_chatApprovalTrustEnabled = false;
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

bool ChatHistory::HasPersistableContent() const
{
    // Historically LlamaBoss only persisted chats once they had at
    // least one message.  Projects Phase 1 adds durable metadata that
    // the user can set before typing a message, so allow metadata-only
    // conversations to be saved as well.  If an empty metadata-only
    // conversation has already been saved, HasFilePath() lets later
    // metadata clears persist too.
    return !m_messages.empty()
        || HasProject()
        || !m_toolCwd.empty()
        || m_toolTimeoutMs != 0
        || HasFilePath();
}

// ═══════════════════════════════════════════════════════════════════
//  API Request Builders
// ═══════════════════════════════════════════════════════════════════

std::string ChatHistory::BuildChatRequestJson(const std::string& model, bool stream,
                                               const std::string& systemPrompt,
                                               int contextTokens,
                                               const std::string& toolsArrayJson,
                                               bool nativeProtocol) const
{
    // ── Phase 3c-i: pre-parse the tools array once ──────────────
    // The caller passes an already-stringified JSON array (from
    // BuildToolsArrayJson).  We parse it here so it nests under
    // "tools" as a real JSON value rather than an embedded string.
    // On parse failure we silently drop the tools field — defensive
    // against a malformed schema slipping through; the request is
    // still valid (just no native function-calling for this turn).
    Poco::JSON::Array::Ptr toolsArr;
    if (!toolsArrayJson.empty()) {
        try {
            Poco::JSON::Parser p;
            auto var = p.parse(toolsArrayJson);
            toolsArr = var.extract<Poco::JSON::Array::Ptr>();
        } catch (...) {
            toolsArr.reset();
        }
    }

    // ── Phase 1: gather wire messages in original form ──────────
    // Build a parallel vector of message records so we can mutate
    // tool-result content in place during compaction and project
    // sidecar fields (tool_calls / tool_call_id) onto the wire JSON
    // when the active protocol is Native.  Poco::JSON::Array is
    // rebuilt from this at the end.  Cheaper than copying objects
    // after each elision.
    struct WireMsg {
        std::string role;
        std::string content;
        bool        isToolResult;     // cached — XML-formatted "[tool: NAME]" body
        // Phase 3c-ii sidecars; only consulted when nativeProtocol
        // is true.  Both empty for messages that didn't carry the
        // matching field on disk.
        std::string             toolCallId;     // user message answering an assistant call
        Poco::JSON::Array::Ptr  toolCalls;      // assistant message that emitted calls
    };
    std::vector<WireMsg> wire;
    wire.reserve(m_messages.size() + (systemPrompt.empty() ? 0 : 1));

    if (!systemPrompt.empty()) {
        wire.push_back({ "system", systemPrompt, false, "", nullptr });
    }
    for (const auto& msg : m_messages) {
        std::string role = msg->getValue<std::string>("role");
        std::string content = msg->has("content")
                                  ? msg->getValue<std::string>("content")
                                  : std::string();

        // Skip placeholder assistant messages that have no content
        // AND no tool_calls.  An assistant message with tool_calls
        // and empty content is valid (and required) under the
        // native protocol.
        const bool hasToolCalls = msg->has("tool_calls");
        if (content.empty() && !hasToolCalls) continue;

        WireMsg w;
        w.role         = std::move(role);
        w.content      = std::move(content);
        w.isToolResult = (w.role == "user") && IsToolResultMessage(w.content);

        if (msg->has("tool_call_id")) {
            try {
                w.toolCallId = msg->getValue<std::string>("tool_call_id");
            } catch (...) { /* leave empty */ }
        }
        if (hasToolCalls) {
            try {
                w.toolCalls = msg->getArray("tool_calls");
            } catch (...) { /* leave null */ }
        }

        wire.push_back(std::move(w));
    }

    // ── Phase 2: optional compaction ────────────────────────────
    // Only runs when the caller provided a context-window hint.
    // Measure the stringified body, and if it exceeds the budget,
    // elide tool-result bodies oldest-first until we're under.
    // The last `kMinPreservedResults` tool results are exempt —
    // that's the recent context the model needs to keep reasoning
    // coherently.
    auto stringifyWire = [&]() -> std::string {
        Poco::JSON::Object::Ptr root = new Poco::JSON::Object;
        root->set("model", model);
        root->set("stream", stream);

        Poco::JSON::Array::Ptr arr = new Poco::JSON::Array;
        for (const auto& w : wire) {
            Poco::JSON::Object::Ptr m = new Poco::JSON::Object;

            // Native-protocol projections of the sidecar fields:
            //
            //   * Assistant message + tool_calls sidecar  →  emit
            //     role:"assistant" with `tool_calls` array; OpenAI
            //     spec requires content to be null (or absent) when
            //     tool_calls is present.  We omit content entirely.
            //
            //   * User message + tool_call_id sidecar  →  emit
            //     role:"tool" with the id and the original tool-
            //     result text as content.  llama-server expects
            //     this exact shape on /v1/chat/completions.
            //
            //   * Anything else  →  emit as plain {role, content}.
            //
            // XML protocol takes only the third path for every
            // message — sidecar fields are ignored, the message
            // appears as ordinary user/assistant content.
            if (nativeProtocol && w.role == "assistant" && w.toolCalls) {
                m->set("role", w.role);
                if (!w.content.empty()) {
                    // Some templates (notably Hermes) tolerate a
                    // non-empty content alongside tool_calls and use
                    // it as the model's reasoning prose.  Pass it
                    // through when present; OpenAI spec allows null
                    // OR content here.
                    m->set("content", w.content);
                }
                m->set("tool_calls", w.toolCalls);
            }
            else if (nativeProtocol && w.role == "user" && !w.toolCallId.empty()) {
                m->set("role", std::string("tool"));
                m->set("tool_call_id", w.toolCallId);
                m->set("content", w.content);
            }
            else {
                m->set("role",    w.role);
                m->set("content", w.content);
            }

            arr->add(m);
        }
        root->set("messages", arr);

        // Phase 3c-i: tools field (Native protocol only).  Sits at
        // the root level alongside messages; the model sees the tool
        // catalog and may emit structured tool_calls in its
        // response.  3c-ii consumes those tool_calls.
        if (toolsArr) {
            root->set("tools", toolsArr);
        }

        std::ostringstream oss;
        Poco::JSON::Stringifier::stringify(root, oss);
        return oss.str();
    };

    std::string body = stringifyWire();

    if (contextTokens > 0) {
        const size_t budget = (size_t)((double)contextTokens
                                       * kBytesPerToken * kBudgetFraction);

        if (body.size() > budget) {
            // Count tool results so we know which ones to preserve.
            size_t totalToolResults = 0;
            for (const auto& w : wire) if (w.isToolResult) ++totalToolResults;

            // Elide oldest-first.  If there are more tool results
            // than the preserve count, the first (total - preserve)
            // are candidates; we stop elision early if we get under
            // budget before exhausting candidates.
            size_t candidatesRemaining = (totalToolResults > kMinPreservedResults)
                ? (totalToolResults - kMinPreservedResults)
                : 0;
            size_t toolResultsSeen = 0;

            for (size_t i = 0; i < wire.size() && candidatesRemaining > 0; ++i) {
                if (!wire[i].isToolResult) continue;
                ++toolResultsSeen;
                // Skip any we're preserving (the last kMinPreservedResults).
                if (toolResultsSeen > (totalToolResults - kMinPreservedResults)) break;

                wire[i].content = ElideToolResultBody(wire[i].content);
                --candidatesRemaining;

                // Rebuild + re-measure.  This is O(n² bytes) in the
                // worst case but n is small (~20 tool results in a
                // long chain) and the rebuild is pure string work.
                body = stringifyWire();
                if (body.size() <= budget) break;
            }
        }
    }

    // ── Phase 3 bugfix #4: native transcript sanitizer ───────
    // OpenAI/llama-server tool-call history is strict:
    //   assistant + tool_calls[id=A]
    //   role:"tool" + tool_call_id=A
    // must stay paired.  Save/reload, cancel, older Phase 3 bugs, or
    // partial multi-call execution can leave one side without the other.
    // Before returning a native request body, sanitize the projected
    // history so it never emits:
    //   * assistant.tool_calls with missing replies
    //   * role:"tool" replies with no valid preceding assistant call
    //   * empty assistant messages left behind after stripping tool_calls
    if (nativeProtocol) {
        std::vector<std::string> validToolReplyIds;

        for (size_t i = 0; i < wire.size(); ++i) {
            if (wire[i].role != "assistant" || !wire[i].toolCalls) continue;

            const std::vector<std::string> expected =
                ExtractToolCallIds(wire[i].toolCalls);

            // If the assistant has a malformed/empty tool_calls array,
            // strip it.  The assistant content, if any, remains as prose.
            if (expected.empty()) {
                wire[i].toolCalls.reset();
                continue;
            }

            // Native tool replies must appear immediately after the
            // assistant tool-call turn.  For Phase 3 we keep this strict
            // instead of searching arbitrarily far forward; if a normal
            // user/assistant message appears before the matching tool
            // replies, the old sidecar is no longer safe to project as
            // role:"tool".
            std::vector<std::string> matched;
            bool allMatched = true;

            for (size_t k = 0; k < expected.size(); ++k) {
                const size_t j = i + 1 + k;
                if (j >= wire.size()) {
                    allMatched = false;
                    break;
                }

                if (wire[j].role != "user" || wire[j].toolCallId.empty()) {
                    allMatched = false;
                    break;
                }

                const std::string& id = wire[j].toolCallId;
                if (!ContainsString(expected, id) || ContainsString(matched, id)) {
                    allMatched = false;
                    break;
                }

                matched.push_back(id);
            }

            if (!allMatched || matched.size() != expected.size()) {
                // Strip the assistant side.  A second pass below clears
                // any now-orphaned user.tool_call_id sidecars so those
                // messages fall back to ordinary user-visible tool blocks.
                wire[i].toolCalls.reset();
                continue;
            }

            for (const auto& id : matched) {
                if (!ContainsString(validToolReplyIds, id)) {
                    validToolReplyIds.push_back(id);
                }
            }
        }

        // Remove orphan tool_call_id sidecars.  The message content is
        // preserved, so the model still sees the tool result as a normal
        // user message instead of an invalid role:"tool" message.
        for (auto& w : wire) {
            if (w.role == "user" && !w.toolCallId.empty() &&
                !ContainsString(validToolReplyIds, w.toolCallId)) {
                w.toolCallId.clear();
            }
        }

        // If an assistant message had empty content and only invalid
        // tool_calls, stripping those calls leaves a pure placeholder.
        // Drop it from the wire request; otherwise llama-server receives
        // an empty assistant turn that adds no value and can confuse the
        // transcript around tool results.
        wire.erase(
            std::remove_if(wire.begin(), wire.end(), [](const WireMsg& w) {
                return w.role == "assistant" &&
                       w.content.find_first_not_of(" \t\r\n") == std::string::npos &&
                       !w.toolCalls;
            }),
            wire.end());

        // Re-stringify after sanitizing so the returned body reflects
        // the cleaned wire list.
        body = stringifyWire();
    }

    return body;
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
    if (!HasPersistableContent()) return false;

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

        // Tool execution context (Phase 3) — only written when the
        // user has customized them.  Older readers tolerate unknown
        // keys; newer readers tolerate missing keys.  No version bump.
        if (!m_toolCwd.empty()) {
            root->set("tool_cwd", m_toolCwd);
        }
        if (m_toolTimeoutMs != 0) {
            root->set("tool_timeout_ms", (Poco::UInt64)m_toolTimeoutMs);
        }

        // Optional long-lived project association (Projects Phase 1).
        // Older readers tolerate unknown keys; newer readers tolerate
        // missing keys.
        if (HasProject()) {
            root->set("project_id", m_projectId);
            root->set("project_name", m_projectName);
            root->set("project_root", m_projectRoot);
        }

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
            // Save messages that have visible content OR native
            // function-calling sidecars.  Native assistant tool-call
            // turns commonly have content == "" and tool_calls != [].
            // Skipping them would leave their following tool result
            // orphaned after reload.  Still skip true empty placeholders.
            std::string content = msg->has("content")
                                      ? msg->getValue<std::string>("content")
                                      : std::string();
            const bool hasToolCalls  = msg->has("tool_calls");
            const bool hasToolCallId = msg->has("tool_call_id");
            if (!content.empty() || hasToolCalls || hasToolCallId) {
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

                // Phase 3c-ii: persist native function-calling
                // sidecars so a saved conversation reloads with its
                // structured tool_calls intact.  Both are protocol-
                // independent on disk; BuildChatRequestJson decides
                // at request time whether to project them onto the
                // wire.  An XML-protocol model loading a saved
                // conversation that has these fields just ignores
                // them — they don't perturb display or wire shape
                // when nativeProtocol=false.
                if (msg->has("tool_call_id")) {
                    saveMsg->set("tool_call_id",
                                 msg->getValue<std::string>("tool_call_id"));
                }
                if (msg->has("tool_calls")) {
                    saveMsg->set("tool_calls", msg->getArray("tool_calls"));
                }
                messagesArray->add(saveMsg);
            }
        }
        root->set("messages", messagesArray);

        // Write to file with pretty formatting
        std::ostringstream oss;
        Poco::JSON::Stringifier::stringify(root, oss, 2);

        // ── Atomic save via staged temp + rename ─────────────────
        // Mirrors tool_write/tool_edit's crash-safe pattern.  An
        // ofstream(...trunc) directly to savePath leaves a window
        // where a crash, power loss, or exception during write
        // truncates the conversation file to whatever was flushed.
        // Auto-save runs after every assistant turn, so that
        // window is hit constantly.  Writing to a sibling temp
        // and then MoveFileExW with REPLACE_EXISTING means the
        // user either sees the previous successful save or the
        // new save — never a half-written file.
        const std::string body = oss.str();

        tool_staged_write::StagedTempFile tmp =
            tool_staged_write::CreateStagedTempFile(savePath);
        if (tmp.handle == INVALID_HANDLE_VALUE) return false;

        {
            const char* data   = body.data();
            size_t      remain = body.size();
            while (remain > 0) {
                DWORD chunk = (remain > 0x40000000U)
                                  ? 0x40000000U
                                  : static_cast<DWORD>(remain);
                DWORD written = 0;
                BOOL  ok = ::WriteFile(tmp.handle, data, chunk, &written, nullptr);
                if (!ok || written == 0) {
                    ::CloseHandle(tmp.handle);
                    ::DeleteFileW(tmp.wPath.c_str());
                    return false;
                }
                data   += written;
                remain -= written;
            }
        }

        if (!::CloseHandle(tmp.handle)) {
            ::DeleteFileW(tmp.wPath.c_str());
            return false;
        }

        std::wstring wFinal = path_safety::Utf8ToWide(savePath);
        if (wFinal.empty()) {
            ::DeleteFileW(tmp.wPath.c_str());
            return false;
        }

        if (!::MoveFileExW(tmp.wPath.c_str(), wFinal.c_str(),
                           MOVEFILE_REPLACE_EXISTING |
                           MOVEFILE_WRITE_THROUGH)) {
            ::DeleteFileW(tmp.wPath.c_str());
            return false;
        }

        m_filePath = savePath;
        m_dirty = false;

        // Update the human-readable marker inside the per-conversation
        // workflow folder so users browsing %USERPROFILE%\LlamaBoss\Workflows
        // can identify which chat_xxxxxxxx belongs to which conversation.
        // Only written when the workflow folder already exists — text-only
        // chats that never invoked tools or attachments don't grow a
        // workflow folder just from saving.
        try {
            std::string workflowDir = GetWorkflowDir(savePath);
            if (!workflowDir.empty()
                && wxDirExists(wxString::FromUTF8(workflowDir)))
            {
                std::string markerPath =
                    JoinWorkflowPath(workflowDir, "_title.txt");
                std::ofstream marker(
                    path_safety::Utf8ToWide(markerPath),
                    std::ios::out | std::ios::trunc);
                if (marker.is_open()) {
                    marker << m_title         << "\n\n"
                           << "Created: " << m_createdAt << "\n"
                           << "Updated: " << m_updatedAt << "\n";
                }
            }
        } catch (...) {
            // Marker is purely cosmetic for filesystem browsing — never
            // let a failure here propagate up and fail the save.
        }

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
        std::ifstream file(path_safety::Utf8ToWide(filePath));
        if (!file.is_open()) return false;

        std::string content((std::istreambuf_iterator<char>(file)),
            std::istreambuf_iterator<char>());
        file.close();

        if (content.empty()) return false;

        Poco::JSON::Parser parser;
        auto result = parser.parse(content);
        auto root = result.extract<Poco::JSON::Object::Ptr>();

        // Per-chat approval choices are intentionally in-memory only.
        // If this ChatHistory instance is reused for a load, do not
        // carry approvals from the previous conversation.
        m_chatApprovedTools.clear();
        m_chatApprovalTrustEnabled = false;

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

        // Tool execution context (Phase 3) — default-initialized to
        // empty/0, overwritten only if the key is present in the file.
        m_toolCwd.clear();
        m_toolTimeoutMs = 0;
        if (root->has("tool_cwd")) {
            m_toolCwd = root->getValue<std::string>("tool_cwd");
        }
        if (root->has("tool_timeout_ms")) {
            m_toolTimeoutMs =
                (unsigned long)root->getValue<Poco::UInt64>("tool_timeout_ms");
        }

        // Optional long-lived project association (Projects Phase 1).
        m_projectId.clear();
        m_projectName.clear();
        m_projectRoot.clear();
        if (root->has("project_id")) {
            m_projectId = root->getValue<std::string>("project_id");
        }
        if (root->has("project_name")) {
            m_projectName = root->getValue<std::string>("project_name");
        }
        if (root->has("project_root")) {
            m_projectRoot = root->getValue<std::string>("project_root");
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
                std::string msgContent = msgObj->has("content")
                                             ? msgObj->getValue<std::string>("content")
                                             : std::string();
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

                // Phase 3c-ii: restore native function-calling
                // sidecars.  Absent in pre-3c-ii files, harmless on
                // restored XML conversations (BuildChatRequestJson
                // ignores them when nativeProtocol=false).
                if (msgObj->has("tool_call_id")) {
                    loadedMsg->set("tool_call_id",
                                   msgObj->getValue<std::string>("tool_call_id"));
                }
                if (msgObj->has("tool_calls")) {
                    loadedMsg->set("tool_calls", msgObj->getArray("tool_calls"));
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
    char sep = (char)wxFileName::GetPathSeparator();

    // 8 hex chars gives ~4B namespace — collisions are unlikely but
    // not impossible as the conversation count grows. Retry on any
    // existing-path hit; a silent overwrite would be catastrophic.
    for (int attempt = 0; attempt < 10; ++attempt) {
        std::string uuid = Poco::UUIDGenerator::defaultGenerator().createRandom().toString();
        std::string shortId = uuid.substr(0, 8);
        std::string path = dir + std::string(1, sep) + "chat_" + shortId + ".json";
        if (!wxFileExists(wxString::FromUTF8(path)))
            return path;
    }

    // Extremely unlikely: fall back to the full UUID for maximum entropy.
    std::string fullUuid = Poco::UUIDGenerator::defaultGenerator().createRandom().toString();
    return dir + std::string(1, sep) + "chat_" + fullUuid + ".json";
}

// ═══════════════════════════════════════════════════════════════════
//  Per-conversation workflow helpers
// ═══════════════════════════════════════════════════════════════════

static std::string JoinWorkflowPath(const std::string& a, const std::string& b)
{
    if (a.empty()) return b;
    if (a.back() == '/' || a.back() == '\\') return a + b;
    return a + std::string(1, wxFILE_SEP_PATH) + b;
}

static std::string LlamaBossUserRootDir()
{
#ifdef __WXMSW__
    wxString userProfile;
    if (wxGetEnv("USERPROFILE", &userProfile) && !userProfile.IsEmpty()) {
        return JoinWorkflowPath(std::string(userProfile.ToUTF8().data()), "LlamaBoss");
    }
#endif

    wxString home = wxGetHomeDir();
    if (!home.IsEmpty()) {
        return JoinWorkflowPath(std::string(home.ToUTF8().data()), "LlamaBoss");
    }

    wxString docs = wxStandardPaths::Get().GetDocumentsDir();
    return JoinWorkflowPath(std::string(docs.ToUTF8().data()), "LlamaBoss");
}

static std::string ConversationStemFromPath(const std::string& conversationPath)
{
    wxFileName fn(wxString::FromUTF8(conversationPath));
    std::string stem(fn.GetName().ToUTF8().data());
    if (stem.empty()) {
        // Last-resort fallback: callers should normally ensure the
        // conversation has a generated chat_xxxxxxxx path before asking
        // for a workflow folder.
        stem = "chat_unsaved";
    }
    return stem;
}

std::string ChatHistory::GetWorkflowsDir()
{
    std::string dir = JoinWorkflowPath(LlamaBossUserRootDir(), "Workflows");
    wxFileName::Mkdir(wxString::FromUTF8(dir), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    return dir;
}

std::string ChatHistory::GetWorkflowDir(const std::string& conversationPath)
{
    return JoinWorkflowPath(GetWorkflowsDir(), ConversationStemFromPath(conversationPath));
}

std::string ChatHistory::GetConversationWorkspaceDir(const std::string& conversationPath)
{
    return JoinWorkflowPath(GetWorkflowDir(conversationPath), "Workspace");
}

bool ChatHistory::EnsureWorkflowDir(const std::string& conversationPath)
{
    std::string root = GetWorkflowDir(conversationPath);
    if (root.empty()) return false;

    // Only the chat_xxxxxxxx root is created eagerly.  Lane subfolders
    // (attachments/, artifacts/, Workspace/, etc.) are created on demand
    // by the code that actually writes into them — see SaveImagesToDisk
    // for attachments, ChatDisplay's file persistence for artifacts, and
    // ResolveCurrentCwd for Workspace.  Pre-creating nine empty lanes
    // for every conversation polluted the user-visible Workflows root
    // with hundreds of empty subfolders for chats that never used them.
    bool ok = wxFileName::Mkdir(wxString::FromUTF8(root),
                                wxS_DIR_DEFAULT,
                                wxPATH_MKDIR_FULL);
    if (!ok && !wxDirExists(wxString::FromUTF8(root))) return false;

    return true;
}

// ═══════════════════════════════════════════════════════════════════
//  Attachment / generated-file workflow helpers
// ═══════════════════════════════════════════════════════════════════

std::string ChatHistory::GetAttachmentDir(const std::string& conversationPath)
{
    return JoinWorkflowPath(GetWorkflowDir(conversationPath), "attachments");
}

std::string ChatHistory::GetAttachmentRelDir(const std::string& /*conversationPath*/)
{
    // Relative to the workflow root.  Forward slashes for JSON portability.
    return "attachments";
}

std::string ChatHistory::GetGeneratedFilesDir(const std::string& conversationPath)
{
    return JoinWorkflowPath(GetWorkflowDir(conversationPath), "artifacts");
}

std::string ChatHistory::GetGeneratedFilesRelDir(const std::string& /*conversationPath*/)
{
    return "artifacts";
}

std::string ChatHistory::CurrentTimestamp()
{
    Poco::Timestamp now;
    return Poco::DateTimeFormatter::format(now, Poco::DateTimeFormat::ISO8601_FORMAT);
}

// ── Tool-result formatting (Phase 3) ─────────────────────────────

// ─── Unified tool-block formatter (Phase 3) ────────────────────
//
// Produces the canonical history-round-trip form for any tool
// invocation.  Dynamic-length backtick fences keep body and errorBody
// safe from collisions with content that itself contains ```.
std::string ChatHistory::FormatToolBlockAsUserMessage(
    const std::string& toolTag,
    const std::string& commandEcho,
    const std::string& body,
    const std::string& errorBody,
    const std::vector<std::string>& statusChips,
    const std::string& bodyLang)
{
    // Longest contiguous run of backticks in `s` — used to pick a
    // fence longer than anything that can appear inside the body.
    auto longestBacktickRun = [](const std::string& s) -> size_t {
        size_t maxRun = 0, cur = 0;
        for (char c : s) {
            if (c == '`') { ++cur; if (cur > maxRun) maxRun = cur; }
            else          { cur = 0; }
        }
        return maxRun;
    };

    size_t n = std::max(longestBacktickRun(body),
                        longestBacktickRun(errorBody));
    size_t fenceLen = std::max<size_t>(3, n + 1);
    const std::string fence(fenceLen, '`');

    std::ostringstream ss;
    ss << "[tool: " << toolTag << "]\n"
       << "> " << commandEcho << "\n";

    // Primary body — monospace fenced block with optional language hint.
    if (!body.empty()) {
        ss << "\n" << fence << bodyLang << "\n"
           << body;
        if (body.back() != '\n') ss << "\n";
        ss << fence << "\n";
    } else if (errorBody.empty()) {
        // Explicit blank-output marker for the model context.  Without
        // this, small models sometimes reuse stale output from an older
        // command when a successful command returns nothing.
        ss << "\n[output]\n(no output)\n";
    }

    // Error body — same fence style, labelled separately so the model
    // can distinguish tool failure from normal output.
    if (!errorBody.empty()) {
        ss << "\n[error]\n" << fence << "\n"
           << errorBody;
        if (errorBody.back() != '\n') ss << "\n";
        ss << fence << "\n";
    }

    // Status chips — comma-joined, always emitted (even if empty, so
    // the closing bracket marks the end of the block unambiguously).
    ss << "\n[status: ";
    for (size_t i = 0; i < statusChips.size(); ++i) {
        if (i > 0) ss << ", ";
        ss << statusChips[i];
    }
    ss << "]";

    return ss.str();
}
