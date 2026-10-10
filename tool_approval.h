// tool_approval.h
//
// Approval Cards -- small, header-only gate shared by the slash path
// (MyFrame) and the agent path (AgentController).
//
// This layer deliberately sits BEFORE DispatchInvocation.  The
// actual tools keep their own sandbox / policy checks; approval
// only pauses risky intent so the user can explicitly allow or deny
// it.  Approval cards are UI-only and are not written to chat history.
//
// PowerShell has a hybrid gate: command_policy.cpp auto-allows
// clearly read-only commands, rejects malformed commands, and routes
// broader shell automation into this approval-card layer.
#pragma once

#include "command_policy.h"
#include "lb_string_utils.h"   // LbUtf8SafeTruncate
#include "tool_block.h"
#include "tool_context.h"
#include "tool_dispatcher.h"
#include "tool_invocation.h"
#include "tool_path.h"
#include "tool_path_safety.h"
#include "tool_router.h"     // GetGlobalRouter() for ClassifyTier lookup
#include "server_manager.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

namespace tool_approval {

// Backward-compatibility alias.  RiskTier moved to tool_safety.h (global
// namespace) so it can live on ToolSpec.safety.tier without circular
// includes.  This alias keeps any older `tool_approval::RiskTier::*`
// references compiling without forcing a tree-wide rename.
using RiskTier = ::RiskTier;

// ═══════════════════════════════════════════════════════════════════
//  Risk tiering (ToolSpec.safety.tier)
// ═══════════════════════════════════════════════════════════════════
//
// Each tool is classified ONCE on its ToolSpec.safety.tier (see
// tool_safety.h and the BuildBuiltinSpecs registrations in
// tool_router.cpp).  ClassifyTier is a one-line lookup against the
// global router kept for older call sites; new code should read
// spec.safety.tier directly.  RequiresApproval renders an approval
// card only for the Dangerous tier.  Safe and Moderate tools rely on:
//   - in agent mode  : the model's natural-language ask + user "yes"
//   - in slash mode  : the user having literally typed the command
// so the user isn't asked twice (model asks, user says yes, card asks
// again).
//
// Because Moderate tools run without a card, a Moderate tool must never
// be able to combine with another approval-free tool into arbitrary
// code execution (e.g. write a script, then run it).  Anything that
// executes code chosen by the model belongs in Dangerous.
//
// PowerShell is conditional: clearly read-only commands stay outside
// the approval-card path, while broader syntactically usable commands
// are routed here by command_policy.cpp for explicit review.
//
// The RiskTier enum lives in tool_safety.h so the ToolSafetyProfile
// struct can carry it as a field.

inline RiskTier ClassifyTier(const std::string& toolName)
{
    const ToolSpec* spec = GetGlobalRouter().Find(toolName);
    return spec ? spec->safety.tier : RiskTier::Safe;
}

// ═══════════════════════════════════════════════════════════════════

struct ApprovalDecision {
    bool        required = false;
    bool        grantsWriteRoot = false;
    std::string reason;
    std::string target;
    std::string preview;
    std::string writeRoot;
    ToolBlock   block;
};

inline std::string Trim(std::string s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

inline std::string FirstLine(const std::string& s)
{
    size_t nl = s.find('\n');
    std::string out = (nl == std::string::npos) ? s : s.substr(0, nl);
    while (!out.empty()) {
        char c = out.back();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') out.pop_back();
        else break;
    }
    return out;
}

inline std::string CommandEcho(const ToolInvocation& inv)
{
    if (inv.name == tool_names::kPowerShell) return inv.args;
    if (inv.name == tool_names::kPy) {
        std::string firstLine = FirstLine(Trim(inv.args));
        firstLine = LbUtf8SafeTruncate(firstLine, 120);
        const bool multiline = Trim(inv.args).find('\n') != std::string::npos;
        return "/py " + firstLine + (multiline ? " ..." : "");
    }
    if (inv.name == tool_names::kPythonHealth) return "python_health";
    if (inv.name == tool_names::kCsvInspect)
        return Trim(inv.args).empty() ? std::string("/csv_inspect")
                                      : ("/csv_inspect " + Trim(inv.args));
    if (inv.name == tool_names::kCsvReport)
        return Trim(inv.args).empty() ? std::string("/csv_report")
                                      : ("/csv_report " + Trim(inv.args));
    if (inv.name == tool_names::kCsvToXlsx)
        return Trim(inv.args).empty() ? std::string("/csv_to_xlsx")
                                      : ("/csv_to_xlsx " + Trim(inv.args));
    if (inv.name == tool_names::kXlsxInspect)
        return Trim(inv.args).empty() ? std::string("/xlsx_inspect")
                                      : ("/xlsx_inspect " + Trim(inv.args));
    if (inv.name == tool_names::kXlsxReport)
        return Trim(inv.args).empty() ? std::string("/xlsx_report")
                                      : ("/xlsx_report " + Trim(inv.args));
    if (inv.name == tool_names::kXlsxCreateWorkbook)
        return "/xlsx_create_workbook";
    if (inv.name == tool_names::kPdfExtractText)
        return Trim(inv.args).empty() ? std::string("/pdf_extract_text")
                                      : ("/pdf_extract_text " + Trim(inv.args));
    if (inv.name == tool_names::kPythonCreateScript) {
        std::string first = FirstLine(inv.args);
        return first.empty() ? std::string("/python_create_script")
                             : ("/python_create_script " + first);
    }
    if (inv.name == tool_names::kPythonRunScript)
        return Trim(inv.args).empty() ? std::string("/python_run_script")
                                      : ("/python_run_script " + Trim(inv.args));
    if (inv.name == tool_names::kPythonInstallPackage)
        return Trim(inv.args).empty() ? std::string("/python_install_package")
                                      : ("/python_install_package " + Trim(inv.args));
    if (inv.name == tool_names::kWrite || inv.name == tool_names::kOverwriteFile || inv.name == tool_names::kWritePowerShellScript || inv.name == tool_names::kEdit) {
        std::string first = FirstLine(inv.args);
        return first.empty() ? ("/" + inv.name) : ("/" + inv.name + " " + first);
    }
    return Trim(inv.args).empty() ? ("/" + inv.name)
                                  : ("/" + inv.name + " " + Trim(inv.args));
}

inline std::string ToolDisplayName(const std::string& name)
{
    // Single source of truth: ToolSpec.displayName, populated by the
    // kPresentation table in tool_router.cpp, so the mapping can't
    // drift between call sites.
    const ToolSpec* spec = GetGlobalRouter().Find(name);
    if (spec && !spec->displayName.empty()) return spec->displayName;
    return name.empty() ? std::string("Tool") : name;
}

inline std::string ToolIcon(const std::string& name)
{
    // Single source of truth: ToolSpec.iconUtf8 (see kPresentation in
    // tool_router.cpp).  Side benefit over the old ladder: tools the
    // ladder never listed (pdf_inspect_form, pdf_fill_form, docx_*,
    // notes_*, open, ls) now render their real icons on approval and
    // pending cards instead of the ⚠ fallback.
    const ToolSpec* spec = GetGlobalRouter().Find(name);
    if (spec && !spec->iconUtf8.empty()) return spec->iconUtf8;
    return "\xE2\x9A\xA0";                                         // ⚠
}

inline std::string LimitText(const std::string& s, size_t maxChars = 1200)
{
    if (s.size() <= maxChars) return s;
    return s.substr(0, maxChars) + "\n... [preview truncated]";
}

inline size_t CountLines(const std::string& s)
{
    if (s.empty()) return 0;
    size_t n = 0;
    for (char c : s) if (c == '\n') ++n;
    if (s.back() != '\n') ++n;
    return n;
}

inline void SplitWriteArgs(const std::string& args,
                           std::string&       pathOut,
                           std::string&       contentOut)
{
    size_t nl = args.find('\n');
    if (nl == std::string::npos) {
        pathOut = args;
        contentOut.clear();
    } else {
        pathOut = args.substr(0, nl);
        contentOut = args.substr(nl + 1);
    }
    pathOut = Trim(pathOut);
}

inline bool SplitEditArgs(const std::string& args,
                          std::string&       pathOut,
                          std::string&       oldOut,
                          std::string&       newOut)
{
    const std::string oldSent = "<<<OLD>>>";
    const std::string newSent = "<<<NEW>>>";

    size_t firstNl = args.find('\n');
    if (firstNl == std::string::npos) return false;
    pathOut = Trim(args.substr(0, firstNl));

    size_t oldPos = args.find(oldSent, firstNl + 1);
    if (oldPos == std::string::npos) return false;
    size_t oldStart = oldPos + oldSent.size();
    if (oldStart < args.size() && args[oldStart] == '\r') ++oldStart;
    if (oldStart < args.size() && args[oldStart] == '\n') ++oldStart;

    size_t newPos = args.find(newSent, oldStart);
    if (newPos == std::string::npos) return false;
    oldOut = args.substr(oldStart, newPos - oldStart);
    while (!oldOut.empty() && (oldOut.back() == '\r' || oldOut.back() == '\n'))
        oldOut.pop_back();

    size_t newStart = newPos + newSent.size();
    if (newStart < args.size() && args[newStart] == '\r') ++newStart;
    if (newStart < args.size() && args[newStart] == '\n') ++newStart;
    newOut = args.substr(newStart);
    return true;
}

inline std::string ResolveTargetForPreview(const std::string& requested,
                                           const ToolContext& ctx)
{
    std::string trimmed = Trim(requested);
    if (trimmed.empty() || ctx.cwd.empty()) return trimmed;
    std::string resolved = tool_path_safety::ResolveProjectAwareToolPath(trimmed, ctx.cwd, ctx.activeProjectRoot);
    return resolved.empty() ? trimmed : resolved;
}

inline std::string JoinPathForApproval(const std::string& a, const std::string& b)
{
    if (a.empty()) return b;
    if (a.back() == '/' || a.back() == '\\') return a + b;
#ifdef _WIN32
    return a + "\\" + b;
#else
    return a + "/" + b;
#endif
}

// The Scripts lane python_create_script will actually write to: this
// chat's Scripts folder, or LlamaBoss\Shared\Scripts outside a chat.
inline std::string ScriptsDirForPreview(const std::string& cwd)
{
    return ServerManager::ConversationScriptsDirForCwd(cwd);
}

inline std::string ScriptPreviewPath(const std::string& requested,
                                     const std::string& cwd)
{
    std::string name = Trim(requested);
    if (name.empty()) return ScriptsDirForPreview(cwd);
    if (name.find('.') == std::string::npos) name += ".py";
    return JoinPathForApproval(ScriptsDirForPreview(cwd), name);
}

inline std::string ProjectWorkflowScriptPreviewPath(const std::string& requested,
                                                    const ToolContext& ctx)
{
    std::string name = Trim(requested);
    if (name.empty()) return ScriptPreviewPath(requested, ctx.cwd);
    if (name.find('.') == std::string::npos) name += ".py";
    if (!ctx.activeProjectRoot.empty()) {
        return JoinPathForApproval(JoinPathForApproval(ctx.activeProjectRoot, "Workflows"), name);
    }
    return ScriptPreviewPath(requested, ctx.cwd);
}

inline std::string PreviewForInvocation(const ToolInvocation& inv,
                                        const ToolContext&    ctx,
                                        std::string&          targetOut)
{
    std::ostringstream p;

    if (inv.name == tool_names::kPy) {
        // The code IS the action: show it whole (limited) so the user
        // approves exactly what will execute in the persistent session.
        const std::string code = Trim(inv.args);
        targetOut.clear();
        p << "Session working directory: " << ctx.cwd << "\n"
          << "Bytes: " << code.size() << "\n"
          << "Lines: " << CountLines(code) << "\n\n"
          << "Runs in this conversation's persistent Python session; "
             "variables persist across py calls. No API keys are "
             "injected.\n\n"
          << LimitText(code.empty() ? std::string("[empty code]") : code);
        return p.str();
    }

    if (inv.name == tool_names::kWrite) {
        std::string path, content;
        SplitWriteArgs(inv.args, path, content);
        targetOut = ResolveTargetForPreview(path, ctx);
        p << "Target: " << targetOut << "\n"
          << "Bytes: " << content.size() << "\n"
          << "Lines: " << CountLines(content) << "\n\n"
          << LimitText(content.empty() ? std::string("[empty file]") : content);
        return p.str();
    }

    if (inv.name == tool_names::kWritePowerShellScript) {
        std::string path, content;
        SplitWriteArgs(inv.args, path, content);
        targetOut = ResolveTargetForPreview(path, ctx);
        p << "Target PowerShell script: " << targetOut << "\n"
          << "Bytes: " << content.size() << "\n"
          << "Lines: " << CountLines(content) << "\n\n"
          << "Creates or replaces a .ps1 file only; it does not execute it. Review the source below before approving.\n\n"
          << LimitText(content.empty() ? std::string("[empty script body]") : content);
        return p.str();
    }

    if (inv.name == tool_names::kEdit) {
        std::string path, oldS, newS;
        if (SplitEditArgs(inv.args, path, oldS, newS)) {
            targetOut = ResolveTargetForPreview(path, ctx);
            p << "Target: " << targetOut << "\n\n"
              << "--- OLD ---\n" << LimitText(oldS, 700) << "\n\n"
              << "+++ NEW +++\n" << LimitText(newS.empty() ? std::string("[empty replacement]") : newS, 700);
        } else {
            targetOut = ResolveTargetForPreview(FirstLine(inv.args), ctx);
            p << "Target: " << targetOut << "\n"
              << "Could not build edit preview from args; dispatch will validate before editing.";
        }
        return p.str();
    }

    if (inv.name == tool_names::kDelete) {
        targetOut = ResolveTargetForPreview(inv.args, ctx);
        p << "Target: " << targetOut << "\n"
          << "Warning: this removes the file or empty directory if the tool's safety checks pass.";
        return p.str();
    }

    if (inv.name == tool_names::kMkdir) {
        targetOut = ResolveTargetForPreview(inv.args, ctx);
        p << "Target directory: " << targetOut;
        return p.str();
    }

    if (inv.name == tool_names::kPowerShell) {
        targetOut = "PowerShell";
        p << "Command:\n" << inv.args;
        return p.str();
    }

    if (inv.name == tool_names::kPythonHealth) {
        targetOut = "python_health";
        p << "Built-in helper: python_health\n"
          << "Runs only the bundled helper script managed by LlamaBoss. No arbitrary Python code or script path is accepted.";
        return p.str();
    }

    if (inv.name == tool_names::kCsvInspect) {
        targetOut = ResolveTargetForPreview(inv.args, ctx);
        p << "Target data file: " << targetOut << "\n"
          << "Runs only the bundled csv_inspect helper. The helper reads .csv/.tsv files inside the current LlamaBoss working directory and returns a JSON summary. It does not modify files.";
        return p.str();
    }

    if (inv.name == tool_names::kCsvReport) {
        targetOut = ResolveTargetForPreview(inv.args, ctx);
        p << "Target data file: " << targetOut << "\n"
          << "Output: this chat's Documents folder\n"
          << "Runs only the bundled csv_report helper. The helper reads a .csv/.tsv file inside the current LlamaBoss working directory and creates one Markdown report artifact. It does not accept arbitrary Python code, script paths, or output paths.";
        return p.str();
    }

    if (inv.name == tool_names::kXlsxInspect) {
        targetOut = ResolveTargetForPreview(inv.args, ctx);
        p << "Target spreadsheet: " << targetOut << "\n"
          << "Runs only the bundled xlsx_inspect helper. The helper reads .xlsx files inside the current LlamaBoss working directory and returns a JSON summary across all sheets. It does not modify files. Requires the openpyxl Python package.";
        return p.str();
    }

    if (inv.name == tool_names::kXlsxReport) {
        targetOut = ResolveTargetForPreview(inv.args, ctx);
        p << "Target spreadsheet: " << targetOut << "\n"
          << "Output: this chat's Documents folder\n"
          << "Runs only the bundled xlsx_report helper. The helper reads an .xlsx file inside the current LlamaBoss working directory and creates one Markdown report artifact across all sheets. It does not accept arbitrary Python code, script paths, or output paths. Requires the openpyxl Python package.";
        return p.str();
    }

    if (inv.name == tool_names::kXlsxCreateWorkbook) {
        targetOut = "LlamaBoss Spreadsheets folder";
        p << "Output: LlamaBoss Spreadsheets folder\n"
          << "Runs only the bundled xlsx_create_workbook helper. The helper accepts structured JSON data and creates one .xlsx workbook. It does not accept arbitrary Python code, script paths, external source reads, or arbitrary output paths. Requires the openpyxl Python package.";
        return p.str();
    }

    if (inv.name == tool_names::kPdfExtractText) {
        targetOut = ResolveTargetForPreview(inv.args, ctx);
        p << "Target PDF file: " << targetOut << "\n"
          << "Output: LlamaBoss PDFs folder\n"
          << "Runs only the bundled pdf_extract_text helper. The helper reads one text-based .pdf file inside the current LlamaBoss working directory and creates one Markdown text artifact. No OCR, PDF editing, arbitrary Python code, script paths, or output paths.";
        return p.str();
    }

    if (inv.name == tool_names::kPythonInstallPackage) {
        std::string packageName = Trim(inv.args);
        targetOut = packageName;
        p << "Package: " << packageName << "\n"
          << "Command: py -3 -m pip install --user --disable-pip-version-check " << packageName << "\n"
          << "Fallback launchers: python -m pip, then python3 -m pip if py -3 is unavailable.\n\n"
          << "This downloads and installs `" << packageName << "` from the Python "
          << "Package Index into the user's per-user site-packages. It changes the "
          << "local Python environment and uses the network. The package name is "
          << "validated as a simple PyPI name -- no versions, URLs, requirements "
          << "files, extras, or pip flags -- and every install renders this card "
          << "with the exact name pip will see, even when one-approval mode is on.";
        return p.str();
    }

    if (inv.name == tool_names::kPythonCreateScript) {
        std::string filename, content;
        SplitWriteArgs(inv.args, filename, content);
        targetOut = ProjectWorkflowScriptPreviewPath(filename, ctx);
        p << "Target Python script: " << targetOut << "\n"
          << (!ctx.activeProjectRoot.empty()
                  ? "Output: active project Workflows folder\n"
                  : "Output: this chat's Scripts folder\n")
          << "Bytes: " << content.size() << "\n"
          << "Lines: " << CountLines(content) << "\n\n"
          << "Creates a reviewable .py script artifact. In a project chat, the script is created in that project's Workflows folder; otherwise it is created in this chat's Scripts folder. If this task needs output, this approval also covers one immediate run of this exact script. Review the source below before approving.\n\n"
          << LimitText(content.empty() ? std::string("[empty script body]") : content);
        return p.str();
    }

    if (inv.name == tool_names::kPythonRunScript) {
        targetOut = ProjectWorkflowScriptPreviewPath(inv.args, ctx);
        p << "Target Python script: " << targetOut << "\n"
          << "Conversation Scripts fallback: " << ScriptPreviewPath(inv.args, ctx.cwd) << "\n"
          << "Working directory: " << ctx.cwd << "\n"
          << "Runs one existing .py script from the conversation Scripts folder, the active project's Workflows folder, or the LlamaBoss Skills folder (bare names are looked up in that order). "
          << "Configured Connection API keys are injected as environment variables. "
          << "Captures stdout, stderr, exit code, runtime, and attaches newly created files under the LlamaBoss root as artifact cards.\n\n"
          << "Request:\n" << LimitText(Trim(inv.args), 600);
        return p.str();
    }

    targetOut = Trim(inv.args);
    return std::string();
}

inline std::string ApprovalActionVerb(const ToolInvocation& inv)
{
    if (inv.name == tool_names::kPythonCreateScript) return "create it";
    if (inv.name == tool_names::kPythonInstallPackage) return "install it";
    if (inv.name == tool_names::kWrite) return "create it";
    if (inv.name == tool_names::kOverwriteFile) return "overwrite it";
    if (inv.name == tool_names::kWritePowerShellScript) return "write it";
    if (inv.name == tool_names::kMkdir) return "create it";
    if (inv.name == tool_names::kEdit) return "edit it";
    if (inv.name == tool_names::kDelete) return "delete it";
    return "run it";
}

inline bool IsNativePathMutation(const std::string& name)
{
    return name == tool_names::kWrite ||
           name == tool_names::kOverwriteFile ||
           name == tool_names::kWritePowerShellScript ||
           name == tool_names::kEdit ||
           name == tool_names::kMkdir ||
           name == tool_names::kDelete;
}

inline bool RequestedMutationPath(const ToolInvocation& inv,
                                  std::string& requestedOut)
{
    requestedOut.clear();

    if (inv.name == tool_names::kWrite ||
        inv.name == tool_names::kOverwriteFile ||
        inv.name == tool_names::kWritePowerShellScript) {
        std::string ignored;
        SplitWriteArgs(inv.args, requestedOut, ignored);
        return !requestedOut.empty();
    }

    if (inv.name == tool_names::kEdit) {
        std::string oldText, newText;
        return SplitEditArgs(inv.args, requestedOut, oldText, newText) &&
               !requestedOut.empty();
    }

    if (inv.name == tool_names::kMkdir ||
        inv.name == tool_names::kDelete) {
        requestedOut = Trim(inv.args);
        return !requestedOut.empty();
    }

    return false;
}

// Pre-dispatch capability gate for native path mutations.  This is separate
// from ordinary Dangerous-tier approval: granting a folder authorizes WHERE
// native tools may operate, not WHAT a particular tool may do.  After a grant
// the caller must run the invocation through RequiresApproval again so delete
// and other dangerous actions retain their normal action review.
inline bool RequiresWriteRootGrant(const ToolInvocation& inv,
                                   const ToolContext&    ctx,
                                   ApprovalDecision&     out)
{
    out = ApprovalDecision{};
    if (!inv.valid || !IsNativePathMutation(inv.name) || ctx.cwd.empty())
        return false;

    std::string requested;
    if (!RequestedMutationPath(inv, requested)) return false;

    const std::string resolved =
        tool_path_safety::ResolveProjectAwareToolPath(
            requested, ctx.cwd, ctx.activeProjectRoot);
    if (resolved.empty()) return false;

    if (tool_path_safety::IsUnderAllowedWriteRoot(
            resolved, ctx.cwd, ctx.activeProjectRoot, ctx.skillsRoot,
            ctx.additionalWriteRoots)) {
        return false;
    }

    std::string grantRoot = tool_path_safety::ParentDir(resolved);
    if (grantRoot.empty()) return false;

    // Never turn a root-level file request into an all-drive capability.
    // Equality still lets the one requested file (or directory being created)
    // pass the containment check without authorizing its siblings.
    if (tool_path_safety::IsDriveRoot(grantRoot)) grantRoot = resolved;

    out.required        = true;
    out.grantsWriteRoot = true;
    out.target          = resolved;
    out.writeRoot       = grantRoot;
    out.reason =
        "The requested native file change is outside this chat's current "
        "writable workspace, attached project, Skills folder, and previously "
        "granted folders.";

    out.block.iconUtf8    = "\xF0\x9F\x94\x92"; // lock
    out.block.toolName    = "Folder Access Required";
    out.block.statusChips = { "pending", "write root" };
    out.block.commandEcho = CommandEcho(inv);
    out.block.approvalPresentation =
        ToolApprovalPresentation::WriteRootGrant;

    std::ostringstream body;
    body << "Native editing access is required for this folder.\n\n"
         << "Requested target: " << resolved << "\n"
         << "Folder to grant: " << grantRoot << "\n\n"
         << "Granting access adds only this exact folder to the current "
            "chat's native write roots. The grant is not saved to the "
            "conversation file and is cleared when LlamaBoss restarts.\n\n"
         << "This folder grant does not approve a destructive action. If the "
            "requested tool normally requires approval, its regular review "
            "card appears next. PowerShell is not used as a workaround.\n\n"
         << "Choose Grant Folder for Chat to continue, or Cancel. You can "
            "also attach the project from the Project menu for durable "
            "project context.";
    out.block.body = body.str();
    return true;
}

inline bool RequiresApproval(const ToolInvocation& inv,
                             const ToolContext&    ctx,
                             ApprovalDecision&     out)
{
    out = ApprovalDecision{};
    if (!inv.valid) return false;

    // ── Conditional PowerShell gate ─────────────────────────────
    // Read-only allowlisted commands run immediately.  Broader shell
    // automation is intentionally not blocked; it pauses here so the
    // user can review and approve the exact command before dispatch.
    // Malformed/empty commands do not render a card -- DispatchInvocation
    // will surface the policy rejection back to the model/user.
    if (inv.name == tool_names::kPowerShell) {
        PolicyDecision ps = EvaluatePowerShellCommand(inv.args);
        if (!ps.requiresApproval) {
            return false;
        }

        out.required = true;
        out.reason = "This PowerShell command falls outside the automatic read-only inspection profile and must be reviewed before execution.";
        if (!ps.reason.empty()) {
            out.reason += " Policy note: " + ps.reason + ".";
        }
        out.preview = PreviewForInvocation(inv, ctx, out.target);

        out.block.iconUtf8     = "\xE2\x9A\xA0"; // ⚠
        out.block.toolName     = "Approval Required";
        out.block.statusChips  = { "pending", ToolDisplayName(inv.name) };
        out.block.commandEcho  = CommandEcho(inv);
        out.block.bodyLang.clear();

        std::ostringstream body;
        body << "Approval required: " << ToolDisplayName(inv.name);
        if (!out.target.empty()) body << " " << out.target;
        body << "\n\nReason: " << out.reason;
        if (!out.preview.empty()) body << "\n\n" << out.preview;
        body << "\n\nApprove options:\n"
             << "  approve       Trust tools for this chat until LlamaBoss restarts.\n"
             << "  approve once  Approve only this action.\n"
             << "  deny          Cancel.\n"
             << "Slash forms also work.";
        out.block.body = body.str();
        return true;
    }

    if (ClassifyTier(inv.name) != RiskTier::Dangerous) {
        // Safe and Moderate tools never render an approval card.
        // Conversational consent (agent mode) and explicit user
        // invocation (slash mode) carry the trust burden instead.
        return false;
    }

    // ── Dangerous tier — render an approval card ─────────────────
    if (inv.name == tool_names::kDelete) {
        out.required = true;
        out.reason = "This tool can permanently remove a file or empty directory. The action cannot be undone.";
    }
    else if (inv.name == tool_names::kPythonCreateScript) {
        out.required = true;
        out.reason = "Creates a reviewable Python script in this chat's Scripts folder. Review the source before approving.";
    }
    else if (inv.name == tool_names::kPythonInstallPackage) {
        out.required = true;
        out.reason = "This installs one Python package from PyPI into the user's Python user-site using pip. It changes the local Python environment and uses the network, so it requires approval -- and every install renders its own card showing the exact package name, even when one-approval mode is on.";
    }
    else if (inv.name == tool_names::kWritePowerShellScript) {
        out.required = true;
        out.reason = "Creates or replaces a PowerShell .ps1 script. The script is written but not executed; review the source before approving.";
    }
    else if (inv.name == tool_names::kPythonRunScript) {
        out.required = true;
        out.reason = "Runs a Python script with full local file and network access. Every configured Connection API key is injected into its environment. Scripts in the project Workflows and Skills folders can be changed by write/overwrite_file/edit without approval, so review the script before approving.";
    }
    else if (inv.name == tool_names::kPy) {
        out.required = true;
        out.reason = "Executes arbitrary Python code in this conversation's persistent session. The code below runs immediately on approval, with full local file and network access (no API keys are injected). One-approval mode covers later py calls in this chat.";
    }
    else {
        // Defensive default for any future Dangerous-tier addition
        // that hasn't yet been given a dedicated reason string.
        out.required = true;
        out.reason = "This tool performs a destructive or irreversible action.";
    }

    out.preview = PreviewForInvocation(inv, ctx, out.target);

    out.block.iconUtf8     = "\xE2\x9A\xA0"; // ⚠
    out.block.toolName     = "Approval Required";
    out.block.statusChips  = { "pending", ToolDisplayName(inv.name) };
    out.block.commandEcho  = CommandEcho(inv);
    out.block.bodyLang.clear();

    std::ostringstream body;
    body << "Approval required: " << ToolDisplayName(inv.name);
    if (!out.target.empty()) body << " " << out.target;
    body << "\n\nReason: " << out.reason;
    if (!out.preview.empty()) body << "\n\n" << out.preview;
    body << "\n\nApprove options:\n"
         << "  approve       Trust tools for this chat until LlamaBoss restarts.\n"
         << "  approve once  Approve only this action.\n"
         << "  deny          Cancel.\n"
         << "Slash forms also work.";
    out.block.body = body.str();
    return true;
}

inline ToolInvocationResult DeniedResult(const ToolInvocation& inv,
                                         const std::string&    message)
{
    ToolInvocationResult r;
    r.toolTag       = inv.name.empty() ? "tool" : inv.name;
    r.invocationRaw = inv.rawBlock;
    r.iconUtf8      = ToolIcon(inv.name);
    r.toolName      = ToolDisplayName(inv.name);
    r.commandEcho   = CommandEcho(inv);
    r.chips         = { "denied" };
    r.body          = message.empty()
                        ? std::string("Denied by user. Tool was not executed.")
                        : message;
    return r;
}

} // namespace tool_approval
