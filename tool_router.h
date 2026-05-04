// tool_router.h
//
// Phase 2 (architecture refactor): ToolSpec + ToolRouter.
//
// A ToolSpec is the single, declarative description of a tool: its
// name, its human-readable description, its argument shape (as a
// JSON Schema for Phase 3 native function calling), its shape-level
// validator, and its dispatcher.
//
// The router owns a map of name -> spec.  Phase 1 had three parallel
// switches across ten tools (IsKnownToolName, ValidateToolArgs,
// DispatchInvocation).  In Phase 2 those three switches collapse to
// a single map lookup; adding a tool becomes one ToolSpec value
// registered in tool_router.cpp's BuildSpecs().
//
// ─── Phase progression ───────────────────────────────────────────
//
//   P2 (this phase): Router used as the back-end behind the existing
//                    public IsKnownToolName / ValidateToolArgs /
//                    DispatchInvocation entry points.  No behaviour
//                    changes; the model never sees a difference.
//
//   P3:              parameters_json_schema gets handed to llama-server
//                    on /v1/chat/completions for models with native
//                    tool-calling chat templates.
//
//   P4:              slash-command handlers in MyFrame route through
//                    the router instead of calling tool functions
//                    directly, unifying user-typed and agent-emitted
//                    invocations behind one entry point.
//
//   P10:             MCP-discovered tools register through the same
//                    router as built-ins — uniform dispatch.
//
// The router has no dependency on the agent loop, MyFrame, or the
// chat UI.  It can be exercised from a test harness or a future CLI
// without pulling those in.
//
#pragma once

#include "tool_dispatcher.h"   // DispatchOutcome, ToolInvocationResult
#include "tool_invocation.h"   // ToolInvocation, tool_names::*
#include "tool_context.h"      // ToolContext

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

class GrepExecutor;
class CmdExecutor;
class PythonRunner;

// ─── Dispatch dependencies ──────────────────────────────────────
// Per-call non-owning pointers to the long-lived executors that
// async tools reach into.  Sync tools ignore these fields; the
// router hands the same struct to every spec's dispatch function
// so the spec signatures stay uniform.
struct DispatchDeps {
    GrepExecutor* grepExec     = nullptr;
    CmdExecutor*  cmdExec      = nullptr;
    PythonRunner* pythonRunner = nullptr;
};

// ─── Tool safety metadata ────────────────────────────────────────
// Small, internal safety profile used as the single source of truth
// for model-facing prompt summaries and future approval/audit surfaces.
// Enforcement still lives in the actual tool implementations and
// policy layers; this metadata describes the contract the model sees.
struct ToolSafetyProfile {
    // True when the tool is intended only to inspect/read state.
    bool readOnly = false;

    // True when the tool can create, edit, delete, or otherwise mutate
    // local files/folders or create artifact outputs.  Moderate tools
    // may run after conversational consent or explicit slash invocation;
    // only tools with requiresApproval pause for an approval card.
    bool mutatesFiles = false;

    // True when the UI/agent flow must pause for user approval before
    // dispatching the tool.
    bool requiresApproval = false;

    // True when read-only inspection may target absolute local paths
    // outside the conversation working directory, if the underlying
    // tool/policy allows the path.
    bool mayInspectOutsideCwd = false;

    // True when writes/mutations are restricted to the conversation
    // working directory.
    bool writesInsideCwdOnly = false;

    // True for broad shell/process tools whose safety is enforced by
    // a separate policy allowlist.
    bool policyEnforced = false;

    // Human-readable safety summary.  Keep short: this is used in
    // prompts and may be appended to native tool descriptions.
    std::string summary;
};

// ─── ToolSpec ────────────────────────────────────────────────────
// Pure value type.  Everything a tool exposes to the rest of the
// system is on this struct; the only state lives inside the
// dispatch function (closures over the tool's actual implementation).
struct ToolSpec {
    // Wire-level name.  Lowercase, matches the lookup key in the
    // router's map and the literal token the model emits in <n>.
    std::string name;

    // Short human-readable summary, used by:
    //   - Phase 3: handed to the model via the function-calling
    //     `description` field on /v1/chat/completions.
    //   - Future Settings UI / "what tools are available" surfaces.
    // Keep these one-line and behaviour-focused, not implementation
    // detail.
    std::string description;

