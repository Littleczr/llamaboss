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
#include <Poco/Types.h>
#include <Poco/Base64Encoder.h>   // image attachment wire projection
#include <Poco/FileStream.h>      // UTF-8-safe binary reads on Windows
#include <Poco/StreamCopier.h>    // file → base64 encoder pump
#include <Poco/File.h>            // stat (size/mtime) for the data-URI cache

// wxWidgets for paths and file system
#include <wx/stdpaths.h>
#include <wx/filename.h>
#include <wx/utils.h>

#include <sstream>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <utility>
#include <unordered_map>   // data-URI cache
#include <mutex>           // data-URI cache guard

// File format version: 9 adds durable awaiting-user prompt/reply checkpoint
// evidence for resumed waiting goals.
static const int CONVERSATION_FORMAT_VERSION = 9;

// Forward declaration — defined further down with the other workflow
// helpers.  Hoisted here so methods earlier in the file (notably
// SaveToFile, which writes the per-conversation _title.txt marker) can
// call it without reordering the entire helpers block.
static std::string JoinWorkflowPath(const std::string& a, const std::string& b);

namespace {

constexpr const char* kSessionContextPrefix = "[Session context:";

bool IsLegacySessionContextTitle(const std::string& title)
{
    return title.rfind(kSessionContextPrefix, 0) == 0;
}

// The wire copy of each user turn begins with a timestamp header for model
// grounding.  That header is intentionally persisted, but it is not authored
// by the user and must never become the conversation title.
std::string StripSessionContextHeader(std::string content)
{
    if (!IsLegacySessionContextTitle(content)) return content;

    const size_t paragraphBreak = content.find("\n\n");
    if (paragraphBreak != std::string::npos)
        return content.substr(paragraphBreak + 2);

    // Defensive fallback for older/normalized files where line breaks may
    // have been collapsed.  The injected header always closes with ']'.
    const size_t closeBracket = content.find(']');
    if (closeBracket != std::string::npos) {
        size_t start = closeBracket + 1;
        while (start < content.size() &&
               std::isspace(static_cast<unsigned char>(content[start]))) {
            ++start;
        }
        return content.substr(start);
    }

    return content;
}

std::string PersistedGoalStatus(GoalStatus status)
{
    switch (status) {
    case GoalStatus::None:          return "none";
    case GoalStatus::Active:        return "active";
    case GoalStatus::Paused:        return "paused";
    case GoalStatus::AwaitingUser:  return "awaiting_user";
    case GoalStatus::Completed:     return "completed";
    case GoalStatus::Cancelled:     return "cancelled";
    case GoalStatus::Failed:        return "failed";
    case GoalStatus::BudgetReached: return "budget_reached";
    }
    return "none";
}

GoalStatus GoalStatusFromPersisted(const std::string& status)
{
    if (status == "active")         return GoalStatus::Active;
    if (status == "paused")         return GoalStatus::Paused;
    if (status == "awaiting_user" ||
        status == "awaiting user")  return GoalStatus::AwaitingUser;
    if (status == "completed")      return GoalStatus::Completed;
    if (status == "cancelled")      return GoalStatus::Cancelled;
    if (status == "failed")         return GoalStatus::Failed;
    if (status == "budget_reached" ||
        status == "budget reached") return GoalStatus::BudgetReached;
    return GoalStatus::None;
}

std::string PersistedGoalContractStatus(GoalContractStatus status)
{
    switch (status) {
    case GoalContractStatus::None:     return "none";
    case GoalContractStatus::Drafting: return "drafting";
    case GoalContractStatus::Ready:    return "ready";
    case GoalContractStatus::Failed:   return "failed";
    }
    return "none";
}

GoalContractStatus GoalContractStatusFromPersisted(const std::string& status)
{
    if (status == "drafting") return GoalContractStatus::Drafting;
    if (status == "ready")    return GoalContractStatus::Ready;
    if (status == "failed")   return GoalContractStatus::Failed;
    return GoalContractStatus::None;
}

Poco::JSON::Array::Ptr StringVectorToJsonArray(const std::vector<std::string>& items)
{
    Poco::JSON::Array::Ptr arr = new Poco::JSON::Array;
    for (const auto& item : items)
        arr->add(item);
    return arr;
}

std::vector<std::string> JsonArrayToStringVector(const Poco::JSON::Array::Ptr& arr)
{
    std::vector<std::string> out;
    if (!arr) return out;

    out.reserve(arr->size());
    for (size_t i = 0; i < arr->size(); ++i) {
        out.push_back(arr->get(i).convert<std::string>());
    }
    return out;
}

int JsonIntOrDefault(const Poco::JSON::Object::Ptr& obj,
                     const std::string& key,
                     int fallback = 0)
{
    if (!obj || !obj->has(key)) return fallback;
    return obj->getValue<int>(key);
}

bool EndsWithAsciiNoCase(const std::string& value, const std::string& suffix)
{
    if (suffix.size() > value.size()) return false;

    const size_t offset = value.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i) {
        const unsigned char a = static_cast<unsigned char>(value[offset + i]);
        const unsigned char b = static_cast<unsigned char>(suffix[i]);
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return true;
}

bool LooksLikeLocalGgufModel(const std::string& model)
{
    // Local LlamaBoss sends the loaded GGUF filesystem path as the wire
    // "model" value. Remote providers receive ids like
    // "anthropic/claude..." or "openai/gpt..." and must not be sent
    // llama.cpp-only sampler extensions such as top_k/min_p.
    return EndsWithAsciiNoCase(model, ".gguf");
}

void ApplyLocalLlamaSampling(Poco::JSON::Object::Ptr root,
                             const std::string& model,
                             bool agentSamplingProfile)
{
    if (!LooksLikeLocalGgufModel(model)) return;

    // Keep defaults explicit instead of inheriting llama-server's more
    // creative defaults.  Normal chat stays flexible; agent/goal/skill
    // builder turns run cooler for more stable JSON/tool/code output.
    root->set("temperature", agentSamplingProfile ? 0.4 : 0.6);
    root->set("top_p", 0.95);
    root->set("top_k", 40);
    root->set("min_p", 0.05);
}

// ─── Image attachment wire projection ────────────────────────────
//
// Reads a persisted attachment image from disk and returns it as an
// OpenAI-style image_url data URI ("" on any failure — a missing or
// unreadable file must degrade to a text-only message, never fail
// the whole request build).  Poco::FileInputStream handles UTF-8
// paths correctly on Windows; std::ifstream would not.  Base64 is
// emitted without line wrapping — Poco inserts a newline every 72
// chars by default and providers reject wrapped payloads.
std::string LoadImageAsDataUri(const std::string& absPath,
                               const std::string& mimeType)
{
    std::ostringstream b64;
    try {
        Poco::FileInputStream in(absPath);
        Poco::Base64Encoder enc(b64);
        enc.rdbuf()->setLineLength(0);
        Poco::StreamCopier::copyStream(in, enc);
        enc.close();   // flush base64 padding
    } catch (...) {
        return std::string();
    }
    if (b64.str().empty()) return std::string();

    const std::string mime = mimeType.empty()
        ? std::string("image/png") : mimeType;
    return "data:" + mime + ";base64," + b64.str();
}

// ─── Data-URI cache ──────────────────────────────────────────────
//
// BuildChatRequestJson rebuilds the wire request on every agent
// iteration, and the image-carrier message re-reads and re-encodes
// its persisted images each time — a multi-MB read plus a ~4/3-size
// base64 string allocation per step, for files that never change
// once written into the conversation's workflow folder.  Cache the
// finished data URI keyed by absolute path, validated by (size,
// mtime, mime): any rewrite of the file — or a future path that
// reuses a name — misses cleanly and re-encodes.
//
// Bounded at 64 MiB of stored URI bytes with LRU eviction, so a
// conversation cycling through many large images degrades to the
// old per-request encode instead of holding every image in memory
// forever.  An entry larger than the whole cap is served uncached.
// Mutex-guarded: builds run on the UI thread today, but the guard
// costs nothing and keeps this correct if a background builder ever
// appears.
namespace {

struct CachedImageUri {
    Poco::Timestamp mtime;
    Poco::File::FileSize size = 0;
    std::string     mime;
    std::string     uri;
    Poco::UInt64    lastUsed = 0;   // LRU tick
};

std::mutex                                        g_imageUriCacheMutex;
std::unordered_map<std::string, CachedImageUri>   g_imageUriCache;
size_t                                            g_imageUriCacheBytes = 0;
Poco::UInt64                                      g_imageUriCacheTick  = 0;

constexpr size_t kImageUriCacheCapBytes = 64ull * 1024 * 1024;

} // namespace

static std::string LoadImageAsDataUriCached(const std::string& absPath,
                                            const std::string& mimeType)
{
    Poco::Timestamp      mtime;
    Poco::File::FileSize size = 0;
    try {
        Poco::File f(absPath);
        if (!f.exists()) return std::string();
        mtime = f.getLastModified();
        size  = f.getSize();
    } catch (...) {
        // Stat failed — path gone or unreadable.  The uncached loader
        // would fail identically; skip the read attempt.
        return std::string();
    }

    {
        std::lock_guard<std::mutex> lock(g_imageUriCacheMutex);
        auto it = g_imageUriCache.find(absPath);
        if (it != g_imageUriCache.end() &&
            it->second.mtime == mtime &&
            it->second.size  == size &&
            it->second.mime  == mimeType) {
            it->second.lastUsed = ++g_imageUriCacheTick;
            return it->second.uri;
        }
    }

    // Miss (or stale) — encode outside the lock; a multi-MB base64
    // pass must never serialize other builders behind it.
    std::string uri = LoadImageAsDataUri(absPath, mimeType);
    if (uri.empty()) return uri;

    if (uri.size() <= kImageUriCacheCapBytes) {
        std::lock_guard<std::mutex> lock(g_imageUriCacheMutex);

        // Replace any stale entry for this path first so its bytes
        // are not double-counted against the cap.
        auto it = g_imageUriCache.find(absPath);
        if (it != g_imageUriCache.end()) {
            g_imageUriCacheBytes -= it->second.uri.size();
            g_imageUriCache.erase(it);
        }

        // LRU-evict until the new entry fits.
        while (g_imageUriCacheBytes + uri.size() > kImageUriCacheCapBytes &&
               !g_imageUriCache.empty()) {
            auto lru = g_imageUriCache.begin();
            for (auto e = g_imageUriCache.begin();
                 e != g_imageUriCache.end(); ++e) {
                if (e->second.lastUsed < lru->second.lastUsed) lru = e;
            }
            g_imageUriCacheBytes -= lru->second.uri.size();
            g_imageUriCache.erase(lru);
        }

        CachedImageUri entry;
        entry.mtime    = mtime;
        entry.size     = size;
        entry.mime     = mimeType;
        entry.uri      = uri;
        entry.lastUsed = ++g_imageUriCacheTick;
        g_imageUriCacheBytes += uri.size();
        g_imageUriCache.emplace(absPath, std::move(entry));
    }

    return uri;
}

// ─── Reasoning stripping ─────────────────────────────────────────
//
// Model thinking is display/storage-only: it renders once in the
// collapsed block and persists in the conversation file, but it is
// never resent on the wire.  Resending burns context (the meter
// reclaims it automatically since counting happens on the stripped
// wire copy) and deviates from what reasoning-model templates
// expect — llama-server's own webui and the DeepSeek/OpenAI APIs
// all drop prior-turn reasoning.
//
// Handles multiple blocks per message and an unterminated open tag
// (a turn cancelled mid-thinking stores "<think>partial…" with no
// close — dropped to end of content).  When anything was stripped,
// leading whitespace left behind by the "</think>\n" separator is
// trimmed so the wire content starts at the first visible byte.
std::string StripThinkBlocks(const std::string& content)
{
    static const std::string kOpen  = "<think>";
    static const std::string kClose = "</think>";

    std::string out = content;
    for (;;) {
        const size_t open = out.find(kOpen);
        if (open == std::string::npos) break;

        const size_t close = out.find(kClose, open + kOpen.size());
        if (close == std::string::npos) {
            out.erase(open);          // unterminated — drop to end
            break;
        }
        out.erase(open, close + kClose.size() - open);
    }

    if (out.size() != content.size()) {
        const size_t first = out.find_first_not_of(" \t\r\n");
        out.erase(0, first == std::string::npos ? out.size() : first);
    }
    return out;
}

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

