// tool_approval.h
//
// Phase 6: Approval Cards -- small, header-only gate shared by
// the slash path (MyFrame) and the agent path (AgentController).
//
// This layer deliberately sits BEFORE DispatchInvocation.  The
// actual tools keep their existing sandbox / policy checks; approval
// only pauses risky intent so the user can explicitly allow or deny
// it.  Approval cards are UI-only and are not written to chat history.
//
// PowerShell is intentionally not gated here: command_policy.cpp's
// allowlist remains the hard boundary for shell invocations.
#pragma once

#include "tool_block.h"
#include "tool_context.h"
#include "tool_dispatcher.h"
#include "tool_invocation.h"
#include "tool_path.h"
#include "server_manager.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

namespace tool_approval {

// ═══════════════════════════════════════════════════════════════════
//  Risk tiering — Phase 3
// ═══════════════════════════════════════════════════════════════════
//
// RequiresApproval used to walk a per-tool if/else ladder where each
// branch hard-coded "needs approval" for tools that touched the
// filesystem.  That worked when /approve was the trust mechanism.
// Once conversational consent matured (the model self-asks "want me
// to do X?" and the user replies in prose), that ladder produced
// double-prompts: model asks, user says yes, system ALSO renders an
// approval card asking the same thing.
//
// The tier system replaces that ladder.  Each tool is classified once
// by ClassifyTier; RequiresApproval renders an approval card only for
// the Dangerous tier.  Safe and Moderate tools rely on:
//   - in agent mode  : the model's natural-language ask + user's "yes"
//   - in slash mode  : the user having literally typed the command
//
// PowerShell sits outside this system entirely.  Its safety boundary
// is command_policy.cpp's allowlist, which rejects disallowed commands
// before the approval gate is ever consulted.
enum class RiskTier {
    // Pure read-only or self-bounded helper.  No file modification, no
    // arbitrary code execution, no network reach.  Examples: read,
    // ls, grep, pwd, open, csv_inspect, xlsx_inspect, pdf_extract_text,
    // python_health.
    Safe,

    // Modifies workspace state (files, directories, generated reports)
    // but cannot escape the workspace and cannot directly create an
    // executable artifact.  write blocks risky extensions; edit only
    // touches existing files; csv_report/xlsx_report write a single
    // markdown file in the LlamaBoss Documents folder; mkdir creates
    // an empty directory; python_run_script runs a .py whose source
    // was already reviewed when python_create_script's approval card
    // was rendered or that lives as an optional helper in an active
    // project Workflows folder.
    Moderate,

