// tool_call_parser.cpp

#include "tool_call_parser.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>

namespace {

const std::string kOpenXml    = "<tool_call>";

// Some local models emit a chat-template style sentinel before the
// name/args tags instead of the exact XML-ish opener we request:
//
//   <|tool_call>call
//   <name>open</name>
//   <args>foo.mp3</args>
//   </tool_call>
//
// Treat it as an alternate opener so the raw marker never reaches the
// chat UI and the tool still executes. Keep this list narrow: we only
// accept the concrete variant observed in testing.
const std::string kOpenGemma  = "<|tool_call>call";

// Gemma-native function-call format observed in mid-conversation
// drift on small instruct models (gemma-4-e4b-it specifically).  The
// model abandons our XML grammar in favour of its training-time
// representation:
//
//   <|tool_call>call:read{hello.txt}<tool_call|>
//
//   <|tool_call>call:edit{src/foo.cpp
//   <<<OLD>>>
//   ...
//   <<<NEW>>>
//   ...
//   }<tool_call|>
//
// Distinct opener (note the colon), distinct closer (note the pipe
// position), distinct inner shape (NAME{ARGS} instead of name/args
// XML tags).  The shim recognizes this variant and re-shapes it
// into the same ToolInvocation the rest of the harness consumes,
// so the model's drift doesn't reach the user as a malformed-call
// cascade.
//
// kOpenGemmaNative is a strict superset of kOpenGemma (one extra
// ':' at the end).  When both could match at the same position,
// FindFirstOpenMarker prefers the longer marker so the colon is
// consumed correctly.
const std::string kOpenGemmaNative  = "<|tool_call>call:";
const std::string kCloseGemmaNative = "<tool_call|>";

const std::string kClose      = "</tool_call>";
const std::string kNameOpen   = "<name>";
const std::string kNameClose  = "</name>";
const std::string kArgsOpen   = "<args>";
const std::string kArgsClose  = "</args>";

// Hard cap for a single <tool_call>...</tool_call> block.
// This prevents a malformed or runaway model response from causing
// the streaming detector to retain unbounded text while waiting for
// a closing </tool_call> tag.
constexpr size_t kMaxToolCallBlockBytes = 64 * 1024;

// Keep diagnostics useful without echoing a giant model mistake back
// into the chat/history. The full oversized block is intentionally
// not preserved.
std::string MakeRawPreview(const std::string& raw)
{
    constexpr size_t kPreviewBytes = 4096;
    if (raw.size() <= kPreviewBytes) return raw;

    return raw.substr(0, kPreviewBytes) +
        "\n...[tool_call truncated for display; block exceeded " +
        std::to_string(kMaxToolCallBlockBytes) + " bytes]";
}

std::string Trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return (char)std::tolower(c); });
    return s;
}

size_t MaxOpenMarkerBytes()
{
    return std::max({kOpenXml.size(),
                     kOpenGemma.size(),
                     kOpenGemmaNative.size()});
}

// Returns the closing marker that pairs with whichever opener was
// matched.  The variant is identified by the opener length the
// detector / batch parser stashed at match time -- this avoids a
// second pass over the buffer to re-detect.
const std::string& CloseForOpenerLen(size_t openerLen)
{
    if (openerLen == kOpenGemmaNative.size()) return kCloseGemmaNative;
    return kClose;
}

bool FindFirstOpenMarker(const std::string& text,
                         size_t             start,
                         size_t&            posOut,
                         size_t&            lenOut)
{
    size_t bestPos = std::string::npos;
    size_t bestLen = 0;

    auto consider = [&](const std::string& marker) {
        size_t pos = text.find(marker, start);
        if (pos == std::string::npos) return;

        // Earliest marker wins. If markers ever begin at the same
        // byte, prefer the longer one so the detector consumes the
        // whole opener before parsing inner content.
        if (bestPos == std::string::npos ||
            pos < bestPos ||
            (pos == bestPos && marker.size() > bestLen)) {
            bestPos = pos;
            bestLen = marker.size();
        }
    };

    consider(kOpenXml);
    consider(kOpenGemma);
    consider(kOpenGemmaNative);

    if (bestPos == std::string::npos) return false;
    posOut = bestPos;
    lenOut = bestLen;
    return true;
}

