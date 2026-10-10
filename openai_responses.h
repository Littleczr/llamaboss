#pragma once

// Stateless OpenAI Responses adapter.
//
// Covers chat, streaming, reasoning effort, errors, token usage, and
// agent tools.  Function tools are converted from the Chat Completions
// catalog, native tool_calls history is replayed as function_call /
// function_call_output items, and the model's own reasoning items
// (encrypted, because store:false) are replayed in front of the function
// calls they produced, preserving the original output items required
// for stateless reasoning continuation.
//
// The application still builds its Chat Completions projection;
// conversion happens once, after attachments, immediately before
// transport.  Header-only so existing Visual Studio builds need no new
// compiled item.
#include "reasoning_policy.h"
#include "repetition_guard.h"
#include "chatgpt_auth_core.h"   // plan-route namespace + error vocabulary
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Stringifier.h>
#include <Poco/URI.h>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace lb_responses {
using Object = Poco::JSON::Object;
using Array = Poco::JSON::Array;

// Shared with the Chat Completions accumulator in chat_client.cpp.
constexpr std::size_t kMaxToolArgumentBytes = 4ull * 1024 * 1024;

// The worker translates this into the existing interrupted-turn completion
// path, which keeps partial prose but discards every tool call in the reply.
class ToolArgumentRepetition : public std::runtime_error {
public:
    ToolArgumentRepetition()
        : std::runtime_error("The model kept repeating tool-call arguments.") {}
};

// Private per-message sidecar key.  ChatHistory attaches the verbatim
// Responses `output` array of an assistant tool-call turn under this key
// (only when the send target is a Responses endpoint) so the converter
// can replay reasoning + function_call items exactly as the model emitted
// them.  Never reaches any provider: the converter consumes it, and no
// Chat Completions target ever sees it.
inline const char* kOutputSidecarKey() { return "lb_responses_output"; }

// Direct-OpenAI routing is family-based, not an allowlist: an allowlist
// of model ids breaks the moment a new id ships (it falls through to
// /v1/chat/completions and 400s on tools + reasoning).  Responses is
// OpenAI's primary text API and accepts every current text model, so
// route everything EXCEPT the families that exist only on Chat
// Completions.  Callers still gate on host == api.openai.com, a default
// chat path, and non-image turns.
inline bool IsChatCompletionsOnlyModel(std::string model)
{
    for (char& c : model)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    static const char* const kChatOnly[] = {
        "audio",            // gpt-4o-audio-preview, gpt-audio, ...
        "search-preview",   // gpt-4o-search-preview, gpt-4o-mini-search-preview
        "realtime",         // realtime models are a different transport
        "instruct",         // legacy completions-style ids
    };
    for (const char* token : kChatOnly)
        if (model.find(token) != std::string::npos) return true;
    return false;
}

inline bool RoutesThroughResponses(const std::string& model)
{
    return !model.empty() && !IsChatCompletionsOnlyModel(model);
}

inline bool IsResponsesPath(std::string path)
{
    if (!path.empty() && path.back() == '/') path.pop_back();
    return path == "/v1/responses" || path == "/responses";
}

inline std::string ResolveChatPath(const std::string& baseUrl,
    const std::string& path, const std::string& model)
{
    if (IsResponsesPath(path)) return path;
    // Automatic migration applies to direct api.openai.com only: Chat
    // Completions cannot combine function tools with reasoning on the
    // reasoning families (Luna 400s on tools + reasoning_effort, and at
    // Auto via the server-side default).  Custom routes, other providers
    // (OpenRouter), and Chat-Completions-only families keep their path.
    if (!RoutesThroughResponses(model)) return path;
    try {
        Poco::URI uri(baseUrl);
        std::string host = uri.getHost();
        for (char& c : host)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        if (host != "api.openai.com") return path;
        if (path == "/v1/chat/completions" || path == "/v1/chat/completions/")
            return "/v1/responses";
        if (path == "/chat/completions" || path == "/chat/completions/")
            return "/responses";
    } catch (const Poco::Exception&) { /* normal transport reports bad URLs */ }
    return path;
}

inline std::string RequiredString(const Object::Ptr& obj, const char* key)
{
    if (!obj || !obj->has(key) || obj->isNull(key) || !obj->get(key).isString())
        throw std::runtime_error(std::string("Invalid Responses field: ") + key);
    return obj->getValue<std::string>(key);
}

