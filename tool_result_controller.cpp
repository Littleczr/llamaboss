// tool_result_controller.cpp
// See tool_result_controller.h.  Handler bodies moved verbatim from MyFrame;
// the only changes are the dependency hops (m_agentController-> became
// m_agentController., SetStreamingState became the setStreamingState callback,
// m_isClosing became IsClosing(), and the local WxToUtf8 helper).
#include "tool_result_controller.h"

#include <wx/wx.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

#include "chat_display.h"
#include "chat_history.h"
#include "agent_controller.h"
#include "conversation_controller.h"

#include "cmd_executor.h"
#include "python_runner.h"
#include "tool_grep.h"
#include "tool_web_fetch.h"
#include "tool_invocation.h"   // tool_names::*
#include "tool_dispatcher.h"   // ToolInvocationResult (full definition)
#include "presented_file.h"
#include "artifact_presentation.h"
#include "python_package_recovery.h"

namespace {

std::string WxToUtf8(const wxString& s)
{
    wxScopedCharBuffer buf = s.ToUTF8();
    if (!buf) return std::string();
    return std::string(buf.data());
}

} // namespace

ToolResultController::ToolResultController(
    ChatDisplay*                  chatDisplay,
    std::unique_ptr<ChatHistory>& chatHistory,
    AgentController&              agentController,
    ConversationController&       convController)
    : m_chatDisplay(chatDisplay)
    , m_chatHistory(chatHistory)
    , m_agentController(agentController)
    , m_convController(convController)
{
}

bool ToolResultController::IsClosing() const
{
    return m_cb.isClosing && m_cb.isClosing();
}

void ToolResultController::FinishToolTurn()
{
    if (m_cb.setStreamingState) m_cb.setStreamingState(false);
    if (!m_chatHistory->IsEmpty())
        m_convController.AutoSaveConversation();
}

void ToolResultController::RenderAndPersistSlashResult(const ToolInvocationResult& r)
{
    ChatDisplay::ToolBlock tb;
    tb.iconUtf8     = r.iconUtf8;
    tb.toolName     = r.toolName;
    tb.statusChips  = r.chips;
    tb.commandEcho  = r.commandEcho;
    tb.body         = r.body;
    tb.errorBody    = r.errorBody;
    tb.bodyLang     = r.bodyLang;
    tb.presentedFiles = r.presentedFiles;
    m_chatDisplay->DisplayToolBlock(tb, /*startExpanded=*/true);

    std::string formatted = ChatHistory::FormatToolBlockAsUserMessage(
        r.toolTag,
        r.commandEcho,
        r.body,
        r.errorBody,
        r.chips,
        r.bodyLang,
        r.presentedFiles);
    m_chatHistory->AddUserMessage(formatted);
}


void ToolResultController::OnToolWorkerComplete(wxCommandEvent& evt)
{
    if (IsClosing()) return;

    auto* data = static_cast<ToolWorkerResultClientData*>(evt.GetClientObject());
    if (!data) {
        if (m_cb.setStreamingState) m_cb.setStreamingState(false);
        return;
    }

    const ToolWorkerResult& workerResult = data->GetResult();

    if (m_agentController.IsActive() &&
        m_agentController.HandleToolWorkerComplete(workerResult)) {
        return;
    }

    // Slash-command worker result.  The worker preserves the normal
    // Completed/Invalid ToolInvocationResult shape, so rendering and history
    // persistence stay identical to the old inline path.  File operations
    // are not generally interruptible after their commit point; when Stop was
    // requested, say so without discarding a result that may have succeeded.
    ToolInvocationResult result = workerResult.outcome.result;
    if (workerResult.cancelled &&
        std::find(result.chips.begin(), result.chips.end(), "stop requested") ==
            result.chips.end()) {
        result.chips.push_back("stop requested");
    }
    RenderAndPersistSlashResult(result);
    FinishToolTurn();
}

