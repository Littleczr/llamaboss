#define _CRT_SECURE_NO_WARNINGS

// tool_invocation.cpp
//
// Phase 2: this file used to host the IsKnownToolName / ValidateToolArgs
// switches across all ten tools.  Both now live as ToolSpec fields
// inside the router (see tool_router.cpp -- BuildBuiltinSpecs and the
// per-tool ValXxx validators).  The two free-function entry points
// declared in tool_invocation.h are kept for parser compatibility and
// delegate to GetGlobalRouter().

#include "tool_invocation.h"
#include "tool_router.h"
#include "tool_call_elision.h"   // copied-elision-marker guard

bool IsKnownToolName(const std::string& name)
{
    return GetGlobalRouter().Has(name);
}

bool ValidateToolArgs(const std::string& name,
                      const std::string& args,
                      std::string&       reasonOut)
{
    const ToolSpec* spec = GetGlobalRouter().Find(name);
    if (!spec) {
        reasonOut = "unknown tool: " + name;
        return false;
    }
    // A shortened old tool call copied into a new one (2026-10-01).
    // Every protocol funnels through here (XML, slash, native after
    // projection; native raw JSON is also checked in agent_controller),
    // so no tool can run or write a truncated copy.
    if (lb_toolcall_elision::ContainsArgElisionMarker(args)) {
        reasonOut = lb_toolcall_elision::CopiedElisionRejection(args);
        return false;
    }
    if (!spec->validate) {
        // Belt-and-braces: a registered spec without a validator is
        // treated as accept-anything-shape-wise.  All built-in specs
        // set this; the branch exists so a future MCP-discovered
        // tool (Phase 10) without its own validator still works.
        return true;
    }
    return spec->validate(args, reasonOut);
}
