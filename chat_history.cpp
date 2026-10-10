// chat_history.cpp
#include "chat_history.h"
#include "reasoning_policy.h"
#include "openai_responses.h"
#include "path_safety.h"
#include "tool_path_safety.h"
#include "tool_staged_write.h"   // CreateStagedTempFile for atomic save
#include "chat_folders.h"        // chat folder naming / recognition
#include "var_store.h"           // elision spool: Vars\ lane + card sentinel
#include "tool_call_elision.h"   // old tool-call argument elision

// Poco headers for JSON
#include <Poco/Timestamp.h>
#include <Poco/DateTimeFormatter.h>
#include <Poco/DateTimeFormat.h>
#include <Poco/UUIDGenerator.h>
#include <Poco/Types.h>
#include <Poco/FileStream.h>      // UTF-8-safe binary reads on Windows
#include <Poco/File.h>            // stat (size/mtime) for the data-URI cache

// wxWidgets for paths and file system
#include <wx/datetime.h>   // chat folder date prefix

// File format version: 9 (last bump added fields for the since-retired
// Goals feature; v9 files are still read, their "goal" block ignored).
static const int CONVERSATION_FORMAT_VERSION = 9;

// Forward declaration — defined further down with the other chat folder
// helpers.  Hoisted here so methods earlier in the file (notably
// SaveToFile, which writes the per-conversation _title.txt marker) can
// call it without reordering the entire helpers block.
static std::string JoinChatPath(const std::string& a, const std::string& b);

namespace {

// Retired Goals stored internal continuation instructions in chat history.
// Keep the saved transcript (and its message indices) intact, but never send
// these stale orchestration instructions to a model again. Restrict this to
// the original system-message prefix so user/assistant discussion is kept.
bool IsRetiredGoalContinuationMessage(const std::string& role,
                                      const std::string& content)
{
    return role == "system" &&
           content.rfind("Goal continuation instruction:", 0) == 0;
}

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
    // creative defaults.  Normal chat stays flexible; agent/skill
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
// once written into the conversation's chat folder.  Cache the
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

// ─── Persisted-image probe ───────────────────────────────────────
// True when a message's attachments sidecar contains at least one
// image that actually made it to disk.  This is the SAME test the
// image-carrier selection pass in BuildChatRequestJson uses, kept in
// one place so the two can't drift: the wire-build loop uses it to
// decide whether a blank-content user message is still meaningful,
// and the carrier pass uses it to pick which turn's images ride the
// request.
//
// Metadata only -- deliberately no filesystem check.  If the storage
// files were deleted out from under the conversation, the projection
// below already degrades gracefully (it skips unreadable images
// individually), and adding disk I/O to this loop to catch that case
// would cost more than it saves.
static bool HasPersistedImageAttachment(const Poco::JSON::Object::Ptr& msg)
{
    if (!msg || !msg->has("attachments")) return false;
    try {
        Poco::JSON::Array::Ptr arr = msg->getArray("attachments");
        if (!arr) return false;
        for (unsigned k = 0; k < arr->size(); ++k) {
            Poco::JSON::Object::Ptr a = arr->getObject(k);
            if (a &&
                a->optValue<std::string>("kind", "") == "image" &&
                !a->optValue<std::string>("storage_path", "").empty()) {
                return true;
            }
        }
    } catch (...) {
        // Malformed sidecar — treat as carrying no images rather than
        // letting a bad attachments array abort the whole request build.
    }
    return false;
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

// Serialized size of a JSON array exactly as the request Stringifier
// writes it.  Used for exact byte deltas when argument elision swaps a
// tool_calls array (and drops its Responses sidecar).
size_t StringifiedJsonSize(const Poco::JSON::Array::Ptr& arr)
{
    if (!arr) return 0;
    try {
        std::ostringstream oss;
        Poco::JSON::Stringifier::stringify(arr, oss);
        return static_cast<size_t>(oss.tellp());
    } catch (...) {
        return 0;
    }
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

// ── Elision spool ────────────────────────────────────────────────
// Build-time elision saves the full tool result to the conversation's
// Vars\ lane and the marker names that file, so an elided result
// becomes a variable, the same as var-store demotion: grep or
// read_range the slice that is needed.  A marker with nothing behind
// it makes the model re-run the tool (or guess a Vars\ filename that
// never existed).
//
// Idempotent by construction: the filename is a hash of the content,
// so every build (and every session) maps the same result to the same
// file, and the marker text is byte-stable across builds.  Results
// that were already demoted at append time point at their existing
// spool instead of being copied.  Any failure falls back to the plain
// marker.

constexpr size_t kMinElisionSpoolBytes = 2048;   // tiny results: marker only

uint64_t Fnv1a64(const std::string& s)
{
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    return h;
}

// Path quoted on an append-time handle card's "file:" line, or "".
std::string ExistingVarHandlePath(const std::string& content)
{
    const size_t sentinel = content.find(varstore::kCardSentinel);
    if (sentinel == std::string::npos) return {};
    const std::string key = "\nfile: ";
    size_t p = content.find(key, sentinel);
    if (p == std::string::npos) return {};
    p += key.size();
    const size_t eol = content.find_first_of("\r\n", p);
    std::string path = content.substr(p, eol == std::string::npos ? std::string::npos : eol - p);
    const size_t stats = path.find("  (");
    if (stats != std::string::npos) path.resize(stats);
    return path;
}

// Writes `content` to <workspace>\Vars\elided_<hash>.txt (or reuses
// an identical existing file) and returns the relative path, or "".
// `minBytes` defaults to the tool-result floor; elided tool-call
// argument values pass 0 (every cut value is already a few hundred
// bytes, and the spool is what makes a copied marker recoverable).
std::string SpoolElidedToolResult(const std::string& content,
                                  const std::string& workspaceDirUtf8,
                                  size_t minBytes = kMinElisionSpoolBytes)
{
    if (workspaceDirUtf8.empty() || content.empty() || content.size() < minBytes)
        return {};

    const std::string varsAbs = varstore::EnsureVarsDir(workspaceDirUtf8);
    if (varsAbs.empty()) return {};

    char name[48];
    std::snprintf(name, sizeof(name), "elided_%016llx.txt",
                  static_cast<unsigned long long>(Fnv1a64(content)));
    const std::string relPath = std::string(varstore::kVarsLaneName) + "\\" + name;

    try {
        const std::filesystem::path abs =
            std::filesystem::path(path_safety::Utf8ToWide(varsAbs)) /
            path_safety::Utf8ToWide(name);

        std::error_code ec;
        if (std::filesystem::exists(abs, ec) &&
            std::filesystem::file_size(abs, ec) == content.size() && !ec) {
            return relPath;   // written by an earlier build or session
        }

        std::ofstream f(abs, std::ios::binary | std::ios::trunc);
        if (!f) return {};
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
        f.close();
        if (!f) {
            std::filesystem::remove(abs, ec);
            return {};
        }
    } catch (...) {
        return {};
    }
    return relPath;
}

// Replace the fenced BODY (and optional ERROR BODY) with an elision
// marker.  Returns the compacted form.  If the input doesn't look
// like a tool result, returns it unchanged.
//
// `spoolRelPath`, when non-empty, names a Vars\ file holding the full
// result; the marker then points there instead of at a rerun.
//
// The marker embeds the echoed command as plain text for auditability,
// but only read-only tools get a replay hint.  Mutating tools must not
// be rerun just because the old output was compacted away.
std::string ElideToolResultBody(const std::string& content,
                                const std::string& spoolRelPath = std::string())
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

    if (!spoolRelPath.empty()) {
        out << "[body elided to fit context. The full result is saved as a variable at "
            << spoolRelPath
            << " - grep or read_range that file for the lines you need instead of rerunning `"
            << callShown << "`.]\n";
    } else if (IsReplaySafeTool(tag)) {
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
    MarkDirty(/*contentActivity=*/true);
}

void ChatHistory::AddAssistantMessage(const std::string& content, const std::string& model)
{
    m_messages.push_back(CreateMessage("assistant", content, model));
    MarkDirty(/*contentActivity=*/true);
}

// ── Native sidecar fields ──────────────────────────────────────

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
            MarkDirty(/*contentActivity=*/true);
        }
    } catch (...) {
        // Drop silently — request builder treats this assistant
        // message as plain prose.
    }
}

void ChatHistory::SetLastAssistantResponsesOutput(const std::string& outputJson)
{
    if (outputJson.empty()) return;
    if (m_messages.empty()) return;

    auto& last = m_messages.back();
    try {
        if (last->getValue<std::string>("role") != "assistant") return;
    } catch (...) { return; }

    try {
        Poco::JSON::Parser parser;
        auto var = parser.parse(outputJson);
        Poco::JSON::Array::Ptr arr = var.extract<Poco::JSON::Array::Ptr>();
        if (arr && arr->size() > 0) {
            last->set(lb_responses::kOutputSidecarKey(), arr);
            MarkDirty(/*contentActivity=*/true);
        }
    } catch (...) {
        // Drop silently — the adapter rebuilds function_call items from
        // tool_calls without reasoning replay when the sidecar is absent.
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
        MarkDirty(/*contentActivity=*/true);
    }
}

