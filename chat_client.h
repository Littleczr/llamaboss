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

// Inference target descriptor (transport URL/path/tls/auth/protocol).
#include "inference_target.h"

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
    explicit AssistantCompletePayload(std::string toolCallsJson,
                                      long promptTokens     = -1,
                                      long completionTokens = -1,
                                      std::vector<std::string> imageDataUrls = {})
        : m_toolCallsJson(std::move(toolCallsJson))
        , m_promptTokens(promptTokens)
        , m_completionTokens(completionTokens)
        , m_imageDataUrls(std::move(imageDataUrls)) {}

    const std::string& ToolCallsJson() const { return m_toolCallsJson; }

    // Generated images from an image-output model (OpenRouter
    // chat-completions image generation).  Each entry is a base64
    // data URL exactly as it arrived on the stream's `images` field
    // ("data:image/png;base64,....").  Decoding and disk persistence
    // happen on the UI thread — the workflow folder is keyed on the
    // conversation's file path, which only the frame knows (and may
    // have to create via autosave first).  Empty for text turns.
    const std::vector<std::string>& ImageDataUrls() const
    { return m_imageDataUrls; }

    // Exact token usage reported by the server for this turn, taken
    // from the SSE stream's `usage` object (llama-server includes it
    // on the final chunk; OpenAI-compatible providers include it on a
    // trailing usage chunk when requested, and some — e.g. OpenRouter —
    // by default).  -1 when the stream carried no usage object; the
    // context meter falls back to its byte heuristic in that case.
    long PromptTokens()     const { return m_promptTokens; }
    long CompletionTokens() const { return m_completionTokens; }

private:
    std::string m_toolCallsJson;
    long m_promptTokens     = -1;
    long m_completionTokens = -1;
    std::vector<std::string> m_imageDataUrls;
};

// Forward declarations
class ChatClient;

// Thread class for handling HTTP requests
//
// The worker no longer takes a bare (model, apiUrl) pair. It takes a
// fully-resolved InferenceTarget describing the endpoint URL, path,
// TLS requirement, and auth/extra headers. The request body still
// carries the wire "model" field, so the target's modelId is purely
// informational at the transport layer.
class ChatWorkerThread : public wxThread
{
public:
    ChatWorkerThread(wxEvtHandler* eventHandler,
        InferenceTarget target,
        std::string requestBody,
        std::shared_ptr<std::atomic<bool>> cancelFlag,
        std::weak_ptr<std::atomic<bool>> aliveToken,
        unsigned long generationId);

protected:
    virtual ExitCode Entry() override;

private:
    wxEvtHandler* m_eventHandler;
    InferenceTarget m_target;
    std::string m_requestBody;
    std::shared_ptr<std::atomic<bool>> m_cancelFlag;
    std::weak_ptr<std::atomic<bool>> m_aliveToken;
    unsigned long m_generationId;

    bool SafeQueueEvent(wxCommandEvent* event);
};

// Chat client class for managing HTTP communication with an
// OpenAI-compatible endpoint (local llama-server or a remote provider).
class ChatClient
{
public:
    ChatClient(wxEvtHandler* eventHandler,
               std::weak_ptr<std::atomic<bool>> aliveToken);
    ~ChatClient();

    // Start a chat request against a resolved target (non-blocking,
    // uses threading). This is the primary entry point; callers that
    // support remote endpoints build the target upstream.
    bool SendMessage(const InferenceTarget& target,
        const std::string& requestBody,
        unsigned long generationId);

    // Back-compat overload. Builds a default LOCAL target (plain http,
    // no auth, OpenAI-compatible path) from (model, apiUrl) and
    // forwards to the target-based overload. Existing call sites that
    // pass a model + base URL keep their exact current behavior.
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