    // ── Error summary preservation ───────────────────────────────
    // For FAILED results, the error text is usually the load-bearing
    // context for why the model changed approach afterwards.  Eliding
    // it makes the surviving transcript read like the model abandoned
    // a working plan for no reason — and invites re-trying the failed
    // call.  Keep a bounded head of the [error] section.
    //
    // FormatToolBlockAsUserMessage emits the section as:
    //   \n[error]\n<fence>\n<errorBody...><fence>\n
    // where <fence> is a backtick run on its own line.  Parse by
    // locating the marker, taking the next line as the fence, and
    // capturing until a line equal to that fence.
    std::string errorSummary;
    {
        constexpr const char* kErrMarker = "\n[error]\n";
        constexpr size_t kErrMarkerLen   = 9;   // strlen("\n[error]\n")
        constexpr size_t kMaxErrorSummaryBytes = 480;
        constexpr int    kMaxErrorSummaryLines = 6;

        size_t errPos = content.find(kErrMarker);
        if (errPos != std::string::npos) {
            size_t fenceStart = errPos + kErrMarkerLen;
            size_t fenceEnd   = content.find('\n', fenceStart);
            if (fenceEnd != std::string::npos && fenceEnd > fenceStart) {
                std::string fence = content.substr(fenceStart,
                                                   fenceEnd - fenceStart);
                bool fenceOk = !fence.empty();
                for (char c : fence) if (c != '`') { fenceOk = false; break; }

                if (fenceOk) {
                    size_t bodyStart = fenceEnd + 1;
                    size_t closePos  = content.find("\n" + fence + "\n",
                                                    bodyStart);
                    size_t bodyEnd = (closePos != std::string::npos)
                                         ? closePos
                                         : content.size();

                    std::string err = content.substr(bodyStart,
                                                     bodyEnd - bodyStart);

                    // Bound by lines, then by bytes (UTF-8 safe cut).
                    int    lines = 0;
                    size_t cut   = err.size();
                    for (size_t i = 0; i < err.size(); ++i) {
                        if (err[i] == '\n' &&
                            ++lines >= kMaxErrorSummaryLines) {
                            cut = i;
                            break;
                        }
                    }
                    bool truncated = cut < err.size();
                    if (cut > kMaxErrorSummaryBytes) {
                        cut = kMaxErrorSummaryBytes;
                        while (cut > 0 &&
                               (static_cast<unsigned char>(err[cut]) & 0xC0)
                                   == 0x80) {
                            --cut;
                        }
                        truncated = true;
                    }
                    errorSummary = err.substr(0, cut);
                    // Trim trailing whitespace so the marker reads tight.
                    while (!errorSummary.empty() &&
                           (errorSummary.back() == ' '  ||
                            errorSummary.back() == '\t' ||
                            errorSummary.back() == '\r' ||
                            errorSummary.back() == '\n')) {
                        errorSummary.pop_back();
                    }
                    if (truncated && !errorSummary.empty()) {
                        errorSummary += "\n... [error truncated]";
                    }
                }
            }
        }
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

    if (!errorSummary.empty()) {
        out << "\n[error summary preserved from the elided result]\n"
            << errorSummary << "\n";
    }

    if (!statusLine.empty()) {
        out << "\n" << statusLine;
    }
    return out.str();
}

std::string StripAgentStepTrailer(const std::string& content)
{
    constexpr const char* kMarker = "\n\n[agent tool step ";
    const size_t pos = content.rfind(kMarker);
    if (pos == std::string::npos) return content;

    const size_t close = content.find(']', pos + 2);
    if (close == std::string::npos) return content;

    // The trailer is always appended at the end of a formatted tool result.
    // It may optionally be followed by the one-sentence budget warning; both
    // are live-loop guidance and become misleading once older turns replay.
    return content.substr(0, pos);
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
            else if (a.kind == AttachmentInfo::Kind::ZipFile)
                kind = "zip_file";
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
    MarkDirty();
}

void ChatHistory::AddAssistantMessage(const std::string& content, const std::string& model)
{
    m_messages.push_back(CreateMessage("assistant", content, model));
    MarkDirty();
}

void ChatHistory::AddSystemMessage(const std::string& content)
{
    m_messages.push_back(CreateMessage("system", content));
    MarkDirty();
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
            MarkDirty();
        }
    } catch (...) {
        // Drop silently — request builder treats this assistant
        // message as plain prose.
    }
}

