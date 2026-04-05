// llm_classifier.cpp
// Implementation of LLM-based natural language classification.

#include "llm_classifier.h"

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
#include <Poco/Exception.h>

#include <sstream>
#include <string>

// ═══════════════════════════════════════════════════════════════════
//  Internal helpers
// ═══════════════════════════════════════════════════════════════════

// The system prompt that teaches the model to classify messages.
// Kept as a function so it's easy to evolve without touching the caller.
static std::string BuildSystemPrompt()
{
    return
        "You are a message classifier for a desktop app called LlamaBoss. "
        "The app has a workspace folder where the user stores files.\n"
        "\n"
        "Your job: decide if the user wants to perform a workspace action "
        "or is just having a normal conversation.\n"
        "\n"
        "Supported actions:\n"
        "- save_chat: The user wants to save, export, or keep a copy of the "
        "current conversation. Examples: \"save this chat\", \"export our "
        "conversation\", \"save this as notes\", \"keep a copy of this chat\".\n"
        "- workspace_search: The user wants to find or search for files in "
        "their workspace. Examples: \"find the budget file\", \"search for "
        "the changelog\", \"do I have any spreadsheets?\", \"look for the "
        "hours report\".\n"
        "- chat: Everything else. Normal questions, greetings, requests for "
        "information, coding help, explanations, opinions, etc.\n"
        "\n"
        "RULES:\n"
        "1. Most messages are just chat. Only classify as an action if the "
        "user clearly wants to perform one.\n"
        "2. If the user is ASKING ABOUT files or saving (\"how do I save a "
        "file?\", \"what is a CSV?\") that is chat, not an action.\n"
        "3. If you are unsure, always choose chat.\n"
        "\n"
        "Respond with ONLY a valid JSON object on a single line. "
        "No explanation, no markdown, no backticks.\n"
        "\n"
        "For chat:\n"
        "{\"action\":\"chat\"}\n"
        "\n"
        "For save_chat:\n"
        "{\"action\":\"save_chat\",\"filename_hint\":\"suggested_name\"}\n"
        "The filename_hint should be a short snake_case name based on what "
        "the user said, or \"chat_export\" if nothing specific was mentioned.\n"
        "\n"
        "For workspace_search:\n"
        "{\"action\":\"workspace_search\",\"topic\":\"what to search for\"}\n"
        "The topic should be the key search terms extracted from the request.\n";
}

// Build the Ollama /api/chat request body as a JSON string.
static std::string BuildRequestBody(const std::string& model,
                                    const std::string& systemPrompt,
                                    const std::string& userMessage)
{
    // System message
    Poco::JSON::Object::Ptr sysMsg = new Poco::JSON::Object;
    sysMsg->set("role", "system");
    sysMsg->set("content", systemPrompt);

    // User message
    Poco::JSON::Object::Ptr userMsg = new Poco::JSON::Object;
    userMsg->set("role", "user");
    userMsg->set("content", userMessage);

    // Messages array
    Poco::JSON::Array::Ptr messages = new Poco::JSON::Array;
    messages->add(sysMsg);
    messages->add(userMsg);

    // Top-level request
    Poco::JSON::Object::Ptr req = new Poco::JSON::Object;
    req->set("model", model);
    req->set("messages", messages);
    req->set("stream", false);

    std::ostringstream oss;
    Poco::JSON::Stringifier::stringify(req, oss);
    return oss.str();
}

// Try to extract a JSON object from a model response string.
// Handles common quirks: markdown backtick fences, leading/trailing whitespace,
// preamble text before the JSON, etc.
static std::string ExtractJsonFromResponse(const std::string& raw)
{
    // Find the first '{' and last '}' — that's our JSON object
    size_t start = raw.find('{');
    size_t end = raw.rfind('}');

    if (start == std::string::npos || end == std::string::npos || end <= start)
        return "";

    return raw.substr(start, end - start + 1);
}

// Build a default "chat" result (used on any failure).
static LLMClassifyResult DefaultChatResult(const std::string& error = "",
                                           const std::string& rawResponse = "")
{
    LLMClassifyResult r;
    r.success = error.empty();
    r.errorMessage = error;
    r.action = "chat";
    r.rawResponse = rawResponse;
    return r;
}

