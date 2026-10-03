// inference_target.h
//
// Describes WHERE one chat turn is sent and HOW to authenticate.
// A single InferenceTarget is resolved per send — produced either by
// the local llama-server lane (managed = true, no auth, plain http)
// or by a configured remote endpoint (managed = false, TLS, auth
// header).
//
// This is a plain value type with no behavior: the transport
// (ChatWorkerThread) reads its fields to build the outbound HTTP
// request. Header-only on purpose — a translation unit would be
// overkill for what is effectively a struct.
//
#pragma once

#include <string>
#include <vector>
#include <utility>

#include "tool_protocol.h"   // ToolProtocol

// How a remote OpenAI-compatible endpoint expects reasoning control
// (/think) to be expressed on the request body.  Resolved per target
// by EndpointStore::ResolveTarget — an explicit "reasoning_dialect"
// override in endpoints.json wins, else the base URL is sniffed —
// and consumed by ChatHistory::BuildChatRequestJson whenever the
// conversation's /think mode is not auto.
//
//   OpenRouterStyle : top-level "reasoning" object —
//                     {"enabled": bool} for on/off, {"effort": "..."}
//                     for low/medium/high.  OpenRouter translates the
//                     object per provider, so it is the safe default
//                     for every remote except direct OpenAI.
//   OpenAIStyle     : top-level "reasoning_effort" string —
//                     "none" | "low" | "medium" | "high".  Direct
//                     api.openai.com REJECTS unknown body fields, so
//                     the OpenRouter object 400s there; conversely
//                     "none" is the documented way to unblock
//                     function tools on models whose server-side
//                     default reasoning effort conflicts with tools
//                     (gpt-5.6-luna on /v1/chat/completions).
//   TemplateKwargs  : "chat_template_kwargs": {"enable_thinking": bool}
//                     — the same wire shape the local llama-server
//                     lane uses, selectable for REMOTE
//                     OpenAI-compatible servers that apply the
//                     model's own HF chat template (FreeToken, vLLM,
//                     SGLang).  Chat templates speak only a boolean,
//                     so the effort levels collapse to enabled.
//                     Endpoint opt-in only ("reasoning_dialect":
//                     "template") — never chosen by the sniff.
enum class ReasoningDialect {
    OpenRouterStyle,
    OpenAIStyle,
    TemplateKwargs
};

struct InferenceTarget
{
    // Non-empty means resolution failed. ChatClient reports this through the
    // normal error event without starting a worker or transmitting any data.
    std::string resolutionError;

    // ── Transport ────────────────────────────────────────────────
    // baseUrl is scheme + host + optional port, no trailing slash:
    //   local :  "http://127.0.0.1:8384"
    //   remote:  "https://openrouter.ai/api"
    // chatPath is appended verbatim. The OpenAI-compatible default
    // suits llama-server, OpenAI, and OpenRouter alike; a future
    // Anthropic-native adapter would override it with "/v1/messages".
    std::string baseUrl;
    std::string chatPath = "/v1/chat/completions";

    // Responses uses a different request body and typed SSE events.
    // Resolved from the endpoint path (or direct OpenAI Astra routing),
    // independently of the model's tool-call protocol. Transient only.
    bool responsesApi = false;

    // When true the worker opens an HTTPSClientSession and initializes
    // SSL first. Set by whoever builds the target from the endpoint's
    // scheme; the back-compat local builder leaves it false.
    bool useTls = false;

    // ── Auth (empty for local) ───────────────────────────────────
    // authHeaderValue is the FULLY-FORMED header value, resolved on
    // the UI thread before the worker launches (SecretsStore is
    // UI-thread-only — never read it from the worker). Examples:
    //   OpenAI / OpenRouter:  name "Authorization", value "Bearer sk-..."
    //   Anthropic (future) :  name "x-api-key",      value "sk-ant-..."
    std::string authHeaderName;
    std::string authHeaderValue;

