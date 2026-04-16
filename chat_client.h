// chat_client.h
#pragma once

#include <wx/wx.h>
#include <wx/thread.h>
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