static Poco::JSON::Array::Ptr BuildToolImagesArray(
    const std::vector<std::pair<std::string, std::string>>& toolImages)
{
    Poco::JSON::Array::Ptr arr = new Poco::JSON::Array;
    for (const auto& img : toolImages) {
        if (img.first.empty()) continue;
        Poco::JSON::Object::Ptr o = new Poco::JSON::Object;
        o->set("path", img.first);
        o->set("mime_type", img.second.empty() ? std::string("image/png") : img.second);
        arr->add(o);
    }
    return arr;
}

void ChatHistory::AddToolResultMessage(const std::string& toolCallId,
                                        const std::string& content,
                                        const std::vector<std::pair<std::string, std::string>>& toolImages)
{
    auto msg = CreateMessage("user", content);
    if (!toolCallId.empty()) {
        // Sidecar field — read by BuildChatRequestJson when the
        // active protocol is Native, ignored under XML.  Both
        // protocols render the same way in chat (the [tool: NAME]
        // text in `content` drives display).
        msg->set("tool_call_id", toolCallId);
    }
    if (!toolImages.empty()) {
        Poco::JSON::Array::Ptr arr = BuildToolImagesArray(toolImages);
        if (arr->size() > 0) msg->set("tool_images", arr);
    }
    m_messages.push_back(msg);
    MarkDirty(/*contentActivity=*/true);
}

void ChatHistory::SetLastMessageToolImages(
    const std::vector<std::pair<std::string, std::string>>& toolImages)
{
    if (toolImages.empty() || m_messages.empty()) return;
    Poco::JSON::Array::Ptr arr = BuildToolImagesArray(toolImages);
    if (arr->size() == 0) return;
    m_messages.back()->set("tool_images", arr);
    MarkDirty(/*contentActivity=*/true);
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
    m_contentActivityPending = false;
    m_toolCwd.clear();
    m_toolTimeoutMs = 0;
    m_thinkOverride = ThinkOverride::Auto;
    m_reasoningDialect = ReasoningDialect::OpenRouterStyle;  // transient; re-set at next turn start
    m_responsesApi = false;                                  // transient; same lifetime
    m_projectId.clear();
    m_projectName.clear();
    m_projectRoot.clear();
    m_pinned = false;
    m_archived = false;
    m_modelSelection.clear();
    m_chatApprovedTools.clear();
    m_chatApprovalTrustEnabled = false;
    m_chatWriteRoots.clear();
    m_dirty = false;
    m_revision = 0;
}

bool ChatHistory::GrantWriteRootForChat(const std::string& canonicalRoot)
{
    if (canonicalRoot.empty()) return false;

    const std::string normalized =
        tool_path_safety::NormalizeForCompare(canonicalRoot);
    if (normalized.empty()) return false;

    for (const std::string& existing : m_chatWriteRoots) {
        if (tool_path_safety::NormalizeForCompare(existing) == normalized)
            return true;
    }

    m_chatWriteRoots.push_back(canonicalRoot);
    return true;
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
    // Projects add durable metadata that the user can set before
    // typing a message, so metadata-only conversations are saved too,
    // not just chats with at least one message.  If an empty
    // metadata-only conversation has already been saved, HasFilePath()
    // lets later metadata clears persist too.
    return !m_messages.empty()
        || HasProject()
        || m_pinned
        || m_archived
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
    // Fixed, conservative estimate for the meter's fallback (before an
    // exact usage report).  The ELISION budget does not use this
    // constant once a usage report has calibrated it (elision_budget.h);
    // over-estimating here only makes the meter read high, while
    // over-estimating in elision would waste the window.
    return (size_t)((double)bytes / kBytesPerToken);
}

double ChatHistory::ElisionBudgetFraction()
{
    return kBudgetFraction;
}

size_t ChatHistory::EstimateNextRequestTokens(const std::string& model,
                                              int contextTokens,
                                              bool* wouldElide) const
{
    size_t bytes = EstimateHistoryBytes();
    bool capped = false;
    if (contextTokens > 0) {
        const size_t budget = (size_t)((double)contextTokens *
                                       m_elisionBpt.Current(model) * kBudgetFraction);
        if (bytes > budget) { bytes = budget; capped = true; }
    }
    if (wouldElide) *wouldElide = capped;
    const double measured = m_elisionBpt.MeasuredFor(model);
    const double bpt = measured > 0.0 ? measured : kBytesPerToken;
    return (size_t)((double)bytes / bpt);
}

