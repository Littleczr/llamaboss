// chat_client.cpp
//
// Communicates with llama-server's OpenAI-compatible endpoint:
//   POST /v1/chat/completions  (SSE streaming)
//
// Response format (Server-Sent Events):
//   data: {"choices":[{"delta":{"content":"hello"}}]}
//   data: {"choices":[{"delta":{"content":" world"},"finish_reason":"stop"}]}
//   data: [DONE]

#include "chat_client.h"

// Poco headers for HTTP communication
#include <Poco/URI.h>
#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/Net/NetException.h>
#include <Poco/StreamCopier.h>
#include <Poco/Timespan.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Parser.h>
#include <Poco/JSON/JSONException.h>
#include <Poco/Exception.h>

#include <sstream>

// Define custom events
wxDEFINE_EVENT(wxEVT_ASSISTANT_DELTA, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_ASSISTANT_COMPLETE, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_ASSISTANT_ERROR, wxCommandEvent);

// ═══════════════════════════════════════════════════════════════════
// ChatWorkerThread Implementation
// ═══════════════════════════════════════════════════════════════════

ChatWorkerThread::ChatWorkerThread(wxEvtHandler* eventHandler,
    const std::string& model,
    const std::string& apiUrl,
    const std::string& requestBody,
    std::shared_ptr<std::atomic<bool>> cancelFlag,
    std::weak_ptr<std::atomic<bool>> aliveToken,
    unsigned long generationId)
    : wxThread(wxTHREAD_DETACHED)
    , m_eventHandler(eventHandler)
    , m_model(model)
    , m_apiUrl(apiUrl)
    , m_requestBody(requestBody)
    , m_cancelFlag(cancelFlag)
    , m_aliveToken(aliveToken)
    , m_generationId(generationId)
{
}

bool ChatWorkerThread::SafeQueueEvent(wxCommandEvent* event)
{
    auto alive = m_aliveToken.lock();
    if (!alive || !alive->load()) {
        delete event;
        return false;
    }
    event->SetExtraLong(m_generationId);
    wxQueueEvent(m_eventHandler, event);
    return true;
}

