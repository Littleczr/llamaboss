// chat_client.h
#pragma once

#include <wx/wx.h>
#include <wx/thread.h>
#include <wx/clntdata.h>
#include <vector>
#include <string>
#include <memory>
#include <atomic>
#include <mutex>

// Poco headers
#include <Poco/Logger.h>
// Already in pch.h; named here so ChatRequestControl's session member is
// a complete type without relying on the precompiled header.
#include <Poco/Net/HTTPClientSession.h>

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
// AssistantCompletePayload is attached via SetClientObject.
// IMPORTANT ownership contract: wxCommandEvent does NOT own or delete
// its client object (wx/event.h keeps a raw m_clientObject with no
// delete in any destructor -- it exists to point at control-owned
// listbox item data).  The receiving handler must take ownership;
// MyFrame::OnAssistantComplete wraps it in a std::unique_ptr as its
// very first action, before any early return.  Recipients
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

    // Destructive read for the one consumer that persists them.
    // A generated image arrives as a base64 data URL, so a 4 MB PNG
    // is ~5.5 MB of std::string per entry; the UI handler used to
    // copy the whole vector out of the payload purely to read it
    // once.  Call this instead when you are about to consume them.
    // The payload is left empty afterwards, which is fine: the
    // handler owns it via unique_ptr and drops it at end of scope.
    std::vector<std::string> TakeImageDataUrls()
    { return std::move(m_imageDataUrls); }

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

// ── Per-request cancellation control ─────────────────────────────
// Replaces the bare shared_ptr<atomic<bool>> cancel flag.
//
// The flag alone could not actually stop a generation.  The worker
// spends nearly all of its life blocked inside std::getline() on the
// SSE stream and only re-checks the flag between lines, so a stalled
// or slow generation ignored Stop until the next token arrived or the
// 120s receive timeout expired.  Meanwhile llama-server is pinned to
// --parallel 1 (see the single-slot rationale in server_manager.cpp),
// so the abandoned generation kept holding the only slot and the next
// send queued behind it.
//
// This object adds the missing half: the worker registers its live
// transport here for the duration of the request, and Cancel() closes
// the socket out from under the blocking read.  tool_web_fetch.cpp
// solves the same problem for WinHTTP via a global registry keyed on
// the cancel flag; chat has exactly one owner, so the state lives in
// the control object itself rather than in a side table.
//
// Lifetime: owned by shared_ptr, held by BOTH ChatClient and the
// detached worker, so it outlives whichever side finishes first.
class ChatRequestControl
{
public:
    // UI thread.  Marks the request cancelled, then aborts the live
    // transport if one is registered.
    //
    // The ordering is load-bearing: the flag is set BEFORE the abort so
    // that when the socket dies and the worker unwinds, every handler
    // in ChatWorkerThread::Entry() -- all of which are already guarded
    // on the cancelled state -- sees a deliberate cancellation and stays
    // silent, instead of posting wxEVT_ASSISTANT_ERROR for a socket the
    // user closed on purpose.
    void Cancel();

    bool IsCancelled() const
    { return m_cancelled.load(std::memory_order_acquire); }

    // Worker thread.  Registers the session for the life of the request.
    //
    // Returns false when Cancel() already ran -- i.e. Stop was pressed in
    // the window between the worker starting and its session existing.
    // The caller must abandon the request rather than connect, otherwise
    // it would block in a read that nothing can now interrupt.
    bool AttachSession(
        const std::shared_ptr<Poco::Net::HTTPClientSession>& session);

    // Worker thread.  Must run on EVERY exit path from the request,
    // including exceptions, before the worker drops its own reference.
    void DetachSession();

private:
    std::atomic<bool> m_cancelled{false};

    // Guards m_session only.  Cancel() copies the pointer out under the
    // lock and aborts outside it, so a slow abort never blocks a worker
    // trying to detach, and the shared_ptr copy keeps the session alive
    // for the duration of the abort regardless of what the worker does.
    std::mutex m_mutex;
    std::shared_ptr<Poco::Net::HTTPClientSession> m_session;
};

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
        std::shared_ptr<ChatRequestControl> control,
        std::weak_ptr<std::atomic<bool>> aliveToken,
        unsigned long generationId);

protected:
    virtual ExitCode Entry() override;

private:
    wxEvtHandler* m_eventHandler;
    InferenceTarget m_target;
    std::string m_requestBody;
    std::shared_ptr<ChatRequestControl> m_control;
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

    // Stop any current generation.  Aborts the in-flight socket, not
    // just a flag -- see ChatRequestControl.
    void StopGeneration();

    // Check if currently streaming
    bool IsStreaming() const { return m_isStreaming; }

    // Reset streaming state (called when streaming completes).
    // Deliberately does NOT cancel: this is the normal-completion path,
    // where the transport has already closed itself.  Use
    // StopGeneration() to interrupt a live request.
    void ResetStreamingState();

private:
    wxEvtHandler* m_eventHandler;
    std::weak_ptr<std::atomic<bool>> m_aliveToken;
    std::shared_ptr<ChatRequestControl> m_activeRequest;
    bool m_isStreaming;
};