// ─── Inner block parser ──────────────────────────────────────────
// Given the content BETWEEN <tool_call> and </tool_call> (exclusive),
// populate a ToolInvocation.  rawBlockOut is the full opening-to-
// closing span with both tags included.
//
// Accepts either order of <name>/<args>, trims whitespace around both,
// and is tolerant of a leading <tool_call>\n convention the model
// is likely to adopt for readability.
//
// Returns true iff a name could be extracted.  Args may still be
// missing (that's a validation failure, not a parse failure — the
// caller records it via ValidateToolArgs).
bool ParseInnerBlock(const std::string& inner,
                     const std::string& rawBlock,
                     ToolInvocation&    out)
{
    out.rawBlock = rawBlock;

    size_t nameA = inner.find(kNameOpen);
    size_t nameB = (nameA == std::string::npos)
                   ? std::string::npos
                   : inner.find(kNameClose, nameA + kNameOpen.size());
    size_t argsA = inner.find(kArgsOpen);
    size_t argsB = (argsA == std::string::npos)
                   ? std::string::npos
                   : inner.find(kArgsClose, argsA + kArgsOpen.size());

    if (nameA == std::string::npos || nameB == std::string::npos) {
        out.valid         = false;
        out.invalidReason = "missing <name>...</name> tag. Format must be: <tool_call><name>TOOL_NAME</name><args>ARGS</args></tool_call>";
        return false;
    }

    std::string rawName = inner.substr(nameA + kNameOpen.size(),
                                       nameB - (nameA + kNameOpen.size()));
    out.name = Lower(Trim(rawName));

    if (argsA != std::string::npos && argsB != std::string::npos) {
        out.args = Trim(inner.substr(argsA + kArgsOpen.size(),
                                     argsB - (argsA + kArgsOpen.size())));
    } else {
        // args block is optional at the parse layer — some tools
        // (pwd, and ls with no arg) don't need one.  Validation
        // decides if it's required for this tool.
        out.args.clear();
    }

    std::string reason;
    if (!IsKnownToolName(out.name)) {
        out.valid         = false;
        out.invalidReason = "unknown tool: " + out.name;
        return true;
    }
    if (!ValidateToolArgs(out.name, out.args, reason)) {
        out.valid         = false;
        out.invalidReason = reason;
        return true;
    }
    out.valid = true;
    return true;
}

// ─── Inner block parser: gemma-native brace syntax ───────────────
// Given the content BETWEEN <|tool_call>call: and <tool_call|>,
// shape NAME{ARGS}, populate a ToolInvocation.  rawBlock is the
// full opener-to-closer span with both markers included.
//
// Parsing rules:
//   - Leading whitespace is skipped.
//   - The name is the run of identifier characters from the start
//     of the inner content up to the first '{' or whitespace,
//     whichever comes first.
//   - If a '{' exists, args is everything from after the first
//     '{' to BEFORE THE LAST '}' in the inner content.  Last-brace
//     semantics make the parser tolerant of args containing
//     literal braces (common for code edits that touch C/C++ /
//     JSON / shell brace blocks).
//   - If no '{' exists at all, args is empty.  This is the
//     pwd / ls-with-no-arg shape that gemma sometimes emits.
//   - Trailing whitespace inside args is trimmed (mirrors the XML
//     path's Trim on args).  Trailing newline before the closing
//     '}' is preserved as-is and Trim'd uniformly.
bool ParseInnerBlockGemmaNative(const std::string& inner,
                                const std::string& rawBlock,
                                ToolInvocation&    out)
{
    out.rawBlock = rawBlock;

    // Skip leading whitespace.
    size_t i = 0;
    while (i < inner.size() &&
           (inner[i] == ' ' || inner[i] == '\t' ||
            inner[i] == '\r' || inner[i] == '\n')) {
        ++i;
    }

    // Read name: run until '{' or whitespace.
    size_t nameStart = i;
    while (i < inner.size()) {
        char c = inner[i];
        if (c == '{' || c == ' ' || c == '\t' ||
            c == '\r' || c == '\n') break;
        ++i;
    }
    std::string rawName = inner.substr(nameStart, i - nameStart);
    out.name = Lower(Trim(rawName));

    if (out.name.empty()) {
        out.valid         = false;
        out.invalidReason = "gemma-native tool call had no name "
                            "between <|tool_call>call: and "
                            "the first '{' or '<tool_call|>'";
        return false;
    }

    // Find first '{' from current position.  Anything between the
    // name end and '{' is whitespace we already trimmed; if there
    // are non-{ non-ws characters here the model emitted something
    // we don't understand, but be permissive and treat as empty
    // args.
    size_t braceA = inner.find('{', i);
    if (braceA == std::string::npos) {
        out.args.clear();
    } else {
        // Find LAST '}' in the inner content.  Last-brace
        // semantics handle args that contain literal '{' / '}'.
        size_t braceB = inner.rfind('}');
        if (braceB == std::string::npos || braceB <= braceA) {
            out.valid         = false;
            out.invalidReason = "gemma-native tool call had '{' "
                                "with no matching '}' before "
                                "<tool_call|>";
            return false;
        }
        out.args = Trim(inner.substr(braceA + 1, braceB - braceA - 1));
    }

    std::string reason;
    if (!IsKnownToolName(out.name)) {
        out.valid         = false;
        out.invalidReason = "unknown tool: " + out.name;
        return true;
    }
    if (!ValidateToolArgs(out.name, out.args, reason)) {
        out.valid         = false;
        out.invalidReason = reason;
        return true;
    }
    out.valid = true;
    return true;
}

