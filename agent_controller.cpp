// agent_controller.cpp
//
// Phase 9: typed AgentEvent envelope.  Keeps the Phase 5 sink
// architecture and Phase 6 approval state machine intact while routing
// controller emissions through a small structured event object.

// MSVC: silence wx's transitive use of strcpy/wcscpy/_wopen in
// wxcrt.h and filefn.h.  Phase 4 got this for free because the file
// included chat_display.h → <wx/wx.h> (the umbrella header has its
// own CRT-secure handling).  Phase 5 dropped that include — the
// controller is wx-free at the source level — so we now match the
// project's per-file convention used by LlamaBoss.cpp,
// chat_history.cpp, tool_dispatcher.cpp, etc.  Must come before any
// other include so the deprecation tagging is suppressed before
// wx headers get to <wxcrt.h>.
#define _CRT_SECURE_NO_WARNINGS

#include "agent_controller.h"
#include "python_runner.h"

#include "app_state.h"
#include "chat_history.h"
#include "cmd_executor.h"     // CmdResult definition
#include "server_manager.h"    // ModelDisplayName
#include "tool_call_parser.h"
#include "tool_grep.h"         // GrepResult definition
#include "tool_router.h"       // BuildToolsArrayJson, GetGlobalRouter
#include "tool_approval.h"     // Phase 6 approval cards

#include <Poco/JSON/Object.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Stringifier.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>


namespace {

// Phase 3 bugfix #1:
// Native function-calling responses may contain several tool_calls in
// one assistant turn.  LlamaBoss currently executes only one tool per
// agent iteration, so the history must also expose only the one tool_call
// that is actually being executed.  If the full tool_calls array is stored
// while only the first result is appended, the next OpenAI-style request is
// invalid: assistant.tool_calls contains A+B, but only role:"tool" for A
// exists.
//
// This helper keeps the native path conservative for Phase 3: one native
// tool call per assistant turn.  Later phases can replace this with true
// multi-call dispatch when the agent loop/event stream is ready for it.
std::string KeepOnlySelectedToolCallJson(const std::string& toolCallsJson,
                                         const std::string& selectedCallId)
{
    if (toolCallsJson.empty()) return std::string();

    try {
        Poco::JSON::Parser parser;
        auto var = parser.parse(toolCallsJson);
        Poco::JSON::Array::Ptr arr = var.extract<Poco::JSON::Array::Ptr>();
        if (!arr || arr->empty()) return std::string();

        Poco::JSON::Object::Ptr selected;

        // Prefer the exact id of the invocation we are about to dispatch.
        // This keeps the assistant.tool_calls id aligned with the later
        // role:"tool" message's tool_call_id.
        if (!selectedCallId.empty()) {
            for (size_t i = 0; i < arr->size(); ++i) {
                Poco::JSON::Object::Ptr obj;
                try { obj = arr->getObject(i); } catch (...) { continue; }
                if (!obj) continue;

                try {
                    if (obj->getValue<std::string>("id") == selectedCallId) {
                        selected = obj;
                        break;
                    }
                } catch (...) {
                    // Missing/non-string id. Keep scanning.
                }
            }
        }

        // Fallback: preserve the first object.  This should only matter for
        // malformed provider output where the id is missing, but it is safer
        // than re-attaching the full multi-call array.
        if (!selected) {
            try { selected = arr->getObject(0); } catch (...) { selected = nullptr; }
        }
        if (!selected) return std::string();

        Poco::JSON::Array one;
        one.add(selected);

        std::ostringstream oss;
        one.stringify(oss);
        return oss.str();
    } catch (...) {
        // Fail closed: returning empty avoids storing a full multi-call array
        // that would not match the single tool result we can currently append.
        return std::string();
    }
}




std::string AgentLowerAscii(std::string s)
{
    for (char& ch : s)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

std::string AgentPresentedFileExtLower(const PresentedFile& f)
{
    std::string name = !f.displayName.empty() ? f.displayName : f.diskPath;
    std::replace(name.begin(), name.end(), '\\', '/');
    size_t slash = name.find_last_of('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);

    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= name.size()) return std::string();
    return AgentLowerAscii(name.substr(dot + 1));
}

struct AgentArtifactPresentation {
    std::string iconUtf8;
    std::string toolName;
};

AgentArtifactPresentation BuildAgentArtifactPresentation(const std::vector<PresentedFile>& files)
{
    AgentArtifactPresentation p;
    if (files.empty()) return p;

    bool hasDocx = false;
    bool hasSheet = false;
    bool hasPdf = false;
    bool hasMarkdown = false;
    bool hasText = false;
    bool hasImage = false;
    bool hasOther = false;

    for (const auto& f : files) {
        const std::string ext = AgentPresentedFileExtLower(f);
        const std::string lang = AgentLowerAscii(f.language);

        if (ext == "docx") {
            hasDocx = true;
        } else if (ext == "xlsx" || ext == "csv") {
            hasSheet = true;
        } else if (ext == "pdf") {
            hasPdf = true;
        } else if (ext == "md" || ext == "markdown" || lang == "markdown" || lang == "md") {
            hasMarkdown = true;
        } else if (ext == "txt" || lang == "text") {
            hasText = true;
        } else if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "webp") {
            hasImage = true;
        } else {
            hasOther = true;
        }
    }

    const int kinds = (hasDocx ? 1 : 0) + (hasSheet ? 1 : 0) +
                      (hasPdf ? 1 : 0) + (hasMarkdown ? 1 : 0) +
                      (hasText ? 1 : 0) + (hasImage ? 1 : 0) +
                      (hasOther ? 1 : 0);

    if (files.size() == 1 && kinds == 1) {
        if (hasDocx)     return { "\xF0\x9F\x93\x84", "Create Word Document" };      // 📄
        if (hasSheet)    return { "\xF0\x9F\x93\x8A", "Create Spreadsheet" };        // 📊
        if (hasPdf)      return { "\xF0\x9F\x93\x84", "Create PDF" };                 // 📄
        if (hasMarkdown) return { "\xF0\x9F\x93\x9D", "Create Markdown Document" };  // 📝
        if (hasText)     return { "\xF0\x9F\x93\x84", "Create Text Document" };       // 📄
        if (hasImage)    return { "\xF0\x9F\x96\xBC", "Create Image" };               // 🖼
        return { "\xF0\x9F\x93\x8E", "Create File" };                                 // 📎
    }

    if (hasDocx && kinds == 1)     return { "\xF0\x9F\x93\x84", "Create Word Documents" };
    if (hasSheet && kinds == 1)    return { "\xF0\x9F\x93\x8A", "Create Spreadsheets" };
    if (hasPdf && kinds == 1)      return { "\xF0\x9F\x93\x84", "Create PDFs" };
    if (hasMarkdown && kinds == 1) return { "\xF0\x9F\x93\x9D", "Create Markdown Documents" };

    return { "\xF0\x9F\x93\xA6", "Create Files" };                                    // 📦
}

void ApplyAgentArtifactPresentation(ToolInvocationResult& r)
{
    if (r.presentedFiles.empty()) return;
    if (!r.errorBody.empty()) return;

    AgentArtifactPresentation p = BuildAgentArtifactPresentation(r.presentedFiles);
    if (p.toolName.empty()) return;

    r.iconUtf8 = p.iconUtf8;
    r.toolName = p.toolName;
}


std::string AgentTrimPackageToken(std::string s)
{
    size_t a = s.find_first_not_of(" \t\r\n\"'`.,:;()[]{}");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t\r\n\"'`.,:;()[]{}");
    return s.substr(a, b - a + 1);
}

bool AgentPackageIsAllowed(const std::string& packageName)
{
    static const char* kAllowed[] = {
        "python-docx", "openpyxl", "pymupdf", "pypdf", "pypdfium2",
        "pandas", "pillow", "reportlab", "matplotlib", "python-pptx",
        "xlsxwriter", "beautifulsoup4", "lxml"
    };
    for (const char* allowed : kAllowed) {
        if (packageName == allowed) return true;
    }
    return false;
}