void ToolResultController::OnCmdComplete(wxCommandEvent& evt)
{
    if (IsClosing()) return;

    auto* data = static_cast<CmdResultClientData*>(evt.GetClientObject());
    if (!data) {
        if (m_cb.setStreamingState) m_cb.setStreamingState(false);
        return;
    }
    const CmdResult& r = data->GetResult();

    // ── Agent mode routing ───────────────────────────────────────
    // When the loop is active and was awaiting THIS result,
    // the controller builds the ToolBlock itself and fires
    // the next iteration — skip the normal user-/cmd path.
    // The controller's HandleCmdComplete returns false if the
    // pending invocation isn't a powershell call (defensive)
    // so we fall through and treat the event as a user-/cmd
    // completion.
    if (m_agentController.IsActive()) {
        bool consumed = m_agentController.HandleCmdComplete(r);
        if (consumed) return;
    }

    // ── Slash arm (Phase 4 unified) ──────────────────────────────
    // Build the same ToolInvocationResult shape the agent's
    // HandleCmdComplete builds, then render + persist via the
    // shared helper.  Chip ordering matches the agent path and
    // the saved-history tool-card layout — pre-Phase
    // 4 the on-screen ordering was [elapsed, status]; it's now
    // [status, elapsed] consistently across display and history.
    ToolInvocationResult tir;
    tir.toolTag       = tool_names::kPowerShell;
    tir.invocationRaw.clear();
    tir.iconUtf8      = "\xE2\x9A\x99";       // ⚙
    tir.toolName      = "PowerShell";
    tir.commandEcho   = r.command;
    tir.body          = r.stdoutText;
    tir.errorBody     = r.stderrText;
    tir.bodyLang      = "powershell";
    tir.presentedFiles = r.presentedFiles;

    // Status chip (cancelled / timed out / exit N) — mutually
    // exclusive — followed by elapsed, then truncated if any.
    if (r.cancelled) {
        tir.chips.push_back("cancelled");
    } else if (r.timedOut) {
        tir.chips.push_back("timed out");
    } else {
        std::ostringstream ec;
        ec << "exit " << r.exitCode;
        tir.chips.push_back(ec.str());
    }
    {
        std::ostringstream ts;
        ts << std::fixed;
        ts.precision(r.elapsedSec < 10.0 ? 2 : 1);
        ts << r.elapsedSec << "s";
        tir.chips.push_back(ts.str());
    }
    if (r.truncated) tir.chips.push_back("truncated");

    RenderAndPersistSlashResult(tir);

    FinishToolTurn();
}

void ToolResultController::OnCmdError(wxCommandEvent& evt)
{
    if (IsClosing()) return;

    const std::string error = WxToUtf8(evt.GetString());

    if (m_agentController.IsActive() &&
        m_agentController.HandleCmdError(error)) {
        return;
    }

    m_chatDisplay->DisplaySystemMessage("Command error: " + error);

    if (m_cb.setStreamingState) m_cb.setStreamingState(false);
    m_chatDisplay->ClearFilePersistenceContext();

    if (!m_chatHistory->IsEmpty())
        m_convController.AutoSaveConversation();
}