// ═══════════════════════════════════════════════════════════════════
//  Main classification function
// ═══════════════════════════════════════════════════════════════════

LLMClassifyResult ClassifyWithLLM(
    const std::string& message,
    const std::string& apiUrl,
    const std::string& model,
    int timeoutSeconds)
{
    // ── Build the request ────────────────────────────────────────
    std::string systemPrompt = BuildSystemPrompt();
    std::string body = BuildRequestBody(model, systemPrompt, message);

    // ── Make the synchronous HTTP call ───────────────────────────
    std::string responseBody;

    try {
        Poco::URI uri(apiUrl + "/api/chat");
        Poco::Net::HTTPClientSession sess(uri.getHost(), uri.getPort());
        sess.setTimeout(Poco::Timespan(timeoutSeconds, 0));

        Poco::Net::HTTPRequest req(
            Poco::Net::HTTPRequest::HTTP_POST,
            uri.getPathAndQuery(),
            Poco::Net::HTTPMessage::HTTP_1_1
        );
        req.setContentType("application/json");
        req.setContentLength(static_cast<long>(body.size()));

        std::ostream& out = sess.sendRequest(req);
        out << body;
        out.flush();

        Poco::Net::HTTPResponse resp;
        std::istream& in = sess.receiveResponse(resp);
        Poco::StreamCopier::copyToString(in, responseBody);

        if (resp.getStatus() != Poco::Net::HTTPResponse::HTTP_OK) {
            return DefaultChatResult(
                "Ollama returned HTTP " + std::to_string(resp.getStatus()),
                responseBody);
        }
    }
    catch (const Poco::TimeoutException&) {
        return DefaultChatResult("Classification timed out");
    }
    catch (const Poco::Net::NetException& ex) {
        return DefaultChatResult("Network error: " + ex.displayText());
    }
    catch (const Poco::Exception& ex) {
        return DefaultChatResult("Poco error: " + ex.displayText());
    }
    catch (const std::exception& ex) {
        return DefaultChatResult(std::string("Error: ") + ex.what());
    }

    // ── Extract the assistant's message content ──────────────────
    std::string assistantContent;

    try {
        Poco::JSON::Parser parser;
        auto parsed = parser.parse(responseBody);
        auto obj = parsed.extract<Poco::JSON::Object::Ptr>();

        if (obj->has("message")) {
            auto msgObj = obj->getObject("message");
            if (msgObj->has("content")) {
                assistantContent = msgObj->getValue<std::string>("content");
            }
        }
    }
    catch (const Poco::Exception&) {
        return DefaultChatResult("Failed to parse Ollama response", responseBody);
    }

    if (assistantContent.empty()) {
        return DefaultChatResult("Empty response from model", responseBody);
    }

    // ── Parse the JSON classification from the model's output ────
    std::string jsonStr = ExtractJsonFromResponse(assistantContent);
    if (jsonStr.empty()) {
        // Model didn't return valid JSON — fall through to chat
        return DefaultChatResult("", assistantContent);
    }

    LLMClassifyResult result;
    result.success = true;
    result.rawResponse = assistantContent;
    result.action = "chat";  // default

    try {
        Poco::JSON::Parser jsonParser;
        auto parsed = jsonParser.parse(jsonStr);
        auto obj = parsed.extract<Poco::JSON::Object::Ptr>();

        if (obj->has("action")) {
            result.action = obj->getValue<std::string>("action");
        }

        if (obj->has("topic")) {
            result.topic = obj->getValue<std::string>("topic");
        }

        if (obj->has("filename_hint")) {
            result.filenameHint = obj->getValue<std::string>("filename_hint");
        }
    }
    catch (const Poco::Exception&) {
        // JSON parse failed — default to chat
        return DefaultChatResult("", assistantContent);
    }

    // ── Validate the action is one we recognize ──────────────────
    if (result.action != "chat" &&
        result.action != "save_chat" &&
        result.action != "workspace_search")
    {
        // Unrecognized action — treat as chat
        result.action = "chat";
    }

    return result;
}