std::string AgentNormalizeMissingPackageName(const std::string& raw)
{
    std::string p = AgentLowerAscii(AgentTrimPackageToken(raw));
    std::replace(p.begin(), p.end(), '_', '-');

    if (p == "docx") p = "python-docx";
    else if (p == "fitz") p = "pymupdf";
    else if (p == "pil") p = "pillow";
    else if (p == "pptx") p = "python-pptx";
    else if (p == "bs4") p = "beautifulsoup4";

    return p;
}

bool AgentExtractAfterToken(const std::string& text,
                         const std::string& token,
                         std::string& out)
{
    size_t pos = text.find(token);
    if (pos == std::string::npos) return false;
    pos += token.size();

    while (pos < text.size() &&
           (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\r' ||
            text[pos] == '\n' || text[pos] == '\'' || text[pos] == '"' ||
            text[pos] == '`')) {
        ++pos;
    }

    size_t end = pos;
    while (end < text.size()) {
        const char c = text[end];
        const bool ok = (c >= 'A' && c <= 'Z') ||
                        (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') ||
                        c == '_' || c == '-' || c == '.';
        if (!ok) break;
        ++end;
    }

    if (end <= pos) return false;
    out = text.substr(pos, end - pos);
    return !out.empty();
}

bool AgentFindMissingPythonPackage(const std::string& stdoutText,
                                const std::string& stderrText,
                                std::string&       importNameOut,
                                std::string&       packageNameOut,
                                bool&              allowlistedOut)
{
    importNameOut.clear();
    packageNameOut.clear();
    allowlistedOut = false;

    const std::string text = stderrText + "\n" + stdoutText;
    const std::string lower = AgentLowerAscii(text);

    std::string candidate;
    if (!AgentExtractAfterToken(text, "No module named", candidate) &&
        !AgentExtractAfterToken(text, "no module named", candidate)) {
        if (!AgentExtractAfterToken(lower, "pip install --user --disable-pip-version-check", candidate) &&
            !AgentExtractAfterToken(lower, "pip install --user", candidate)) {
            if (lower.find("openpyxl python package is required") != std::string::npos) {
                candidate = "openpyxl";
            } else if (lower.find("missing pdf form dependency") != std::string::npos ||
                       lower.find("install pymupdf") != std::string::npos) {
                candidate = "pymupdf";
            } else if (lower.find("missing pdf text extraction dependency") != std::string::npos ||
                       lower.find("install pypdf") != std::string::npos) {
                candidate = "pypdf";
            }
        }
    }

    candidate = AgentTrimPackageToken(candidate);
    if (candidate.empty()) return false;

    importNameOut = candidate;
    packageNameOut = AgentNormalizeMissingPackageName(candidate);
    allowlistedOut = AgentPackageIsAllowed(packageNameOut);
    return true;
}

void ApplyAgentMissingPythonPackageRecovery(ToolInvocationResult& r,
                                       const PythonRunResult& py)
{
    if (py.exitCode == 0 || py.cancelled || py.timedOut) return;

    std::string importName;
    std::string packageName;
    bool allowlisted = false;
    if (!AgentFindMissingPythonPackage(py.stdoutText,
                                    py.stderrText,
                                    importName,
                                    packageName,
                                    allowlisted)) {
        return;
    }

    r.iconUtf8 = "\xF0\x9F\x93\xA6"; // 📦
    r.toolName = allowlisted ? std::string("Missing Python Package")
                             : std::string("Unsupported Python Package");

    std::ostringstream body;
    if (allowlisted) {
        body << "Python needs the allowlisted package `" << packageName
             << "` before this step can continue.\n\n"
             << "Suggested next step for LlamaBoss: use `python_install_package "
             << packageName << "`, then retry the failed step once.\n\n"
             << "No package was installed yet.";
    } else {
        body << "Python tried to import `" << importName
             << "`, but that package is not on the current LlamaBoss install allowlist.\n\n"
             << "For safety, LlamaBoss will not install it automatically. The script may need to be rewritten using the standard library or an allowlisted package.";
    }

    if (!r.body.empty()) {
        body << "\n\nOriginal stdout:\n" << r.body;
    }

    r.body = body.str();
    r.bodyLang = "markdown";
}

bool TryReadSmallTextFile(const std::string& path,
                          size_t             maxBytes,
                          std::string&       out,
                          size_t&            sizeOut)
{
    out.clear();
    sizeOut = 0;

    std::ifstream file(path, std::ios::binary | std::ios::ate);
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
}

void InlineSmallPdfExtractedMarkdown(ToolInvocationResult& r)
{
    if (r.toolTag != tool_names::kPdfExtractText &&
        r.toolTag != tool_names::kDocxExtractText) return;
    if (!r.errorBody.empty()) return;
    if (r.presentedFiles.empty()) return;

    constexpr size_t kMaxInlinePdfMarkdownBytes = 32 * 1024;

    const PresentedFile& f = r.presentedFiles.front();
    if (f.diskPath.empty()) return;

    std::string inlineMarkdown;
    size_t inlineBytes = 0;
    if (TryReadSmallTextFile(f.diskPath,
                             kMaxInlinePdfMarkdownBytes,
                             inlineMarkdown,
                             inlineBytes)) {
        r.body = inlineMarkdown;
        r.errorBody.clear();
        r.bodyLang = "markdown";
    }
}



std::string NormalizeScriptNameForOneShotApproval(const std::string& input)
{
    std::string name = input;

    size_t a = name.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    size_t b = name.find_last_not_of(" \t\r\n");
    name = name.substr(a, b - a + 1);

    // python_run_script accepts filenames only, but artifact display names
    // are also filenames. Be defensive and collapse accidental paths to the
    // basename before comparing.
    size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);

    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) name += ".py";

    std::string out;
    out.reserve(name.size());
    for (char ch : name) {
        unsigned char c = static_cast<unsigned char>(ch);
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

std::string NormalizeForToolSignature(const std::string& input)
{
    std::string out;
    out.reserve(input.size());

    bool lastWasSpace = false;
    for (size_t i = 0; i < input.size(); ++i) {
        char ch = input[i];
        unsigned char c = static_cast<unsigned char>(ch);

        // Normalize CRLF and CR to LF so the same edit/write request
        // does not evade the guard just because line endings differ.
        if (ch == '\r') {
            if (i + 1 < input.size() && input[i + 1] == '\n')
                ++i;
            ch = '\n';
            c = '\n';
        }

        if (ch == '\n') {
            while (!out.empty() && (out.back() == ' ' || out.back() == '\t'))
                out.pop_back();
            if (out.empty() || out.back() != '\n')
                out.push_back('\n');
            lastWasSpace = false;
            continue;
        }

        if (std::isspace(c)) {
            if (!lastWasSpace) {
                out.push_back(' ');
                lastWasSpace = true;
            }
            continue;
        }

        out.push_back(ch);
        lastWasSpace = false;
    }

    // Trim outer whitespace/newlines after normalization.
    size_t a = out.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    size_t b = out.find_last_not_of(" \t\r\n");
    out = out.substr(a, b - a + 1);

    // Keep the ring buffer lightweight even if the model repeats a
    // long write/edit body.  The length suffix still distinguishes
    // differently-sized large calls.
    constexpr size_t kMaxSignatureArgChars = 4096;
    if (out.size() > kMaxSignatureArgChars) {
        out = out.substr(0, kMaxSignatureArgChars) +
              "\n...[signature truncated, original bytes=" +
              std::to_string(input.size()) + "]";
    }
    return out;
}


AgentEventType ClassifyToolOutputEvent(const ToolInvocationResult& r)
{
    auto hasChip = [&](const std::string& needle) -> bool {
        return std::find(r.chips.begin(), r.chips.end(), needle) != r.chips.end();
    };

    if (r.toolTag == "agent_status")
        return AgentEventType::AgentStatus;

    if (r.toolTag == tool_names::kWrite && hasChip("created"))
        return AgentEventType::FileCreated;

    if (r.toolTag == tool_names::kEdit && hasChip("edited"))
        return AgentEventType::EditApplied;

    if (r.toolTag == tool_names::kMkdir && hasChip("created"))
        return AgentEventType::DirectoryCreated;

    if (r.toolTag == tool_names::kDelete && hasChip("deleted"))
        return AgentEventType::FileDeleted;

    if (!r.errorBody.empty() && r.body.empty())
        return AgentEventType::Error;

    return AgentEventType::ToolOutput;
}

} // namespace

