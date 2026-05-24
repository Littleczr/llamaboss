#define _CRT_SECURE_NO_WARNINGS

// tool_router.cpp

#include "tool_router.h"

#include "python_arg_policy.h"

// Tool function signatures + result structs.
#include "tool_read.h"
#include "tool_ls.h"
#include "tool_grep.h"
#include "tool_path.h"
#include "tool_open.h"
#include "tool_write.h"
#include "tool_mkdir.h"
#include "tool_edit.h"
#include "tool_delete.h"
#include "tool_notes.h"
#include "tool_web_fetch.h"
#include "cmd_executor.h"
#include "python_runner.h"
#include "command_policy.h"
#include "chat_history.h"
#include "server_manager.h"
#include "path_safety.h"
#include "project_manager.h"
#include "tool_path_safety.h"
#include "tool_python_syntax.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <vector>

#include <wx/filename.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <Poco/JSON/Object.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Stringifier.h>

namespace {

// ─── Local helpers (mirror the older anonymous helpers in
// tool_dispatcher.cpp / tool_invocation.cpp).  Self-contained on
// purpose — Phase 2 deliberately doesn't introduce a tool_util TU.

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return (char)std::tolower(c); });
    return s;
}

std::string Trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}


// NOTE: The historical C++-side pre-repair helpers (LooksLikeNamePropertyAt,
// IsValidJsonObjectForToolArg, RepairXlsxWorkbookJsonArg) used to insert a
// missing rows-array ']' before xlsx_create_workbook ran.  The Python helper
// (load_workbook_spec) now performs string-aware JSON repair covering this
// case and several others (trailing commas, spurious closers, missing commas
// between adjacent containers, invalid backslash escapes), and surfaces
// repair_note in the helper output.  The C++ layer was redundant and not
// string-aware, so it has been removed.  Repair reporting still flows back
// through the helper JSON.

// "/read foo.cpp" style echo, identical to what HandleSlash* and the
// historical MakeCommandEcho produced.

std::string MakeCommandEcho(const std::string& name,
                            const std::string& args)
{
    if (args.empty()) return "/" + name;
    return "/" + name + " " + args;
}

bool HasNewline(const std::string& s)
{
    return s.find('\n') != std::string::npos || s.find('\r') != std::string::npos;
}

std::string FirstLine(const std::string& s)
{
    size_t nl = s.find_first_of("\r\n");
    return (nl == std::string::npos) ? s : s.substr(0, nl);
}

std::string ReplaceFirstLine(const std::string& args, const std::string& replacement)
{
    size_t nl = args.find_first_of("\r\n");
    if (nl == std::string::npos) return replacement;

    size_t after = nl;
    // Preserve CRLF/LF/CR as the original separator.
    if (args[after] == '\r' && after + 1 < args.size() && args[after + 1] == '\n') after += 2;
    else after += 1;

    return replacement + args.substr(nl, after - nl) + args.substr(after);
}

bool ExistingPathAsGivenOrCwdRelative(const std::string& arg, const ToolContext& ctx)
{
    const std::string path = tool_path_safety::ResolveProjectAwareToolPath(arg, ctx.cwd, ctx.activeProjectRoot);
    return !path.empty() && (IsFile(path) || IsDirectory(path));
}

// Phase 4: when a project is attached, let read/open/helper tools accept a
// project source by bare filename ("zayra.pdf"), stem ("zayra"),
// Sources/<name>, or unique partial match. Existing absolute/cwd-relative paths
// still win, so old behavior does not change.
std::string ResolveProjectSourceArgForSinglePathTool(const std::string& args,
                                                     const ToolContext& ctx)
{
    const std::string trimmed = Trim(args);
    if (trimmed.empty() || ctx.activeProjectRoot.empty()) return args;
    if (HasNewline(trimmed)) return args;

    if (ExistingPathAsGivenOrCwdRelative(trimmed, ctx)) return args;

    ProjectSourceInfo src;
    std::string error;
    if (ProjectManager::ResolveProjectSource(ctx.activeProjectRoot, trimmed, src, error)) {
        return src.path;
    }

    return args;
}

std::string ResolveProjectWorkflowArgForSinglePathTool(const std::string& args,
                                                       const ToolContext& ctx)
{
    const std::string trimmed = Trim(args);
    if (trimmed.empty() || ctx.activeProjectRoot.empty()) return args;
    if (HasNewline(trimmed)) return args;

    if (ExistingPathAsGivenOrCwdRelative(trimmed, ctx)) return args;

    ProjectWorkflowInfo wf;
    std::string error;
    if (ProjectManager::ResolveProjectWorkflow(ctx.activeProjectRoot, trimmed, wf, error)) {
        return wf.path;
    }

    return args;
}
std::string ResolveProjectWorkflowScriptArgForSinglePathTool(const std::string& args,
                                                             const ToolContext& ctx)
{
    const std::string trimmed = Trim(args);
    if (trimmed.empty() || ctx.activeProjectRoot.empty()) return args;
    if (HasNewline(trimmed)) return args;

    if (ExistingPathAsGivenOrCwdRelative(trimmed, ctx)) return args;

    ProjectWorkflowScriptInfo script;
    std::string error;
    if (ProjectManager::ResolveProjectWorkflowScript(ctx.activeProjectRoot, trimmed, script, error)) {
        return script.path;
    }

    return args;
}

// Skill counterparts: same shape as the project resolvers, but they
// don't gate on ctx.activeProjectRoot -- Skills are
// available regardless of whether a project is attached.  The fall-
// through order in ResolveProjectFileArgForSinglePathTool runs project
// scope first so a per-project workflow with the same filename
// shadows the Skill one.
std::string ResolveSkillArgForSinglePathTool(const std::string& args,
                                             const ToolContext& ctx)
{
    const std::string trimmed = Trim(args);
    if (trimmed.empty()) return args;
    if (HasNewline(trimmed)) return args;

    if (ExistingPathAsGivenOrCwdRelative(trimmed, ctx)) return args;

    SkillInfo skill;
    std::string error;
    if (ProjectManager::ResolveSkill(trimmed, skill, error)) {
        return skill.path;
    }

    return args;
}

std::string ResolveSkillScriptArgForSinglePathTool(const std::string& args,
                                                   const ToolContext& ctx)
{
    const std::string trimmed = Trim(args);
    if (trimmed.empty()) return args;
    if (HasNewline(trimmed)) return args;

    if (ExistingPathAsGivenOrCwdRelative(trimmed, ctx)) return args;

    SkillScriptInfo script;
    std::string error;
    if (ProjectManager::ResolveSkillScript(trimmed, script, error)) {
        return script.path;
    }

    return args;
}


std::string ResolveProjectFileArgForSinglePathTool(const std::string& args,
                                                   const ToolContext& ctx)
{
    const std::string afterSource = ResolveProjectSourceArgForSinglePathTool(args, ctx);
    if (afterSource != args) return afterSource;
    const std::string afterWorkflow = ResolveProjectWorkflowArgForSinglePathTool(args, ctx);
    if (afterWorkflow != args) return afterWorkflow;
    const std::string afterScript = ResolveProjectWorkflowScriptArgForSinglePathTool(args, ctx);
    if (afterScript != args) return afterScript;
    // Skills lane: project scope wins on filename collision, so these
    // run only after the project-scoped resolvers leave the args
    // untouched. Available even when no project is attached.
    const std::string afterSkill = ResolveSkillArgForSinglePathTool(args, ctx);
    if (afterSkill != args) return afterSkill;
    return ResolveSkillScriptArgForSinglePathTool(args, ctx);
}

std::string ResolveProjectSourceArgForFirstLineTool(const std::string& args,
                                                    const ToolContext& ctx)
{
    if (ctx.activeProjectRoot.empty()) return args;

    const std::string first = Trim(FirstLine(args));
    if (first.empty()) return args;
    if (ExistingPathAsGivenOrCwdRelative(first, ctx)) return args;

    ProjectSourceInfo src;
    std::string error;
    if (ProjectManager::ResolveProjectSource(ctx.activeProjectRoot, first, src, error)) {
        return ReplaceFirstLine(args, src.path);
    }
    return args;
}

std::string ResolveProjectFolderAliasForPathTool(const std::string& args,
                                                 const ToolContext& ctx)
{
    const std::string trimmed = Trim(args);
    if (trimmed.empty()) return args;
    if (ExistingPathAsGivenOrCwdRelative(trimmed, ctx)) return args;

    std::string key = Lower(trimmed);
    std::replace(key.begin(), key.end(), '\\', '/');

    if (key == "skills" || key == "skill" ||
        key == "global skills" || key == "global skill" ||
        key == "llamaboss skills") {
        ProjectManager::EnsureSkillsRoot();
        return ProjectManager::GetSkillsDir();
    }

    if (!ctx.activeProjectRoot.empty()) {
        if (key == "sources" || key == "source" ||
            key == "project sources" || key == "project source" ||
            key == "project/sources" || key == "project/source") {
            return ProjectManager::ProjectSourcesPath(ctx.activeProjectRoot);
        }
        if (key == "workflows" || key == "workflow" ||
            key == "project workflows" || key == "project workflow" ||
            key == "project/workflows" || key == "project/workflow") {
            return ProjectManager::ProjectWorkflowsPath(ctx.activeProjectRoot);
        }
        if (key == "inputs" || key == "input" ||
            key == "project inputs" || key == "project input" ||
            key == "project/inputs" || key == "project/input") {
            return ctx.activeProjectRoot + "\\Inputs";
        }
        if (key == "outputs" || key == "output" ||
            key == "project outputs" || key == "project output" ||
            key == "project/outputs" || key == "project/output") {
            return ctx.activeProjectRoot + "\\Outputs";
        }
        if (key == "notes" || key == "project notes" || key == "project/notes") {
            return ctx.activeProjectRoot + "\\Notes";
        }
        if (key == "templates" || key == "template" ||
            key == "project templates" || key == "project template" ||
            key == "project/templates" || key == "project/template") {
            return ctx.activeProjectRoot + "\\Templates";
        }
        if (key == "project.md" || key == "project/project.md") {
            return ctx.activeProjectRoot + "\\PROJECT.md";
        }
        if (key == "project.json" || key == "project/project.json") {
            return ctx.activeProjectRoot + "\\project.json";
        }
    }

    if (!ctx.activeProjectRoot.empty() && tool_path_safety::IsKnownProjectRelativePath(trimmed)) {
        return tool_path_safety::ResolveProjectAwareToolPath(trimmed, ctx.cwd, ctx.activeProjectRoot);
    }

    return ResolveProjectFileArgForSinglePathTool(args, ctx);
}

ToolSafetyProfile ReadOnlySafety(const std::string& summary,
                                 bool mayInspectOutsideCwd = true,
                                 bool policyEnforced = false)
{
    ToolSafetyProfile safety;
    safety.tier                 = RiskTier::Safe;
    safety.readOnly             = true;
    safety.mayInspectOutsideCwd = mayInspectOutsideCwd;
    safety.policyEnforced       = policyEnforced;
    safety.network              = NetworkReach::None;
    safety.reversibility        = Reversibility::Reversible;
    safety.summary              = summary;
    return safety;
}

ToolSafetyProfile MutatingCwdSafety(const std::string& summary)
{
    ToolSafetyProfile safety;
    safety.tier                = RiskTier::Moderate;
    safety.mutatesFiles        = true;
    safety.writesInsideCwdOnly = true;
    safety.network             = NetworkReach::None;
    // Default to Reversible — a freshly-written file can simply be
    // deleted afterward.  Tools that destroy or replace prior content
    // (delete, overwrite_file when no staged backup is retained)
    // override to Irreversible at their registration site.
    safety.reversibility       = Reversibility::Reversible;
    safety.summary             = summary;
    return safety;
}

std::string SafetySuffix(const ToolSpec& spec)
{
    std::vector<std::string> bits;

    if (spec.safety.readOnly) {
        bits.push_back("read-only");
    }
    if (spec.safety.mayInspectOutsideCwd) {
        bits.push_back("may inspect absolute local paths outside cwd");
    }
    if (spec.safety.mutatesFiles) {
        bits.push_back("mutates files/folders");
    }
    if (spec.safety.writesInsideCwdOnly) {
        bits.push_back("cwd/project-scoped");
    }
    if (spec.safety.requiresApproval()) {
        bits.push_back("requires approval");
    }
    if (spec.safety.policyEnforced) {
        bits.push_back("policy-enforced");
    }

    if (bits.empty()) return {};

    std::ostringstream ss;
    ss << " Safety: ";
    for (size_t i = 0; i < bits.size(); ++i) {
        if (i > 0) ss << "; ";
        ss << bits[i];
    }
    ss << ".";
    return ss.str();
}

// First-line echo for tools whose args carry trailing content
// (write, edit).  The full content is still visible in the body if
// the user expands the block; cramming it into the header would
// either truncate or balloon the chat.
std::string FirstLineEcho(const std::string& slashName,
                          const std::string& args)
{
    size_t nl = args.find('\n');
    std::string firstLine = (nl == std::string::npos)
                                ? args
                                : args.substr(0, nl);
    while (!firstLine.empty()) {
        char c = firstLine.back();
        if (c == ' ' || c == '\t' || c == '\r') firstLine.pop_back();
        else break;
    }
    return firstLine.empty() ? ("/" + slashName)
                             : ("/" + slashName + " " + firstLine);
}

// Common pre-fill for every dispatch outcome: the tag, the raw block
// echo for elision, the toolName for display.  Per-tool dispatch
// functions adjust icon/chips/body/etc. on top of this.
DispatchOutcome PreFill(const ToolInvocation& inv,
                        const char*           toolTag,
                        const char*           toolName)
{
    DispatchOutcome out;
    out.status = DispatchStatus::Completed;
    out.result.toolTag       = toolTag;
    out.result.invocationRaw = inv.rawBlock;
    out.result.toolName      = toolName;
    return out;
}

// ─── Per-tool dispatch implementations ──────────────────────────
// These are the same bodies as the old static DispatchRead/Ls/...
// functions in tool_dispatcher.cpp, captured here as named functions
// so the spec lambdas can call them by name.  Logic is byte-equivalent
// to Phase 1; behavior unchanged.

DispatchOutcome DoRead(const ToolInvocation& inv,
                       const ToolContext&    ctx,
                       const DispatchDeps&   /*deps*/)
{
    DispatchOutcome out = PreFill(inv, tool_names::kRead, "Read");
    out.result.iconUtf8    = "\xF0\x9F\x93\x84";   // 📄
    const std::string toolArgs = ResolveProjectFileArgForSinglePathTool(inv.args, ctx);
    out.result.commandEcho = MakeCommandEcho(inv.name, toolArgs);

    ReadResult r = ReadFile(toolArgs, ctx);
    out.result.chips     = r.chips;
    out.result.body      = r.body;
    out.result.errorBody = r.errorBody;
    out.result.bodyLang  = r.bodyLang;
    return out;
}

void ParseReadHeadArgs(const std::string& args,
                       std::string&       pathOut,
                       size_t&            linesOut)
{
    std::string s = Trim(args);
    linesOut = 40;
    pathOut.clear();

    if (s.empty()) return;

    auto parseLines = [](const std::string& raw, size_t& out) -> bool {
        std::string n = Trim(raw);
        if (n.empty() || !std::all_of(n.begin(), n.end(), [](unsigned char c){ return std::isdigit(c); }))
            return false;
        try {
            unsigned long v = std::stoul(n);
            if (v == 0) v = 1;
            if (v > 500) v = 500;
            out = static_cast<size_t>(v);
            return true;
        } catch (...) {
            return false;
        }
    };

    // Native structured projection uses: "<lines>\n<path>".
    size_t nl = s.find('\n');
    if (nl != std::string::npos) {
        std::string first = Trim(s.substr(0, nl));
        std::string rest  = Trim(s.substr(nl + 1));
        size_t parsedLines = 40;
        if (parseLines(first, parsedLines)) {
            linesOut = parsedLines;
            pathOut = rest;
            return;
        }
    }

    std::string lower = Lower(s);
    const std::string headPrefix = "--head";
    if (lower.rfind(headPrefix, 0) == 0) {
        std::string rest = Trim(s.substr(headPrefix.size()));
        size_t sp = rest.find_first_of(" \t");
        if (sp != std::string::npos) {
            std::string n = Trim(rest.substr(0, sp));
            std::string p = Trim(rest.substr(sp + 1));
            size_t parsedLines = 40;
            if (parseLines(n, parsedLines)) {
                linesOut = parsedLines;
                pathOut = p;
                return;
            }
        }
        pathOut = rest;
        return;
    }

    size_t sp = s.find_first_of(" \t");
    if (sp != std::string::npos) {
        std::string first = Trim(s.substr(0, sp));
        std::string rest  = Trim(s.substr(sp + 1));
        size_t parsedLines = 40;
        if (parseLines(first, parsedLines)) {
            linesOut = parsedLines;
            pathOut = rest;
            return;
        }
    }

    pathOut = s;
}

