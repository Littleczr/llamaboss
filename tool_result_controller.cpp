// tool_result_controller.cpp
// See tool_result_controller.h.  Handler bodies moved verbatim from MyFrame;
// the only changes are the dependency hops (m_agentController-> became
// m_agentController., SetStreamingState became the setStreamingState callback,
// m_isClosing became IsClosing(), and the local WxToUtf8 helper).
#include "tool_result_controller.h"

#include "chat_display.h"
#include "chat_history.h"
#include "agent_controller.h"
#include "conversation_controller.h"

#include "cmd_executor.h"
#include "wait_executor.h"
#include "python_runner.h"
#include "python_session.h"
#include "tool_grep.h"
#include "tool_web_fetch.h"
#include "tool_invocation.h"   // tool_names::*
#include "tool_dispatcher.h"   // ToolInvocationResult (full definition)
#include "tool_router.h"       // registry-backed helper presentation metadata
#include "presented_file.h"
#include "var_store.h"

namespace {

std::string WxToUtf8(const wxString& s)
{
    wxScopedCharBuffer buf = s.ToUTF8();
    if (!buf) return std::string();
    return std::string(buf.data());
}

varstore::DemotionConfig DemotionConfigForResult(
    const ToolInvocationResult& r)
{
    varstore::DemotionConfig cfg;
    if (r.historyInlineBudgetBytes > 0)
        cfg.thresholdBytes = r.historyInlineBudgetBytes;
    return cfg;
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

void ToolResultController::DropUnclaimedResult(const std::string& toolName)
{
    // Typed commands are synchronous, so every async result here belongs to
    // an agent turn.  One the loop did not claim (it ended, or was waiting
    // on a different tool) is not persisted as a free-standing message.
    const ToolSpec* spec = GetGlobalRouter().Find(toolName);
    const std::string label = spec && !spec->displayName.empty()
        ? spec->displayName
        : (toolName.empty() ? std::string("A tool") : toolName);
    m_chatDisplay->DisplaySystemMessage(
        label + " finished after its agent turn ended; the result was not "
        "added to the chat.");

    // A still-active loop owns the busy state; leave it alone.
    if (m_agentController.IsActive()) return;
    FinishToolTurn();
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

    // RLM Phase A: the history copy of a large body is demoted to a
    // Vars\ handle card; the ToolBlock rendered above keeps the full
    // body (display is not context).
    std::string historyBody = r.body;
    {
        std::string cwd = m_chatHistory->GetToolCwd();
        if (cwd.empty()) {
            const std::string fp = m_chatHistory->GetFilePath();
            if (!fp.empty())
                cwd = ChatHistory::GetConversationWorkspaceDir(fp);
        }
        varstore::DemoteOutcome d = varstore::MaybeDemoteToolBody(
            r.toolTag, r.commandEcho, r.body, cwd,
            DemotionConfigForResult(r));
        if (d.demoted) historyBody = d.cardBody;
    }

    std::string formatted = ChatHistory::FormatToolBlockAsUserMessage(
        r.toolTag,
        r.commandEcho,
        historyBody,
        r.errorBody,
        r.chips,
        r.bodyLang,
        r.presentedFiles);
    m_chatHistory->AddUserMessage(formatted);
    // /view_image: the pixels ride as a sidecar, projected with the
    // user's next message (see ChatHistory's tool-image carrier).
    m_chatHistory->SetLastMessageToolImages(r.viewImages);
}


void ToolResultController::OnToolWorkerComplete(wxCommandEvent& evt)
{
    // Payload ownership.  wxCommandEvent does not delete its client
    // object (see the contract in cmd_executor.h).  Claim it before ANY
    // early return -- IsClosing(), null-data, and the agent-consumed
    // path below are all reached on normal runs, so a leak here fires on
    // every tool completion, not just at shutdown.  payloadOwner outlives
    // every reference taken out of it in this function.
    std::unique_ptr<wxClientData> payloadOwner(evt.GetClientObject());
    evt.SetClientObject(nullptr);

    if (IsClosing()) return;

    auto* data = static_cast<ToolWorkerResultClientData*>(payloadOwner.get());
    if (!data) {
        if (m_cb.setStreamingState) m_cb.setStreamingState(false);
        return;
    }

    const ToolWorkerResult& workerResult = data->GetResult();

    if (m_agentController.IsActive() &&
        m_agentController.HandleToolWorkerComplete(workerResult)) {
        return;
    }

    DropUnclaimedResult(workerResult.toolName);
}

void ToolResultController::OnWaitComplete(wxCommandEvent& evt)
{
    // Payload ownership -- claim before any early return.  Same
    // contract as CmdResultClientData (see cmd_executor.h).
    std::unique_ptr<wxClientData> payloadOwner(evt.GetClientObject());
    evt.SetClientObject(nullptr);

    if (IsClosing()) return;

    auto* data = static_cast<WaitResultClientData*>(payloadOwner.get());
    if (!data) {
        if (m_cb.setStreamingState) m_cb.setStreamingState(false);
        return;
    }
    const WaitResult& r = data->GetResult();

    // wait is agent-loop-owned; there is no slash arm.  When the loop
    // is active and awaiting THIS wait, the controller feeds the
    // result and continues (or ends on cancel).  Anything else is a
    // stale event from a frame teardown race -- just release the
    // busy chrome defensively.
    if (m_agentController.IsActive() &&
        m_agentController.HandleWaitComplete(r)) {
        return;
    }

    if (m_cb.setStreamingState) m_cb.setStreamingState(false);
}

void ToolResultController::OnCmdComplete(wxCommandEvent& evt)
{
    // Payload ownership -- claim before any early return.  See the
    // contract comment on CmdResultClientData in cmd_executor.h.
    std::unique_ptr<wxClientData> payloadOwner(evt.GetClientObject());
    evt.SetClientObject(nullptr);

    if (IsClosing()) return;

    auto* data = static_cast<CmdResultClientData*>(payloadOwner.get());
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

    DropUnclaimedResult(tool_names::kPowerShell);
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
    // Payload ownership -- claim before any early return.  Note this
    // handler also receives the synchronous python_install_package
    // rejection event, which is posted from PythonRunner directly
    // rather than from a worker thread; same contract applies.
    std::unique_ptr<wxClientData> payloadOwner(evt.GetClientObject());
    evt.SetClientObject(nullptr);

    if (IsClosing()) return;

    auto* data = static_cast<PythonRunResultClientData*>(payloadOwner.get());
    if (!data) {
        if (m_cb.setStreamingState) m_cb.setStreamingState(false);
        return;
    }
    const PythonRunResult& r = data->GetResult();

    if (m_agentController.IsActive()) {
        bool consumed = m_agentController.HandlePythonComplete(r);
        if (consumed) return;
    }

    DropUnclaimedResult(!r.helperName.empty() ? r.helperName : r.toolName);
}

// ── persistent Python session completion (RLM step 2, S2) ────────
// Same shape as OnPythonComplete: claim payload, bail if closing,
// offer the result to the agent loop.
void ToolResultController::OnPySessionComplete(wxCommandEvent& evt)
{
    // Payload ownership -- claim before any early return.
    std::unique_ptr<wxClientData> payloadOwner(evt.GetClientObject());
    evt.SetClientObject(nullptr);

    if (IsClosing()) return;

    auto* data = static_cast<PySessionResultClientData*>(payloadOwner.get());
    if (!data) {
        if (m_cb.setStreamingState) m_cb.setStreamingState(false);
        return;
    }
    const PySessionResult& r = data->GetResult();

    if (m_agentController.IsActive()) {
        bool consumed = m_agentController.HandlePySessionComplete(r);
        if (consumed) return;
    }

    DropUnclaimedResult(tool_names::kPy);
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

// ── grep completion handler ──────────────────────────────────────
// Worker posts this from the thread.  Pattern matches PowerShell:
// unpack the client data, render, persist, reset UI state,
// auto-save if anything's in history.
void ToolResultController::OnGrepComplete(wxCommandEvent& evt)
{
    // Payload ownership -- claim before any early return.
    std::unique_ptr<wxClientData> payloadOwner(evt.GetClientObject());
    evt.SetClientObject(nullptr);

    if (IsClosing()) return;

    auto* data = static_cast<GrepResultClientData*>(payloadOwner.get());
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
    }

    DropUnclaimedResult(tool_names::kGrep);
}

void ToolResultController::OnWebFetchComplete(wxCommandEvent& evt)
{
    // Payload ownership -- claim before any early return.  WebFetchResult
    // carries the extracted markdown body, so the agent-consumed path is
    // the expensive one to leak.
    std::unique_ptr<wxClientData> payloadOwner(evt.GetClientObject());
    evt.SetClientObject(nullptr);

    if (IsClosing()) return;

    auto* data = static_cast<WebFetchResultClientData*>(payloadOwner.get());
    if (!data) {
        if (m_cb.setStreamingState) m_cb.setStreamingState(false);
        return;
    }
    const WebFetchResult& r = data->GetResult();

    if (m_agentController.IsActive()) {
        bool consumed = m_agentController.HandleWebFetchComplete(r);
        if (consumed) return;
    }

    DropUnclaimedResult(tool_names::kWebFetchUrl);
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