inline std::string Stringify(const Poco::Dynamic::Var& value)
{
    std::ostringstream out;
    Poco::JSON::Stringifier::stringify(value, out);
    return out.str();
}

// ── Tool catalog ─────────────────────────────────────────────────
// Chat Completions:  {"type":"function","function":{name,description,parameters}}
// Responses:         {"type":"function",name,description,parameters,strict}
// ChatGPT plan route: the same function tools, but grouped in ONE
// namespace -- {"type":"namespace","name":"llamaboss","tools":[...]} --
// because that route does not accept bare top-level function tools.
// The model's function_call items then carry "namespace":"llamaboss",
// and every replayed function_call must carry it back (the server
// rejects a namespaced call that arrives without it).
inline Array::Ptr ConvertTools(const Array::Ptr& chatTools)
{
    Array::Ptr tools = new Array;
    for (std::size_t i = 0; i < chatTools->size(); ++i) {
        const auto entry = chatTools->getObject(i);
        if (!entry) throw std::runtime_error("Invalid tool catalog entry.");
        if (RequiredString(entry, "type") != "function")
            throw std::runtime_error("Only function tools are supported on this Responses connection.");
        const auto fn = entry->getObject("function");
        if (!fn) throw std::runtime_error("Tool catalog entry has no function object.");
        Object::Ptr tool = new Object;
        tool->set("type", "function");
        tool->set("name", RequiredString(fn, "name"));
        if (fn->has("description") && !fn->isNull("description"))
            tool->set("description", RequiredString(fn, "description"));
        if (fn->has("parameters") && !fn->isNull("parameters"))
            tool->set("parameters", fn->get("parameters"));
        // The catalog schemas are not strict-mode schemas (optional
        // properties, no additionalProperties:false everywhere).  Strict
        // would make the provider reject the whole request.
        tool->set("strict", false);
        tools->add(tool);
    }
    return tools;
}

inline Array::Ptr WrapToolsInNamespace(const Array::Ptr& functionTools)
{
    Object::Ptr ns = new Object;
    ns->set("type", "namespace");
    ns->set("name", std::string(lb_chatgpt::kToolNamespace()));
    ns->set("description", std::string(
        "LlamaBoss tools that act on the user's own computer: files, shell, Python, web fetch."));
    ns->set("tools", functionTools);
    Array::Ptr wrapped = new Array;
    wrapped->add(ns);
    return wrapped;
}

// Function-call items replayed into `input` must match how the tools
// are declared on THIS request: namespaced on the plan route, bare on
// API-key routes.  A conversation can move between the two (same chat,
// different connection), so the stored items are normalized per send.
inline Object::Ptr NormalizeFunctionCallNamespace(Object::Ptr item, bool chatgptPlan)
{
    if (!item || !item->has("type") || !item->get("type").isString() ||
        item->getValue<std::string>("type") != "function_call")
        return item;
    if (chatgptPlan) {
        if (!item->has("namespace") || item->isNull("namespace"))
            item->set("namespace", std::string(lb_chatgpt::kToolNamespace()));
    } else if (item->has("namespace")) {
        item->remove("namespace");
    }
    return item;
}

