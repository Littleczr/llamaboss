// chat_client.h
#pragma once

#include <wx/wx.h>
#include <wx/thread.h>
#include <wx/clntdata.h>
#include <vector>
#include <string>
#include <memory>
#include <atomic>

// Poco headers
#include <Poco/Logger.h>

// Custom events for thread communication
wxDECLARE_EVENT(wxEVT_ASSISTANT_DELTA, wxCommandEvent);
wxDECLARE_EVENT(wxEVT_ASSISTANT_COMPLETE, wxCommandEvent);
wxDECLARE_EVENT(wxEVT_ASSISTANT_ERROR, wxCommandEvent);

// ── Phase 3c-ii: extra payload on assistant-complete events ────
// wxCommandEvent::SetString already carries the model's prose
// content for the completed turn.  When the model is on the
// native tool-calling protocol it may also have emitted a
// structured tool_calls array; we surface that here.
//
// AssistantCompletePayload is attached via SetClientObject (wx
// owns it and frees it after the event is consumed).  Recipients
// that don't know about tool_calls can ignore the payload entirely
// — the existing SetString-based contract is unchanged.
//
// toolCallsJson is a JSON array string in OpenAI shape:
//   [{"id":"call_0","type":"function",
//     "function":{"name":"pwd","arguments":"{}"}}]
// Empty when the stream had no tool_calls.
class AssistantCompletePayload : public wxClientData
{
public:
    explicit AssistantCompletePayload(std::string toolCallsJson)
        : m_toolCallsJson(std::move(toolCallsJson)) {}

    const std::string& ToolCallsJson() const { return m_toolCallsJson; }

private:
    std::string m_toolCallsJson;
};

// Forward declarations
class ChatClient;

// Thread class for handling HTTP requests
class ChatWorkerThread : public wxThread
{
public:
    ChatWorkerThread(wxEvtHandler* eventHandler,
        const std::string& model,
        const std::string& apiUrl,
        const std::string& requestBody,
        std::shared_ptr<std::atomic<bool>> cancelFlag,
        std::weak_ptr<std::atomic<bool>> aliveToken,
        unsigned long generationId);

protected:
    virtual ExitCode Entry() override;

private:
    wxEvtHandler* m_eventHandler;
    std::string m_model;
    std::string m_apiUrl;
    std::string m_requestBody;
    std::shared_ptr<std::atomic<bool>> m_cancelFlag;
    std::weak_ptr<std::atomic<bool>> m_aliveToken;
    unsigned long m_generationId;

    bool SafeQueueEvent(wxCommandEvent* event);
};

// Chat client class for managing HTTP communication with llama-server
class ChatClient
{
public:
    ChatClient(wxEvtHandler* eventHandler,
               std::weak_ptr<std::atomic<bool>> aliveToken);
    ~ChatClient();

    // Start a chat request (non-blocking, uses threading)
    bool SendMessage(const std::string& model,
        const std::string& apiUrl,
        const std::string& requestBody,
        unsigned long generationId);

    // Stop any current generation
    void StopGeneration();

    // Check if currently streaming
    bool IsStreaming() const { return m_isStreaming; }

    // Reset streaming state (called when streaming completes)
    void ResetStreamingState();

private:
    wxEvtHandler* m_eventHandler;
    std::weak_ptr<std::atomic<bool>> m_aliveToken;
    std::shared_ptr<std::atomic<bool>> m_cancelFlag;
    bool m_isStreaming;
};