    // Irreversible, or carries arbitrary code-execution risk that
    // benefits from explicit per-invocation review:
    //   - delete                : removes a file or empty directory; can't undo.
    //   - python_create_script  : writes a .py the user will then run; the
    //                             approval card body shows the script source
    //                             so the user can review before authorizing.
    Dangerous,
};

inline RiskTier ClassifyTier(const std::string& toolName)
{
    if (toolName == tool_names::kDelete ||
        toolName == tool_names::kPythonCreateScript ||
        toolName == tool_names::kPythonInstallPackage) {
        return RiskTier::Dangerous;
    }

    if (toolName == tool_names::kWrite              ||
        toolName == tool_names::kEdit               ||
        toolName == tool_names::kMkdir              ||
        toolName == tool_names::kCsvReport          ||
        toolName == tool_names::kXlsxReport         ||
        toolName == tool_names::kPythonRunScript) {
        return RiskTier::Moderate;
    }

    // Default to Safe for read-only tools and any future unrecognized
    // tool name.  Unknown tools fail at dispatch time anyway; gating
    // them here would surface a confusing approval card.
    return RiskTier::Safe;
}

// ═══════════════════════════════════════════════════════════════════

struct ApprovalDecision {
    bool        required = false;
    std::string reason;
    std::string target;
    std::string preview;
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
    if (inv.name == tool_names::kPythonHealth) return "python_health";
    if (inv.name == tool_names::kCsvInspect)
        return Trim(inv.args).empty() ? std::string("/csv_inspect")
                                      : ("/csv_inspect " + Trim(inv.args));
    if (inv.name == tool_names::kCsvReport)
        return Trim(inv.args).empty() ? std::string("/csv_report")
                                      : ("/csv_report " + Trim(inv.args));
    if (inv.name == tool_names::kXlsxInspect)
        return Trim(inv.args).empty() ? std::string("/xlsx_inspect")
                                      : ("/xlsx_inspect " + Trim(inv.args));
    if (inv.name == tool_names::kXlsxReport)
        return Trim(inv.args).empty() ? std::string("/xlsx_report")
                                      : ("/xlsx_report " + Trim(inv.args));
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
    if (inv.name == tool_names::kWrite || inv.name == tool_names::kEdit) {
        std::string first = FirstLine(inv.args);
        return first.empty() ? ("/" + inv.name) : ("/" + inv.name + " " + first);
    }
    return Trim(inv.args).empty() ? ("/" + inv.name)
                                  : ("/" + inv.name + " " + Trim(inv.args));
}

inline std::string ToolDisplayName(const std::string& name)
{
    if (name == tool_names::kWrite)      return "Write";
    if (name == tool_names::kEdit)       return "Edit";
    if (name == tool_names::kDelete)     return "Delete";
    if (name == tool_names::kMkdir)      return "Mkdir";
    if (name == tool_names::kPowerShell) return "PowerShell";
    if (name == tool_names::kPythonHealth) return "Python Health";
    if (name == tool_names::kCsvInspect) return "CSV Inspect";
    if (name == tool_names::kCsvReport)  return "CSV Report";
    if (name == tool_names::kXlsxInspect) return "XLSX Inspect";
    if (name == tool_names::kXlsxReport)  return "XLSX Report";
    if (name == tool_names::kPdfExtractText) return "PDF Extract Text";
    if (name == tool_names::kPythonCreateScript) return "Python Script";
    if (name == tool_names::kPythonRunScript) return "Create Files";
    if (name == tool_names::kPythonInstallPackage) return "Install Python Package";
    if (name == tool_names::kRead)       return "Read";
    if (name == tool_names::kLs)         return "List";
    if (name == tool_names::kGrep)       return "Grep";
    if (name == tool_names::kPwd)        return "Pwd";
    if (name == tool_names::kOpen)       return "Open";
    return name.empty() ? std::string("Tool") : name;
}

inline std::string ToolIcon(const std::string& name)
{
    if (name == tool_names::kWrite)      return "\xF0\x9F\x93\x9D"; // 📝
    if (name == tool_names::kEdit)       return "\xE2\x9C\x8F";     // ✏
    if (name == tool_names::kDelete)     return "\xF0\x9F\x97\x91"; // 🗑
    if (name == tool_names::kMkdir)      return "\xE2\x9E\x95";     // ➕
    if (name == tool_names::kPowerShell) return "\xE2\x9A\x99";     // ⚙
    if (name == tool_names::kPythonHealth) return "\xF0\x9F\x90\x8D"; // 🐍
    if (name == tool_names::kCsvInspect) return "\xF0\x9F\x93\x8A";   // 📊
    if (name == tool_names::kCsvReport)  return "\xF0\x9F\x93\x9D";   // 📝
    if (name == tool_names::kXlsxInspect) return "\xF0\x9F\x93\x8A";  // 📊
    if (name == tool_names::kXlsxReport)  return "\xF0\x9F\x93\x97";  // 📗
    if (name == tool_names::kPdfExtractText) return "\xF0\x9F\x93\x84"; // 📄
    if (name == tool_names::kPythonCreateScript) return "\xF0\x9F\x90\x8D"; // 🐍
    if (name == tool_names::kPythonRunScript) return "\xF0\x9F\x93\xA6"; // 📦
    if (name == tool_names::kPythonInstallPackage) return "\xF0\x9F\x90\x8D"; // 🐍
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
    std::string resolved = ResolveToolPath(trimmed, ctx.cwd);
    return resolved.empty() ? trimmed : resolved;
}

inline std::string ParentDirForApproval(std::string path)
{
    while (!path.empty() && (path.back() == '/' || path.back() == '\\')) path.pop_back();
    size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return std::string();
    return path.substr(0, pos);
}

inline std::string JoinPathForApproval(const std::string& a, const std::string& b)
{
    if (a.empty()) return b;
    if (a.back() == '/' || a.back() == '\\') return a + b;
    return a + "\\" + b;
}

inline std::string ScriptsDirForPreview()
{
    std::string root = ParentDirForApproval(ServerManager::GetDefaultWorkspaceDir());
    if (root.empty()) root = ParentDirForApproval(ServerManager::GetWorkspaceDir());
    return JoinPathForApproval(root, "Scripts");
}

inline std::string ScriptPreviewPath(const std::string& requested)
{
    std::string name = Trim(requested);
    if (name.empty()) return ScriptsDirForPreview();
    if (name.find('.') == std::string::npos) name += ".py";
    return JoinPathForApproval(ScriptsDirForPreview(), name);
}

inline std::string ProjectWorkflowScriptPreviewPath(const std::string& requested,
                                                    const ToolContext& ctx)
{
    std::string name = Trim(requested);
    if (name.empty()) return ScriptPreviewPath(requested);
    if (name.find('.') == std::string::npos) name += ".py";
    if (!ctx.activeProjectRoot.empty()) {
        return JoinPathForApproval(JoinPathForApproval(ctx.activeProjectRoot, "Workflows"), name);
    }
    return ScriptPreviewPath(requested);
}

inline std::string PreviewForInvocation(const ToolInvocation& inv,
                                        const ToolContext&    ctx,
                                        std::string&          targetOut)
{
    std::ostringstream p;

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
          << "Output: LlamaBoss Documents folder\n"
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
          << "Output: LlamaBoss Documents folder\n"
          << "Runs only the bundled xlsx_report helper. The helper reads an .xlsx file inside the current LlamaBoss working directory and creates one Markdown report artifact across all sheets. It does not accept arbitrary Python code, script paths, or output paths. Requires the openpyxl Python package.";
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
          << "This changes the user's Python environment and may download files from the Python Package Index. "
          << "The package name must be on LlamaBoss's allowlist; no versions, URLs, requirements files, extras, or pip flags are accepted.";
        return p.str();
    }