// Dispatch to the right inner parser based on which opener was
// matched.  openerLen is the length stashed by the batch parser /
// streaming detector at the time FindFirstOpenMarker matched; we
// use it to recover the variant without re-scanning the buffer.
bool ParseInnerByVariant(size_t             openerLen,
                         const std::string& inner,
                         const std::string& rawBlock,
                         ToolInvocation&    out)
{
    if (openerLen == kOpenGemmaNative.size()) {
        return ParseInnerBlockGemmaNative(inner, rawBlock, out);
    }
    return ParseInnerBlock(inner, rawBlock, out);
}

} // namespace

bool ContainsToolCallOpenMarker(const std::string& text)
{
    size_t pos = std::string::npos;
    size_t len = 0;
    return FindFirstOpenMarker(text, 0, pos, len);
}

// ═══════════════════════════════════════════════════════════════════
//  Batch parser
// ═══════════════════════════════════════════════════════════════════

ParsedAssistantResponse ParseAssistantResponse(const std::string& text)
{
    ParsedAssistantResponse out;

    // Locate the first tool-call opener. Subsequent blocks are
    // ignored for execution purposes per protocol; they remain in
    // the prose so the user can see what happened.
    size_t openPos = std::string::npos;
    size_t openLen = 0;
    if (!FindFirstOpenMarker(text, 0, openPos, openLen)) {
        out.prose = text;
        return out;
    }

    size_t contentStart = openPos + openLen;
    const std::string& closer = CloseForOpenerLen(openLen);
    size_t closePos     = text.find(closer, contentStart);

    if (closePos == std::string::npos) {
        // Unclosed block — strip the broken tail from user-visible prose.
        // Also cap the diagnostic body so one runaway <tool_call> does
        // not get stored in full in chat history.
        std::string rawTail = text.substr(openPos);

        MalformedBlock m;
        m.rawText = MakeRawPreview(rawTail);
        if (rawTail.size() > kMaxToolCallBlockBytes) {
            m.reason = "unterminated tool call exceeded " +
                       std::to_string(kMaxToolCallBlockBytes) +
                       " bytes before " + closer + " was found";
        } else {
            m.reason = "unterminated tool call (missing " + closer + ")";
        }
        out.malformed.push_back(m);
        out.prose = text.substr(0, openPos);
        return out;
    }

    std::string inner    = text.substr(contentStart, closePos - contentStart);
    std::string rawBlock = text.substr(openPos,
                                       (closePos + closer.size()) - openPos);

    // Closed but oversized blocks are treated as malformed. Do not
    // execute them, and do not persist the full block.
    if (rawBlock.size() > kMaxToolCallBlockBytes) {
        MalformedBlock m;
        m.rawText = MakeRawPreview(rawBlock);
        m.reason  = "tool call block exceeded " +
                    std::to_string(kMaxToolCallBlockBytes) +
                    " byte limit";
        out.malformed.push_back(m);

        std::string before = text.substr(0, openPos);
        std::string after  = text.substr(closePos + closer.size());
        out.prose = before + after;
        return out;
    }

    ToolInvocation inv;
    bool parsed = ParseInnerByVariant(openLen, inner, rawBlock, inv);

    if (!parsed) {
        MalformedBlock m;
        m.rawText = rawBlock;
        m.reason  = inv.invalidReason.empty() ? "parse failure" : inv.invalidReason;
        out.malformed.push_back(m);
    } else {
        out.invocation    = inv;
        out.hasInvocation = true;
    }

    // Prose = before + after, with the consumed block removed.
    // Trailing text after a successful call is rare but possible
    // (model adds "I'll look this up..." after the block); keep it
    // visible rather than hiding it.
    std::string before = text.substr(0, openPos);
    std::string after  = text.substr(closePos + closer.size());
    out.prose = before + after;

    return out;
}