void ChatHistory::SetLastAssistantImages(const std::vector<std::string>& relPaths)
{
    if (relPaths.empty()) return;
    if (m_messages.empty()) return;

    auto& last = m_messages.back();
    try {
        if (last->getValue<std::string>("role") != "assistant") return;
    } catch (...) { return; }

    Poco::JSON::Array::Ptr arr = new Poco::JSON::Array;
    for (const auto& p : relPaths) {
        if (!p.empty()) arr->add(p);
    }
    if (arr->size() > 0) {
        last->set("images", arr);
        MarkDirty();
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
    MarkDirty();
}

void ChatHistory::Clear()
{
    m_messages.clear();
    m_streamBuffer.clear();
    m_streamBufferDirty = 0;
    m_filePath.clear();
    m_title.clear();
    m_createdAt.clear();
    m_updatedAt.clear();
    m_toolCwd.clear();
    m_toolTimeoutMs = 0;
    m_thinkOverride = ThinkOverride::Auto;
    m_projectId.clear();
    m_projectName.clear();
    m_projectRoot.clear();
    m_goalState.Reset();
    m_chatApprovedTools.clear();
    m_chatApprovalTrustEnabled = false;
    m_dirty = false;
    m_revision = 0;
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
        || m_goalState.HasGoal()
        || !m_toolCwd.empty()
        || m_toolTimeoutMs != 0
        || HasFilePath();
}

// ═══════════════════════════════════════════════════════════════════
//  API Request Builders
// ═══════════════════════════════════════════════════════════════════

// ── Context meter support ────────────────────────────────────────

size_t ChatHistory::EstimateTokensFromBytes(size_t bytes)
{
    // Same constant the elision budget uses (kBytesPerToken above), so
    // the meter and the request builder agree by construction.
    return (size_t)((double)bytes / kBytesPerToken);
}

double ChatHistory::ElisionBudgetFraction()
{
    return kBudgetFraction;
}

size_t ChatHistory::EstimateHistoryTokens() const
{
    size_t bytes = 0;
    for (const auto& msg : m_messages) {
        if (!msg) continue;
        try {
            if (msg->has("content") && !msg->isNull("content"))
                bytes += msg->getValue<std::string>("content").size();
        } catch (...) { /* non-string content — skip */ }
        if (msg->has("tool_calls")) bytes += 200;  // sidecar overhead, rough
        bytes += 40;                               // per-message JSON + role
    }
    // Include content still sitting in the streaming buffer that hasn't
    // been synced to the JSON object yet (sub-4KiB partial replies).
    bytes += m_streamBuffer.size();
    return EstimateTokensFromBytes(bytes);
}

std::string ChatHistory::BuildChatRequestJson(const std::string& model, bool stream,
                                               const std::string& systemPrompt,
                                               int contextTokens,
                                               const std::string& toolsArrayJson,
                                               bool nativeProtocol,
                                               bool agentSamplingProfile,
                                               bool imageOutput)
{
    // Context meter: fresh count for this build; incremented by the
    // elision pass below when tool-result bodies are actually dropped.
    m_lastBuildElidedCount = 0;

    // Make sure any in-flight streamed content is reflected in the JSON
    // objects before we build the wire request.  AppendToLastAssistantMessage
    // amortizes the sync; this is the read-side counterpart.
    FlushStreamBuffer();

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
        // Image-projection sidecars.  attachMeta is the message's
        // stored attachments array (kind/mime_type/storage_path);
        // isImageCarrier marks the single message whose persisted
        // images ride the wire this request.  A flag on the element
        // (not an index) so the elision/sanitizer phases that mutate
        // or remove wire entries can never orphan the selection.
        Poco::JSON::Array::Ptr  attachMeta;
        bool                    isImageCarrier = false;
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

        // Reasoning never goes back on the wire (see StripThinkBlocks).
        // Applied here — before the blank-content skip, elision, and
        // token counting — so all downstream phases see the wire-true
        // content and a thinking-only assistant turn drops out entirely
        // (its native tool_calls sidecar, when present, still survives
        // via the hasToolCalls path below).
        if (role == "assistant")
            content = StripThinkBlocks(content);

        // Skip placeholder assistant messages that have no visible content.
        // An assistant message with tool_calls and empty content is valid
        // only when projecting a Native tool-calling transcript.  Under the
        // XML/plain protocol, sidecars are ignored, so keeping that row would
        // put an empty assistant turn on the wire after model switching.
        const bool hasToolCalls = msg->has("tool_calls");
        const bool blankContent =
            content.find_first_not_of(" \t\r\n") == std::string::npos;
        if (blankContent && !(nativeProtocol && hasToolCalls)) continue;

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
        if (w.role == "user" && !w.isToolResult && msg->has("attachments")) {
            try {
                w.attachMeta = msg->getArray("attachments");
            } catch (...) { /* leave null */ }
        }

        wire.push_back(std::move(w));
    }

    // ── Phase 1b: strip stale live-loop budget trailers ─────────
    // AgentController appends a model-facing "[agent tool step N of 12]"
    // trailer to the newest counted tool result while a loop is actively
    // iterating.  That trailer is useful for the immediate follow-up request,
    // but stale trailers from older turns become misleading instructions.
    // Keep it only when the latest non-system wire message is itself a tool
    // result; otherwise strip it from every tool result before request build.
    size_t lastNonSystemIndex = wire.size();
    for (size_t i = wire.size(); i-- > 0;) {
        if (wire[i].role != "system") {
            lastNonSystemIndex = i;
            break;
        }
    }

    for (size_t i = 0; i < wire.size(); ++i) {
        if (!wire[i].isToolResult) continue;
        if (i == lastNonSystemIndex) continue;
        wire[i].content = StripAgentStepTrailer(wire[i].content);
    }

    // ── Phase 1c: image carrier selection ────────────────────────
    // User-attached images are persisted to the conversation's
    // attachments sidecar folder, but historically rode the wire only
    // in the single request built right after attach (via
    // AttachmentManager::InjectImagesIntoRequest).  Any rebuild —
    // every agent-loop iteration, or a follow-up turn — silently
    // dropped them, so vision models (Grok, Gemini, local mmproj
    // pairs) went blind after one tool call and truthfully claimed
    // they could not see the image.
    //
    // Policy: project images from the NEWEST real user message (not a
    // tool result under either protocol), and only that one.  This
    // keeps the image visible for the entire agent loop of the attach
    // turn and for immediate re-asks, while bounding per-request
    // upload cost to one message's images.  Once the user sends a
    // newer message, older images age off the wire — which also means
    // switching to a text-only model afterwards keeps working exactly
    // as before.
    for (size_t i = wire.size(); i-- > 0;) {
        WireMsg& w = wire[i];
        if (w.role != "user" || w.isToolResult || !w.toolCallId.empty())
            continue;
        // Newest real user message found — carrier iff it has at
        // least one persisted image attachment.
        if (w.attachMeta) {
            for (unsigned k = 0; k < w.attachMeta->size(); ++k) {
                Poco::JSON::Object::Ptr a = w.attachMeta->getObject(k);
                if (a &&
                    a->optValue<std::string>("kind", "") == "image" &&
                    !a->optValue<std::string>("storage_path", "").empty()) {
                    w.isImageCarrier = true;
                    break;
                }
            }
        }
        break;
    }

    // Resolved once; the stringify lambda below may run twice (before
    // and after elision) and must not recompute path plumbing.
    const std::string imageWorkflowDir =
        m_filePath.empty() ? std::string() : GetWorkflowDir(m_filePath);

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
        ApplyLocalLlamaSampling(root, model, agentSamplingProfile);

        // ── Reasoning override (/think on|off) ───────────────────
        // Auto sends nothing, keeping the historical request shape
        // byte-for-byte.  Local llama-server targets (.gguf model
        // value) get chat_template_kwargs.enable_thinking, which
        // hybrid-reasoning chat templates (Qwen3 family and similar)
        // honor when the server runs with --jinja; templates that
        // never reference the flag ignore it, and llama-server
        // ignores the field entirely without --jinja.  Remote
        // OpenAI-compatible targets get an OpenRouter-style
        // reasoning object.  A strict remote endpoint that rejects
        // unknown fields surfaces its error in chat, and /think auto
        // restores the untouched request.  Skipped on image-output
        // turns so provider routing for image models is unaffected.
        if (m_thinkOverride != ThinkOverride::Auto && !imageOutput) {
            const bool wantThink = (m_thinkOverride == ThinkOverride::On);
            if (LooksLikeLocalGgufModel(model)) {
                Poco::JSON::Object::Ptr kwargs = new Poco::JSON::Object;
                kwargs->set("enable_thinking", wantThink);
                root->set("chat_template_kwargs", kwargs);
            } else {
                Poco::JSON::Object::Ptr reasoning = new Poco::JSON::Object;
                reasoning->set("enabled", wantThink);
                root->set("reasoning", reasoning);
            }
        }

        // Image-generation turn (OpenRouter chat-completions image
        // models).  Without an explicit modalities request the
        // provider returns text only; with it, generated images
        // arrive on the assistant message's `images` field as base64
        // data URLs.  Callers pass imageOutput=true only for remote
        // targets whose selected model carries the image_output flag,
        // so local llama-server never sees this field.
        if (imageOutput) {
            Poco::JSON::Array::Ptr modalities = new Poco::JSON::Array;
            modalities->add(std::string("image"));
            modalities->add(std::string("text"));
            root->set("modalities", modalities);
        }

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
                m->set("role", w.role);

                // Image carrier: rebuild the OpenAI-style content
                // array (image_url data-URI parts + text part) from
                // the persisted attachment files.  Unreadable or
                // missing files are skipped individually; if nothing
                // loads, fall back to the plain string so the request
                // stays valid.  Capped at 8 parts as a safety bound.
                bool emittedParts = false;
                if (w.isImageCarrier && !imageWorkflowDir.empty()) {
                    Poco::JSON::Array::Ptr parts = new Poco::JSON::Array;
                    int imageCount = 0;
                    for (unsigned k = 0;
                         k < w.attachMeta->size() && imageCount < 8; ++k) {
                        Poco::JSON::Object::Ptr a =
                            w.attachMeta->getObject(k);
                        if (!a) continue;
                        if (a->optValue<std::string>("kind", "") != "image")
                            continue;
                        const std::string rel =
                            a->optValue<std::string>("storage_path", "");
                        if (rel.empty()) continue;

                        const std::string uri = LoadImageAsDataUriCached(
                            imageWorkflowDir + "/" + rel,
                            a->optValue<std::string>("mime_type", ""));
                        if (uri.empty()) continue;

                        Poco::JSON::Object::Ptr imageUrl =
                            new Poco::JSON::Object;
                        imageUrl->set("url", uri);
                        Poco::JSON::Object::Ptr part =
                            new Poco::JSON::Object;
                        part->set("type", "image_url");
                        part->set("image_url", imageUrl);
                        parts->add(part);
                        ++imageCount;
                    }
                    if (imageCount > 0) {
                        Poco::JSON::Object::Ptr textPart =
                            new Poco::JSON::Object;
                        textPart->set("type", "text");
                        textPart->set("text", w.content);
                        parts->add(textPart);
                        m->set("content", parts);
                        emittedParts = true;
                    }
                }
                if (!emittedParts)
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

            // LlamaBoss dispatches exactly one tool call per assistant
            // turn (AgentController truncates extra entries via
            // KeepOnlySelectedToolCallJson).  Ask the model not to
            // generate parallel calls in the first place: extra calls
            // would only be silently discarded, wasting tokens and
            // leaving the model believing it issued work that never
            // ran.  llama.cpp's OpenAI-compatible endpoint honors this
            // for templates that support it and ignores it otherwise;
            // the controller-side truncation remains as the backstop.
            root->set("parallel_tool_calls", false);
        }

        std::ostringstream oss;
        Poco::JSON::Stringifier::stringify(root, oss);
        return oss.str();
    };

    std::string body = stringifyWire();

    // Mutation tracker for the elision and sanitizer phases below.
    // Each phase mutates `wire` in place; `body` is only re-stringified
    // once at the end if anything actually changed.  This replaces the
    // previous pattern of re-stringifying after every elision (O(n²)
    // bytes worst case) plus an unconditional re-stringify at the end
    // of the sanitizer (wasted on the common no-orphan path).
    bool wireDirty = false;

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
            const size_t maxCandidates =
                (totalToolResults > kMinPreservedResults)
                    ? (totalToolResults - kMinPreservedResults)
                    : 0;

            // Track an *estimated* body size with a running raw-content
            // delta instead of re-stringifying the wire after each
            // elision.  This is always a conservative over-estimate of
            // the actual JSON-escaped size after elision: the elided
            // content typically contains \n / quotes / control chars
            // (each costing 1+ extra byte once JSON-escaped on the
            // wire), while the marker we substitute is plain ASCII
            // (~1:1 escape ratio).  So actual_body <= estimated_body,
            // and stopping when estimated <= budget guarantees the real
            // body is also <= budget.  We may elide one extra candidate
            // versus the old per-iteration measurement, never under.
            size_t estimatedBodySize = body.size();
            size_t toolResultsSeen   = 0;

            for (size_t i = 0; i < wire.size(); ++i) {
                if (!wire[i].isToolResult) continue;
                ++toolResultsSeen;
                if (toolResultsSeen > maxCandidates) break;

                const size_t oldSize = wire[i].content.size();
                wire[i].content = ElideToolResultBody(wire[i].content);
                const size_t newSize = wire[i].content.size();

                if (oldSize > newSize) {
                    estimatedBodySize -= (oldSize - newSize);
                    ++m_lastBuildElidedCount;   // context meter: real drop
                }
                wireDirty = true;

                if (estimatedBodySize <= budget) break;
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
                wireDirty = true;
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
                wireDirty = true;
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
                wireDirty = true;
            }
        }

        // If an assistant message had empty content and only invalid
        // tool_calls, stripping those calls leaves a pure placeholder.
        // Drop it from the wire request; otherwise llama-server receives
        // an empty assistant turn that adds no value and can confuse the
        // transcript around tool results.
        const size_t preEraseSize = wire.size();
        wire.erase(
            std::remove_if(wire.begin(), wire.end(), [](const WireMsg& w) {
                return w.role == "assistant" &&
                       w.content.find_first_not_of(" \t\r\n") == std::string::npos &&
                       !w.toolCalls;
            }),
            wire.end());
        if (wire.size() != preEraseSize) {
            wireDirty = true;
        }
    }

    // Single re-stringify at the end if either phase actually mutated
    // the wire.  In the common short-conversation case (no elision
    // needed, sanitizer found nothing to fix), this is skipped and the
    // initial stringify above is the only JSON serialization performed.
    if (wireDirty) {
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
    m_streamBufferDirty = 0;
    AddAssistantMessage("", model);
}