DispatchOutcome DoReadHead(const ToolInvocation& inv,
                           const ToolContext&    ctx,
                           const DispatchDeps&   /*deps*/)
{
    DispatchOutcome out = PreFill(inv, tool_names::kReadHead, "Read Head");
    out.result.iconUtf8    = "\xF0\x9F\x93\x84";   // 📄

    std::string rawPath;
    size_t lines = 40;
    ParseReadHeadArgs(inv.args, rawPath, lines);
    const std::string toolArgs = ResolveProjectFileArgForSinglePathTool(rawPath, ctx);
    out.result.commandEcho = "/read_head " + std::to_string(lines) + " " + toolArgs;

    ReadResult r = ReadFileHead(toolArgs, ctx, lines);
    out.result.chips     = r.chips;
    out.result.body      = r.body;
    out.result.errorBody = r.errorBody;
    out.result.bodyLang  = r.bodyLang;
    return out;
}

DispatchOutcome DoLs(const ToolInvocation& inv,
                     const ToolContext&    ctx,
                     const DispatchDeps&   /*deps*/)
{
    DispatchOutcome out = PreFill(inv, tool_names::kLs, "List");
    out.result.iconUtf8    = "\xF0\x9F\x93\x81";   // 📁
    const std::string toolArgs = ResolveProjectFolderAliasForPathTool(inv.args, ctx);
    out.result.commandEcho = MakeCommandEcho(inv.name, toolArgs);

    LsResult r = ListDirectory(toolArgs, ctx);
    out.result.chips     = r.chips;
    out.result.body      = r.body;
    out.result.errorBody = r.errorBody;
    out.result.bodyLang  = r.bodyLang;
    return out;
}

DispatchOutcome DoPwd(const ToolInvocation& inv,
                      const ToolContext&    ctx,
                      const DispatchDeps&   /*deps*/)
{
    DispatchOutcome out = PreFill(inv, tool_names::kPwd, "Pwd");
    out.result.iconUtf8    = "\xE2\x9E\xA4";       // ➤
    out.result.commandEcho = "/pwd";
    out.result.body        = ctx.cwd + "\n";
    return out;
}

DispatchOutcome DoGrep(const ToolInvocation& inv,
                       const ToolContext&    ctx,
                       const DispatchDeps&   deps)
{
    DispatchOutcome out;
    out.result.toolTag       = tool_names::kGrep;
    out.result.invocationRaw = inv.rawBlock;
    out.result.toolName      = "Grep";
    out.result.commandEcho   = MakeCommandEcho(inv.name, inv.args);

    if (!deps.grepExec) {
        out.status            = DispatchStatus::Invalid;
        out.result.errorBody  = "Grep executor not available in this context.";
        return out;
    }

    // Same arg-parsing rule as HandleSlashGrep: first whitespace token
    // is the pattern, rest is path.
    std::string pattern, rawPath;
    {
        const std::string& s = inv.args;
        size_t sep = s.find_first_of(" \t");
        if (sep == std::string::npos) {
            pattern = s;
        } else {
            pattern = s.substr(0, sep);
            rawPath = s.substr(sep + 1);
            size_t a = rawPath.find_first_not_of(" \t\r\n");
            size_t b = rawPath.find_last_not_of(" \t\r\n");
            if (a == std::string::npos) rawPath.clear();
            else                        rawPath = rawPath.substr(a, b - a + 1);
        }
    }

    if (pattern.empty()) {
        out.status            = DispatchStatus::Invalid;
        out.result.errorBody  = "Empty grep pattern.";
        return out;
    }

    std::string target = rawPath.empty() ? ctx.cwd : rawPath;
    if (!rawPath.empty()) {
        target = ResolveProjectFileArgForSinglePathTool(rawPath, ctx);
    }
    std::string resolved = tool_path_safety::ResolveProjectAwareToolPath(target, ctx.cwd, ctx.activeProjectRoot);
    if (resolved.empty()) {
        out.status            = DispatchStatus::Invalid;
        out.result.errorBody  = "Could not resolve path: " + target;
        return out;
    }
    if (!IsFile(resolved) && !IsDirectory(resolved)) {
        out.status            = DispatchStatus::Invalid;
        out.result.errorBody  = "Path not found: " + resolved;
        return out;
    }

    std::string echo = rawPath.empty()
        ? MakeCommandEcho(inv.name, pattern)
        : MakeCommandEcho(inv.name, pattern + " " + target);
    if (!deps.grepExec->Start(pattern, resolved, echo, ctx)) {
        out.status            = DispatchStatus::Invalid;
        out.result.commandEcho = echo;
        out.result.errorBody  = "Could not start grep (already running?).";
        return out;
    }

    out.status              = DispatchStatus::Async;
    out.result.commandEcho  = echo;
    return out;
}

// ─── PowerShell rejection recovery hints ────────────────────────
//
// EvaluatePowerShellCommand now has three outcomes:
//   - auto-allow clearly read-only inspection commands,
//   - route broader syntactically usable shell automation through the
//     approval-card path,
//   - reject malformed/empty commands.
//
// The helpers below remain useful for the final rejection case and for
// any future policy errors that still need a recovery hint.  They are
// appended only when dispatch actually rejects an invocation; commands
// that merely require approval never land in this error path.

inline std::string PowerShellRejectionHeadLowered(const std::string& cmdRaw)
{
    // Skip leading whitespace on the user's input.
    size_t a = 0;
    while (a < cmdRaw.size() &&
           (cmdRaw[a] == ' ' || cmdRaw[a] == '\t' ||
            cmdRaw[a] == '\r' || cmdRaw[a] == '\n')) {
        ++a;
    }
    if (a >= cmdRaw.size()) return std::string();

    // Take everything up to the next whitespace or pipe -- this
    // matches the head-token rule used by the policy evaluator.
    size_t b = a;
    while (b < cmdRaw.size() &&
           cmdRaw[b] != ' ' && cmdRaw[b] != '\t' &&
           cmdRaw[b] != '\r' && cmdRaw[b] != '\n' &&
           cmdRaw[b] != '|') {
        ++b;
    }
    std::string head = cmdRaw.substr(a, b - a);

    // ASCII fold for case-insensitive comparison; non-ASCII passes
    // through unchanged.  PowerShell is case-insensitive for cmdlet
    // names so the model can have written "Python" or "PIP".
    for (char& c : head) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    }
    return head;
}

inline std::string PowerShellRejectionHint(const std::string& cmdRaw)
{
    const std::string head = PowerShellRejectionHeadLowered(cmdRaw);
    if (head.empty()) return std::string();

    // ── Tier 1: head is itself a router-registered tool ─────────
    // Most common confusion mode: the model emits a plain tool
    // name as if powershell were a generic dispatcher.  Skip
    // "powershell" itself (would be a no-op recommendation) and
    // any tool whose name overlaps a real PowerShell keyword.
    if (head != "powershell" && GetGlobalRouter().Has(head)) {
        return "\n\nHint: '" + head + "' is itself a tool. Call it directly with "
               "<name>" + head + "</name> and the appropriate <args>, instead of "
               "running it through powershell.";
    }

    // ── Tier 2: python launcher attempts ────────────────────────
    if (head == "python"     || head == "py"      || head == "python3" ||
        head == "python.exe" || head == "py.exe"  || head == "python3.exe") {
        return "\n\nHint: PowerShell cannot launch python directly. Use one of:\n"
               "  python_health         - check the python backend (version, exe path, cwd)\n"
               "  python_run_script     - run an existing .py from the conversation Scripts lane,\n"
               "                          a project Workflows lane, or the Skills lane;\n"
               "                          optional argv lines may follow the script line\n"
               "  python_create_script  - save a reviewable new .py before running it (approval-gated)\n"
               "  python_install_package - install a single PyPI package (approval-gated)";
    }

    // ── Tier 3: pip launcher attempts ───────────────────────────
    if (head == "pip"     || head == "pip3" ||
        head == "pip.exe" || head == "pip3.exe") {
        return "\n\nHint: PowerShell cannot launch pip directly. Use "
               "<name>python_install_package</name><args>PACKAGE_NAME</args>. "
               "Each install renders an approval card showing the exact name "
               "pip will see, even when chat-wide trust is enabled.";
    }

    // ── Tier 4: cmd-style mutation commands ─────────────────────
    if (head == "mkdir" || head == "md") {
        return "\n\nHint: PowerShell is read-only here. Use "
               "<name>mkdir</name><args>relative/path</args> to create a directory "
               "inside the working directory or active project root.";
    }
    if (head == "rm"    || head == "del" || head == "erase" ||
        head == "rmdir" || head == "rd") {
        return "\n\nHint: PowerShell is read-only here. Use "
               "<name>delete</name><args>relative/path</args>. Files are removed; "
               "non-empty directories are refused -- list with ls and delete entries "
               "individually if needed. Approval-gated.";
    }
    if (head == "cp"  || head == "copy" ||
        head == "mv"  || head == "move" ||
        head == "ren" || head == "rename") {
        return "\n\nHint: PowerShell is read-only here. To produce a new file with "
               "specific content, use <name>write</name>. There is no native copy/move "
               "tool; if you need to duplicate an existing file, read its contents and "
               "write them to the new path.";
    }
    if (head == "cd" || head == "chdir" || head == "set-location") {
        return "\n\nHint: The working directory is per-conversation and tools cannot "
               "change it. Pass paths relative to the current cwd, or absolute paths "
               "where the tool's policy permits it.";
    }
    if (head == "touch" || head == "new-item") {
        return "\n\nHint: Use <name>write</name> to create a new file with content, "
               "or <name>mkdir</name> to create a directory.";
    }

    // ── Tier 5: cmd-style read patterns ─────────────────────────
    // (Tier 1 already catches the case where head equals an actual
    // tool name like "read" or "ls"; this tier handles unix/cmd
    // shorthand the model might reach for from training data.)
    if (head == "cat"  || head == "type" || head == "more" ||
        head == "less" || head == "head" || head == "tail") {
        return "\n\nHint: Use <name>read</name> for file contents, or PowerShell's "
               "Get-Content (which is on the read-only allowlist).";
    }
    if (head == "dir") {
        return "\n\nHint: Use <name>ls</name> for a directory listing, or PowerShell's "
               "Get-ChildItem.";
    }
    if (head == "findstr" || head == "rg" || head == "ack") {
        return "\n\nHint: Use <name>grep</name> for content search, or PowerShell's "
               "Select-String.";
    }

    return std::string();
}

inline bool RejectionLooksLikeAllowlistMiss(const std::string& reason)
{
    return reason.find("not on the read-only allowlist") != std::string::npos
        || reason.find("not a plain cmdlet name")        != std::string::npos
        || reason.find("empty pipeline stage")           != std::string::npos;
}

constexpr const char* kPowerShellAllowedShapeSummary =
    "\n\nAllowed PowerShell heads on this allowlist:\n"
    "  Read-action verbs:  Get-*, Test-*, Measure-*, Select-*, Where-*, Sort-*,\n"
    "                      Group-*, Compare-*, ConvertTo-*, ConvertFrom-*,\n"
    "                      Format-*, Find-*, Resolve-*\n"
    "  Exact names:        ForEach-Object, Out-String, Out-Default, Out-Host,\n"
    "                      Out-Null, date, whoami, hostname, echo\n"
    "Pipelines are allowed when every stage's head fits one of these. Use "
    "Select-Object -ExpandProperty for projections instead of script blocks.";

DispatchOutcome DoPowerShell(const ToolInvocation& inv,
                             const ToolContext&    ctx,
                             const DispatchDeps&   deps)
{
    DispatchOutcome out;
    out.result.toolTag       = tool_names::kPowerShell;
    out.result.invocationRaw = inv.rawBlock;
    out.result.iconUtf8      = "\xE2\x9A\x99";   // ⚙
    out.result.toolName      = "PowerShell";
    out.result.commandEcho   = inv.args;
    out.result.bodyLang      = "powershell";

    if (!deps.cmdExec) {
        out.status            = DispatchStatus::Invalid;
        out.result.errorBody  = "PowerShell executor not available "
                                "in this context.";
        out.result.chips      = { "error" };
        return out;
    }

    PolicyDecision decision = EvaluatePowerShellCommand(inv.args);
    if (!decision.allowed && !decision.requiresApproval) {
        out.status            = DispatchStatus::Invalid;

        // Only malformed/empty commands reach this rejection branch.
        // Broader valid shell commands are paused earlier by the
        // approval gate and are intentionally allowed to dispatch once
        // the user approves them.
        std::string body = "Command rejected by policy: " + decision.reason;

        const std::string hint = PowerShellRejectionHint(inv.args);
        if (!hint.empty()) {
            body += hint;
        }
        if (RejectionLooksLikeAllowlistMiss(decision.reason)) {
            body += kPowerShellAllowedShapeSummary;
        }

        out.result.errorBody  = body;
        out.result.chips      = { "blocked" };
        return out;
    }


    if (!deps.cmdExec->Start(inv.args, ctx.cwd, ctx.timeoutMs)) {
        out.status            = DispatchStatus::Invalid;
        out.result.errorBody  = "Could not start PowerShell "
                                "(another command is already running).";
        out.result.chips      = { "error" };
        return out;
    }

    out.status = DispatchStatus::Async;
    return out;
}

DispatchOutcome DoPythonHealth(const ToolInvocation& inv,
                                 const ToolContext&    ctx,
                                 const DispatchDeps&   deps)
{
    DispatchOutcome out;
    out.result.toolTag       = tool_names::kPythonHealth;
    out.result.invocationRaw = inv.rawBlock;
    out.result.iconUtf8      = "\xF0\x9F\x90\x8D";   // 🐍
    out.result.toolName      = "Python Health";
    out.result.commandEcho   = "python_health";
    out.result.bodyLang      = "json";

    if (!deps.pythonRunner) {
        out.status           = DispatchStatus::Invalid;
        out.result.errorBody = "Python runner not available in this context.";
        out.result.chips     = { "error" };
        return out;
    }

    if (!deps.pythonRunner->StartHealth(ctx.cwd, ctx.timeoutMs)) {
        out.status           = DispatchStatus::Invalid;
        out.result.errorBody = "Could not start Python helper (another Python helper is already running?).";
        out.result.chips     = { "error" };
        return out;
    }

    out.status = DispatchStatus::Async;
    return out;
}

DispatchOutcome DoCsvInspect(const ToolInvocation& inv,
                             const ToolContext&    ctx,
                             const DispatchDeps&   deps)
{
    DispatchOutcome out;
    out.result.toolTag       = tool_names::kCsvInspect;
    out.result.invocationRaw = inv.rawBlock;
    out.result.iconUtf8      = "\xF0\x9F\x93\x8A";   // 📊
    out.result.toolName      = "CSV Inspect";
    const std::string toolArgs = ResolveProjectSourceArgForSinglePathTool(inv.args, ctx);
    out.result.commandEcho   = MakeCommandEcho("csv_inspect", toolArgs);
    out.result.bodyLang      = "json";

    if (!deps.pythonRunner) {
        out.status           = DispatchStatus::Invalid;
        out.result.errorBody = "Python runner not available in this context.";
        out.result.chips     = { "error" };
        return out;
    }

    if (!deps.pythonRunner->StartCsvInspect(toolArgs, ctx.cwd, ctx.timeoutMs)) {
        out.status           = DispatchStatus::Invalid;
        out.result.errorBody = "Could not start CSV inspect helper (another Python helper is already running?).";
        out.result.chips     = { "error" };
        return out;
    }

    out.status = DispatchStatus::Async;
    return out;
}