// ── Assistant tool-call turn ─────────────────────────────────────
// Replay the complete original output when available. The agent may have
// executed only a subset (batch limit, or a tool that must run alone).
// Preserve those skipped calls too, with explicit not-executed results;
// never silently remove items from the model's reasoning continuation.
// ChatHistory supplies the actual results for every surviving tool call.
// Legacy turns have no raw output and are reconstructed from chat history.
inline void AppendAssistantToolTurn(Array::Ptr input,
    const Object::Ptr& old, const std::string& visibleText, bool chatgptPlan = false)
{
    const auto toolCalls = old->getArray("tool_calls");
    if (!toolCalls) throw std::runtime_error("Assistant tool_calls is not an array.");

    // call_id -> function object (name/arguments) for the surviving calls.
    std::vector<std::string> order;
    std::map<std::string, Object::Ptr> surviving;
    for (std::size_t i = 0; i < toolCalls->size(); ++i) {
        const auto call = toolCalls->getObject(i);
        if (!call) throw std::runtime_error("Invalid tool_calls entry.");
        const std::string id = RequiredString(call, "id");
        const auto fn = call->getObject("function");
        if (!fn) throw std::runtime_error("tool_calls entry has no function object.");
        if (id.empty() || !surviving.emplace(id, fn).second)
            throw std::runtime_error("Assistant tool_calls contains an empty or duplicate call id.");
        order.push_back(id);
    }

    Array::Ptr sidecar;
    if (old->has(kOutputSidecarKey()) && !old->isNull(kOutputSidecarKey()))
        sidecar = old->getArray(kOutputSidecarKey());

    if (sidecar) {
        std::set<std::string> originalCalls;
        std::vector<std::string> skipped;
        // Validate before emitting anything: stale sidecar arguments must
        // never describe a different operation from the one actually run.
        for (std::size_t i = 0; i < sidecar->size(); ++i) {
            const auto item = sidecar->getObject(i);
            const std::string type = RequiredString(item, "type");
            if (type == "function_call") {
                const std::string callId = RequiredString(item, "call_id");
                if (callId.empty() || !originalCalls.insert(callId).second)
                    throw std::runtime_error("Saved Responses output contains an empty or duplicate call id.");
                const auto selected = surviving.find(callId);
                if (selected == surviving.end()) skipped.push_back(callId);
                else if (RequiredString(item, "name") != RequiredString(selected->second, "name") ||
                         RequiredString(item, "arguments") != RequiredString(selected->second, "arguments"))
                    throw std::runtime_error("Saved Responses output does not match the executed tool call.");
            } else if (type != "reasoning" && type != "message") {
                throw std::runtime_error("Saved Responses output contains an unsupported item.");
            }
        }
        for (const auto& id : order)
            if (originalCalls.count(id) == 0)
                throw std::runtime_error("Saved Responses output is missing an executed tool call.");
        for (std::size_t i = 0; i < sidecar->size(); ++i)
            input->add(NormalizeFunctionCallNamespace(sidecar->getObject(i), chatgptPlan));
        for (const auto& id : skipped) {
            Object::Ptr result = new Object;
            result->set("type", "function_call_output");
            result->set("call_id", id);
            result->set("output", "This tool call was not executed: LlamaBoss selected only part of this batch. Request it again if still needed.");
            input->add(result);
        }
        return;
    }

    if (!visibleText.empty()) {
        Object::Ptr msg = new Object;
        msg->set("role", "assistant");
        msg->set("content", visibleText);
        input->add(msg);
    }
    // Legacy history has call ids, but no Responses item ids to replay.
    for (const auto& id : order) {
        const auto fn = surviving[id];
        Object::Ptr fc = new Object;
        fc->set("type", "function_call");
        fc->set("call_id", id);
        fc->set("name", RequiredString(fn, "name"));
        fc->set("arguments", fn->has("arguments") && !fn->isNull("arguments")
            ? RequiredString(fn, "arguments") : std::string("{}"));
        input->add(NormalizeFunctionCallNamespace(fc, chatgptPlan));
    }
}

