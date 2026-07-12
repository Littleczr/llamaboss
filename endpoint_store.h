// endpoint_store.h
//
// Persisted configuration for remote, OpenAI-compatible inference
// endpoints (OpenRouter, OpenAI, etc.). Sibling to SecretsStore:
// EndpointStore holds the NON-secret connection config — base URL,
// chat path, headers, protocol, and the list of selectable models —
// plus which SecretsStore provider/key holds the API key. The key
// itself never lives here; it stays in SecretsStore.
//
// One JSON file at:
//     %LOCALAPPDATA%\LlamaBoss\endpoints.json
//
//   {
//     "version": 1,
//     "endpoints": [
//       {
//         "id": "openrouter",
//         "display_name": "OpenRouter",
//         "base_url": "https://openrouter.ai/api",
//         "chat_path": "/v1/chat/completions",
//         "auth_scheme": "bearer",
//         "secret_provider": "openrouter",
//         "secret_key": "api_key",
//         "protocol": "native",
//         "extra_headers": { "HTTP-Referer": "https://llamaboss.com",
//                            "X-Title": "LlamaBoss" },
//         "models": [
//           { "id": "anthropic/claude-sonnet-4.6",
//             "display_name": "Claude Sonnet 4.6" },
//           { "id": "google/gemini-2.5-flash-image",
//             "display_name": "Nano Banana",
//             "image_output": true }
//         ]
//       }
//     ]
//   }
//
// Lifetime:
//   * Owned by AppState (one per process), lazily constructed.
//   * UI-thread mutation. ResolveTarget() reads SecretsStore (which is
//     UI-thread-only), so build targets on the UI thread before handing
//     them to the worker.
//
#pragma once

#include <string>
#include <vector>
#include <utility>

#include "tool_protocol.h"      // ToolProtocol
#include "inference_target.h"   // InferenceTarget (ResolveTarget output)

class SecretsStore;

class EndpointStore
{
public:
    // One selectable model offered by an endpoint.
    struct Model {
        std::string id;            // wire model id, e.g. "anthropic/claude-sonnet-4.6"
        std::string displayName;   // human label for the picker

        // True for image-generation models served over chat
        // completions (OpenRouter's google/gemini-*-image family,
        // FLUX, etc.).  Selecting such a model makes the request
        // builder omit the tool catalog (providers for these models
        // reject tool-bearing requests) and add
        // "modalities": ["image", "text"] so the provider actually
        // returns image data.  Persisted as "image_output": true.
        bool imageOutput = false;
    };

    // How the API key is presented on the wire.
    enum class AuthScheme {
        Bearer,     // Authorization: Bearer <key>   (OpenAI, OpenRouter)
        XApiKey     // x-api-key: <key>              (Anthropic-native, future)
    };

    struct Endpoint {
        std::string  id;                  // stable internal id, e.g. "openrouter"
        std::string  displayName;         // "OpenRouter"
        std::string  baseUrl;             // "https://openrouter.ai/api"
        std::string  chatPath = "/v1/chat/completions";
        AuthScheme   authScheme = AuthScheme::Bearer;
        std::string  secretProvider;      // SecretsStore provider, e.g. "openrouter"
        std::string  secretKey = "api_key";
        ToolProtocol protocol = ToolProtocol::Native;
        std::vector<std::pair<std::string, std::string>> extraHeaders;
        std::vector<Model> models;
    };

    EndpointStore() = default;
    ~EndpointStore() = default;

    // Absolute path to endpoints.json (creates the parent dir on first
    // call; never creates the file itself).
    static std::string GetEndpointsFilePath();

    // Load from disk. A MISSING file seeds a single default OpenRouter
    // endpoint so the picker has something usable before the user opens
    // the editor. A present-but-empty file is respected as-is (so a user
    // who deletes every endpoint doesn't get one re-seeded). Malformed
    // JSON logs a warning and starts from the seeded default.
    bool Load();

    // Atomically rewrite endpoints.json from the in-memory list.
    bool Save();

    const std::vector<Endpoint>& Endpoints() const { return m_endpoints; }

    // Find by id; nullptr if absent.
    const Endpoint* FindEndpoint(const std::string& id) const;

    // Add a new endpoint or replace an existing one with the same id.
    void UpsertEndpoint(const Endpoint& ep);

    // Remove an endpoint by id (no-op if absent).
    void RemoveEndpoint(const std::string& id);

    // Resolve an (endpoint id, wire model id) pair into a ready-to-send
    // InferenceTarget, pulling the API key from SecretsStore. Returns
    // false with a human-readable reason when the endpoint is unknown or
    // no key is configured. useTls is derived from the base URL scheme.
    bool ResolveTarget(const std::string&  endpointId,
                       const std::string&  wireModelId,
                       const SecretsStore& secrets,
                       InferenceTarget&    out,
                       std::string&        outReason) const;

private:
    void SeedDefaults();   // single default OpenRouter endpoint

    std::vector<Endpoint> m_endpoints;
    bool m_loaded = false;
};