DispatchOutcome DoCsvReport(const ToolInvocation& inv,
                            const ToolContext&    ctx,
                            const DispatchDeps&   deps)
{
    DispatchOutcome out;
    out.result.toolTag       = tool_names::kCsvReport;
    out.result.invocationRaw = inv.rawBlock;
    out.result.iconUtf8      = "\xF0\x9F\x93\x9D";   // 📝
    out.result.toolName      = "CSV Report";
    const std::string toolArgs = ResolveProjectSourceArgForSinglePathTool(inv.args, ctx);
    out.result.commandEcho   = MakeCommandEcho("csv_report", toolArgs);
    out.result.bodyLang      = "json";

    if (!deps.pythonRunner) {
        out.status           = DispatchStatus::Invalid;
        out.result.errorBody = "Python runner not available in this context.";
        out.result.chips     = { "error" };
        return out;
    }

    if (!deps.pythonRunner->StartCsvReport(toolArgs, ctx.cwd, ctx.timeoutMs)) {
        out.status           = DispatchStatus::Invalid;
        out.result.errorBody = "Could not start CSV report helper (another Python helper is already running?).";
        out.result.chips     = { "error" };
        return out;
    }

    out.status = DispatchStatus::Async;
    return out;
}

DispatchOutcome DoCsvToXlsx(const ToolInvocation& inv,
                             const ToolContext&    ctx,
                             const DispatchDeps&   deps)
{
    DispatchOutcome out;
    out.result.toolTag       = tool_names::kCsvToXlsx;
    out.result.invocationRaw = inv.rawBlock;
    out.result.iconUtf8      = "\xF0\x9F\x93\x97";   // 📗
    out.result.toolName      = "CSV to XLSX";
    const std::string toolArgs = ResolveProjectSourceArgForSinglePathTool(inv.args, ctx);
    out.result.commandEcho   = MakeCommandEcho("csv_to_xlsx", toolArgs);
    out.result.bodyLang      = "json";

    if (!deps.pythonRunner) {
        out.status           = DispatchStatus::Invalid;
        out.result.errorBody = "Python runner not available in this context.";
        out.result.chips     = { "error" };
        return out;
    }

    if (!deps.pythonRunner->StartCsvToXlsx(toolArgs, ctx.cwd, ctx.timeoutMs)) {
        out.status           = DispatchStatus::Invalid;
        out.result.errorBody = "Could not start CSV to XLSX helper (another Python helper is already running?).";
        out.result.chips     = { "error" };
        return out;
    }

    out.status = DispatchStatus::Async;
    return out;
}

DispatchOutcome DoXlsxInspect(const ToolInvocation& inv,
                              const ToolContext&    ctx,
                              const DispatchDeps&   deps)
{
    DispatchOutcome out;
    out.result.toolTag       = tool_names::kXlsxInspect;
    out.result.invocationRaw = inv.rawBlock;
    out.result.iconUtf8      = "\xF0\x9F\x93\x8A";   // 📊
    out.result.toolName      = "XLSX Inspect";
    const std::string toolArgs = ResolveProjectSourceArgForSinglePathTool(inv.args, ctx);
    out.result.commandEcho   = MakeCommandEcho("xlsx_inspect", toolArgs);
    out.result.bodyLang      = "json";

    if (!deps.pythonRunner) {
        out.status           = DispatchStatus::Invalid;
        out.result.errorBody = "Python runner not available in this context.";
        out.result.chips     = { "error" };
        return out;
    }

    if (!deps.pythonRunner->StartXlsxInspect(toolArgs, ctx.cwd, ctx.timeoutMs)) {
        out.status           = DispatchStatus::Invalid;
        out.result.errorBody = "Could not start XLSX inspect helper (another Python helper is already running?).";
        out.result.chips     = { "error" };
        return out;
    }

    out.status = DispatchStatus::Async;
    return out;
}

DispatchOutcome DoXlsxReport(const ToolInvocation& inv,
                             const ToolContext&    ctx,
                             const DispatchDeps&   deps)
{
    DispatchOutcome out;
    out.result.toolTag       = tool_names::kXlsxReport;
    out.result.invocationRaw = inv.rawBlock;
    out.result.iconUtf8      = "\xF0\x9F\x93\x97";   // 📗
    out.result.toolName      = "XLSX Report";
    const std::string toolArgs = ResolveProjectSourceArgForSinglePathTool(inv.args, ctx);
    out.result.commandEcho   = MakeCommandEcho("xlsx_report", toolArgs);
    out.result.bodyLang      = "json";

    if (!deps.pythonRunner) {
        out.status           = DispatchStatus::Invalid;
        out.result.errorBody = "Python runner not available in this context.";
        out.result.chips     = { "error" };
        return out;
    }

    if (!deps.pythonRunner->StartXlsxReport(toolArgs, ctx.cwd, ctx.timeoutMs)) {
        out.status           = DispatchStatus::Invalid;
        out.result.errorBody = "Could not start XLSX report helper (another Python helper is already running?).";
        out.result.chips     = { "error" };
        return out;
    }

    out.status = DispatchStatus::Async;
    return out;
}


DispatchOutcome DoXlsxCreateWorkbook(const ToolInvocation& inv,
                                      const ToolContext&    ctx,
                                      const DispatchDeps&   deps)
{
    DispatchOutcome out;
    out.result.toolTag       = tool_names::kXlsxCreateWorkbook;
    out.result.invocationRaw = inv.rawBlock;
    out.result.iconUtf8      = "\xF0\x9F\x93\x97";   // 📗
    out.result.toolName      = "Create Workbook";
    // JSON repair (trailing commas, missing rows-array closes, spurious
    // closers, invalid escapes) is handled string-aware in the Python helper
    // and surfaced via repair_note/json_repaired in its output.
    const std::string toolArgs = Trim(inv.args);
    out.result.commandEcho   = "xlsx_create_workbook";
    out.result.bodyLang      = "json";

    if (!deps.pythonRunner) {
        out.status           = DispatchStatus::Invalid;
        out.result.errorBody = "Python runner not available in this context.";
        out.result.chips     = { "error" };
        return out;
    }

    if (!deps.pythonRunner->StartXlsxCreateWorkbook(toolArgs, ctx.cwd, ctx.timeoutMs)) {
        out.status           = DispatchStatus::Invalid;
        out.result.errorBody = "Could not start XLSX create workbook helper (another Python helper is already running?).";
        out.result.chips     = { "error" };
        return out;
    }

    out.status = DispatchStatus::Async;
    return out;
}

DispatchOutcome DoPdfExtractText(const ToolInvocation& inv,
                                   const ToolContext&    ctx,
                                   const DispatchDeps&   deps)
{
    DispatchOutcome out;
    out.result.toolTag       = tool_names::kPdfExtractText;
    out.result.invocationRaw = inv.rawBlock;
    out.result.iconUtf8      = "\xF0\x9F\x93\x84";   // 📄
    out.result.toolName      = "PDF Extract Text";
    const std::string toolArgs = ResolveProjectSourceArgForSinglePathTool(inv.args, ctx);
    out.result.commandEcho   = MakeCommandEcho("pdf_extract_text", toolArgs);
    out.result.bodyLang      = "json";

    if (!deps.pythonRunner) {
        out.status           = DispatchStatus::Invalid;
        out.result.errorBody = "Python runner not available in this context.";
        out.result.chips     = { "error" };
        return out;
    }

    if (!deps.pythonRunner->StartPdfExtractText(toolArgs, ctx.cwd, ctx.timeoutMs)) {
        out.status           = DispatchStatus::Invalid;
        out.result.errorBody = "Could not start PDF text extraction helper (another Python helper is already running?).";
        out.result.chips     = { "error" };
        return out;
    }

    out.status = DispatchStatus::Async;
    return out;
}

DispatchOutcome DoPdfInspectForm(const ToolInvocation& inv,
                                 const ToolContext&    ctx,
                                 const DispatchDeps&   deps)
{
    DispatchOutcome out;
    out.result.toolTag       = tool_names::kPdfInspectForm;
    out.result.invocationRaw = inv.rawBlock;
    out.result.iconUtf8      = "\xF0\x9F\x93\x84";   // 📄
    out.result.toolName      = "PDF Inspect Form";
    const std::string toolArgs = ResolveProjectSourceArgForSinglePathTool(inv.args, ctx);
    out.result.commandEcho   = MakeCommandEcho("pdf_inspect_form", toolArgs);
    out.result.bodyLang      = "json";

    if (!deps.pythonRunner) {
        out.status           = DispatchStatus::Invalid;
        out.result.errorBody = "Python runner not available in this context.";
        out.result.chips     = { "error" };
        return out;
    }

    if (!deps.pythonRunner->StartPdfInspectForm(toolArgs, ctx.cwd, ctx.timeoutMs)) {
        out.status           = DispatchStatus::Invalid;
        out.result.errorBody = "Could not start PDF form inspector helper (another Python helper is already running?).";
        out.result.chips     = { "error" };
        return out;
    }

    out.status = DispatchStatus::Async;
    return out;
}

DispatchOutcome DoPdfFillForm(const ToolInvocation& inv,
                              const ToolContext&    ctx,
                              const DispatchDeps&   deps)
{
    DispatchOutcome out;
    out.result.toolTag       = tool_names::kPdfFillForm;
    out.result.invocationRaw = inv.rawBlock;
    out.result.iconUtf8      = "\xF0\x9F\x93\x84";   // 📄
    out.result.toolName      = "PDF Fill Form";
    // Echo only the first line (path) so the chat header doesn't show
    // a JSON blob after the command name.
    const std::string toolArgs = ResolveProjectSourceArgForFirstLineTool(inv.args, ctx);
    out.result.commandEcho   = FirstLineEcho("pdf_fill_form", toolArgs);
    out.result.bodyLang      = "json";

    if (!deps.pythonRunner) {
        out.status           = DispatchStatus::Invalid;
        out.result.errorBody = "Python runner not available in this context.";
        out.result.chips     = { "error" };
        return out;
    }

    if (!deps.pythonRunner->StartPdfFillForm(toolArgs, ctx.cwd, ctx.timeoutMs)) {
        out.status           = DispatchStatus::Invalid;
        out.result.errorBody = "Could not start PDF form fill helper (another Python helper is already running?).";
        out.result.chips     = { "error" };
        return out;
    }

    out.status = DispatchStatus::Async;
    return out;
}

DispatchOutcome DoDocxExtractText(const ToolInvocation& inv,
                                  const ToolContext&    ctx,
                                  const DispatchDeps&   deps)
{
    DispatchOutcome out;
    out.result.toolTag       = tool_names::kDocxExtractText;
    out.result.invocationRaw = inv.rawBlock;
    out.result.iconUtf8      = "\xF0\x9F\x93\x84";   // 📄
    out.result.toolName      = "DOCX Extract Text";
    const std::string toolArgs = ResolveProjectSourceArgForSinglePathTool(inv.args, ctx);
    out.result.commandEcho   = MakeCommandEcho("docx_extract_text", toolArgs);
    out.result.bodyLang      = "json";

    if (!deps.pythonRunner) {
        out.status           = DispatchStatus::Invalid;
        out.result.errorBody = "Python runner not available in this context.";
        out.result.chips     = { "error" };
        return out;
    }

    if (!deps.pythonRunner->StartDocxExtractText(toolArgs, ctx.cwd, ctx.timeoutMs)) {
        out.status           = DispatchStatus::Invalid;
        out.result.errorBody = "Could not start DOCX text extraction helper (another Python helper is already running?).";
        out.result.chips     = { "error" };
        return out;
    }

    out.status = DispatchStatus::Async;
    return out;
}

DispatchOutcome DoDocxInspect(const ToolInvocation& inv,
                              const ToolContext&    ctx,
                              const DispatchDeps&   deps)
{
    DispatchOutcome out;
    out.result.toolTag       = tool_names::kDocxInspect;
    out.result.invocationRaw = inv.rawBlock;
    out.result.iconUtf8      = "\xF0\x9F\x93\x84";   // 📄
    out.result.toolName      = "DOCX Inspect";
    const std::string toolArgs = ResolveProjectSourceArgForSinglePathTool(inv.args, ctx);
    out.result.commandEcho   = MakeCommandEcho("docx_inspect", toolArgs);
    out.result.bodyLang      = "json";

    if (!deps.pythonRunner) {
        out.status           = DispatchStatus::Invalid;
        out.result.errorBody = "Python runner not available in this context.";
        out.result.chips     = { "error" };
        return out;
    }

    if (!deps.pythonRunner->StartDocxInspect(toolArgs, ctx.cwd, ctx.timeoutMs)) {
        out.status           = DispatchStatus::Invalid;
        out.result.errorBody = "Could not start DOCX inspect helper (another Python helper is already running?).";
        out.result.chips     = { "error" };
        return out;
    }

    out.status = DispatchStatus::Async;
    return out;
}


// ─── python_create_script ───────────────────────────────────────
// Synchronous, approval-gated artifact creator.  It does NOT run
// Python.  It creates a .py script in the conversation Scripts
// lane and returns the file as a PresentedFile artifact card.

struct PythonCreateScriptResult {
    std::vector<std::string> chips;
    std::string body;
    std::string errorBody;
    std::string createdPath;
    std::string displayName;
    size_t      sizeBytes = 0;
    int         lineCount = 0;
};

std::string JoinToolPath(const std::string& a, const std::string& b)
{
    if (a.empty()) return b;
    const char sep = wxFILE_SEP_PATH;
    if (a.back() == '/' || a.back() == '\\') return a + b;
    return a + std::string(1, sep) + b;
}

std::string ParentDirOfPath(std::string path)
{
    while (!path.empty() && (path.back() == '/' || path.back() == '\\')) {
        path.pop_back();
    }
    size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return std::string();
    return path.substr(0, pos);
}

