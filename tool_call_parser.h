// tool_call_parser.h
//
// Agent harness — tool call parser (XML protocol).
//
// ─── Protocol ────────────────────────────────────────────────────
// The agent-mode system prompt instructs the model to emit tool
// calls as a single XML-ish block:
//
//    <tool_call>
//    <name>read</name>
//    <args>chat_display.h</args>
//    </tool_call>
//
// Some local models prepend/replace the opener with the sentinel
// "<|tool_call>call". The parser accepts that narrow compatibility
// form too, so raw tool-call markup does not leak into the chat and
// the requested tool still executes.
//
// Qwen models drift into their trained Qwen3-Coder tool format. When a
// block has no <name> tag but opens with <function>NAME</name>,
// <function=NAME</name>, <function>NAME</function>, <function=NAME> or
// <function>NAME>,
// followed only by a balanced <args>…</args> (or ONE native
// <parameter=…>…</parameter>) and an optional </function>, NAME is
// taken as the tool name. Name and args still pass the normal
// IsKnownToolName / ValidateToolArgs gates. Looser shapes stay malformed,
// with an error that quotes the <function> tag the model actually wrote.
//
// Only one block per assistant turn is honored on the XML protocol.
// If the model emits more, additional blocks are left in the prose for
// the user to see but do not trigger execution.  (Native function
// calling supports multi-call batches; see AgentController.)
//
// Important limitation: this is an in-band protocol. A model that emits
// a well-formed literal example of the protocol can be indistinguishable
// from an intentional tool call. Approval gates and tool risk tiers are
// the safety layer above this parser.
//
// ─── Reasoning is never a tool call ──────────────────────────────
// ChatClient re-wraps delta.reasoning_content as inline
// <think>…</think>, so the text both parse modes see includes the
// model's reasoning.  Models routinely quote the protocol while
// thinking ("I need to use the exact format: <tool_call><name>
// powershell</name><args>...</args></tool_call>"), and that example
// must not be dispatched as a real call.
//
// Openers are ignored when they sit in reasoning, in any of the three
// shapes it arrives in (details at FindFirstOpenMarkerOutsideReasoning):
//   * wrapped   <think>…</think>; an unterminated <think> hides the rest;
//   * orphan    …reasoning…</think>  (template prefilled <think>);
//   * leaked    reasoning that llama-server's splitter pushed into
//               content at the first "<tool_call>" mention, closed by
//               the model's own stray </think>.
// A "</think>" inside the ARGS of a real call (writing source that
// contains the tag) does not count, so such calls still dispatch.
// Finally, a call whose entire args is a template placeholder ("...",
// "ARGS") is returned invalid instead of executed.
//
// The streaming detector (display only) handles the same shapes, except
// a complete quoted example followed later by a stray </think>: that
// may be hidden while it streams, but it is never EXECUTED, because
// dispatch is decided by the batch parser at completion.
//
// ─── Two parse modes ─────────────────────────────────────────────
// Batch  : ParseAssistantResponse() — take a complete string,
//          return prose + first invocation + all malformed blocks.
//          Used when loading a saved conversation, for replay, and
//          for the non-streaming code path.
//
// Stream : ToolCallStreamDetector — fed deltas as they arrive,
//          signals when a complete <tool_call>...</tool_call> has
//          been seen so the agent loop can short-circuit the
//          remaining stream and execute.  The detector holds
//          enough buffered bytes to survive tag splits across
//          delta boundaries (same trick DisplayAssistantDelta uses
//          for </think>).
//
// Both modes share the same low-level primitives defined in the
// .cpp so behavior stays consistent.
//
#pragma once

#include "tool_invocation.h"

#include <string>
#include <vector>