void ChatHistory::AppendToLastAssistantMessage(const std::string& delta)
{
    if (delta.empty()) return;

    if (!m_messages.empty() && IsLastMessageRole("assistant")) {
        // Buffer-only path.  We deliberately avoid copying the full
        // m_streamBuffer into the JSON object on every delta:
        // Poco::JSON::Object::set("content", value) takes the value by
        // copy, so per-delta sync makes the streaming cost O(n²) in body
        // bytes — measurable as a typewriter that gets slower as the
        // assistant response grows.
        //
        // Instead, accumulate in m_streamBuffer (amortized O(1) on
        // std::string append) and only push into the JSON object when
        // enough bytes have piled up to make the copy worth it, or when
        // a reader (BuildChatRequestJson, SaveToFile,
        // UpdateLastAssistantMessage on completion) explicitly demands
        // it via FlushStreamBuffer().
        //
        // 4 KiB is small enough that crashes / SIGTERM / OnClose
        // mid-stream lose at most ~4 KiB of buffered output via
        // auto-save (which also calls FlushStreamBuffer first), and
        // large enough to drop the per-delta cost by ~3 orders of
        // magnitude on typical token deltas.  This is also why we keep
        // a periodic sync rather than going purely lazy: even though
        // every reader currently flushes first, the periodic sync
        // limits the worst-case data loss window if a future code path
        // ever forgets to flush before reading m_messages directly.
        m_streamBuffer += delta;
        m_streamBufferDirty += delta.size();
        MarkDirty();

        constexpr std::size_t kStreamSyncThresholdBytes = 4 * 1024;
        if (m_streamBufferDirty >= kStreamSyncThresholdBytes) {
            m_messages.back()->set("content", m_streamBuffer);
            m_streamBufferDirty = 0;
        }
    }
}