std::string PathBaseNameTool(std::string path)
{
    while (!path.empty() && (path.back() == '/' || path.back() == '\\')) {
        path.pop_back();
    }
    size_t pos = path.find_last_of("/\\");
    return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

bool StartsWithTool(const std::string& s, const std::string& prefix)
{
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

std::string WorkflowRootFromCwdTool(const std::string& cwd)
{
    std::string clean = cwd;
    while (!clean.empty() && (clean.back() == '/' || clean.back() == '\\')) {
        clean.pop_back();
    }
    if (clean.empty()) return std::string();

    if (Lower(PathBaseNameTool(clean)) != "workspace") return std::string();

    std::string parent = ParentDirOfPath(clean);
    std::string workflows = ParentDirOfPath(parent);
    if (parent.empty() || workflows.empty()) return std::string();

    if (!StartsWithTool(PathBaseNameTool(parent), "chat_")) return std::string();
    if (Lower(PathBaseNameTool(workflows)) != "workflows") return std::string();
    return parent;
}

std::string LlamaBossScriptsDir(const std::string& cwd = std::string())
{
    std::string workflowRoot = WorkflowRootFromCwdTool(cwd);
    if (!workflowRoot.empty()) return JoinToolPath(workflowRoot, "Scripts");

    std::string root = ParentDirOfPath(ServerManager::GetDefaultWorkspaceDir());
    if (root.empty()) root = ParentDirOfPath(ServerManager::GetWorkspaceDir());
    return JoinToolPath(root, "Scripts");
}

size_t CountTextLines(const std::string& s)
{
    if (s.empty()) return 0;
    size_t n = 0;
    for (char c : s) if (c == '\n') ++n;
    if (s.back() != '\n') ++n;
    return n;
}

std::string HumanBytesLocal(size_t b)
{
    std::ostringstream ss;
    ss << std::fixed;
    if (b < 1024) {
        ss << b << " B";
    } else if (b < 1024 * 1024) {
        ss.precision(1);
        ss << (b / 1024.0) << " KB";
    } else {
        ss.precision(2);
        ss << (b / (1024.0 * 1024.0)) << " MB";
    }
    return ss.str();
}

std::string ElapsedChipLocal(std::chrono::steady_clock::time_point t0)
{
    double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();
    std::ostringstream ts;
    ts << std::fixed;
    ts.precision(elapsed < 10.0 ? 2 : 1);
    ts << elapsed << "s";
    return ts.str();
}

void SplitPythonCreateScriptArgs(const std::string& argsBlob,
                                 std::string&       filenameOut,
                                 std::string&       contentOut)
{
    size_t nl = argsBlob.find('\n');
    if (nl == std::string::npos) {
        filenameOut = argsBlob;
        contentOut.clear();
    } else {
        filenameOut = argsBlob.substr(0, nl);
        contentOut  = argsBlob.substr(nl + 1);
    }
    filenameOut = Trim(filenameOut);

    if (!contentOut.empty() && contentOut.back() != '\n') {
        contentOut += '\n';
    }
}

bool NormalizePythonScriptFilename(const std::string& requested,
                                   std::string&       filenameOut,
                                   std::string&       errorOut)
{
    std::string name = Trim(requested);
    if (name.empty()) {
        errorOut = "python_create_script requires a filename on the first line of args.";
        return false;
    }

    if (name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos ||
        name.find(':') != std::string::npos) {
        errorOut = "python_create_script accepts a filename only, not a path. Scripts are always saved in the conversation Scripts folder.";
        return false;
    }

    // Friendly default: "list_pdfs" becomes "list_pdfs.py".
    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) {
        name += ".py";
    } else {
        std::string ext = Lower(name.substr(dot));
        if (ext != ".py") {
            errorOut = "python_create_script only creates .py files.";
            return false;
        }
    }

    std::string safe = path_safety::SanitizeFilename(name, "");
    if (safe.empty() || safe != name) {
        errorOut = "Unsafe Python script filename '" + name + "'. Try '" +
                   (safe.empty() ? std::string("script.py") : safe) + "'.";
        return false;
    }

    if (python_arg_policy::IsReservedBuiltInPythonHelperName(name)) {
        errorOut = "Reserved Python helper filename: " + name +
                   ". Choose a project-specific name such as custom_" + name +
                   " so user-created scripts never collide with LlamaBoss built-in helpers.";
        return false;
    }

    filenameOut = name;
    return true;
}


void SplitPythonRunScriptInvocationForValidation(const std::string& raw,
                                               std::string&       scriptRequestOut,
                                               std::vector<std::string>& argvOut)
{
    scriptRequestOut.clear();
    argvOut.clear();

    auto trimCopy = [](const std::string& s) -> std::string {
        const size_t first = s.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return std::string();
        const size_t last = s.find_last_not_of(" \t\r\n");
        return s.substr(first, last - first + 1);
    };

    const size_t firstBreak = raw.find_first_of("\r\n");
    if (firstBreak != std::string::npos) {
        scriptRequestOut = trimCopy(raw.substr(0, firstBreak));

        size_t restStart = firstBreak;
        while (restStart < raw.size() &&
               (raw[restStart] == '\r' || raw[restStart] == '\n')) {
            ++restStart;
        }

        std::string rest = restStart < raw.size() ? raw.substr(restStart) : std::string();
        size_t lineStart = 0;
        while (lineStart <= rest.size()) {
            size_t lineEnd = rest.find('\n', lineStart);
            std::string line = (lineEnd == std::string::npos)
                ? rest.substr(lineStart)
                : rest.substr(lineStart, lineEnd - lineStart);

            std::string arg = trimCopy(line);
            if (!arg.empty()) argvOut.push_back(std::move(arg));

            if (lineEnd == std::string::npos) break;
            lineStart = lineEnd + 1;
        }
        return;
    }

    const std::string oneLine = trimCopy(raw);
    if (oneLine.empty()) return;

    // Preferred contract is multi-line args: script on line 1, each argv
    // value on a later line. This fallback keeps validation aligned with the
    // runner's forgiving handling of "helper.py C:\\path\\to\\input".
    std::string lower = Lower(oneLine);
    size_t search = 0;
    while (true) {
        size_t py = lower.find(".py", search);
        if (py == std::string::npos) break;
        const size_t afterPy = py + 3;
        const bool terminalScriptMarker =
            afterPy == oneLine.size() ||
            (afterPy < oneLine.size() &&
             (oneLine[afterPy] == ' ' || oneLine[afterPy] == '\t'));
        if (terminalScriptMarker) {
            scriptRequestOut = trimCopy(oneLine.substr(0, afterPy));
            if (afterPy < oneLine.size()) {
                std::string inlineArg = trimCopy(oneLine.substr(afterPy));
                if (!inlineArg.empty()) argvOut.push_back(std::move(inlineArg));
            }
            return;
        }
        search = afterPy;
    }

    scriptRequestOut = oneLine;
}

bool NormalizePythonRunScriptFilename(const std::string& requested,
                                      std::string&       filenameOut,
                                      std::string&       errorOut)
{
    std::string name = Trim(requested);
    if (name.empty()) {
        errorOut = "python_run_script requires a script filename or script path.";
        return false;
    }

    // Friendly project-relative form: Workflows\helper.py means the
    // same thing as helper.py for active project workflow scripts.  Keep
    // this narrow: no nested paths and no arbitrary directory prefixes.
    {
        std::string pathish = name;
        std::replace(pathish.begin(), pathish.end(), '/', '\\');
        std::string key = Lower(pathish);
        const std::string p1 = "workflows\\";
        const std::string p2 = "project\\workflows\\";
        if (key.rfind(p1, 0) == 0) {
            name = pathish.substr(p1.size());
        } else if (key.rfind(p2, 0) == 0) {
            name = pathish.substr(p2.size());
        }
    }

    const bool pathShaped =
        name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos ||
        name.find(':') != std::string::npos;

    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) {
        if (pathShaped) {
            errorOut = "python_run_script path arguments must point to a .py file.";
            return false;
        }
        name += ".py";
    } else if (Lower(name.substr(dot)) != ".py") {
        errorOut = "python_run_script only runs .py files from the conversation Scripts folder, the active project Workflows folder, or the LlamaBoss Skills folder.";
        return false;
    }

    // Shape validation only.  Plain filenames get the historical strict
    // sanitizer.  Path-shaped arguments are allowed here so the runtime can
    // accept explicit in-lane paths such as a full Skills helper path; we
    // still require a safe basename and the runner enforces lane containment.
    std::string basename = name;
    size_t slash = basename.find_last_of("/\\");
    if (slash != std::string::npos) basename = basename.substr(slash + 1);
    std::string safe = path_safety::SanitizeFilename(basename, "");
    if (safe.empty() || safe != basename) {
        errorOut = pathShaped
            ? "Unsafe Python script filename in path: " + basename
            : "Unsafe Python script filename: " + name;
        return false;
    }

    filenameOut = name;
    return true;
}

std::string UniqueScriptPath(const std::string& dir,
                             const std::string& filename,
                             std::string&       finalNameOut)
{
    size_t dot = filename.find_last_of('.');
    std::string stem = (dot == std::string::npos) ? filename : filename.substr(0, dot);
    std::string ext  = (dot == std::string::npos) ? std::string() : filename.substr(dot);

    if (stem.empty()) stem = "script";

    for (int i = 0; i < 1000; ++i) {
        std::string candidateName = (i == 0)
            ? filename
            : (stem + "_" + std::to_string(i + 1) + ext);
        std::string candidatePath = JoinToolPath(dir, candidateName);
        if (!IsFile(candidatePath) && !IsDirectory(candidatePath)) {
            finalNameOut = candidateName;
            return candidatePath;
        }
    }

    finalNameOut.clear();
    return std::string();
}

