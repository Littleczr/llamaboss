// tool_invocation.h
//
// Phase 4: Agent harness — tool invocation types.
//
// A ToolInvocation is the protocol-neutral internal representation
// of a single tool call, regardless of whether it arrived as a
// text-mode <tool_call>...</tool_call> block (primary) or a
// JSON function-call payload (future provider-native adapter).
//
// The Phase 3 slash-command handlers on MyFrame already do the
// right thing for user-typed "/read foo.cpp" etc.  The dispatcher
// (see tool_dispatcher.h) maps a ToolInvocation onto the same
// underlying tool functions so user-typed and agent-emitted
// invocations produce byte-identical results.
//
#pragma once

#include <string>

// ─── Known tool names ───────────────────────────────────────────
// Centralized so parser, dispatcher, and system-prompt builder all
// agree.  Keep lowercase — the parser normalizes <name> contents
// to lowercase before matching.
namespace tool_names {
    constexpr const char* kRead       = "read";
    constexpr const char* kLs         = "ls";
    constexpr const char* kGrep       = "grep";
    constexpr const char* kPwd        = "pwd";
    constexpr const char* kPowerShell = "powershell";
    constexpr const char* kPythonHealth = "python_health";
    constexpr const char* kCsvInspect   = "csv_inspect";
    constexpr const char* kCsvReport    = "csv_report";
    constexpr const char* kCsvToXlsx    = "csv_to_xlsx";
    constexpr const char* kXlsxInspect  = "xlsx_inspect";
    constexpr const char* kXlsxReport   = "xlsx_report";
    constexpr const char* kPdfExtractText = "pdf_extract_text";
    constexpr const char* kPdfInspectForm = "pdf_inspect_form";
    constexpr const char* kPdfFillForm    = "pdf_fill_form";
    constexpr const char* kDocxExtractText = "docx_extract_text";
    constexpr const char* kDocxInspect     = "docx_inspect";
    constexpr const char* kPythonCreateScript = "python_create_script";
    constexpr const char* kPythonRunScript = "python_run_script";
    constexpr const char* kPythonInstallPackage = "python_install_package";
    constexpr const char* kOpen       = "open";
    constexpr const char* kWrite      = "write";
    constexpr const char* kMkdir      = "mkdir";
    constexpr const char* kEdit       = "edit";
    constexpr const char* kDelete     = "delete";
    constexpr const char* kNotesRead   = "notes_read";
    constexpr const char* kNotesAppend = "notes_append";
    constexpr const char* kProjectNotesRead   = "project_notes_read";
    constexpr const char* kProjectNotesAppend = "project_notes_append";
}

// ─── Single parsed tool call ────────────────────────────────────
// name     : normalized (lowercase, trimmed).  Empty on parse
//            failure — inspect `valid` first.
// args     : raw text between <args> and </args>, with surrounding
//            whitespace trimmed.  Newlines inside are preserved so
//            future multi-line grep patterns work.
// rawBlock : the full <tool_call>...</tool_call> span the model
//            emitted, verbatim.  Used for:
//              - elision markers ("re-run <tool_call>..." in the
//                collapsed tool body)
//              - malformed-call diagnostics shown to the user
//              - retry/repeat detection in the agent loop
// valid    : true iff name maps to a known tool AND args is
//            well-formed for that tool.  Args validation is
//            conservative: empty-when-required fails, anything
//            else passes (the actual tool handles path resolution
//            and will report detailed errors via errorBody).
struct ToolInvocation {
    std::string name;
    std::string args;
    std::string rawBlock;
    bool        valid = false;

    // Human-readable reason the invocation is invalid.  Empty when
    // valid == true.  Surfaced to the model as a <tool_result> with
    // an error chip so it can self-correct.
    std::string invalidReason;

    // Phase 3c-ii: id of the model-emitted tool call this
    // invocation was synthesized from.  Empty for XML-protocol
    // invocations (no ids exist in that protocol).  When non-empty,
    // the dispatched result is stored as a tool-result message
    // tagged with this id so subsequent requests can thread
    // role:"tool" replies via tool_call_id.
    std::string toolCallId;
};

// Returns true iff `name` is a recognized tool.
bool IsKnownToolName(const std::string& name);

// Validates args for a given tool name.  Fills `reasonOut` with a
// short diagnostic on failure.  Only shape-level validation — path
// resolution, existence, permission etc. are the tool's job.
bool ValidateToolArgs(const std::string& name,
                      const std::string& args,
                      std::string&       reasonOut);