// chatgptPlan: the request rides a ChatGPT Plus/Pro plan through Sign in
// with ChatGPT.  That route (siwc "Preview limitations") additionally:
//   * rejects explicit system-role items -> the leading system prompt
//     becomes top-level `instructions`, any later system message a
//     developer message;
//   * rejects max_output_tokens (and other fields this converter never
//     emits: temperature, top_p, metadata, truncation, user, ...);
//   * requires function tools to be grouped in a namespace.
// store:false and stream:true are required there and are already the
// converter's behavior for every Responses target.
inline std::string BuildChatRequest(const std::string& chatJson, bool chatgptPlan = false)
{
    Poco::JSON::Parser parser;
    const auto chat = parser.parse(chatJson).extract<Object::Ptr>();
    if (!chat) throw std::runtime_error("Invalid chat request.");
    if (chat->has("modalities") || chat->has("functions"))
        throw std::runtime_error("This Responses connection supports text chat and function tools only.");

    Object::Ptr request = new Object;
    const std::string model = RequiredString(chat, "model");
    request->set("model", model);
    request->set("stream", chat->optValue<bool>("stream", true));
    // LlamaBoss owns the conversation. Do not introduce server-side history
    // or previous_response_id state that could cross chats or endpoints.
    request->set("store", false);
    // Per-conversation cache routing key from ChatHistory (see
    // BuildChatRequestJson).  Absent for callers that do not set it.
    if (chat->has("prompt_cache_key") && chat->get("prompt_cache_key").isString())
        request->set("prompt_cache_key", chat->getValue<std::string>("prompt_cache_key"));
    if (chat->has("reasoning_effort") && !chat->isNull("reasoning_effort")) {
        Object::Ptr reasoning = new Object;
        reasoning->set("effort", lb_reasoning::OpenAIEffort(
            model, RequiredString(chat, "reasoning_effort")));
        request->set("reasoning", reasoning);
    }
    // Auto omits the override. Do not request reasoning summaries: availability
    // can depend on account verification. Consume summaries if provided.
    if (!chatgptPlan) {
        if (chat->has("max_completion_tokens"))
            request->set("max_output_tokens", chat->get("max_completion_tokens"));
        else if (chat->has("max_tokens"))
            request->set("max_output_tokens", chat->get("max_tokens"));
    }

    if (chat->has("tools") && !chat->isNull("tools")) {
        const auto tools = chat->getArray("tools");
        if (tools && tools->size() > 0) {
            request->set("tools", chatgptPlan ? WrapToolsInNamespace(ConvertTools(tools))
                                              : ConvertTools(tools));
            if (chat->has("parallel_tool_calls") && !chat->isNull("parallel_tool_calls"))
                request->set("parallel_tool_calls", chat->get("parallel_tool_calls"));
            // store:false means the provider keeps nothing between calls;
            // the encrypted reasoning item is the only way to hand its
            // chain of thought back on the next iteration of a tool loop.
            Array::Ptr include = new Array;
            include->add(std::string("reasoning.encrypted_content"));
            request->set("include", include);
        }
    }

    const auto messages = chat->getArray("messages");
    if (!messages) throw std::runtime_error("Chat request has no messages array.");
    Array::Ptr input = new Array;
    for (std::size_t i = 0; i < messages->size(); ++i) {
        const auto old = messages->getObject(i);
        const std::string role = RequiredString(old, "role");

        // Native tool-result reply -> function_call_output.
        if (role == "tool") {
            Object::Ptr out = new Object;
            out->set("type", "function_call_output");
            out->set("call_id", RequiredString(old, "tool_call_id"));
            if (!old->has("content") || old->isNull("content") || !old->get("content").isString())
                throw std::runtime_error("Tool result message has no text content.");
            out->set("output", old->getValue<std::string>("content"));
            input->add(out);
            continue;
        }
        if (role != "system" && role != "developer" && role != "user" && role != "assistant")
            throw std::runtime_error("Unsupported message role for this Responses connection: " + role);
        if (old->has("function_call"))
            throw std::runtime_error("Legacy function_call history is not supported on this Responses connection.");
        if (old->has("tool_call_id"))
            throw std::runtime_error("Unexpected tool_call_id on a non-tool message.");

        // Assistant turn that emitted native tool calls.
        if (role == "assistant" && old->has("tool_calls") && !old->isNull("tool_calls")) {
            std::string visible;
            if (old->has("content") && !old->isNull("content") && old->get("content").isString())
                visible = old->getValue<std::string>("content");
            AppendAssistantToolTurn(input, old, visible, chatgptPlan);
            continue;
        }

        // Plan route: no system-role items.  The leading system prompt
        // (ChatHistory always puts it first) becomes `instructions`;
        // any later system message keeps its position as a developer
        // message, which that route accepts.
        if (chatgptPlan && role == "system") {
            if (i == 0 && old->has("content") && !old->isNull("content") &&
                old->get("content").isString()) {
                request->set("instructions", old->getValue<std::string>("content"));
                continue;
            }
        }

        Object::Ptr msg = new Object;
        msg->set("role", (chatgptPlan && role == "system") ? std::string("developer") : role);
        if (!old->has("content") || old->isNull("content"))
            throw std::runtime_error("Chat message has no content.");
        if (old->get("content").isString()) {
            // Easy input messages accept text for all four supported roles.
            msg->set("content", old->getValue<std::string>("content"));
        } else {
            const auto parts = old->getArray("content");
            if (!parts) throw std::runtime_error("Unsupported chat content.");
            Array::Ptr converted = new Array;
            for (std::size_t j = 0; j < parts->size(); ++j) {
                const auto part = parts->getObject(j);
                const std::string type = RequiredString(part, "type");
                Object::Ptr item = new Object;
                if (type == "text") {
                    item->set("type", "input_text");
                    item->set("text", RequiredString(part, "text"));
                } else if (type == "image_url" && role == "user") {
                    const auto url = part->getObject("image_url");
                    item->set("type", "input_image");
                    item->set("image_url", RequiredString(url, "url"));
                    if (url->has("detail")) item->set("detail", RequiredString(url, "detail"));
                } else {
                    throw std::runtime_error("Unsupported attachment type on this Responses connection.");
                }
                converted->add(item);
            }
            msg->set("content", converted);
        }
        input->add(msg);
    }
    request->set("input", input);
    // Whitelist only the fields above: stream_options, sampling, and
    // chat_template_kwargs must never leak here.
    return Stringify(request);
}