wxThread::ExitCode ChatWorkerThread::Entry()
{
    std::string fullReply;

    auto isCancelled = [this]() { return m_cancelFlag->load(); };

    try {
        // ── Connect to llama-server's OpenAI-compatible endpoint ──
        Poco::URI uri(m_apiUrl + "/v1/chat/completions");
        Poco::Net::HTTPClientSession sess(uri.getHost(), uri.getPort());
        sess.setTimeout(Poco::Timespan(120, 0)); // 2min timeout for large models

        Poco::Net::HTTPRequest req(
            Poco::Net::HTTPRequest::HTTP_POST,
            uri.getPathAndQuery(),
            Poco::Net::HTTPMessage::HTTP_1_1
        );
        req.setContentType("application/json");
        req.setContentLength((long)m_requestBody.size());

        std::ostream& out = sess.sendRequest(req);
        out << m_requestBody;
        out.flush();

        Poco::Net::HTTPResponse resp;
        std::istream& in = sess.receiveResponse(resp);

        if (resp.getStatus() != Poco::Net::HTTPResponse::HTTP_OK) {
            std::string err;
            Poco::StreamCopier::copyToString(in, err);

            wxCommandEvent* event = new wxCommandEvent(wxEVT_ASSISTANT_ERROR);
            event->SetString(wxString::FromUTF8(
                "API Error (" + std::to_string(resp.getStatus()) + "): "
                + resp.getReason() + " - " + err
            ));
            SafeQueueEvent(event);
            return (ExitCode)0;
        }

        // ── Parse SSE stream ─────────────────────────────────────
        // Each event is: "data: <json>\n\n"
        // Final event is: "data: [DONE]\n\n"
        std::string line;

        while (std::getline(in, line) && !isCancelled()) {
            // Strip trailing \r
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            // Skip empty lines (SSE event boundaries)
            if (line.empty()) continue;

            // Only process "data: " prefixed lines
            if (line.size() < 6 || line.substr(0, 6) != "data: ")
                continue;

            std::string data = line.substr(6);

            // End of stream marker
            if (data == "[DONE]")
                break;

            try {
                Poco::JSON::Parser parser;
                auto obj = parser.parse(data).extract<Poco::JSON::Object::Ptr>();

                if (!obj->has("choices")) continue;
                auto choices = obj->getArray("choices");
                if (!choices || choices->size() == 0) continue;

                auto choice = choices->getObject(0);

                // Extract content delta
                if (choice->has("delta")) {
                    auto delta = choice->getObject("delta");
                    if (delta->has("content") && !delta->isNull("content")) {
                        std::string content = delta->getValue<std::string>("content");
                        fullReply += content;

                        wxCommandEvent* event = new wxCommandEvent(wxEVT_ASSISTANT_DELTA);
                        event->SetString(wxString::FromUTF8(content));
                        if (!SafeQueueEvent(event))
                            return (ExitCode)0;
                    }
                }

                // Check finish_reason for completion
                if (choice->has("finish_reason") && !choice->isNull("finish_reason")) {
                    std::string reason = choice->getValue<std::string>("finish_reason");
                    if (reason == "stop" || reason == "length")
                        break;
                }
            }
            catch (const Poco::JSON::JSONException&) {
                // Skip malformed JSON lines
                continue;
            }
        }

        if (!isCancelled()) {
            wxCommandEvent* event = new wxCommandEvent(wxEVT_ASSISTANT_COMPLETE);
            event->SetString(wxString::FromUTF8(fullReply));
            SafeQueueEvent(event);
        }
    }
    catch (const Poco::Net::HTTPException& ex) {
        if (!isCancelled()) {
            wxCommandEvent* event = new wxCommandEvent(wxEVT_ASSISTANT_ERROR);
            event->SetString(wxString::FromUTF8("HTTP Error: " + ex.displayText()));
            SafeQueueEvent(event);
        }
    }
    catch (const Poco::Net::NetException& ex) {
        if (!isCancelled()) {
            wxCommandEvent* event = new wxCommandEvent(wxEVT_ASSISTANT_ERROR);
            event->SetString(wxString::FromUTF8("Network Error: " + ex.displayText()));
            SafeQueueEvent(event);
        }
    }
    catch (const Poco::Exception& ex) {
        if (!isCancelled()) {
            wxCommandEvent* event = new wxCommandEvent(wxEVT_ASSISTANT_ERROR);
            event->SetString(wxString::FromUTF8("Poco Error: " + ex.displayText()));
            SafeQueueEvent(event);
        }
    }
    catch (const std::exception& ex) {
        if (!isCancelled()) {
            wxCommandEvent* event = new wxCommandEvent(wxEVT_ASSISTANT_ERROR);
            event->SetString(wxString::FromUTF8(std::string("Error: ") + ex.what()));
            SafeQueueEvent(event);
        }
    }

    return (ExitCode)0;
}

// ═══════════════════════════════════════════════════════════════════
// ChatClient Implementation
// ═══════════════════════════════════════════════════════════════════

ChatClient::ChatClient(wxEvtHandler* eventHandler,
                       std::weak_ptr<std::atomic<bool>> aliveToken)
    : m_eventHandler(eventHandler)
    , m_aliveToken(aliveToken)
    , m_isStreaming(false)
{
}

ChatClient::~ChatClient()
{
    StopGeneration();
}

bool ChatClient::SendMessage(const std::string& model,
    const std::string& apiUrl,
    const std::string& requestBody,
    unsigned long generationId)
{
    if (m_isStreaming) {
        return false;
    }

    m_isStreaming = true;
    m_cancelFlag = std::make_shared<std::atomic<bool>>(false);

    auto* thread = new ChatWorkerThread(
        m_eventHandler, model, apiUrl, requestBody,
        m_cancelFlag, m_aliveToken, generationId);

    if (thread->Run() != wxTHREAD_NO_ERROR) {
        delete thread;
        m_cancelFlag.reset();
        m_isStreaming = false;
        return false;
    }

    return true;
}

void ChatClient::StopGeneration()
{
    if (m_isStreaming && m_cancelFlag) {
        m_cancelFlag->store(true);
        m_cancelFlag.reset();
        m_isStreaming = false;
    }
}

void ChatClient::ResetStreamingState()
{
    m_isStreaming = false;
    m_cancelFlag.reset();
}