// ═══════════════════════════════════════════════════════════════════
//  Construction
// ═══════════════════════════════════════════════════════════════════

AgentController::AgentController(std::unique_ptr<ChatHistory>& history,
                                 AgentEventSink* sink,
                                 AppState*       appState,
                                 GrepExecutor*   grepExec,
                                 CmdExecutor*    cmdExec,
                                 PythonRunner*   pythonRunner)
    : m_history(history)
    , m_sink(sink)
    , m_appState(appState)
    , m_grepExec(grepExec)
    , m_cmdExec(cmdExec)
    , m_pythonRunner(pythonRunner)
{}

// ═══════════════════════════════════════════════════════════════════
//  Lifecycle
// ═══════════════════════════════════════════════════════════════════

void AgentController::Begin()
{
    m_active               = true;
    m_cancelled            = false;
    m_iterationsUsed       = 0;   // incremented when we receive iteration 1's reply
    m_consecutiveMalformed = 0;
    m_awaitingAsyncResult    = false;
    m_pendingAsyncInvocation = ToolInvocation{};
    m_awaitingApproval       = false;
    m_pendingApprovalInvocation = ToolInvocation{};
    m_pendingApprovalContext    = ToolContext{};
    m_currentToolCallId.clear();
    m_recentToolSignatures.clear();
    // Note: m_oneShotApprovedScriptRun is intentionally NOT cleared here.
    // It survives a Normal EndLoop (model paused with "Want me to run it?"
    // prose and ended the turn) so the user's natural "yes" reply lands
    // on a still-armed bypass instead of forcing a second /approve.
    // Abnormal exits (Cancel, LoopGuard, errors) clear it inside EndLoop.
    // Single-use consumption clears it inside the gate.

    // Phase 5: signal loop start so the frame (or future P9 parent)
    // can install loop-scoped UI state.  Currently a no-op in
    // MyFrame's implementation — the user message and first request
    // are already on screen by the time Begin() runs — but the seam
    // exists for future hooks.
    if (m_sink) m_sink->OnAgentEvent(AgentEvent::LoopBegin());
}

void AgentController::Cancel()
{
    if (!m_active) return;
    m_cancelled = true;

    // Agent-owned async tools run while MyFrame's coarse state may still
    // look like Streaming.  Cancel them here so the Stop button reaches
    // the actual worker instead of only stopping a chat stream.
    if (m_awaitingAsyncResult) {
        if (m_pendingAsyncInvocation.name == tool_names::kGrep && m_grepExec) {
            m_grepExec->Cancel();
        }
        else if (m_pendingAsyncInvocation.name == tool_names::kPowerShell && m_cmdExec) {
            m_cmdExec->Cancel();
        }
        else if ((m_pendingAsyncInvocation.name == tool_names::kPythonHealth ||
                  m_pendingAsyncInvocation.name == tool_names::kCsvInspect ||
                  m_pendingAsyncInvocation.name == tool_names::kCsvReport ||
                  m_pendingAsyncInvocation.name == tool_names::kXlsxInspect ||
                  m_pendingAsyncInvocation.name == tool_names::kXlsxReport ||
                  m_pendingAsyncInvocation.name == tool_names::kPdfExtractText ||
                  m_pendingAsyncInvocation.name == tool_names::kPdfInspectForm ||
                  m_pendingAsyncInvocation.name == tool_names::kPdfFillForm ||
                  m_pendingAsyncInvocation.name == tool_names::kDocxExtractText ||
                  m_pendingAsyncInvocation.name == tool_names::kDocxInspect ||
                  m_pendingAsyncInvocation.name == tool_names::kPythonRunScript ||
                  m_pendingAsyncInvocation.name == tool_names::kPythonInstallPackage) && m_pythonRunner) {
            m_pythonRunner->Cancel();
        }
    }
}

void AgentController::EndLoop(AgentEndReason     reason,
                              const std::string& userFacingMessage)
{
    m_active                 = false;
    m_awaitingAsyncResult    = false;
    m_pendingAsyncInvocation = ToolInvocation{};
    m_awaitingApproval       = false;
    m_pendingApprovalInvocation = ToolInvocation{};
    m_pendingApprovalContext    = ToolContext{};
    m_currentToolCallId.clear();
    m_recentToolSignatures.clear();
    // Bundled-approval bypass survives a Normal exit (model paused with
    // "Want me to run it?" prose).  Abnormal exits — user cancelled,
    // loop guard tripped, malformed cap, iteration cap, send/stream
    // errors — wipe it because the user's intent context is gone.
    if (reason != AgentEndReason::Normal) {
        m_oneShotApprovedScriptRun.clear();
    }

    // Phase 5: single sink call replaces the Phase-4 pair of
    // (DisplaySystemMessage + onLoopEnd callback).  The frame's
    // implementation surfaces the message via DisplaySystemMessage
    // when non-empty and runs its standard finalization sequence.
    if (m_sink) m_sink->OnAgentEvent(AgentEvent::LoopEnd(reason, userFacingMessage));
}

// Phase 5: pack a ToolInvocationResult into a ToolBlock and send it
// to the sink.  Replaces four near-duplicate inline blocks that
// constructed a ChatDisplay::ToolBlock and called DisplayToolBlock
// directly — sync dispatch (FeedResultAndIterate), grep cancel
// rendering, cmd cancel rendering, and malformed-cap error
// rendering.
void AgentController::EmitToolBlock(const ToolInvocationResult& r,
                                    bool startExpanded)
{
    if (!m_sink) return;
    ToolBlock tb;
    tb.iconUtf8    = r.iconUtf8;
    tb.toolName    = r.toolName;
    tb.statusChips = r.chips;
    tb.commandEcho = r.commandEcho;
    tb.body        = r.body;
    tb.errorBody   = r.errorBody;
    tb.bodyLang    = r.bodyLang;
    tb.presentedFiles = r.presentedFiles;
    m_sink->OnAgentEvent(AgentEvent::ToolOutput(tb, startExpanded, ClassifyToolOutputEvent(r)));
}


void AgentController::EmitAndStoreTerminalToolResult(const ToolInvocationResult& r,
                                                     bool startExpanded)
{
    EmitToolBlock(r, startExpanded);

    std::string formatted = ChatHistory::FormatToolBlockAsUserMessage(
        r.toolTag, r.commandEcho, r.body, r.errorBody, r.chips, r.bodyLang);
    m_history->AddToolResultMessage(m_currentToolCallId, formatted);
    m_currentToolCallId.clear();
}

void AgentController::EmitPendingToolBlock(const ToolInvocation& inv)
{
    if (!m_sink) return;

    ToolBlock tb;
    tb.iconUtf8    = tool_approval::ToolIcon(inv.name);
    tb.toolName    = tool_approval::ToolDisplayName(inv.name);
    tb.statusChips = { "pending" };
    tb.commandEcho = tool_approval::CommandEcho(inv);

    // Keep this intentionally body-less. The completed ToolBlock will carry
    // stdout/stderr/artifacts. This card is just a lightweight acknowledgement
    // that the approved async tool has started, so the user is not left staring
    // at a blank assistant turn. It is UI-only and is not added to chat history.
    m_sink->OnAgentEvent(AgentEvent::ToolOutput(
        tb, /*expanded=*/false, AgentEventType::AgentStatus));
}