void ChatHistory::UpdateLastAssistantMessage(const std::string& content)
{
    if (!m_messages.empty() && IsLastMessageRole("assistant")) {
        m_messages.back()->set("content", content);
        MarkDirty();
    }
    // Replacing content makes any buffered streaming bytes obsolete.
    // Clear them so subsequent FlushStreamBuffer() calls don't overwrite
    // the explicit content with a stale buffer snapshot.
    m_streamBuffer.clear();
    m_streamBufferDirty = 0;
}

void ChatHistory::RemoveLastAssistantMessage()
{
    if (!m_messages.empty() && IsLastMessageRole("assistant")) {
        m_messages.pop_back();
        MarkDirty();
    }
    // Defensive: if the removed assistant message was an in-flight
    // streaming target, the buffer carried its content and would
    // otherwise leak forward into the next assistant message added to
    // the history.  Belt and braces — current callers always go through
    // UpdateLastAssistantMessage("") first, but a future caller might
    // not.
    m_streamBuffer.clear();
    m_streamBufferDirty = 0;
}

void ChatHistory::RemoveLastSystemMessage()
{
    if (!m_messages.empty() && IsLastMessageRole("system")) {
        m_messages.pop_back();
        MarkDirty();
    }
}

bool ChatHistory::HasAssistantPlaceholder() const
{
    if (!m_messages.empty() && IsLastMessageRole("assistant")) {
        const auto& last = m_messages.back();

        // Native function-calling turns can be a valid assistant message with
        // empty visible content but a tool_calls sidecar.  Treating that as a
        // placeholder would let Stop/cancel cleanup remove the assistant call
        // and orphan the following tool result in saved/native transcripts.
        if (last->has("tool_calls")) return false;

        // Consult the streaming buffer first.  During an active stream,
        // the JSON "content" field can lag behind the buffer by up to
        // kStreamSyncThresholdBytes (see AppendToLastAssistantMessage).
        // A non-empty buffer means real content has arrived even if it
        // has not been synced into the JSON object yet, so this is
        // *not* a placeholder.
        //
        // This matters for the Stop button path in MyFrame: it calls
        // HasAssistantPlaceholder() and removes the message if true.
        // Without consulting the buffer, an early Stop during a long
        // stream could drop the partial response.
        if (!m_streamBuffer.empty()) return false;
        return last->getValue<std::string>("content").empty();
    }
    return false;
}

