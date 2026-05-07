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
#include <Poco/JSON/Stringifier.h>
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

    // ── Phase 3c-ii: structured tool_calls accumulator ──────────
    // OpenAI streaming format delivers tool_calls in fragments
    // across many SSE events.  Each delta.tool_calls entry carries
    // an `index` — fragments accumulate per-index until the stream
    // ends.  `id` and `function.name` typically arrive on the
    // first fragment for an index; `function.arguments` arrives
    // char-by-char across many.  We slot fragments into this
    // vector and serialize the result on stream end.
    //
    // Empty if the model emitted no tool_calls (common case for
    // chat-only turns or XML-protocol models).  When non-empty,
    // serialized as a JSON array string and attached to the
    // completion event as wxClientData.
    struct ToolCallAcc {
        std::string id;
        std::string type;     // "function" — kept for round-trip fidelity
        std::string name;
        std::string arguments;
    };
    std::vector<ToolCallAcc> toolCalls;

    auto ensureToolCallSlot = [&](int idx) -> ToolCallAcc* {
        if (idx < 0) return nullptr;
        if ((size_t)idx >= toolCalls.size()) {
            toolCalls.resize(idx + 1);
        }
        return &toolCalls[(size_t)idx];
    };

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

            // Only process "data:" prefixed lines. The SSE spec allows
            // an optional single space after the colon — handle both.
            if (line.size() < 5 || line.compare(0, 5, "data:") != 0)
                continue;

            std::string data = line.substr(5);
            if (!data.empty() && data.front() == ' ')
                data.erase(0, 1);

            // End of stream marker
            if (data == "[DONE]")
                break;

            try {
                Poco::JSON::Parser parser;
                auto obj = parser.parse(data).extract<Poco::JSON::Object::Ptr>();

                // Mid-stream error (OOM, context overflow, model unload, etc.)
                // llama-server emits `{"error": {"message": "..."}}` as a
                // regular SSE event on these — without this check the UI
                // would just stop receiving deltas and stay stuck in the
                // streaming state.
                if (obj->has("error")) {
                    std::string errMsg;
                    try {
                        auto errObj = obj->getObject("error");
                        if (errObj && errObj->has("message"))
                            errMsg = errObj->getValue<std::string>("message");
                    } catch (...) {
                        // "error" may be a plain string on some builds
                        try { errMsg = obj->getValue<std::string>("error"); }
                        catch (...) { /* unknown shape */ }
                    }
                    if (errMsg.empty())
                        errMsg = "Server returned an error mid-stream";

                    wxCommandEvent* event = new wxCommandEvent(wxEVT_ASSISTANT_ERROR);
                    event->SetString(wxString::FromUTF8("Stream error: " + errMsg));
                    SafeQueueEvent(event);
                    return (ExitCode)0;
                }

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

                    // Phase 3c-ii: tool_calls fragments.  OpenAI
                    // streaming format — each entry has an index
                    // and partial field updates that we accumulate.
                    if (delta->has("tool_calls") && !delta->isNull("tool_calls")) {
                        try {
                            auto tcArr = delta->getArray("tool_calls");
                            if (tcArr) {
                                for (size_t k = 0; k < tcArr->size(); ++k) {
                                    auto entry = tcArr->getObject(k);
                                    if (!entry) continue;

                                    // index is required; without it
                                    // we can't slot the fragment.
                                    int idx = -1;
                                    if (entry->has("index")) {
                                        try {
                                            idx = entry->getValue<int>("index");
                                        } catch (...) { idx = -1; }
                                    }
                                    ToolCallAcc* slot = ensureToolCallSlot(idx);
                                    if (!slot) continue;

                                    if (entry->has("id")) {
                                        try {
                                            std::string s = entry->getValue<std::string>("id");
                                            if (!s.empty()) slot->id = s;
                                        } catch (...) { /* skip */ }
                                    }
                                    if (entry->has("type")) {
                                        try {
                                            std::string s = entry->getValue<std::string>("type");
                                            if (!s.empty()) slot->type = s;
                                        } catch (...) { /* skip */ }
                                    }
                                    if (entry->has("function") && !entry->isNull("function")) {
                                        try {
                                            auto fn = entry->getObject("function");
                                            if (fn) {
                                                if (fn->has("name")) {
                                                    try {
                                                        std::string s = fn->getValue<std::string>("name");
                                                        if (!s.empty()) slot->name = s;
                                                    } catch (...) { /* skip */ }
                                                }
                                                if (fn->has("arguments")) {
                                                    try {
                                                        std::string s = fn->getValue<std::string>("arguments");
                                                        slot->arguments += s;
                                                    } catch (...) { /* skip */ }
                                                }
                                            }
                                        } catch (...) { /* skip */ }
                                    }
                                }
                            }
                        } catch (...) {
                            // Malformed tool_calls fragment — skip.
                            // We continue parsing rather than abort
                            // the whole stream; a partial tool call
                            // accumulator becomes a partial result.
                        }
                    }
                }

                // Check finish_reason for completion
                if (choice->has("finish_reason") && !choice->isNull("finish_reason")) {
                    std::string reason = choice->getValue<std::string>("finish_reason");
                    if (reason == "stop" || reason == "length" || reason == "tool_calls")
                        break;
                }
            }
            catch (const Poco::JSON::JSONException&) {
                // Skip malformed JSON lines
                continue;
            }
        }

        if (!isCancelled()) {
            // ── Phase 3c-ii: serialize the tool_calls accumulator ──
            // Render the per-index slots into an OpenAI-shape JSON
            // array.  Empty slots (no id AND no name AND no
            // arguments — happens if a fragment carried just an
            // index with no payload) are skipped.  Auto-id any
            // slot the model didn't name (rare but per-spec OK; we
            // synthesize "call_<idx>" so downstream threading has
            // something stable).
            std::string toolCallsJson;
            if (!toolCalls.empty()) {
                Poco::JSON::Array::Ptr arr = new Poco::JSON::Array;
                int slotIdx = 0;
                for (const auto& acc : toolCalls) {
                    if (acc.id.empty() && acc.name.empty() &&
                        acc.arguments.empty()) {
                        ++slotIdx;
                        continue;
                    }

                    Poco::JSON::Object::Ptr entry = new Poco::JSON::Object;
                    entry->set("id",
                        acc.id.empty() ? std::string("call_") + std::to_string(slotIdx)
                                       : acc.id);
                    entry->set("type",
                        acc.type.empty() ? std::string("function") : acc.type);

                    Poco::JSON::Object::Ptr fn = new Poco::JSON::Object;
                    fn->set("name", acc.name);
                    // arguments is a JSON-encoded string per OpenAI
                    // spec — we pass it through verbatim.  An empty
                    // arguments string is valid for tools that take
                    // no parameters.
                    fn->set("arguments", acc.arguments);
                    entry->set("function", fn);

                    arr->add(entry);
                    ++slotIdx;
                }
                if (arr->size() > 0) {
                    std::ostringstream oss;
                    Poco::JSON::Stringifier::stringify(arr, oss);
                    toolCallsJson = oss.str();
                }
            }

            wxCommandEvent* event = new wxCommandEvent(wxEVT_ASSISTANT_COMPLETE);
            event->SetString(wxString::FromUTF8(fullReply));
            event->SetClientObject(new AssistantCompletePayload(toolCallsJson));
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