struct StreamUpdate {
    std::string text;
    std::string summary;
    std::string error;
    bool completed = false;
    long inputTokens = -1;
    long outputTokens = -1;
    long cachedInputTokens = -1;   // usage.input_tokens_details.cached_tokens
    long reasoningTokens = -1;     // usage.output_tokens_details.reasoning_tokens
    // Set on completion only.  toolCallsJson is the Chat Completions
    // shape the rest of LlamaBoss already consumes
    // ([{"id":call_id,"type":"function","function":{name,arguments}}]);
    // outputJson is the verbatim `output` array of the completed
    // response, populated only when it contains at least one
    // function_call (that is the only case where replaying it is
    // required, and it keeps conversation files small otherwise).
    std::string toolCallsJson;
    std::string outputJson;
};

class ChatStream {
public:
    StreamUpdate Consume(const Object::Ptr& event)
    {
        StreamUpdate update;
        if (m_terminal) throw std::runtime_error("Responses event arrived after completion.");
        const std::string type = RequiredString(event, "type");
        if (type == "error") {
            update.error = event->optValue<std::string>("message", "Responses stream error.");
            if (update.error.empty()) update.error = "Responses stream error.";
            m_terminal = true;
            return update;
        }
        if (type == "response.failed" || type == "response.incomplete" || type == "response.cancelled") {
            const auto response = event->getObject("response");
            update.error = "Responses request " + type.substr(9) + ".";
            if (response) {
                const auto error = response->getObject("error");
                const auto details = response->getObject("incomplete_details");
                if (error) update.error += " " + error->optValue<std::string>("message", "");
                if (details) update.error += " Reason: " + details->optValue<std::string>("reason", "unknown");
                // ChatGPT plan codes (usage limit, eligibility, ...) can
                // arrive mid-stream as response.failed; say what to do.
                if (error) {
                    const std::string code = error->optValue<std::string>("code", "");
                    const std::string plain = lb_chatgpt::DescribePlanErrorCode(code);
                    if (!plain.empty()) update.error = plain + " [" + code + "]";
                }
            }
            m_terminal = true;
            return update;
        }
        const bool summary = type == "response.reasoning_summary_text.delta" ||
                             type == "response.reasoning_summary_text.done";
        const bool delta = type == "response.output_text.delta" ||
                           type == "response.refusal.delta" ||
                           type == "response.reasoning_summary_text.delta";
        const bool done = type == "response.output_text.done" ||
                          type == "response.refusal.done" ||
                          type == "response.reasoning_summary_text.done";
        if (delta || done) {
            const int outputIndex = Index(event, "output_index");
            const int partIndex = Index(event, summary ? "summary_index" : "content_index");
            const char* field = delta ? "delta" : type == "response.refusal.done" ? "refusal" : "text";
            const auto addition = AddPart(outputIndex, summary, partIndex,
                RequiredString(event, field), done);
            if (summary) update.summary = addition;
            else update.text = addition;
        } else if (type == "response.function_call_arguments.delta") {
            auto& call = CallSlot(Index(event, "output_index"));
            Adopt(call.itemId, RequiredString(event, "item_id"), "id");
            if (call.argumentsDone)
                throw std::runtime_error("Responses function arguments continued after they ended.");
            AppendArguments(call, RequiredString(event, "delta"));
        } else if (type == "response.function_call_arguments.done") {
            auto& call = CallSlot(Index(event, "output_index"));
            Adopt(call.itemId, RequiredString(event, "item_id"), "id");
            SetFinalArguments(call, RequiredString(event, "arguments"));
        } else if (type == "response.output_item.added" || type == "response.output_item.done") {
            const auto item = event->getObject("item");
            CheckItem(item);
            if (RequiredString(item, "type") == "function_call") {
                auto& call = CallSlot(Index(event, "output_index"));
                RecordCall(call, item, /*snapshot*/ type == "response.output_item.done");
            }
            // Keep every finished item.  The ChatGPT plan route ends with a
            // response.completed whose `output` is EMPTY (the Codex backend
            // streams items only), so these are the only complete copy of
            // the reply -- including the encrypted reasoning items a tool
            // loop must replay.
            if (type == "response.output_item.done") {
                const int index = Index(event, "output_index");
                if (m_doneItems.count(index) == 0 && m_doneItems.size() >= 1024)
                    throw std::runtime_error("Responses stream contains too many output items.");
                m_doneItems[index] = item;
            }
        } else if (type == "response.completed") {
            const auto response = event->getObject("response");
            if (!response || RequiredString(response, "status") != "completed")
                throw std::runtime_error("Responses completion has no completed response.");
            const auto output = response->getArray("output");
            if (!output && m_doneItems.empty())
                throw std::runtime_error("Responses completion has no output array.");
            // Final items by output index: the streamed output_item.done
            // copies, overridden by the completion's own output array when
            // the server repeats it (api.openai.com with an API key does;
            // the ChatGPT plan route sends an empty array).
            std::map<int, Object::Ptr> finalItems = m_doneItems;
            const bool outputRepeated = output && output->size() > 0;
            if (outputRepeated)
                for (std::size_t i = 0; i < output->size(); ++i)
                    finalItems[static_cast<int>(i)] = output->getObject(i);
            Array::Ptr finalOutput = new Array;
            for (const auto& entry : finalItems) finalOutput->add(entry.second);
            std::set<PartKey> finalParts;
            std::set<int> finalCalls;
            std::set<std::string> finalCallIds, finalItemIds;
            Array::Ptr toolCalls = new Array;
            // Completion repeats the final text. Verify/fill any missing suffix
            // by content part instead of rendering it twice. Also supports an
            // endpoint sending only the final completed snapshot.
            for (const auto& finalEntry : finalItems) {
                const int i = finalEntry.first;
                const auto item = finalEntry.second;
                CheckItem(item);
                const std::string itemType = RequiredString(item, "type");
                if (itemType == "function_call") {
                    auto& call = CallSlot(i);
                    RecordCall(call, item, /*snapshot*/ true);
                    if (!finalCallIds.insert(call.callId).second ||
                        (!call.itemId.empty() && !finalItemIds.insert(call.itemId).second))
                        throw std::runtime_error("Responses completion contains duplicate function call ids.");
                    finalCalls.insert(i);
                    Object::Ptr entry = new Object;
                    entry->set("id", call.callId);
                    entry->set("type", "function");
                    Object::Ptr fn = new Object;
                    fn->set("name", call.name);
                    fn->set("arguments", call.arguments);
                    entry->set("function", fn);
                    toolCalls->add(entry);
                    continue;
                }
                const bool isSummary = itemType == "reasoning";
                const auto parts = item->getArray(isSummary ? "summary" : "content");
                if (!parts) {
                    if (isSummary) continue;
                    throw std::runtime_error("Responses message has no content array.");
                }
                for (std::size_t j = 0; j < parts->size(); ++j) {
                    const auto part = parts->getObject(j);
                    const auto partType = RequiredString(part, "type");
                    if (partType != "output_text" && partType != "refusal" && partType != "summary_text")
                        throw std::runtime_error("Unsupported Responses output content.");
                    finalParts.emplace(i, isSummary, static_cast<int>(j));
                    const auto addition = AddPart(i, isSummary,
                        static_cast<int>(j), RequiredString(part, partType == "refusal" ? "refusal" : "text"), true);
                    if (isSummary) update.summary += addition;
                    else update.text += addition;
                }
            }
            // Text already streamed (and shown) that no final item covers is
            // only an error when the server claimed to repeat the whole
            // output.  Without that repeat, the deltas ARE the record.
            // Function calls are still strict below: one is never run
            // without its finished item.
            for (const auto& part : m_parts) {
                if (outputRepeated && !part.second.text.empty() && finalParts.count(part.first) == 0)
                    throw std::runtime_error("Responses completion omitted streamed content.");
            }
            for (const auto& call : m_calls) {
                if (finalCalls.count(call.first) == 0)
                    throw std::runtime_error("Responses completion omitted a streamed function call.");
            }
            const auto usage = response->getObject("usage");
            if (usage) {
                update.inputTokens = ReadTokens(usage, "input_tokens");
                update.outputTokens = ReadTokens(usage, "output_tokens");
                // Optional detail objects; absent on many providers.
                try {
                    if (usage->has("input_tokens_details") &&
                        !usage->isNull("input_tokens_details")) {
                        const auto d = usage->getObject("input_tokens_details");
                        if (d) update.cachedInputTokens = ReadTokens(d, "cached_tokens");
                    }
                    if (usage->has("output_tokens_details") &&
                        !usage->isNull("output_tokens_details")) {
                        const auto d = usage->getObject("output_tokens_details");
                        if (d) update.reasoningTokens = ReadTokens(d, "reasoning_tokens");
                    }
                } catch (...) { /* malformed details: counts stay -1 */ }
            }
            if (toolCalls->size() > 0) {
                update.toolCallsJson = Stringify(toolCalls);
                update.outputJson = Stringify(finalOutput);
            } else if (!m_visibleText) {
                throw std::runtime_error("Responses completed without an answer. Try Low or resend the message.");
            }
            m_terminal = true;
            update.completed = true;
        } else if (type.find("custom_tool_call") != std::string::npos ||
                   type.find("web_search_call") != std::string::npos ||
                   type.find("file_search_call") != std::string::npos ||
                   type.find("mcp_call") != std::string::npos ||
                   type.find("computer_call") != std::string::npos ||
                   type.find("code_interpreter_call") != std::string::npos) {
            throw std::runtime_error("This Responses connection supports function tools only; received: " + type);
        }
        // Lifecycle/annotation/encrypted reasoning events contain no display
        // text. They are intentionally ignored, never mistaken for completion.
        return update;
    }

private:
    using PartKey = std::tuple<int, bool, int>;
    struct Part { std::string text; bool done = false; };
    struct Call {
        std::string itemId;     // "fc_..." (may be empty on some providers)
        std::string callId;     // "call_..." -- what function_call_output threads on
        std::string name;
        std::string arguments;
        bool argumentsDone = false;
        std::size_t repetitionCheckedAt = 0;
    };
    std::map<PartKey, Part> m_parts;
    std::map<int, Call> m_calls;    // keyed by output_index
    std::map<int, Object::Ptr> m_doneItems;   // output_item.done, by output_index
    std::size_t m_bytes = 0;
    bool m_visibleText = false;
    bool m_terminal = false;

