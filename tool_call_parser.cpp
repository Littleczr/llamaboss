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

// Inline reasoning markers (see FindFirstOpenMarkerOutsideReasoning).
const std::string kThinkOpen  = "<think>";
const std::string kThinkClose = "</think>";

// Hard cap for a single <tool_call>...</tool_call> block.
// This prevents a malformed or runaway model response from causing
// the streaming detector to retain unbounded text while waiting for
// a closing </tool_call> tag.
constexpr size_t kMaxToolCallBlockBytes = 64 * 1024;

// Keep diagnostics useful without echoing a giant model mistake back
// into the chat/history. The full malformed block is intentionally
// not preserved.
//
// Head+tail shape, not head-only: for unterminated / wrong-closer
// blocks the diagnostic evidence is at the END of the block (e.g. a
// stray </name> where </tool_call> belonged). A head-only preview
// hides exactly the bytes the model needs to see to self-correct.
//
// Keeping the preview small is also deliberate model-feedback
// hygiene: this string is echoed back into the model context as the
// failed command. Re-feeding a small model (gemma-4-e4b class) a
// multi-KB verbatim copy of its own broken block makes the broken
// pattern the strongest signal in recent context, and the model
// repeats it verbatim until the malformed cap trips.
//
// Both cut points are nudged to UTF-8 sequence boundaries so the
// preview never contains a split multi-byte character (Poco's JSON
// escaping downstream must only ever see valid UTF-8).
std::string MakeRawPreview(const std::string& raw)
{
    constexpr size_t kHeadBytes = 600;
    constexpr size_t kTailBytes = 600;
    if (raw.size() <= kHeadBytes + kTailBytes) return raw;

    auto isUtf8Continuation = [&](size_t i) {
        return (static_cast<unsigned char>(raw[i]) & 0xC0) == 0x80;
    };

    size_t headEnd = kHeadBytes;
    while (headEnd > 0 && isUtf8Continuation(headEnd)) --headEnd;

    size_t tailStart = raw.size() - kTailBytes;
    while (tailStart < raw.size() && isUtf8Continuation(tailStart)) ++tailStart;

    return raw.substr(0, headEnd) +
        "\n...[" + std::to_string(tailStart - headEnd) +
        " bytes omitted from this diagnostic preview]...\n" +
        raw.substr(tailStart);
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

// Gemma can drift into brace-style calls that still wrap the payload in
// either XML-ish <args>...</args> tags or a textual `args:` prefix, e.g.:
//
//   <|tool_call>call:python_run_script{
//   <args>helper.py
//   input.txt</args>
//   }<tool_call|>
//
//   <|tool_call>call:python_run_script{args:
//   helper.py
//   input.txt
//   }<tool_call|>
//
// Normalize those narrow compatibility forms back to the plain legacy args
// body that validators and dispatchers already expect. This only runs on the
// Gemma-native brace parser, never on normal XML tool calls.
std::string NormalizeGemmaNativeArgs(std::string args)
{
    args = Trim(args);

    if (args.size() >= kArgsOpen.size() + kArgsClose.size() &&
        args.compare(0, kArgsOpen.size(), kArgsOpen) == 0 &&
        args.compare(args.size() - kArgsClose.size(),
                     kArgsClose.size(),
                     kArgsClose) == 0) {
        args = args.substr(kArgsOpen.size(),
                           args.size() - kArgsOpen.size() - kArgsClose.size());
        return Trim(args);
    }

    constexpr const char* kArgsColon = "args:";
    constexpr size_t kArgsColonLen = 5;
    if (args.size() >= kArgsColonLen &&
        Lower(args.substr(0, kArgsColonLen)) == kArgsColon) {
        args = args.substr(kArgsColonLen);
        return Trim(args);
    }

    return args;
}

size_t MaxOpenMarkerBytes()
{
    return std::max({kOpenXml.size(),
                     kOpenGemma.size(),
                     kOpenGemmaNative.size()});
}


// Find the closer that pairs with a matched opener.  Most
// <|tool_call>call:... blocks use Gemma's native <tool_call|> closer,
// but gemma-4-e4b-it can drift into a hybrid shape:
//
//   <|tool_call>call:
//   <name>tool</name>
//   <args>...</args>
//   </tool_call>
//
// Accepting the normal XML closer for this hybrid prevents a malformed-call
// cascade after a tool error, while the inner parser still validates the
// actual tool name/args before dispatch.
bool FindCloseMarker(const std::string& text,
                     size_t             contentStart,
                     size_t             openerLen,
                     size_t&            closePosOut,
                     std::string&       closerOut)
{
    if (openerLen != kOpenGemmaNative.size()) {
        size_t pos = text.find(kClose, contentStart);
        if (pos == std::string::npos) return false;
        closePosOut = pos;
        closerOut = kClose;
        return true;
    }

    size_t nativePos = text.find(kCloseGemmaNative, contentStart);
    size_t xmlPos    = text.find(kClose, contentStart);

    if (nativePos == std::string::npos && xmlPos == std::string::npos) {
        return false;
    }
    if (xmlPos != std::string::npos &&
        (nativePos == std::string::npos || xmlPos < nativePos)) {
        closePosOut = xmlPos;
        closerOut = kClose;
        return true;
    }

    closePosOut = nativePos;
    closerOut = kCloseGemmaNative;
    return true;
}

std::string MissingCloseReason(size_t openerLen)
{
    // Corrective, not just diagnostic. The dominant failure shape is a
    // model that BELIEVES it closed the block — it ended with a stray
    // tag like </name> instead of </tool_call> — so the message must
    // point at the tail and restate the full grammar, the same way the
    // inner-parser errors already do. A bare "missing </tool_call>"
    // gives a small model nothing to act on.
    static const std::string kFormatReminder =
        " Format must be: <tool_call><name>TOOL_NAME</name>"
        "<args>ARGS</args></tool_call>";

    if (openerLen == kOpenGemmaNative.size()) {
        return "unterminated tool call: the block must end with </tool_call>"
               " or <tool_call|>; no other tag is a valid closer." +
               kFormatReminder;
    }
    return "unterminated tool call: the block must end with " + kClose +
           "; a stray closing tag such as </name> or </args> is not a"
           " valid closer." + kFormatReminder;
}

// Raw opener search: no notion of reasoning spans.  Callers that
// decide dispatch must use FindFirstOpenMarkerOutsideReasoning.
bool FindFirstOpenMarkerRaw(const std::string& text,
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

// ─── Qwen function-tag drift ─────────────────────────────────────
// Qwen models trained on the Qwen3-Coder tool format
//
//   <tool_call>
//   <function=powershell>
//   <parameter=command>Get-ChildItem …</parameter>
//   </function>
//   </tool_call>
//
// blend it with ours, and repeat the blend verbatim through
// malformed-call coaching:
//
//   <function>powershell</name>   <args>…</args>
//   <function=powershell</name>   <args>…</args>
//   <function>powershell</function>             (no args)
//   <function>powershell>        <args>…</args>
//
// The intent is unambiguous when the tag is the FIRST thing in the
// block, carries an identifier-only name, and is followed only by a
// balanced <args> envelope (or one native <parameter=…> body) and an
// optional </function>.  Name/args still go through the normal
// IsKnownToolName / ValidateToolArgs gates.  Anything looser falls
// through to a malformed-call error that names the mistake.

const std::string kFunctionTag      = "<function";
const std::string kFunctionClose    = "</function>";
const std::string kParameterOpen    = "<parameter=";
const std::string kParameterClose   = "</parameter>";

size_t SkipWs(const std::string& s, size_t i, size_t to)
{
    while (i < to && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return i;
}

bool At(const std::string& s, size_t i, size_t to, const std::string& tok)
{
    return i <= to && to - i >= tok.size() &&
           s.compare(i, tok.size(), tok) == 0;
}

// Matches <function>NAME</name>, <function=NAME</name>,
// <function>NAME</function>, <function=NAME</function>,
// <function=NAME> or <function>NAME> at the first non-whitespace byte of s[from, to).
// On success sets nameOut (lower-cased) and afterOut (just past the
// tag's closer).
bool MatchFunctionNameTag(const std::string& s,
                          size_t             from,
                          size_t             to,
                          std::string&       nameOut,
                          size_t&            afterOut)
{
    size_t i = SkipWs(s, from, to);
    if (!At(s, i, to, kFunctionTag)) return false;
    i += kFunctionTag.size();
    if (i >= to) return false;

    const bool eqForm = (s[i] == '=');
    if (!eqForm && s[i] != '>') return false;
    i = SkipWs(s, i + 1, to);

    const size_t n0 = i;
    while (i < to &&
           (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_')) {
        ++i;
    }
    if (i == n0) return false;
    const std::string name = s.substr(n0, i - n0);
    i = SkipWs(s, i, to);

    if (At(s, i, to, kNameClose))          i += kNameClose.size();
    else if (At(s, i, to, kFunctionClose)) i += kFunctionClose.size();
    else if (i < to && s[i] == '>') ++i;   // <function=NAME> or <function>NAME>
    else return false;

    nameOut  = Lower(name);
    afterOut = i;
    return true;
}

// Recovers a <name>-less block written with a function tag.  Returns
// true iff the shape was recognized (out is then populated, valid or
// not); false leaves the caller to report a malformed call.
bool TryParseFunctionTagBody(const std::string& inner, ToolInvocation& out)
{
    std::string name;
    size_t i = 0;
    if (!MatchFunctionNameTag(inner, 0, inner.size(), name, i)) return false;

    const size_t end = inner.size();
    i = SkipWs(inner, i, end);

    std::string args;
    if (At(inner, i, end, kArgsOpen)) {
        const size_t a = i + kArgsOpen.size();
        const size_t b = inner.find(kArgsClose, a);
        if (b == std::string::npos) return false;   // partial envelope: fail closed
        args = Trim(inner.substr(a, b - a));
        i = b + kArgsClose.size();
    } else if (At(inner, i, end, kParameterOpen)) {
        // Qwen3-Coder native body: exactly ONE parameter.  Tools that
        // need several fields have their own args layout, which a
        // multi-parameter body cannot be mapped onto safely.
        const size_t gt = inner.find('>', i + kParameterOpen.size());
        if (gt == std::string::npos) return false;
        const size_t pb = inner.find(kParameterClose, gt + 1);
        if (pb == std::string::npos) return false;
        args = Trim(inner.substr(gt + 1, pb - gt - 1));
        i = pb + kParameterClose.size();
        if (inner.find("<parameter", i) != std::string::npos) return false;
    }

    // Tail: whitespace and at most one </function>.
    i = SkipWs(inner, i, end);
    if (At(inner, i, end, kFunctionClose)) i += kFunctionClose.size();
    if (SkipWs(inner, i, end) != end) return false;

    out.name = name;
    out.args = args;

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

// Malformed-call reason that points at the model's actual mistake.
// A bare "missing <name>" reads, to a model that wrote <function>, like
// a restatement of what it already sent, so it repeats the same block.
std::string MissingNameReason(const std::string& inner)
{
    static const std::string kFormat =
        "<tool_call><name>TOOL_NAME</name><args>ARGS</args></tool_call>";

    const size_t fn = inner.find(kFunctionTag);
    if (fn == std::string::npos) {
        return "missing <name>...</name> tag. Format must be: " + kFormat;
    }

    size_t stop = inner.find('\n', fn);
    if (stop == std::string::npos) stop = inner.size();
    std::string snippet = inner.substr(fn, std::min<size_t>(stop - fn, 60));

    return "missing <name>...</name> tag: you wrote \"" + snippet +
           "\", but the tool name goes in <name>...</name>, not <function>, "
           "and arguments go in <args>...</args>, not <parameter>. "
           "Format must be: " + kFormat;
}

// ─── Reasoning is never a tool call ──────────────────────────────
// ChatClient re-wraps delta.reasoning_content as inline <think>…</think>,
// so fullResponse carries the model's reasoning.  Models quote the
// protocol while thinking ("I need to use the exact format:
// <tool_call><name>powershell</name><args>...</args></tool_call>"),
// and a raw search would dispatch that quote as a real call.
//
// Reasoning reaches this parser in three shapes:
//
//   1. Wrapped:  <think>…</think>          (reasoning_content, or inline)
//   2. Orphan:   …reasoning…</think>        (template prefilled <think>)
//   3. Leaked:   <think>…`</think><tool_call>`. more reasoning…</think>
//      llama-server's reasoning splitter ends reasoning_content at the
//      first "<tool_call>" the model writes, even a mention inside its
//      thinking.  The rest of the thought arrives as CONTENT, terminated
//      by the model's own stray </think>.
//
// Shapes 2 and 3 are both "an unmatched </think> closes reasoning that
// started somewhere before it".  The complication is legitimate calls
// whose ARGS contain "</think>" (writing LlamaBoss's own sources, say),
// which must still dispatch.  Rule for an opener that precedes an
// unmatched </think>:
//   * the </think> falls AFTER the block's closer (and no <think> sits
//     between them): the whole block was a quote inside leaked
//     reasoning — skip past the </think>;
//   * the </think> falls INSIDE the block: it is reasoning only if the
//     block had not started a real call first, i.e. no complete
//     <name>…</name> precedes the </think> (the Gemma colon-native
//     opener carries its name inline, so it always counts as started).
// Any misjudgment fails closed (a call is not run), never open.

// True if [from, to) of `text` already contains a real call start.
bool SegmentStartedRealCall(const std::string& text,
                            size_t             from,
                            size_t             to,
                            size_t             openerLen)
{
    if (openerLen == kOpenGemmaNative.size()) return true;

    // A recognized function tag (Qwen drift) is a started call too.
    std::string fnName;
    size_t fnAfter = 0;
    if (MatchFunctionNameTag(text, from, to, fnName, fnAfter)) return true;

    const size_t nameA = text.find(kNameOpen, from);
    if (nameA == std::string::npos || nameA >= to) return false;
    const size_t nameB = text.find(kNameClose, nameA + kNameOpen.size());
    return nameB != std::string::npos &&
           nameB + kNameClose.size() <= to;
}

// First tool-call opener at or after `start` that is NOT reasoning.
// An unterminated <think> hides everything after it (fail closed: an
// unfinished thought never dispatches).
bool FindFirstOpenMarkerOutsideReasoning(const std::string& text,
                                         size_t             start,
                                         size_t&            posOut,
                                         size_t&            lenOut)
{
    size_t cur = start;

    while (cur <= text.size()) {
        size_t markerPos = std::string::npos;
        size_t markerLen = 0;
        const bool haveMarker =
            FindFirstOpenMarkerRaw(text, cur, markerPos, markerLen);
        const size_t thinkOpen  = text.find(kThinkOpen, cur);
        const size_t thinkClose = text.find(kThinkClose, cur);

        // Earliest of: wrapped <think>, orphan </think>, tool opener.
        const size_t mPos = haveMarker ? markerPos : std::string::npos;
        const size_t first = std::min({thinkOpen, thinkClose, mPos});
        if (first == std::string::npos) return false;

        // Shape 1: wrapped block — skip it whole.
        if (first == thinkOpen) {
            const size_t close =
                text.find(kThinkClose, thinkOpen + kThinkOpen.size());
            if (close == std::string::npos) return false;
            cur = close + kThinkClose.size();
            continue;
        }

        // Shape 2: orphan closer with no opener before it — the text up
        // to here was reasoning.
        if (first == thinkClose) {
            cur = thinkClose + kThinkClose.size();
            continue;
        }

        // A tool-call opener.  Is an unmatched </think> still ahead?
        const size_t contentStart = markerPos + markerLen;
        const size_t stray = text.find(kThinkClose, contentStart);
        if (stray != std::string::npos) {
            size_t closePos = std::string::npos;
            std::string closer;
            const bool closed =
                FindCloseMarker(text, contentStart, markerLen, closePos, closer);

            if (!closed || stray < closePos) {
                // </think> inside the block (or the block never closes).
                if (!SegmentStartedRealCall(text, contentStart, stray,
                                            markerLen)) {
                    cur = stray + kThinkClose.size();
                    continue;
                }
            } else {
                // </think> after a complete block: a quoted example in
                // leaked reasoning, unless a new <think> opens first
                // (then that </think> belongs to its own wrapped block).
                const size_t blockEnd = closePos + closer.size();
                const size_t nextOpen = text.find(kThinkOpen, blockEnd);
                if (nextOpen == std::string::npos || nextOpen > stray) {
                    cur = stray + kThinkClose.size();
                    continue;
                }
            }
        }

        posOut = markerPos;
        lenOut = markerLen;
        return true;
    }
    return false;
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
        // Qwen function-tag drift: recover the unambiguous shapes.
        if (nameA == std::string::npos && TryParseFunctionTagBody(inner, out)) {
            return true;
        }
        out.valid         = false;
        out.invalidReason = MissingNameReason(inner);
        return false;
    }

    // Fail closed on a partial <args> envelope.  The args tag is optional,
    // but once the model starts one, it must close it.  Treating
    // "<args>some/path" as "no args" can silently execute an optional-arg
    // tool such as ls against the cwd instead of the intended target.
    if (argsA != std::string::npos && argsB == std::string::npos) {
        out.valid         = false;
        out.invalidReason = "missing </args> tag. Format must be: <tool_call><name>TOOL_NAME</name><args>ARGS</args></tool_call>";
        return false;
    }

    std::string rawName = inner.substr(nameA + kNameOpen.size(),
                                       nameB - (nameA + kNameOpen.size()));
    out.name = Lower(Trim(rawName));

    if (out.name.empty()) {
        out.valid         = false;
        out.invalidReason = "empty <name>...</name> tag. Tool name is required.";
        return false;
    }

    if (argsA != std::string::npos) {
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
        out.args = NormalizeGemmaNativeArgs(
            inner.substr(braceA + 1, braceB - braceA - 1));
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

// Defense in depth for quoted protocol examples that no structural
// rule can catch: a well-formed call whose whole argument is the
// template placeholder ("..." / "ARGS") was copied from a format
// example, never meant.  Reject it with coaching instead of executing
// it.
void RejectPlaceholderArgs(ToolInvocation& inv)
{
    if (!inv.valid) return;
    const std::string a = Trim(inv.args);
    if (a == "..." || a == "\xE2\x80\xA6" /* U+2026 ellipsis */ ||
        a == "ARGS" || a == "<args>") {
        inv.valid         = false;
        inv.invalidReason =
            "the <args> value \"" + a + "\" is a placeholder copied from "
            "the format example, so nothing was run. Send the real "
            "arguments: <tool_call><name>TOOL_NAME</name><args>ARGS"
            "</args></tool_call>";
    }
}

// Dispatch to the right inner parser based on which opener was
// matched.  openerLen is the length stashed by the batch parser /
// streaming detector at the time FindFirstOpenMarker matched; we
// use it to recover the variant without re-scanning the buffer.
bool ParseInnerByVariantImpl(size_t             openerLen,
                             const std::string& inner,
                             const std::string& rawBlock,
                             ToolInvocation&    out)
{
    if (openerLen == kOpenGemmaNative.size()) {
        // Hybrid drift observed in Gemma: colon opener, but XML <name>/<args>
        // body and </tool_call> closer.  Parse it through the XML path.
        std::string t = Trim(inner);
        if (t.find(kNameOpen) != std::string::npos) {
            return ParseInnerBlock(inner, rawBlock, out);
        }

        // Gemma drift: colon opener carrying the name, then an XML
        // <args> body with NO <name> tag, and a proper closer:
        //
        //   <|tool_call>call:python_run_script
        //   <args>cli_downloader.py
        //   https://example.com/...</args>
        //   </tool_call>
        //
        // Without this branch the shape falls through to the brace
        // parser, which finds no '{' and SILENTLY CLEARS ARGS — so an
        // intelligible call dispatches with empty args and the model
        // gets a misleading "requires a filename" validation error,
        // burning malformed-counter strikes on retries of the same form.
        size_t hybridArgsA = t.find(kArgsOpen);
        if (hybridArgsA != std::string::npos) {
            out.rawBlock = rawBlock;

            // Name is the identifier run at the start of the inner content,
            // exactly like the brace parser's name read.  Identifier-only,
            // mirroring TryRecoverTerminalGemmaDanglingArgsWithoutCloser,
            // so prose ahead of a literal <args> example cannot dispatch.
            size_t n = 0;
            while (n < t.size() &&
                   (std::isalnum(static_cast<unsigned char>(t[n])) ||
                    t[n] == '_')) {
                ++n;
            }
            std::string rawName = t.substr(0, n);

            // Between the name and <args> only whitespace is allowed —
            // anything else means this isn't the recognized hybrid shape,
            // so fall through to the brace parser as before.
            bool onlyWsBetween = !rawName.empty();
            for (size_t i = n; onlyWsBetween && i < hybridArgsA; ++i) {
                if (!std::isspace(static_cast<unsigned char>(t[i])))
                    onlyWsBetween = false;
            }

            if (onlyWsBetween) {
                // Fail closed on a partial <args> envelope, same as
                // ParseInnerBlock: "<args>some/path" with no closer must
                // not silently execute with empty or truncated args.
                size_t hybridArgsB = t.rfind(kArgsClose);
                if (hybridArgsB == std::string::npos ||
                    hybridArgsB < hybridArgsA + kArgsOpen.size()) {
                    out.name          = Lower(rawName);
                    out.valid         = false;
                    out.invalidReason = "missing </args> tag. Format must be: "
                        "<tool_call><name>TOOL_NAME</name><args>ARGS</args>"
                        "</tool_call>";
                    return false;
                }

                out.name = Lower(rawName);
                out.args = Trim(t.substr(hybridArgsA + kArgsOpen.size(),
                                         hybridArgsB -
                                         (hybridArgsA + kArgsOpen.size())));

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
        }

        return ParseInnerBlockGemmaNative(inner, rawBlock, out);
    }
    return ParseInnerBlock(inner, rawBlock, out);
}

bool ParseInnerByVariant(size_t             openerLen,
                         const std::string& inner,
                         const std::string& rawBlock,
                         ToolInvocation&    out)
{
    const bool parsed = ParseInnerByVariantImpl(openerLen, inner, rawBlock, out);
    RejectPlaceholderArgs(out);
    return parsed;
}

// Gemma 4 e4b occasionally emits a nearly-complete brace-style call at the
// very end of a reply, but omits BOTH supported closers. Observed shape:
//
//   <|tool_call>call:python_run_script{
//   <args>helper.py
//   arg1
//   arg2</args>
//   }
//
// The normal parser has no closing marker to latch onto and would surface an
// "unterminated tool call" error, even though the intent is unambiguous. Keep
// recovery deliberately narrow:
//   * only the Gemma-native colon opener,
//   * only when the call consumes the response tail,
//   * only when the tail ends in a final `}`,
//   * only when a balanced <args>...</args> envelope precedes that brace.
//
// This prevents arbitrary half-written tool calls from dispatching while
// recovering the concrete malformed pattern seen in local-model testing.
bool TryRecoverTerminalGemmaHybridWithoutCloser(
    const std::string& text,
    size_t             openPos,
    size_t             openerLen,
    ToolInvocation&    invocationOut)
{
    if (openerLen != kOpenGemmaNative.size()) return false;
    if (openPos >= text.size()) return false;

    std::string rawTail = text.substr(openPos);
    if (rawTail.size() > kMaxToolCallBlockBytes) return false;

    std::string inner = rawTail.substr(openerLen);
    std::string trimmed = Trim(inner);
    if (trimmed.empty() || trimmed.back() != '}') return false;

    size_t argsA = trimmed.find(kArgsOpen);
    size_t argsB = trimmed.rfind(kArgsClose);
    size_t finalBrace = trimmed.rfind('}');
    if (argsA == std::string::npos || argsB == std::string::npos ||
        finalBrace == std::string::npos) {
        return false;
    }
    if (argsB < argsA + kArgsOpen.size()) return false;
    if (argsB + kArgsClose.size() > finalBrace) return false;

    // Enforce the documented recovery scope: the balanced <args>...</args>
    // envelope must consume the tail up to the final brace.  Without this,
    // trailing prose that happens to end in '}' can be swallowed into args
    // by the native parser's last-brace semantics.
    const size_t afterArgs = argsB + kArgsClose.size();
    for (size_t i = afterArgs; i < finalBrace; ++i) {
        if (!std::isspace(static_cast<unsigned char>(trimmed[i]))) {
            return false;
        }
    }

    ToolInvocation recovered;
    bool parsed = ParseInnerByVariant(openerLen, inner, rawTail, recovered);
    if (!parsed) return false;

    invocationOut = recovered;
    return true;
}


// Gemma 4 e4b can also emit an even more compressed malformed native tail
// during conversational Skill design, with no brace body and no official
// closer. Observed shapes:
//
//   <|tool_call>call:notes_read</args>
//   <|tool_call>call:powershell</args>Get-ChildItem ...</args>
//   <|tool_call>call:python_run_script</args>helper.py\ninput</args>
//
// The first </args> mistakenly terminates the tool name; an optional second
// </args> terminates the argument body. Recovery remains intentionally narrow:
// only Gemma's colon opener, only at end-of-response, only identifier-like
// known tool names, and only the exact stray </args> boundary shape.

// Gemma drift: colon-native opener, tool name ALONE on the call line,
// multi-line argument payload laid out exactly as the XML protocol
// teaches, but closed with a bare </args> instead of a recognized
// closer:
//
//   <|tool_call>call:write\nmain.py\n<file content...>\n</args>
//   <|tool_call>call:python_run_script\nmain.py\nhttps://youtube...\n</args>
//
// Models repeat this shape verbatim through malformed-call coaching,
// so it must recover.  The intent is unambiguous when all of these hold:
//   * Gemma colon-native opener only,
//   * terminal tail only (no recognized closer; enforced by the caller),
//   * the call line after `call:` holds ONLY an identifier-like name
//     (an inline <name>/<path>/<args> token is the tagged shape's job;
//     a stray </args> on the call line is the dangling shape's job),
//   * the LAST </args> in the tail is followed by whitespace only,
//   * args = the lines between the call line and that final </args>.
//
// Two deliberate choices: using the LAST </args> keeps written file
// payloads that themselves contain a literal "</args>" intact, and the
// whitespace-only-tail rule keeps prose discussion of the protocol from
// dispatching.  This is also why </args> stays a terminal RECOVERY shape
// rather than being promoted to a first-class close marker — a first-class
// closer would truncate any legitimately written file whose content
// contains that token mid-stream.
bool TryRecoverTerminalGemmaNewlineArgsWithArgsCloser(
    const std::string& text,
    size_t             openPos,
    size_t             openerLen,
    ToolInvocation&    invocationOut)
{
    if (openerLen != kOpenGemmaNative.size()) return false;
    if (openPos >= text.size()) return false;

    std::string rawTail = text.substr(openPos);
    if (rawTail.size() > kMaxToolCallBlockBytes) return false;

    std::string inner = rawTail.substr(openerLen);

    // Call line: identifier-only tool name, nothing else.
    const size_t nl = inner.find('\n');
    if (nl == std::string::npos) return false;

    std::string rawName = Trim(inner.substr(0, nl));
    if (rawName.empty()) return false;
    for (unsigned char ch : rawName) {
        if (!(std::isalnum(ch) || ch == '_')) return false;
    }

    // The final </args> must close the tail (whitespace-only after it)
    // and must live AFTER the call line.
    const size_t closeAt = inner.rfind(kArgsClose);
    if (closeAt == std::string::npos || closeAt <= nl) return false;
    for (size_t i = closeAt + kArgsClose.size(); i < inner.size(); ++i) {
        if (!std::isspace(static_cast<unsigned char>(inner[i]))) {
            return false;
        }
    }

    std::string recoveredArgs =
        Trim(inner.substr(nl + 1, closeAt - (nl + 1)));

    ToolInvocation recovered;
    recovered.rawBlock = rawTail;
    recovered.name     = Lower(rawName);
    recovered.args     = recoveredArgs;

    std::string reason;
    if (!IsKnownToolName(recovered.name)) {
        recovered.valid = false;
        recovered.invalidReason = "unknown tool: " + recovered.name;
        invocationOut = recovered;
        return true;
    }
    if (!ValidateToolArgs(recovered.name, recovered.args, reason)) {
        recovered.valid = false;
        recovered.invalidReason = reason;
        invocationOut = recovered;
        return true;
    }

    recovered.valid = true;
    invocationOut = recovered;
    return true;
}

// Gemma drift: colon-native opener with the tool name inline, then a
// WRONG XML-ish tag used as the argument opener, and only </args> at
// EOS:
//
//   <|tool_call>call:python_install_package<name>yt-dlp</args>
//   <|tool_call>call:overwrite_file<path>main.py\n...content...</args>
//
// The model is mixing its native `call:tool` shape with fragments of the
// XML examples.  The intent is still unambiguous when all of these hold:
//   * Gemma colon-native opener only,
//   * terminal tail only (no recognized closer),
//   * identifier-like tool name at the start,
//   * the next token is one of a tiny allowlist of mistaken arg-open tags,
//   * a final </args> closes the argument payload,
//   * only whitespace follows that final </args>.
//
// For <path>, keep the path inside args.  write/overwrite_file expect
// `path\ncontent`, and python_run_script expects `script\nargv...`, so
// discarding the tag but preserving the payload is the least surprising
// normalization.
bool TryRecoverTerminalGemmaTaggedArgsWithoutCloser(
    const std::string& text,
    size_t             openPos,
    size_t             openerLen,
    ToolInvocation&    invocationOut)
{
    if (openerLen != kOpenGemmaNative.size()) return false;
    if (openPos >= text.size()) return false;

    std::string rawTail = text.substr(openPos);
    if (rawTail.size() > kMaxToolCallBlockBytes) return false;

    std::string inner = Trim(rawTail.substr(openerLen));
    if (inner.empty()) return false;

    size_t i = 0;
    while (i < inner.size() &&
           (std::isalnum(static_cast<unsigned char>(inner[i])) ||
            inner[i] == '_')) {
        ++i;
    }
    if (i == 0) return false;

    std::string rawName = inner.substr(0, i);
    std::string name = Lower(rawName);

    while (i < inner.size() &&
           std::isspace(static_cast<unsigned char>(inner[i]))) {
        ++i;
    }

    static const std::string kTagName = "<name>";
    static const std::string kTagPath = "<path>";
    static const std::string kTagArgs = "<args>";

    size_t argStart = std::string::npos;
    if (inner.compare(i, kTagName.size(), kTagName) == 0) {
        argStart = i + kTagName.size();
    } else if (inner.compare(i, kTagPath.size(), kTagPath) == 0) {
        argStart = i + kTagPath.size();
    } else if (inner.compare(i, kTagArgs.size(), kTagArgs) == 0) {
        argStart = i + kTagArgs.size();
    } else {
        return false;
    }

    size_t argsEnd = inner.rfind(kArgsClose);
    if (argsEnd == std::string::npos || argsEnd < argStart) return false;

    // Require </args> to terminate the tail.  This prevents swallowing
    // trailing prose or protocol examples into an executable invocation.
    for (size_t j = argsEnd + kArgsClose.size(); j < inner.size(); ++j) {
        if (!std::isspace(static_cast<unsigned char>(inner[j]))) return false;
    }

    ToolInvocation recovered;
    recovered.rawBlock = rawTail;
    recovered.name = name;
    recovered.args = Trim(inner.substr(argStart, argsEnd - argStart));

    std::string reason;
    if (!IsKnownToolName(recovered.name)) {
        recovered.valid = false;
        recovered.invalidReason = "unknown tool: " + recovered.name;
        invocationOut = recovered;
        return true;
    }
    if (!ValidateToolArgs(recovered.name, recovered.args, reason)) {
        recovered.valid = false;
        recovered.invalidReason = reason;
        invocationOut = recovered;
        return true;
    }

    recovered.valid = true;
    invocationOut = recovered;
    return true;
}

bool TryRecoverTerminalGemmaDanglingArgsWithoutCloser(
    const std::string& text,
    size_t             openPos,
    size_t             openerLen,
    ToolInvocation&    invocationOut)
{
    if (openerLen != kOpenGemmaNative.size()) return false;
    if (openPos >= text.size()) return false;

    std::string rawTail = text.substr(openPos);
    if (rawTail.size() > kMaxToolCallBlockBytes) return false;

    std::string inner = Trim(rawTail.substr(openerLen));
    if (inner.empty()) return false;

    const size_t nameBoundary = inner.find(kArgsClose);
    if (nameBoundary == std::string::npos) return false;

    std::string rawName = Trim(inner.substr(0, nameBoundary));
    if (rawName.empty()) return false;
    for (unsigned char ch : rawName) {
        if (!(std::isalnum(ch) || ch == '_')) return false;
    }

    std::string remainder = Trim(inner.substr(nameBoundary + kArgsClose.size()));
    std::string recoveredArgs;
    if (!remainder.empty()) {
        if (remainder.size() < kArgsClose.size() ||
            remainder.compare(remainder.size() - kArgsClose.size(),
                              kArgsClose.size(),
                              kArgsClose) != 0) {
            return false;
        }
        recoveredArgs = Trim(remainder.substr(
            0, remainder.size() - kArgsClose.size()));
    }

    ToolInvocation recovered;
    recovered.rawBlock = rawTail;
    recovered.name = Lower(rawName);
    recovered.args = recoveredArgs;

    std::string reason;
    if (!IsKnownToolName(recovered.name)) {
        recovered.valid = false;
        recovered.invalidReason = "unknown tool: " + recovered.name;
        invocationOut = recovered;
        return true;
    }
    if (!ValidateToolArgs(recovered.name, recovered.args, reason)) {
        recovered.valid = false;
        recovered.invalidReason = reason;
        invocationOut = recovered;
        return true;
    }

    recovered.valid = true;
    invocationOut = recovered;
    return true;
}

// Tail-of-response content that consists only of whitespace and a small
// number of stray CLOSING tags (e.g. "</name>", "</args>", a truncated
// "</tool_cal" cut off by end-of-stream). Used by the stray-closer
// recovery below to confirm that nothing semantically meaningful
// follows the balanced <name>/<args> body. Any opening tag, prose, or
// other content fails the check, so half-written calls and literal
// protocol explanations in prose do not dispatch.
bool IsWhitespaceAndStrayClosingTags(const std::string& s)
{
    size_t i = 0;
    int tags = 0;
    while (i < s.size()) {
        if (std::isspace(static_cast<unsigned char>(s[i]))) { ++i; continue; }
        if (s[i] != '<') return false;
        ++i;
        if (i >= s.size() || s[i] != '/') return false;
        ++i;
        while (i < s.size() &&
               (std::isalnum(static_cast<unsigned char>(s[i])) ||
                s[i] == '_' || s[i] == '|')) {
            ++i;
        }
        // The trailing '>' may be missing only because the response was
        // cut at end-of-stream mid-tag; anything else after the tag name
        // is not a stray closer.
        if (i < s.size()) {
            if (s[i] != '>') return false;
            ++i;
        }
        if (++tags > 4) return false;  // unbounded tag soup is not "stray"
    }
    return true;
}

// Gemma drift: colon-LESS opener, perfectly well-formed XML
// <name>/<args> body, then a stray closing tag in place of
// </tool_call> before EOS:
//
//   <|tool_call>call
//   <name>write</name>
//   <args>file_list.txt
//   ...content...
//   </args>
//   </name>          <-- mirror of the last tag family, not a closer
//
// FindCloseMarker correctly finds no closer, and the two recovery
// shims above are gated to the colon-native opener, so without this
// shim the shape surfaces as an unterminated-call error — which the
// model then repeats verbatim until the malformed cap stops the loop,
// even though the payload is unambiguous.
//
// Recovery scope is deliberately narrow, mirroring the shims above:
//   * only the XML opener or the colon-less Gemma opener (the colon
//     variants have their own shims),
//   * only when the call consumes the response tail (batch parse at
//     stream end; mid-stream the detector is still waiting),
//   * only when a well-formed <name>...</name> is present and any
//     <args> envelope is balanced,
//   * only when everything after the body is whitespace and/or a few
//     stray closing tags.
//
// The inner content is then parsed by the normal XML inner parser, so
// name/args validation still gates dispatch exactly as it would for a
// properly closed block. EOS right after </args> with no closer at all
// also recovers (same unambiguity argument).
bool TryRecoverTerminalXmlBodyWithStrayCloser(
    const std::string& text,
    size_t             openPos,
    size_t             openerLen,
    ToolInvocation&    invocationOut)
{
    if (openerLen != kOpenXml.size() &&
        openerLen != kOpenGemma.size()) {
        return false;
    }
    if (openPos >= text.size()) return false;

    std::string rawTail = text.substr(openPos);
    if (rawTail.size() > kMaxToolCallBlockBytes) return false;

    std::string inner = rawTail.substr(openerLen);

    size_t nameA = inner.find(kNameOpen);
    if (nameA == std::string::npos) return false;
    size_t nameB = inner.find(kNameClose, nameA + kNameOpen.size());
    if (nameB == std::string::npos) return false;

    size_t argsA = inner.find(kArgsOpen);
    size_t argsB = (argsA == std::string::npos)
                   ? std::string::npos
                   : inner.find(kArgsClose, argsA + kArgsOpen.size());

    // An opened-but-unclosed <args> stays a hard failure (fail closed —
    // same rationale as ParseInnerBlock's partial-envelope rule).
    if (argsA != std::string::npos && argsB == std::string::npos) {
        return false;
    }

    // End of the recognized body: past </name> and, when present, past
    // the first </args>. ParseInnerBlock uses these same first-match
    // boundaries, so the recovered invocation reads the body exactly as
    // a closed block would have been read.
    size_t bodyEnd = nameB + kNameClose.size();
    if (argsB != std::string::npos) {
        bodyEnd = std::max(bodyEnd, argsB + kArgsClose.size());
    }

    if (!IsWhitespaceAndStrayClosingTags(inner.substr(bodyEnd))) {
        return false;
    }

    ToolInvocation recovered;
    if (!ParseInnerBlock(inner, rawTail, recovered)) return false;

    // recovered.valid may still be false (unknown tool / bad args);
    // return it anyway so the agent loop feeds back the SPECIFIC
    // validation reason rather than a generic unterminated error.
    invocationOut = recovered;
    return true;
}

} // namespace

bool ContainsToolCallOpenMarker(const std::string& text)
{
    size_t pos = std::string::npos;
    size_t len = 0;
    return FindFirstOpenMarkerRaw(text, 0, pos, len);
}

std::string MakeToolCallDiagnosticPreview(const std::string& raw)
{
    return MakeRawPreview(raw);
}

// ═══════════════════════════════════════════════════════════════════
//  Batch parser
// ═══════════════════════════════════════════════════════════════════

ParsedAssistantResponse ParseAssistantResponse(const std::string& text)
{
    ParsedAssistantResponse out;

    // Locate the first tool-call opener OUTSIDE reasoning. Subsequent
    // blocks are ignored for execution purposes per protocol; they
    // remain in the prose so the user can see what happened. Openers
    // quoted inside <think>…</think> stay in the prose untouched.
    size_t openPos = std::string::npos;
    size_t openLen = 0;
    if (!FindFirstOpenMarkerOutsideReasoning(text, 0, openPos, openLen)) {
        out.prose = text;
        return out;
    }

    size_t contentStart = openPos + openLen;
    size_t closePos = std::string::npos;
    std::string closer;

    if (!FindCloseMarker(text, contentStart, openLen, closePos, closer)) {
        // Recover the narrow drift shapes that reach end-of-response
        // without a recognized closer:
        //   * colon-native opener, brace body, final `}` (hybrid),
        //   * colon-native opener, mistaken <name>/<path>/<args> arg tag,
        //   * colon-native opener, stray </args> boundaries (dangling),
        //   * colon-native opener, name-on-call-line + newline-separated
        //     payload closed by a bare final </args> (newline-args),
        //   * XML / colon-less opener, well-formed <name>/<args> body,
        //     stray closing tag or EOS in place of </tool_call>.
        // The streaming UI already withholds this tail because it
        // contains a recognized opener; batch completion is the right
        // place to decide if it is safe to dispatch.
        ToolInvocation recovered;
        if (TryRecoverTerminalGemmaHybridWithoutCloser(
                text, openPos, openLen, recovered) ||
            TryRecoverTerminalGemmaTaggedArgsWithoutCloser(
                text, openPos, openLen, recovered) ||
            TryRecoverTerminalGemmaDanglingArgsWithoutCloser(
                text, openPos, openLen, recovered) ||
            TryRecoverTerminalGemmaNewlineArgsWithArgsCloser(
                text, openPos, openLen, recovered) ||
            TryRecoverTerminalXmlBodyWithStrayCloser(
                text, openPos, openLen, recovered)) {
            RejectPlaceholderArgs(recovered);
            out.invocation    = recovered;
            out.hasInvocation = true;
            out.prose         = text.substr(0, openPos);
            return out;
        }

        // Unclosed block — strip the broken tail from user-visible prose.
        // Also cap the diagnostic body so one runaway <tool_call> does
        // not get stored in full in chat history.
        std::string rawTail = text.substr(openPos);

        MalformedBlock m;
        m.rawText = MakeRawPreview(rawTail);
        if (rawTail.size() > kMaxToolCallBlockBytes) {
            m.reason = "unterminated tool call exceeded " +
                       std::to_string(kMaxToolCallBlockBytes) +
                       " bytes before a valid closer was found";
        } else {
            m.reason = MissingCloseReason(openLen);
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
        // Preview, not the full block — this string round-trips into the
        // model context as the failed command echo; see MakeRawPreview.
        m.rawText = MakeRawPreview(rawBlock);
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
    m_inThink       = false;
    m_blockStart    = 0;
    m_openMarkerLen = 0;
}

bool ToolCallStreamDetector::Feed(const std::string& delta)
{
    if (m_complete) return false;  // already fired; caller should Reset or ignore

    m_buffer += delta;

    // ── Stage 1: searching for a tool-call opener ────────────────
    // Anything before the opening marker is prose. We can safely
    // publish everything up to (buffer.size - maxOpenLen + 1) as
    // prose — the trailing window is held back in case a marker is
    // splitting across deltas.
    //
    // Reasoning (<think>…</think>) is tracked as state because the
    // buffer is only a sliding window: once reasoning text has been
    // published as prose, the <think> that opened it is gone from
    // m_buffer.  Reasoning flows through as prose unchanged (ChatDisplay
    // renders it as the thinking block), and openers inside it are not
    // tool calls.
  for (;;) {   // re-entered when a "block" turns out to be leaked reasoning
    while (!m_insideBlock) {
        if (m_inThink) {
            const size_t thinkClose = m_buffer.find(kThinkClose);
            if (thinkClose == std::string::npos) {
                // Hold back enough to catch a </think> split across deltas.
                const size_t kHold = kThinkClose.size() - 1;
                if (m_buffer.size() > kHold) {
                    size_t safeLen = m_buffer.size() - kHold;
                    m_prosePrefix += m_buffer.substr(0, safeLen);
                    m_buffer       = m_buffer.substr(safeLen);
                }
                return false;
            }
            const size_t end = thinkClose + kThinkClose.size();
            m_prosePrefix += m_buffer.substr(0, end);
            m_buffer       = m_buffer.substr(end);
            m_inThink      = false;
            continue;   // rescan the remainder as visible content
        }

        size_t openPos = std::string::npos;
        size_t openLen = 0;
        const bool haveMarker =
            FindFirstOpenMarkerRaw(m_buffer, 0, openPos, openLen);

        const size_t thinkOpen = m_buffer.find(kThinkOpen);
        if (thinkOpen != std::string::npos &&
            (!haveMarker || thinkOpen < openPos)) {
            const size_t end = thinkOpen + kThinkOpen.size();
            m_prosePrefix += m_buffer.substr(0, end);
            m_buffer       = m_buffer.substr(end);
            m_inThink      = true;
            continue;
        }

        if (haveMarker) {
            // kOpenGemma ("<|tool_call>call") is a strict prefix of
            // kOpenGemmaNative ("<|tool_call>call:").  If a stream delta
            // ends exactly after "call", wait for one more byte before
            // committing to the non-native variant; the next byte may be
            // the ':' that changes both the opener length and closer.
            if (openLen == kOpenGemma.size() &&
                openPos + openLen == m_buffer.size()) {
                return false;
            }

            m_prosePrefix += m_buffer.substr(0, openPos);
            m_buffer       = m_buffer.substr(openPos);   // buffer now starts with an opener
            m_insideBlock  = true;
            m_blockStart   = 0;
            m_openMarkerLen = openLen;
            // fall through to stage 2 (maybe closer already in buffer)
        } else {
            // Hold back the last bytes in case a split opener lands
            // here (tool-call opener OR <think>); publish everything
            // else as prose so the UI can render smoothly.
            const size_t kHoldBack =
                std::max(MaxOpenMarkerBytes(), kThinkOpen.size()) - 1;
            if (m_buffer.size() > kHoldBack) {
                size_t safeLen = m_buffer.size() - kHoldBack;
                m_prosePrefix += m_buffer.substr(0, safeLen);
                m_buffer       = m_buffer.substr(safeLen);
            }
            return false;
        }
    }

    // ── Stage 2: inside a tool call, searching for the closer ────
    // m_buffer is guaranteed to start with a recognized opener here.
    // The closer to look for depends on which opener was matched
    // (XML opener -> </tool_call>, gemma-native opener with colon
    // -> <tool_call|>).  m_openMarkerLen, set in stage 1, is the
    // disambiguator.
    size_t contentStart = m_openMarkerLen;
    size_t closePos = std::string::npos;
    std::string closer;
    const bool closed =
        FindCloseMarker(m_buffer, contentStart, m_openMarkerLen, closePos, closer);

    // Leaked reasoning (shape 3 in FindFirstOpenMarkerOutsideReasoning):
    // a stray </think> inside the "block" before any real call started
    // means the opener was a mention in the model's thinking.  Give the
    // text back to the display as prose and resume scanning after it.
    // (A complete quoted example FOLLOWED by a stray </think> cannot be
    // told apart until after the detector has fired; that case is
    // display-only, because dispatch is decided by the batch parser.)
    {
        const size_t stray = m_buffer.find(kThinkClose, contentStart);
        if (stray != std::string::npos &&
            (!closed || stray < closePos) &&
            !SegmentStartedRealCall(m_buffer, contentStart, stray,
                                    m_openMarkerLen)) {
            const size_t end = stray + kThinkClose.size();
            m_prosePrefix  += m_buffer.substr(0, end);
            m_buffer        = m_buffer.substr(end);
            m_insideBlock   = false;
            m_openMarkerLen = 0;
            continue;
        }
    }

    if (!closed) {
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
                " bytes before a valid closer was found";
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
}