void AgentController::EmitAndStoreAgentStatusCard(
    const std::string& title,
    const std::vector<std::string>& chips,
    const std::string& message,
    bool startExpanded)
{
    ToolInvocationResult r;
    r.toolTag     = "agent_status";
    r.iconUtf8    = "\xE2\x9A\xA0";  // ⚠
    r.toolName    = title.empty() ? "Agent Status" : title;
    r.commandEcho = "agent loop";
    r.chips       = chips;
    r.errorBody   = message;

    EmitAndStoreTerminalToolResult(r, startExpanded);
}

std::string AgentController::BuildToolSignature(const ToolInvocation& inv) const
{
    std::string name = inv.name;
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });

    return name + "|" + NormalizeForToolSignature(inv.args);
}

bool AgentController::WouldTripLoopGuard(const ToolInvocation& inv,
                                         std::string&          signatureOut,
                                         int&                  repeatCountOut) const
{
    signatureOut = BuildToolSignature(inv);
    repeatCountOut = 1;  // include the candidate invocation

    if (signatureOut.empty()) return false;

    const size_t keepBeforeCandidate =
        (kLoopGuardWindow > 0) ? static_cast<size_t>(kLoopGuardWindow - 1) : 0;
    const size_t n = m_recentToolSignatures.size();
    const size_t start = (n > keepBeforeCandidate) ? (n - keepBeforeCandidate) : 0;

    for (size_t i = start; i < n; ++i) {
        if (m_recentToolSignatures[i] == signatureOut)
            ++repeatCountOut;
    }

    return repeatCountOut >= kLoopGuardRepeatThreshold;
}

void AgentController::RecordToolSignature(const std::string& signature)
{
    if (signature.empty()) return;

    m_recentToolSignatures.push_back(signature);
    const size_t maxKeep = static_cast<size_t>(
        std::max(kLoopGuardWindow, kLoopGuardRepeatThreshold));
    while (m_recentToolSignatures.size() > maxKeep)
        m_recentToolSignatures.erase(m_recentToolSignatures.begin());
}

void AgentController::EmitToolCallEvent(const ToolInvocation& inv)
{
    if (!m_sink) return;

    m_sink->OnAgentEvent(AgentEvent::ToolCall(
        inv.name,
        tool_approval::CommandEcho(inv),
        inv.toolCallId));
}


// ═══════════════════════════════════════════════════════════════════
//  Request building
// ═══════════════════════════════════════════════════════════════════

std::string AgentController::BuildRequestBody()
{
    // ChatHistory::BuildChatRequestJson accepts an optional system
    // prompt as its third arg.  Poco handles proper JSON escaping
    // (including UTF-8 multi-byte sequences) — don't reinvent it.
    //
    // Fourth arg is the context-size hint in tokens; when provided,
    // the builder elides old tool-result bodies if the request would
    // exceed ~70% of the budget.  We grab the live ctx size from
    // AppState so Settings changes mid-loop take effect on the next
    // iteration without needing to restart anything.
    //
    // Phase 3c-i: fifth arg is an optional tools-array JSON string.
    // We attach it only when the active model has been confirmed
    // (via Phase 3b detection) to support native function calling.
    // On Xml/Unknown protocol we leave the field empty and the
    // builder produces the historical XML-only request shape.
    std::string model     = m_appState->GetModel();
    std::string sysPrompt = m_cb.buildSystemPrompt
                              ? m_cb.buildSystemPrompt()
                              : std::string();
    int ctxTokens = m_appState->GetCtxSize();
    if (ctxTokens <= 0) ctxTokens = 8192;  // defensive fallback

    std::string tools;
    bool native = false;
    if (m_cb.getActiveProtocol &&
        m_cb.getActiveProtocol() == ToolProtocol::Native) {
        tools = BuildToolsArrayJson(GetGlobalRouter());
        native = true;
    }

    return m_history->BuildChatRequestJson(model, /*stream*/ true,
                                           sysPrompt, ctxTokens,
                                           tools, native);
}

// ═══════════════════════════════════════════════════════════════════
//  Dispatch + iteration
// ═══════════════════════════════════════════════════════════════════

bool AgentController::ApprovePendingTool(bool rememberForChat)
{
    if (!m_active || !m_awaitingApproval) return false;

    ToolInvocation inv = m_pendingApprovalInvocation;
    ToolContext    ctx = m_pendingApprovalContext;

    m_awaitingApproval = false;
    m_pendingApprovalInvocation = ToolInvocation{};
    m_pendingApprovalContext    = ToolContext{};

    // Mark approval memory BEFORE dispatch so an immediate follow-up
    // tool call in the same loop can skip another card.  "Approve
    // always" now means one-approval mode for this conversation, not
    // only this individual tool name.
    if (rememberForChat && m_history) {
        m_history->RememberAllToolApprovalsForChat();
    }

    return DispatchApprovedAndContinue(inv, ctx);
}

bool AgentController::DenyPendingTool()
{
    if (!m_active || !m_awaitingApproval) return false;

    ToolInvocation inv = m_pendingApprovalInvocation;
    m_awaitingApproval = false;
    m_pendingApprovalInvocation = ToolInvocation{};
    m_pendingApprovalContext    = ToolContext{};

    ToolInvocationResult r = tool_approval::DeniedResult(
        inv, "Denied by user. Tool was not executed.");
    FeedResultAndIterate(r);
    return true;
}

bool AgentController::CancelPendingApproval()
{
    if (!m_active || !m_awaitingApproval) return false;

    ToolInvocation inv = m_pendingApprovalInvocation;
    m_awaitingApproval = false;
    m_pendingApprovalInvocation = ToolInvocation{};
    m_pendingApprovalContext    = ToolContext{};

    ToolInvocationResult r = tool_approval::DeniedResult(
        inv, "Cancelled by user before approval. Tool was not executed.");
    EmitAndStoreTerminalToolResult(r, true);

    EndLoop(AgentEndReason::Cancelled, "Agent stopped by user.");
    return true;
}

