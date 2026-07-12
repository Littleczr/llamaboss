// endpoint_store.cpp
#define _CRT_SECURE_NO_WARNINGS

#include "endpoint_store.h"
#include "secrets_store.h"

#include <wx/wx.h>
#include <wx/stdpaths.h>
#include <wx/filename.h>
#include <wx/log.h>

#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Stringifier.h>
#include <Poco/Dynamic/Var.h>

#include <fstream>
#include <sstream>
#include <algorithm>

namespace {

// ── File IO (same shape as SecretsStore's helpers) ───────────────
// Kept local rather than shared to keep this checkpoint self-contained;
// a later cleanup can factor ReadWholeFile/WriteWholeFileAtomic into a
// common lb_file_io unit used by both stores.
std::string ReadWholeFile(const wxString& path)
{
    std::ifstream f(path.fn_str(), std::ios::in | std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool WriteWholeFileAtomic(const wxString& path, const std::string& body)
{
    wxString tmpPath = path + ".tmp";
    {
        std::ofstream f(tmpPath.fn_str(),
                        std::ios::out | std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(body.data(), static_cast<std::streamsize>(body.size()));
        if (!f) return false;
    }
    return wxRenameFile(tmpPath, path, true);
}

// ── enum <-> string ──────────────────────────────────────────────
std::string AuthSchemeToString(EndpointStore::AuthScheme s)
{
    return (s == EndpointStore::AuthScheme::XApiKey) ? "x-api-key" : "bearer";
}

EndpointStore::AuthScheme AuthSchemeFromString(const std::string& s)
{
    return (s == "x-api-key") ? EndpointStore::AuthScheme::XApiKey
                              : EndpointStore::AuthScheme::Bearer;
}

std::string ProtocolToString(ToolProtocol p)
{
    // Endpoints are remote and OpenAI-compatible -> native by default.
    return (p == ToolProtocol::Xml) ? "xml" : "native";
}

ToolProtocol ProtocolFromString(const std::string& s)
{
    return (s == "xml") ? ToolProtocol::Xml : ToolProtocol::Native;
}

} // namespace

// ─── Path resolution ────────────────────────────────────────────

std::string EndpointStore::GetEndpointsFilePath()
{
    wxString localData = wxStandardPaths::Get().GetUserLocalDataDir();
    if (!wxDirExists(localData)) {
        wxFileName::Mkdir(localData, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    }
    wxFileName fn(localData, "endpoints.json");
    return std::string(fn.GetFullPath().ToUTF8().data());
}

// ─── Defaults ───────────────────────────────────────────────────

void EndpointStore::SeedDefaults()
{
    m_endpoints.clear();

    Endpoint openrouter;
    openrouter.id             = "openrouter";
    openrouter.displayName    = "OpenRouter";
    openrouter.baseUrl        = "https://openrouter.ai/api";
    openrouter.chatPath       = "/v1/chat/completions";
    openrouter.authScheme     = AuthScheme::Bearer;
    openrouter.secretProvider = "openrouter";
    openrouter.secretKey      = "api_key";
    openrouter.protocol       = ToolProtocol::Native;
    openrouter.extraHeaders   = {
        { "HTTP-Referer", "https://llamaboss.com" },
        { "X-Title",      "LlamaBoss" },
    };
    openrouter.models = {
        { "anthropic/claude-sonnet-4.6", "Claude Sonnet 4.6" },
        { "openai/gpt-4o-mini",          "GPT-4o mini" },
    };

    m_endpoints.push_back(std::move(openrouter));
}

// ─── Load / Save ────────────────────────────────────────────────

bool EndpointStore::Load()
{
    m_endpoints.clear();
    m_loaded = true;

    wxString path = wxString::FromUTF8(GetEndpointsFilePath().c_str());
    if (!wxFileExists(path)) {
        // No file: seed a usable default so the picker isn't empty
        // before the user configures anything.
        SeedDefaults();
        return true;
    }

    std::string body = ReadWholeFile(path);
    if (body.empty()) {
        // Present but unreadable/empty — treat like a fresh seed rather
        // than leaving the user with no endpoints at all.
        SeedDefaults();
        return true;
    }

    try {
        Poco::JSON::Parser parser;
        auto val  = parser.parse(body);
        auto root = val.extract<Poco::JSON::Object::Ptr>();
        if (!root) { SeedDefaults(); return false; }

        auto arr = root->getArray("endpoints");
        if (!arr) {
            // A present file with no endpoints array: respect it as empty
            // (do NOT re-seed — the user may have cleared the list).
            return true;
        }

        for (size_t i = 0; i < arr->size(); ++i) {
            auto obj = arr->getObject(i);
            if (!obj) continue;

            Endpoint ep;
            auto getStr = [&](const char* k, const std::string& dflt) {
                return obj->has(k) ? obj->getValue<std::string>(k) : dflt;
            };

            ep.id             = getStr("id", "");
            if (ep.id.empty()) continue;   // id is the primary key
            ep.displayName    = getStr("display_name", ep.id);
            ep.baseUrl        = getStr("base_url", "");
            ep.chatPath       = getStr("chat_path", "/v1/chat/completions");
            ep.authScheme     = AuthSchemeFromString(getStr("auth_scheme", "bearer"));
            ep.secretProvider = getStr("secret_provider", ep.id);
            ep.secretKey      = getStr("secret_key", "api_key");
            ep.protocol       = ProtocolFromString(getStr("protocol", "native"));

            if (obj->has("extra_headers")) {
                auto hdrs = obj->getObject("extra_headers");
                if (hdrs) {
                    std::vector<std::string> names;
                    hdrs->getNames(names);
                    for (const auto& n : names) {
                        try {
                            ep.extraHeaders.emplace_back(
                                n, hdrs->getValue<std::string>(n));
                        } catch (...) { /* skip non-string header */ }
                    }
                }
            }

            if (obj->has("models")) {
                auto models = obj->getArray("models");
                if (models) {
                    for (size_t m = 0; m < models->size(); ++m) {
                        auto mo = models->getObject(m);
                        if (!mo || !mo->has("id")) continue;
                        Model model;
                        model.id = mo->getValue<std::string>("id");
                        if (model.id.empty()) continue;
                        model.displayName = mo->has("display_name")
                            ? mo->getValue<std::string>("display_name")
                            : model.id;
                        if (mo->has("image_output")) {
                            try {
                                model.imageOutput =
                                    mo->getValue<bool>("image_output");
                            } catch (...) { /* non-bool — leave false */ }
                        }
                        ep.models.push_back(std::move(model));
                    }
                }
            }

            m_endpoints.push_back(std::move(ep));
        }
        return true;
    }
    catch (const std::exception& e) {
        wxLogWarning("EndpointStore: failed to parse endpoints.json (%s); "
                     "starting from the default endpoint.", e.what());
        SeedDefaults();
        return false;
    }
}

bool EndpointStore::Save()
{
    Poco::JSON::Object::Ptr root = new Poco::JSON::Object(true);
    root->set("version", 1);

    Poco::JSON::Array::Ptr arr = new Poco::JSON::Array;
    for (const auto& ep : m_endpoints) {
        Poco::JSON::Object::Ptr obj = new Poco::JSON::Object(true);
        obj->set("id",              ep.id);
        obj->set("display_name",    ep.displayName);
        obj->set("base_url",        ep.baseUrl);
        obj->set("chat_path",       ep.chatPath);
        obj->set("auth_scheme",     AuthSchemeToString(ep.authScheme));
        obj->set("secret_provider", ep.secretProvider);
        obj->set("secret_key",      ep.secretKey);
        obj->set("protocol",        ProtocolToString(ep.protocol));

        if (!ep.extraHeaders.empty()) {
            Poco::JSON::Object::Ptr hdrs = new Poco::JSON::Object(true);
            for (const auto& [k, v] : ep.extraHeaders) {
                if (!k.empty()) hdrs->set(k, v);
            }
            obj->set("extra_headers", hdrs);
        }

        Poco::JSON::Array::Ptr models = new Poco::JSON::Array;
        for (const auto& m : ep.models) {
            Poco::JSON::Object::Ptr mo = new Poco::JSON::Object(true);
            mo->set("id",           m.id);
            mo->set("display_name", m.displayName);
            // Only written when set — keeps hand-edited files clean
            // and the absent-key default (false) is the common case.
            if (m.imageOutput) mo->set("image_output", true);
            models->add(mo);
        }
        obj->set("models", models);

        arr->add(obj);
    }
    root->set("endpoints", arr);

    std::ostringstream body;
    Poco::JSON::Stringifier::stringify(root, body, 2);

    wxString path = wxString::FromUTF8(GetEndpointsFilePath().c_str());
    return WriteWholeFileAtomic(path, body.str());
}

// ─── Lookup / mutation ──────────────────────────────────────────

const EndpointStore::Endpoint*
EndpointStore::FindEndpoint(const std::string& id) const
{
    for (const auto& ep : m_endpoints) {
        if (ep.id == id) return &ep;
    }
    return nullptr;
}

void EndpointStore::UpsertEndpoint(const Endpoint& ep)
{
    for (auto& existing : m_endpoints) {
        if (existing.id == ep.id) {
            existing = ep;
            return;
        }
    }
    m_endpoints.push_back(ep);
}

void EndpointStore::RemoveEndpoint(const std::string& id)
{
    m_endpoints.erase(
        std::remove_if(m_endpoints.begin(), m_endpoints.end(),
                       [&](const Endpoint& e) { return e.id == id; }),
        m_endpoints.end());
}

// ─── Target resolution ──────────────────────────────────────────

bool EndpointStore::ResolveTarget(const std::string&  endpointId,
                                  const std::string&  wireModelId,
                                  const SecretsStore& secrets,
                                  InferenceTarget&    out,
                                  std::string&        outReason) const
{
    const Endpoint* ep = FindEndpoint(endpointId);
    if (!ep) {
        outReason = "Unknown remote endpoint: " + endpointId;
        return false;
    }
    if (wireModelId.empty()) {
        outReason = "No model id given for endpoint '" + ep->displayName + "'.";
        return false;
    }

    const std::string key = secrets.GetSecret(ep->secretProvider, ep->secretKey);
    if (key.empty()) {
        outReason = "No API key for '" + ep->displayName +
                    "'. Add one under Settings -> Connections (provider \"" +
                    ep->secretProvider + "\", key \"" + ep->secretKey + "\").";
        return false;
    }

    InferenceTarget t;
    t.baseUrl  = ep->baseUrl;
    t.chatPath = ep->chatPath;
    t.useTls   = (ep->baseUrl.rfind("https://", 0) == 0);

    if (ep->authScheme == AuthScheme::XApiKey) {
        t.authHeaderName  = "x-api-key";
        t.authHeaderValue = key;
    } else {
        t.authHeaderName  = "Authorization";
        t.authHeaderValue = "Bearer " + key;
    }

    t.extraHeaders = ep->extraHeaders;
    t.managed      = false;
    t.protocol     = ep->protocol;
    t.modelId      = wireModelId;

    // Per-model image-output flag.  wireModelId may name a model that
    // isn't in the configured list (typed into the picker manually);
    // in that case the flag stays false and the model is treated as a
    // plain text model — same behavior as before this feature.
    for (const auto& m : ep->models) {
        if (m.id == wireModelId) {
            t.imageOutput = m.imageOutput;
            break;
        }
    }

    out = std::move(t);
    return true;
}