PythonCreateScriptResult CreatePythonScriptArtifact(const std::string& argsBlob,
                                                const std::string& cwd,
                                                const std::string& activeProjectRoot)
{
    PythonCreateScriptResult r;
    auto t0 = std::chrono::steady_clock::now();

    constexpr size_t kMaxScriptBytes = 512 * 1024; // 512 KiB

    std::string requestedName, content;
    SplitPythonCreateScriptArgs(argsBlob, requestedName, content);

    std::string filename, err;
    if (!NormalizePythonScriptFilename(requestedName, filename, err)) {
        r.chips = { "blocked", ElapsedChipLocal(t0) };
        r.errorBody = err;
        return r;
    }

    if (content.empty()) {
        r.chips = { "failed", ElapsedChipLocal(t0) };
        r.errorBody = "python_create_script requires script content after the filename line. It creates a reviewable .py file but does not run it.";
        return r;
    }

    if (content.size() > kMaxScriptBytes) {
        r.chips = { "too large", ElapsedChipLocal(t0) };
        r.errorBody = "Python script content exceeds cap: " +
                      HumanBytesLocal(content.size()) +
                      " (max " + HumanBytesLocal(kMaxScriptBytes) + ").";
        return r;
    }

    // Project chats create reusable workflow scripts directly in the
    // active project's Workflows folder.  Non-project chats keep the
    // previous conversation/global Scripts lane.
    const bool projectScoped = !activeProjectRoot.empty();
    std::string scriptsDir = projectScoped
        ? ProjectManager::ProjectWorkflowsPath(activeProjectRoot)
        : LlamaBossScriptsDir(cwd);
    if (scriptsDir.empty()) {
        r.chips = { "failed", ElapsedChipLocal(t0) };
        r.errorBody = projectScoped
            ? "Could not resolve the active project Workflows folder."
            : "Could not resolve the conversation Scripts folder.";
        return r;
    }

    wxFileName::Mkdir(wxString::FromUTF8(scriptsDir.c_str()),
                      wxS_DIR_DEFAULT,
                      wxPATH_MKDIR_FULL);

    std::string finalName;
    std::string outPath = UniqueScriptPath(scriptsDir, filename, finalName);
    if (outPath.empty()) {
        r.chips = { "failed", ElapsedChipLocal(t0) };
        r.errorBody = "Could not find a collision-safe filename in: " + scriptsDir;
        return r;
    }

    std::wstring wOut = path_safety::Utf8ToWide(outPath);
    if (wOut.empty()) {
        r.chips = { "failed", ElapsedChipLocal(t0) };
        r.errorBody = "Path conversion failed: " + outPath;
        return r;
    }

    HANDLE hFile = ::CreateFileW(
        wOut.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (hFile == INVALID_HANDLE_VALUE) {
        DWORD winErr = ::GetLastError();
        r.chips = { "failed", ElapsedChipLocal(t0) };
        r.errorBody = "Could not create Python script: " + outPath +
                      " (Win32 error " + std::to_string(winErr) + ")";
        return r;
    }

    const char* data = content.data();
    size_t remain = content.size();
    while (remain > 0) {
        DWORD chunk = (remain > 0x40000000U)
            ? 0x40000000U
            : static_cast<DWORD>(remain);
        DWORD written = 0;
        BOOL ok = ::WriteFile(hFile, data, chunk, &written, nullptr);
        if (!ok || written == 0) {
            DWORD winErr = ::GetLastError();
            ::CloseHandle(hFile);
            ::DeleteFileW(wOut.c_str());
            r.chips = { "failed", ElapsedChipLocal(t0) };
            r.errorBody = "Failed while writing Python script: " + outPath +
                          " (Win32 error " + std::to_string(winErr) + ")";
            return r;
        }
        data += written;
        remain -= written;
    }

    if (!::CloseHandle(hFile)) {
        DWORD winErr = ::GetLastError();
        r.chips = { "failed", ElapsedChipLocal(t0) };
        r.errorBody = "Failed while closing Python script after write: " +
                      outPath + " (Win32 error " + std::to_string(winErr) + ")";
        return r;
    }

    tool_python_syntax::SyntaxCheckResult syntax =
        tool_python_syntax::CheckFile(outPath);
    if (!syntax.ok) {
        ::DeleteFileW(wOut.c_str());
        r.chips = { "failed", "syntax error", ElapsedChipLocal(t0) };
        r.errorBody = "Python syntax check failed; the script was not created.\n\n" + syntax.message;
        return r;
    }

    r.createdPath = outPath;
    r.displayName = finalName;
    r.sizeBytes   = content.size();
    r.lineCount   = static_cast<int>(CountTextLines(content));

    r.chips.push_back("created");
    r.chips.push_back(HumanBytesLocal(r.sizeBytes));
    r.chips.push_back(std::to_string(r.lineCount) + " lines");
    r.chips.push_back(ElapsedChipLocal(t0));

    r.body = std::string(projectScoped
                 ? "Created Python project workflow script at "
                 : "Created Python script artifact at ") + outPath +
             "\n\nFinal filename: " + finalName +
             "\n\nNEXT STEP FOR THE ASSISTANT: If the user asked for a finished file, report, document, spreadsheet, chart, conversion, or transformed output, immediately call python_run_script with this exact final filename next. Do not ask the user for another approval; the approval for python_create_script carries forward to one immediate run of this exact script.\n";
    return r;
}

DispatchOutcome DoPythonCreateScript(const ToolInvocation& inv,
                                      const ToolContext&    ctx,
                                      const DispatchDeps&   /*deps*/)
{
    DispatchOutcome out = PreFill(inv, tool_names::kPythonCreateScript, "Python Script");
    out.result.iconUtf8    = "\xF0\x9F\x90\x8D";   // 🐍
    out.result.commandEcho = FirstLineEcho("python_create_script", inv.args);

    PythonCreateScriptResult r = CreatePythonScriptArtifact(inv.args, ctx.cwd, ctx.activeProjectRoot);
    out.result.chips     = r.chips;
    out.result.body      = r.body;
    out.result.errorBody = r.errorBody;
    out.result.bodyLang.clear();

    if (!r.createdPath.empty()) {
        PresentedFile file;
        file.displayName = r.displayName.empty() ? std::string("script.py")
                                                 : r.displayName;
        file.language    = "python";
        file.diskPath    = r.createdPath;
        file.sizeBytes   = r.sizeBytes;
        file.lineCount   = r.lineCount;
        out.result.presentedFiles.push_back(std::move(file));
    }

    return out;
}


DispatchOutcome DoPythonRunScript(const ToolInvocation& inv,
                                  const ToolContext&    ctx,
                                  const DispatchDeps&   deps)
{
    DispatchOutcome out = PreFill(inv, tool_names::kPythonRunScript, "Python Run");
    out.result.iconUtf8    = "\xF0\x9F\x90\x8D";   // 🐍
    out.result.commandEcho = MakeCommandEcho("python_run_script", inv.args);
    out.result.bodyLang.clear();

    if (!deps.pythonRunner) {
        out.status           = DispatchStatus::Invalid;
        out.result.errorBody = "Python runner not available in this context.";
        out.result.chips     = { "error" };
        return out;
    }

    if (!deps.pythonRunner->StartPythonRunScript(inv.args, ctx.cwd, ctx.timeoutMs ? ctx.timeoutMs : 30000, ctx.activeProjectRoot)) {
        out.status           = DispatchStatus::Invalid;
        out.result.errorBody = "Could not start Python script runner (another Python helper is already running?).";
        out.result.chips     = { "error" };
        return out;
    }

    out.status = DispatchStatus::Async;
    return out;
}


DispatchOutcome DoPythonInstallPackage(const ToolInvocation& inv,
                                       const ToolContext&    ctx,
                                       const DispatchDeps&   deps)
{
    std::string packageName;
    std::string err;
    if (!python_arg_policy::NormalizeAllowedPythonPackage(inv.args, packageName, err)) {
        DispatchOutcome out = PreFill(inv, tool_names::kPythonInstallPackage, "Install Python Package");
        out.status             = DispatchStatus::Invalid;
        out.result.iconUtf8    = "\xF0\x9F\x90\x8D";   // 🐍
        out.result.commandEcho = MakeCommandEcho("python_install_package", inv.args);
        out.result.errorBody   = err;
        out.result.chips       = { "error" };
        return out;
    }

    DispatchOutcome out = PreFill(inv, tool_names::kPythonInstallPackage, "Install Python Package");
    out.result.iconUtf8    = "\xF0\x9F\x90\x8D";   // 🐍
    out.result.commandEcho = MakeCommandEcho("python_install_package", packageName);
    out.result.bodyLang.clear();

    if (!deps.pythonRunner) {
        out.status           = DispatchStatus::Invalid;
        out.result.errorBody = "Python runner not available in this context.";
        out.result.chips     = { "error" };
        return out;
    }

    if (!deps.pythonRunner->StartPythonInstallPackage(packageName, ctx.cwd, ctx.timeoutMs ? ctx.timeoutMs : 300000)) {
        out.status           = DispatchStatus::Invalid;
        out.result.errorBody = "Could not start Python package installer (another Python helper is already running?).";
        out.result.chips     = { "error" };
        return out;
    }

    out.status = DispatchStatus::Async;
    return out;
}

DispatchOutcome DoOpen(const ToolInvocation& inv,
                       const ToolContext&    ctx,
                       const DispatchDeps&   /*deps*/)
{
    DispatchOutcome out = PreFill(inv, tool_names::kOpen, "Open");
    out.result.iconUtf8    = "\xE2\x96\xB6";   // ▶
    const std::string toolArgs = ResolveProjectFileArgForSinglePathTool(inv.args, ctx);
    out.result.commandEcho = MakeCommandEcho(inv.name, toolArgs);

    std::vector<LsListing> recent;
    if (ctx.history) {
        recent = CollectRecentLsListings(*ctx.history, ctx.cwd, 3);
    }

    OpenResult r = OpenFile(toolArgs, ctx, recent);
    out.result.chips     = r.chips;
    out.result.body      = r.body;
    out.result.errorBody = r.errorBody;
    out.result.bodyLang  = r.bodyLang;
    return out;
}

DispatchOutcome DoWrite(const ToolInvocation& inv,
                        const ToolContext&    ctx,
                        const DispatchDeps&   /*deps*/)
{
    DispatchOutcome out = PreFill(inv, tool_names::kWrite, "Write");
    out.result.iconUtf8    = "\xF0\x9F\x93\x9D";   // 📝
    out.result.commandEcho = FirstLineEcho("write", inv.args);

    WriteResult r = WriteNewFile(inv.args, ctx);
    out.result.chips     = r.chips;
    out.result.body      = r.body;
    out.result.errorBody = r.errorBody;
    out.result.bodyLang  = r.bodyLang;

    if (!r.createdPath.empty()) {
        PresentedFile file;
        file.displayName = r.displayName.empty() ? std::string("file")
                                                 : r.displayName;
        file.diskPath    = r.createdPath;
        file.sizeBytes   = r.sizeBytes;
        file.lineCount   = r.lineCount;
        out.result.presentedFiles.push_back(std::move(file));
    }

    return out;
}


DispatchOutcome DoOverwriteFile(const ToolInvocation& inv,
                                const ToolContext&    ctx,
                                const DispatchDeps&   /*deps*/)
{
    DispatchOutcome out = PreFill(inv, tool_names::kOverwriteFile, "Overwrite File");
    out.result.iconUtf8    = "\xE2\x99\xBB";   // ♻
    out.result.commandEcho = FirstLineEcho("overwrite_file", inv.args);

    WriteResult r = OverwriteFileContent(inv.args, ctx);
    out.result.chips     = r.chips;
    out.result.body      = r.body;
    out.result.errorBody = r.errorBody;
    out.result.bodyLang  = r.bodyLang;

    if (!r.createdPath.empty()) {
        PresentedFile file;
        file.displayName = r.displayName.empty() ? std::string("file")
                                                 : r.displayName;
        file.diskPath    = r.createdPath;
        file.sizeBytes   = r.sizeBytes;
        file.lineCount   = r.lineCount;
        out.result.presentedFiles.push_back(std::move(file));
    }

    return out;
}

DispatchOutcome DoMkdir(const ToolInvocation& inv,
                        const ToolContext&    ctx,
                        const DispatchDeps&   /*deps*/)
{
    DispatchOutcome out = PreFill(inv, tool_names::kMkdir, "Mkdir");
    out.result.iconUtf8    = "\xE2\x9E\x95";   // ➕
    out.result.commandEcho = MakeCommandEcho(inv.name, inv.args);

    MkdirResult r = MakeDirectory(inv.args, ctx);
    out.result.chips     = r.chips;
    out.result.body      = r.body;
    out.result.errorBody = r.errorBody;
    out.result.bodyLang  = r.bodyLang;
    return out;
}

DispatchOutcome DoEdit(const ToolInvocation& inv,
                       const ToolContext&    ctx,
                       const DispatchDeps&   /*deps*/)
{
    DispatchOutcome out = PreFill(inv, tool_names::kEdit, "Edit");
    out.result.iconUtf8    = "\xE2\x9C\x8F";   // ✏
    out.result.commandEcho = FirstLineEcho("edit", inv.args);

    EditResult r = EditFile(inv.args, ctx);
    out.result.chips     = r.chips;
    out.result.body      = r.body;
    out.result.errorBody = r.errorBody;
    out.result.bodyLang  = r.bodyLang;
    return out;
}

DispatchOutcome DoDelete(const ToolInvocation& inv,
                         const ToolContext&    ctx,
                         const DispatchDeps&   /*deps*/)
{
    DispatchOutcome out = PreFill(inv, tool_names::kDelete, "Delete");
    out.result.iconUtf8    = "\xF0\x9F\x97\x91";   // 🗑
    out.result.commandEcho = MakeCommandEcho(inv.name, inv.args);

    DeleteResult r = DeleteEntry(inv.args, ctx);
    out.result.chips     = r.chips;
    out.result.body      = r.body;
    out.result.errorBody = r.errorBody;
    out.result.bodyLang  = r.bodyLang;
    return out;
}

DispatchOutcome DoNotesRead(const ToolInvocation& inv,
                            const ToolContext&    ctx,
                            const DispatchDeps&   /*deps*/)
{
    DispatchOutcome out = PreFill(inv, tool_names::kNotesRead, "Notes");
    out.result.iconUtf8    = "\xF0\x9F\x93\x92";   // 📒
    out.result.commandEcho = "notes_read";

    NotesResult r = NotesRead(ctx);
    out.result.chips     = r.chips;
    out.result.body      = r.body;
    out.result.errorBody = r.errorBody;
    out.result.bodyLang  = r.bodyLang;
    return out;
}

DispatchOutcome DoNotesAppend(const ToolInvocation& inv,
                              const ToolContext&    ctx,
                              const DispatchDeps&   /*deps*/)
{
    DispatchOutcome out = PreFill(inv, tool_names::kNotesAppend, "Notes");
    out.result.iconUtf8    = "\xF0\x9F\x93\x92";   // 📒
    // Echo only the first line of the entry to keep the chip header
    // tidy when the model appends multi-line notes.  The full body
    // still appears in the result body below.
    out.result.commandEcho = FirstLineEcho("notes_append", inv.args);

    NotesResult r = NotesAppend(inv.args, ctx);
    out.result.chips     = r.chips;
    out.result.body      = r.body;
    out.result.errorBody = r.errorBody;
    out.result.bodyLang  = r.bodyLang;
    return out;
}


DispatchOutcome DoProjectNotesRead(const ToolInvocation& inv,
                                   const ToolContext&    ctx,
                                   const DispatchDeps&   /*deps*/)
{
    DispatchOutcome out = PreFill(inv, tool_names::kProjectNotesRead, "Project Notes");
    out.result.iconUtf8    = "\xF0\x9F\x93\x93";   // Project notes icon
    out.result.commandEcho = "project_notes_read";

    NotesResult r = ProjectNotesRead(ctx);
    out.result.chips     = r.chips;
    out.result.body      = r.body;
    out.result.errorBody = r.errorBody;
    out.result.bodyLang  = r.bodyLang;
    return out;
}

DispatchOutcome DoProjectNotesAppend(const ToolInvocation& inv,
                                     const ToolContext&    ctx,
                                     const DispatchDeps&   /*deps*/)
{
    DispatchOutcome out = PreFill(inv, tool_names::kProjectNotesAppend, "Project Notes");
    out.result.iconUtf8    = "\xF0\x9F\x93\x93";   // Project notes icon
    out.result.commandEcho = FirstLineEcho("project_notes_append", inv.args);

    NotesResult r = ProjectNotesAppend(inv.args, ctx);
    out.result.chips     = r.chips;
    out.result.body      = r.body;
    out.result.errorBody = r.errorBody;
    out.result.bodyLang  = r.bodyLang;
    return out;
}

// ─── Per-tool validators ────────────────────────────────────────
// Mirror the old ValidateToolArgs branches.  Shape-only; the heavy
// validation lives inside the tool implementations.
//
// Phrasing note: the messages refer to "args" rather than the XML
// "<args>" form so they read sensibly under both protocols.  Native
// tool calls present `args` as a JSON string field; XML calls
// present it as the body between <args>...</args> tags.  Either
// interpretation makes the same error actionable.

bool ValRequireNonEmpty(const std::string& args,
                        std::string&       reasonOut,
                        const char*        msg)
{
    if (Trim(args).empty()) {
        reasonOut = msg;
        return false;
    }
    return true;
}

bool ValRead(const std::string& a, std::string& r) {
    return ValRequireNonEmpty(a, r,
        "read requires a non-empty file path in args");
}
bool ValReadHead(const std::string& a, std::string& r) {
    std::string path;
    size_t lines = 40;
    ParseReadHeadArgs(a, path, lines);
    if (Trim(path).empty()) {
        r = "read_head requires a file path, optionally preceded by a line count";
        return false;
    }
    return true;
}
bool ValLs(const std::string&, std::string&) {
    return true;   // empty args = list cwd
}
bool ValGrep(const std::string& a, std::string& r) {
    return ValRequireNonEmpty(a, r,
        "grep requires args in the form '<pattern> [path]'");
}
bool ValPwd(const std::string&, std::string&) {
    return true;   // args ignored
}
bool ValPowerShell(const std::string& a, std::string& r) {
    return ValRequireNonEmpty(a, r,
        "powershell requires a command in args");
}
bool ValPythonHealth(const std::string& a, std::string& r) {
    if (!Trim(a).empty()) {
        r = "python_health does not accept args; it runs the fixed built-in helper only";
        return false;
    }
    return true;
}
bool ValCsvInspect(const std::string& a, std::string& r) {
    if (!ValRequireNonEmpty(a, r,
            "csv_inspect requires a .csv or .tsv file path in args"))
        return false;
    if (a.find('\n') != std::string::npos || a.find('\r') != std::string::npos) {
        r = "csv_inspect accepts one file path only; no multi-line args";
        return false;
    }
    return true;
}
bool ValCsvReport(const std::string& a, std::string& r) {
    if (!ValRequireNonEmpty(a, r,
            "csv_report requires a .csv or .tsv file path in args"))
        return false;
    if (a.find('\n') != std::string::npos || a.find('\r') != std::string::npos) {
        r = "csv_report accepts one file path only; no multi-line args";
        return false;
    }
    return true;
}
bool ValCsvToXlsx(const std::string& a, std::string& r) {
    if (!ValRequireNonEmpty(a, r,
            "csv_to_xlsx requires a .csv or .tsv file path in args"))
        return false;
    if (a.find('\n') != std::string::npos || a.find('\r') != std::string::npos) {
        r = "csv_to_xlsx accepts one file path only; no multi-line args";
        return false;
    }
    return true;
}
bool ValXlsxInspect(const std::string& a, std::string& r) {
    if (!ValRequireNonEmpty(a, r,
            "xlsx_inspect requires an .xlsx file path in args"))
        return false;
    if (a.find('\n') != std::string::npos || a.find('\r') != std::string::npos) {
        r = "xlsx_inspect accepts one file path only; no multi-line args";
        return false;
    }
    return true;
}
bool ValXlsxReport(const std::string& a, std::string& r) {
    if (!ValRequireNonEmpty(a, r,
            "xlsx_report requires an .xlsx file path in args"))
        return false;
    if (a.find('\n') != std::string::npos || a.find('\r') != std::string::npos) {
        r = "xlsx_report accepts one file path only; no multi-line args";
        return false;
    }
    return true;
}

bool ValXlsxCreateWorkbook(const std::string& a, std::string& r) {
    if (!ValRequireNonEmpty(a, r,
            "xlsx_create_workbook requires a JSON workbook spec in args"))
        return false;
    std::string trimmed = Trim(a);
    if (trimmed.empty() || trimmed.front() != '{') {
        r = "xlsx_create_workbook args must be a JSON object workbook spec";
        return false;
    }
    if (trimmed.size() > 256 * 1024) {
        r = "xlsx_create_workbook JSON spec is too large for the first-phase helper";
        return false;
    }
    return true;
}
bool ValPdfExtractText(const std::string& a, std::string& r) {
    if (!ValRequireNonEmpty(a, r,
            "pdf_extract_text requires a .pdf file path in args"))
        return false;
    if (a.find('\n') != std::string::npos || a.find('\r') != std::string::npos) {
        r = "pdf_extract_text accepts one file path only; no multi-line args";
        return false;
    }
    return true;
}
bool ValPdfInspectForm(const std::string& a, std::string& r) {
    if (!ValRequireNonEmpty(a, r,
            "pdf_inspect_form requires a .pdf file path in args"))
        return false;
    if (a.find('\n') != std::string::npos || a.find('\r') != std::string::npos) {
        r = "pdf_inspect_form accepts one file path only; no multi-line args";
        return false;
    }
    return true;
}
bool ValDocxExtractText(const std::string& a, std::string& r) {
    if (!ValRequireNonEmpty(a, r,
            "docx_extract_text requires a .docx file path in args"))
        return false;
    if (a.find('\n') != std::string::npos || a.find('\r') != std::string::npos) {
        r = "docx_extract_text accepts one file path only; no multi-line args";
        return false;
    }
    return true;
}
bool ValDocxInspect(const std::string& a, std::string& r) {
    if (!ValRequireNonEmpty(a, r,
            "docx_inspect requires a .docx file path in args"))
        return false;
    if (a.find('\n') != std::string::npos || a.find('\r') != std::string::npos) {
        r = "docx_inspect accepts one file path only; no multi-line args";
        return false;
    }
    return true;
}
bool ValPdfFillForm(const std::string& a, std::string& r) {
    if (!ValRequireNonEmpty(a, r,
            "pdf_fill_form requires a .pdf file path on the first line of args and a JSON {field: value} object on the remaining lines"))
        return false;
    size_t nl = a.find('\n');
    if (nl == std::string::npos) {
        r = "pdf_fill_form args must be: path/to/form.pdf on the first line, then a JSON {field: value, ...} object on the remaining lines";
        return false;
    }
    std::string first = Trim(a.substr(0, nl));
    if (first.empty()) {
        r = "pdf_fill_form requires a .pdf file path on the first line";
        return false;
    }
    std::string rest = a.substr(nl + 1);
    if (Trim(rest).empty()) {
        r = "pdf_fill_form requires a JSON {field: value, ...} object after the path line";
        return false;
    }
    return true;
}
bool ValPythonCreateScript(const std::string& a, std::string& r) {
    if (!ValRequireNonEmpty(a, r,
            "python_create_script requires a filename on the first line of args and script content on subsequent lines"))
        return false;
    size_t nl = a.find('\n');
    if (nl == std::string::npos) {
        r = "python_create_script args must be: filename.py on the first line, then Python code on subsequent lines";
        return false;
    }
    std::string first = Trim(a.substr(0, nl));
    if (first.empty()) {
        r = "python_create_script requires a filename on the first line";
        return false;
    }
    return true;
}


bool ValPythonRunScript(const std::string& a, std::string& r) {
    if (!ValRequireNonEmpty(a, r,
            "python_run_script requires a .py filename or an in-lane .py path from the conversation Scripts folder, the active project Workflows folder, or the LlamaBoss Skills folder"))
        return false;

    std::string scriptRequest;
    std::vector<std::string> argv;
    SplitPythonRunScriptInvocationForValidation(a, scriptRequest, argv);

    std::string normalized, err;
    if (!NormalizePythonRunScriptFilename(scriptRequest, normalized, err)) {
        r = err;
        return false;
    }
    // Shape-only validation.  The actual existence check is done at
    // dispatch time because the correct Scripts folder is now tied to
    // the conversation working directory. Optional command-line arguments
    // are passed through to the script by the runner.
    return true;
}

bool ValPythonInstallPackage(const std::string& a, std::string& r) {
    if (!ValRequireNonEmpty(a, r,
            "python_install_package requires one Python package name"))
        return false;
    if (a.find('\n') != std::string::npos || a.find('\r') != std::string::npos) {
        r = "python_install_package accepts one package name only; no multi-line args";
        return false;
    }
    std::string packageName;
    std::string err;
    if (!python_arg_policy::NormalizeAllowedPythonPackage(a, packageName, err)) {
        r = err;
        return false;
    }
    return true;
}

bool ValOpen(const std::string& a, std::string& r) {
    return ValRequireNonEmpty(a, r,
        "open requires a path or filename in args");
}
bool ValWrite(const std::string& a, std::string& r) {
    return ValRequireNonEmpty(a, r,
        "write requires a path on the first line of args, with "
        "optional file content on subsequent lines");
}
bool ValOverwriteFile(const std::string& a, std::string& r) {
    return ValRequireNonEmpty(a, r,
        "overwrite_file requires a path on the first line of args, with file content on subsequent lines");
}
bool ValMkdir(const std::string& a, std::string& r) {
    return ValRequireNonEmpty(a, r,
        "mkdir requires a directory path in args");
}
bool ValEdit(const std::string& a, std::string& r) {
    if (!ValRequireNonEmpty(a, r,
            "edit requires a path on the first line of args, then "
            "<<<OLD>>> and <<<NEW>>> blocks"))
        return false;
    if (a.find("<<<OLD>>>") == std::string::npos) {
        r = "edit args must include a <<<OLD>>> sentinel on its own line";
        return false;
    }
    if (a.find("<<<NEW>>>") == std::string::npos) {
        r = "edit args must include a <<<NEW>>> sentinel on its own line";
        return false;
    }
    return true;
}
bool ValDelete(const std::string& a, std::string& r) {
    return ValRequireNonEmpty(a, r,
        "delete requires a path in args");
}

bool ValNotesRead(const std::string& a, std::string& r) {
    if (!Trim(a).empty()) {
        r = "notes_read does not accept args; it returns the full NOTES.md contents";
        return false;
    }
    return true;
}

bool ValNotesAppend(const std::string& a, std::string& r) {
    return ValRequireNonEmpty(a, r,
        "notes_append requires the entry text in args");
}


bool ValProjectNotesRead(const std::string& a, std::string& r) {
    if (!Trim(a).empty()) {
        r = "project_notes_read does not accept args; it returns the active project's Notes/NOTES.md contents";
        return false;
    }
    return true;
}

bool ValProjectNotesAppend(const std::string& a, std::string& r) {
    return ValRequireNonEmpty(a, r,
        "project_notes_append requires the entry text in args");
}

bool ValWebFetchUrl(const std::string& a, std::string& r) {
    if (!ValRequireNonEmpty(a, r,
            "web_fetch_url requires one http:// or https:// URL"))
        return false;
    if (a.find('\n') != std::string::npos || a.find('\r') != std::string::npos) {
        r = "web_fetch_url accepts one URL only; no multi-line args";
        return false;
    }
    std::string u = Lower(Trim(a));
    if (!(StartsWithTool(u, "http://") || StartsWithTool(u, "https://"))) {
        r = "web_fetch_url only supports http:// and https:// URLs";
        return false;
    }
    return true;
}


DispatchOutcome DoWebFetchUrl(const ToolInvocation& inv,
                              const ToolContext&    ctx,
                              const DispatchDeps&   /*deps*/)
{
    DispatchOutcome out = PreFill(inv, tool_names::kWebFetchUrl, "Web Page Inspect");
    out.result.iconUtf8    = "\xF0\x9F\x8C\x90";   // 🌐
    out.result.commandEcho = MakeCommandEcho("web_fetch_url", inv.args);
    out.result.bodyLang    = "markdown";

    WebFetchResult r = FetchWebPageUrl(inv.args, ctx);
    out.result.chips     = r.chips;
    out.result.body      = r.body;
    out.result.errorBody = r.errorBody;
    out.result.bodyLang  = r.bodyLang;

    if (!r.textPath.empty()) {
        PresentedFile textFile;
        textFile.displayName = r.textDisplayName.empty() ? std::string("webpage_text.md")
                                                         : r.textDisplayName;
        textFile.language    = "markdown";
        textFile.diskPath    = r.textPath;
        textFile.sizeBytes   = r.textBytes;
        textFile.lineCount   = r.textLineCount;
        out.result.presentedFiles.push_back(std::move(textFile));
    }

    if (!r.rawHtmlPath.empty()) {
        PresentedFile htmlFile;
        htmlFile.displayName = r.rawHtmlDisplayName.empty() ? std::string("webpage_raw.html")
                                                            : r.rawHtmlDisplayName;
        htmlFile.language    = "html";
        htmlFile.diskPath    = r.rawHtmlPath;
        htmlFile.sizeBytes   = r.htmlBytes;
        htmlFile.lineCount   = 0;
        out.result.presentedFiles.push_back(std::move(htmlFile));
    }

    return out;
}

// ─── Schemas ────────────────────────────────────────────────────
// Hand-written JSON Schema strings.  These are the contract between
// LlamaBoss and the model under Phase 3 native function calling: the
// model sees these via /v1/chat/completions and emits structured
// tool_calls keyed to them.  Phase 2 stores them on the spec but
// doesn't consume them.
//
// Style notes:
//   - Each schema is a top-level JSON object (the `parameters` field
//     of an OpenAI-style function definition).
//   - Single freeform string args use one "args" property — matches
//     the current XML protocol where the entire <args> blob is one
//     string regardless of internal structure.
//   - Future Phase 3 work may split write/edit into typed properties
//     ({path, content} / {path, old, new}), but for the P2 → P3
//     migration the args-string shape lets the existing tool
//     implementations keep their parsers unchanged.

constexpr const char* kSchemaRead = R"({
"type":"object",
"properties":{"args":{"type":"string","description":"File path to read for read-only inspection. Relative paths resolve against the conversation working directory; absolute local paths outside it are allowed when readable."}},
"required":["args"]
})";