// ─── Batch parse result ─────────────────────────────────────────
// prose        : everything outside the first tool_call block plus
//                any text after it — i.e. the parts of the model's
//                output meant for the user to read.  For v1 we
//                keep it simple: prose is the full assistant reply
//                with the consumed <tool_call> block removed.
//
// invocation   : the first tool call found.  Check `invocation.valid`
//                before dispatching; invalid invocations are still
//                returned so the caller can surface a diagnostic.
//
// hasInvocation: true iff a <tool_call> block was found.  Separate
//                from invocation.valid because a malformed block
//                still counts as "the model tried to call a tool"
//                for loop-control purposes.
//
// malformed    : blocks we tried to parse but couldn't — missing
//                <name> tag, unclosed <args>, etc.  Each entry is
//                the raw span plus the reason.  Used by the agent
//                loop's malformed-counter stop rule.
struct MalformedBlock {
    std::string rawText;
    std::string reason;
};

struct ParsedAssistantResponse {
    std::string                   prose;
    ToolInvocation                invocation;
    bool                          hasInvocation = false;
    std::vector<MalformedBlock>   malformed;
};

// Parses a complete assistant response string.  Never throws;
// on any structural issue, returns a best-effort result with
// malformed entries populated.
ParsedAssistantResponse ParseAssistantResponse(const std::string& text);

// Head+tail diagnostic preview of a (possibly large) tool-call block.
// Small blocks pass through unchanged. Use this for any malformed-call
// text that will be echoed back into the model context: feeding a
// small local model a multi-KB verbatim copy of its own broken block
// reinforces the broken pattern and produces identical retries until
// the malformed cap trips. Head AND tail are kept because for
// wrong-closer mistakes the evidence is at the end of the block.
std::string MakeToolCallDiagnosticPreview(const std::string& raw);

// True if text contains a recognized tool-call opening marker. Used
// by streaming UI cleanup to avoid flushing partial tool-call syntax.
// Deliberately NOT reasoning-aware: it inspects the detector's small
// held-back window, which carries no <think> context of its own.
bool ContainsToolCallOpenMarker(const std::string& text);

// ─── Streaming detector ──────────────────────────────────────────
// Stateful incremental parser.  Call Feed() for each token delta;
// when Complete() becomes true, GetInvocation() returns the parsed
// call and GetProsePrefix() returns any text that appeared before
// the tool_call block (which the display should render normally).
// The suffix after </tool_call> is discarded — when the detector
// fires, the agent loop is about to cancel the stream anyway.
class ToolCallStreamDetector {
public:
    ToolCallStreamDetector();

    // Accepts a new chunk of assistant output.  Updates internal
    // state.  Returns true iff a complete <tool_call> was just
    // recognized on this feed.
    bool Feed(const std::string& delta);

    // True iff Feed() has observed a full <tool_call>...</tool_call>.
    bool Complete() const { return m_complete; }

    // Valid only after Complete() returns true.  The prose prefix
    // is everything the model emitted before the opening <tool_call>
    // tag — callers should render this as normal assistant text
    // before handling the invocation.
    const std::string&    GetProsePrefix() const { return m_prosePrefix; }
    const ToolInvocation& GetInvocation()  const { return m_invocation;  }

    // Content buffered but not yet emitted — useful when the agent
    // loop needs to decide, at stream-end, what hasn't been shown
    // to the user.  While mid-parse of a tag the detector holds
    // characters back; this exposes them for flushing.
    const std::string& GetHeldBuffer() const { return m_buffer; }

    // Reset state so the detector can be reused for the next
    // streamed response in the same agent loop iteration.
    void Reset();

private:
    std::string    m_buffer;        // sliding window of recent chars
    std::string    m_prosePrefix;   // accumulated text before <tool_call>
    ToolInvocation m_invocation;    // populated when m_complete flips true
    bool           m_complete = false;
    bool           m_insideBlock = false;    // past a tool-call opener
    bool           m_inThink     = false;    // inside <think>…</think>; openers ignored
    size_t         m_blockStart  = 0;        // index of '<' in m_buffer
    size_t         m_openMarkerLen = 0;      // bytes consumed by the opener
};
