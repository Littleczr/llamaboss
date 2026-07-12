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

struct InferenceTarget
{
    // ── Transport ────────────────────────────────────────────────
    // baseUrl is scheme + host + optional port, no trailing slash:
    //   local :  "http://127.0.0.1:8384"
    //   remote:  "https://openrouter.ai/api"
    // chatPath is appended verbatim. The OpenAI-compatible default
    // suits llama-server, OpenAI, and OpenRouter alike; a future
    // Anthropic-native adapter would override it with "/v1/messages".
    std::string baseUrl;
    std::string chatPath = "/v1/chat/completions";

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
    //     and saves them to the conversation workflow folder.
    // Always false for local lanes.
    bool imageOutput = false;

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
