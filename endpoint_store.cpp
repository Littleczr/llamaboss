// endpoint_store.cpp
#define _CRT_SECURE_NO_WARNINGS

#include "endpoint_store.h"
#include "openai_responses.h"
#include "secrets_store.h"

#include <wx/wx.h>
#include <wx/stdpaths.h>
#include <wx/filename.h>
#include <wx/log.h>
#include <wx/datetime.h>
#include <wx/filefn.h>

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
// Returns false when the file exists but could not be read (locked,
// permissions, I/O error) -- distinct from an empty file.
bool ReadWholeFile(const wxString& path, std::string& out)
{
    out.clear();
    std::ifstream f(path.fn_str(), std::ios::in | std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    if (f.bad()) return false;
    out = ss.str();
    return true;
}

// Keeps a copy of a settings file that failed to load, next to it, so a
// later "start fresh" can never destroy the only copy.  Returns the
// backup path, or empty when no copy could be made.
std::string BackupUnreadableFile(const wxString& path)
{
    const wxString stamp = wxDateTime::Now().Format("%Y%m%d-%H%M%S");
    wxLogNull quiet;
    // Retry can fail again within the same second; never overwrite an
    // earlier backup, pick the next free name instead.
    for (int n = 0; n < 100; ++n) {
        wxString dest = path + ".unreadable-" + stamp;
        if (n > 0) dest << "-" << n;
        dest << ".bak";
        if (wxFileExists(dest)) continue;
        if (wxCopyFile(path, dest, /*overwrite=*/false))
            return std::string(dest.ToUTF8().data());
        return std::string();
    }
    return std::string();
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
    switch (s) {
        case EndpointStore::AuthScheme::XApiKey: return "x-api-key";
        case EndpointStore::AuthScheme::None:    return "none";
        default:                                 return "bearer";
    }
}

EndpointStore::AuthScheme AuthSchemeFromString(const std::string& s)
{
    if (s == "x-api-key") return EndpointStore::AuthScheme::XApiKey;
    if (s == "none")      return EndpointStore::AuthScheme::None;
    return EndpointStore::AuthScheme::Bearer;   // unknown values stay
                                                // on the historical
                                                // default
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
    m_loadFailed = false;
    m_loadError.clear();
    m_loadBackupPath.clear();

    wxString path = wxString::FromUTF8(GetEndpointsFilePath().c_str());
    if (!wxFileExists(path)) {
        // No file: seed a usable default so the picker isn't empty
        // before the user configures anything.
        SeedDefaults();
        return true;
    }

    // A present file that can't be read or parsed must not be replaced by
    // the seeded default on the next save.  Seed in memory so the picker
    // still works, keep a copy of the file, and block Save() until a
    // reload succeeds or the user explicitly starts fresh.
    auto fail = [&](const std::string& why) {
        SeedDefaults();
        m_loadFailed = true;
        m_loadError = why;
        m_loadBackupPath = BackupUnreadableFile(path);
        wxLogWarning("EndpointStore: %s; saving is disabled until this is resolved.",
                     wxString::FromUTF8(why));
        return false;
    };

    std::string body;
    if (!ReadWholeFile(path, body))
        return fail("endpoints.json could not be read");
    if (body.empty()) {
        // Present but empty (nothing to lose) — seed a fresh default.
        SeedDefaults();
        return true;
    }

    try {
        Poco::JSON::Parser parser;
        auto val  = parser.parse(body);
        auto root = val.extract<Poco::JSON::Object::Ptr>();
        if (!root) return fail("endpoints.json is not a JSON object");

        if (!root->has("endpoints")) {
            // A present file with no endpoints array: respect it as empty
            // (do NOT re-seed — the user may have cleared the list).
            return true;
        }
        auto arr = root->getArray("endpoints");
        if (!arr) return fail("endpoints.json has a malformed \"endpoints\" list");

        for (size_t i = 0; i < arr->size(); ++i) {
            auto obj = arr->getObject(i);
            if (!obj) return fail("endpoints.json has a malformed connection entry");

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
            // Optional /think dialect override; empty means "sniff the
            // base URL at resolve time" (the backward-compatible path).
            ep.reasoningDialect = getStr("reasoning_dialect", "");

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
                        if (mo->has("no_tools")) {
                            try {
                                model.noTools =
                                    mo->getValue<bool>("no_tools");
                            } catch (...) { /* non-bool — leave false */ }
                        }
                        if (mo->has("show_in_picker")) {
                            try {
                                model.showInPicker = mo->getValue<bool>("show_in_picker");
                            } catch (...) { /* non-bool — keep visible */ }
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
        return fail(std::string("endpoints.json could not be parsed (") + e.what() + ")");
    }
}

void EndpointStore::ResetAfterFailedLoad()
{
    // Explicit user choice: keep the current in-memory list and let the
    // next Save() replace the unreadable file (a copy was kept if possible).
    m_loadFailed = false;
    m_loadError.clear();
}

bool EndpointStore::Save()
{
    // Never overwrite a file that failed to load (see Load()).
    if (m_loadFailed) return false;

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
        // Sparse like the per-model flags: absent means "auto-sniff",
        // which is the common case, and keeps hand-edited files clean.
        if (!ep.reasoningDialect.empty())
            obj->set("reasoning_dialect", ep.reasoningDialect);

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
            if (m.noTools)     mo->set("no_tools", true);
            if (!m.showInPicker) mo->set("show_in_picker", false);
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

    // AuthScheme::None endpoints (local or SSH-tunneled servers such
    // as FreeToken or a bare llama-server) skip the SecretsStore
    // lookup entirely and send no auth header — the transport only
    // sets the header when both name and value are non-empty, which
    // is exactly how the managed local lane has always worked.
    std::string key;
    if (ep->authScheme != AuthScheme::None) {
        key = secrets.GetSecret(ep->secretProvider, ep->secretKey);
        if (key.empty()) {
            outReason = "No API key for '" + ep->displayName +
                        "'. Open Settings -> Remote Endpoints, edit the endpoint, "
                        "and paste the key into its API key field (connection \"" +
                        ep->secretProvider + "\", key \"" + ep->secretKey + "\").";
            return false;
        }
    }

    InferenceTarget t;
    t.baseUrl  = ep->baseUrl;
    t.chatPath = ep->chatPath;
    t.useTls   = (ep->baseUrl.rfind("https://", 0) == 0);

    if (ep->authScheme == AuthScheme::XApiKey) {
        t.authHeaderName  = "x-api-key";
        t.authHeaderValue = key;
    } else if (ep->authScheme != AuthScheme::None) {
        t.authHeaderName  = "Authorization";
        t.authHeaderValue = "Bearer " + key;
    }
    // None: both auth fields stay empty and no header is emitted.

    t.extraHeaders = ep->extraHeaders;
    t.managed      = false;
    t.protocol     = ep->protocol;
    t.modelId      = wireModelId;

    // Reasoning dialect for /think: an explicit endpoint override wins;
    // otherwise sniff the host.  Only direct OpenAI needs the
    // reasoning_effort string — it rejects unknown body fields, so the
    // historical OpenRouter reasoning object 400s there.  Every other
    // remote keeps the historical shape byte-for-byte.
    //
    // The sniff parses the HOST out of the base URL and compares it
    // lowercased and exactly, rather than substring-matching the whole
    // URL: "API.OpenAI.com" still classifies, and a look-alike host
    // ("api.openai.com.evil.example") or a path that merely contains
    // the string does NOT.
    {
        std::string d = ep->reasoningDialect;
        for (char& c : d)
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (d == "openai") {
            t.reasoningDialect = ReasoningDialect::OpenAIStyle;
        } else if (d == "openrouter") {
            t.reasoningDialect = ReasoningDialect::OpenRouterStyle;
        } else if (d == "template") {
            // Remote server that applies the model's own chat template
            // (FreeToken, vLLM, SGLang).  Never chosen by the sniff —
            // endpoint opt-in only.
            t.reasoningDialect = ReasoningDialect::TemplateKwargs;
        } else {
            std::string host = ep->baseUrl;
            const size_t scheme = host.find("://");
            if (scheme != std::string::npos) host.erase(0, scheme + 3);
            const size_t cut = host.find_first_of("/?#");
            if (cut != std::string::npos) host.erase(cut);
            const size_t at = host.rfind('@');           // strip userinfo
            if (at != std::string::npos) host.erase(0, at + 1);
            const size_t port = host.rfind(':');         // strip port
            if (port != std::string::npos) host.erase(port);
            for (char& c : host)
                if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            if (host == "api.openai.com")
                t.reasoningDialect = ReasoningDialect::OpenAIStyle;
            // default (struct initializer) stays OpenRouterStyle
        }
    }

    // Include hidden model records: removing a model from the picker must
    // not change the behavior of chats already using it.
    // Per-model behavior flags.  wireModelId may name a model that isn't
    // in the configured list (typed into the picker manually); in that
    // case both stay false and the model is treated as an ordinary,
    // tool-capable text model — the backward-compatible default.
    for (const auto& m : ep->models) {
        if (m.id == wireModelId) {
            t.imageOutput = m.imageOutput;
            t.noTools     = m.noTools;
            break;
        }
    }

    if (!t.imageOutput)
        t.chatPath = lb_responses::ResolveChatPath(t.baseUrl, t.chatPath, t.modelId);
    t.responsesApi = lb_responses::IsResponsesPath(t.chatPath);
    if (t.responsesApi) {
        // Responses speaks the OpenAI reasoning vocabulary by definition.
        // Phase 2: agent tools work on this lane (function tools +
        // encrypted reasoning replay), so the model's own Allow agent
        // tools flag is the only gate — no forced noTools any more.
        t.reasoningDialect = ReasoningDialect::OpenAIStyle;
    }

    out = std::move(t);
    return true;
}