bool AgentController::DispatchAndContinue(const ToolInvocation& inv)
{
    // Phase 3c-ii: thread the call id through to FeedResultAndIterate
    // so the eventual tool-result message can carry the right
    // tool_call_id.  Empty for XML-protocol invocations and for
    // malformed/missing invocations — both are stored as plain user
    // messages exactly as before.
    m_currentToolCallId = inv.toolCallId;

    if (!inv.valid) {
        // Malformed / unknown tool.  Feed the reason back to the
        // model as an error result — it may self-correct.  Counter
        // ticks up; if we exceed the cap, EndLoop.
        ++m_consecutiveMalformed;

        ToolInvocationResult r;
        r.toolTag       = inv.name.empty() ? "tool" : inv.name;
        r.invocationRaw = inv.rawBlock;
        r.toolName      = inv.name.empty() ? "Tool" : inv.name;
        r.commandEcho   = inv.rawBlock.empty()
            ? std::string("<malformed tool_call>") : inv.rawBlock;
        r.errorBody     = inv.invalidReason.empty()
            ? "Invalid tool invocation."
            : inv.invalidReason;
        r.iconUtf8      = "\xE2\x9A\xA0";  // ⚠
        r.chips         = { "error" };

        if (m_consecutiveMalformed >= kMaxMalformedPerTurn) {
            // Render the error so the user sees WHY we bailed,
            // then stop.  No iteration follow-up.
            EmitToolBlock(r);

            EndLoop(AgentEndReason::MalformedCap,
                    "Agent stopped: " +
                    std::to_string(kMaxMalformedPerTurn) +
                    " malformed tool calls in a row.");
            return false;
        }

        // Feed the error back and let the model try again.
        FeedResultAndIterate(r);
        return true;
    }

    // Valid invocation — reset malformed counter (we saw progress).
    m_consecutiveMalformed = 0;

    // Phase 9: typed, non-rendered event for observers/tests/future
    // sub-agent forwarders.  The existing UI ignores ToolCall by
    // default, so this adds structure without adding visible noise.
    EmitToolCallEvent(inv);

    // Phase 7: controlled multi-step loop guard.  Stop before
    // dispatch if the model is about to repeat the same exact tool
    // call too many times in a small rolling window.
    std::string signature;
    int repeatCount = 0;
    if (WouldTripLoopGuard(inv, signature, repeatCount)) {
        ToolInvocationResult r;
        r.toolTag       = inv.name.empty() ? "tool" : inv.name;
        r.invocationRaw = inv.rawBlock;
        r.iconUtf8      = "\xE2\x9A\xA0";  // ⚠
        r.toolName      = "Loop Guard";
        r.commandEcho   = tool_approval::CommandEcho(inv);
        r.chips         = { "blocked", "loop guard" };
        r.errorBody     =
            "Agent stopped before dispatch: the same tool call was requested " +
            std::to_string(repeatCount) +
            " times within the recent loop window. Tool was not executed.";

        EmitAndStoreTerminalToolResult(r, true);
        EndLoop(AgentEndReason::LoopGuard,
                "Agent stopped: repeated the same tool call.");
        return false;
    }
    RecordToolSignature(signature);

    ToolContext ctx = m_cb.buildToolContext ? m_cb.buildToolContext()
                                              : ToolContext{};

    // Phase 6 follow-up: per-chat remembered approvals.  If the user
    // has previously approved this tool in this conversation with the
    // "always" variant, skip the approval card entirely and dispatch
    // directly.  Read-only tools never enter the gate to begin with.
    const bool alreadyApproved =
        m_history && m_history->IsToolChatApproved(inv.name);

    bool oneShotScriptRunApproved = false;
    if (inv.name == tool_names::kPythonRunScript &&
        !m_oneShotApprovedScriptRun.empty()) {
        std::string requested = NormalizeScriptNameForOneShotApproval(inv.args);
        if (!requested.empty()) {
            for (const auto& candidate : m_oneShotApprovedScriptRun) {
                if (requested == candidate) {
                    oneShotScriptRunApproved = true;
                    break;
                }
            }
        }

        // The carry-forward approval is single-use. If the model asks to run
        // a different script, fall through to the normal approval card and
        // do not keep a stale bypass around for later.
        m_oneShotApprovedScriptRun.clear();
    }

    tool_approval::ApprovalDecision approval;
    if (!alreadyApproved &&
        !oneShotScriptRunApproved &&
        tool_approval::RequiresApproval(inv, ctx, approval)) {
        m_awaitingApproval = true;
        m_pendingApprovalInvocation = inv;
        m_pendingApprovalContext    = ctx;
        if (m_sink) m_sink->OnAgentEvent(AgentEvent::ApprovalRequired(approval.block));
        return true;
    }

    return DispatchApprovedAndContinue(inv, ctx);
}

bool AgentController::DispatchApprovedAndContinue(const ToolInvocation& inv,
                                                  const ToolContext&    ctx)
{
    DispatchOutcome out = DispatchInvocation(inv, ctx, m_grepExec, m_cmdExec, m_pythonRunner);

    switch (out.status) {
        case DispatchStatus::Completed:
            if (inv.name == tool_names::kPythonCreateScript &&
                out.result.errorBody.empty() &&
                !out.result.presentedFiles.empty()) {
                m_oneShotApprovedScriptRun.clear();

                // Grant the one-shot run bypass ONLY for the final on-disk
                // artifact name returned by python_create_script.
                //
                // Important safety edge case:
                //   requested: report.py
                //   existing : report.py
                //   created  : report_2.py
                //
                // If we also approved the originally-requested name, the next
                // python_run_script report.py could bypass approval and run the
                // older script instead of the just-reviewed artifact.  Collision
                // renamed scripts therefore require the model to run the final
                // displayed artifact name, or the normal approval card appears.
                std::string finalName = NormalizeScriptNameForOneShotApproval(
                    out.result.presentedFiles.front().displayName);
                if (!finalName.empty())
                    m_oneShotApprovedScriptRun.push_back(finalName);
            }
            FeedResultAndIterate(out.result);
            return true;

        case DispatchStatus::Invalid: {
            // Dispatch-level failure (e.g. grep path resolution).
            // Render, feed back, continue loop — do NOT bump the
            // malformed counter since the INVOCATION was valid;
            // only the runtime state was wrong.
            FeedResultAndIterate(out.result);
            return true;
        }

        case DispatchStatus::Async: {
            // Worker running. Stash the invocation for when the completion
            // event arrives, then show a lightweight pending card immediately.
            // Without this, approvals / natural "yes run it" follow-ups can
            // look like a blank assistant turn until the worker finishes.
            m_awaitingAsyncResult    = true;
            m_pendingAsyncInvocation = inv;
            EmitPendingToolBlock(inv);
            return true;
        }
    }
    return false;
}