size_t ChatHistory::EstimateHistoryBytes() const
{
    size_t bytes = 0;
    for (const auto& msg : m_messages) {
        if (!msg) continue;
        try {
            if (msg->has("content") && !msg->isNull("content"))
                bytes += msg->getValue<std::string>("content").size();
        } catch (...) { /* non-string content — skip */ }
        // tool_calls sidecar: name + arguments lengths plus a fixed
        // per-call overhead.  The old flat 200 bytes hid what dominates
        // long agent chats (multi-KB script arguments).  Walked, not
        // stringified: this runs on every revision change while the
        // meter has no exact anchor, including during streaming.
        if (msg->has("tool_calls")) {
            size_t callBytes = 0;
            try {
                const auto calls = msg->getArray("tool_calls");
                for (size_t k = 0; calls && k < calls->size(); ++k) {
                    callBytes += 64;   // id/type/keys/quoting
                    const auto call = calls->getObject((unsigned)k);
                    const auto fn = call ? call->getObject("function") : nullptr;
                    if (!fn) continue;
                    if (fn->has("name") && fn->get("name").isString())
                        callBytes += fn->getValue<std::string>("name").size();
                    if (fn->has("arguments") && fn->get("arguments").isString())
                        callBytes += fn->getValue<std::string>("arguments").size();
                }
            } catch (...) { callBytes = 0; }
            bytes += callBytes ? callBytes : 200;
        }
        bytes += 40;                               // per-message JSON + role
    }
    // Include content still sitting in the streaming buffer that hasn't
    // been synced to the JSON object yet (sub-4KiB partial replies).
    bytes += m_streamBuffer.size();
    return bytes;
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
    m_lastBuildArgsElidedCount = 0;

    // Image-carrier projection flag for this build; see
    // LastBuildProjectedImages().  Also reset inside stringifyWire so
    // a second (post-elision/sanitizer) pass always overwrites it.
    m_lastBuildProjectedImages = false;

    // Make sure any in-flight streamed content is reflected in the JSON
    // objects before we build the wire request.  AppendToLastAssistantMessage
    // amortizes the sync; this is the read-side counterpart.
    FlushStreamBuffer();

    // ── Pre-parse the tools array once ──────────────────────────
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

    // ── Step 1: gather wire messages in original form ───────────
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
        // Native sidecars; only consulted when nativeProtocol is true.
        // Both empty for messages that didn't carry the matching field
        // on disk.
        std::string             toolCallId;     // user message answering an assistant call
        Poco::JSON::Array::Ptr  toolCalls;      // assistant message that emitted calls
        // OpenAI Responses sidecar: the verbatim output array of the
        // turn that emitted toolCalls.  Projected only when the send
        // target is a Responses endpoint AND toolCalls survives the
        // sanitizer — it is meaningless (and rejected by the provider)
        // without the function calls it belongs to.
        Poco::JSON::Array::Ptr  responsesOutput;
        // Image-projection sidecars.  attachMeta is the message's
        // stored attachments array (kind/mime_type/storage_path);
        // isImageCarrier marks the single message whose persisted
        // images ride the wire this request.  A flag on the element
        // (not an index) so the elision/sanitizer phases that mutate
        // or remove wire entries can never orphan the selection.
        Poco::JSON::Array::Ptr  attachMeta;
        bool                    isImageCarrier = false;
        // view_image sidecar: {path, mime_type} entries of images the
        // model asked to see.  projectToolImages marks the tool results
        // of the CURRENT turn whose images ride this request (see the
        // tool-image carrier below); an unprojected message with images
        // gets a short "no longer attached" note instead.
        Poco::JSON::Array::Ptr  toolImages;
        bool                    projectToolImages = false;
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

        // The replay UI already hides these legacy Goal messages. Filter
        // them here too, before token budgeting and either wire protocol.
        if (IsRetiredGoalContinuationMessage(role, content)) continue;

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
        //
        // A blank USER message is a different case: attachments live in a
        // sidecar array, not in content, so a message can be blank here and
        // still carry a persisted image that the carrier pass below must
        // project.  Dropping it would lose the image AND make the carrier
        // loop (which scans `wire` backwards for the newest real user
        // message) select the PREVIOUS turn instead, re-sending stale
        // images.  The UI injects default text ("What is in this image?")
        // on the normal send path, so this matters for imported
        // conversations and any caller that skips that injection.
        const bool hasToolCalls = msg->has("tool_calls");
        const bool blankContent =
            content.find_first_not_of(" \t\r\n") == std::string::npos;
        const bool blankButCarriesImage =
            blankContent && role == "user" && HasPersistedImageAttachment(msg);
        if (blankContent && !(nativeProtocol && hasToolCalls) &&
            !blankButCarriesImage) continue;

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
            if (m_responsesApi && msg->has(lb_responses::kOutputSidecarKey())) {
                try {
                    w.responsesOutput =
                        msg->getArray(lb_responses::kOutputSidecarKey());
                } catch (...) { /* leave null */ }
            }
        }
        if (w.role == "user" && msg->has("tool_images")) {
            try {
                w.toolImages = msg->getArray("tool_images");
                if (w.toolImages && w.toolImages->size() == 0)
                    w.toolImages.reset();
            } catch (...) { w.toolImages.reset(); }
        }
        if (w.role == "user" && !w.isToolResult && msg->has("attachments")) {
            try {
                w.attachMeta = msg->getArray("attachments");
            } catch (...) { /* leave null */ }
        }

        wire.push_back(std::move(w));
    }

    // ── Step 1b: strip stale live-loop budget trailers ──────────
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

    // ── Step 1c: image carrier selection ─────────────────────────
    // User-attached images are persisted to the conversation's
    // attachments sidecar folder.  Every request rebuild (each
    // agent-loop iteration, or a follow-up turn) must re-project them,
    // or vision models go blind after one tool call.
    //
    // Policy: project images from the NEWEST real user message (not a
    // tool result under either protocol), and only that one.  This
    // keeps the image visible for the entire agent loop of the attach
    // turn and for immediate re-asks, while bounding per-request
    // upload cost to one message's images.  Once the user sends a
    // newer message, older images age off the wire — which also means
    // switching to a text-only model afterwards keeps working.
    for (size_t i = wire.size(); i-- > 0;) {
        WireMsg& w = wire[i];
        if (w.role != "user" || w.isToolResult || !w.toolCallId.empty())
            continue;
        // Newest real user message found — carrier iff it has at
        // least one persisted image attachment.  Same probe the
        // wire-build loop above uses to decide whether a blank
        // message was worth keeping, so the two stay consistent.
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

    // ── Step 1d: tool-image carrier (view_image) ─────────────────
    // Images the model pulled in with view_image (photos extracted from
    // a .zip, downloaded files, script output, any local image) live in
    // a "tool_images" sidecar on the tool-result message.  They follow
    // the same "current turn only" policy as composer images:
    //   * every tool result AFTER the newest real user message (this
    //     turn's agent loop), and
    //   * the contiguous run of tool results immediately BEFORE it —
    //     that is where a user-typed "/view_image ..." lands when the
    //     user then asks a question about the picture.
    // Newest first, whole messages only, bounded by
    // kMaxToolImagesPerRequest so a loop that views batch after batch
    // cannot grow the request without limit.  Older batches age off
    // with a note telling the model to call view_image again.
    {
        constexpr unsigned kMaxToolImagesPerRequest = 8;
        size_t newestRealUser = wire.size();
        for (size_t i = wire.size(); i-- > 0;) {
            const WireMsg& w = wire[i];
            if (w.role == "user" && !w.isToolResult && w.toolCallId.empty()) {
                newestRealUser = i;
                break;
            }
        }
        size_t firstEligible = 0;
        if (newestRealUser < wire.size()) {
            firstEligible = newestRealUser;
            while (firstEligible > 0) {
                const WireMsg& prev = wire[firstEligible - 1];
                const bool prevIsToolResult =
                    prev.role == "user" && (prev.isToolResult || !prev.toolCallId.empty());
                if (!prevIsToolResult) break;
                --firstEligible;
            }
        }
        unsigned remaining = kMaxToolImagesPerRequest;
        for (size_t i = wire.size(); i-- > firstEligible;) {
            WireMsg& w = wire[i];
            if (!w.toolImages || i == newestRealUser) continue;
            const unsigned count = static_cast<unsigned>(w.toolImages->size());
            if (count == 0 || count > remaining) continue;
            w.projectToolImages = true;
            remaining -= count;
            if (remaining == 0) break;
        }
    }

    // Resolved once; the stringify lambda below may run twice (before
    // and after elision) and must not recompute path plumbing.
    const std::string imageChatDir =
        m_filePath.empty() ? std::string() : GetChatFolder(m_filePath);

    // ── Step 2: optional compaction ─────────────────────────────
    // Only runs when the caller provided a context-window hint.
    // Measure the stringified body, and if it exceeds the budget,
    // first shorten the arguments of old tool calls (step 2a, native
    // protocol), then elide tool-result bodies (step 2b), oldest-first
    // in both, until we're under.
    // The last `kMinPreservedResults` tool results are exempt —
    // that's the recent context the model needs to keep reasoning
    // coherently.
    auto stringifyWire = [&]() -> std::string {
        // This lambda can run twice (initial build, then once more if
        // elision or the native sanitizer dirtied the wire).  Reset the
        // projection flag per pass so the last pass — the one whose
        // output is actually returned — decides its value.
        m_lastBuildProjectedImages = false;

        Poco::JSON::Object::Ptr root = new Poco::JSON::Object;
        root->set("model", model);
        root->set("stream", stream);

        // OpenAI prompt caching routes on the first few hundred prompt
        // tokens plus an optional prompt_cache_key.  Every LlamaBoss chat
        // shares the same system prompt and tool catalog, so without a key
        // all chats compete for one cache slot and a fast agent loop spills
        // across machines, leaving the cache stuck at the shared prefix.
        // A stable per-conversation key keeps one chat on one cache.
        // Responses endpoints only: lb_responses::BuildChatRequest copies
        // it and every other endpoint keeps its request bytes unchanged.
        if (m_responsesApi) {
            if (m_createdAt.empty()) m_createdAt = CurrentTimestamp();
            char key[24];
            std::snprintf(key, sizeof(key), "lb-%016llx",
                          static_cast<unsigned long long>(Fnv1a64(m_createdAt)));
            root->set("prompt_cache_key", std::string(key));
        }

        // Ask the server to report token usage on the stream.  Without
        // this an OpenAI-compatible endpoint (llama-server included)
        // emits no `usage` object while streaming, so the exact context
        // anchor in MyFrame never fires and the meter is stuck on the
        // kBytesPerToken heuristic forever.  Request-level field only —
        // the messages array is untouched, so the prompt prefix that
        // llama-server's KV cache keys on is unchanged.
        if (stream) {
            Poco::JSON::Object::Ptr streamOpts = new Poco::JSON::Object;
            streamOpts->set("include_usage", true);
            root->set("stream_options", streamOpts);
        }

        ApplyLocalLlamaSampling(root, model, agentSamplingProfile);

        // ── Reasoning override (/think off|low|medium|high|on) ───
        // Auto sends nothing.  Local llama-server targets (.gguf model
        // value) get chat_template_kwargs.enable_thinking, which
        // hybrid-reasoning chat templates (Qwen3 family and similar)
        // honor when the server runs with --jinja; templates that
        // never reference the flag ignore it, and llama-server
        // ignores the field entirely without --jinja.  Templates
        // speak only a boolean, so any effort level enables.
        //
        // Remote targets branch on the dialect the frame resolved
        // from the endpoint (SetActiveReasoningDialect):
        //   * OpenRouter style — a "reasoning" object.  On/Off send
        //     {"enabled": bool}; Low/Medium/High use the documented
        //     {"effort": "..."}.
        //   * OpenAI style (direct api.openai.com, which rejects
        //     unknown body fields — the reasoning object 400s
        //     there) — the "reasoning_effort" string.  Off maps to
        //     "none" when supported (Astra is clamped to "low").
        //     "none" also unblocks function tools on models
        //     whose server-side default effort conflicts with tools
        //     (gpt-5.6-luna on /v1/chat/completions).  Plain On maps
        //     to "medium", the provider default when reasoning is on.
        //   * Template style (endpoint "reasoning_dialect":
        //     "template") — chat_template_kwargs.enable_thinking,
        //     the same shape as the local lane, for remote
        //     OpenAI-compatible servers that apply the model's own
        //     chat template (FreeToken, vLLM, SGLang).  Templates
        //     speak only a boolean, so effort levels collapse to
        //     enabled.  Endpoint opt-in only — never sniffed.
        //
        // A strict remote endpoint that rejects the field surfaces
        // its error in chat, and /think auto restores the untouched
        // request.  Skipped on image-output turns so provider
        // routing for image models is unaffected.
        if (m_thinkOverride != ThinkOverride::Auto && !imageOutput) {
            const bool wantThink = (m_thinkOverride != ThinkOverride::Off);
            const char* effort = nullptr;   // set only for the levels
            switch (m_thinkOverride) {
                case ThinkOverride::Low:    effort = "low";    break;
                case ThinkOverride::Medium: effort = "medium"; break;
                case ThinkOverride::High:   effort = "high";   break;
                default: break;
            }
            if (LooksLikeLocalGgufModel(model)) {
                Poco::JSON::Object::Ptr kwargs = new Poco::JSON::Object;
                kwargs->set("enable_thinking", wantThink);
                root->set("chat_template_kwargs", kwargs);
            } else if (m_reasoningDialect ==
                       ReasoningDialect::TemplateKwargs) {
                // Remote server that applies the model's own chat
                // template — same wire shape as the local lane above.
                Poco::JSON::Object::Ptr kwargs = new Poco::JSON::Object;
                kwargs->set("enable_thinking", wantThink);
                root->set("chat_template_kwargs", kwargs);
            } else if (m_reasoningDialect == ReasoningDialect::OpenAIStyle) {
                // Backstop for saved Off settings and callers outside the
                // main UI send path. Astra requires at least low reasoning.
                root->set("reasoning_effort", lb_reasoning::OpenAIEffort(
                    model, !wantThink ? "none" : (effort ? effort : "medium")));
            } else {
                Poco::JSON::Object::Ptr reasoning = new Poco::JSON::Object;
                if (effort) reasoning->set("effort", std::string(effort));
                else        reasoning->set("enabled", wantThink);
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

        // Tool-image projection helpers.  appendToolImageParts adds a
        // short text label plus one image_url part per loadable image
        // of `w` to `parts`; returns how many images actually loaded.
        auto appendToolImageParts = [&](const WireMsg& w,
                                        Poco::JSON::Array::Ptr parts) -> int {
            int loaded = 0;
            for (unsigned k = 0; k < w.toolImages->size(); ++k) {
                Poco::JSON::Object::Ptr img = w.toolImages->getObject(k);
                if (!img) continue;
                const std::string path = img->optValue<std::string>("path", "");
                if (path.empty()) continue;
                const size_t sep = path.find_last_of("\\/");
                const std::string name =
                    (sep == std::string::npos) ? path : path.substr(sep + 1);

                const std::string uri = LoadImageAsDataUriCached(
                    path, img->optValue<std::string>("mime_type", ""));

                Poco::JSON::Object::Ptr label = new Poco::JSON::Object;
                label->set("type", "text");
                label->set("text", "Image " + std::to_string(k + 1) + ": " + name +
                                   (uri.empty() ? " (could not be loaded -- the file "
                                                  "was moved or deleted)" : ""));
                parts->add(label);
                if (uri.empty()) continue;

                Poco::JSON::Object::Ptr imageUrl = new Poco::JSON::Object;
                imageUrl->set("url", uri);
                Poco::JSON::Object::Ptr part = new Poco::JSON::Object;
                part->set("type", "image_url");
                part->set("image_url", imageUrl);
                parts->add(part);
                ++loaded;
            }
            return loaded;
        };

        const std::string kToolImagesAgedOffNote =
            "\n\n[The images from this view_image call are no longer attached "
            "to the conversation. Call view_image again if you need to look at them.]";

        // Native protocol: role:"tool" content must be a plain string
        // (OpenAI chat spec, the Responses adapter, and most llama.cpp
        // chat templates reject image parts there), and every tool reply
        // must directly follow its assistant tool_calls turn.  So images
        // from native tool results are buffered and emitted as ONE
        // synthetic user message right after the contiguous run of tool
        // replies — the conventional "tool returned an image" shape.
        Poco::JSON::Array::Ptr pendingToolImageParts;
        int pendingToolImageCount = 0;
        auto flushPendingToolImages = [&]() {
            if (!pendingToolImageParts) return;
            if (pendingToolImageCount > 0) {
                Poco::JSON::Array::Ptr parts = new Poco::JSON::Array;
                Poco::JSON::Object::Ptr intro = new Poco::JSON::Object;
                intro->set("type", "text");
                intro->set("text", std::string(
                    "[view_image output] These are the images requested by the "
                    "view_image tool call above. This message was generated by "
                    "LlamaBoss, not typed by the user."));
                parts->add(intro);
                for (unsigned k = 0; k < pendingToolImageParts->size(); ++k)
                    parts->add(pendingToolImageParts->get(k));
                Poco::JSON::Object::Ptr um = new Poco::JSON::Object;
                um->set("role", std::string("user"));
                um->set("content", parts);
                arr->add(um);
            }
            pendingToolImageParts.reset();
            pendingToolImageCount = 0;
        };

        for (const auto& w : wire) {
            // A buffered native tool-image message goes out as soon as
            // the run of role:"tool" replies ends.
            if (pendingToolImageParts &&
                !(nativeProtocol && w.role == "user" && !w.toolCallId.empty())) {
                flushPendingToolImages();
            }

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
                // Responses replay material.  The adapter strips this
                // key while converting; it is only ever attached when
                // the target is a Responses endpoint (m_responsesApi).
                if (w.responsesOutput)
                    m->set(lb_responses::kOutputSidecarKey(), w.responsesOutput);
            }
            else if (nativeProtocol && w.role == "user" && !w.toolCallId.empty()) {
                m->set("role", std::string("tool"));
                m->set("tool_call_id", w.toolCallId);
                if (w.toolImages && w.projectToolImages) {
                    if (!pendingToolImageParts)
                        pendingToolImageParts = new Poco::JSON::Array;
                    pendingToolImageCount +=
                        appendToolImageParts(w, pendingToolImageParts);
                    m->set("content", w.content);
                } else if (w.toolImages) {
                    m->set("content", w.content + kToolImagesAgedOffNote);
                } else {
                    m->set("content", w.content);
                }
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
                if (w.isImageCarrier && !imageChatDir.empty()) {
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
                            imageChatDir + "/" + rel,
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
                        // Text part only when there is text: blank user
                        // messages carrying only images reach this point,
                        // and an empty {"type":"text","text":""} part is
                        // rejected outright by some OpenAI-compatible
                        // providers.  Images alone are a valid content
                        // array.
                        if (!w.content.empty()) {
                            Poco::JSON::Object::Ptr textPart =
                                new Poco::JSON::Object;
                            textPart->set("type", "text");
                            textPart->set("text", w.content);
                            parts->add(textPart);
                        }
                        m->set("content", parts);
                        emittedParts = true;
                        m_lastBuildProjectedImages = true;
                    }
                }
                // XML protocol (and slash-command results): the tool
                // result is an ordinary user message, so its images ride
                // inline as a multimodal content array — text first,
                // then the labelled images.
                if (!emittedParts && w.toolImages && w.projectToolImages &&
                    w.role == "user") {
                    Poco::JSON::Array::Ptr parts = new Poco::JSON::Array;
                    Poco::JSON::Object::Ptr textPart = new Poco::JSON::Object;
                    textPart->set("type", "text");
                    textPart->set("text", w.content);
                    parts->add(textPart);
                    if (appendToolImageParts(w, parts) > 0) {
                        m->set("content", parts);
                        emittedParts = true;
                    }
                }
                if (!emittedParts) {
                    if (w.toolImages && !w.projectToolImages && w.role == "user")
                        m->set("content", w.content + kToolImagesAgedOffNote);
                    else
                        m->set("content", w.content);
                }
            }

            arr->add(m);
        }
        flushPendingToolImages();
        root->set("messages", arr);

        // tools field (Native protocol only).  Sits at the root level
        // alongside messages; the model sees the tool catalog and may
        // emit structured tool_calls in its response.
        if (toolsArr) {
            root->set("tools", toolsArr);

            // Native multi-call responses are supported, so the request
            // advertises parallel tool calling; AgentController is still
            // the enforcement layer.  It accepts a batch only when every
            // invocation carries a usable unique id and every ToolSpec has
            // explicitly opted in via batchSafe.  Mutating, approval-gated,
            // policy-enforced, or otherwise non-batchable tools fall back to
            // the conservative single-call transcript path.
            //
            // Execution remains ordered/sequential inside LlamaBoss.  This
            // flag permits the model to RETURN several independent calls in
            // one assistant response; it does not make dispatch concurrent.
            root->set("parallel_tool_calls", true);
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
        // Bytes-per-token: learned from exact usage for this model
        // (elision_budget.h), kBytesPerToken (3.0) until the first
        // usable report.  The meter keeps kBytesPerToken.
        const double elisionBpt = m_elisionBpt.Current(model);
        const size_t budget = (size_t)((double)contextTokens
                                       * elisionBpt * kBudgetFraction);

        // Image data URIs are compared at an estimated token cost, not
        // their base64 bytes (elision_budget.h, BudgetedBodyBytes).  A
        // couple of screenshots otherwise exceeded the budget on their
        // own and every elidable text result was elided for nothing.
        const size_t budgetedBodySize =
            lb_elision::BudgetedBodyBytes(body, elisionBpt);

        if (budgetedBodySize > budget) {
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
            size_t estimatedBodySize = budgetedBodySize;

            // Wire index of the first PRESERVED tool result (the newest
            // kMinPreservedResults stay intact).  wire.size() when every
            // tool result is a candidate.
            size_t preservedStart = wire.size();
            {
                size_t seen = 0;
                for (size_t i = 0; i < wire.size(); ++i) {
                    if (!wire[i].isToolResult) continue;
                    if (++seen > maxCandidates) { preservedStart = i; break; }
                }
            }

            // ── Step 2a: shorten arguments of old tool calls ─────
            // Runs BEFORE result elision: the arguments of a call that
            // already ran (2-4 KB PowerShell scripts, write_file bodies)
            // are the least valuable bytes in a long agent chat, and the
            // result echo still names the call.
            //
            // Eligible: an assistant tool_calls turn (native protocol;
            // XML-protocol calls live in content and are untouched)
            // whose native replies all sit before preservedStart, so the
            // calls belonging to the recent, preserved results keep
            // their exact arguments.  Oldest first, stop once under
            // budget.  The stored message is never touched:
            // ShortenToolCallsArray returns a deep copy.  Each cut value
            // is spooled to Vars\ and the marker names the file, because
            // models DO copy an old call to rerun it; the copy is then
            // rejected at validation (ContainsArgElisionMarker) and the
            // rejection points at the spool.
            //
            // Responses replay: the saved output sidecar must match the
            // executed arguments byte-for-byte (openai_responses.h throws
            // "does not match the executed tool call" otherwise), so a
            // shortened turn drops its sidecar and replays via the legacy
            // function_call path.  That turn loses its encrypted
            // reasoning items, which also frees their bytes; the exact
            // delta below counts them.
            if (nativeProtocol) {
                // Full text of every cut value goes to Vars\ (same
                // content-hash files and cache as elided results), and
                // the marker names it.  Empty workspace: marker only.
                const lb_toolcall_elision::SpoolFn spoolArg =
                    [this](const std::string& full) -> std::string {
                        if (m_elisionSpoolWorkspace.empty()) return {};
                        const uint64_t key = Fnv1a64(full);
                        auto hit = m_elisionSpoolCache.find(key);
                        if (hit != m_elisionSpoolCache.end()) return hit->second;
                        std::string rel = SpoolElidedToolResult(
                            full, m_elisionSpoolWorkspace, /*minBytes=*/0);
                        if (!rel.empty()) m_elisionSpoolCache.emplace(key, rel);
                        return rel;
                    };
                for (size_t i = 0; i < preservedStart && estimatedBodySize > budget; ++i) {
                    WireMsg& w = wire[i];
                    if (w.role != "assistant" || !w.toolCalls) continue;

                    size_t j = i + 1;
                    while (j < wire.size() && wire[j].role == "user" &&
                           !wire[j].toolCallId.empty()) ++j;
                    if (j == i + 1) continue;            // no replies: not completed
                    if (j > preservedStart) continue;    // replies reach preserved results

                    Poco::JSON::Array::Ptr shorter =
                        lb_toolcall_elision::ShortenToolCallsArray(
                            w.toolCalls,
                            lb_toolcall_elision::kKeepChars,
                            lb_toolcall_elision::kMinElideArgBytes,
                            spoolArg);
                    if (!shorter) continue;              // nothing large enough

                    const size_t before = StringifiedJsonSize(w.toolCalls) +
                                          StringifiedJsonSize(w.responsesOutput);
                    const size_t after  = StringifiedJsonSize(shorter);
                    w.toolCalls = shorter;
                    w.responsesOutput.reset();
                    if (before > after) {
                        const size_t saved = before - after;
                        estimatedBodySize = (saved < estimatedBodySize)
                                                ? estimatedBodySize - saved : 0;
                    }
                    ++m_lastBuildArgsElidedCount;
                    wireDirty = true;
                }
            }

            // ── Step 2b: elide old tool-result bodies ────────────
            size_t toolResultsSeen   = 0;

            for (size_t i = 0; i < wire.size() && estimatedBodySize > budget; ++i) {
                if (!wire[i].isToolResult) continue;
                ++toolResultsSeen;
                if (toolResultsSeen > maxCandidates) break;

                const size_t oldSize = wire[i].content.size();
                // Spool first so the marker can name the file.  A result
                // demoted at append time already lives in Vars\: point at
                // that spool rather than copying its handle card.
                std::string spoolRel = ExistingVarHandlePath(wire[i].content);
                if (spoolRel.empty() && !m_elisionSpoolWorkspace.empty()) {
                    const uint64_t key = Fnv1a64(wire[i].content);
                    auto hit = m_elisionSpoolCache.find(key);
                    if (hit != m_elisionSpoolCache.end()) {
                        spoolRel = hit->second;
                    } else {
                        spoolRel = SpoolElidedToolResult(wire[i].content,
                                                         m_elisionSpoolWorkspace);
                        if (!spoolRel.empty()) m_elisionSpoolCache.emplace(key, spoolRel);
                    }
                }
                wire[i].content = ElideToolResultBody(wire[i].content, spoolRel);
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

    // ── Native transcript sanitizer ──────────────────────────────
    // OpenAI/llama-server tool-call history is strict:
    //   assistant + tool_calls[id=A]
    //   role:"tool" + tool_call_id=A
    // must stay paired.  Save/reload, cancel, or partial multi-call
    // execution can leave one side without the other.  Before
    // returning a native request body, sanitize the projected history
    // so it never emits:
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
            // assistant tool-call turn.  This stays strict instead of
            // searching arbitrarily far forward; if a normal
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

    // Context-meter calibration: remember what actually went on the
    // wire.  Set here rather than at the top so it reflects the final
    // body after both the elision pass and the sanitizer re-stringify.
    m_lastBuildRequestBytes = body.size();
    m_lastBuildModel        = model;

    // Context details panel: what the final request is made of.  Content
    // bytes per role from the final wire (post-elision, post-sanitizer),
    // plus base64 image data measured on the body itself so it can be
    // kept out of the text-based token split.
    {
        RequestBreakdown b;
        b.totalBytes   = body.size();
        b.elidedCount  = m_lastBuildElidedCount;
        b.messageCount = static_cast<int>(wire.size());
        if (toolsArr) b.toolsBytes = toolsArrayJson.size();
        for (const WireMsg& w : wire) {
            const size_t c = w.content.size();
            if (w.role == "system") {
                b.systemBytes += c;
            } else if (w.role == "assistant") {
                b.assistantBytes += c;
                if (nativeProtocol && w.toolCalls) {
                    try {
                        std::ostringstream tc;
                        Poco::JSON::Stringifier::stringify(w.toolCalls, tc);
                        b.assistantBytes += static_cast<size_t>(tc.tellp());
                    } catch (...) { /* size is best-effort */ }
                }
                // Responses replay: only the reasoning items are extra
                // (the sidecar's function_call items stand in for the
                // tool_calls already counted above).  Logged to
                // ctx_calibration.tsv for the metrics export.
                if (w.responsesOutput) {
                    try {
                        for (size_t k = 0; k < w.responsesOutput->size(); ++k) {
                            const auto item = w.responsesOutput->getObject(k);
                            if (!item || !item->has("type") ||
                                item->getValue<std::string>("type") != "reasoning")
                                continue;
                            std::ostringstream rs;
                            Poco::JSON::Stringifier::stringify(item, rs);
                            b.replayBytes += static_cast<size_t>(rs.tellp());
                        }
                    } catch (...) { /* size is best-effort */ }
                }
            } else if (w.isToolResult || !w.toolCallId.empty()) {
                b.toolResultBytes += c;
            } else {
                b.userBytes += c;
            }
        }
        static const char kDataImage[] = "data:image/";
        for (size_t pos = body.find(kDataImage); pos != std::string::npos;
             pos = body.find(kDataImage, pos + 1)) {
            size_t end = body.find('"', pos);
            if (end == std::string::npos) end = body.size();
            b.imageBytes += end - pos;
            ++b.imageCount;
            pos = end;
            if (pos >= body.size()) break;
        }
        m_lastBuildBreakdown = b;
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
        MarkDirty(/*contentActivity=*/true);

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
        MarkDirty(/*contentActivity=*/true);
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
        MarkDirty(/*contentActivity=*/true);
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
    SaveSnapshot& outSnapshot,
    bool touchActivityTimestamp)
{
    const std::string savePath = filePath.empty() ? m_filePath : filePath;
    if (savePath.empty() || !HasPersistableContent()) return false;

    // Make any buffered streaming bytes part of the immutable snapshot.
    FlushStreamBuffer();

    try {
        if (m_createdAt.empty()) m_createdAt = CurrentTimestamp();
        // A durable save also runs when merely leaving/reopening a chat.
        // Only pending message activity may advance the sidebar clock;
        // generic dirty state also includes metadata and workspace settings.
        if (touchActivityTimestamp && m_contentActivityPending) {
            m_updatedAt = CurrentTimestamp();
            // Consume at snapshot time, not async commit time. A later message
            // re-arms this flag; an older completion must never clear it.
            // On write failure, retries retain this timestamp in memory.
            m_contentActivityPending = false;
        }
        else if (m_updatedAt.empty()) {
            m_updatedAt = m_createdAt;
        }

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
        snapshot.pinned        = m_pinned;
        snapshot.archived      = m_archived;
        snapshot.modelSelection = m_modelSelection;
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
            // hasAttachments was computed here but left out of the skip
            // condition, so an attachment-only message with empty text was
            // dropped on save even though the branch below knows how to
            // serialize its attachments array.  Normal UI turns substitute
            // default text ("What is in this image?") for empty input, so
            // this is mainly an import / round-trip guard -- but the loss
            // is silent and unrecoverable when it happens.
            if (content.empty() && !hasToolCalls && !hasToolCallId &&
                !hasImages && !hasAttachments) {
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
                if (msg->has(lb_responses::kOutputSidecarKey())) {
                    try {
                        saved.responsesOutputJson = ArrayToJson(
                            msg->getArray(lb_responses::kOutputSidecarKey()));
                    } catch (...) { saved.responsesOutputJson.clear(); }
                }
            }
            if (hasImages) {
                try { saved.imagesJson = ArrayToJson(msg->getArray("images")); }
                catch (...) { saved.imagesJson.clear(); }
            }
            if (msg->has("tool_images")) {
                try { saved.toolImagesJson = ArrayToJson(msg->getArray("tool_images")); }
                catch (...) { saved.toolImagesJson.clear(); }
            }
            snapshot.messages.push_back(std::move(saved));
        }

        // Resolve the optional cosmetic marker on the UI thread because the
        // wx filesystem helpers are not part of the worker contract.
        try {
            const std::string chatDir = GetChatFolder(savePath);
            if (!chatDir.empty() &&
                wxDirExists(wxString::FromUTF8(chatDir))) {
                snapshot.titleMarkerPath =
                    JoinChatPath(chatDir, "_title.txt");
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
        // Older builds reading a level token they don't know fall back
        // to Auto on load (their parser matches only on/off), which is
        // the safe degradation.
        if (snapshot.thinkOverride == ThinkOverride::On)
            root->set("think", std::string("on"));
        else if (snapshot.thinkOverride == ThinkOverride::Off)
            root->set("think", std::string("off"));
        else if (snapshot.thinkOverride == ThinkOverride::Low)
            root->set("think", std::string("low"));
        else if (snapshot.thinkOverride == ThinkOverride::Medium)
            root->set("think", std::string("medium"));
        else if (snapshot.thinkOverride == ThinkOverride::High)
            root->set("think", std::string("high"));

        if (!snapshot.projectId.empty() && !snapshot.projectRoot.empty()) {
            root->set("project_id", snapshot.projectId);
            root->set("project_name", snapshot.projectName);
            root->set("project_root", snapshot.projectRoot);
        }

        // Sidebar organization metadata is sparse for backward
        // compatibility: false values are omitted from the JSON.
        if (snapshot.pinned)
            root->set("pinned", true);
        if (snapshot.archived)
            root->set("archived", true);

        Poco::JSON::Array::Ptr modelsArray = new Poco::JSON::Array;
        for (const auto& model : snapshot.models) modelsArray->add(model);
        root->set("models", modelsArray);
        if (!snapshot.models.empty()) root->set("model", snapshot.models.front());
        // Sparse, like pinned/archived: older builds ignore the key.
        if (!snapshot.modelSelection.empty())
            root->set("model_selection", snapshot.modelSelection);

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
                if (auto arr = ParseArray(msg.responsesOutputJson))
                    saveMsg->set(lb_responses::kOutputSidecarKey(), arr);
            } catch (...) { /* malformed sidecar: omit */ }
            try {
                if (auto arr = ParseArray(msg.imagesJson))
                    saveMsg->set("images", arr);
            } catch (...) { /* malformed sidecar: omit */ }
            try {
                if (auto arr = ParseArray(msg.toolImagesJson))
                    saveMsg->set("tool_images", arr);
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
                             bool durable,
                             bool touchActivityTimestamp)
{
    SaveSnapshot snapshot;
    if (!CreateSaveSnapshot(filePath, models, snapshot,
                            touchActivityTimestamp)) return false;
    if (!WriteSaveSnapshot(snapshot, durable)) return false;

    // Synchronous Save/Save-As owns this transition and may intentionally
    // change the current path.  Async callers use CommitSaveSnapshot directly,
    // which rejects completions for a different active conversation.
    m_filePath = snapshot.filePath;
    CommitSaveSnapshot(snapshot);
    return true;
}

bool ChatHistory::SaveToFile(const std::string& filePath, const std::string& model,
                             bool durable,
                             bool touchActivityTimestamp)
{
    return SaveToFile(filePath, std::vector<std::string>{ model }, durable,
                      touchActivityTimestamp);
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
        // malformed array leaves this ChatHistory exactly as it was —
        // the previously loaded conversation stays intact, and the `false`
        // return is truthful.  This matters because m_filePath is left
        // pointing at the prior conversation on failure, so a half-mutated
        // state could otherwise be autosaved over a perfectly good file.
        //
        // Title and creation time keep their absent-field behavior.
        // Activity time is resolved from this file below, never another
        // chat.  Fields that are "absent => cleared" (tool/project) start
        // empty.
        std::string   newTitle      = m_title;
        std::string   newCreatedAt  = m_createdAt;
        std::string   newUpdatedAt; // Never inherit another chat's activity time.
        std::string   newToolCwd;
        unsigned long newToolTimeoutMs = 0;
        ThinkOverride newThinkOverride = ThinkOverride::Auto;
        std::string   newProjectId;
        std::string   newProjectName;
        std::string   newProjectRoot;
        bool          newPinned = false;
        bool          newArchived = false;
        std::string   newModelSelection;
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
        // Preserve the sidebar's existing file-time fallback for legacy chats
        // BEFORE the next durable save rewrites their filesystem timestamp.
        if (newUpdatedAt.empty()) {
            try {
                newUpdatedAt = Poco::DateTimeFormatter::format(
                    Poco::File(filePath).getLastModified(),
                    Poco::DateTimeFormat::ISO8601_FORMAT);
            } catch (...) {
                newUpdatedAt = newCreatedAt;
            }
        }

        // Tool execution context — locals default to empty/0,
        // overwritten only if the key is present in the file.
        if (root->has("tool_cwd")) {
            // Chats saved before the Workflows -> Chats rename may pin a
            // /cd into LlamaBoss\Workflows\chat_<id>\...; follow the
            // folder to its migrated home.  Unmigrated paths pass through.
            newToolCwd = RemapLegacyChatPath(
                root->getValue<std::string>("tool_cwd"));
        }
        if (root->has("tool_timeout_ms")) {
            newToolTimeoutMs =
                (unsigned long)root->getValue<Poco::UInt64>("tool_timeout_ms");
        }
        if (root->has("think")) {
            const std::string t = root->getValue<std::string>("think");
            if (t == "on")          newThinkOverride = ThinkOverride::On;
            else if (t == "off")    newThinkOverride = ThinkOverride::Off;
            else if (t == "low")    newThinkOverride = ThinkOverride::Low;
            else if (t == "medium") newThinkOverride = ThinkOverride::Medium;
            else if (t == "high")   newThinkOverride = ThinkOverride::High;
            // anything else (including future values) loads as Auto
        }

        // Optional long-lived project association.
        if (root->has("project_id")) {
            newProjectId = root->getValue<std::string>("project_id");
        }
        if (root->has("project_name")) {
            newProjectName = root->getValue<std::string>("project_name");
        }
        if (root->has("project_root")) {
            newProjectRoot = root->getValue<std::string>("project_root");
        }

        if (root->has("pinned")) {
            newPinned = root->getValue<bool>("pinned");
        }
        if (root->has("archived")) {
            newArchived = root->getValue<bool>("archived");
        }
        if (root->has("model_selection")) {
            // Tolerate a non-string value: fall back to the legacy
            // model-id resolution rather than failing the whole load.
            try { newModelSelection = root->getValue<std::string>("model_selection"); }
            catch (...) { newModelSelection.clear(); }
        }

        // A "goal" block from older conversations (the retired Goals
        // feature) is intentionally ignored; the next save drops it.

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

                // Restore native function-calling sidecars.  Absent in
                // older files, harmless on XML conversations
                // (BuildChatRequestJson ignores them when
                // nativeProtocol=false).
                if (msgObj->has("tool_call_id")) {
                    loadedMsg->set("tool_call_id",
                                   msgObj->getValue<std::string>("tool_call_id"));
                }
                if (msgObj->has("tool_calls")) {
                    loadedMsg->set("tool_calls", msgObj->getArray("tool_calls"));
                    // Responses reasoning-replay sidecar; only ever
                    // written next to tool_calls.
                    if (msgObj->has(lb_responses::kOutputSidecarKey())) {
                        try {
                            loadedMsg->set(lb_responses::kOutputSidecarKey(),
                                msgObj->getArray(lb_responses::kOutputSidecarKey()));
                        } catch (...) { /* malformed — skip */ }
                    }
                }

                // Generated-images sidecar (absent in older files).
                if (msgObj->has("images")) {
                    try {
                        loadedMsg->set("images", msgObj->getArray("images"));
                    } catch (...) { /* malformed — skip */ }
                }
                // view_image sidecar (absent in older files).
                if (msgObj->has("tool_images")) {
                    try {
                        loadedMsg->set("tool_images", msgObj->getArray("tool_images"));
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
        m_pinned         = newPinned;
        m_archived       = newArchived;
        m_modelSelection = std::move(newModelSelection);
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
        m_chatWriteRoots.clear();
        m_streamBuffer.clear();
        m_streamBufferDirty = 0;

        outModels   = std::move(newModels);
        m_filePath  = filePath;
        m_dirty     = false;
        m_contentActivityPending = false;
        m_revision  = 0;

        // Self-calibrating elision budget: replay this chat's
        // ctx_calibration.tsv so the first request after reopening uses
        // the learned bytes-per-token, not the 3.0 default (which elides
        // far too early on the first request and then jumps).  No log /
        // no usable rows: keep the current value.  Best-effort; never
        // fails the load.
        SeedElisionCalibrationFromChatFolder();
        return true;
    }
    catch (...) {
        return false;
    }
}

void ChatHistory::SeedElisionCalibrationFromChatFolder()
{
    try {
        if (m_filePath.empty()) return;
        const std::string dir = GetChatFolder(m_filePath);
        if (dir.empty()) return;
        const std::filesystem::path p =
            std::filesystem::path(path_safety::Utf8ToWide(dir)) / L"ctx_calibration.tsv";
        std::ifstream f(p, std::ios::binary);
        if (!f) return;

        // Header line + at most the last 64 KiB (~600 rows) of the log.
        std::string header;
        std::getline(f, header);
        const std::streamoff headerEnd = f.tellg();
        f.seekg(0, std::ios::end);
        const std::streamoff size = f.tellg();
        constexpr std::streamoff kTail = 64 * 1024;
        const std::streamoff start = (size - kTail > headerEnd) ? size - kTail : headerEnd;
        f.seekg(start);
        std::string tail((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (start > headerEnd) {   // drop the partial first line
            const size_t nl = tail.find('\n');
            tail = (nl == std::string::npos) ? std::string() : tail.substr(nl + 1);
        }

        lb_elision::AdaptiveBytesPerToken seeded;
        if (lb_elision::SeedFromCalibrationTsv(seeded, header + "\n" + tail) > 0)
            m_elisionBpt = seeded;
    } catch (...) {
        // best-effort
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

std::string ChatHistory::TitleFromUserText(const std::string& text)
{
    std::string content = StripSessionContextHeader(text);
    content = TitleStripGreeting(TitleStripOuterQuotes(
        TitleCollapseWhitespace(std::move(content))));
    if (content.empty()) return std::string();

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

std::string ChatHistory::GenerateTitle() const
{
    // Use the first real user message.  Project-attached chats can be
    // saved before the first message, so SaveToFile may call this before
    // any usable content exists; in that case keep the placeholder.
    for (const auto& msg : m_messages) {
        if (msg->getValue<std::string>("role") != "user") {
            continue;
        }
        std::string title =
            TitleFromUserText(msg->getValue<std::string>("content"));
        if (!title.empty()) return title;
    }
    return "Untitled conversation";
}

std::string ChatHistory::GetChatFolderTitle() const
{
    if (!m_title.empty() &&
        m_title != "Untitled conversation" &&
        !IsLegacySessionContextTitle(m_title)) {
        return m_title;
    }
    return GenerateTitle();
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
//  Per-conversation chat folders
// ═══════════════════════════════════════════════════════════════════
//
// Layout and naming rules live in chat_folders.h.  This block adds the
// filesystem side: resolving a conversation to its folder (by id, never by
// title), creating it on first use, and the one-time migration out of the
// legacy LlamaBoss\Workflows root.

static std::string JoinChatPath(const std::string& a, const std::string& b)
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
        return JoinChatPath(std::string(userProfile.ToUTF8().data()), "LlamaBoss");
    }
#endif

    wxString home = wxGetHomeDir();
    if (!home.IsEmpty()) {
        return JoinChatPath(std::string(home.ToUTF8().data()), "LlamaBoss");
    }

    wxString docs = wxStandardPaths::Get().GetDocumentsDir();
    return JoinChatPath(std::string(docs.ToUTF8().data()), "LlamaBoss");
}

static std::string ConversationStemFromPath(const std::string& conversationPath)
{
    wxFileName fn(wxString::FromUTF8(conversationPath));
    std::string stem(fn.GetName().ToUTF8().data());
    if (stem.empty()) {
        // Last-resort fallback: callers should normally ensure the
        // conversation has a generated chat_xxxxxxxx path before asking
        // for a chat folder.
        stem = "chat_unsaved";
    }
    return stem;
}

namespace {

// id -> resolved folder.  `provisional` means nobody has created the
// folder yet; EnsureChatFolder may still pick a better (titled) name for it.
struct ChatFolderEntry {
    std::string path;
    bool        provisional = false;
};

std::mutex& ChatFolderMutex()
{
    static std::mutex m;
    return m;
}

std::unordered_map<std::string, ChatFolderEntry>& ChatFolderCache()
{
    static std::unordered_map<std::string, ChatFolderEntry> cache;
    return cache;
}

bool g_chatFolderIndexBuilt = false;

std::string TodayYmdLocal()
{
    return std::string(wxDateTime::Now().Format("%Y-%m-%d").ToUTF8().data());
}

// Adds every chat folder under `root` to the cache.  Existing non-provisional
// entries win, so a folder found in Chats beats a leftover legacy copy.
void IndexChatFoldersLocked(const std::string& root)
{
    const wxString wroot = wxString::FromUTF8(root);
    if (!wxDirExists(wroot)) return;
    wxDir dir(wroot);
    if (!dir.IsOpened()) return;

    auto& cache = ChatFolderCache();
    wxString name;
    bool more = dir.GetFirst(&name, wxEmptyString, wxDIR_DIRS);
    while (more) {
        const std::string n(name.ToUTF8().data());
        const std::string id = chat_folders::ChatIdFromFolderName(n);
        if (!id.empty()) {
            auto it = cache.find(id);
            if (it == cache.end() || it->second.provisional) {
                cache[id] = ChatFolderEntry{ JoinChatPath(root, n), false };
            }
        }
        more = dir.GetNext(&name);
    }
}

void RebuildChatFolderIndexLocked()
{
    IndexChatFoldersLocked(ChatHistory::GetChatsRootDir());
    IndexChatFoldersLocked(ChatHistory::GetLegacyChatsRootDir());
    g_chatFolderIndexBuilt = true;
}

// Looks `stem` up without creating anything.  Returns the cached entry, or
// nullptr when no folder exists for it on disk.
const ChatFolderEntry* FindChatFolderLocked(const std::string& stem,
                                            const std::string& id)
{
    auto& cache = ChatFolderCache();
    if (!g_chatFolderIndexBuilt) RebuildChatFolderIndexLocked();

    auto it = cache.find(id);
    if (it != cache.end()) return &it->second;

    // Legacy Save-As chats used their raw JSON stem as the folder name
    // (LlamaBoss\Workflows\My notes).  Those are not id-shaped, so the
    // index cannot see them; probe the exact legacy path instead.
    const std::string legacyExact =
        JoinChatPath(ChatHistory::GetLegacyChatsRootDir(), stem);
    if (!chat_folders::IsChatFolderName(stem) &&
        wxDirExists(wxString::FromUTF8(legacyExact))) {
        cache[id] = ChatFolderEntry{ legacyExact, false };
        return &cache[id];
    }

    // Created since the index was built (another window, or by hand)?
    RebuildChatFolderIndexLocked();
    it = cache.find(id);
    return it != cache.end() ? &it->second : nullptr;
}

} // namespace

std::string ChatHistory::GetChatsRootDir()
{
    std::string dir = JoinChatPath(LlamaBossUserRootDir(), chat_folders::kChatsRootName);
    wxFileName::Mkdir(wxString::FromUTF8(dir), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    return dir;
}

std::string ChatHistory::GetLegacyChatsRootDir()
{
    // Never created: this only exists on installs that predate the rename.
    return JoinChatPath(LlamaBossUserRootDir(), chat_folders::kLegacyChatsRootName);
}

std::string ChatHistory::GetChatFolder(const std::string& conversationPath)
{
    const std::string stem = ConversationStemFromPath(conversationPath);
    const std::string id   = chat_folders::ChatIdFromConversationStem(stem);

    std::lock_guard<std::mutex> lock(ChatFolderMutex());
    if (const ChatFolderEntry* e = FindChatFolderLocked(stem, id))
        return e->path;

    // Nothing on disk yet.  Hand out an untitled name and remember it, so
    // every caller in this session agrees on the path even if one of them
    // creates a subfolder before EnsureChatFolder runs.
    ChatFolderEntry entry;
    entry.path = JoinChatPath(GetChatsRootDir(),
        chat_folders::BuildChatFolderName(TodayYmdLocal(), std::string(), id));
    entry.provisional = true;
    ChatFolderCache()[id] = entry;
    return entry.path;
}

std::string ChatHistory::GetConversationWorkspaceDir(const std::string& conversationPath)
{
    return JoinChatPath(GetChatFolder(conversationPath), "Workspace");
}

bool ChatHistory::EnsureChatFolder(const std::string& conversationPath,
                                   const std::string& title)
{
    const std::string stem = ConversationStemFromPath(conversationPath);
    const std::string id   = chat_folders::ChatIdFromConversationStem(stem);

    std::string root;
    {
        std::lock_guard<std::mutex> lock(ChatFolderMutex());
        const ChatFolderEntry* e = FindChatFolderLocked(stem, id);

        if (e && (!e->provisional || wxDirExists(wxString::FromUTF8(e->path)))) {
            // Already named (or already used on disk) — never rename.
            root = e->path;
            ChatFolderCache()[id].provisional = false;
        } else {
            // First creation: this is the one moment the folder gets its
            // human-readable name.  An empty/placeholder title yields
            // "<date>_<id>", which is still sortable and unique.
            root = JoinChatPath(GetChatsRootDir(),
                chat_folders::BuildChatFolderName(TodayYmdLocal(), title, id));
            ChatFolderCache()[id] = ChatFolderEntry{ root, false };
        }
    }
    if (root.empty()) return false;

    // Only the chat folder root is created eagerly.  Lane subfolders
    // (attachments/, artifacts/, Workspace/, etc.) are created on demand
    // by the code that actually writes into them — see SaveImagesToDisk
    // for attachments, ChatDisplay's file persistence for artifacts, and
    // ResolveCurrentCwd for Workspace.  Pre-creating nine empty lanes
    // for every conversation polluted the user-visible Chats root
    // with hundreds of empty subfolders for chats that never used them.
    bool ok = wxFileName::Mkdir(wxString::FromUTF8(root),
                                wxS_DIR_DEFAULT,
                                wxPATH_MKDIR_FULL);
    if (!ok && !wxDirExists(wxString::FromUTF8(root))) return false;

    return true;
}

void ChatHistory::ForgetChatFolder(const std::string& conversationPath)
{
    const std::string id = chat_folders::ChatIdFromConversationStem(
        ConversationStemFromPath(conversationPath));
    std::lock_guard<std::mutex> lock(ChatFolderMutex());
    ChatFolderCache().erase(id);
}

std::string ChatHistory::RemapLegacyChatPath(const std::string& path)
{
    if (path.empty()) return path;

    // Match "<legacy root>\<chat folder>[\rest]" case-insensitively.
    std::string legacyRoot = GetLegacyChatsRootDir();
    auto lower = [](std::string s) {
        for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        for (char& c : s) if (c == '/') c = '\\';
        return s;
    };
    const std::string lp = lower(path);
    const std::string lr = lower(legacyRoot) + "\\";
    if (lp.compare(0, lr.size(), lr) != 0) return path;

    const size_t segStart = lr.size();
    size_t segEnd = path.find_first_of("\\/", segStart);
    if (segEnd == std::string::npos) segEnd = path.size();
    const std::string stem = path.substr(segStart, segEnd - segStart);
    if (stem.empty()) return path;

    // Only chat folders; anything else under the legacy root (old global
    // workflow files awaiting the Skills migration) is left alone.
    const std::string id = chat_folders::ChatIdFromFolderName(stem);
    if (id.empty()) return path;

    std::string target;
    {
        std::lock_guard<std::mutex> lock(ChatFolderMutex());
        if (const ChatFolderEntry* e = FindChatFolderLocked(stem, id))
            if (!e->provisional) target = e->path;
    }
    if (target.empty()) return path;
    if (lower(target) == lower(JoinChatPath(legacyRoot, stem))) return path;

    return target + path.substr(segEnd);
}

namespace {

// Reads title + created date for a legacy chat folder, preferring the
// conversation JSON and falling back to the _title.txt marker / folder time.
void ReadLegacyChatFolderMeta(const std::string& folder,
                              const std::string& stem,
                              std::string& titleOut,
                              std::string& dateOut)
{
    titleOut.clear();
    dateOut.clear();

    const std::string json = JoinChatPath(ChatHistory::GetConversationsDir(), stem + ".json");
    try {
        if (wxFileExists(wxString::FromUTF8(json))) {
            Poco::FileInputStream in(json);
            Poco::JSON::Parser parser;
            auto root = parser.parse(in).extract<Poco::JSON::Object::Ptr>();
            if (root) {
                if (root->has("title") && !root->isNull("title"))
                    titleOut = root->getValue<std::string>("title");
                if (root->has("created_at") && !root->isNull("created_at")) {
                    const std::string c = root->getValue<std::string>("created_at");
                    if (c.size() >= 10 && chat_folders::HasDatePrefix(c.substr(0, 10) + "_"))
                        dateOut = c.substr(0, 10);
                }
            }
        }
    } catch (...) {
        // Unreadable JSON: fall through to the marker / folder time.
    }

    if (titleOut.empty()) {
        std::ifstream marker(path_safety::Utf8ToWide(JoinChatPath(folder, "_title.txt")));
        if (marker.is_open()) std::getline(marker, titleOut);
        while (!titleOut.empty() && (titleOut.back() == '\r' || titleOut.back() == '\n'))
            titleOut.pop_back();
    }

    if (dateOut.empty()) {
        wxDateTime created, modified;
        wxFileName fn = wxFileName::DirName(wxString::FromUTF8(folder));
        if (fn.GetTimes(nullptr, &modified, &created) && created.IsValid())
            dateOut = std::string(created.Format("%Y-%m-%d").ToUTF8().data());
        else
            dateOut = TodayYmdLocal();
    }
}

} // namespace

ChatHistory::ChatFolderMigrationResult ChatHistory::MigrateLegacyChatFolders()
{
    ChatFolderMigrationResult result;

    const std::string legacyRoot = GetLegacyChatsRootDir();
    const wxString wLegacy = wxString::FromUTF8(legacyRoot);
    if (!wxDirExists(wLegacy)) return result;

    const std::string chatsRoot = GetChatsRootDir();

    // Collect first; renaming while enumerating is undefined on some FSs.
    std::vector<std::string> names;
    {
        wxDir dir(wLegacy);
        if (!dir.IsOpened()) return result;
        wxString name;
        bool more = dir.GetFirst(&name, wxEmptyString, wxDIR_DIRS);
        while (more) {
            names.emplace_back(name.ToUTF8().data());
            more = dir.GetNext(&name);
        }
    }

    for (const std::string& stem : names) {
        // Only legacy "chat_<id>" folders.  Custom-named Save-As folders and
        // anything else (pre-Skills workflow files) stay put; the resolver
        // still finds Save-As folders at their legacy path.
        if (!chat_folders::detail::StartsWithNoCase(stem, chat_folders::kLegacyChatPrefix))
            continue;
        const std::string id = chat_folders::ChatIdFromFolderName(stem);
        if (id.empty()) continue;

        const std::string src = JoinChatPath(legacyRoot, stem);
        std::string title, date;
        ReadLegacyChatFolderMeta(src, stem, title, date);
        const std::string dst = JoinChatPath(chatsRoot,
            chat_folders::BuildChatFolderName(date, title, id));

        if (wxDirExists(wxString::FromUTF8(dst))) {
            ++result.skipped;               // never merge into an existing folder
            continue;
        }

        bool moved = false;
#ifdef __WXMSW__
        // Same-volume directory move.  Fails (and changes nothing) if any
        // process holds a file or a cwd inside the folder.
        moved = ::MoveFileExW(path_safety::Utf8ToWide(src).c_str(),
                              path_safety::Utf8ToWide(dst).c_str(), 0) != 0;
#else
        moved = wxRenameFile(wxString::FromUTF8(src), wxString::FromUTF8(dst), false);
#endif
        if (moved) {
            ++result.moved;
        } else {
            ++result.failed;
            result.failedFolders.push_back(src);
        }
    }

    // Remove the legacy root only if the migration emptied it.  Leftover
    // content (failed moves, Save-As folders, pre-Skills files) keeps it.
    bool legacyEmpty = false;
    {
        wxDir dir(wLegacy);   // closed at end of scope, before the rmdir
        legacyEmpty = dir.IsOpened() && !dir.HasFiles() && !dir.HasSubDirs();
    }
    if (legacyEmpty) wxRmdir(wLegacy);

    // Folder locations changed: rebuild the id index on next lookup.
    {
        std::lock_guard<std::mutex> lock(ChatFolderMutex());
        ChatFolderCache().clear();
        g_chatFolderIndexBuilt = false;
    }
    return result;
}

// ═══════════════════════════════════════════════════════════════════
//  Attachment / generated-file chat folder lanes
// ═══════════════════════════════════════════════════════════════════

std::string ChatHistory::GetAttachmentDir(const std::string& conversationPath)
{
    return JoinChatPath(GetChatFolder(conversationPath), "attachments");
}

std::string ChatHistory::GetAttachmentRelDir(const std::string& /*conversationPath*/)
{
    // Relative to the chat folder.  Forward slashes for JSON portability.
    return "attachments";
}

std::string ChatHistory::GetGeneratedFilesDir(const std::string& conversationPath)
{
    return JoinChatPath(GetChatFolder(conversationPath), "artifacts");
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

// ── Tool-result formatting ───────────────────────────────────────

// ─── Unified tool-block formatter ──────────────────────────────
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