    static int Index(const Object::Ptr& obj, const char* name)
    {
        if (!obj->has(name) || obj->isNull(name) || !obj->get(name).isInteger())
            throw std::runtime_error("Responses event is missing its output index.");
        const int index = obj->getValue<int>(name);
        if (index < 0 || index >= 1024) throw std::runtime_error("Responses output index is out of range.");
        return index;
    }
    static long ReadTokens(const Object::Ptr& usage, const char* key)
    {
        if (!usage->has(key) || usage->isNull(key)) return -1;
        try {
            const long value = usage->getValue<long>(key);
            return value >= 0 ? value : -1;
        } catch (const Poco::Exception&) { return -1; }
    }
    static void CheckItem(const Object::Ptr& item)
    {
        const auto type = RequiredString(item, "type");
        if (type != "message" && type != "reasoning" && type != "function_call")
            throw std::runtime_error("This Responses connection supports function tools only; received an unsupported output item: " + type);
    }
    Call& CallSlot(int outputIndex)
    {
        if (m_calls.count(outputIndex) == 0 && m_calls.size() >= 128)
            throw std::runtime_error("Responses stream contains too many function calls.");
        return m_calls[outputIndex];
    }
    void Budget(std::size_t addition)
    {
        constexpr std::size_t kMaxTextBytes = 64ull * 1024 * 1024;
        if (addition > kMaxTextBytes - m_bytes)
            throw std::runtime_error("Responses answer exceeded the text size limit.");
        m_bytes += addition;
    }
    void AppendArgumentSuffix(Call& call, const std::string& text, std::size_t offset)
    {
        const std::size_t addition = text.size() - offset;
        if (addition > kMaxToolArgumentBytes - call.arguments.size())
            throw std::runtime_error(
                "Stopped this reply: the model's tool-call arguments passed 4 MB, "
                "which usually means it is stuck in a loop. The tool call was not run.");
        Budget(addition);

        // Check the same output policy every 512 bytes, even when a provider
        // sends a large delta or only a final snapshot. Checking only the
        // final tail would let a closing JSON brace hide a repeating body.
        while (offset < text.size()) {
            const std::size_t untilCheck = 512 -
                (call.arguments.size() - call.repetitionCheckedAt);
            const std::size_t count = (std::min)(untilCheck, text.size() - offset);
            call.arguments.append(text, offset, count);
            offset += count;
            if (call.arguments.size() - call.repetitionCheckedAt >= 512) {
                call.repetitionCheckedAt = call.arguments.size();
                if (repetition_guard::EndsInRepetitionLoop(
                        call.arguments, repetition_guard::OutputPolicy())) {
                    m_terminal = true;
                    throw ToolArgumentRepetition();
                }
            }
        }
    }
    void AppendArguments(Call& call, const std::string& delta)
    {
        AppendArgumentSuffix(call, delta, 0);
    }
    // Final arguments must extend what was streamed, exactly like text.
    void SetFinalArguments(Call& call, const std::string& full)
    {
        if (call.argumentsDone && full != call.arguments)
            throw std::runtime_error("Responses function arguments changed after they ended.");
        if (full.compare(0, call.arguments.size(), call.arguments) != 0)
            throw std::runtime_error("Responses final function arguments did not match the streamed arguments.");
        AppendArgumentSuffix(call, full, call.arguments.size());
        call.argumentsDone = true;
    }
    static void Adopt(std::string& slot, const std::string& value, const char* what)
    {
        if (value.empty()) return;
        if (slot.empty()) { slot = value; return; }
        if (slot != value)
            throw std::runtime_error(std::string("Responses function call ") + what + " changed mid-stream.");
    }
    void RecordCall(Call& call, const Object::Ptr& item, bool snapshot)
    {
        if (item->has("id") && !item->isNull("id"))
            Adopt(call.itemId, RequiredString(item, "id"), "id");
        Adopt(call.callId, RequiredString(item, "call_id"), "call_id");
        Adopt(call.name, RequiredString(item, "name"), "name");
        if (snapshot) {
            if (item->has("status") && !item->isNull("status") &&
                RequiredString(item, "status") != "completed")
                throw std::runtime_error("Responses final function call is not completed.");
            SetFinalArguments(call, RequiredString(item, "arguments"));
        } else if (item->has("arguments") && !item->isNull("arguments")) {
            const std::string args = RequiredString(item, "arguments");
            if (!args.empty() && call.arguments.empty()) AppendArguments(call, args);
        }
        if (call.callId.empty()) throw std::runtime_error("Responses function call has no call_id.");
        if (call.name.empty()) throw std::runtime_error("Responses function call has no name.");
    }
    std::string AddPart(int output, bool summary, int index,
                        const std::string& text, bool snapshot)
    {
        if (output < 0 || output >= 1024 || index < 0 || index >= 1024)
            throw std::runtime_error("Responses content index is out of range.");
        const PartKey key{output, summary, index};
        if (m_parts.count(key) == 0 && m_parts.size() >= 1024)
            throw std::runtime_error("Responses stream contains too many content parts.");
        auto& part = m_parts[key];
        std::string addition;
        if (snapshot) {
            if (text.compare(0, part.text.size(), part.text) != 0)
                throw std::runtime_error("Responses final text did not match the streamed answer.");
            addition = text.substr(part.text.size());
            part.done = true;
        } else {
            if (part.done) throw std::runtime_error("Responses text continued after its content part ended.");
            addition = text;
        }
        Budget(addition.size());
        part.text += addition;
        if (!summary && !addition.empty()) m_visibleText = true;
        return addition;
    }
};
} // namespace lb_responses