constexpr const char* kSchemaReadHead = R"({
"type":"object",
"properties":{
"path":{"type":"string","description":"File path to preview. Relative project paths such as PROJECT.md, requirements.txt, Inputs\file.txt, Workflows\script.py, and Outputs\... resolve to the active project root when attached."},
"lines":{"type":"integer","description":"Number of leading lines to read. Defaults to 40. Maximum 500."}
},
"required":["path"]
})";

constexpr const char* kSchemaLs = R"({
"type":"object",
"properties":{"args":{"type":"string","description":"Directory path to list for read-only inspection. Empty for the current working directory; absolute local paths such as D:\\ or %USERPROFILE%\\Desktop are allowed when readable."}}
})";

constexpr const char* kSchemaPwd = R"({
"type":"object",
"properties":{}
})";

constexpr const char* kSchemaGrep = R"({
"type":"object",
"properties":{"args":{"type":"string","description":"<pattern> [path]. First whitespace-separated token is the pattern; rest is the path. Empty path searches the working directory recursively; absolute local paths are allowed for read-only search."}},
"required":["args"]
})";

constexpr const char* kSchemaPowerShell = R"({
"type":"object",
"properties":{"args":{"type":"string","description":"PowerShell command line. Clearly read-only inspection commands may run immediately; broader valid PowerShell commands pause for approval before execution."}},
"required":["args"]
})";

constexpr const char* kSchemaPythonHealth = R"({
"type":"object",
"properties":{}
})";

constexpr const char* kSchemaCsvInspect = R"({
"type":"object",
"properties":{"args":{"type":"string","description":"Path to a local .csv or .tsv file; absolute paths outside the working directory are allowed for reading. The fixed Python helper returns JSON with encoding, delimiter, columns, row count, and sample rows."}},
"required":["args"]
})";

constexpr const char* kSchemaCsvReport = R"({
"type":"object",
"properties":{"args":{"type":"string","description":"Path to a local .csv or .tsv file; absolute paths outside the working directory are allowed for reading. The fixed Python helper creates a Markdown report in the conversation Documents folder and returns JSON plus an artifact card."}},
"required":["args"]
})";

constexpr const char* kSchemaCsvToXlsx = R"({
"type":"object",
"properties":{"args":{"type":"string","description":"Path to a local .csv or .tsv file; absolute paths outside the working directory are allowed for reading. The fixed Python helper converts it into an .xlsx workbook in the conversation Spreadsheets folder and returns JSON plus an artifact card. Requires the openpyxl package on the system Python."}},
"required":["args"]
})";

constexpr const char* kSchemaXlsxInspect = R"({
"type":"object",
"properties":{"args":{"type":"string","description":"Path to a local .xlsx file; absolute paths outside the working directory are allowed for reading. The fixed Python helper returns JSON describing every sheet (name, dimensions, columns, sample rows). Read-only. Requires the openpyxl package on the system Python."}},
"required":["args"]
})";

constexpr const char* kSchemaXlsxReport = R"({
"type":"object",
"properties":{"args":{"type":"string","description":"Path to a local .xlsx file; absolute paths outside the working directory are allowed for reading. The fixed Python helper creates a Markdown report covering every sheet in the conversation Documents folder and returns JSON plus an artifact card. Requires the openpyxl package on the system Python."}},
"required":["args"]
})";


constexpr const char* kSchemaXlsxCreateWorkbook = R"({
"type":"object",
"properties":{"args":{"type":"string","description":"JSON workbook spec for the fixed xlsx_create_workbook helper. Writes one .xlsx workbook to the conversation Spreadsheets folder. Use this instead of python_create_script when creating a new formatted Excel workbook from structured data. Required JSON shape (put \"rows\" LAST in each sheet object so you can close the sheet with } immediately after the row data): {\"filename\":\"report.xlsx\",\"allow_formulas\":false,\"sheets\":[{\"name\":\"Sheet\",\"title\":\"Optional title\",\"headers\":[\"A\",\"B\"],\"currency_columns\":[\"Amount\"],\"number_columns\":[\"Hours\"],\"percent_columns\":[\"Completion %\"],\"integer_columns\":[\"Count\"],\"date_columns\":[\"Review Date\"],\"freeze_top_row\":true,\"auto_filter\":true,\"table\":true,\"rows\":[[1,2]]}]}. Percent columns expect the decimal form (0.92 = 92%); whole-number values greater than 1 are auto-divided by 100 with a logged adjustment. Date columns accept yyyy-mm-dd, m/d/yyyy, or m/d/yy and are parsed to true Excel dates. Per-cell formatting for mixed Metric/Value summary sheets can use \"cell_formats\":{\"B5\":\"currency\",\"B7\":\"percent\"} with named formats currency / number / integer / percent / date. Formula safety: formulas are treated as text unless the user explicitly asks to allow formulas. If the user says allow formulas / enable formulas / formulas are allowed, include top-level \"allow_formulas\":true and pass formula cells as normal strings like \"=A2+B2\" with no leading apostrophe."}},
"required":["args"]
})";

constexpr const char* kSchemaPdfExtractText = R"({
"type":"object",
"properties":{"args":{"type":"string","description":"Path to a local text-based .pdf file; absolute paths outside the working directory are allowed for reading. The fixed Python helper extracts selectable text, writes a Markdown artifact to the conversation PDFs folder, and returns JSON plus an artifact card. No OCR or PDF editing."}},
"required":["args"]
})";

constexpr const char* kSchemaPdfInspectForm = R"({
"type":"object",
"properties":{"args":{"type":"string","description":"Path to a local fillable AcroForm .pdf file; absolute paths outside the working directory are allowed for reading. The fixed Python helper reads form metadata and returns JSON listing every field (name, type, page, current_value, options for dropdowns/listboxes, on_states for checkboxes/radios, required flag, tooltip). Read-only: no files are created or modified. XFA-only PDFs are refused with an explanation. Call this before pdf_fill_form so you know the exact field names to use."}},
"required":["args"]
})";

constexpr const char* kSchemaPdfFillForm = R"({
"type":"object",
"properties":{"args":{"type":"string","description":"Multi-line argument. First line is the path to a local fillable AcroForm .pdf file; absolute paths outside the working directory are allowed for reading. Remaining lines are a JSON object mapping field names to values, e.g. {\"Employee Name\": \"Cesar Rodriguez\", \"Effective Date\": \"04/30/2026\", \"Currently\": true, \"Group6\": \"1\"}. Field names MUST match exactly what pdf_inspect_form reported -- call pdf_inspect_form first if you have not already; do not guess names. Value rules: text fields take strings (numbers and booleans are coerced to strings); checkboxes take true/false (recommended) or the literal on-state string from inspect; radio groups take the on-state string of the option to select, or \"Off\" to deselect; dropdowns/listboxes take one of the option strings. Validation is hard-fail: any unknown field name or invalid value rejects the entire call with no partial write. On success, writes one filled .pdf to the conversation Filled Forms folder and returns an artifact card."}},
"required":["args"]
})";

constexpr const char* kSchemaDocxExtractText = R"({
"type":"object",
"properties":{"args":{"type":"string","description":"Path to a local .docx (or .docm) file; absolute paths outside the working directory are allowed for reading. The fixed Python helper reads paragraphs and tables in document order, maps Word heading styles to Markdown headings, list paragraphs to bullet/numbered lists, and tables to Markdown tables. Writes one Markdown artifact to the conversation Word folder and returns JSON plus an artifact card. Requires the python-docx package on the system Python."}},
"required":["args"]
})";

constexpr const char* kSchemaDocxInspect = R"({
"type":"object",
"properties":{"args":{"type":"string","description":"Path to a local .docx (or .docm) file; absolute paths outside the working directory are allowed for reading. The fixed Python helper returns a JSON summary: paragraph_count, headings (level + text, capped at 100), tables (rows + cols, capped at 100), section_count, has_images, styles_in_use. Read-only: no files are created or modified. Requires the python-docx package on the system Python."}},
"required":["args"]
})";

constexpr const char* kSchemaPythonCreateScript = R"({
"type":"object",
"properties":{
"filename":{"type":"string","description":"Filename for the Python script. Filename only, no path. LlamaBoss saves it in the conversation Scripts folder. .py is required or appended."},
"content":{"type":"string","description":"Python script contents. The script is created as a reviewable artifact and is not run by this tool."}
},
"required":["filename","content"]
})";

constexpr const char* kSchemaPythonRunScript = R"({
"type":"object",
"properties":{"args":{"type":"string","description":"Filename of an existing .py script in the fixed conversation Scripts folder, an optional .py helper script in the active project's Workflows folder, or a Skill helper script in the LlamaBoss Skills folder. A full .py path inside one of those script lanes is also accepted. Preferred argument contract: put the script filename/path on the first line, then put each optional command-line argument on its own later line. A simple one-line 'script.py ARG' form is also accepted as one argument when unambiguous. Runs locally with stdout/stderr/exit code captured."}},
"required":["args"]
})";

constexpr const char* kSchemaPythonInstallPackage = R"({
"type":"object",
"properties":{"args":{"type":"string","description":"One Python package name to install from PyPI into the user's Python user-site with pip. Approval required: every install renders its own approval card with the exact package name, even when one-approval mode is on for the chat. Pass the canonical PyPI name (python-docx, openpyxl, pymupdf, pypdf, pypdfium2, pandas, pillow, reportlab, matplotlib, python-pptx, xlsxwriter, beautifulsoup4, lxml, requests, youtube-transcript-api, etc.) rather than the import name. Aliases docx, fitz, pil, pptx, and bs4 are normalized to python-docx/pymupdf/pillow/python-pptx/beautifulsoup4. One package per call; no versions, URLs, requirements files, extras, or pip flags."}},
"required":["args"]
})";

constexpr const char* kSchemaOpen = R"({
"type":"object",
"properties":{"args":{"type":"string","description":"Path or filename to open, play, run, show, or view. Filenames fuzzy-match against recent ls or recognized Get-ChildItem results. Partial paths like D:\\Music\\Hotel California can resolve by fuzzy-matching inside the parent folder. Absolute local paths are allowed; text/code returns inline; media launches in the user's default app; executables and scripts are blocked."}},
"required":["args"]
})";

// Phase 3c-iii: write moves to structured {path, content}.  The
// legacy single-string schema (path on the first line, content
// after the first newline) forced models to embed multi-line
// content as a JSON-escaped \n-laden string, which is exactly
// where small models fumble escaping.  Splitting into named
// fields removes that class of error entirely.  ProjectStructuredArgs
// flattens {path, content} back to the legacy "path\ncontent"
// string the WriteNewFile parser consumes, so tool internals
// stay unchanged.
constexpr const char* kSchemaWrite = R"({
"type":"object",
"properties":{
"path":{"type":"string","description":"File path to create. Refuses to overwrite existing files (use overwrite_file for full-file replacement, or edit for a small unique replacement). Restricted to the working directory, or the active project root when a project is attached."},
"content":{"type":"string","description":"File contents. Empty string creates an empty file. A trailing newline is added automatically if not present."}
},
"required":["path","content"]
})";

constexpr const char* kSchemaOverwriteFile = R"({
"type":"object",
"properties":{
"path":{"type":"string","description":"File path to create or replace. Restricted to the working directory, or the active project root when a project is attached. Use this for full-file replacement such as PROJECT.md, requirements.txt, input files, and generated project workflow scripts."},
"content":{"type":"string","description":"Full file contents to write. Existing regular files are atomically replaced. Directories are never overwritten. Python files are syntax-checked before commit when Python is available."}
},
"required":["path","content"]
})";

constexpr const char* kSchemaMkdir = R"({
"type":"object",
"properties":{"args":{"type":"string","description":"Directory path to create. Single level only; parent must exist. Restricted to the working directory, or the active project root when a project is attached."}},
"required":["args"]
})";