// ═══════════════════════════════════════════════════════════════════
//  Streaming detector
// ═══════════════════════════════════════════════════════════════════

ToolCallStreamDetector::ToolCallStreamDetector() { Reset(); }

void ToolCallStreamDetector::Reset()
{
    m_buffer.clear();
    m_prosePrefix.clear();
    m_invocation    = ToolInvocation{};
    m_complete      = false;
    m_insideBlock   = false;
    m_blockStart    = 0;
    m_openMarkerLen = 0;
}

bool ToolCallStreamDetector::Feed(const std::string& delta)
{
    if (m_complete) return false;  // already fired; caller should Reset or ignore

    m_buffer += delta;

    // ── Phase 1: searching for a tool-call opener ────────────────
    // Anything before the opening marker is prose. We can safely
    // publish everything up to (buffer.size - maxOpenLen + 1) as
    // prose — the trailing window is held back in case a marker is
    // splitting across deltas.
    if (!m_insideBlock) {
        size_t openPos = std::string::npos;
        size_t openLen = 0;
        if (FindFirstOpenMarker(m_buffer, 0, openPos, openLen)) {
            m_prosePrefix += m_buffer.substr(0, openPos);
            m_buffer       = m_buffer.substr(openPos);   // buffer now starts with an opener
            m_insideBlock  = true;
            m_blockStart   = 0;
            m_openMarkerLen = openLen;
            // fall through to Phase 2 (maybe closer already in buffer)
        } else {
            // Hold back the last bytes in case an opener split lands
            // here; publish everything else as prose so the UI can
            // render smoothly.
            const size_t kHoldBack = MaxOpenMarkerBytes() - 1;
            if (m_buffer.size() > kHoldBack) {
                size_t safeLen = m_buffer.size() - kHoldBack;
                m_prosePrefix += m_buffer.substr(0, safeLen);
                m_buffer       = m_buffer.substr(safeLen);
            }
            return false;
        }
    }

    // ── Phase 2: inside a tool call, searching for the closer ────
    // m_buffer is guaranteed to start with a recognized opener here.
    // The closer to look for depends on which opener was matched
    // (XML opener -> </tool_call>, gemma-native opener with colon
    // -> <tool_call|>).  m_openMarkerLen, set in Phase 1, is the
    // disambiguator.
    size_t contentStart = m_openMarkerLen;
    const std::string& closer = CloseForOpenerLen(m_openMarkerLen);
    size_t closePos     = m_buffer.find(closer, contentStart);
    if (closePos == std::string::npos) {
        // No close yet — keep buffering, but never without a hard cap.
        // Once the cap is exceeded, surface an invalid invocation so
        // the agent loop can feed an error back to the model instead
        // of retaining unbounded text.
        if (m_buffer.size() > kMaxToolCallBlockBytes) {
            m_invocation = ToolInvocation{};
            m_invocation.valid = false;
            m_invocation.rawBlock = MakeRawPreview(m_buffer);
            m_invocation.invalidReason =
                "unterminated tool call exceeded " +
                std::to_string(kMaxToolCallBlockBytes) +
                " bytes before " + closer + " was found";
            m_complete = true;
            return true;
        }
        return false;
    }

    std::string inner    = m_buffer.substr(contentStart, closePos - contentStart);
    std::string rawBlock = m_buffer.substr(0, closePos + closer.size());

    if (rawBlock.size() > kMaxToolCallBlockBytes) {
        m_invocation = ToolInvocation{};
        m_invocation.valid = false;
        m_invocation.rawBlock = MakeRawPreview(rawBlock);
        m_invocation.invalidReason =
            "tool call block exceeded " +
            std::to_string(kMaxToolCallBlockBytes) +
            " byte limit";
        m_complete = true;
        return true;
    }

    ParseInnerByVariant(m_openMarkerLen, inner, rawBlock, m_invocation);
    m_complete = true;
    return true;
}