void ChatHistory::FlushStreamBuffer()
{
    if (m_streamBufferDirty == 0) return;

    // The buffer can only meaningfully attach to a trailing assistant
    // message.  If the last message is not an assistant message (e.g. a
    // tool result was just appended after the streaming reply), the
    // buffer's content has already been committed to the JSON object on
    // the prior assistant turn and the dirty counter just needs to be
    // reset to avoid an out-of-band write to a non-assistant message.
    if (m_messages.empty() || !IsLastMessageRole("assistant")) {
        m_streamBufferDirty = 0;
        return;
    }

    m_messages.back()->set("content", m_streamBuffer);
    m_streamBufferDirty = 0;
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
            // If the latest assistant response is actively streaming, the
            // JSON object may intentionally lag behind m_streamBuffer.  Return
            // the buffer so callers do not see stale/truncated assistant text.
            if (it == m_messages.rbegin() && !m_streamBuffer.empty()) {
                return m_streamBuffer;
            }
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

bool ChatHistory::CreateSaveSnapshot(
    const std::string& filePath,
    const std::vector<std::string>& models,
    SaveSnapshot& outSnapshot)
{
    const std::string savePath = filePath.empty() ? m_filePath : filePath;
    if (savePath.empty() || !HasPersistableContent()) return false;

    // Make any buffered streaming bytes part of the immutable snapshot.
    FlushStreamBuffer();

    try {
        if (m_createdAt.empty()) m_createdAt = CurrentTimestamp();
        m_updatedAt = CurrentTimestamp();

        const std::string generatedTitle = GenerateTitle();
        const bool titleNeedsRepair =
            m_title.empty() ||
            m_title == "Untitled conversation" ||
            IsLegacySessionContextTitle(m_title);
        if (titleNeedsRepair &&
            generatedTitle != "Untitled conversation") {
            m_title = generatedTitle;
        }

        SaveSnapshot snapshot;
        snapshot.filePath      = savePath;
        snapshot.title         = m_title;
        snapshot.createdAt     = m_createdAt;
        snapshot.updatedAt     = m_updatedAt;
        snapshot.toolCwd       = m_toolCwd;
        snapshot.toolTimeoutMs = m_toolTimeoutMs;
        snapshot.thinkOverride = m_thinkOverride;
        snapshot.projectId     = m_projectId;
        snapshot.projectName   = m_projectName;
        snapshot.projectRoot   = m_projectRoot;
        snapshot.goalState     = m_goalState;
        snapshot.models        = models;
        snapshot.revision      = m_revision;
        snapshot.messages.reserve(m_messages.size());

        auto ArrayToJson = [](const Poco::JSON::Array::Ptr& arr) -> std::string {
            if (!arr) return {};
            std::ostringstream oss;
            Poco::JSON::Stringifier::stringify(arr, oss);
            return oss.str();
        };

        for (const auto& msg : m_messages) {
            if (!msg) continue;

            const std::string content = msg->has("content")
                ? msg->getValue<std::string>("content")
                : std::string();
            const bool hasToolCalls  = msg->has("tool_calls");
            const bool hasToolCallId = msg->has("tool_call_id");
            const bool hasImages     = msg->has("images");
            const bool hasAttachments = msg->has("attachments");
            if (content.empty() && !hasToolCalls && !hasToolCallId &&
                !hasImages) {
                continue;
            }

            SaveMessageSnapshot saved;
            saved.role    = msg->getValue<std::string>("role");
            saved.content = content;
            saved.model   = GetMessageModel(msg);
            saved.target  = GetMessageTarget(msg);
            if (hasToolCallId) {
                saved.toolCallId =
                    msg->getValue<std::string>("tool_call_id");
            }
            if (hasAttachments) {
                try { saved.attachmentsJson = ArrayToJson(msg->getArray("attachments")); }
                catch (...) { saved.attachmentsJson.clear(); }
            }
            if (hasToolCalls) {
                try { saved.toolCallsJson = ArrayToJson(msg->getArray("tool_calls")); }
                catch (...) { saved.toolCallsJson.clear(); }
            }
            if (hasImages) {
                try { saved.imagesJson = ArrayToJson(msg->getArray("images")); }
                catch (...) { saved.imagesJson.clear(); }
            }
            snapshot.messages.push_back(std::move(saved));
        }

        // Resolve the optional cosmetic marker on the UI thread because the
        // wx filesystem helpers are not part of the worker contract.
        try {
            const std::string workflowDir = GetWorkflowDir(savePath);
            if (!workflowDir.empty() &&
                wxDirExists(wxString::FromUTF8(workflowDir))) {
                snapshot.titleMarkerPath =
                    JoinWorkflowPath(workflowDir, "_title.txt");
            }
        } catch (...) {
            snapshot.titleMarkerPath.clear();
        }

        outSnapshot = std::move(snapshot);
        return true;
    }
    catch (...) {
        return false;
    }
}

bool ChatHistory::WriteSaveSnapshot(const SaveSnapshot& snapshot,
                                    bool durable)
{
    if (snapshot.filePath.empty()) return false;

    try {
        auto ParseArray = [](const std::string& json) -> Poco::JSON::Array::Ptr {
            if (json.empty()) return nullptr;
            Poco::JSON::Parser parser;
            auto var = parser.parse(json);
            return var.extract<Poco::JSON::Array::Ptr>();
        };

        Poco::JSON::Object::Ptr root = new Poco::JSON::Object(true);
        root->set("version", CONVERSATION_FORMAT_VERSION);
        root->set("title", snapshot.title);
        root->set("created_at", snapshot.createdAt);
        root->set("updated_at", snapshot.updatedAt);

        if (!snapshot.toolCwd.empty())
            root->set("tool_cwd", snapshot.toolCwd);
        if (snapshot.toolTimeoutMs != 0)
            root->set("tool_timeout_ms", (Poco::UInt64)snapshot.toolTimeoutMs);
        // Reasoning override — omitted entirely when Auto so older
        // builds and untouched conversations keep an identical file.
        if (snapshot.thinkOverride == ThinkOverride::On)
            root->set("think", std::string("on"));
        else if (snapshot.thinkOverride == ThinkOverride::Off)
            root->set("think", std::string("off"));

        if (!snapshot.projectId.empty() && !snapshot.projectRoot.empty()) {
            root->set("project_id", snapshot.projectId);
            root->set("project_name", snapshot.projectName);
            root->set("project_root", snapshot.projectRoot);
        }

        const GoalState& goal = snapshot.goalState;
        if (goal.HasGoal()) {
            Poco::JSON::Object::Ptr goalObj = new Poco::JSON::Object;
            goalObj->set("status", PersistedGoalStatus(goal.status));
            goalObj->set("objective", goal.objective);
            goalObj->set("turns_used", goal.turnsUsed);
            goalObj->set("verifier_passes", goal.verifierPasses);
            goalObj->set("verifier_failures", goal.verifierFailures);
            if (!goal.lastVerifierReason.empty())
                goalObj->set("last_verifier_reason", goal.lastVerifierReason);
            if (!goal.lastInterruptionReason.empty())
                goalObj->set("last_interruption_reason", goal.lastInterruptionReason);
            if (!goal.awaitingUserReason.empty())
                goalObj->set("awaiting_user_reason", goal.awaitingUserReason);
            if (!goal.awaitingUserPromptEvidence.empty())
                goalObj->set("awaiting_user_prompt_evidence", goal.awaitingUserPromptEvidence);
            if (!goal.awaitingUserReplyEvidence.empty())
                goalObj->set("awaiting_user_reply_evidence", goal.awaitingUserReplyEvidence);
            if (!goal.structuredAgentEvidence.empty()) {
                goalObj->set("structured_agent_evidence",
                             StringVectorToJsonArray(goal.structuredAgentEvidence));
            }

            const GoalContract& contract = goal.contract;
            if (contract.status != GoalContractStatus::None ||
                !contract.successCriteria.empty() ||
                !contract.constraints.empty() ||
                !contract.evidenceChecks.empty() ||
                !contract.lastBuilderReason.empty()) {
                Poco::JSON::Object::Ptr contractObj = new Poco::JSON::Object;
                contractObj->set("status",
                                 PersistedGoalContractStatus(contract.status));
                contractObj->set("success_criteria",
                                 StringVectorToJsonArray(contract.successCriteria));
                contractObj->set("constraints",
                                 StringVectorToJsonArray(contract.constraints));
                contractObj->set("evidence_checks",
                                 StringVectorToJsonArray(contract.evidenceChecks));
                if (!contract.lastBuilderReason.empty())
                    contractObj->set("last_builder_reason",
                                     contract.lastBuilderReason);
                goalObj->set("contract", contractObj);
            }
            root->set("goal", goalObj);
        }

        Poco::JSON::Array::Ptr modelsArray = new Poco::JSON::Array;
        for (const auto& model : snapshot.models) modelsArray->add(model);
        root->set("models", modelsArray);
        if (!snapshot.models.empty()) root->set("model", snapshot.models.front());

        Poco::JSON::Array::Ptr messagesArray = new Poco::JSON::Array;
        for (const auto& msg : snapshot.messages) {
            Poco::JSON::Object::Ptr saveMsg = new Poco::JSON::Object;
            saveMsg->set("role", msg.role);
            saveMsg->set("content", msg.content);
            if (!msg.model.empty()) saveMsg->set("model", msg.model);
            if (!msg.target.empty()) saveMsg->set("target", msg.target);
            if (!msg.toolCallId.empty())
                saveMsg->set("tool_call_id", msg.toolCallId);

            try {
                if (auto arr = ParseArray(msg.attachmentsJson))
                    saveMsg->set("attachments", arr);
            } catch (...) { /* malformed sidecar: omit */ }
            try {
                if (auto arr = ParseArray(msg.toolCallsJson))
                    saveMsg->set("tool_calls", arr);
            } catch (...) { /* malformed sidecar: omit */ }
            try {
                if (auto arr = ParseArray(msg.imagesJson))
                    saveMsg->set("images", arr);
            } catch (...) { /* malformed sidecar: omit */ }

            messagesArray->add(saveMsg);
        }
        root->set("messages", messagesArray);

        std::ostringstream oss;
        Poco::JSON::Stringifier::stringify(root, oss, 2);
        const std::string body = oss.str();

        tool_staged_write::StagedTempFile tmp =
            tool_staged_write::CreateStagedTempFile(snapshot.filePath);
        if (tmp.handle == INVALID_HANDLE_VALUE) return false;

        const char* data = body.data();
        size_t remain = body.size();
        while (remain > 0) {
            const DWORD chunk = remain > 0x40000000U
                ? 0x40000000U : static_cast<DWORD>(remain);
            DWORD written = 0;
            if (!::WriteFile(tmp.handle, data, chunk, &written, nullptr) ||
                written == 0) {
                ::CloseHandle(tmp.handle);
                ::DeleteFileW(tmp.wPath.c_str());
                return false;
            }
            data += written;
            remain -= written;
        }

        if (durable && !::FlushFileBuffers(tmp.handle)) {
            ::CloseHandle(tmp.handle);
            ::DeleteFileW(tmp.wPath.c_str());
            return false;
        }
        if (!::CloseHandle(tmp.handle)) {
            ::DeleteFileW(tmp.wPath.c_str());
            return false;
        }

        const std::wstring wFinal =
            path_safety::Utf8ToWide(snapshot.filePath);
        if (wFinal.empty()) {
            ::DeleteFileW(tmp.wPath.c_str());
            return false;
        }

        const DWORD moveFlags = MOVEFILE_REPLACE_EXISTING |
            (durable ? MOVEFILE_WRITE_THROUGH : 0);
        if (!::MoveFileExW(tmp.wPath.c_str(), wFinal.c_str(), moveFlags)) {
            ::DeleteFileW(tmp.wPath.c_str());
            return false;
        }

        if (!snapshot.titleMarkerPath.empty()) {
            try {
                std::ofstream marker(
                    path_safety::Utf8ToWide(snapshot.titleMarkerPath),
                    std::ios::out | std::ios::trunc);
                if (marker.is_open()) {
                    marker << snapshot.title << "\n\n"
                           << "Created: " << snapshot.createdAt << "\n"
                           << "Updated: " << snapshot.updatedAt << "\n";
                }
            } catch (...) {
                // Cosmetic marker failure never fails the conversation save.
            }
        }

        return true;
    }
    catch (...) {
        return false;
    }
}

void ChatHistory::CommitSaveSnapshot(const SaveSnapshot& snapshot)
{
    // Async completion can arrive after a conversation switch.  Never let a
    // stale snapshot rewrite the identity or dirty flag of the new history.
    if (m_filePath != snapshot.filePath) return;
    if (m_revision == snapshot.revision) m_dirty = false;
}

bool ChatHistory::SaveToFile(const std::string& filePath,
                             const std::vector<std::string>& models,
                             bool durable)
{
    SaveSnapshot snapshot;
    if (!CreateSaveSnapshot(filePath, models, snapshot)) return false;
    if (!WriteSaveSnapshot(snapshot, durable)) return false;

    // Synchronous Save/Save-As owns this transition and may intentionally
    // change the current path.  Async callers use CommitSaveSnapshot directly,
    // which rejects completions for a different active conversation.
    m_filePath = snapshot.filePath;
    CommitSaveSnapshot(snapshot);
    return true;
}

bool ChatHistory::SaveToFile(const std::string& filePath, const std::string& model,
                             bool durable)
{
    return SaveToFile(filePath, std::vector<std::string>{ model }, durable);
}

bool ChatHistory::LoadFromFile(const std::string& filePath, std::vector<std::string>& outModels)
{
    // Make failure deterministic for callers that reuse an output vector.
    // A missing/corrupt file should not leave stale model paths from a
    // previous successful load in outModels.
    outModels.clear();

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

        // ── Strong exception guarantee ───────────────────────────────
        // Everything below parses into *locals* and commits to members
        // only in the noexcept block at the very end.  A throw (bad field
        // type, missing "role", non-convertible model) or a structurally
        // malformed array now leaves this ChatHistory exactly as it was —
        // the previously loaded conversation stays intact, and the `false`
        // return is truthful.  This matters because m_filePath is left
        // pointing at the prior conversation on failure, so a half-mutated
        // state could otherwise be autosaved over a perfectly good file.
        //
        // Fields the original code overwrote only when present (title,
        // timestamps) seed their locals from the current members so the
        // "absent => unchanged" behaviour is preserved.  Fields the
        // original cleared-then-maybe-set (tool/project) start empty so the
        // "absent => cleared" behaviour is preserved.
        std::string   newTitle      = m_title;
        std::string   newCreatedAt  = m_createdAt;
        std::string   newUpdatedAt  = m_updatedAt;
        std::string   newToolCwd;
        unsigned long newToolTimeoutMs = 0;
        ThinkOverride newThinkOverride = ThinkOverride::Auto;
        std::string   newProjectId;
        std::string   newProjectName;
        std::string   newProjectRoot;
        GoalState     newGoal;                       // default == Reset() (no goal)
        std::vector<std::string>               newModels;
        std::vector<Poco::JSON::Object::Ptr>   newMessages;

        // Read metadata
        if (root->has("title")) {
            newTitle = root->getValue<std::string>("title");
        }
        if (root->has("created_at")) {
            newCreatedAt = root->getValue<std::string>("created_at");
        }
        if (root->has("updated_at")) {
            newUpdatedAt = root->getValue<std::string>("updated_at");
        }

        // Tool execution context (Phase 3) — locals default to empty/0,
        // overwritten only if the key is present in the file.
        if (root->has("tool_cwd")) {
            newToolCwd = root->getValue<std::string>("tool_cwd");
        }
        if (root->has("tool_timeout_ms")) {
            newToolTimeoutMs =
                (unsigned long)root->getValue<Poco::UInt64>("tool_timeout_ms");
        }
        if (root->has("think")) {
            const std::string t = root->getValue<std::string>("think");
            if (t == "on")       newThinkOverride = ThinkOverride::On;
            else if (t == "off") newThinkOverride = ThinkOverride::Off;
            // anything else (including future values) loads as Auto
        }

        // Optional long-lived project association (Projects Phase 1).
        if (root->has("project_id")) {
            newProjectId = root->getValue<std::string>("project_id");
        }
        if (root->has("project_name")) {
            newProjectName = root->getValue<std::string>("project_name");
        }
        if (root->has("project_root")) {
            newProjectRoot = root->getValue<std::string>("project_root");
        }

        // Durable goal state (Goals Phase 5).  Older conversations simply
        // omit this block and load with no active goal.
        if (root->has("goal")) {
            Poco::JSON::Object::Ptr goalObj = root->getObject("goal");
            if (goalObj) {
                const std::string objective = goalObj->has("objective")
                    ? goalObj->getValue<std::string>("objective")
                    : std::string();
                const std::string statusText = goalObj->has("status")
                    ? goalObj->getValue<std::string>("status")
                    : std::string("none");
                const GoalStatus loadedStatus = GoalStatusFromPersisted(statusText);

                if (!objective.empty() && loadedStatus != GoalStatus::None) {
                    newGoal.status = loadedStatus;
                    newGoal.objective = objective;
                    newGoal.turnsUsed = JsonIntOrDefault(goalObj, "turns_used", 0);
                    newGoal.verifierPasses = JsonIntOrDefault(goalObj, "verifier_passes", 0);
                    newGoal.verifierFailures = JsonIntOrDefault(goalObj, "verifier_failures", 0);
                    newGoal.lastVerifierReason = goalObj->has("last_verifier_reason")
                        ? goalObj->getValue<std::string>("last_verifier_reason")
                        : std::string();
                    newGoal.lastInterruptionReason = goalObj->has("last_interruption_reason")
                        ? goalObj->getValue<std::string>("last_interruption_reason")
                        : std::string();
                    newGoal.awaitingUserReason = goalObj->has("awaiting_user_reason")
                        ? goalObj->getValue<std::string>("awaiting_user_reason")
                        : std::string();
                    newGoal.awaitingUserPromptEvidence = goalObj->has("awaiting_user_prompt_evidence")
                        ? goalObj->getValue<std::string>("awaiting_user_prompt_evidence")
                        : std::string();
                    newGoal.awaitingUserReplyEvidence = goalObj->has("awaiting_user_reply_evidence")
                        ? goalObj->getValue<std::string>("awaiting_user_reply_evidence")
                        : std::string();
                    newGoal.structuredAgentEvidence =
                        goalObj->has("structured_agent_evidence")
                            ? JsonArrayToStringVector(goalObj->getArray("structured_agent_evidence"))
                            : std::vector<std::string>{};
                    newGoal.TrimStructuredAgentEvidenceToCap();
                    newGoal.explicitlyCleared = false;

                    if (goalObj->has("contract")) {
                        Poco::JSON::Object::Ptr contractObj = goalObj->getObject("contract");
                        if (contractObj) {
                            GoalContract& contract = newGoal.contract;
                            const std::string contractStatusText = contractObj->has("status")
                                ? contractObj->getValue<std::string>("status")
                                : std::string("none");
                            contract.status = GoalContractStatusFromPersisted(contractStatusText);
                            contract.successCriteria = contractObj->has("success_criteria")
                                ? JsonArrayToStringVector(contractObj->getArray("success_criteria"))
                                : std::vector<std::string>{};
                            contract.constraints = contractObj->has("constraints")
                                ? JsonArrayToStringVector(contractObj->getArray("constraints"))
                                : std::vector<std::string>{};
                            contract.evidenceChecks = contractObj->has("evidence_checks")
                                ? JsonArrayToStringVector(contractObj->getArray("evidence_checks"))
                                : std::vector<std::string>{};
                            contract.lastBuilderReason = contractObj->has("last_builder_reason")
                                ? contractObj->getValue<std::string>("last_builder_reason")
                                : std::string();

                            // A hidden contract-builder turn cannot resume after an app
                            // restart. Convert an interrupted draft into objective-only
                            // verification rather than leaving /goal status stuck at
                            // "drafting" forever.
                            if (contract.status == GoalContractStatus::Drafting) {
                                contract.MarkFailed(
                                    "Contract drafting was interrupted before the conversation was reloaded.");
                            }
                            else if (contract.status == GoalContractStatus::Ready &&
                                     contract.successCriteria.empty()) {
                                contract.MarkFailed(
                                    "Saved contract was missing success criteria and was restored as objective-only.");
                            }
                        }
                    }
                }
            }
        }

        // Read models — prefer v2 "models" array, fall back to v1 "model" string.
        // A present-but-non-array "models" yields a null Ptr from getArray();
        // treat that as a malformed file (clean failure, members untouched)
        // rather than dereferencing null.
        if (root->has("models")) {
            auto arr = root->getArray("models");
            if (!arr) return false;
            for (size_t i = 0; i < arr->size(); ++i) {
                newModels.push_back(arr->get(i).convert<std::string>());
            }
        }
        else if (root->has("model")) {
            newModels.push_back(root->getValue<std::string>("model"));
        }

        // Read messages (with optional per-message "model" field).  As with
        // "models", a non-array "messages" or a non-object element would have
        // dereferenced a null Poco Ptr (an uncatchable crash, not an
        // exception); guard both as a clean load failure.
        if (root->has("messages")) {
            auto messagesArray = root->getArray("messages");
            if (!messagesArray) return false;
            for (size_t i = 0; i < messagesArray->size(); ++i) {
                auto msgObj = messagesArray->getObject(i);
                if (!msgObj) return false;
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

                // Generated-images sidecar (absent in older files).
                if (msgObj->has("images")) {
                    try {
                        loadedMsg->set("images", msgObj->getArray("images"));
                    } catch (...) { /* malformed — skip */ }
                }
                newMessages.push_back(loadedMsg);
            }
        }

        // ── Commit ───────────────────────────────────────────────────
        // Past every throw/return point.  Only moves and scalar assigns
        // below, all non-throwing, so the object transitions atomically
        // from the old conversation to the new one.
        m_title          = std::move(newTitle);
        m_createdAt      = std::move(newCreatedAt);
        m_updatedAt      = std::move(newUpdatedAt);
        m_toolCwd        = std::move(newToolCwd);
        m_toolTimeoutMs  = newToolTimeoutMs;
        m_thinkOverride  = newThinkOverride;
        m_projectId      = std::move(newProjectId);
        m_projectName    = std::move(newProjectName);
        m_projectRoot    = std::move(newProjectRoot);
        m_goalState      = std::move(newGoal);
        m_messages       = std::move(newMessages);

        // Repair legacy titles in memory as soon as the conversation opens
        // so the window title is useful immediately.  The next normal save
        // persists the repaired value; loading itself remains read-only.
        if (IsLegacySessionContextTitle(m_title) ||
            m_title == "Untitled conversation") {
            const std::string repairedTitle = GenerateTitle();
            if (repairedTitle != "Untitled conversation")
                m_title = repairedTitle;
        }

        // Per-chat approval choices are intentionally in-memory only and
        // reset on every load.  Loading also clears any unflushed streaming
        // buffer so a prior session can't leak into the next assistant
        // message.
        m_chatApprovedTools.clear();
        m_chatApprovalTrustEnabled = false;
        m_streamBuffer.clear();
        m_streamBufferDirty = 0;

        outModels   = std::move(newModels);
        m_filePath  = filePath;
        m_dirty     = false;
        m_revision  = 0;
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

namespace {

bool TitleIsIdChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
}

std::string TitleTrim(std::string s)
{
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

std::string TitleLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string TitleCollapseWhitespace(std::string s)
{
    for (auto& c : s) {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    }

    std::string out;
    out.reserve(s.size());
    bool lastWasSpace = false;
    for (unsigned char ch : s) {
        if (std::isspace(ch)) {
            if (!lastWasSpace) out.push_back(' ');
            lastWasSpace = true;
        }
        else {
            out.push_back(static_cast<char>(ch));
            lastWasSpace = false;
        }
    }
    return TitleTrim(out);
}

std::string TitleStripOuterQuotes(std::string s)
{
    s = TitleTrim(std::move(s));
    while (s.size() >= 2) {
        char first = s.front();
        char last  = s.back();
        if ((first == '"' && last == '"') ||
            (first == '\'' && last == '\'')) {
            s = TitleTrim(s.substr(1, s.size() - 2));
        }
        else {
            break;
        }
    }
    return s;
}

std::string TitleStripGreeting(std::string s)
{
    std::string lower = TitleLower(s);
    const char* greetings[] = {
        "hello ", "hello. ", "hello, ", "hi ", "hi. ", "hi, ", "hey ", "hey. ", "hey, "
    };

    for (const char* prefix : greetings) {
        const size_t n = std::strlen(prefix);
        if (lower.compare(0, n, prefix) == 0 && s.size() > n + 4) {
            return TitleTrim(s.substr(n));
        }
    }
    return s;
}

std::string TitleReadIdAfter(const std::string& text, size_t start)
{
    while (start < text.size() && (text[start] == '/' || text[start] == '=')) {
        ++start;
    }

    std::string id;
    while (start < text.size() && TitleIsIdChar(text[start]) && id.size() < 11) {
        id.push_back(text[start]);
        ++start;
    }

    return id.size() == 11 ? id : std::string();
}

std::string TitleExtractYouTubeId(const std::string& content)
{
    const std::string lower = TitleLower(content);

    size_t pos = lower.find("youtu.be/");
    if (pos != std::string::npos) {
        std::string id = TitleReadIdAfter(content, pos + 8);
        if (!id.empty()) return id;
    }

    pos = lower.find("youtube.com/watch");
    if (pos != std::string::npos) {
        size_t v = lower.find("v=", pos);
        if (v != std::string::npos) {
            std::string id = TitleReadIdAfter(content, v + 2);
            if (!id.empty()) return id;
        }
    }

    pos = lower.find("youtube.com/shorts/");
    if (pos != std::string::npos) {
        std::string id = TitleReadIdAfter(content, pos + 19);
        if (!id.empty()) return id;
    }

    pos = lower.find("youtube.com/embed/");
    if (pos != std::string::npos) {
        std::string id = TitleReadIdAfter(content, pos + 18);
        if (!id.empty()) return id;
    }

    return {};
}

std::string TitleMakeFromYouTube(const std::string& content,
                                 const std::string& videoId)
{
    const std::string lower = TitleLower(content);

    if (lower.find("transcript") != std::string::npos ||
        lower.find("markdown")   != std::string::npos ||
        lower.find("text file")  != std::string::npos) {
        return "YouTube transcript " + videoId;
    }

    if (lower.find("summar") != std::string::npos) {
        return "YouTube summary " + videoId;
    }

    return "YouTube video " + videoId;
}

std::string TitleTruncate(std::string s, size_t maxLen = 64)
{
    s = TitleCollapseWhitespace(std::move(s));
    if (s.size() > maxLen) {
        // Titles are UTF-8.  Truncate by byte budget, but only on a
        // code-point boundary so we never persist invalid UTF-8 into
        // the conversation JSON.  This keeps downstream wxString::FromUTF8
        // calls from producing blank labels for CJK/emoji/accented titles.
        if (maxLen <= 3) {
            s = s.substr(0, maxLen);
        } else {
            size_t cut = maxLen - 3;
            while (cut > 0 &&
                   (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) {
                --cut;
            }
            s = s.substr(0, cut) + "...";
        }
    }
    return s.empty() ? "Untitled conversation" : s;
}

} // namespace

std::string ChatHistory::GenerateTitle() const
{
    // Use the first real user message.  Project-attached chats can be
    // saved before the first message, so SaveToFile may call this before
    // any usable content exists; in that case keep the placeholder.
    for (const auto& msg : m_messages) {
        if (msg->getValue<std::string>("role") != "user") {
            continue;
        }

        std::string content = msg->getValue<std::string>("content");
        content = StripSessionContextHeader(std::move(content));
        content = TitleStripGreeting(TitleStripOuterQuotes(
            TitleCollapseWhitespace(std::move(content))));
        if (content.empty()) continue;

        const std::string youtubeId = TitleExtractYouTubeId(content);
        if (!youtubeId.empty()) {
            return TitleMakeFromYouTube(content, youtubeId);
        }

        const std::string lower = TitleLower(content);
        if (lower.find("project.md") != std::string::npos) {
            return "Update PROJECT.md";
        }
        if (lower.find("requirements.txt") != std::string::npos) {
            return "Update project requirements";
        }

        return TitleTruncate(content);
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
    const std::string& bodyLang,
    const std::vector<PresentedFile>& presentedFiles)
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

    auto lowerAscii = [](std::string v) {
        for (char& c : v) c = (char)std::tolower((unsigned char)c);
        return v;
    };
    bool failedTool = !errorBody.empty();
    for (const std::string& chip : statusChips) {
        std::string c = lowerAscii(chip);
        if (c == "failed" || c == "blocked" || c == "error" ||
            c == "missing" || c == "too large" || c == "timed out" ||
            c == "cancelled" || c == "syntax error") {
            failedTool = true;
        }
        if (c.rfind("exit ", 0) == 0 && c != "exit 0") {
            failedTool = true;
        }
    }

    std::ostringstream ss;
    ss << "[tool: " << toolTag << "]\n"
       << "> " << commandEcho << "\n";

    if (failedTool) {
        ss << "\n[tool outcome]\n"
           << "TOOL FAILED OR DID NOT COMPLETE SUCCESSFULLY. Do not claim success, do not invent generated files, and do not summarize stale output. Fix the problem, retry only with a corrected tool call, or explain the failure to the user.\n";
    }

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

    bool wroteArtifactsHeader = false;
    for (const PresentedFile& file : presentedFiles) {
        // Persist only disk-backed artifacts. Inline-only chips are generated
        // from transient in-memory content and cannot be reopened after reload.
        if (file.diskPath.empty()) continue;

        if (!wroteArtifactsHeader) {
            ss << "\n[artifacts]\n";
            wroteArtifactsHeader = true;
        }

        Poco::JSON::Object obj;
        obj.set("display_name", file.displayName);
        obj.set("language", file.language);
        obj.set("disk_path", file.diskPath);
        obj.set("size_bytes", static_cast<Poco::UInt64>(file.sizeBytes));
        obj.set("line_count", file.lineCount);
        Poco::JSON::Stringifier::stringify(obj, ss);
        ss << "\n";
    }

    // Status chips — comma-joined, always emitted (even if empty, so
    // the closing bracket marks the end of the block unambiguously).
    // Keep this as the final section; the context-compaction helper relies
    // on [status: ...] being the footer.
    ss << "\n[status: ";
    for (size_t i = 0; i < statusChips.size(); ++i) {
        if (i > 0) ss << ", ";
        ss << statusChips[i];
    }
    ss << "]";

    return ss.str();
}