// ── controlled Python helper completion handler ──────────────────
void ToolResultController::OnPythonComplete(wxCommandEvent& evt)
{
    if (IsClosing()) return;

    auto* data = static_cast<PythonRunResultClientData*>(evt.GetClientObject());
    if (!data) {
        if (m_cb.setStreamingState) m_cb.setStreamingState(false);
        return;
    }
    const PythonRunResult& r = data->GetResult();

    auto TryReadSmallTextFile = [](const std::string& path,
                                   size_t             maxBytes,
                                   std::string&       out,
                                   size_t&            sizeOut) -> bool {
        out.clear();
        sizeOut = 0;

#ifdef __WXMSW__
        std::ifstream file(
            std::filesystem::path(wxString::FromUTF8(path).ToStdWstring()),
            std::ios::binary | std::ios::ate);
#else
        std::ifstream file(std::filesystem::path(path),
                           std::ios::binary | std::ios::ate);
#endif
        if (!file) return false;

        std::streampos end = file.tellg();
        if (end == std::streampos(-1)) return false;

        sizeOut = static_cast<size_t>(end);
        if (sizeOut > maxBytes) return false;

        file.seekg(0, std::ios::beg);
        std::ostringstream buffer;
        buffer << file.rdbuf();
        out = buffer.str();
        return true;
    };

    if (m_agentController.IsActive()) {
        bool consumed = m_agentController.HandlePythonComplete(r);
        if (consumed) return;
    }

    const bool isInspect = (r.toolName == tool_names::kCsvInspect ||
                            r.helperName == tool_names::kCsvInspect);
    const bool isReport  = (r.toolName == tool_names::kCsvReport ||
                            r.helperName == tool_names::kCsvReport);
    const bool isCsvToXlsx = (r.toolName == tool_names::kCsvToXlsx ||
                              r.helperName == tool_names::kCsvToXlsx);
    const bool isXlsxIns = (r.toolName == tool_names::kXlsxInspect ||
                            r.helperName == tool_names::kXlsxInspect);
    const bool isXlsxRep = (r.toolName == tool_names::kXlsxReport ||
                            r.helperName == tool_names::kXlsxReport);
    const bool isXlsxCreate = (r.toolName == tool_names::kXlsxCreateWorkbook ||
                               r.helperName == tool_names::kXlsxCreateWorkbook);
    const bool isPdf     = (r.toolName == tool_names::kPdfExtractText ||
                            r.helperName == tool_names::kPdfExtractText);
    const bool isPdfInspect = (r.toolName == tool_names::kPdfInspectForm ||
                               r.helperName == tool_names::kPdfInspectForm);
    const bool isPdfFill = (r.toolName == tool_names::kPdfFillForm ||
                            r.helperName == tool_names::kPdfFillForm);
    const bool isDocxExtract = (r.toolName == tool_names::kDocxExtractText ||
                                r.helperName == tool_names::kDocxExtractText);
    const bool isDocxInspect = (r.toolName == tool_names::kDocxInspect ||
                                r.helperName == tool_names::kDocxInspect);
    const bool isRun     = (r.toolName == tool_names::kPythonRunScript ||
                            r.helperName == tool_names::kPythonRunScript);
    const bool isInstall = (r.toolName == tool_names::kPythonInstallPackage ||
                            r.helperName == tool_names::kPythonInstallPackage);
    ToolInvocationResult tir;
    tir.toolTag       = isInstall ? std::string(tool_names::kPythonInstallPackage)
                     : isRun ? std::string(tool_names::kPythonRunScript)
                     : isPdf ? std::string(tool_names::kPdfExtractText)
                     : isPdfInspect ? std::string(tool_names::kPdfInspectForm)
                     : isPdfFill ? std::string(tool_names::kPdfFillForm)
                     : isDocxExtract ? std::string(tool_names::kDocxExtractText)
                     : isDocxInspect ? std::string(tool_names::kDocxInspect)
                     : isXlsxCreate ? std::string(tool_names::kXlsxCreateWorkbook)
                     : isCsvToXlsx ? std::string(tool_names::kCsvToXlsx)
                     : isXlsxRep ? std::string(tool_names::kXlsxReport)
                     : isXlsxIns ? std::string(tool_names::kXlsxInspect)
                     : isReport ? std::string(tool_names::kCsvReport)
                     : isInspect ? std::string(tool_names::kCsvInspect)
                                 : std::string(tool_names::kPythonHealth);
    tir.invocationRaw.clear();
    tir.iconUtf8      = (isPdf || isPdfInspect || isPdfFill ||
                           isDocxExtract || isDocxInspect) ? std::string("\xF0\x9F\x93\x84")  // 📄
                     : (isXlsxRep || isXlsxCreate || isCsvToXlsx) ? std::string("\xF0\x9F\x93\x97")       // 📗
                     : isReport ? std::string("\xF0\x9F\x93\x9D")        // 📝
                     : (isInspect || isXlsxIns) ? std::string("\xF0\x9F\x93\x8A") // 📊
                                 : std::string("\xF0\x9F\x90\x8D");      // 🐍
    tir.toolName      = isInstall ? std::string("Install Python Package")
                     : isRun ? std::string("Python Run")
                     : isPdf ? std::string("PDF Extract Text")
                     : isPdfInspect ? std::string("PDF Inspect Form")
                     : isPdfFill ? std::string("PDF Fill Form")
                     : isDocxExtract ? std::string("DOCX Extract Text")
                     : isDocxInspect ? std::string("DOCX Inspect")
                     : isXlsxCreate ? std::string("Create Workbook")
                     : isCsvToXlsx ? std::string("CSV to XLSX")
                     : isXlsxRep ? std::string("XLSX Report")
                     : isXlsxIns ? std::string("XLSX Inspect")
                     : isReport ? std::string("CSV Report")
                     : isInspect ? std::string("CSV Inspect")
                                 : std::string("Python Health");
    tir.commandEcho   = r.commandEcho.empty()
                            ? (isInstall ? std::string("python_install_package")
                              : isRun ? std::string("python_run_script")
                              : isPdf ? std::string("pdf_extract_text")
                              : isPdfInspect ? std::string("pdf_inspect_form")
                              : isPdfFill ? std::string("pdf_fill_form")
                              : isDocxExtract ? std::string("docx_extract_text")
                              : isDocxInspect ? std::string("docx_inspect")
                              : isXlsxCreate ? std::string("xlsx_create_workbook")
                              : isCsvToXlsx ? std::string("csv_to_xlsx")
                              : isXlsxRep ? std::string("xlsx_report")
                              : isXlsxIns ? std::string("xlsx_inspect")
                              : isReport ? std::string("csv_report")
                              : isInspect ? std::string("csv_inspect")
                                          : std::string("python_health"))
                            : r.commandEcho;
    tir.body          = r.stdoutText;
    tir.errorBody     = r.stderrText;
    tir.bodyLang      = (isRun || isInstall) ? std::string() : std::string("json");
    tir.presentedFiles = r.presentedFiles;
    if (isRun && r.exitCode == 0 && !r.cancelled && !r.timedOut) {
        ApplyArtifactPresentation(tir);
    }
    ApplyMissingPythonPackageRecovery(tir, r);

    if (r.cancelled) {
        tir.chips.push_back("cancelled");
    } else if (r.timedOut) {
        tir.chips.push_back("timed out");
    } else {
        std::ostringstream ec;
        ec << "exit " << r.exitCode;
        tir.chips.push_back(ec.str());
    }
    {
        std::ostringstream ts;
        ts << std::fixed;
        ts.precision(r.elapsedSec < 10.0 ? 2 : 1);
        ts << r.elapsedSec << "s";
        tir.chips.push_back(ts.str());
    }
    if (!r.pythonCommand.empty()) tir.chips.push_back(r.pythonCommand);
    if (r.truncated) tir.chips.push_back("truncated");

    // PDF UX polish: card-first policy.  The artifact chip is always
    // the primary affordance for tool-produced files; for small
    // extractions we ALSO surface the Markdown body inline as a
    // courtesy preview, but the [Open] / [Save As] / [Open Folder]
    // card stays.  Large extractions get a short explanation in the
    // body and rely on the card alone.  This also generalizes
    // cleanly to binary-output tools (e.g. pdf_fill_form) where
    // there is no inline-able body at all.
    if (isPdf && r.exitCode == 0 && !r.presentedFiles.empty()) {
        constexpr size_t kMaxInlinePdfMarkdownBytes = 32 * 1024;

        std::string inlineMarkdown;
        size_t inlineBytes = 0;
        const std::string artifactPath = r.presentedFiles.front().diskPath;

        if (TryReadSmallTextFile(artifactPath,
                                 kMaxInlinePdfMarkdownBytes,
                                 inlineMarkdown,
                                 inlineBytes)) {
            tir.body = inlineMarkdown;
            tir.errorBody.clear();
            tir.bodyLang = "markdown";
            // presentedFiles intentionally retained -- card is the
            // primary deliverable.  No "inline" chip; card + inline
            // body is now the default success shape.
        } else if (inlineBytes > kMaxInlinePdfMarkdownBytes) {
            std::ostringstream msg;
            msg << "PDF text was extracted successfully, but the extracted "
                   "Markdown is too large to show inline.\n\n"
                << "Extracted Markdown size: " << inlineBytes << " bytes\n"
                << "Inline display limit: " << kMaxInlinePdfMarkdownBytes
                << " bytes\n\n"
                << "Use the file chip above to save or open the extracted Markdown.";
            tir.body = msg.str();
            tir.errorBody.clear();
            tir.bodyLang.clear();
        }
    }

    RenderAndPersistSlashResult(tir);

    FinishToolTurn();
}