    // JSON Schema for the tool's arguments.  Populated now so Phase 3
    // is a flip-the-switch change rather than a writing-schemas
    // change.  Currently unused at runtime (the XML protocol carries
    // freeform text args); served to the model in Phase 3.
    //
    // Stored as a string of JSON text, not a Poco::JSON::Object,
    // so this header doesn't pull Poco JSON into every TU that
    // includes it.  The router treats it as opaque.
    std::string parameters_json_schema;

    // Declarative safety metadata.  This is NOT the enforcement layer;
    // it keeps the router catalog, native function descriptions, and
    // system prompts aligned so the model understands which tools are
    // read-only, which tools mutate files, and which tools require
    // approval.
    ToolSafetyProfile safety;

    // Shape-level argument validation.  Mirrors the contract of the
    // historical ValidateToolArgs: returns true if args are
    // well-formed enough to dispatch; on false, fills `reasonOut`
    // with a short diagnostic surfaced to the model.
    //
    // This is shape-only — no path resolution, no existence checks,
    // no policy gates.  Those live inside the dispatch function or
    // deeper in the per-tool implementation.
    using ValidateFn =
        std::function<bool(const std::string& args,
                           std::string&       reasonOut)>;
    ValidateFn validate;

    // Synchronous-or-async dispatcher.  Returns a fully-populated
    // DispatchOutcome.  Async tools (grep, powershell) set
    // status = Async and rely on the executor inside `deps` to
    // post a completion event back to ctx.eventHandler.  Sync
    // tools set status = Completed and fill in `result` directly.
    using DispatchFn =
        std::function<DispatchOutcome(const ToolInvocation& inv,
                                      const ToolContext&    ctx,
                                      const DispatchDeps&   deps)>;
    DispatchFn dispatch;
};

// ─── ToolRouter ──────────────────────────────────────────────────
// Owns the registry; resolves names to specs.  Construction is
// cheap; the global instance is built lazily on first GetGlobalRouter()
// call.  All accessors are const so the router is safe to share
// across threads as long as no one calls Register() after init.
class ToolRouter {
public:
    void Register(ToolSpec spec);

    // Returns nullptr if `name` (case-insensitive, trimmed) is not
    // registered.  Callers MUST null-check before dereferencing.
    const ToolSpec* Find(const std::string& name) const;

    // Membership query — same semantics as Find() != nullptr but
    // matches the older IsKnownToolName naming.
    bool Has(const std::string& name) const;

    // Snapshot of all registered specs in registration order.  Used
    // by Phase 3 to build the function-calling tool list to send to
    // the model, and by Phase 4's unified slash-handler.
    std::vector<const ToolSpec*> All() const;

private:
    // Names are stored lowercased for case-insensitive lookup.  The
    // spec retains its original-case `name` field for display.
    std::unordered_map<std::string, ToolSpec> m_specs;
    std::vector<std::string>                  m_order;    // insertion order
};

// Lazily-initialized singleton.  Populated on first call from
// BuildBuiltinSpecs() in tool_router.cpp.
ToolRouter& GetGlobalRouter();

// ─── Phase 3c-i: native tool catalog ───────────────────────────
// Render every registered tool as an entry in the OpenAI function-
// calling shape and return the resulting JSON array as a string.
// The output is suitable for splicing into a /v1/chat/completions
// body as the "tools" field.
//
// Each entry has the form
//   { "type": "function",
//     "function": { "name": ..., "description": ..., "parameters": ... } }
// where "parameters" is the JSON schema string already stored on
// the spec (see Phase 2's parameters_json_schema, parsed here as
// a JSON value rather than re-stringified).
//
// Used by ChatHistory::BuildChatRequestJson when the active
// protocol is ToolProtocol::Native.  XML-protocol requests do not
// call this and the resulting body has no "tools" field.
std::string BuildToolsArrayJson(const ToolRouter& router);

// Short generated safety summary for the system prompt.  This keeps
// XML and native prompts from drifting away from ToolSpec metadata.
std::string BuildToolSafetySummaryText(const ToolRouter& router);
