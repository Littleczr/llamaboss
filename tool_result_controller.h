// tool_result_controller.h
// Owns completion/error handling for specialized async executors
// (PowerShell/cmd, Python helpers, grep, web-fetch) plus the generic
// ToolWorkerExecutor used by blocking synchronous file tools.  Extracted from MyFrame
// to keep the frame thin.
//
// Each async executor posts a wxCommandEvent to the frame when its worker
// finishes; the frame binds those events directly to this controller.  The
// handlers all share one shape: bail if closing, unpack the result client
// data, hand off to the agent loop when it is active, otherwise build a
// ToolInvocationResult and render + persist it as a tool card, then reset the
// streaming UI and auto-save.
//
// RenderAndPersistSlashResult lives here (not the frame) because it only
// touches the chat display and history, which this controller already owns;
// the frame's slash-command path calls it through this controller.
#pragma once

#include <functional>
#include <memory>

class wxCommandEvent;
class ChatDisplay;
class ChatHistory;
class AgentController;
class ConversationController;
struct WebFetchResult;
struct ToolInvocationResult;   // defined in tool_dispatcher.h; forward decl is
                               // enough for the by-value return / const-ref param

class ToolResultController
{
public:
    struct Callbacks {
        // Resets the frame's streaming UI (buttons, chat-state enum, animation).
        // Owned by the frame because ~25 other call sites drive the same state.
        std::function<void(bool)> setStreamingState;

        // True once the frame has begun tearing down; handlers no-op so late
        // worker events cannot touch destroyed UI.
        std::function<bool()>     isClosing;
    };

    ToolResultController(ChatDisplay*                    chatDisplay,
                         std::unique_ptr<ChatHistory>&   chatHistory,
                         AgentController&                agentController,
                         ConversationController&         convController);

    void SetCallbacks(Callbacks cb) { m_cb = std::move(cb); }

    // ── Event handlers (bound directly to the frame's event table) ──
    void OnCmdComplete(wxCommandEvent& evt);
    void OnCmdError(wxCommandEvent& evt);
    void OnPythonComplete(wxCommandEvent& evt);
    void OnPythonError(wxCommandEvent& evt);
    void OnGrepComplete(wxCommandEvent& evt);
    void OnWebFetchComplete(wxCommandEvent& evt);
    void OnWebFetchError(wxCommandEvent& evt);
    void OnToolWorkerComplete(wxCommandEvent& evt);

    // Renders a tool result as a chat card and appends it to history as a
    // user-role message.  Shared by the handlers above and the frame's
    // slash-command dispatch path.
    void RenderAndPersistSlashResult(const ToolInvocationResult& r);

private:
    // Shared tail: reset streaming UI and auto-save if history is non-empty.
    void FinishToolTurn();
    bool IsClosing() const;

    ToolInvocationResult MakeWebFetchToolInvocationResult(const WebFetchResult& r);

    ChatDisplay*                  m_chatDisplay;
    std::unique_ptr<ChatHistory>& m_chatHistory;
    AgentController&              m_agentController;
    ConversationController&       m_convController;
    Callbacks                     m_cb;
};