void ToolResultController::OnPythonError(wxCommandEvent& evt)
{
    if (IsClosing()) return;

    const std::string error = WxToUtf8(evt.GetString());

    if (m_agentController.IsActive() &&
        m_agentController.HandlePythonError(error)) {
        return;
    }

    m_chatDisplay->DisplaySystemMessage("Python runner error: " + error);

    if (m_cb.setStreamingState) m_cb.setStreamingState(false);
    m_chatDisplay->ClearFilePersistenceContext();

    if (!m_chatHistory->IsEmpty())
        m_convController.AutoSaveConversation();
}

// ── /grep completion handler (Phase 3) ───────────────────────────
// Worker posts this from the thread.  Pattern matches /cmd:
// unpack the client data, render, persist, reset UI state,
// auto-save if anything's in history.
void ToolResultController::OnGrepComplete(wxCommandEvent& evt)
{
    if (IsClosing()) return;

    auto* data = static_cast<GrepResultClientData*>(evt.GetClientObject());
    if (!data) {
        if (m_cb.setStreamingState) m_cb.setStreamingState(false);
        return;
    }
    const GrepResult& r = data->GetResult();

    // ── Agent mode routing ───────────────────────────────────────
    // When a loop is active and awaiting this result, the
    // controller builds the ToolBlock itself and fires the
    // next iteration — skip the normal path entirely.
    if (m_agentController.IsActive()) {
        bool consumed = m_agentController.HandleGrepComplete(r);
        if (consumed) return;
        // Fall through only if controller declined (shouldn't
        // happen in practice since grep only runs via agent or
        // /grep, never both simultaneously — but defensive).
    }

    // ── Slash arm (Phase 4 unified) ──────────────────────────────
    // Build a ToolInvocationResult from GrepResult, then render
    // + persist via the shared helper.  Same shape the agent's
    // HandleGrepComplete uses (minus the toolCallId threading).
    ToolInvocationResult tir;
    tir.toolTag       = tool_names::kGrep;
    tir.invocationRaw.clear();
    tir.iconUtf8      = "\xF0\x9F\x94\x8D";   // 🔍
    tir.toolName      = "Grep";
    tir.commandEcho   = r.commandEcho;
    tir.chips         = r.chips;
    tir.body          = r.body;
    tir.errorBody     = r.errorBody;
    tir.bodyLang      = r.bodyLang;

    RenderAndPersistSlashResult(tir);

    FinishToolTurn();
}