    // Any additional fixed headers a provider requires, e.g.
    //   { "anthropic-version", "2023-06-01" }
    // Applied verbatim after the auth header.
    std::vector<std::pair<std::string, std::string>> extraHeaders;

    // ── Behavior ─────────────────────────────────────────────────
    // managed == true  : a local llama-server lane we spawn and
    //                     health-check.
    // managed == false : a remote endpoint with no process lifecycle
    //                     of ours (skip spawn, skip /health, skip the
    //                     OnServerReady gate, skip /props detection).
    // The transport itself does not consult this flag — it exists for
    // the upstream model-source fork (a later checkpoint). It travels
    // on the target so the producer's intent is explicit end to end.
    bool managed = true;

    // For local lanes this is detected via /props + smoke test
    // upstream. For remote endpoints it is forced by the provider
    // (OpenAI / OpenRouter -> Native). Carried for upstream use; the
    // transport does not read it.
    ToolProtocol protocol = ToolProtocol::Unknown;

    // The wire "model" field. llama-server ignores it (it serves
    // whatever GGUF is loaded); remote providers require it, e.g.
    // "anthropic/claude-3.5-sonnet" or "gpt-4o-mini".
    std::string modelId;

    // True when the selected remote model generates images over chat
    // completions (per-model "image_output" flag in endpoints.json).
    // Consumers:
    //   * the send path omits the tool catalog for the turn — image
    //     models have no tool-supporting providers, and OpenRouter
    //     404s any request whose feature set no provider satisfies;
    //   * the request builder adds "modalities": ["image", "text"];
    //   * the stream parser collects delta.images / message.images
    //     and saves them to the conversation chat folder.
    // Always false for local lanes.
    bool imageOutput = false;

    // True when the selected remote model is explicitly configured as
    // chat/reasoning-only (per-model "no_tools" in endpoints.json).
    // Upstream orchestration uses this to bypass the complete Agent
    // path -- native tools catalog, XML tool prompt, and tool loop --
    // while leaving the ordinary chat request and reasoning behavior
    // untouched.  Always false for local lanes.
    bool noTools = false;

    // Reasoning-control dialect for /think on remote lanes (see the
    // enum above).  Ignored for local lanes — those are addressed via
    // chat_template_kwargs.enable_thinking, selected by the request
    // builder's own .gguf model check.  Defaults to the historical
    // OpenRouter-style object so unresolved/legacy targets behave
    // exactly as before.
    ReasoningDialect reasoningDialect = ReasoningDialect::OpenRouterStyle;

    // Build the default LOCAL target that reproduces the historical
    // SendMessage(model, apiUrl, ...) behavior exactly: plain http,
    // no auth, OpenAI-compatible path, managed lane. Used by the
    // back-compat ChatClient::SendMessage overload so existing call
    // sites keep their current behavior bit-for-bit.
    static InferenceTarget Local(const std::string& apiUrl,
                                 const std::string& model)
    {
        InferenceTarget t;
        t.baseUrl = apiUrl;
        t.modelId = model;
        t.managed = true;
        t.useTls  = false;
        // protocol intentionally left Unknown: the transport never
        // reads it, and local protocol selection lives in tool_protocol.
        return t;
    }
};

// ── Context budget per lane (2026-10-01) ────────────────────────
// Local models use the Settings context length, which is also what
// llama-server is launched with.  Remote endpoints have no launch
// length and used to inherit that same local number, so a remote model
// with a far larger window had old tool results elided at ~68k real
// tokens and then re-read them.  Remote lanes now get one fixed
// budget instead.  It is a COST ceiling as much as a size: every
// prompt token is billed per request, so it is deliberately not the
// provider's full window.  Applies to the meter, the elision budget
// and the ctx-aware read caps alike.
constexpr int kRemoteContextTokens = 262144;   // 256k

inline int ContextTokensForLane(bool remote, int localCtxTokens)
{
    if (remote) return kRemoteContextTokens;
    return localCtxTokens > 0 ? localCtxTokens : 8192;   // defensive
}