// Phase 3c-iii: edit moves to structured {path, old_str, new_str}.
// Same reasoning as write — embedding the <<<OLD>>>...<<<NEW>>>...
// sentinel form inside a JSON-escaped string is fragile.  The
// structured shape lets the model write the OLD and NEW blocks
// as plain JSON strings.  ProjectStructuredArgs reconstructs the
// sentinel form for ParseEditArgs.
//
// Field names match the str_replace convention (old_str / new_str)
// that's familiar from major editor-tool ecosystems; small models
// trained on those datasets pick this shape up immediately.
constexpr const char* kSchemaEdit = R"({
"type":"object",
"properties":{
"path":{"type":"string","description":"File path to edit. Must be inside the working directory, or the active project root when a project is attached."},
"old_str":{"type":"string","description":"Exact text to find. Must appear EXACTLY ONCE in the file. Include enough surrounding context to be unique."},
"new_str":{"type":"string","description":"Replacement text. Empty string deletes the matched OLD text without inserting anything."}
},
"required":["path","old_str","new_str"]
})";

constexpr const char* kSchemaDelete = R"({
"type":"object",
"properties":{"args":{"type":"string","description":"Path to delete. Files are removed; directories are removed only if empty. Restricted to the working directory, or the active project root when a project is attached; risky extensions blocked. Requires approval."}},
"required":["args"]
})";

constexpr const char* kSchemaNotesRead = R"({
"type":"object",
"properties":{}
})";

constexpr const char* kSchemaNotesAppend = R"({
"type":"object",
"properties":{"args":{"type":"string","description":"Entry text to append to the user's NOTES.md. One short fact, path, preference, or named workflow in the user's own voice. Multi-line entries are allowed; an automatic dated header is added. Examples: 'Music library: D:\\Music', 'Mac mini share: \\\\\\\\macmini\\\\notes', 'When I say my LISA workflow I mean: 1) launch playwright, 2) ...'"}},
"required":["args"]
})";

constexpr const char* kSchemaProjectNotesRead = R"({
"type":"object",
"properties":{}
})";

constexpr const char* kSchemaProjectNotesAppend = R"({
"type":"object",
"properties":{"args":{"type":"string","description":"Entry text to append to the active project's Notes/NOTES.md. Use for project-specific durable instructions, preferences, client/site facts, workflow decisions, and reusable project guidance. Requires an active project."}},
"required":["args"]
})";

constexpr const char* kSchemaWebFetchUrl = R"({
"type":"object",
"properties":{"args":{"type":"string","description":"One public http:// or https:// URL to fetch for read-only webpage inspection. Downloads the page with WinHTTP, does not execute JavaScript, blocks localhost/private-network/file/data/javascript URLs, saves raw HTML plus extracted Markdown text artifacts, and returns a preview for answering the user's question."}},
"required":["args"]
})";

// ─── Spec construction ──────────────────────────────────────────

std::vector<ToolSpec> BuildBuiltinSpecs()
{
    std::vector<ToolSpec> specs;
    specs.reserve(36);

    auto add = [&](const char* name,
                   const char* description,
                   const char* schema,
                   ToolSafetyProfile safety,
                   ToolSpec::ValidateFn validate,
                   ToolSpec::DispatchFn dispatch)
    {
        ToolSpec s;
        s.name                    = name;
        s.description             = description;
        s.parameters_json_schema  = schema;
        s.safety                  = std::move(safety);
        s.validate                = std::move(validate);
        s.dispatch                = std::move(dispatch);
        specs.push_back(std::move(s));
    };

    add(tool_names::kRead,
        "Read a text or code file into the conversation.",
        kSchemaRead,
        ReadOnlySafety("Read-only file inspection. Absolute local paths outside the cwd may be inspected when readable."),
        ValRead, DoRead);

    add(tool_names::kReadHead,
        "Preview the first N lines of a text/code file without reading the whole file. Prefer this before editing or inspecting large source files.",
        kSchemaReadHead,
        ReadOnlySafety("Read-only file preview/head inspection. Absolute local paths outside the cwd may be inspected when readable."),
        ValReadHead, DoReadHead);

    add(tool_names::kLs,
        "List the contents of a directory.",
        kSchemaLs,
        ReadOnlySafety("Read-only directory listing. Absolute local paths outside the cwd may be inspected when readable."),
        ValLs, DoLs);

    add(tool_names::kPwd,
        "Show the current conversation working directory.",
        kSchemaPwd,
        ReadOnlySafety("Read-only cwd inspection.", false),
        ValPwd, DoPwd);

    {
        ToolSafetyProfile grepSafety = ReadOnlySafety("Read-only literal text search. Absolute local paths outside the cwd may be searched when readable.");
        grepSafety.isAsync = true;
        add(tool_names::kGrep,
            "Search files for a literal text pattern.",
            kSchemaGrep,
            std::move(grepSafety),
            ValGrep, DoGrep);
    }

    {
        ToolSafetyProfile psSafety = ReadOnlySafety("Auto-runs clearly read-only PowerShell inspection commands; broader syntactically usable PowerShell commands pause for approval before execution.", true, true);
        psSafety.isAsync = true;
        add(tool_names::kPowerShell,
            "Run PowerShell for local inspection or developer automation. Clearly read-only commands run immediately; broader valid commands require approval before execution.",
            kSchemaPowerShell,
            std::move(psSafety),
            ValPowerShell, DoPowerShell);
    }

    {
        ToolSafetyProfile pythonSafety;
        pythonSafety.tier     = RiskTier::Safe;
        pythonSafety.readOnly = true;
        pythonSafety.isAsync  = true;
        pythonSafety.summary = "Runs only the fixed built-in python_health helper. Returns Python version, executable path, cwd, and platform. No arbitrary code, no user files read or written.";
        add(tool_names::kPythonHealth,
            "Check the controlled Python backend by running the fixed built-in python_health helper. Takes no arguments and never runs arbitrary code.",
            kSchemaPythonHealth,
            std::move(pythonSafety),
            ValPythonHealth, DoPythonHealth);
    }

    {
        ToolSafetyProfile csvSafety;
        csvSafety.tier                 = RiskTier::Safe;
        csvSafety.readOnly             = true;
        csvSafety.mayInspectOutsideCwd = true;
        csvSafety.isAsync              = true;
        csvSafety.summary = "Runs only the fixed csv_inspect helper against local .csv/.tsv files; absolute source paths outside cwd are allowed for reading. Read-only JSON summary; does not modify files. No arbitrary code, no arbitrary script paths, no package installs.";
        add(tool_names::kCsvInspect,
            "Inspect a local CSV/TSV file using the fixed Python csv_inspect helper. Returns JSON summary only; does not modify files.",
            kSchemaCsvInspect,
            std::move(csvSafety),
            ValCsvInspect, DoCsvInspect);
    }

    {
        ToolSafetyProfile reportSafety;
        reportSafety.tier                 = RiskTier::Moderate;
        reportSafety.mutatesFiles         = true;
        reportSafety.mayInspectOutsideCwd = true;
        reportSafety.isAsync              = true;
        reportSafety.summary = "Runs only the fixed csv_report helper against a local .csv/.tsv source file and writes one Markdown report to the conversation Documents folder. No arbitrary code or output paths.";
        add(tool_names::kCsvReport,
            "Create a Markdown report from a local CSV/TSV file using the fixed Python csv_report helper. Writes one report to the conversation Documents folder and returns an artifact card.",
            kSchemaCsvReport,
            std::move(reportSafety),
            ValCsvReport, DoCsvReport);
    }

    {
        ToolSafetyProfile convertSafety;
        convertSafety.tier                 = RiskTier::Moderate;
        convertSafety.mutatesFiles         = true;
        convertSafety.mayInspectOutsideCwd = true;
        convertSafety.isAsync              = true;
        convertSafety.summary = "Runs only the fixed csv_to_xlsx helper against a local .csv/.tsv source file and writes one .xlsx workbook to the conversation Spreadsheets folder. No arbitrary code or output paths. Requires the openpyxl Python package.";
        add(tool_names::kCsvToXlsx,
            "Convert a local CSV/TSV file into an .xlsx workbook using the fixed Python csv_to_xlsx helper. Writes one workbook to the conversation Spreadsheets folder and returns an artifact card.",
            kSchemaCsvToXlsx,
            std::move(convertSafety),
            ValCsvToXlsx, DoCsvToXlsx);
    }

    {
        ToolSafetyProfile xlsxInspectSafety;
        xlsxInspectSafety.tier                 = RiskTier::Safe;
        xlsxInspectSafety.readOnly             = true;
        xlsxInspectSafety.mayInspectOutsideCwd = true;
        xlsxInspectSafety.isAsync              = true;
        xlsxInspectSafety.summary = "Runs only the fixed xlsx_inspect helper against local .xlsx files; absolute source paths outside cwd are allowed for reading. Read-only JSON summary across every sheet; does not modify files. Requires the openpyxl Python package on the system Python.";
        add(tool_names::kXlsxInspect,
            "Inspect a local .xlsx workbook using the fixed Python xlsx_inspect helper. Returns a JSON summary across every sheet only; does not modify files.",
            kSchemaXlsxInspect,
            std::move(xlsxInspectSafety),
            ValXlsxInspect, DoXlsxInspect);
    }

    {
        ToolSafetyProfile xlsxReportSafety;
        xlsxReportSafety.tier                 = RiskTier::Moderate;
        xlsxReportSafety.mutatesFiles         = true;
        xlsxReportSafety.mayInspectOutsideCwd = true;
        xlsxReportSafety.isAsync              = true;
        xlsxReportSafety.summary = "Runs only the fixed xlsx_report helper against a local .xlsx source file and writes one Markdown report covering every sheet to the conversation Documents folder. No arbitrary code or output paths. Requires the openpyxl Python package on the system Python.";
        add(tool_names::kXlsxReport,
            "Create a Markdown report from a local .xlsx workbook using the fixed Python xlsx_report helper. Writes one report covering every sheet to the conversation Documents folder and returns an artifact card.",
            kSchemaXlsxReport,
            std::move(xlsxReportSafety),
            ValXlsxReport, DoXlsxReport);
    }


    {
        ToolSafetyProfile xlsxCreateSafety;
        xlsxCreateSafety.tier                 = RiskTier::Moderate;
        xlsxCreateSafety.mutatesFiles         = true;
        xlsxCreateSafety.mayInspectOutsideCwd = false;
        xlsxCreateSafety.isAsync              = true;
        xlsxCreateSafety.summary = "Runs only the fixed xlsx_create_workbook helper. It accepts structured JSON data and writes one .xlsx workbook to the conversation Spreadsheets folder. No arbitrary Python code, script paths, external reads, or arbitrary output paths. Requires the openpyxl Python package.";
        add(tool_names::kXlsxCreateWorkbook,
            "Create a new formatted .xlsx workbook from structured JSON data using the fixed Python helper. Writes one workbook to the conversation Spreadsheets folder and returns an artifact card. Prefer this over python_create_script for new Excel reports.",
            kSchemaXlsxCreateWorkbook,
            std::move(xlsxCreateSafety),
            ValXlsxCreateWorkbook, DoXlsxCreateWorkbook);
    }

    {
        ToolSafetyProfile pdfSafety;
        pdfSafety.tier                 = RiskTier::Moderate;
        pdfSafety.mutatesFiles         = true;
        pdfSafety.mayInspectOutsideCwd = true;
        pdfSafety.isAsync              = true;
        pdfSafety.summary = "Runs only the fixed pdf_extract_text helper against a local .pdf source file and writes one Markdown text extraction artifact to the conversation PDFs folder. No OCR, PDF editing, arbitrary code, or output paths.";
        add(tool_names::kPdfExtractText,
            "Extract selectable text from a local text-based PDF using the fixed Python pdf_extract_text helper. Writes one Markdown artifact to the conversation PDFs folder and returns an artifact card. No OCR.",
            kSchemaPdfExtractText,
            std::move(pdfSafety),
            ValPdfExtractText, DoPdfExtractText);
    }

    {
        ToolSafetyProfile pdfInspectSafety;
        pdfInspectSafety.tier                 = RiskTier::Safe;
        pdfInspectSafety.readOnly             = true;
        pdfInspectSafety.mayInspectOutsideCwd = true;
        pdfInspectSafety.isAsync              = true;
        pdfInspectSafety.summary = "Runs only the fixed pdf_inspect_form helper against a local AcroForm .pdf source file. Read-only JSON listing of every form field (name, type, page, current value, options, on-states, required flag, tooltip). Does not create or modify files. Refuses XFA-only PDFs with a clear explanation. Requires the PyMuPDF (`pymupdf`) Python package on the system Python.";
        add(tool_names::kPdfInspectForm,
            "Inspect the AcroForm fields of a local fillable PDF using the fixed Python pdf_inspect_form helper. Returns a JSON list of every field (name, type, page, current value, options/on_states, required, tooltip). Does not modify files and does not require approval. Call this before pdf_fill_form so you know the exact field names to fill.",
            kSchemaPdfInspectForm,
            std::move(pdfInspectSafety),
            ValPdfInspectForm, DoPdfInspectForm);
    }

    {
        ToolSafetyProfile pdfFillSafety;
        pdfFillSafety.tier                 = RiskTier::Moderate;
        pdfFillSafety.mutatesFiles         = true;
        pdfFillSafety.mayInspectOutsideCwd = true;
        pdfFillSafety.writesInsideCwdOnly  = false;  // writes to fixed Filled Forms lane, not cwd
        pdfFillSafety.isAsync              = true;
        pdfFillSafety.summary = "Runs only the fixed pdf_fill_form helper against a local AcroForm .pdf source file. Validates the entire {field: value} map first; on any unknown field or invalid value, rejects the whole call with no partial write. On success, writes one filled .pdf to the conversation Filled Forms folder. Cannot fill XFA forms or signature fields. Requires the PyMuPDF (`pymupdf`) Python package on the system Python.";
        add(tool_names::kPdfFillForm,
            "Fill the AcroForm fields of a local fillable PDF using the fixed Python pdf_fill_form helper. Args: first line is the path; remaining lines are a JSON object mapping field names (exactly as pdf_inspect_form reported) to values. Validation is hard-fail across the entire map: any unknown field or invalid value rejects the whole call with no partial write. On success, writes one filled .pdf to the conversation Filled Forms folder and returns an artifact card. Always call pdf_inspect_form first; do not guess field names.",
            kSchemaPdfFillForm,
            std::move(pdfFillSafety),
            ValPdfFillForm, DoPdfFillForm);
    }

    {
        ToolSafetyProfile docxExtractSafety;
        docxExtractSafety.tier                 = RiskTier::Moderate;
        docxExtractSafety.mutatesFiles         = true;
        docxExtractSafety.mayInspectOutsideCwd = true;
        docxExtractSafety.writesInsideCwdOnly  = false;  // writes to the fixed Word lane, not cwd
        docxExtractSafety.isAsync              = true;
        docxExtractSafety.summary = "Runs only the fixed docx_extract_text helper against a local .docx/.docm source file and writes one Markdown text extraction artifact to the conversation Word folder. Uses python-docx when available; if python-docx is missing, falls back to a built-in ZIP/XML extractor for readable text and basic tables. No arbitrary code, output paths, package installs, or network access.";
        add(tool_names::kDocxExtractText,
            "Extract text from a local Word .docx/.docm file using the fixed Python docx_extract_text helper. Uses python-docx when available; if it is missing, falls back to built-in ZIP/XML parsing to extract readable text and basic tables. Writes one Markdown artifact to the conversation Word folder and returns an artifact card.",
            kSchemaDocxExtractText,
            std::move(docxExtractSafety),
            ValDocxExtractText, DoDocxExtractText);
    }

    {
        ToolSafetyProfile docxInspectSafety;
        docxInspectSafety.tier                 = RiskTier::Safe;
        docxInspectSafety.readOnly             = true;
        docxInspectSafety.mayInspectOutsideCwd = true;
        docxInspectSafety.isAsync              = true;
        docxInspectSafety.summary = "Runs only the fixed docx_inspect helper against a local .docx/.docm source file. Read-only JSON summary: paragraph_count, headings (level + text), tables (rows + cols), section_count, has_images, styles_in_use. Uses python-docx when available and a built-in ZIP/XML fallback when it is missing. Does not create or modify files.";
        add(tool_names::kDocxInspect,
            "Inspect the structure of a local Word .docx/.docm file using the fixed Python docx_inspect helper. Uses python-docx when available and a built-in ZIP/XML fallback when it is missing. Returns a JSON summary: paragraph count, heading list (level + text, capped at 100), table list (rows + cols, capped at 100), section count, has_images, styles in use. Does not modify files and does not require approval. Call this when you want a structure overview before extracting full text.",
            kSchemaDocxInspect,
            std::move(docxInspectSafety),
            ValDocxInspect, DoDocxInspect);
    }

    {
        ToolSafetyProfile scriptSafety;
        scriptSafety.tier         = RiskTier::Dangerous;
        scriptSafety.mutatesFiles = true;
        scriptSafety.summary = "Creates a reviewable .py script artifact in the fixed conversation Scripts folder. In agent mode, approval carries forward to one immediate run of that exact created script; approve enables one-approval mode for this chat.";
        add(tool_names::kPythonCreateScript,
            "Create a Python script artifact in the conversation Scripts folder from model-provided code. Returns an artifact card. In agent mode, immediately call python_run_script with the exact created filename next if the user's workflow requires an output file; do not ask for a second approval.",
            kSchemaPythonCreateScript,
            std::move(scriptSafety),
            ValPythonCreateScript, DoPythonCreateScript);
    }

    {
        ToolSafetyProfile runSafety;
        runSafety.tier         = RiskTier::Moderate;
        runSafety.mutatesFiles = true;
        runSafety.isAsync      = true;
        runSafety.summary = "Runs one existing .py script from the fixed conversation Scripts folder, an optional .py helper script from the active project Workflows folder, or a .py helper script from the LlamaBoss Skills folder. A full .py path inside one of those script lanes is also accepted. Optional command-line arguments may be supplied after the script line, one argument per later line. Captures stdout, stderr, exit code, runtime, and may attach newly-created files under the conversation workflow folder as artifact cards.";
        add(tool_names::kPythonRunScript,
            "Run an existing Python script from the conversation Scripts folder, an optional Python helper script from the active project Workflows folder, or a Python helper script from the LlamaBoss Skills folder. Use a filename or a full .py path inside one of those script lanes. Preferred args contract: first line is the script filename/path; optional later lines are command-line arguments, one argument per line. A simple one-line 'script.py ARG' form is also accepted as one argument when unambiguous. Lookup order for bare filenames is conversation Scripts, then project Workflows (when a project is attached), then Skills -- so a project-scoped script with the same filename shadows a Skill one. Captures stdout/stderr/exit code. If the tool exits nonzero, do not claim success; fix the script or explain the failure.",
            kSchemaPythonRunScript,
            std::move(runSafety),
            ValPythonRunScript, DoPythonRunScript);
    }

    {
        ToolSafetyProfile installSafety;
        installSafety.tier         = RiskTier::Dangerous;
        installSafety.mutatesFiles = true;            // installs into user-site
        installSafety.network      = NetworkReach::PublicRead;  // pip fetches from PyPI
        installSafety.isAsync      = true;
        installSafety.summary = "Installs one Python package from PyPI into the user's Python user-site with `py -3 -m pip install --user <package>` (falling back to python/python3 if needed). Per-install approval required: every install renders its own card with the exact package name, even when one-approval mode is on. The package name is validated as a simple PyPI name (no versions, URLs, paths, requirements files, extras, or pip flags).";
        add(tool_names::kPythonInstallPackage,
            "Install one Python package from PyPI into the user's Python user-site using pip. Per-install approval required (each package shows its own approval card). Use when a helper or approved script failed because a dependency is missing, or when the user explicitly asks to install one. Pass the canonical PyPI name; one package per call; no versions, URLs, requirements files, extras, or pip flags.",
            kSchemaPythonInstallPackage,
            std::move(installSafety),
            ValPythonInstallPackage, DoPythonInstallPackage);
    }

    add(tool_names::kOpen,
        "Open/play/view a file or fuzzy-match against recent listings. Safe media launches in the user's default app.",
        kSchemaOpen,
        ReadOnlySafety("Read-only open/view flow. Executable/scriptable files are blocked."),
        ValOpen, DoOpen);

    add(tool_names::kWrite,
        "Create a new file inside the working directory or active project root.",
        kSchemaWrite,
        MutatingCwdSafety("Creates a new file. Controlled write scoped to cwd or active project root."),
        ValWrite, DoWrite);

    add(tool_names::kOverwriteFile,
        "Create or replace a whole file inside the working directory or active project root. Prefer this over edit for full-file replacement, project setup files, and generated scripts.",
        kSchemaOverwriteFile,
        MutatingCwdSafety("Creates or atomically replaces one whole file. Controlled write scoped to cwd or active project root. Python files are syntax-checked before commit when Python is available."),
        ValOverwriteFile, DoOverwriteFile);

    add(tool_names::kMkdir,
        "Create a new directory inside the working directory or active project root.",
        kSchemaMkdir,
        MutatingCwdSafety("Creates a new directory. Controlled write scoped to cwd or active project root."),
        ValMkdir, DoMkdir);

    add(tool_names::kEdit,
        "Replace a unique text block in an existing file.",
        kSchemaEdit,
        MutatingCwdSafety("Edits an existing file. Controlled write scoped to cwd or active project root."),
        ValEdit, DoEdit);

    {
        ToolSafetyProfile deleteSafety = MutatingCwdSafety("Deletes one file or one empty directory. Scoped to cwd or active project root and approval-card gated.");
        deleteSafety.tier          = RiskTier::Dangerous;
        deleteSafety.reversibility = Reversibility::Irreversible;
        add(tool_names::kDelete,
            "Delete a file, or an empty directory, in the working directory or active project root. Requires approval.",
            kSchemaDelete,
            std::move(deleteSafety),
            ValDelete, DoDelete);
    }

    // ── notes_read / notes_append ────────────────────────────────
    // Cross-conversation memory layer. Both target the single fixed
    // path %USERPROFILE%\LlamaBoss\NOTES.md and never touch the cwd or
    // any other location, so they are not subject to cwd-scoping rules
    // — read is treated as a read-only inspection, append is treated
    // as a low-risk fixed-target write that does not need an approval
    // card (the target path is user-owned and the body is plain text).
    {
        ToolSafetyProfile notesReadSafety;
        notesReadSafety.tier                 = RiskTier::Safe;
        notesReadSafety.readOnly             = true;
        notesReadSafety.mayInspectOutsideCwd = true;
        notesReadSafety.summary =
            "Read-only access to the user's NOTES.md (a single fixed file at "
            "%USERPROFILE%\\LlamaBoss\\NOTES.md). Returns the full markdown body; "
            "does not touch any other path.";
        add(tool_names::kNotesRead,
            "Read the user's NOTES.md (cross-conversation facts, paths, preferences, "
            "and named workflows the user has saved). Use when the user references "
            "their notes, when they ask if you remember something, or when prior "
            "guesses about a path or preference have not worked.",
            kSchemaNotesRead,
            std::move(notesReadSafety),
            ValNotesRead, DoNotesRead);
    }

    {
        ToolSafetyProfile notesAppendSafety;
        notesAppendSafety.tier         = RiskTier::Moderate;
        notesAppendSafety.mutatesFiles = true;
        // The target is a single fixed user-owned file; not the cwd.
        // writesInsideCwdOnly stays false so cwd-scope checks don't try
        // to re-anchor it.
        notesAppendSafety.writesInsideCwdOnly = false;
        notesAppendSafety.summary =
            "Append-only write to the user's NOTES.md. Single fixed target path; "
            "never overwrites or deletes prior entries; never touches any other "
            "location. Each call adds one dated entry.";
        add(tool_names::kNotesAppend,
            "Append a new entry to notes. If no project is active, this appends the full "
            "entry to global NOTES.md. If a project is active, this appends the full entry "
            "to the active project's Notes/NOTES.md and writes only a compact pointer/index "
            "entry to global NOTES.md. Use ONLY when the user explicitly asks you to save, "
            "remember, or note something for later. One entry per call.",
            kSchemaNotesAppend,
            std::move(notesAppendSafety),
            ValNotesAppend, DoNotesAppend);
    }

    {
        ToolSafetyProfile projectNotesReadSafety;
        projectNotesReadSafety.tier                 = RiskTier::Safe;
        projectNotesReadSafety.readOnly             = true;
        projectNotesReadSafety.mayInspectOutsideCwd = true;
        projectNotesReadSafety.summary =
            "Read-only access to the active project's Notes\\NOTES.md. "
            "Requires an active project and does not touch any other path.";
        add(tool_names::kProjectNotesRead,
            "Read the active project's Notes/NOTES.md. Use when the user asks for "
            "project notes, notes for this project, project memory, or saved project-specific guidance.",
            kSchemaProjectNotesRead,
            std::move(projectNotesReadSafety),
            ValProjectNotesRead, DoProjectNotesRead);
    }

    {
        ToolSafetyProfile projectNotesAppendSafety;
        projectNotesAppendSafety.tier                = RiskTier::Moderate;
        projectNotesAppendSafety.mutatesFiles        = true;
        projectNotesAppendSafety.writesInsideCwdOnly = false;
        projectNotesAppendSafety.summary =
            "Append-only write to the active project's Notes\\NOTES.md. "
            "Requires an active project; no global NOTES.md pointer is created by this tool.";
        add(tool_names::kProjectNotesAppend,
            "Append a new entry directly to the active project's Notes/NOTES.md. Use when "
            "the user explicitly says to save something to project notes, this project's notes, "
            "or this project memory. Do not use speculatively. One entry per call.",
            kSchemaProjectNotesAppend,
            std::move(projectNotesAppendSafety),
            ValProjectNotesAppend, DoProjectNotesAppend);
    }


    {
        ToolSafetyProfile webSafety;
        webSafety.tier     = RiskTier::Safe;
        webSafety.readOnly = true;
        webSafety.network  = NetworkReach::PublicRead;
        webSafety.summary = "Read-only public webpage inspection using native WinHTTP. Supports http/https GET only, does not execute JavaScript, blocks local/private-network schemes and hosts, limits download size, and saves raw HTML plus extracted text artifacts in the conversation workspace.";
        add(tool_names::kWebFetchUrl,
            "Fetch a public webpage URL and extract readable text so you can summarize, explain, or answer questions about the site. Use when the user pastes a website link or asks what a page is about. Does not execute JavaScript or access logged-in browser sessions.",
            kSchemaWebFetchUrl,
            std::move(webSafety),
            ValWebFetchUrl, DoWebFetchUrl);
    }

    return specs;
}

} // anonymous namespace