ToolInvocationResult
ToolResultController::MakeWebFetchToolInvocationResult(const WebFetchResult& r)
{
    ToolInvocationResult tir;
    tir.toolTag       = tool_names::kWebFetchUrl;
    tir.invocationRaw.clear();
    tir.iconUtf8      = "\xF0\x9F\x8C\x90";
    tir.toolName      = "Web Page Inspect";
    tir.commandEcho   = r.commandEcho;
    tir.chips         = r.chips;
    tir.body          = r.body;
    tir.errorBody     = r.errorBody;
    tir.bodyLang      = r.bodyLang;

    if (!r.textPath.empty()) {
        PresentedFile textFile;
        textFile.displayName = r.textDisplayName.empty()
            ? std::string("webpage_text.md")
            : r.textDisplayName;
        textFile.language  = "markdown";
        textFile.diskPath  = r.textPath;
        textFile.sizeBytes = r.textBytes;
        textFile.lineCount = r.textLineCount;
        tir.presentedFiles.push_back(std::move(textFile));
    }

    if (!r.rawHtmlPath.empty()) {
        PresentedFile htmlFile;
        htmlFile.displayName = r.rawHtmlDisplayName.empty()
            ? std::string("webpage_raw.html")
            : r.rawHtmlDisplayName;
        htmlFile.language  = "html";
        htmlFile.diskPath  = r.rawHtmlPath;
        htmlFile.sizeBytes = r.htmlBytes;
        htmlFile.lineCount = 0;
        tir.presentedFiles.push_back(std::move(htmlFile));
    }

    return tir;
}

void ToolResultController::OnWebFetchComplete(wxCommandEvent& evt)
{
    if (IsClosing()) return;

    auto* data = static_cast<WebFetchResultClientData*>(evt.GetClientObject());
    if (!data) {
        if (m_cb.setStreamingState) m_cb.setStreamingState(false);
        return;
    }
    const WebFetchResult& r = data->GetResult();

    if (m_agentController.IsActive()) {
        bool consumed = m_agentController.HandleWebFetchComplete(r);
        if (consumed) return;
    }

    ToolInvocationResult tir = MakeWebFetchToolInvocationResult(r);
    RenderAndPersistSlashResult(tir);

    FinishToolTurn();
}

void ToolResultController::OnWebFetchError(wxCommandEvent& evt)
{
    if (IsClosing()) return;

    const std::string error = WxToUtf8(evt.GetString());

    if (m_agentController.IsActive() &&
        m_agentController.HandleWebFetchError(error)) {
        return;
    }

    m_chatDisplay->DisplaySystemMessage("Web fetch error: " + error);

    if (m_cb.setStreamingState) m_cb.setStreamingState(false);
    m_chatDisplay->ClearFilePersistenceContext();

    if (!m_chatHistory->IsEmpty())
        m_convController.AutoSaveConversation();
}