void AgentController::FeedResultAndIterate(const ToolInvocationResult& rIn)
{
    ToolInvocationResult r = rIn;
    InlineSmallPdfExtractedMarkdown(r);

    // Phase 5: emit a ToolBlock event instead of pushing directly
    // to ChatDisplay.  MyFrame's sink implementation forwards to
    // DisplayToolBlock unchanged; future P6 approval cards will
    // intercept this seam to gate dangerous tool results.
    EmitToolBlock(r);

    // Round-trip to history.  Uses the exact same format as a
    // user-typed invocation — so from the model's POV, its own
    // calls look identical to user calls on the next turn.  That's
    // deliberate: uniform history means uniform behavior.
    //
    // Phase 3c-ii: AddToolResultMessage attaches the tool_call_id
    // sidecar (if any) so the next request — under native protocol
    // — can emit a proper role:"tool" reply threaded to the
    // assistant call.  XML-protocol invocations have empty ids,
    // and AddToolResultMessage degrades to AddUserMessage in that
    // case, preserving Phase 1/2 behaviour.
    std::string formatted = ChatHistory::FormatToolBlockAsUserMessage(
        r.toolTag, r.commandEcho, r.body, r.errorBody, r.chips, r.bodyLang);
    m_history->AddToolResultMessage(m_currentToolCallId, formatted);

    // Consumed — clear so the next iteration's malformed-error
    // path or sync dispatch doesn't accidentally re-use it.
    m_currentToolCallId.clear();

    // Iteration-cap check BEFORE kicking off the next request.
    ++m_iterationsUsed;
    if (m_iterationsUsed >= kMaxIterations) {
        const std::string msg =
            "Agent stopped after " +
            std::to_string(kMaxIterations) +
            " tool step(s). This is a safety cap to prevent runaway loops. "
            "Ask the model to continue if you want more inspection.";

        EmitAndStoreAgentStatusCard(
            "Agent Status",
            { "stopped", "tool cap", std::to_string(kMaxIterations) + " steps" },
            msg,
            true);

        EndLoop(AgentEndReason::IterationCap, msg);
        return;
    }
    if (m_cancelled) {
        EndLoop(AgentEndReason::Cancelled, "Agent stopped by user.");
        return;
    }

    // Fire next iteration: add a fresh assistant placeholder for
    // the upcoming streamed reply, prepare the UI, send request.
    //
    // Phase 5: OnAgentIterationBegin replaces the old
    // beginNextIteration callback.  The frame uses it to reset
    // streaming state, render the assistant prefix, and re-arm the
    // streaming flag.
    std::string model = m_appState->GetModel();
    m_history->AddAssistantPlaceholder(model);

    unsigned long genId = m_cb.bumpGenerationId ? m_cb.bumpGenerationId() : 0;
    if (m_sink) m_sink->OnAgentEvent(AgentEvent::IterationBegin());

    std::string body = BuildRequestBody();
    if (!m_cb.sendRequest || !m_cb.sendRequest(model, body, genId)) {
        m_history->RemoveLastAssistantMessage();
        EndLoop(AgentEndReason::SendFailed,
                "Agent stopped: failed to send next request.");
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Event handlers
// ═══════════════════════════════════════════════════════════════════

bool AgentController::HandleAssistantComplete(const std::string& fullResponse,
                                                const std::string& toolCallsJson)
{
    if (!m_active) return false;

    if (m_cancelled) {
        EndLoop(AgentEndReason::Cancelled, "Agent stopped by user.");
        return true;
    }

    // ── Phase 3c-ii: native protocol path ───────────────────────
    // When the active model is on the native tool-calling protocol
    // AND the streaming layer extracted at least one structured
    // tool_call, we bypass the XML parser entirely.  The model's
    // call(s) arrive as a JSON array of {id, type, function:
    // {name, arguments}} objects; we synthesize ToolInvocations
    // (carrying the call ids), store the assistant message with
    // its tool_calls sidecar so the next request can thread
    // role:"tool" replies, and dispatch through the router exactly
    // the same way XML invocations dispatch.
    //
    // Phase 3 bugfix #1: keep native dispatch one-call-at-a-time.
    // If the model emits multiple native tool_calls in one assistant
    // turn, execute only the first parsed invocation AND persist only
    // that matching tool_call sidecar.  Storing the full array while
    // appending one result would create an invalid OpenAI transcript:
    // assistant.tool_calls = A+B, but role:"tool" result only for A.
    const bool nativeActive =
        m_cb.getActiveProtocol &&
        m_cb.getActiveProtocol() == ToolProtocol::Native;

    if (nativeActive && !toolCallsJson.empty()) {
        std::vector<ToolInvocation> invocations =
            ParseStructuredToolCalls(toolCallsJson);

        if (!invocations.empty()) {
            // Conservative Phase 3 rule: execute exactly one native
            // tool call per assistant turn.  Most importantly, persist
            // only the same tool_call that we are about to dispatch.
            // That guarantees the next request has a valid pair:
            //   assistant.tool_calls[0].id == role:"tool".tool_call_id
            ToolInvocation first = invocations.front();
            std::string oneToolCallJson =
                KeepOnlySelectedToolCallJson(toolCallsJson, first.toolCallId);

            if (!oneToolCallJson.empty()) {
                m_history->SetLastAssistantToolCalls(oneToolCallJson);
            } else {
                // Defensive fallback: if we could not safely attach the
                // assistant.tool_calls sidecar, do not add the upcoming
                // result as role:"tool".  Let AddToolResultMessage degrade
                // to the legacy user-message format instead; that is valid
                // for both XML and native requests, while an orphan tool
                // message is not.
                first.toolCallId.clear();
            }

            bool cont = DispatchAndContinue(first);
            return cont;
        }

        // Model emitted toolCallsJson but parser produced nothing —
        // malformed structured output.  Fall through to XML parse
        // attempt as a safety net (shouldn't happen with conformant
        // servers, but the fallback costs us nothing).
    }

    // ── XML protocol path (Phase 1/2/3a/3b/3c-i unchanged) ──────
    ParsedAssistantResponse parsed = ParseAssistantResponse(fullResponse);

    // Malformed-only reply (has blocks but all unparseable).
    // Treat this as if we received one invalid invocation — the
    // counter-based stop rule kicks in.
    if (!parsed.hasInvocation && !parsed.malformed.empty()) {
        ToolInvocation bad;
        bad.valid         = false;
        bad.rawBlock      = parsed.malformed.front().rawText;
        bad.invalidReason = parsed.malformed.front().reason;
        bool cont = DispatchAndContinue(bad);
        return cont;  // true if loop continuing; false if we bailed
    }

    // No tool call at all — this is the model's final answer.
    // MyFrame already rendered the streamed prose; we just exit
    // the loop and let normal completion flow through.
    if (!parsed.hasInvocation) {
        // Normal end of loop — no user message needed.
        EndLoop(AgentEndReason::Normal, "");
        return false;   // let MyFrame do normal completion handling
    }

    // Has a tool call.  The model's reply may have included prose
    // before the <tool_call>.  That prose was already streamed to
    // the display via DisplayAssistantDelta; we only need to clean
    // up history — strip the <tool_call> block from the stored
    // assistant message so it reads as prose-only.
    if (m_history->HasAssistantPlaceholder() ||
        m_history->GetMessageCount() > 0) {
        // Replace the last assistant message (which was just
        // finalized by MyFrame's OnAssistantComplete before this
        // call) with the prose portion only.  If prose is empty,
        // the message effectively disappears — that's fine,
        // reduces noise.
        m_history->UpdateLastAssistantMessage(parsed.prose);
    }

    // If prose is empty AND the model emitted only a tool call,
    // remove the now-empty assistant message so history stays tidy.
    // (An empty assistant message before a tool-result user message
    // confuses chat templates on some models.)
    if (parsed.prose.empty()) {
        m_history->RemoveLastAssistantMessage();
    }

    bool cont = DispatchAndContinue(parsed.invocation);
    return cont;
}

// Phase 3c-ii: parse the structured tool_calls JSON from the
// streaming response into ToolInvocations.  Each entry's
// `function.arguments` is a JSON-encoded string per OpenAI spec;
// we project that into our existing args-string contract by
// extracting the *single argument* shape that LlamaBoss's tool
// schemas use (each tool exposes one or two parameters that we
// flatten to the conventional space-separated form expected by
// the existing dispatchers).
//
// On any malformed entry we skip it rather than abort the whole
// batch — partial dispatch is better than total failure.
std::vector<ToolInvocation> AgentController::ParseStructuredToolCalls(
    const std::string& toolCallsJson)
{
    std::vector<ToolInvocation> out;
    if (toolCallsJson.empty()) return out;

    Poco::JSON::Array::Ptr arr;
    try {
        Poco::JSON::Parser parser;
        auto var = parser.parse(toolCallsJson);
        arr = var.extract<Poco::JSON::Array::Ptr>();
    } catch (...) {
        return out;
    }
    if (!arr) return out;

    for (size_t i = 0; i < arr->size(); ++i) {
        Poco::JSON::Object::Ptr entry;
        try { entry = arr->getObject(i); } catch (...) { continue; }
        if (!entry) continue;

        ToolInvocation inv;
        try { inv.toolCallId = entry->getValue<std::string>("id"); }
        catch (...) { /* leave empty */ }

        Poco::JSON::Object::Ptr fn;
        try { fn = entry->getObject("function"); } catch (...) { continue; }
        if (!fn) continue;

        try { inv.name = fn->getValue<std::string>("name"); }
        catch (...) { /* leave empty */ }

        // arguments is a JSON-encoded string.  We parse it as a
        // small JSON object and project to the legacy args-string
        // shape that ValidateToolArgs and the dispatchers expect.
        std::string argsRaw;
        try { argsRaw = fn->getValue<std::string>("arguments"); }
        catch (...) { argsRaw.clear(); }

        inv.args     = ProjectStructuredArgs(inv.name, argsRaw);
        inv.rawBlock = "[native tool_call] " + inv.name + "(" + argsRaw + ")";

        // Validate now so DispatchAndContinue can route the same
        // way malformed XML invocations route.
        std::string reason;
        if (inv.name.empty()) {
            inv.valid         = false;
            inv.invalidReason = "tool call missing function name";
        } else if (!ValidateToolArgs(inv.name, inv.args, reason)) {
            inv.valid         = false;
            inv.invalidReason = reason;
        } else {
            inv.valid = true;
        }

        out.push_back(std::move(inv));
    }

    return out;
}

// Project the OpenAI structured arguments (a JSON object inside a
// stringified payload) onto LlamaBoss's legacy args-string form.
//
// Most tools' wire schemas declare a single string parameter named
// "args" carrying the same legacy args body the XML protocol
// produced, and the dispatchers reuse their existing per-tool
// parsers without a structured-args branch.  pwd is the lone
// no-args exception: empty string always.
//
// Phase 3c-iii: write and edit moved to structured shapes.
//   write : {path, content}              → "path\ncontent"
//   edit  : {path, old_str, new_str}     → "path\n<<<OLD>>>\nold\n<<<NEW>>>\nnew"
// The flattened result feeds the existing SplitPathAndContent /
// ParseEditArgs parsers verbatim, so the tool implementations stay
// unchanged.  Backward-compat: if a model emits the old {args}
// shape for write or edit, the trailing fallback at the bottom
// picks it up and the dispatchers run as before.
//
// On parse failure we fall through to the raw string; most
// dispatchers tolerate that for single-arg tools and the
// validator catches anything truly malformed.
std::string AgentController::ProjectStructuredArgs(
    const std::string& toolName,
    const std::string& argsJson)
{
    if (toolName == tool_names::kPwd) return std::string();

    Poco::JSON::Object::Ptr obj;
    try {
        Poco::JSON::Parser parser;
        auto var = parser.parse(argsJson.empty() ? std::string("{}") : argsJson);
        obj = var.extract<Poco::JSON::Object::Ptr>();
    } catch (...) {
        return argsJson;   // best-effort fallback
    }
    if (!obj) return argsJson;

    // Small helper: pull a string field, return empty on absence
    // or type mismatch (rather than throw) so the projection
    // stays best-effort.
    auto getStr = [&](const std::string& key) -> std::string {
        if (!obj->has(key)) return std::string();
        try { return obj->getValue<std::string>(key); }
        catch (...) { return std::string(); }
    };

    // ── python_create_script: {filename, content} → "filename\n" + content
    // This creates a reviewable .py artifact in the Scripts lane.
    // It does not execute the script.
    if (toolName == tool_names::kPythonCreateScript && obj->has("filename")) {
        std::string filename = getStr("filename");
        std::string content  = getStr("content");
        if (!filename.empty()) {
            return filename + "\n" + content;
        }
    }

    // ── write: {path, content} → "path\n" + content ─────────────
    // SplitPathAndContent reads the first line as the path and
    // everything after the first '\n' as the content body, so the
    // simple concatenation reproduces the legacy shape exactly.
    // Missing content → empty string → empty-file write.  We only
    // engage this branch if `path` is present; otherwise we fall
    // through so a model still emitting the old {args} shape gets
    // handled as a backward-compat case.
    if (toolName == tool_names::kWrite && obj->has("path")) {
        std::string path    = getStr("path");
        std::string content = getStr("content");
        if (!path.empty()) {
            return path + "\n" + content;
        }
    }

    // ── edit: {path, old_str, new_str} → sentinel form ──────────
    // ParseEditArgs requires <<<OLD>>> and <<<NEW>>> each on their
    // own line.  We surround them with '\n' on both sides; a missing
    // trailing newline on new_str is fine — ParseEditArgs accepts
    // either shape.  Empty new_str produces a delete-OLD operation
    // (allowed).  Empty old_str makes ParseEditArgs reject with a
    // useful "<<<OLD>>> block is empty" message — the model gets
    // clear feedback instead of silent misbehavior.
    if (toolName == tool_names::kEdit && obj->has("path")) {
        std::string path = getStr("path");
        std::string oldS = getStr("old_str");
        std::string newS = getStr("new_str");
        if (!path.empty()) {
            return path + "\n<<<OLD>>>\n" + oldS +
                   "\n<<<NEW>>>\n" + newS;
        }
    }

    // ── Single-string tools, plus backward-compat fallback ──────
    // Every other tool (read, ls, grep, open, mkdir, delete,
    // powershell) declares a single "args" string property and
    // the dispatcher's per-tool parser consumes it.  This branch
    // is also the fallback for write/edit when a model emits
    // the pre-Phase-3c-iii {args} shape — saved-history replay,
    // older models, or transient regressions all land here and
    // continue to work.
    if (obj->has("args")) {
        try { return obj->getValue<std::string>("args"); }
        catch (...) { /* fall through */ }
    }
    return std::string();
}


bool AgentController::HandleGrepComplete(const GrepResult& grepResult)
{
    if (!m_active || !m_awaitingAsyncResult) return false;

    // Defensive: only consume the event if THIS controller fired
    // the grep call.  Without this check, a stale /grep result
    // could be mis-attributed while another async tool is pending.
    if (m_pendingAsyncInvocation.name != tool_names::kGrep)
        return false;

    m_awaitingAsyncResult = false;
    ToolInvocation inv = m_pendingAsyncInvocation;
    m_pendingAsyncInvocation = ToolInvocation{};

    if (m_cancelled || grepResult.cancelled) {
        // Still render the (likely-partial) result so the user
        // sees what was found before cancel.
        ToolInvocationResult r;
        r.toolTag       = tool_names::kGrep;
        r.invocationRaw = inv.rawBlock;
        r.iconUtf8      = "\xF0\x9F\x94\x8D";
        r.toolName      = "Grep";
        r.commandEcho   = grepResult.commandEcho;
        r.chips         = grepResult.chips;
        r.body          = grepResult.body;
        r.errorBody     = grepResult.errorBody;
        r.bodyLang      = grepResult.bodyLang;

        EmitToolBlock(r);

        std::string formatted = ChatHistory::FormatToolBlockAsUserMessage(
            r.toolTag, r.commandEcho, r.body, r.errorBody, r.chips, r.bodyLang);
        m_history->AddToolResultMessage(inv.toolCallId, formatted);

        EndLoop(AgentEndReason::Cancelled, "Agent stopped by user.");
        return true;
    }

    // Build the result struct and continue the loop normally.
    ToolInvocationResult r;
    r.toolTag       = tool_names::kGrep;
    r.invocationRaw = inv.rawBlock;
    r.iconUtf8      = "\xF0\x9F\x94\x8D";
    r.toolName      = "Grep";
    r.commandEcho   = grepResult.commandEcho;
    r.chips         = grepResult.chips;
    r.body          = grepResult.body;
    r.errorBody     = grepResult.errorBody;
    r.bodyLang      = grepResult.bodyLang;

    FeedResultAndIterate(r);
    return true;
}

// ─── powershell completion ──────────────────────────────────────
//
// Mirrors HandleGrepComplete.  The chip composition matches
// the slash /cmd renderer so the agent-rendered ToolBlock and the
// history round-trip carry the same status / elapsed / truncated
// markers a user-typed /cmd would.
//
// Cancel handling: if either the loop was cancelled OR the
// executor reports cancelled, render whatever output we have, feed
// it back to history (so the model's transcript reflects what
// actually happened), and end the loop.
bool AgentController::HandleCmdComplete(const CmdResult& cmdResult)
{
    if (!m_active || !m_awaitingAsyncResult) return false;

    // Defensive: only consume the event if THIS controller fired
    // the call.  Without this check, a /cmd run started before the
    // loop began (and still in flight) could be mis-attributed.
    if (m_pendingAsyncInvocation.name != tool_names::kPowerShell)
        return false;

    m_awaitingAsyncResult = false;
    ToolInvocation inv = m_pendingAsyncInvocation;
    m_pendingAsyncInvocation = ToolInvocation{};

    // Build chips identical to the slash /cmd result layout.
    std::vector<std::string> chips;
    if (cmdResult.cancelled) {
        chips.push_back("cancelled");
    } else if (cmdResult.timedOut) {
        chips.push_back("timed out");
    } else {
        std::ostringstream ec;
        ec << "exit " << cmdResult.exitCode;
        chips.push_back(ec.str());
    }
    {
        std::ostringstream ts;
        ts << std::fixed;
        ts.precision(cmdResult.elapsedSec < 10.0 ? 2 : 1);
        ts << cmdResult.elapsedSec << "s";
        chips.push_back(ts.str());
    }
    if (cmdResult.truncated) chips.push_back("truncated");

    ToolInvocationResult r;
    r.toolTag       = tool_names::kPowerShell;
    r.invocationRaw = inv.rawBlock;
    r.iconUtf8      = "\xE2\x9A\x99";       // ⚙
    r.toolName      = "PowerShell";
    r.commandEcho   = cmdResult.command;
    r.chips         = chips;
    r.body          = cmdResult.stdoutText;
    r.errorBody     = cmdResult.stderrText;
    r.bodyLang      = "powershell";
    r.presentedFiles = cmdResult.presentedFiles;

    if (m_cancelled || cmdResult.cancelled) {
        // Render the (possibly partial) result so the user sees
        // what came back before the cancel landed.
        EmitToolBlock(r);

        std::string formatted = ChatHistory::FormatToolBlockAsUserMessage(
            r.toolTag, r.commandEcho, r.body, r.errorBody, r.chips, r.bodyLang);
        m_history->AddToolResultMessage(inv.toolCallId, formatted);

        EndLoop(AgentEndReason::Cancelled, "Agent stopped by user.");
        return true;
    }

    FeedResultAndIterate(r);
    return true;
}

// ─── controlled Python helper completion ─────────────────────────
// Mirrors HandleCmdComplete, but for fixed Python helper tools.
bool AgentController::HandlePythonComplete(const PythonRunResult& pythonResult)
{
    if (!m_active || !m_awaitingAsyncResult) return false;

    if (m_pendingAsyncInvocation.name != tool_names::kPythonHealth &&
        m_pendingAsyncInvocation.name != tool_names::kCsvInspect &&
        m_pendingAsyncInvocation.name != tool_names::kCsvReport &&
        m_pendingAsyncInvocation.name != tool_names::kXlsxInspect &&
        m_pendingAsyncInvocation.name != tool_names::kXlsxReport &&
        m_pendingAsyncInvocation.name != tool_names::kPdfExtractText &&
        m_pendingAsyncInvocation.name != tool_names::kPdfInspectForm &&
        m_pendingAsyncInvocation.name != tool_names::kPdfFillForm &&
        m_pendingAsyncInvocation.name != tool_names::kDocxExtractText &&
        m_pendingAsyncInvocation.name != tool_names::kDocxInspect &&
        m_pendingAsyncInvocation.name != tool_names::kPythonRunScript &&
        m_pendingAsyncInvocation.name != tool_names::kPythonInstallPackage)
        return false;

    m_awaitingAsyncResult = false;
    ToolInvocation inv = m_pendingAsyncInvocation;
    m_pendingAsyncInvocation = ToolInvocation{};

    std::vector<std::string> chips;
    if (pythonResult.cancelled) {
        chips.push_back("cancelled");
    } else if (pythonResult.timedOut) {
        chips.push_back("timed out");
    } else {
        std::ostringstream ec;
        ec << "exit " << pythonResult.exitCode;
        chips.push_back(ec.str());
    }
    {
        std::ostringstream ts;
        ts << std::fixed;
        ts.precision(pythonResult.elapsedSec < 10.0 ? 2 : 1);
        ts << pythonResult.elapsedSec << "s";
        chips.push_back(ts.str());
    }
    if (!pythonResult.pythonCommand.empty())
        chips.push_back(pythonResult.pythonCommand);
    if (pythonResult.truncated) chips.push_back("truncated");

    const bool isInspect = (inv.name == tool_names::kCsvInspect);
    const bool isReport  = (inv.name == tool_names::kCsvReport);
    const bool isXlsxIns = (inv.name == tool_names::kXlsxInspect);
    const bool isXlsxRep = (inv.name == tool_names::kXlsxReport);
    const bool isPdf     = (inv.name == tool_names::kPdfExtractText);
    const bool isPdfInspect = (inv.name == tool_names::kPdfInspectForm);
    const bool isPdfFill = (inv.name == tool_names::kPdfFillForm);
    const bool isDocx    = (inv.name == tool_names::kDocxExtractText);
    const bool isDocxIns = (inv.name == tool_names::kDocxInspect);
    const bool isRun     = (inv.name == tool_names::kPythonRunScript);
    const bool isInstall = (inv.name == tool_names::kPythonInstallPackage);

    ToolInvocationResult r;
    r.toolTag       = inv.name;
    r.invocationRaw = inv.rawBlock;
    r.iconUtf8      = (isPdf || isPdfInspect || isPdfFill ||
                       isDocx || isDocxIns)        ? std::string("\xF0\x9F\x93\x84")  // 📄
                    : isXlsxRep ? std::string("\xF0\x9F\x93\x97")      // 📗
                    : isReport ? std::string("\xF0\x9F\x93\x9D")       // 📝
                    : (isInspect || isXlsxIns) ? std::string("\xF0\x9F\x93\x8A") // 📊
                                : std::string("\xF0\x9F\x90\x8D");     // 🐍
    r.toolName      = isInstall ? std::string("Install Python Package")
                    : isRun ? std::string("Python Run")
                    : isPdf ? std::string("PDF Extract Text")
                    : isPdfInspect ? std::string("PDF Inspect Form")
                    : isPdfFill ? std::string("PDF Fill Form")
                    : isDocx ? std::string("DOCX Extract Text")
                    : isDocxIns ? std::string("DOCX Inspect")
                    : isXlsxRep ? std::string("XLSX Report")
                    : isXlsxIns ? std::string("XLSX Inspect")
                    : isReport ? std::string("CSV Report")
                    : isInspect ? std::string("CSV Inspect")
                                : std::string("Python Health");
    r.commandEcho   = pythonResult.commandEcho.empty()
                        ? (isInstall ? std::string("python_install_package")
                          : isRun ? std::string("python_run_script")
                          : isPdf ? std::string("pdf_extract_text")
                          : isPdfInspect ? std::string("pdf_inspect_form")
                          : isPdfFill ? std::string("pdf_fill_form")
                          : isDocx ? std::string("docx_extract_text")
                          : isDocxIns ? std::string("docx_inspect")
                          : isXlsxRep ? std::string("xlsx_report")
                          : isXlsxIns ? std::string("xlsx_inspect")
                          : isReport ? std::string("csv_report")
                          : isInspect ? std::string("csv_inspect")
                                      : std::string("python_health"))
                        : pythonResult.commandEcho;
    r.chips         = chips;
    r.body          = pythonResult.stdoutText;
    r.errorBody     = pythonResult.stderrText;
    r.bodyLang      = (isRun || isInstall) ? std::string() : std::string("json");
    r.presentedFiles = pythonResult.presentedFiles;
    if (isRun && pythonResult.exitCode == 0 && !pythonResult.cancelled && !pythonResult.timedOut) {
        ApplyAgentArtifactPresentation(r);
    }
    ApplyAgentMissingPythonPackageRecovery(r, pythonResult);

    if (m_cancelled || pythonResult.cancelled) {
        EmitToolBlock(r);

        std::string formatted = ChatHistory::FormatToolBlockAsUserMessage(
            r.toolTag, r.commandEcho, r.body, r.errorBody, r.chips, r.bodyLang);
        m_history->AddToolResultMessage(inv.toolCallId, formatted);

        EndLoop(AgentEndReason::Cancelled, "Agent stopped by user.");
        return true;
    }

    FeedResultAndIterate(r);
    return true;
}

bool AgentController::HandleAssistantError(const std::string& /*errorText*/)
{
    if (!m_active) return false;
    // Errors always end the loop.  Let MyFrame's existing error
    // handler render the friendly message; we just reset state.
    EndLoop(AgentEndReason::StreamError, "");  // no extra message
    return false;
}