// ─── ToolRouter ─────────────────────────────────────────────────

void ToolRouter::Register(ToolSpec spec)
{
    std::string key = Lower(Trim(spec.name));
    if (key.empty()) return;
    if (m_specs.find(key) == m_specs.end()) {
        m_order.push_back(key);
    }
    m_specs[key] = std::move(spec);
}

const ToolSpec* ToolRouter::Find(const std::string& name) const
{
    std::string key = Lower(Trim(name));
    auto it = m_specs.find(key);
    return (it == m_specs.end()) ? nullptr : &it->second;
}

bool ToolRouter::Has(const std::string& name) const
{
    return Find(name) != nullptr;
}

std::vector<const ToolSpec*> ToolRouter::All() const
{
    std::vector<const ToolSpec*> out;
    out.reserve(m_order.size());
    for (const std::string& key : m_order) {
        auto it = m_specs.find(key);
        if (it != m_specs.end()) out.push_back(&it->second);
    }
    return out;
}

// ─── Global router ──────────────────────────────────────────────
// Built lazily on first call; std::call_once keeps the build itself
// race-free on the off chance two threads request the router at
// startup.  After init the router is read-only and shared across
// callers without further locking.
ToolRouter& GetGlobalRouter()
{
    static ToolRouter      router;
    static std::once_flag  initFlag;
    std::call_once(initFlag, []() {
        for (ToolSpec& s : BuildBuiltinSpecs()) {
            router.Register(std::move(s));
        }
    });
    return router;
}

// ─── Phase 3c-i: native tool catalog ────────────────────────────
// Walk every registered spec and emit an OpenAI-shape tool entry.
// Each spec carries its parameter schema as a stringified JSON
// object (constructed in BuildBuiltinSpecs from the kSchemaXxx raw
// strings).  We parse those strings into Poco JSON values here so
// the final output is a single coherent JSON document, not a
// document with embedded JSON-as-string.
//
// On parse failure for a particular schema we fall back to an
// empty {} object.  This is defensive: any schema that fails to
// parse is a bug in our own constants and should be visible at
// development time, but we don't want a single typo to nuke the
// entire tool catalog at runtime.
std::string BuildToolsArrayJson(const ToolRouter& router)
{
    Poco::JSON::Array::Ptr toolsArr = new Poco::JSON::Array;

    for (const ToolSpec* spec : router.All()) {
        if (!spec) continue;

        // Parse the schema string into a JSON object so it nests
        // cleanly under "function.parameters".  An empty schema
        // string maps to an empty object — valid for tools that
        // take no arguments (e.g. pwd).
        Poco::JSON::Object::Ptr params;
        if (spec->parameters_json_schema.empty()) {
            params = new Poco::JSON::Object;
            params->set("type", std::string("object"));
        } else {
            try {
                Poco::JSON::Parser p;
                auto var = p.parse(spec->parameters_json_schema);
                params = var.extract<Poco::JSON::Object::Ptr>();
            } catch (...) {
                params = new Poco::JSON::Object;
                params->set("type", std::string("object"));
            }
        }

        Poco::JSON::Object::Ptr fn = new Poco::JSON::Object;
        fn->set("name",        spec->name);
        fn->set("description", spec->description + SafetySuffix(*spec));
        fn->set("parameters",  params);

        Poco::JSON::Object::Ptr entry = new Poco::JSON::Object;
        entry->set("type",     std::string("function"));
        entry->set("function", fn);

        toolsArr->add(entry);
    }

    std::ostringstream os;
    Poco::JSON::Stringifier::stringify(toolsArr, os);
    return os.str();
}

const std::string& GetCachedToolsArrayJson()
{
    static std::once_flag initFlag;
    static std::string cached;

    std::call_once(initFlag, []() {
        cached = BuildToolsArrayJson(GetGlobalRouter());
    });

    return cached;
}


// ─── Prompt-facing safety summary ────────────────────────────────
// Generated from ToolSpec safety metadata so the model-facing prompt
// and the router catalog stay aligned as new tools are added.
std::string BuildToolSafetySummaryText(const ToolRouter& router)
{
    std::vector<std::string> readOnly;
    std::vector<std::string> mutating;
    std::vector<std::string> policy;
    std::vector<std::string> approval;

    for (const ToolSpec* spec : router.All()) {
        if (!spec) continue;
        if (spec->safety.readOnly) {
            readOnly.push_back(spec->name);
        }
        if (spec->safety.mutatesFiles) {
            mutating.push_back(spec->name);
        }
        if (spec->safety.policyEnforced) {
            policy.push_back(spec->name);
        }
        if (spec->safety.requiresApproval()) {
            approval.push_back(spec->name);
        }
    }

    auto join = [](const std::vector<std::string>& items) {
        std::ostringstream ss;
        for (size_t i = 0; i < items.size(); ++i) {
            if (i > 0) ss << ", ";
            ss << items[i];
        }
        return ss.str();
    };

    std::ostringstream ss;
    ss << "Tool safety summary generated from ToolSpec metadata:\n";
    if (!readOnly.empty()) {
        ss << "  Read-only tools: " << join(readOnly)
           << ". These may inspect local paths outside the cwd when their tool/policy allows it.\n";
    }
    if (!mutating.empty()) {
        ss << "  Controlled write/artifact tools: " << join(mutating)
           << ". These use tool-specific path controls; workspace writes stay scoped to the cwd plus the active project root when attached, and artifact helpers write only into conversation workflow folders unless a workflow explicitly writes project outputs.\n";
    }
    if (!policy.empty()) {
        ss << "  Policy-enforced tools: " << join(policy)
           << ". Clearly read-only PowerShell inspection commands run immediately; broader valid PowerShell commands pause for approval; malformed commands are rejected.\n";
    }
    if (!approval.empty()) {
        ss << "  Approval-card tools: " << join(approval)
           << ". These pause for /approve or /deny before execution.\n";
    }
    ss << "  Rule of thumb: use native read/file/artifact tools when they fit; use PowerShell for read-only inspection automatically and for broader developer/system automation only when the user approves the command.\n";
    return ss.str();
}
