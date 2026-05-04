#define _CRT_SECURE_NO_WARNINGS

// tool_protocol.cpp

#include "tool_protocol.h"

#include <wx/fileconf.h>
#include <wx/string.h>

#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Stringifier.h>
#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/StreamCopier.h>
#include <Poco/URI.h>
#include <Poco/SHA1Engine.h>
#include <Poco/DigestStream.h>

#include <chrono>
#include <ostream>
#include <sstream>

wxDEFINE_EVENT(wxEVT_TOOL_PROTOCOL_DETECTED, wxThreadEvent);

namespace {

// Cache schema version.  Bump when changing how cache values are
// formatted or what protocols are stored.  Older entries with a
// different version prefix are treated as cache misses.
constexpr const char* kCacheSchema = "v2";

// Cache entries older than this (in seconds) are treated as misses
// and re-probed.  Long enough that a typical user never re-probes
// a stable model; short enough that template bugs fixed in a new
// llama-server release get re-evaluated within a month.
constexpr long long kCacheMaxAgeSeconds = 30LL * 24 * 60 * 60;  // 30 days

constexpr const char* kCacheConfigSection = "ToolCapabilities";

// SHA-1 of "<modelPath>\n<mmprojPath>" as a lowercase hex string.
// Stable cache key — invalidates iff either path changes (model
// reinstalled, mmproj added/removed/repaired).
std::string CacheKey(const std::string& modelPath,
                     const std::string& mmprojPath)
{
    Poco::SHA1Engine sha;
    Poco::DigestOutputStream dos(sha);
    dos << modelPath << "\n" << mmprojPath;
    dos.flush();
    return Poco::DigestEngine::digestToHex(sha.digest());
}

// Stringify "v1|native|<unix_seconds>".  Inverse of ParseCacheValue.
std::string FormatCacheValue(ToolProtocol p)
{
    long long now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::ostringstream os;
    os << kCacheSchema << "|"
       << ((p == ToolProtocol::Native) ? "native" : "xml") << "|"
       << now;
    return os.str();
}

// Parse a cache value back to (protocol, age_seconds).  Returns
// false on schema mismatch, malformed value, or unrecognized
// protocol token.  ageSeconds is set to 0 on parse failure.
bool ParseCacheValue(const std::string& value,
                     ToolProtocol&      protoOut,
                     long long&         ageSecondsOut)
{
    ageSecondsOut = 0;
    protoOut      = ToolProtocol::Unknown;

    // "v1|native|1761598123" — three pipe-separated fields.
    size_t p1 = value.find('|');
    if (p1 == std::string::npos) return false;
    size_t p2 = value.find('|', p1 + 1);
    if (p2 == std::string::npos) return false;

    std::string schema = value.substr(0, p1);
    std::string proto  = value.substr(p1 + 1, p2 - p1 - 1);
    std::string when   = value.substr(p2 + 1);

    if (schema != kCacheSchema) return false;

    if      (proto == "native") protoOut = ToolProtocol::Native;
    else if (proto == "xml")    protoOut = ToolProtocol::Xml;
    else return false;

    long long whenSec = 0;
    try { whenSec = std::stoll(when); }
    catch (...) { return false; }

    long long now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    ageSecondsOut = now - whenSec;
    return true;
}

// Heuristic scan over a chat_template string.  Returns true if any
// of the known tool-call markers appear anywhere.  Case-sensitive
// — these markers are literal substrings emitted by the templates
// we care about (Hermes 2 Pro, Qwen 2.5, Llama 3.x).
bool TemplateHasToolMarkers(const std::string& tmpl)
{
    if (tmpl.empty()) return false;

    static const char* kMarkers[] = {
        "tool_calls",
        "tool_call_id",
        "<tool_call>",
        "<|tool_call|>",
        "<|python_tag|>",
        "tool_use",
        // "tools" alone is too generic — many templates mention
        // "tools" in unrelated contexts.  We rely on the more
        // specific markers above.
    };

    for (const char* needle : kMarkers) {
        if (tmpl.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// Step 1: GET /props.  Returns the JSON object on success, nullptr
// on any failure.  Times out at 5 seconds — /props is a synchronous
// in-memory query inside llama-server, so anything slower than that
// indicates the server is hung or unreachable.
Poco::JSON::Object::Ptr FetchProps(const std::string& baseUrl)
{
    try {
        Poco::URI uri(baseUrl + "/props");
        Poco::Net::HTTPClientSession sess(uri.getHost(), uri.getPort());
        sess.setTimeout(Poco::Timespan(5, 0));

        Poco::Net::HTTPRequest req(Poco::Net::HTTPRequest::HTTP_GET,
                                   uri.getPathAndQuery());
        sess.sendRequest(req);

        Poco::Net::HTTPResponse resp;
        std::istream& in = sess.receiveResponse(resp);
        std::string body;
        Poco::StreamCopier::copyToString(in, body);

        if (resp.getStatus() != Poco::Net::HTTPResponse::HTTP_OK) {
            return nullptr;
        }

        Poco::JSON::Parser parser;
        auto var = parser.parse(body);
        return var.extract<Poco::JSON::Object::Ptr>();
    }
    catch (...) {
        return nullptr;
    }
}

// Step 2: heuristic.  Any tool marker in either the default or the
// tool-use chat template counts as "could possibly emit tool_calls."
// The smoke test in step 3 is the actual confirmation.
bool RunHeuristic(const Poco::JSON::Object::Ptr& props,
                  std::string&                   reasonOut)
{
    if (!props) {
        reasonOut = "no /props payload";
        return false;
    }

    auto extractString = [&](const std::string& key) -> std::string {
        if (!props->has(key)) return {};
        try {
            return props->getValue<std::string>(key);
        }
        catch (...) {
            return {};
        }
    };

    std::string tmpl     = extractString("chat_template");
    std::string toolTmpl = extractString("chat_template_tool_use");

    if (TemplateHasToolMarkers(tmpl) || TemplateHasToolMarkers(toolTmpl)) {
        reasonOut = "chat_template carries tool markers";
        return true;
    }

    reasonOut = "no tool markers found in chat templates";
    return false;
}

// Step 3: smoke test.  POST a minimal /v1/chat/completions request
// with one tool defined (`pwd`, no side effects), tool_choice set
// to "required", and a user message that should trigger it.  If
// the response contains a tool_calls array with a function whose
// name is "pwd", native is confirmed.  Anything else fails the
// test and we fall back to XML for this model.
bool RunSmokeTest(const std::string& baseUrl,
                  std::string&       reasonOut)
{
    // Build the request body.
    Poco::JSON::Object::Ptr body = new Poco::JSON::Object;
    body->set("model", "any");          // llama-server ignores this
    body->set("max_tokens", 64);
    body->set("temperature", 0.0);
    body->set("tool_choice", "required");

    // messages: [{role:"user", content:"What working directory am I in?"}]
    Poco::JSON::Array::Ptr messages = new Poco::JSON::Array;
    {
        Poco::JSON::Object::Ptr m = new Poco::JSON::Object;
        m->set("role",    std::string("user"));
        m->set("content", std::string("What working directory am I in?"));
        messages->add(m);
    }
    body->set("messages", messages);

    // tools: [{type:"function", function:{name:"pwd", description, parameters}}]
    Poco::JSON::Array::Ptr tools = new Poco::JSON::Array;
    {
        Poco::JSON::Object::Ptr fn = new Poco::JSON::Object;
        fn->set("name",        std::string("pwd"));
        fn->set("description", std::string(
            "Return the current working directory."));

        Poco::JSON::Object::Ptr params = new Poco::JSON::Object;
        params->set("type", std::string("object"));
        Poco::JSON::Object::Ptr props = new Poco::JSON::Object;
        params->set("properties", props);
        fn->set("parameters", params);

        Poco::JSON::Object::Ptr tool = new Poco::JSON::Object;
        tool->set("type",     std::string("function"));
        tool->set("function", fn);
        tools->add(tool);
    }
    body->set("tools", tools);

    std::string requestJson;
    {
        std::ostringstream os;
        body->stringify(os);
        requestJson = os.str();
    }

    // POST.
    std::string responseText;
    try {
        Poco::URI uri(baseUrl + "/v1/chat/completions");
        Poco::Net::HTTPClientSession sess(uri.getHost(), uri.getPort());
        // 30 seconds is plenty — the request is ~250 bytes and the
        // model only needs to emit a tool_call object, ~30 tokens.
        // If it takes longer than 30s on a 5090, something is off.
        sess.setTimeout(Poco::Timespan(30, 0));

        Poco::Net::HTTPRequest req(Poco::Net::HTTPRequest::HTTP_POST,
                                   uri.getPathAndQuery(),
                                   Poco::Net::HTTPMessage::HTTP_1_1);
        req.setContentType("application/json");
        req.setContentLength(static_cast<std::streamsize>(requestJson.size()));

        std::ostream& body_os = sess.sendRequest(req);
        body_os << requestJson;

        Poco::Net::HTTPResponse resp;
        std::istream& in = sess.receiveResponse(resp);
        Poco::StreamCopier::copyToString(in, responseText);

        if (resp.getStatus() != Poco::Net::HTTPResponse::HTTP_OK) {
            reasonOut = "smoke test HTTP " +
                        std::to_string(static_cast<int>(resp.getStatus()));
            return false;
        }
    }
    catch (const std::exception& ex) {
        reasonOut = std::string("smoke test exception: ") + ex.what();
        return false;
    }
    catch (...) {
        reasonOut = "smoke test: unknown exception";
        return false;
    }

    // Parse the response and look for choices[0].message.tool_calls[0]
    // with function.name == "pwd".
    try {
        Poco::JSON::Parser parser;
        auto var  = parser.parse(responseText);
        auto root = var.extract<Poco::JSON::Object::Ptr>();
        if (!root) {
            reasonOut = "smoke test: response not a JSON object";
            return false;
        }

        auto choices = root->getArray("choices");
        if (!choices || choices->size() == 0) {
            reasonOut = "smoke test: no choices in response";
            return false;
        }

        auto choice0 = choices->getObject(0);
        if (!choice0) {
            reasonOut = "smoke test: choices[0] not an object";
            return false;
        }

        auto message = choice0->getObject("message");
        if (!message) {
            reasonOut = "smoke test: choices[0].message missing";
            return false;
        }

        if (!message->has("tool_calls")) {
            reasonOut = "smoke test: message.tool_calls absent";
            return false;
        }

        auto toolCalls = message->getArray("tool_calls");
        if (!toolCalls || toolCalls->size() == 0) {
            reasonOut = "smoke test: tool_calls absent or empty";
            return false;
        }

        auto call0 = toolCalls->getObject(0);
        if (!call0) {
            reasonOut = "smoke test: tool_calls[0] not an object";
            return false;
        }

        auto fn = call0->getObject("function");
        if (!fn) {
            reasonOut = "smoke test: tool_calls[0].function missing";
            return false;
        }

        std::string name;
        try { name = fn->getValue<std::string>("name"); }
        catch (...) { name.clear(); }

        if (name != "pwd") {
            reasonOut = "smoke test: tool name was '" + name +
                        "', expected 'pwd'";
            return false;
        }

        reasonOut = "smoke test: native tool_calls confirmed";
        return true;
    }
    catch (const std::exception& ex) {
        reasonOut = std::string("smoke test parse exception: ") + ex.what();
        return false;
    }
    catch (...) {
        reasonOut = "smoke test: unknown parse exception";
        return false;
    }
}

} // anonymous namespace

const char* ToolProtocolName(ToolProtocol p)
{
    switch (p) {
        case ToolProtocol::Native:  return "native";
        case ToolProtocol::Xml:     return "xml";
        case ToolProtocol::Unknown: return "unknown";
    }
    return "unknown";
}

// ─── Cache I/O ──────────────────────────────────────────────────

bool LoadCachedProtocol(const std::string& modelPath,
                        const std::string& mmprojPath,
                        ToolProtocol&      out)
{
    out = ToolProtocol::Unknown;

    wxFileConfig cfg("LlamaBoss");
    wxString prevPath = cfg.GetPath();
    cfg.SetPath(wxString::FromUTF8(std::string("/") + kCacheConfigSection));

    std::string key = CacheKey(modelPath, mmprojPath);
    wxString value;
    bool found = cfg.Read(wxString::FromUTF8(key), &value);

    cfg.SetPath(prevPath);

    if (!found || value.IsEmpty()) return false;

    ToolProtocol proto = ToolProtocol::Unknown;
    long long age = 0;
    if (!ParseCacheValue(std::string(value.ToUTF8().data()), proto, age))
        return false;

    if (age < 0 || age > kCacheMaxAgeSeconds) return false;

    out = proto;
    return true;
}

void SaveProtocolToCache(const std::string& modelPath,
                         const std::string& mmprojPath,
                         ToolProtocol       protocol)
{
    if (protocol != ToolProtocol::Native &&
        protocol != ToolProtocol::Xml) {
        return;   // Don't cache Unknown
    }

    wxFileConfig cfg("LlamaBoss");
    wxString prevPath = cfg.GetPath();
    cfg.SetPath(wxString::FromUTF8(std::string("/") + kCacheConfigSection));

    std::string key = CacheKey(modelPath, mmprojPath);
    cfg.Write(wxString::FromUTF8(key),
              wxString::FromUTF8(FormatCacheValue(protocol)));
    cfg.Flush();

    cfg.SetPath(prevPath);
}

// ─── Worker ─────────────────────────────────────────────────────

ProtocolProbeWorker::ProtocolProbeWorker(
    wxEvtHandler* handler,
    std::weak_ptr<std::atomic<bool>> aliveToken,
    std::string baseUrl,
    std::string modelPath,
    std::string mmprojPath)
    : wxThread(wxTHREAD_DETACHED)
    , m_handler(handler)
    , m_aliveToken(aliveToken)
    , m_baseUrl(std::move(baseUrl))
    , m_modelPath(std::move(modelPath))
    , m_mmprojPath(std::move(mmprojPath))
{
}

bool ProtocolProbeWorker::SafePost(wxThreadEvent* ev)
{
    auto strong = m_aliveToken.lock();
    if (!strong || !*strong) {
        delete ev;
        return false;
    }
    wxQueueEvent(m_handler, ev);
    return true;
}

wxThread::ExitCode ProtocolProbeWorker::Entry()
{
    ProtocolDetectionResult result;
    result.modelPath = m_modelPath;
    result.cacheHit  = false;

    // Step 1: /props
    auto props = FetchProps(m_baseUrl);

    // Step 2: heuristic
    std::string reason;
    bool heuristicOk = RunHeuristic(props, reason);
    if (!heuristicOk) {
        result.protocol = ToolProtocol::Xml;
        result.reason   = reason;
        SaveProtocolToCache(m_modelPath, m_mmprojPath, result.protocol);

        auto* ev = new wxThreadEvent(wxEVT_TOOL_PROTOCOL_DETECTED);
        ev->SetPayload<ProtocolDetectionResult>(result);
        SafePost(ev);
        return (ExitCode)0;
    }

    // Step 3: smoke test
    std::string smokeReason;
    bool smokeOk = RunSmokeTest(m_baseUrl, smokeReason);
    result.protocol = smokeOk ? ToolProtocol::Native : ToolProtocol::Xml;
    result.reason   = reason + "; " + smokeReason;
    SaveProtocolToCache(m_modelPath, m_mmprojPath, result.protocol);

    auto* ev = new wxThreadEvent(wxEVT_TOOL_PROTOCOL_DETECTED);
    ev->SetPayload<ProtocolDetectionResult>(result);
    SafePost(ev);
    return (ExitCode)0;
}

// ─── Public kickoff ─────────────────────────────────────────────

bool KickOffToolProtocolDetection(
    wxEvtHandler*                    handler,
    std::weak_ptr<std::atomic<bool>> aliveToken,
    const std::string&               baseUrl,
    const std::string&               modelPath,
    const std::string&               mmprojPath,
    bool                             serverJinjaEnabled)
{
    if (!handler || modelPath.empty()) return false;

    // Native tool calling is impossible if this particular server
    // process was not launched with --jinja. Return XML immediately
    // and deliberately do NOT cache that result. A later launch of
    // the same model with --jinja should still get a fresh probe.
    if (!serverJinjaEnabled) {
        ProtocolDetectionResult r;
        r.protocol  = ToolProtocol::Xml;
        r.cacheHit  = false;
        r.modelPath = modelPath;
        r.reason    = "server is running without --jinja; native tool calling unavailable";

        auto* ev = new wxThreadEvent(wxEVT_TOOL_PROTOCOL_DETECTED);
        ev->SetPayload<ProtocolDetectionResult>(r);
        wxQueueEvent(handler, ev);
        return true;
    }

    // Cache hit fast-path: post the cached result on this thread,
    // no worker needed.  Use wxQueueEvent so the handler still
    // sees it on its event-processing thread and not synchronously
    // mid-OnServerReady.
    ToolProtocol cached = ToolProtocol::Unknown;
    if (LoadCachedProtocol(modelPath, mmprojPath, cached)) {
        ProtocolDetectionResult r;
        r.protocol  = cached;
        r.cacheHit  = true;
        r.modelPath = modelPath;
        r.reason    = "cache hit";

        auto* ev = new wxThreadEvent(wxEVT_TOOL_PROTOCOL_DETECTED);
        ev->SetPayload<ProtocolDetectionResult>(r);
        wxQueueEvent(handler, ev);
        return true;
    }

    // Cache miss — spawn the probe worker.  Detached thread; it
    // posts the event and self-deletes.
    auto* worker = new ProtocolProbeWorker(
        handler, aliveToken, baseUrl, modelPath, mmprojPath);
    if (worker->Run() != wxTHREAD_NO_ERROR) {
        delete worker;
        return false;
    }
    return true;
}