    if (inv.name == tool_names::kPythonCreateScript) {
        std::string filename, content;
        SplitWriteArgs(inv.args, filename, content);
        targetOut = ScriptPreviewPath(filename);
        p << "Target Python script: " << targetOut << "\n"
          << "Output: LlamaBoss Scripts folder\n"
          << "Bytes: " << content.size() << "\n"
          << "Lines: " << CountLines(content) << "\n\n"
          << "Creates a reviewable .py script artifact. If this task needs output, this approval also covers one immediate run of this exact script. Review the source below before approving.\n\n"
          << LimitText(content.empty() ? std::string("[empty script body]") : content);
        return p.str();
    }

    if (inv.name == tool_names::kPythonRunScript) {
        targetOut = ProjectWorkflowScriptPreviewPath(inv.args, ctx);
        p << "Target Python script: " << targetOut << "\n"
          << "Conversation Scripts fallback: " << ScriptPreviewPath(inv.args) << "\n"
          << "Working directory: " << ctx.cwd << "\n"
          << "Runs one existing .py script from the fixed conversation Scripts folder, or an optional .py helper script from the active project's Workflows folder. "
          << "Captures stdout, stderr, exit code, runtime, and attaches newly created files under the LlamaBoss root as artifact cards. No command-line arguments, package installs, or automatic sends in this phase.";
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
    if (inv.name == tool_names::kMkdir) return "create it";
    if (inv.name == tool_names::kEdit) return "edit it";
    if (inv.name == tool_names::kDelete) return "delete it";
    return "run it";
}

inline bool RequiresApproval(const ToolInvocation& inv,
                             const ToolContext&    ctx,
                             ApprovalDecision&     out)
{
    out = ApprovalDecision{};
    if (!inv.valid) return false;

    if (ClassifyTier(inv.name) != RiskTier::Dangerous) {
        // Safe and Moderate tools never render an approval card.
        // Conversational consent (agent mode) and explicit user
        // invocation (slash mode) carry the trust burden instead.
        // PowerShell never reaches this path; its allowlist in
        // command_policy.cpp is the hard boundary.
        return false;
    }

    // ── Dangerous tier — render an approval card ─────────────────
    if (inv.name == tool_names::kDelete) {
        out.required = true;
        out.reason = "This tool can permanently remove a file or empty directory. The action cannot be undone.";
    }
    else if (inv.name == tool_names::kPythonCreateScript) {
        out.required = true;
        out.reason = "Creates a reviewable Python script in the LlamaBoss Scripts folder. Review the source before approving.";
    }
    else if (inv.name == tool_names::kPythonInstallPackage) {
        out.required = true;
        out.reason = "This installs one allowlisted Python package into the user's Python user-site using pip. It changes the local Python environment and may use the network, so it requires approval.";
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
         << "  approve       Continue and trust tools for this chat.\n"
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
