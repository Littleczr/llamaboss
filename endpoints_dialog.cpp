// endpoints_dialog.cpp
#define _CRT_SECURE_NO_WARNINGS

#include "endpoints_dialog.h"
#include "reasoning_policy.h"
#include "openai_responses.h"
#include "endpoint_store.h"
#include "secrets_store.h"
#include "theme.h"
#include "widgets.h"   // ApplyDialogThemeRecursive, ApplyDarkTitleBar
#include "connections_dialog.h"
#include <wx/simplebook.h>
#include <wx/scrolwin.h>
#include <wx/checkbox.h>
#include <wx/utils.h>
#ifdef __WXMSW__
#include <windows.h>
#endif
#include "ui_event_post.h"   // LbQueueEventIfAlive (fetch-models worker)

#include <wx/checklst.h>
#include <wx/choice.h>
#include <wx/collpane.h>
#include <wx/combobox.h>
#include <wx/sizer.h>
#include <wx/radiobut.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/display.h>

#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPSClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/Net/HTTPMessage.h>
#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Array.h>
#include <Poco/URI.h>
#include <Poco/Exception.h>

#include <algorithm>   // std::transform (model tags, case-insensitive)
#include <atomic>      // fetch-worker alive token
#include <cctype>      // std::tolower
#include <memory>
#include <sstream>
#include <iomanip>      // quoted display names in the advanced model editor
#include <set>
#include <string>
#include <thread>      // detached fetch-models / test-connection workers
#include <stdexcept>
#include <chrono>      // probe timing

// ─── Event table ────────────────────────────────────────────────

enum {
    ID_EP_ADD = wxID_HIGHEST + 3200,
    ID_EP_EDIT,
    ID_EP_DELETE,
    ID_EP_LIST
};

// ─────────────────────────────────────────────────────────────────
//  Button / helper recipes (file-local) — same family as
//  connections_dialog.cpp so this dialog matches the rest.
// ─────────────────────────────────────────────────────────────────
namespace {

wxButton* MakeAccentButton(wxWindow* parent, wxWindowID id,
                           const wxString& label, const ThemeData& t,
                           int height = 32)
{
    auto* btn = new wxButton(parent, id, label,
                             wxDefaultPosition, wxSize(-1, height),
                             wxBORDER_NONE);
    wxFont bf = btn->GetFont();
    bf.SetPointSize(10);
    bf.SetWeight(wxFONTWEIGHT_SEMIBOLD);
    btn->SetFont(bf);
    btn->SetBackgroundColour(t.accentButton);
    btn->SetForegroundColour(t.accentButtonText);
    return btn;
}

wxButton* MakeDestructiveButton(wxWindow* parent, wxWindowID id,
                                const wxString& label, const ThemeData& t,
                                int height = 32)
{
    auto* btn = new wxButton(parent, id, label,
                             wxDefaultPosition, wxSize(-1, height),
                             wxBORDER_NONE);
    wxFont bf = btn->GetFont();
    bf.SetPointSize(10);
    bf.SetWeight(wxFONTWEIGHT_SEMIBOLD);
    btn->SetFont(bf);
    btn->SetBackgroundColour(t.stopButton);
    btn->SetForegroundColour(t.stopButtonText);
    return btn;
}

wxButton* MakeFlatButton(wxWindow* parent, wxWindowID id,
                         const wxString& label, const ThemeData& t,
                         int height = 32)
{
    auto* btn = new wxButton(parent, id, label,
                             wxDefaultPosition, wxSize(-1, height),
                             wxBORDER_NONE);
    wxFont bf = btn->GetFont();
    bf.SetPointSize(10);
    bf.SetWeight(wxFONTWEIGHT_SEMIBOLD);
    btn->SetFont(bf);
    btn->SetBackgroundColour(t.bgDialogSurface);
    btn->SetForegroundColour(t.textMuted);
    return btn;
}

wxPanel* MakeHairline(wxWindow* parent, const ThemeData& t)
{
    auto* line = new wxPanel(parent, wxID_ANY,
                             wxDefaultPosition, wxSize(-1, 1));
    line->SetBackgroundColour(t.borderSubtle);
    return line;
}

// Status-line glyph prefixes (UTF-8 check mark / ballot x).
const std::string kCheck = "\xE2\x9C\x93 ";
const std::string kCross = "\xE2\x9C\x97 ";

// Label-column width shared by every form grid in the edit dialog.
constexpr int kLabelWidth = 118;

// Endpoint ids, secret provider/key names: lowercase letters, digits,
// underscores — same identifier rule the Connections dialog uses.
bool IsSafeIdentifier(const wxString& s)
{
    if (s.IsEmpty()) return false;
    for (size_t i = 0; i < s.length(); ++i) {
        const wxUniChar c = s[i];
        if ((c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '_') {
            continue;
        }
        return false;
    }
    return true;
}

std::string TrimUtf8(wxString value)
{
    value.Trim().Trim(false);
    return std::string(value.ToUTF8().data());
}

std::string NormalizeBaseUrl(std::string value)
{
    // A provider preset already supplies the canonical form, but custom
    // users commonly paste a trailing slash.  Strip it here so joining the
    // chat path can never silently produce a double slash.
    while (value.size() > 8 && value.back() == '/') value.pop_back();
    return value;
}

std::string NormalizeChatPath(std::string value)
{
    if (value.empty()) return "/v1/chat/completions";
    if (value.front() != '/') value.insert(value.begin(), '/');
    return value;
}

std::string JoinEndpointUrl(const std::string& baseUrl,
                            const std::string& chatPath)
{
    if (baseUrl.empty()) return {};
    return NormalizeBaseUrl(baseUrl) + NormalizeChatPath(chatPath);
}

EndpointStore::Endpoint MakeProviderPreset(int selection)
{
    EndpointStore::Endpoint ep;
    ep.chatPath  = "/v1/chat/completions";
    ep.secretKey = "api_key";
    ep.authScheme = EndpointStore::AuthScheme::Bearer;
    ep.protocol   = ToolProtocol::Native;

    if (selection == 0) {
        ep.id             = "openai";
        ep.displayName    = "OpenAI";
        ep.baseUrl        = "https://api.openai.com";
        ep.secretProvider = "openai";
    }
    else if (selection == 1) {
        ep.id             = "openrouter";
        ep.displayName    = "OpenRouter";
        ep.baseUrl        = "https://openrouter.ai/api";
        ep.secretProvider = "openrouter";
        ep.extraHeaders   = {
            { "HTTP-Referer", "https://llamaboss.com" },
            { "X-Title",      "LlamaBoss" },
        };
    }
    // selection 2 is Custom: retain the safe struct defaults and let the
    // user fill only the values that genuinely vary by provider.
    return ep;
}

// ── Model-list text <-> vector ───────────────────────────────────
// The edit dialog presents the model list as one line per model:
//   anthropic/claude-sonnet-4.6 = Claude Sonnet 4.6
//   google/gemini-2.5-flash-image = Nano Banana [image]
//   gpt-5.6-luna = GPT-5.6 Luna [no-tools]
// The part after the first '=' is the display name (optional). A
// trailing "[image]" tag (case-insensitive) marks an image-generation
// model; "[text]" forces it off.  A trailing "[no-tools]" marks a
// chat/reasoning-only model whose provider rejects function tools.
// Distinct tags may be combined in either order. Both the display name
// and the image flag are AUTO-DERIVED for bare ids so a casual user can
// paste just the model id and be done:
//   * ids whose last path segment contains the token "image"
//     (gemini-3.1-flash-lite-image, gpt-image-1, ...) are auto-marked
//     as image models -- explicit [image]/[text] always wins;
//   * a missing display name is prettified from the id
//     (google/gemini-3.1-flash-lite-image -> Gemini 3.1 Flash Lite
//     Image) -- an explicit "= name" always wins.
// Flags-as-line-suffix keeps the editor a single monospace text box
// rather than a nested list control with a checkbox column.

// True if the id's final path segment contains "image" as a token
// delimited by '-', '_', '.', ':' or string edges.  Token match (not
// substring) so a hypothetical "imagen-analyzer" style id doesn't
// misfire on partial words.
bool IdLooksLikeImageModel(const std::string& id)
{
    const auto slash = id.find_last_of('/');
    std::string seg = (slash == std::string::npos) ? id
                                                   : id.substr(slash + 1);
    std::transform(seg.begin(), seg.end(), seg.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    std::string token;
    auto flush = [&]() {
        const bool hit = (token == "image");
        token.clear();
        return hit;
    };
    for (char c : seg) {
        if (c == '-' || c == '_' || c == '.' || c == ':') {
            if (flush()) return true;
        } else {
            token += c;
        }
    }
    return flush();
}

// "google/gemini-3.1-flash-lite-image" -> "Gemini 3.1 Flash Lite Image"
// "moonshotai/kimi-k3:free"            -> "Kimi K3 Free"
// Last path segment; '-'/'_'/':' become spaces; each word gets an
// uppercase first letter ('.'-joined version numbers pass through
// untouched), plus a tiny acronym map for the usual suspects.
std::string PrettyNameFromId(const std::string& id)
{
    const auto slash = id.find_last_of('/');
    const std::string seg = (slash == std::string::npos)
                                ? id : id.substr(slash + 1);
    std::string out;
    std::string word;
    auto flushWord = [&]() {
        if (word.empty()) return;
        std::string lower = word;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        if (!out.empty()) out += ' ';
        if (lower == "gpt" || lower == "ai" || lower == "vl" ||
            lower == "xl") {
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return (char)std::toupper(c); });
            out += lower;
        } else {
            lower[0] = (char)std::toupper((unsigned char)lower[0]);
            out += lower;
        }
        word.clear();
    };
    for (char c : seg) {
        if (c == '-' || c == '_' || c == ':') flushWord();
        else                                  word += c;
    }
    flushWord();
    return out.empty() ? id : out;
}

std::vector<EndpointStore::Model> ParseModelsText(const wxString& text)
{
    std::vector<EndpointStore::Model> out;
    std::istringstream ss(std::string(text.ToUTF8().data()));
    std::string line;
    while (std::getline(ss, line)) {
        // trim
        auto a = line.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) continue;
        auto b = line.find_last_not_of(" \t\r\n");
        line = line.substr(a, b - a + 1);
        if (line.empty()) continue;

        EndpointStore::Model m;

        // Strip trailing behavior tags before the '=' split so they can
        // follow either a bare id or a display name.  Looping allows
        // distinct tags to be combined, e.g. "[image] [no-tools]".
        // For the image override, the rightmost explicit tag wins.
        int imageOverride = -1;  // -1 auto, 0 text, 1 image
        {
            auto stripTag = [&line](const std::string& tag) -> bool {
                if (line.size() < tag.size()) return false;
                std::string tail = line.substr(line.size() - tag.size());
                std::transform(tail.begin(), tail.end(), tail.begin(),
                               [](unsigned char c) { return (char)std::tolower(c); });
                if (tail != tag) return false;
                line.erase(line.size() - tag.size());
                auto e = line.find_last_not_of(" \t");
                line = (e == std::string::npos)
                           ? std::string()
                           : line.substr(0, e + 1);
                return true;
            };

            for (;;) {
                if (stripTag("[no-tools]")) {
                    m.noTools = true;
                    continue;
                }
                if (stripTag("[image]")) {
                    if (imageOverride < 0) imageOverride = 1;
                    continue;
                }
                if (stripTag("[text]")) {
                    if (imageOverride < 0) imageOverride = 0;
                    continue;
                }
                break;
            }
            if (line.empty()) continue;  // tag with no model id
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            m.id = line;
            m.displayName = PrettyNameFromId(line);
        } else {
            std::string id   = line.substr(0, eq);
            std::string disp = line.substr(eq + 1);
            auto trim = [](std::string s) {
                auto x = s.find_first_not_of(" \t");
                if (x == std::string::npos) return std::string();
                auto y = s.find_last_not_of(" \t");
                return s.substr(x, y - x + 1);
            };
            m.id = trim(id);
            m.displayName = trim(disp);
            // Quoted names are literal text. Behavior tags only belong
            // outside the closing quote, never inside a display name.
            if (!m.displayName.empty() && m.displayName.front() == '"') {
                std::istringstream quotedName(m.displayName);
                std::string decoded;
                if (quotedName >> std::quoted(decoded)) {
                    quotedName >> std::ws;
                    if (quotedName.eof()) m.displayName = std::move(decoded);
                }
            }
            if (m.id.empty()) continue;
            if (m.displayName.empty())
                m.displayName = PrettyNameFromId(m.id);
        }
        if (imageOverride >= 0)
            m.imageOutput = (imageOverride == 1);
        else if (IdLooksLikeImageModel(m.id))
            m.imageOutput = true;
        // One record per wire ID; the last explicit raw entry wins.
        auto existing = std::find_if(out.begin(), out.end(),
            [&](const auto& item) { return item.id == m.id; });
        if (existing == out.end()) out.push_back(std::move(m));
        else *existing = std::move(m);
    }
    return out;
}

wxString ModelsToText(const std::vector<EndpointStore::Model>& models)
{
    std::string out;
    for (const auto& m : models) {
        if (!m.showInPicker) continue;
        out += m.id;
        if (!m.displayName.empty()) {
            out += " = ";
            std::ostringstream name;
            name << std::quoted(m.displayName);
            out += name.str();
        }
        if (m.imageOutput) out += " [image]";
        else if (IdLooksLikeImageModel(m.id)) out += " [text]";
        if (m.noTools)     out += " [no-tools]";
        out += "\n";
    }
    return wxString::FromUTF8(out.c_str());
}

// The structured records own all model settings. Raw text only edits picker
// membership and the visible models; omitted records keep their metadata.
std::vector<EndpointStore::Model> ModelsForPicker(
    const std::vector<EndpointStore::Model>& records)
{
    std::vector<EndpointStore::Model> selected;
    for (const auto& model : records)
        if (model.showInPicker) selected.push_back(model);
    return selected;
}

void MergePickerModels(std::vector<EndpointStore::Model>& records,
                       const std::vector<EndpointStore::Model>& selected)
{
    for (auto& model : records) model.showInPicker = false;
    for (const auto& model : selected) {
        auto existing = std::find_if(records.begin(), records.end(),
            [&](const auto& item) { return item.id == model.id; });
        if (existing == records.end()) records.push_back(model);
        else *existing = model;
    }
}

void SetModelPickerVisibility(std::vector<EndpointStore::Model>& records,
                              const std::string& id, bool visible)
{
    auto existing = std::find_if(records.begin(), records.end(),
        [&](const auto& model) { return model.id == id; });
    if (existing != records.end()) {
        existing->showInPicker = visible;
    } else if (visible) {
        EndpointStore::Model model;
        model.id = id;
        model.displayName = PrettyNameFromId(id);
        model.imageOutput = IdLooksLikeImageModel(id);
        records.push_back(std::move(model));
    }
}

// ─── Composite Add/Edit dialog ──────────────────────────────────
//
// Redesigned around the two personas that actually use it:
//
//   * Provider preset (OpenAI / OpenRouter) — the overwhelmingly
//     common case.  Every transport constant is known for these, so
//     the visible dialog is just: Provider, Saved connection, and a
//     fetched model checklist.  The old separate "Add Remote
//     Endpoint" chooser modal is gone — the Provider combo IS that
//     choice, and switching it re-seeds the constants live.
//   * Custom OpenAI-compatible service — Base URL sits in the
//     Connection group; chat path, auth scheme and tool protocol are
//     folded into an "Advanced" expander that opens automatically
//     when Provider is Custom.
//
// Two-step setup with independently scrolling pages and a fixed footer.
//
// Fetch models: GET <base>/v1/models with the endpoint's own auth,
// on a detached worker thread, feeding a filterable checklist.
// Checking/unchecking changes membership on structured model records.
// The advanced text editor projects selected records and imports its final
// draft on save or return to the checklist. A 401 on the fetch doubles as the auth test
// that previously required a failed first chat message.

struct FetchModelsResult {
    bool        ok = false;
    int         status = 0;             // HTTP status (0 = transport error)
    std::string error;
    std::vector<std::string> ids;       // sorted, deduplicated
    bool credentialsVerified = false;
};

wxDEFINE_EVENT(wxEVT_EP_MODELS_FETCHED, wxThreadEvent);

// Blocking GET <base>/v1/models — worker-thread only.  Both OpenAI
// and OpenRouter answer {"data":[{"id":...},...]}; the same shape is
// the de-facto standard for OpenAI-compatible services, so it is the
// default probe for Custom endpoints too.  Auth material is resolved
// by the CALLER on the UI thread (SecretsStore is UI-thread-only)
// and passed in as a ready-made header.
FetchModelsResult FetchModelIds(
    const std::string& url,
    const std::string& authHeaderName,     // empty = send without auth
    const std::string& authHeaderValue,
    const std::vector<std::pair<std::string, std::string>>& extraHeaders, bool inspectKey = false)
{
    FetchModelsResult out;
    try {
        Poco::URI uri(url);
        const std::string scheme = uri.getScheme();
        int port = uri.getPort();
        if (port == 0) port = (scheme == "https") ? 443 : 80;

        std::unique_ptr<Poco::Net::HTTPClientSession> sess;
        if (scheme == "https") {
            sess.reset(new Poco::Net::HTTPSClientSession(
                uri.getHost(), (Poco::UInt16)port));
        } else {
            sess.reset(new Poco::Net::HTTPClientSession(
                uri.getHost(), (Poco::UInt16)port));
        }
        // Hard bound on the whole exchange — this is what makes the
        // detached worker safe to abandon at dialog close.
        sess->setTimeout(Poco::Timespan(15, 0));

        std::string path = uri.getPathAndQuery();
        if (path.empty()) path = "/";

        Poco::Net::HTTPRequest req(Poco::Net::HTTPRequest::HTTP_GET, path,
                                   Poco::Net::HTTPMessage::HTTP_1_1);
        req.set("User-Agent", "LlamaBoss/1.0");
        req.set("Accept",     "application/json");
        // Host is deliberately NOT set here: HTTPClientSession fills it
        // from the session's host AND port when absent, so a custom
        // endpoint on localhost:8080 sends "Host: localhost:8080".
        // Setting uri.getHost() manually would strip the port (the same
        // latent issue model_downloader.cpp carries for its 443-only
        // hosts).  chat_client.cpp relies on the same Poco behavior.
        if (!authHeaderName.empty())
            req.set(authHeaderName, authHeaderValue);
        for (const auto& [k, v] : extraHeaders)
            if (!k.empty()) req.set(k, v);
        sess->sendRequest(req);

        Poco::Net::HTTPResponse resp;
        std::istream& in = sess->receiveResponse(resp);
        out.status = (int)resp.getStatus();

        // Bounded body read — OpenRouter's full catalog is ~1 MB of
        // JSON; 4 MB leaves generous headroom without letting a
        // misconfigured URL stream something huge into the dialog.
        std::string body;
        body.reserve(64 * 1024);
        constexpr size_t kMaxBody = 4u * 1024 * 1024;
        char buf[8192];
        while (in.good() && body.size() < kMaxBody) {
            in.read(buf, sizeof(buf));
            const std::streamsize got = in.gcount();
            if (got <= 0) break;
            body.append(buf, (size_t)got);
        }

        if (out.status != 200) {
            out.error = "HTTP " + std::to_string(out.status) + " " +
                        resp.getReason();
            // Short JSON error bodies (auth failures) are worth showing.
            if (!body.empty() && body.size() <= 300)
                out.error += " - " + body;
            return out;
        }

        Poco::JSON::Parser parser;
        auto parsed  = parser.parse(body);
        auto rootObj = parsed.extract<Poco::JSON::Object::Ptr>();
        if (!rootObj) {
            out.error = "Unexpected response (not a JSON object).";
            return out;
        }
        if (inspectKey) {
            const auto keyData = rootObj->getObject("data");
            if (!keyData || !keyData->has("label")) {
                out.error = "The key check returned an unexpected response.";
                return out;
            }
            out.credentialsVerified = true;
            out.ok = true;
            return out;
        }
        Poco::JSON::Array::Ptr data = rootObj->getArray("data");
        if (!data) {
            out.error = "Response has no \"data\" array - not an "
                        "OpenAI-compatible /v1/models endpoint?";
            return out;
        }
        std::set<std::string> ids;   // set: sorted + deduplicated
        for (size_t i = 0; i < data->size(); ++i) {
            auto o = data->getObject(i);
            if (!o || !o->has("id")) continue;
            try {
                std::string id = o->getValue<std::string>("id");
                if (!id.empty()) ids.insert(std::move(id));
            } catch (...) { /* non-string id — skip */ }
        }
        out.ids.assign(ids.begin(), ids.end());
        out.ok = true;
    }
    catch (const std::exception& e) {
        out.error = e.what();
    }
    return out;
}

// Catalog access alone is not credential verification on a public endpoint.
// OpenRouter has a dedicated authenticated key check; OpenAI's models route
// requires authentication. Custom servers may expose their catalog publicly.
FetchModelsResult FetchConnectionCatalog(const std::string& base, const std::string& url,
    const std::string& headerName, const std::string& headerValue,
    const std::vector<std::pair<std::string, std::string>>& extra, bool noAuth)
{
    const bool openRouter = base == "https://openrouter.ai/api";
    const bool openAI = base == "https://api.openai.com";
    if (openRouter && !noAuth) {
        auto keyResult = FetchModelIds(base + "/v1/key", headerName, headerValue, extra, true);
        if (!keyResult.ok) return keyResult;
    }
    auto result = FetchModelIds(url, headerName, headerValue, extra);
    result.credentialsVerified = result.ok && !noAuth && (openRouter || openAI);
    return result;
}

// ── Test connection ──────────────────────────────────────────────
// Two minimal POSTs to the chat path, run back-to-back on a worker:
//   1. bare chat      — {"messages":[ping], max_tokens, stream:false}
//   2. tool-bearing   — the same request plus ONE trivial function tool
// The pair isolates exactly the failure class seen on OpenAI-direct
// with reasoning models (chat works, chat+tools 400s), which a single
// probe or the /v1/models fetch cannot distinguish.  A provider that
// refuses tools is a valid [no-tools] endpoint, not a broken one, so
// the two outcomes are reported separately.
struct ProbeOutcome {
    bool        ok = false;
    int         status = 0;      // HTTP status (0 = transport error)
    std::string error;           // short reason (status text / body head)
    Poco::JSON::Object::Ptr message;
    std::string text;
    long        millis = 0;
};

struct TestConnectionResult {
    std::string  model;
    std::string  effort;
    std::string  skipReason;
    ProbeOutcome chat;
    ProbeOutcome tools;
    bool         toolsRun = false;   // false if chat failed (tools skipped)
};

wxDEFINE_EVENT(wxEVT_EP_CONNECTION_TESTED, wxThreadEvent);

ProbeOutcome PostChatProbe(
    const std::string& url,
    const std::string& body,
    const std::string& authHeaderName,
    const std::string& authHeaderValue,
    const std::vector<std::pair<std::string, std::string>>& extraHeaders)
{
    ProbeOutcome out;
    const auto t0 = std::chrono::steady_clock::now();
    try {
        Poco::URI uri(url);
        const std::string scheme = uri.getScheme();
        int port = uri.getPort();
        if (port == 0) port = (scheme == "https") ? 443 : 80;

        std::unique_ptr<Poco::Net::HTTPClientSession> sess;
        if (scheme == "https")
            sess.reset(new Poco::Net::HTTPSClientSession(
                uri.getHost(), (Poco::UInt16)port));
        else
            sess.reset(new Poco::Net::HTTPClientSession(
                uri.getHost(), (Poco::UInt16)port));
        // Longer than the models fetch: a cold local llama-server or a
        // reasoning model can take a while to emit even 8 tokens.
        sess->setTimeout(Poco::Timespan(60, 0));

        std::string path = uri.getPathAndQuery();
        if (path.empty()) path = "/";

        Poco::Net::HTTPRequest req(Poco::Net::HTTPRequest::HTTP_POST, path,
                                   Poco::Net::HTTPMessage::HTTP_1_1);
        req.set("User-Agent",   "LlamaBoss/1.0");
        req.set("Accept",       "application/json");
        req.setContentType("application/json");
        req.setContentLength((std::streamsize)body.size());
        if (!authHeaderName.empty())
            req.set(authHeaderName, authHeaderValue);
        for (const auto& [k, v] : extraHeaders)
            if (!k.empty()) req.set(k, v);

        sess->sendRequest(req) << body;

        Poco::Net::HTTPResponse resp;
        std::istream& in = sess->receiveResponse(resp);
        out.status = (int)resp.getStatus();

        std::string respBody;
        constexpr size_t kMaxBody = 256u * 1024;
        char buf[4096];
        while (in.good() && respBody.size() < kMaxBody) {
            in.read(buf, sizeof(buf));
            const std::streamsize got = in.gcount();
            if (got <= 0) break;
            respBody.append(buf, (size_t)got);
        }

        if (out.status == 200) {
            Poco::JSON::Parser parser;
            const auto root = parser.parse(respBody).extract<Poco::JSON::Object::Ptr>();
            const auto choices = root ? root->getArray("choices") : Poco::JSON::Array::Ptr();
            if (!choices || choices->size() != 1) throw std::runtime_error("Expected one chat completion choice.");
            const auto choice = choices->getObject(0);
            out.message = choice ? choice->getObject("message") : Poco::JSON::Object::Ptr();
            if (!out.message || out.message->optValue<std::string>("role", "") != "assistant")
                throw std::runtime_error("Response is not an assistant chat message.");
            if (out.message->has("content") && !out.message->isNull("content"))
                out.text = out.message->getValue<std::string>("content");
            const auto calls = out.message->getArray("tool_calls");
            out.ok = !out.text.empty() || (calls && calls->size() > 0);
            if (!out.ok) out.error = "Provider returned no text or tool call. Try a lower reasoning level.";
        } else {
            out.error = "HTTP " + std::to_string(out.status) + " " +
                        resp.getReason();
            // Pull the provider's message when the body is JSON with
            // {"error":{"message":...}} (OpenAI / OpenRouter shape);
            // otherwise show a bounded head of the body.
            std::string detail;
            try {
                Poco::JSON::Parser parser;
                auto parsed = parser.parse(respBody);
                auto root   = parsed.extract<Poco::JSON::Object::Ptr>();
                if (root) {
                    if (auto err = root->getObject("error")) {
                        if (err->has("message"))
                            detail = err->getValue<std::string>("message");
                    } else if (root->has("error")) {
                        detail = root->getValue<std::string>("error");
                    }
                }
            } catch (...) {}
            if (detail.empty() && !respBody.empty())
                detail = respBody.substr(0, 160);
            if (!detail.empty()) out.error += " - " + detail;
        }
    }
    catch (const std::exception& e) {
        out.error = e.what();
    }
    out.millis = (long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    return out;
}

std::string BuildProbeBody(const std::string& model, bool withTool,
    const std::string& effort, const std::string& dialect,
    Poco::JSON::Object::Ptr assistant = {}, const std::string& callId = "")
{
    Poco::JSON::Object::Ptr root = new Poco::JSON::Object;
    root->set("model", model);
    root->set(dialect == "openai" ? "max_completion_tokens" : "max_tokens", 1024);
    root->set("stream", false);
    if (effort == "auto") { /* Match /think auto: omit reasoning overrides. */ }
    else if (dialect == "openai") root->set("reasoning_effort", effort);
    else {
        Poco::JSON::Object::Ptr reasoning = new Poco::JSON::Object;
        if (dialect == "template") {
            reasoning->set("enable_thinking", effort != "none");
            root->set("chat_template_kwargs", reasoning);
        } else {
            if (effort == "none") reasoning->set("enabled", false);
            else reasoning->set("effort", effort);
            root->set("reasoning", reasoning);
        }
    }
    Poco::JSON::Array::Ptr msgs = new Poco::JSON::Array;
    Poco::JSON::Object::Ptr user = new Poco::JSON::Object;
    user->set("role", "user");
    user->set("content", withTool
        ? "Call ping exactly once with no arguments, then reply with exactly the text it returns."
        : "Reply with exactly OK.");
    msgs->add(user);
    if (assistant) {
        msgs->add(assistant);
        Poco::JSON::Object::Ptr result = new Poco::JSON::Object;
        result->set("role", "tool"); result->set("tool_call_id", callId);
        result->set("content", "LLAMABOSS_PROBE_OK"); msgs->add(result);
    }
    root->set("messages", msgs);
    if (withTool) {
        Poco::JSON::Object::Ptr params = new Poco::JSON::Object;
        params->set("type", "object");
        params->set("properties", Poco::JSON::Object::Ptr(new Poco::JSON::Object));
        params->set("required", Poco::JSON::Array::Ptr(new Poco::JSON::Array));
        params->set("additionalProperties", false);
        Poco::JSON::Object::Ptr fn = new Poco::JSON::Object;
        fn->set("name", "ping"); fn->set("description", "Returns a fixed connectivity marker. No file or shell access.");
        fn->set("parameters", params);
        Poco::JSON::Object::Ptr tool = new Poco::JSON::Object;
        tool->set("type", "function"); tool->set("function", fn);
        Poco::JSON::Array::Ptr tools = new Poco::JSON::Array; tools->add(tool);
        root->set("tools", tools);
        if (assistant) root->set("tool_choice", "none");
        else {
            Poco::JSON::Object::Ptr choice = new Poco::JSON::Object;
            Poco::JSON::Object::Ptr name = new Poco::JSON::Object;
            name->set("name", "ping"); choice->set("type", "function"); choice->set("function", name);
            root->set("tool_choice", choice);
            root->set("parallel_tool_calls", false);
        }
    }
    std::ostringstream os; root->stringify(os); return os.str();
}

bool ValidateProbeCall(const ProbeOutcome& probe, std::string& callId)
{
    try {
        if (!probe.ok || !probe.message) return false;
        const auto calls = probe.message->getArray("tool_calls");
        if (!calls || calls->size() != 1) return false;
        const auto call = calls->getObject(0);
        if (!call || call->getValue<std::string>("type") != "function") return false;
        const auto fn = call->getObject("function");
        if (!fn || fn->getValue<std::string>("name") != "ping") return false;
        Poco::JSON::Parser parser;
        const auto args = parser.parse(fn->getValue<std::string>("arguments")).extract<Poco::JSON::Object::Ptr>();
        if (!args || args->size() != 0) return false;
        callId = call->getValue<std::string>("id");
        return !callId.empty();
    } catch (...) { return false; }
}

void RedactProbeError(std::string& message, const std::string& key)
{
    if (!key.empty()) {
        size_t pos = 0;
        while ((pos = message.find(key, pos)) != std::string::npos) {
            message.replace(pos, key.size(), "[redacted]"); pos += 10;
        }
    }
    if (message.size() > 900) message.resize(900);
}

class EndpointEditDialog : public wxDialog
{
public:
    // isNew: Add mode.  The endpoint id then follows the display name
    // (slugified) until the user edits the id by hand.  In Edit mode
    // the id is never auto-rewritten: conversations key remote model
    // entries on it, so a casual rename must not silently re-id.
    EndpointEditDialog(wxWindow* parent,
                       const wxString& title,
                       const EndpointStore::Endpoint& seed,
                       SecretsStore* secretsStore,
                       const ThemeData& theme,
                       bool isNew, EndpointStore* endpointStore,
                       const wxString& modelSearch = wxEmptyString)
        : wxDialog(parent, wxID_ANY, title,
                   wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
        , m_isNew(isNew)
        , m_editMode(!isNew)
        , m_seedDisplayName(wxString::FromUTF8(seed.displayName))
        , m_endpointStore(endpointStore)
        , m_secretsStore(secretsStore)
        , m_extraHeaders(seed.extraHeaders)
    {
        SetBackgroundColour(theme.bgDialogSurface);
        m_models = seed.models;

        if (m_secretsStore)
            m_connections = m_secretsStore->ListConnections();
        if (m_endpointStore && m_endpointStore->FindEndpoint(seed.id)) m_seededExistingId = seed.id;

        wxFont base = GetFont();
        base.SetPointSize(11);
        SetFont(base);

        // Two steps share the same underlying editor and persistence path.
        // The form scrolls independently; navigation stays reachable.
        auto* outer = new wxBoxSizer(wxVERTICAL);
        m_stepLabel = new wxStaticText(this, wxID_ANY, "1 of 2 - Connect a provider");
        wxFont heading = GetFont(); heading.SetWeight(wxFONTWEIGHT_BOLD);
        m_stepLabel->SetFont(heading);
        outer->Add(m_stepLabel, 0, wxEXPAND | wxALL, FromDIP(12));
        m_pages = new wxSimplebook(this, wxID_ANY);
        auto* credentials = new wxScrolledWindow(m_pages, wxID_ANY);
        credentials->SetScrollRate(0, FromDIP(12));
        m_credentialsPage = credentials;
        auto* first = new wxBoxSizer(wxVERTICAL);
        auto label = [&](wxWindow* parent, wxSizer* sizer, const wxString& text) {
            auto* result = new wxStaticText(parent, wxID_ANY, text);
            result->Wrap(FromDIP(560));
            result->SetMinSize(wxSize(FromDIP(40), -1));
            sizer->Add(result, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(6));
            return result;
        };
        auto mutedText = [&](wxWindow* parent, const wxString& text) {
            auto* result = new wxStaticText(parent, wxID_ANY, text,
                wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
            result->SetMinSize(wxSize(FromDIP(40), -1));
            m_hints.push_back(result);
            return result;
        };
        label(credentials, first, "Provider");
        wxArrayString providers; providers.Add("OpenAI"); providers.Add("OpenRouter");
        providers.Add("Custom compatible service");
        m_providerChoice = new wxChoice(credentials, wxID_ANY,
            wxDefaultPosition, wxDefaultSize, providers);
        first->Add(m_providerChoice, 0, wxEXPAND);
        label(credentials, first, "API key");
        m_apiKeyField = new wxTextCtrl(credentials, wxID_ANY, wxEmptyString,
            wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
        m_apiKeyField->SetHint("Paste your API key, or leave blank to use your saved key");
        first->Add(m_apiKeyField, 0, wxEXPAND);
        auto* showKey = new wxCheckBox(credentials, wxID_ANY, "Show key");
        first->Add(showKey, 0, wxTOP, FromDIP(6));
        auto* getKey = MakeFlatButton(credentials, wxID_ANY, "Get an API key in your browser", theme);
        first->Add(getKey, 0, wxTOP, FromDIP(6));
        getKey->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            const int preset = m_providerChoice->GetSelection();
            if (preset == 2) {
                SetFetchStatus("Get an API key from your custom provider's website, then paste it here."); return;
            }
            wxLaunchDefaultBrowser(preset == 0 ? "https://platform.openai.com/api-keys" : "https://openrouter.ai/settings/keys");
        });
        showKey->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent& event) {
#ifdef __WXMSW__
            ::SendMessage((HWND)m_apiKeyField->GetHandle(), EM_SETPASSWORDCHAR,
                event.IsChecked() ? 0 : 0x25CF, 0);
#else
            const long style = m_apiKeyField->GetWindowStyleFlag();
            m_apiKeyField->SetWindowStyleFlag(event.IsChecked()
                ? (style & ~wxTE_PASSWORD) : (style | wxTE_PASSWORD));
#endif
            m_apiKeyField->Refresh();
        });
        m_connectionStatus = mutedText(credentials, wxEmptyString);
        first->Add(m_connectionStatus, 0, wxEXPAND | wxTOP, FromDIP(8));
        auto* keyHelp = label(credentials, first,
            "Your key stays out of chat and is saved when you finish setup.");
        keyHelp->Wrap(FromDIP(600));
        m_hints.push_back(keyHelp);

        m_advancedPane = new wxCollapsiblePane(credentials, wxID_ANY, "Advanced",
            wxDefaultPosition, wxDefaultSize, wxCP_DEFAULT_STYLE | wxCP_NO_TLW_RESIZE);
        wxWindow* adv = m_advancedPane->GetPane();
        auto* advSizer = new wxBoxSizer(wxVERTICAL);
        auto* grid = new wxFlexGridSizer(2, FromDIP(8), FromDIP(8));
        grid->AddGrowableCol(1);
        auto fieldLabel = [&](const wxString& text) {
            grid->Add(new wxStaticText(adv, wxID_ANY, text), 0, wxALIGN_CENTER_VERTICAL);
        };
        auto field = [&](const wxString& text, const std::string& value) {
            fieldLabel(text);
            auto* control = new wxTextCtrl(adv, wxID_ANY, wxString::FromUTF8(value));
            grid->Add(control, 1, wxEXPAND); return control;
        };
        m_nameField = field("Display name", seed.displayName);
        m_idField = field("Connection id", seed.id);
        m_originalDialect = seed.reasoningDialect;
        m_idField->Enable(isNew);
        m_baseUrlField = field("Base URL", seed.baseUrl);
        m_chatPathField = field("Chat path", seed.chatPath);
        wxArrayString savedProviders, savedKeys;
        std::set<std::string> seen;
        for (const auto& connection : m_connections) {
            if (seen.insert(connection.provider).second)
                savedProviders.Add(wxString::FromUTF8(connection.provider));
            if (connection.provider == seed.secretProvider)
                savedKeys.Add(wxString::FromUTF8(connection.key));
        }
        fieldLabel("Saved connection");
        m_providerField = new wxComboBox(adv, wxID_ANY, wxString::FromUTF8(seed.secretProvider),
            wxDefaultPosition, wxDefaultSize, savedProviders, wxCB_DROPDOWN);
        grid->Add(m_providerField, 1, wxEXPAND);
        fieldLabel("Key name");
        m_keyField = new wxComboBox(adv, wxID_ANY, wxString::FromUTF8(seed.secretKey),
            wxDefaultPosition, wxDefaultSize, savedKeys, wxCB_DROPDOWN);
        grid->Add(m_keyField, 1, wxEXPAND);
        fieldLabel("Authentication");
        auto* auth = new wxBoxSizer(wxVERTICAL);
        m_bearerRadio = new wxRadioButton(adv, wxID_ANY, "Bearer token",
            wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
        m_xApiKeyRadio = new wxRadioButton(adv, wxID_ANY, "x-api-key header");
        m_noAuthRadio = new wxRadioButton(adv, wxID_ANY, "No authentication (local server)");
        m_bearerRadio->SetValue(seed.authScheme == EndpointStore::AuthScheme::Bearer);
        m_xApiKeyRadio->SetValue(seed.authScheme == EndpointStore::AuthScheme::XApiKey);
        m_noAuthRadio->SetValue(seed.authScheme == EndpointStore::AuthScheme::None);
        for (auto* control : {m_bearerRadio, m_xApiKeyRadio, m_noAuthRadio}) auth->Add(control, 0, wxBOTTOM, FromDIP(4));
        grid->Add(auth, 0, wxEXPAND);
        fieldLabel("Tool protocol");
        auto* protocol = new wxBoxSizer(wxVERTICAL);
        m_nativeRadio = new wxRadioButton(adv, wxID_ANY, "Native function calls",
            wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
        m_xmlRadio = new wxRadioButton(adv, wxID_ANY, "XML fallback");
        m_nativeRadio->SetValue(seed.protocol != ToolProtocol::Xml);
        m_xmlRadio->SetValue(seed.protocol == ToolProtocol::Xml);
        protocol->Add(m_nativeRadio); protocol->Add(m_xmlRadio);
        grid->Add(protocol, 0, wxEXPAND);
        fieldLabel("Reasoning format");
        wxArrayString dialects;
        dialects.Add("Auto (detect from host)"); dialects.Add("OpenAI");
        dialects.Add("OpenRouter"); dialects.Add("Chat template");
        m_dialectChoice = new wxChoice(adv, wxID_ANY, wxDefaultPosition, wxDefaultSize, dialects);
        m_dialectChoice->SetSelection(seed.reasoningDialect == "openai" ? 1 :
            seed.reasoningDialect == "openrouter" ? 2 : seed.reasoningDialect == "template" ? 3 : 0);
        grid->Add(m_dialectChoice, 0, wxEXPAND);
        advSizer->Add(grid, 0, wxEXPAND | wxALL, FromDIP(8));
        m_resolvedUrlLabel = mutedText(adv, wxEmptyString);
        advSizer->Add(m_resolvedUrlLabel, 0, wxEXPAND | wxALL, FromDIP(8));
        auto* manualModels = MakeFlatButton(adv, wxID_ANY, "Enter model IDs without connecting...", theme);
        manualModels->SetToolTip("For services without a model catalog. Credentials will not be verified by this shortcut.");
        advSizer->Add(manualModels, 0, wxALL, FromDIP(8));
        manualModels->Bind(wxEVT_BUTTON, [this](wxCommandEvent& event) {
            m_manualModelsRequested = true;
            OnOK(event);
            m_manualModelsRequested = false;
        });
        adv->SetSizer(advSizer);
        first->Add(m_advancedPane, 0, wxEXPAND | wxTOP, FromDIP(12));
        auto* firstMargins = new wxBoxSizer(wxVERTICAL);
        firstMargins->Add(first, 1, wxEXPAND | wxALL, FromDIP(12));
        credentials->SetSizer(firstMargins);
        m_pages->AddPage(credentials, "Connection", true);

        auto* right = new wxScrolledWindow(m_pages, wxID_ANY);
        right->SetScrollRate(0, FromDIP(12));
        m_modelsPage = right;
        auto* rightSizer = new wxBoxSizer(wxVERTICAL);
        // Edit mode opens here.  The connection is summarised on one
        // line with a way back to the credentials page; Add mode keeps
        // the wizard order and never shows the button.
        auto* connRow = new wxBoxSizer(wxHORIZONTAL);
        m_modelConnectionStatus = new wxStaticText(right, wxID_ANY, wxEmptyString,
            wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
        m_modelConnectionStatus->SetMinSize(wxSize(FromDIP(40), -1));
        connRow->Add(m_modelConnectionStatus, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        m_connectionBtn = MakeFlatButton(right, wxID_ANY, "Connection...", theme);
        m_connectionBtn->SetToolTip("Change the API key or advanced connection settings");
        connRow->Add(m_connectionBtn, 0, wxALIGN_CENTER_VERTICAL);
        m_connectionBtn->Show(m_editMode);
        rightSizer->Add(connRow, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(6));
        auto* searchRow = new wxBoxSizer(wxHORIZONTAL);
        m_filterField = new wxTextCtrl(right, wxID_ANY, modelSearch);
        m_filterField->SetHint("Search models by name or id");
        searchRow->Add(m_filterField, 1, wxEXPAND | wxRIGHT, FromDIP(8));
        m_fetchBtn = MakeAccentButton(right, wxID_ANY, "Refresh models", theme);
        searchRow->Add(m_fetchBtn, 0);
        rightSizer->Add(searchRow, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
        label(right, rightSizer, "Check models to keep in your picker. Highlight a checked model to use in this chat.");
        m_modelList = new wxCheckListBox(right, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(-1, 170)));
        rightSizer->Add(m_modelList, 1, wxEXPAND);
        m_modelsField = new wxTextCtrl(right, wxID_ANY, ModelsToText(seed.models),
            wxDefaultPosition, FromDIP(wxSize(-1, 170)), wxTE_MULTILINE | wxHSCROLL);
        rightSizer->Add(m_modelsField, 1, wxEXPAND); m_modelsField->Hide();
        auto* listFooter = new wxBoxSizer(wxHORIZONTAL);
        m_modelCountLabel = mutedText(right, wxEmptyString);
        listFooter->Add(m_modelCountLabel, 1, wxALIGN_CENTER_VERTICAL);
        m_rawToggleBtn = MakeFlatButton(right, wxID_ANY, "Advanced: model IDs", theme);
        listFooter->Add(m_rawToggleBtn, 0);
        rightSizer->Add(listFooter, 0, wxEXPAND | wxTOP, FromDIP(6));
        m_selectedModelLabel = label(right, rightSizer, "Select a model to edit its options.");
        m_modelNameField = new wxTextCtrl(right, wxID_ANY);
        m_modelNameField->SetHint("Display name in the model picker");
        rightSizer->Add(m_modelNameField, 0, wxEXPAND);
        m_allowTools = new wxCheckBox(right, wxID_ANY, "Allow agent tools");
        m_imageOutput = new wxCheckBox(right, wxID_ANY, "Generate images");
        rightSizer->Add(m_allowTools, 0, wxTOP, FromDIP(6));
        rightSizer->Add(m_imageOutput, 0, wxTOP, FromDIP(4));
        m_compatibilityHint = label(right, rightSizer, wxEmptyString);
        m_compatibilityHint->SetMinSize(wxSize(FromDIP(100), FromDIP(46)));
        auto* testRow = new wxBoxSizer(wxHORIZONTAL);
        testRow->Add(new wxStaticText(right, wxID_ANY, "Test reasoning:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        wxArrayString efforts; efforts.Add("Auto"); efforts.Add("Off"); efforts.Add("Low"); efforts.Add("Medium"); efforts.Add("High");
        m_testEffort = new wxChoice(right, wxID_ANY, wxDefaultPosition, wxDefaultSize, efforts);
        m_testEffort->SetSelection(0); testRow->Add(m_testEffort, 0);
        m_testBtn = MakeFlatButton(right, wxID_ANY, "Test selected model", theme);
        testRow->Add(m_testBtn, 0, wxLEFT, FromDIP(12));
        rightSizer->Add(testRow, 0, wxEXPAND | wxTOP, FromDIP(6));
        auto* testHelp = label(right, rightSizer, "Optional test sends small API requests; normal provider usage charges apply.");
        m_hints.push_back(testHelp);
        auto* rightMargins = new wxBoxSizer(wxVERTICAL);
        rightMargins->Add(rightSizer, 1, wxEXPAND | wxALL, FromDIP(12));
        right->SetSizer(rightMargins);
        m_pages->AddPage(right, "Models");
        outer->Add(m_pages, 1, wxEXPAND);
        m_pages->SetMinSize(FromDIP(wxSize(100, 100)));
        auto* statusRow = new wxBoxSizer(wxHORIZONTAL);
        m_fetchStatus = new wxStaticText(this, wxID_ANY, wxEmptyString);
        m_fetchStatus->SetMinSize(FromDIP(wxSize(100, 44)));
        statusRow->Add(m_fetchStatus, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        m_statusDetails = MakeFlatButton(this, wxID_ANY, "Details...", theme);
        statusRow->Add(m_statusDetails, 0, wxALIGN_CENTER_VERTICAL);
        m_statusDetails->Hide();
        m_statusDetails->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            wxMessageBox(wxString::FromUTF8(m_statusText), "Connection details", wxOK | wxICON_INFORMATION, this);
        });
        outer->Add(statusRow, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
        auto* navigation = new wxBoxSizer(wxHORIZONTAL);
        m_backBtn = MakeFlatButton(this, wxID_ANY, "Back", theme);
        navigation->Add(m_backBtn, 0); navigation->AddStretchSpacer();
        auto* cancelBtn = MakeFlatButton(this, wxID_CANCEL, "Cancel", theme);
        // "Save" keeps the checked list (and any new key) without touching
        // the current chat's model.  "Save and use model" additionally
        // switches to the highlighted checked model and is only enabled
        // when there is one.  Both exist only on the Models page.
        m_saveBtn = MakeFlatButton(this, wxID_ANY, "Save", theme);
        m_saveBtn->SetToolTip("Save the checked models and connection without changing the current chat's model");
        auto* okBtn = MakeAccentButton(this, wxID_OK, "Connect", theme);
        m_finishBtn = okBtn;
        navigation->Add(cancelBtn, 0, wxRIGHT, FromDIP(10));
        navigation->Add(m_saveBtn, 0, wxRIGHT, FromDIP(10));
        navigation->Add(okBtn, 0);
        outer->Add(navigation, 0, wxEXPAND | wxALL, FromDIP(12));
        // Back: wizard goes to the credentials page; in Edit mode the
        // credentials page is the detour, so Back returns to Models.
        m_backBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            ShowStep(m_editMode && m_pages->GetSelection() == 0 ? 1 : 0);
        });
        m_connectionBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ShowStep(0); });
        m_saveBtn->Bind(wxEVT_BUTTON, &EndpointEditDialog::OnSaveOnly, this);
        m_modelList->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) { LoadModelOptions(); });
        m_modelNameField->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { SaveModelOptions(); });
        m_allowTools->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { SaveModelOptions(); });
        m_imageOutput->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { SaveModelOptions(); });
        m_testEffort->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { InvalidateTest(); });

        // ── Wiring ──────────────────────────────────────────────
        m_nameField->Bind(wxEVT_TEXT,
            [this](wxCommandEvent&) { SyncIdFromName(); });
        m_idField->Bind(wxEVT_TEXT,
            [this](wxCommandEvent&) {
                // Only USER edits reach here; programmatic updates use
                // ChangeValue.  From now on the id is theirs.
                m_idEditedByUser = true;
            });
        m_baseUrlField->Bind(wxEVT_TEXT,
            [this](wxCommandEvent&) {
                // The catalog is keyed on the base URL: editing it
                // orphans any in-flight fetch.
                InvalidateFetch();
                UpdateResolvedUrl();
            });
        m_chatPathField->Bind(wxEVT_TEXT,
            [this](wxCommandEvent&) { InvalidateTest(); UpdateResolvedUrl(); });
        m_providerField->Bind(wxEVT_COMBOBOX,
            &EndpointEditDialog::OnProviderChanged, this);
        m_providerField->Bind(wxEVT_TEXT,
            &EndpointEditDialog::OnProviderChanged, this);
        m_keyField->Bind(wxEVT_COMBOBOX,
            &EndpointEditDialog::OnKeyChanged, this);
        m_keyField->Bind(wxEVT_TEXT,
            &EndpointEditDialog::OnKeyChanged, this);
        m_apiKeyField->Bind(wxEVT_TEXT,
            [this](wxCommandEvent&) {
                InvalidateFetch();
                UpdateConnectionStatus();
            });
        // Switching the auth scheme changes the catalog's credential
        // identity (the fetch doubles as the auth test) and whether
        // the key fields matter at all.
        {
            auto onAuthScheme = [this](wxCommandEvent&) {
                InvalidateFetch();
                UpdateAuthFieldEnables();
                UpdateConnectionStatus();
            };
            m_bearerRadio ->Bind(wxEVT_RADIOBUTTON, onAuthScheme);
            m_xApiKeyRadio->Bind(wxEVT_RADIOBUTTON, onAuthScheme);
            m_noAuthRadio ->Bind(wxEVT_RADIOBUTTON, onAuthScheme);
        }
        m_providerChoice->Bind(wxEVT_CHOICE,
            [this](wxCommandEvent& e) { SeedFromPreset(e.GetSelection()); });
        m_fetchBtn->Bind(wxEVT_BUTTON,
            &EndpointEditDialog::OnFetchModels, this);
        Bind(wxEVT_EP_MODELS_FETCHED,
             &EndpointEditDialog::OnModelsFetched, this);
        m_testBtn->Bind(wxEVT_BUTTON,
            &EndpointEditDialog::OnTestConnection, this);
        Bind(wxEVT_EP_CONNECTION_TESTED,
             &EndpointEditDialog::OnConnectionTested, this);
        m_filterField->Bind(wxEVT_TEXT,
            [this](wxCommandEvent&) { RefreshModelChecklist(); });
        m_modelList->Bind(wxEVT_CHECKLISTBOX,
            &EndpointEditDialog::OnModelToggled, this);
        m_modelsField->Bind(wxEVT_TEXT,
            [this](wxCommandEvent&) {
                if (!m_syncingModels) {
                    // Import only the completed draft. Importing every
                    // keystroke would retain partially typed IDs as hidden models.
                    m_rawModelsDirty = true;
                    InvalidateTest();
                }
            });
        m_rawToggleBtn->Bind(wxEVT_BUTTON,
            [this](wxCommandEvent&) { ToggleRawEditor(); });
        m_advancedPane->Bind(wxEVT_COLLAPSIBLEPANE_CHANGED,
            [this](wxCollapsiblePaneEvent&) { ResizeForStep(); RefreshLayout(); });

        m_dialectChoice->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { m_originalDialect.clear(); InvalidateTest(); });
        m_nativeRadio->Bind(wxEVT_RADIOBUTTON, [this](wxCommandEvent&) { InvalidateTest(); });
        m_xmlRadio->Bind(wxEVT_RADIOBUTTON, [this](wxCommandEvent&) { InvalidateTest(); });

        // Provider selection from the seed's base URL; Add passes an
        // OpenAI preset, Edit whatever the endpoint actually is.
        m_providerChoice->SetSelection(DetectPreset(seed.baseUrl));
        m_providerChoice->Enable(isNew);

        UpdateResolvedUrl();
        UpdateAuthFieldEnables();
        UpdateConnectionStatus();
        SetFetchStatus("Connect to check your credentials and load models.");
        RefreshModelChecklist();

        if (m_providerChoice->GetSelection() == 2)
            m_advancedPane->Expand();

        SetSizer(outer);

        // Clamp the wizard to the work area. Both pages can scroll when
        // Advanced or display scaling makes the content taller than the screen.
        int winW = FromDIP(700), winH = FromDIP(560);
        const int displayIndex = wxDisplay::GetFromWindow(
            GetParent() ? GetParent() : static_cast<wxWindow*>(this));
        if (displayIndex != wxNOT_FOUND) {
            const wxRect workArea =
                wxDisplay(static_cast<unsigned>(displayIndex)).GetClientArea();
            winW = std::min(winW, workArea.GetWidth()  * 9 / 10);
            winH = std::min(winH, workArea.GetHeight() * 9 / 10);
        }
        SetSize(wxSize(winW, winH));
        SetMinSize(wxSize(std::min(winW, FromDIP(480)), std::min(winH, FromDIP(400))));
        Layout();
        AlignAdvancedLabels();

        okBtn->Bind(wxEVT_BUTTON, &EndpointEditDialog::OnOK, this);
        okBtn->SetDefault();
        if (m_editMode) {
            // A provider that already works opens on the page people
            // actually come here for.  Credentials are checked by the
            // catalog fetch itself; a 401 lands in the status line and
            // "Connection..." is one click away.
            ShowStep(1);
            const bool noAuth = m_noAuthRadio->GetValue();
            if (noAuth || !EffectiveApiKey().empty()) {
                m_modelConnectionStatus->SetLabel("Loading the model catalog...");
                CallAfter([this]() {
                    if (m_fetchInFlight) return;
                    wxCommandEvent e;
                    OnFetchModels(e);
                });
            } else {
                m_modelConnectionStatus->SetLabel(
                    wxString::FromUTF8(kCross.c_str()) +
                    "No saved key for this connection. Click Connection... to paste one.");
                SetFetchStatus("Your checked models are shown. Refreshing the catalog needs an API key.");
            }
        } else {
            ShowStep(0);
            m_apiKeyField->SetFocus();
        }

        // ── Theming ─────────────────────────────────────────────
        ApplyDialogThemeRecursive(this,
                                  theme.textPrimary,
                                  theme.bgInputField,
                                  theme.textPrimary);

        okBtn    ->SetBackgroundColour(theme.accentButton);
        okBtn    ->SetForegroundColour(theme.accentButtonText);
        cancelBtn->SetBackgroundColour(theme.bgDialogSurface);
        cancelBtn->SetForegroundColour(theme.textMuted);
        m_fetchBtn->SetBackgroundColour(theme.accentButton);
        m_fetchBtn->SetForegroundColour(theme.accentButtonText);
        m_rawToggleBtn->SetBackgroundColour(theme.bgDialogSurface);
        m_rawToggleBtn->SetForegroundColour(theme.textMuted);
        m_saveBtn->SetBackgroundColour(theme.bgDialogSurface);
        m_saveBtn->SetForegroundColour(theme.textPrimary);
        m_connectionBtn->SetBackgroundColour(theme.bgDialogSurface);
        m_connectionBtn->SetForegroundColour(theme.textMuted);
        m_testBtn->SetBackgroundColour(theme.bgDialogSurface);
        m_testBtn->SetForegroundColour(theme.textMuted);

        m_modelList->SetBackgroundColour(theme.bgInputField);
        m_modelList->SetForegroundColour(theme.textPrimary);

        m_advancedPane->SetBackgroundColour(theme.bgDialogSurface);
        m_advancedPane->SetForegroundColour(theme.textPrimary);
        adv->SetBackgroundColour(theme.bgDialogSurface);

        // Recursive theming painted every label textPrimary; restore
        // the muted ones.
        for (auto* h : m_hints) h->SetForegroundColour(theme.textMuted);

        for (wxRadioButton* rb : { m_bearerRadio, m_xApiKeyRadio,
                                   m_nativeRadio, m_xmlRadio }) {
            rb->SetForegroundColour(theme.textPrimary);
            rb->SetBackgroundColour(theme.bgDialogSurface);
        }

        ApplyDarkTitleBar(this, theme.name != "light");
    }
    ~EndpointEditDialog() override
    {
        // Gate for the detached fetch worker.  MUST go through
        // LbMarkUiEventTargetDead — it shares a mutex with
        // LbQueueEventIfAlive, closing the window where the worker has
        // passed the alive check but not yet queued the event to this
        // (about-to-be-destroyed) handler.  A bare store(false) would
        // reintroduce exactly the race ui_event_post.h exists to stop.
        LbMarkUiEventTargetDead(m_alive);
    }

private:
    // ── Provider presets ────────────────────────────────────────
    // Lowercased host parsed out of a URL — scheme, path, userinfo,
    // and port stripped.  Mirrors the hardened sniff in
    // EndpointStore::ResolveTarget so the dialog can never DISPLAY a
    // look-alike host ("api.openai.com.evil.example") as a preset
    // that the store would classify as custom.
    static std::string HostOfUrl(const std::string& url)
    {
        std::string host = url;
        const size_t scheme = host.find("://");
        if (scheme != std::string::npos) host.erase(0, scheme + 3);
        const size_t cut = host.find_first_of("/?#");
        if (cut != std::string::npos) host.erase(cut);
        const size_t at = host.rfind('@');
        if (at != std::string::npos) host.erase(0, at + 1);
        const size_t port = host.rfind(':');
        if (port != std::string::npos) host.erase(port);
        std::transform(host.begin(), host.end(), host.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        return host;
    }

    static int DetectPreset(const std::string& baseUrl)
    {
        const std::string host = HostOfUrl(baseUrl);
        if (host == "api.openai.com")  return 0;
        if (host == "openrouter.ai")   return 1;
        return 2;   // Custom
    }

    void SeedFromPreset(int preset)
    {
        auto p = MakeProviderPreset(preset);
        if (preset == 2) { p.id = "custom"; p.displayName = "Custom provider"; p.secretProvider = "custom"; }
        const auto* configured = m_endpointStore ? m_endpointStore->FindEndpoint(p.id) : nullptr;
        if (configured) p = *configured;
        const std::string oldProvider =
            TrimUtf8(m_providerField->GetValue());

        // ChangeValue everywhere: apply without firing wxEVT_TEXT, so
        // the resolved-URL/status/checklist refreshes below run once,
        // after all fields are consistent.
        if (m_isNew) m_idField->ChangeValue(wxString::FromUTF8(p.id));
        m_nameField   ->ChangeValue(wxString::FromUTF8(p.displayName.c_str()));
        // A preset re-seeds both identity fields; the id follows the
        // name again from here (Add mode only).
        m_idEditedByUser = false;
        m_baseUrlField->ChangeValue(wxString::FromUTF8(p.baseUrl.c_str()));
        m_chatPathField->ChangeValue(wxString::FromUTF8(p.chatPath.c_str()));

        const bool xApi = (p.authScheme == EndpointStore::AuthScheme::XApiKey);
        m_bearerRadio ->SetValue(p.authScheme == EndpointStore::AuthScheme::Bearer);
        m_xApiKeyRadio->SetValue(xApi);
        m_noAuthRadio ->SetValue(p.authScheme == EndpointStore::AuthScheme::None);
        UpdateAuthFieldEnables();
        m_originalDialect = p.reasoningDialect;
        if (m_dialectChoice) m_dialectChoice->SetSelection(p.reasoningDialect == "openai" ? 1 :
            p.reasoningDialect == "openrouter" ? 2 : p.reasoningDialect == "template" ? 3 : 0);
        const bool xml = (p.protocol == ToolProtocol::Xml);
        m_nativeRadio->SetValue(!xml);
        m_xmlRadio   ->SetValue(xml);

        m_extraHeaders = p.extraHeaders;

        m_providerField->ChangeValue(
            wxString::FromUTF8(p.secretProvider.c_str()));
        if (oldProvider != p.secretProvider)
            m_apiKeyField->ChangeValue(wxEmptyString);
        m_keyField->ChangeValue(wxString::FromUTF8(p.secretKey));
        RebuildKeyChoices();

        m_seededExistingId = configured ? p.id : std::string();
        // The model list is part of the seed too: ids configured for
        // one provider are not valid on another (OpenAI's bare ids vs
        // OpenRouter's vendor-prefixed ones), so carrying them across
        // would save a broken endpoint.  Clearing here is safe because
        // the dialog is transactional — Cancel discards everything, so
        // an accidental provider flip during Edit never destroys the
        // stored endpoint.
        m_syncingModels = true;
        m_models = p.models;
        m_rawModelsDirty = false;
        m_modelsField->ChangeValue(ModelsToText(m_models));
        m_syncingModels = false;

        // A different provider means a different catalog, and any
        // in-flight fetch now belongs to the OLD one.
        m_fetchedIds.clear();
        InvalidateFetch();
        RefreshModelChecklist();
        SetFetchStatus(configured ? "Existing connection loaded. Your selected models are preserved."
                                  : "Connect to check your credentials and load models.");

        if (m_advancedPane && m_advancedPane->IsExpanded()) {
            m_advancedPane->Collapse();
            RefreshLayout();
        }
        if (preset == 2) ExpandAdvanced();
        UpdateResolvedUrl();
        UpdateConnectionStatus();
    }

    void ExpandAdvanced()
    {
        if (m_advancedPane && !m_advancedPane->IsExpanded()) {
            m_advancedPane->Expand();
            RefreshLayout();
        }
    }

    void RefreshLayout()
    {
        // The collapsible pane lives inside the left panel; relayout
        // that panel first so its new height is known to the dialog.
        if (m_advancedPane && m_advancedPane->GetParent())
            m_advancedPane->GetParent()->Layout();
        if (m_credentialsPage) m_credentialsPage->FitInside();
        if (m_modelsPage) m_modelsPage->FitInside();
        Layout();
        AlignAdvancedLabels();
    }

    // wxCollapsiblePane insets its child panel by a few px (platform
    // and theme dependent), which would put "Chat path" to the right
    // of "Base URL".  Measure the real inset once the pane is laid
    // out and shrink the Advanced label column by exactly that much.
    void AlignAdvancedLabels()
    {
        if (!m_advGrid || !m_advancedPane || !m_advancedPane->IsExpanded())
            return;
        wxWindow* pane = m_advancedPane->GetPane();
        wxWindow* left = m_advancedPane->GetParent();
        if (!pane || !left || !pane->IsShown()) return;
        const int inset = pane->GetScreenPosition().x -
                          left->GetScreenPosition().x;
        const int want = kLabelWidth - inset;
        if (inset < 0 || inset > kLabelWidth / 2 || want == m_advLabelWidth)
            return;
        m_advLabelWidth = want;
        for (auto* item : m_advGrid->GetChildren()) {
            if (auto* w = dynamic_cast<wxStaticText*>(item->GetWindow()))
                w->SetMinSize(wxSize(want, -1));
        }
        left->Layout();
    }

    // ── Identity helpers ────────────────────────────────────────
    // "FreeToken RunPod" -> "freetoken_runpod".  Lowercase, alnum kept,
    // every other run collapsed to one underscore, edges trimmed —
    // always satisfies IsSafeIdentifier when non-empty.
    static wxString SlugifyId(const wxString& name)
    {
        std::string out;
        bool pendingSep = false;
        for (size_t i = 0; i < name.length(); ++i) {
            const wxUniChar uc = name[i];
            const wxUint32 cp = uc.GetValue();
            if (cp > 0x7F) { pendingSep = !out.empty(); continue; }
            const char c = (char)std::tolower((unsigned char)cp);
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
                if (pendingSep) out += '_';
                pendingSep = false;
                out += c;
            } else {
                pendingSep = !out.empty();
            }
        }
        return wxString::FromUTF8(out.c_str());
    }

    void SyncIdFromName()
    {
        if (!m_isNew || m_idEditedByUser || !m_idField) return;
        m_idField->ChangeValue(SlugifyId(m_nameField->GetValue()));
    }

    // ── Checklist <-> raw text view toggle ──────────────────────
    void ToggleRawEditor()
    {
        if (m_rawMode && m_rawModelsDirty) {
            MergePickerModels(m_models, ParseModelsText(m_modelsField->GetValue()));
            m_rawModelsDirty = false;
        }
        m_rawMode = !m_rawMode;
        m_modelList  ->Show(!m_rawMode);
        m_modelsField->Show(m_rawMode);
        m_filterField->Enable(!m_rawMode);
        m_rawToggleBtn->SetLabel(m_rawMode ? "Show model list" : "Advanced: model IDs");
        LoadModelOptions();
        if (m_rawMode) {
            m_modelCountLabel->SetLabel(
                "id = \"literal name\"   [image]   [text]   [no-tools]   "
                "- one model per line");
        } else {
            RefreshModelChecklist();   // re-derives the count label
        }
        if (m_modelList->GetParent()) m_modelList->GetParent()->Layout();
        if (m_rawMode) m_modelsField->SetFocus();
    }

    void UpdateModelCountLabel(size_t configured, size_t shown)
    {
        if (!m_modelCountLabel || m_rawMode) return;
        wxString text;
        if (m_fetchedIds.empty()) {
            text = configured == 1
                ? wxString("1 model configured")
                : wxString::Format("%lu models configured",
                                   (unsigned long)configured);
        } else {
            text = wxString::Format("%lu of %lu checked",
                                    (unsigned long)configured,
                                    (unsigned long)m_fetchedIds.size());
        }
        const bool filtering = !TrimUtf8(m_filterField->GetValue()).empty();
        if (filtering)
            text += wxString::Format("  (%lu shown)", (unsigned long)shown);
        m_modelCountLabel->SetLabel(text);
    }

    // The key fields are meaningless under "No authentication";
    // disabling them says so louder than any status line could.
    void UpdateAuthFieldEnables()
    {
        const bool useKeys = !(m_noAuthRadio && m_noAuthRadio->GetValue());
        if (m_providerField) m_providerField->Enable(useKeys);
        if (m_keyField)      m_keyField->Enable(useKeys);
        if (m_apiKeyField)   m_apiKeyField->Enable(useKeys);
    }

    std::string EffectiveApiKey() const
    {
        // No-auth endpoints never send a key — even one sitting in the
        // fields from before the scheme was switched.
        if (m_noAuthRadio && m_noAuthRadio->GetValue()) return {};

        const std::string entered =
            m_apiKeyField ? TrimUtf8(m_apiKeyField->GetValue()) : std::string();
        if (!entered.empty()) return entered;

        const std::string provider = TrimUtf8(m_providerField->GetValue());
        const std::string keyName  = TrimUtf8(m_keyField->GetValue());
        if (m_secretsStore && !provider.empty() && !keyName.empty() &&
            m_secretsStore->HasSecret(provider, keyName)) {
            return m_secretsStore->GetSecret(provider, keyName);
        }
        return {};
    }

    // ── Fetch models ────────────────────────────────────────────
    // Anything that changes which endpoint/credential a catalog
    // belongs to bumps the generation; a completed fetch is applied
    // only if its stamped generation still matches, so a result for
    // the OLD provider/URL/key can never populate the NEW one.
    void InvalidateTest()
    {
        ++m_testRevision;
        if (m_hasTestResult || m_testInFlight) {
            m_hasTestResult = false;
            SetFetchStatus("Model or test settings changed. Run the model test again to verify them.");
        }
    }

    void InvalidateFetch()
    {
        ++m_fetchGeneration;
        InvalidateTest();
        // A cached catalog and success banner belong to the old URL/key.
        // Retain user model records, but discard remote catalog entries.
        m_fetchedIds.clear();
        if (m_modelConnectionStatus) {
            m_modelConnectionStatus->SetLabel("Connection changed - not verified. Refresh models to check.");
            m_modelConnectionStatus->SetToolTip(m_modelConnectionStatus->GetLabel());
        }
        SetFetchStatus("Connection changed. Connect or refresh models to verify the new settings.");
        RefreshModelChecklist();
    }

    void SetFetchStatus(const std::string& text)
    {
        if (!m_fetchStatus) return;
        m_statusText = text;
        const wxString full = wxString::FromUTF8(text);
        const bool longText = full.length() > 180;
        wxString summary = full;
        if (longText) summary = full.Left(177) + "...";
        m_fetchStatus->SetLabel(summary);
        m_fetchStatus->Wrap(std::max(FromDIP(240), GetClientSize().x - FromDIP(140)));
        m_fetchStatus->SetToolTip(full);
        if (m_statusDetails) m_statusDetails->Show(longText);
        if (GetSizer()) Layout();
    }

    void OnFetchModels(wxCommandEvent&)
    {
        if (m_fetchInFlight) return;

        const std::string base =
            NormalizeBaseUrl(TrimUtf8(m_baseUrlField->GetValue()));
        if (base.rfind("http://", 0) != 0 &&
            base.rfind("https://", 0) != 0) {
            SetFetchStatus("Base URL must start with http:// or https://.");
            m_baseUrlField->SetFocus();
            return;
        }

        // Resolve auth material HERE — SecretsStore is UI-thread-only,
        // so the worker receives a ready-made header, never the store.
        const std::string key = EffectiveApiKey();
        if (!m_noAuthRadio->GetValue() && key.empty()) { SetFetchStatus("Paste an API key or choose a saved key first."); return; }
        std::string headerName, headerValue;
        if (!key.empty()) {
            if (m_xApiKeyRadio->GetValue()) {
                headerName  = "x-api-key";
                headerValue = key;
            } else {
                headerName  = "Authorization";
                headerValue = "Bearer " + key;
            }
        }

        const std::string url = base + "/v1/models";
        const auto extra = m_extraHeaders;

        m_fetchInFlight = true;
        m_fetchBtn->Enable(false);
        UpdateSaveButtons();
        // Stamp this request with the current generation; the result
        // handler compares against m_fetchGeneration at arrival time.
        const unsigned gen = m_fetchGeneration;
        m_inFlightGeneration = gen;
        const bool noAuth = m_noAuthRadio && m_noAuthRadio->GetValue();
        if (!m_testInFlight && !m_hasTestResult)
            SetFetchStatus(m_connectPending ? "Checking connection and loading models..." : "Refreshing models...");
        // Detached worker: the request is hard-bounded by the 15 s Poco
        // timeout and the result post goes through the alive-token gate,
        // so the worst case at dialog close is a short anonymous
        // lingerer.  (LbBackgroundThreadKeeper is file-local to
        // LlamaBoss.cpp and not reachable from here.)
        wxEvtHandler* target = this;
        std::weak_ptr<std::atomic<bool>> alive = m_alive;
        std::thread([target, alive, url, headerName, headerValue, extra,
                     gen, key, base, noAuth]() {
            FetchModelsResult r = FetchConnectionCatalog(base, url, headerName, headerValue, extra, noAuth);
            RedactProbeError(r.error, key);
            auto* ev = new wxThreadEvent(wxEVT_EP_MODELS_FETCHED);
            ev->SetPayload(r);
            ev->SetInt((int)gen);
            LbQueueEventIfAlive(target, alive, ev);
        }).detach();
    }

    void OnModelsFetched(wxThreadEvent& event)
    {
        const unsigned gen = (unsigned)event.GetInt();
        if (gen != m_inFlightGeneration) return;
        const bool advance = m_connectPending && m_pages->GetSelection() == 0;
        m_connectPending = false;
        m_fetchInFlight = false;
        if (m_fetchBtn) m_fetchBtn->Enable(true);
        UpdateSaveButtons();

        // Stale: provider/URL/credential changed while this request
        // was in flight.  Discard rather than populate the wrong
        // endpoint's checklist.
        if (gen != m_fetchGeneration) {
            if (!m_testInFlight && !m_hasTestResult)
                SetFetchStatus("Connection changed during loading. Connect or refresh models again.");
            return;
        }

        FetchModelsResult r = event.GetPayload<FetchModelsResult>();
        if (!r.ok) {
            if (m_editMode && m_pages->GetSelection() == 1) {
                // The auto-fetch on open failed: say so where the user is
                // looking, and point at the fix.  The checked models are
                // still editable from the configured list.
                const wxString text = wxString::FromUTF8(kCross.c_str()) +
                    ((r.status == 401 || r.status == 403)
                        ? "The saved key was rejected. Click Connection... to paste a new one."
                        : "Could not load the catalog. Your checked models are still shown.");
                m_modelConnectionStatus->SetLabel(text);
                m_modelConnectionStatus->SetToolTip(text);
                if (m_modelConnectionStatus->GetParent()) m_modelConnectionStatus->GetParent()->Layout();
            }
            if (r.status == 401 || r.status == 403) {
                if (!m_testInFlight && !m_hasTestResult)
                    SetFetchStatus(kCross + "Authentication or access check failed (" + r.error +
                                   "). Chat requests use this same key.");
            } else {
                if (!m_testInFlight && !m_hasTestResult)
                    SetFetchStatus(kCross + "Fetch failed: " + r.error);
            }
            return;
        }

        m_fetchedIds = std::move(r.ids);
        RefreshModelChecklist();
        wxString connectionText;
        if (r.credentialsVerified && m_editMode)
            connectionText = wxString::FromUTF8(kCheck.c_str()) + "Connected. Check the models you want in your picker.";
        else if (r.credentialsVerified)
            connectionText = "Credentials accepted. Choose a model to start chatting.";
        else if (m_noAuthRadio->GetValue()) connectionText = "Server reached without authentication. Choose a model.";
        else connectionText = "Model catalog loaded. This custom service's key has not been independently verified.";
        m_modelConnectionStatus->SetLabel(connectionText);
        m_modelConnectionStatus->SetToolTip(connectionText);
        if (m_modelConnectionStatus->GetParent()) m_modelConnectionStatus->GetParent()->Layout();
        if (advance) ShowStep(1);
        if (!m_testInFlight && !m_hasTestResult) SetFetchStatus(m_fetchedIds.empty()
            ? "The catalog is empty. Use Advanced: model IDs if you know the model ID."
            : "Check models to keep, then Save. To switch this chat, highlight a checked model and click Save and use model.");
    }

    // ── Test connection ─────────────────────────────────────────
    // Independent test revision: model edits invalidate a probe without
    // invalidating a catalog fetched with unchanged connection settings.
    void OnTestConnection(wxCommandEvent&)
    {
        if (m_testInFlight) return;

        const std::string base =
            NormalizeBaseUrl(TrimUtf8(m_baseUrlField->GetValue()));
        if (base.rfind("http://", 0) != 0 &&
            base.rfind("https://", 0) != 0) {
            SetFetchStatus(kCross + "Base URL must start with http:// or https://.");
            m_baseUrlField->SetFocus();
            return;
        }
        const auto models = ModelsForPicker(m_models);
        if (models.empty()) {
            SetFetchStatus(kCross + "Check or type at least one model to test with.");
            m_modelList->SetFocus();
            return;
        }
        const int row = m_modelList->GetSelection();
        const std::string selectedId = row >= 0 && row < (int)m_rowModelIds.size() ? m_rowModelIds[row] : std::string();
        auto selected = std::find_if(models.begin(), models.end(), [&](const auto& m) { return m.id == selectedId; });
        if (selected == models.end()) { SetFetchStatus("Select a checked model to test."); return; }
        const std::string model = selected->id;
        if (selected->imageOutput) { SetFetchStatus("This test checks text and tools. Test image generation in a chat."); return; }
        const std::string resolvedPath = lb_responses::ResolveChatPath(
            base, TrimUtf8(m_chatPathField->GetValue()), model);
        if (lb_responses::IsResponsesPath(resolvedPath)) {
            SetFetchStatus("This model uses OpenAI Responses. Test it by sending a chat message (and a tool request with Agent mode on); the connection-test button only speaks Chat Completions.");
            return;
        }
        const bool testTools = !selected->noTools && m_nativeRadio->GetValue();
        const std::string skipReason = selected->noTools ? "Allow agent tools is off" : "XML fallback is selected";
        static const char* efforts[] = {"auto", "none", "low", "medium", "high"};
        const std::string effort = efforts[std::max(0, m_testEffort->GetSelection())];
        std::string dialect = GetEndpoint().reasoningDialect;
        if (dialect.empty()) dialect = DetectPreset(base) == 0 ? "openai" : "openrouter";
        if (effort == "none" && lb_reasoning::RequiresReasoning(model, dialect == "openai")) {
            SetFetchStatus("This model requires reasoning. Select Low or Auto under Test reasoning.");
            m_testEffort->SetFocus();
            return;
        }

        const std::string key = EffectiveApiKey();
        if (!m_noAuthRadio->GetValue() && key.empty()) { SetFetchStatus("Paste an API key or choose a saved key first."); return; }
        std::string headerName, headerValue;
        if (!key.empty()) {
            if (m_xApiKeyRadio->GetValue()) {
                headerName  = "x-api-key";
                headerValue = key;
            } else {
                headerName  = "Authorization";
                headerValue = "Bearer " + key;
            }
        }
        const std::string url = JoinEndpointUrl(
            base, TrimUtf8(m_chatPathField->GetValue()));
        const auto extra = m_extraHeaders;

        m_testInFlight = true;
        m_hasTestResult = false;
        m_testBtn->Enable(false);
        const unsigned gen = m_testRevision;
        m_testGeneration = gen;
        SetFetchStatus("Testing " + model + " - plain chat, then chat with "
                       "a tool...");

        wxEvtHandler* target = this;
        std::weak_ptr<std::atomic<bool>> alive = m_alive;
        std::thread([target, alive, url, model, headerName, headerValue,
                     extra, gen, effort, dialect, testTools, skipReason, key]() {
            TestConnectionResult r;
            r.model = model; r.effort = effort; r.skipReason = skipReason;
            r.chat = PostChatProbe(url, BuildProbeBody(model, false, effort, dialect), headerName, headerValue, extra);
            if (r.chat.ok && r.chat.text.empty()) { r.chat.ok = false; r.chat.error = "Chat returned no text."; }
            if (r.chat.ok && testTools) {
                r.toolsRun = true;
                r.tools = PostChatProbe(url, BuildProbeBody(model, true, effort, dialect), headerName, headerValue, extra);
                std::string callId;
                if (r.tools.ok && !ValidateProbeCall(r.tools, callId)) {
                    r.tools.ok = false; r.tools.error = "No valid ping function call returned; tool support was not verified.";
                }
                if (r.tools.ok) {
                    const long firstMs = r.tools.millis;
                    r.tools = PostChatProbe(url, BuildProbeBody(model, true, effort, dialect, r.tools.message, callId), headerName, headerValue, extra);
                    r.tools.millis += firstMs;
                    if (r.tools.ok && r.tools.text != "LLAMABOSS_PROBE_OK") {
                        r.tools.ok = false; r.tools.error = "Tool result was sent, but the model did not return the expected marker.";
                    }
                }
            }
            RedactProbeError(r.chat.error, key); RedactProbeError(r.tools.error, key);
            // Do not transfer raw provider messages to the UI event.
            r.chat.message.reset(); r.chat.text.clear(); r.tools.message.reset(); r.tools.text.clear();
            auto* ev = new wxThreadEvent(wxEVT_EP_CONNECTION_TESTED);
            ev->SetPayload(r);
            ev->SetInt((int)gen);
            LbQueueEventIfAlive(target, alive, ev);
        }).detach();
    }

    void OnConnectionTested(wxThreadEvent& event)
    {
        const unsigned gen = (unsigned)event.GetInt();
        if (gen == m_testGeneration) {
            m_testInFlight = false;
            LoadModelOptions();
        }
        if (gen != m_testRevision) {
            SetFetchStatus("Discarded a connection test started before "
                           "the settings changed - test again.");
            return;
        }

        m_hasTestResult = true;
        const TestConnectionResult r =
            event.GetPayload<TestConnectionResult>();
        auto ms = [](long v) { return std::to_string(v) + " ms"; };

        if (!r.chat.ok) {
            std::string why = r.chat.error;
            if (r.chat.status == 401 || r.chat.status == 403)
                why += " (key rejected - check the saved connection)";
            else if (r.chat.status == 404)
                why += " (check Base URL and Chat path under Advanced)";
            SetFetchStatus(kCross + "Chat failed for " + r.model + ": " + why);
            return;
        }
        if (!r.toolsRun) {
            SetFetchStatus(kCheck + r.model + " - chat passed. Tool test skipped: " + r.skipReason + ".");
        } else if (r.tools.ok) {
            SetFetchStatus(kCheck + r.model + " - chat and tool round trip passed (reasoning " + r.effort +
                ", " + ms(r.tools.millis) + "). This verifies the ping test, not every agent tool.");
        } else {
            SetFetchStatus("Chat passed; tool round trip failed (reasoning " + r.effort + "): " + r.tools.error);
        }
    }

    // ── Checklist <-> raw text sync ─────────────────────────────
    // Structured model records are authoritative. Display checked records
    // first, retained unchecked records next, and new catalog entries last.
    void RefreshModelChecklist()
    {
        if (!m_modelList) return;

        const auto configured = ModelsForPicker(m_models);
        std::string selectedId;
        const int previousSelection = m_modelList->GetSelection();
        if (previousSelection >= 0 && previousSelection < (int)m_rowModelIds.size())
            selectedId = m_rowModelIds[previousSelection];

        std::string filter = TrimUtf8(m_filterField->GetValue());
        std::transform(filter.begin(), filter.end(), filter.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        auto lower = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) {
                               return (char)std::tolower(c);
                           });
            return s;
        };
        auto matches = [&](const std::string& id, const std::string& name) {
            if (filter.empty()) return true;
            return lower(id).find(filter)   != std::string::npos ||
                   lower(name).find(filter) != std::string::npos;
        };

        m_modelList->Freeze();
        m_modelList->Clear();
        m_rowModelIds.clear();

        std::set<std::string> configuredIds;
        auto appendModel = [&](const EndpointStore::Model& m) {
            configuredIds.insert(m.id);
            if (!matches(m.id, m.displayName)) return;
            std::string label = m.id;
            if (!m.displayName.empty() && m.displayName != m.id &&
                m.displayName != PrettyNameFromId(m.id)) {
                label += " = " + m.displayName;
            }
            if (m.imageOutput && !IdLooksLikeImageModel(m.id))
                label += " [image]";
            if (!m.imageOutput && IdLooksLikeImageModel(m.id))
                label += " [text]";
            if (m.noTools)
                label += " (chat only)";
            const int idx = (int)m_modelList->Append(
                wxString::FromUTF8(label.c_str()));
            m_modelList->Check(idx, m.showInPicker);
            m_rowModelIds.push_back(m.id);
        };
        for (const auto& model : configured) appendModel(model);
        for (const auto& model : m_models)
            if (!model.showInPicker) appendModel(model);

        for (const auto& id : m_fetchedIds) {
            if (configuredIds.count(id)) continue;
            if (!matches(id, PrettyNameFromId(id))) continue;
            const int idx = (int)m_modelList->Append(
                wxString::FromUTF8(id.c_str()));
            m_modelList->Check(idx, false);
            m_rowModelIds.push_back(id);
        }

        if (!selectedId.empty()) {
            auto it = std::find(m_rowModelIds.begin(), m_rowModelIds.end(), selectedId);
            if (it != m_rowModelIds.end()) m_modelList->SetSelection((int)(it - m_rowModelIds.begin()));
        }
        if (m_modelList->GetSelection() == wxNOT_FOUND && !m_rowModelIds.empty()) m_modelList->SetSelection(0);
        m_modelList->Thaw();
        LoadModelOptions();
        UpdateModelCountLabel(configured.size(), m_rowModelIds.size());
    }

    void OnModelToggled(wxCommandEvent& event)
    {
        const int idx = event.GetInt();
        if (idx < 0 || idx >= (int)m_rowModelIds.size()) return;

        const std::string id       = m_rowModelIds[(size_t)idx];
        const bool        nowChecked = m_modelList->IsChecked((unsigned)idx);

        SetModelPickerVisibility(m_models, id, nowChecked);
        m_modelsField->ChangeValue(ModelsToText(m_models));

        // Keep the toggled model selected after the checked block reorders.
        m_modelList->SetSelection(idx);
        InvalidateTest();
        RefreshModelChecklist();
    }

    // ── Existing helpers (unchanged behavior) ───────────────────
    void UpdateResolvedUrl()
    {
        if (!m_resolvedUrlLabel) return;
        const std::string resolved = JoinEndpointUrl(
            TrimUtf8(m_baseUrlField->GetValue()),
            TrimUtf8(m_chatPathField->GetValue()));

        if (resolved.empty()) {
            m_resolvedUrlLabel->SetLabel("POST <base URL + chat path>");
        }
        else {
            m_resolvedUrlLabel->SetLabel(
                wxString::FromUTF8(
                    (std::string("POST ") + resolved).c_str()));
        }
        m_resolvedUrlLabel->SetToolTip(m_resolvedUrlLabel->GetLabel());
        if (m_resolvedUrlLabel->GetParent())
            m_resolvedUrlLabel->GetParent()->Layout();
    }

    void RebuildKeyChoices()
    {
        if (!m_keyField || m_rebuildingKeys) return;

        const std::string provider = TrimUtf8(m_providerField->GetValue());
        const wxString previous = m_keyField->GetValue();
        std::set<std::string> uniqueKeys;
        for (const auto& row : m_connections) {
            if (row.provider == provider)
                uniqueKeys.insert(row.key);
        }

        m_rebuildingKeys = true;
        m_keyField->Freeze();
        m_keyField->Clear();

        bool previousExists = false;
        for (const auto& key : uniqueKeys) {
            const wxString item = wxString::FromUTF8(key.c_str());
            m_keyField->Append(item);
            if (item == previous) previousExists = true;
        }

        if (previousExists) {
            m_keyField->SetValue(previous);
        }
        else if (m_keyField->GetCount() > 0) {
            m_keyField->SetSelection(0);
        }
        else if (!previous.IsEmpty()) {
            m_keyField->SetValue(previous);
        }
        else {
            m_keyField->SetValue("api_key");
        }

        m_keyField->Thaw();
        m_rebuildingKeys = false;
    }

    void UpdateConnectionStatus()
    {
        if (!m_connectionStatus || m_rebuildingKeys) return;

        const std::string provider = TrimUtf8(m_providerField->GetValue());
        const std::string key      = TrimUtf8(m_keyField->GetValue());
        const std::string entered  = m_apiKeyField
            ? TrimUtf8(m_apiKeyField->GetValue()) : std::string();

        if (m_noAuthRadio && m_noAuthRadio->GetValue()) {
            m_connectionStatus->SetLabel(
                "No authentication - requests are sent without a key.");
        }
        else if (!m_secretsStore) {
            m_connectionStatus->SetLabel(
                "Credential storage is unavailable in this window.");
        }
        else if (provider.empty() || key.empty()) {
            m_connectionStatus->SetLabel(
                "Choose a saved connection and key name.");
        }
        else if (!entered.empty()) {
            m_connectionStatus->SetLabel(
                wxString::Format(
                    "Key entered for %s.",
                    wxString::FromUTF8(provider.c_str())));
        }
        else if (!m_secretsStore->HasSecret(provider, key)) {
            m_connectionStatus->SetLabel(
                wxString::FromUTF8(kCross.c_str()) + wxString::Format("No key saved for %s / %s - paste one above.",
                                 wxString::FromUTF8(provider.c_str()),
                                 wxString::FromUTF8(key.c_str())));
        }
        else if (m_secretsStore->GetSecret(provider, key).empty()) {
            m_connectionStatus->SetLabel(
                wxString::FromUTF8(kCross.c_str()) + wxString::Format("%s / %s exists but its value is unavailable.",
                                 wxString::FromUTF8(provider.c_str()),
                                 wxString::FromUTF8(key.c_str())));
        }
        else {
            m_connectionStatus->SetLabel(
                wxString::FromUTF8(kCheck.c_str()) + wxString::Format("Saved key found for %s (leave the API key field blank to reuse it)",
                                 wxString::FromUTF8(provider.c_str())));
        }

        m_connectionStatus->SetToolTip(m_connectionStatus->GetLabel());
        if (m_connectionStatus->GetParent())
            m_connectionStatus->GetParent()->Layout();
    }

    void OnProviderChanged(wxCommandEvent&)
    {
        // A different credential can see a different catalog (and the
        // fetch doubles as its auth test) — orphan any in-flight fetch.
        InvalidateFetch();
        RebuildKeyChoices();
        UpdateConnectionStatus();
    }

    void OnKeyChanged(wxCommandEvent&)
    {
        InvalidateFetch();
        UpdateConnectionStatus();
    }

public:

    const std::string& GetModelToUse() const { return m_modelToUse; }

    EndpointStore::Endpoint GetEndpoint() const
    {
        EndpointStore::Endpoint ep;
        ep.id          = TrimUtf8(m_idField->GetValue());
        ep.displayName = TrimUtf8(m_nameField->GetValue());
        if (ep.displayName.empty()) ep.displayName = ep.id;
        ep.baseUrl     = NormalizeBaseUrl(TrimUtf8(m_baseUrlField->GetValue()));
        ep.chatPath    = NormalizeChatPath(TrimUtf8(m_chatPathField->GetValue()));
        if (m_noAuthRadio && m_noAuthRadio->GetValue())
            ep.authScheme = EndpointStore::AuthScheme::None;
        else
            ep.authScheme = m_xApiKeyRadio->GetValue()
                                ? EndpointStore::AuthScheme::XApiKey
                                : EndpointStore::AuthScheme::Bearer;
        ep.secretProvider = TrimUtf8(m_providerField->GetValue());
        ep.secretKey      = TrimUtf8(m_keyField->GetValue());
        if (ep.secretKey.empty()) ep.secretKey = "api_key";
        ep.protocol    = m_xmlRadio->GetValue() ? ToolProtocol::Xml
                                                : ToolProtocol::Native;
        switch (m_dialectChoice ? m_dialectChoice->GetSelection() : 0) {
            case 1:  ep.reasoningDialect = "openai";     break;
            case 2:  ep.reasoningDialect = "openrouter"; break;
            case 3:  ep.reasoningDialect = "template";   break;
            default: ep.reasoningDialect = m_originalDialect; break;  // preserve unknown overrides until edited
        }
        ep.models      = m_models;
        if (m_rawModelsDirty)
            MergePickerModels(ep.models, ParseModelsText(m_modelsField->GetValue()));
        ep.extraHeaders = m_extraHeaders;
        return ep;
    }

private:
    // Field checks shared by Connect / Save / Save and use model.  On
    // failure the credentials page is shown with Advanced open and the
    // message in the status line; returns false.
    bool ValidateConnectionFields(EndpointStore::Endpoint& ep, bool& noAuth)
    {
        ep = GetEndpoint();
        auto connectionError = [this](const wxString& message) {
            ShowStep(0);
            m_advancedPane->Expand();
            RefreshLayout();
            SetFetchStatus(TrimUtf8(message));
        };
        if (!IsSafeIdentifier(wxString::FromUTF8(ep.id))) {
            connectionError("Choose a connection id using lowercase letters, digits or underscores in Advanced."); return false;
        }
        try {
            Poco::URI uri(ep.baseUrl);
            if ((uri.getScheme() != "http" && uri.getScheme() != "https") || uri.getHost().empty() ||
                !uri.getUserInfo().empty() || !uri.getQuery().empty() || !uri.getFragment().empty())
                throw std::runtime_error("Invalid base URL");
        } catch (...) {
            connectionError("Enter a valid HTTP(S) base URL in Advanced, without credentials, query or fragment."); return false;
        }
        noAuth = ep.authScheme == EndpointStore::AuthScheme::None;
        if (!noAuth && (!IsSafeIdentifier(wxString::FromUTF8(ep.secretProvider)) ||
                       !IsSafeIdentifier(wxString::FromUTF8(ep.secretKey)))) {
            connectionError("Choose a saved-key provider and key name in Advanced (lowercase letters, digits or underscores)."); return false;
        }
        if (!noAuth && EffectiveApiKey().empty()) {
            ShowStep(0); SetFetchStatus("Paste your API key to continue, or choose a saved key in Advanced.");
            m_apiKeyField->SetFocus(); return false;
        }
        if (!m_endpointStore || (!noAuth && !m_secretsStore)) {
            SetFetchStatus("Connection storage is unavailable. Restart LlamaBoss and try again."); return false;
        }
        return true;
    }

    // Persist the endpoint and (if one was typed) its key, with the same
    // rollback rules as before.  Returns true when everything is on disk.
    bool CommitEndpoint(const EndpointStore::Endpoint& ep, bool noAuth)
    {
        EndpointStore::Endpoint previousEndpoint;
        const auto* existing = m_endpointStore->FindEndpoint(ep.id);
        const bool hadEndpoint = existing != nullptr;
        if (hadEndpoint) previousEndpoint = *existing;
        if (m_isNew && hadEndpoint && m_seededExistingId != ep.id && wxMessageBox(
            "This connection already exists. Replace its model list and settings? Use Edit to keep its existing models.",
            "Replace connection?", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES) return false;

        const std::string enteredKey = noAuth ? std::string() : TrimUtf8(m_apiKeyField->GetValue());
        SecretsStore::SecretEntry previousKey;
        const bool hadKey = !enteredKey.empty() && m_secretsStore->TryGetSecretEntry(ep.secretProvider, ep.secretKey, previousKey);
        auto restoreKey = [&]() {
            if (enteredKey.empty()) return;
            if (!hadKey) m_secretsStore->RemoveSecret(ep.secretProvider, ep.secretKey);
            else if (previousKey.isEnvRef) m_secretsStore->SetSecretEnvRef(ep.secretProvider, ep.secretKey, previousKey.value);
            else m_secretsStore->SetSecret(ep.secretProvider, ep.secretKey, previousKey.value);
        };
        if (!enteredKey.empty()) {
            m_secretsStore->SetSecret(ep.secretProvider, ep.secretKey, enteredKey);
            if (!m_secretsStore->Save()) {
                restoreKey();
                SetFetchStatus("Could not save the API key. Connection settings were not changed. Check the data folder is writable."); return false;
            }
        }
        m_endpointStore->UpsertEndpoint(ep);
        if (!m_endpointStore->Save()) {
            if (hadEndpoint) m_endpointStore->UpsertEndpoint(previousEndpoint);
            else m_endpointStore->RemoveEndpoint(ep.id);
            restoreKey();
            const bool rollbackOk = enteredKey.empty() || m_secretsStore->Save();
            SetFetchStatus(rollbackOk
                ? "Could not save the connection. Previous settings and key were restored. Check the data folder is writable."
                : "Could not save the connection or restore the previous key on disk. Check service keys before restarting.");
            return false;
        }
        m_apiKeyField->ChangeValue(wxEmptyString);
        return true;
    }

    // "Save": keep the list and connection, leave the current chat alone.
    void OnSaveOnly(wxCommandEvent&)
    {
        if (m_fetchInFlight && m_connectPending) return;
        EndpointStore::Endpoint ep; bool noAuth = false;
        if (!ValidateConnectionFields(ep, noAuth)) return;
        if (!CommitEndpoint(ep, noAuth)) return;
        m_modelToUse.clear();
        EndModal(wxID_OK);
    }

    // Accent button: "Connect" on the credentials page, "Save and use
    // model" on the Models page.
    void OnOK(wxCommandEvent&)
    {
        EndpointStore::Endpoint ep; bool noAuth = false;
        if (!ValidateConnectionFields(ep, noAuth)) return;
        if (m_pages->GetSelection() == 0) {
            if (m_fetchInFlight) return;
            if (m_manualModelsRequested) {
                m_modelConnectionStatus->SetLabel("Manual setup: connection and key have not been verified.");
                ShowStep(1);
                if (!m_rawMode) ToggleRawEditor();
                SetFetchStatus("Enter model IDs, then switch to the list and select a checked model to use.");
                return;
            }
            m_connectPending = true;
            wxCommandEvent e;
            OnFetchModels(e);
            return;
        }
        if (m_rawMode) {
            ToggleRawEditor();
            SetFetchStatus("Highlight the checked model you want to use, then Save and use model - or click Save to keep the list.");
            return;
        }
        const int selectedRow = m_modelList->GetSelection();
        if (selectedRow < 0 || selectedRow >= (int)m_rowModelIds.size() ||
            !m_modelList->IsChecked(selectedRow)) {
            SetFetchStatus("Highlight a checked model to use. Click Save instead to keep the list without switching."); return;
        }
        const std::string selectedId = m_rowModelIds[selectedRow];
        if (std::none_of(ep.models.begin(), ep.models.end(), [&](const auto& model) { return model.id == selectedId && model.showInPicker; })) {
            SetFetchStatus("Select a checked model from the list before saving."); return;
        }
        if (!CommitEndpoint(ep, noAuth)) return;
        m_modelToUse = "remote:" + ep.id + "/" + selectedId;
        EndModal(wxID_OK);
    }

    void ResizeForStep()
    {
        const bool expanded = m_pages->GetSelection() == 0 && m_advancedPane->IsExpanded();
        int height = FromDIP(m_pages->GetSelection() == 1 || expanded ? 780 : 560);
        const int displayIndex = wxDisplay::GetFromWindow(GetParent() ? GetParent() : this);
        if (displayIndex != wxNOT_FOUND) {
            const auto area = wxDisplay(static_cast<unsigned>(displayIndex)).GetClientArea();
            height = std::min(height, area.GetHeight() * 9 / 10);
            SetSize(wxSize(std::min(GetSize().x, area.GetWidth() * 9 / 10), height));
            const auto pos = GetPosition();
            Move(std::max(area.x, std::min(pos.x, area.GetRight() - GetSize().x + 1)),
                 std::max(area.y, std::min(pos.y, area.GetBottom() - GetSize().y + 1)));
        } else SetSize(wxSize(GetSize().x, height));
    }

    void ShowStep(int step)
    {
        m_pages->SetSelection(step);
        if (m_editMode) {
            const wxString name = m_seedDisplayName.IsEmpty() ? wxString("Provider") : m_seedDisplayName;
            m_stepLabel->SetLabel(name + (step == 0 ? " - Connection" : " - Models"));
            // The credentials page is a detour in Edit mode: Back leads
            // to Models, and Models itself needs no Back at all.
            m_backBtn->SetLabel("Back to models");
            m_backBtn->Show(step == 0);
        } else {
            m_stepLabel->SetLabel(step == 0 ? "1 of 2 - Connect a provider" : "2 of 2 - Choose a model");
            m_backBtn->SetLabel("Back");
            m_backBtn->Show(step != 0);
        }
        m_saveBtn->Show(step == 1);
        m_finishBtn->SetLabel(step == 0 ? "Connect" : "Save and use model");
        UpdateSaveButtons();
        if (step == 1) m_filterField->SetFocus();
        ResizeForStep();
        RefreshLayout();
    }

    // "Save and use model" needs a highlighted, checked row; "Save" never
    // does.  On the credentials page the accent button is Connect and
    // always available.  Raw-text mode keeps the accent button live: its
    // handler flips back to the list so the user can pick.
    void UpdateSaveButtons()
    {
        if (!m_finishBtn || !m_pages) return;
        const bool connecting = m_connectPending && m_fetchInFlight;
        const bool connectionPage = m_pages->GetSelection() == 0;
        m_finishBtn->SetLabel(connectionPage
            ? (connecting ? "Connecting..." : "Connect") : "Save and use model");
        if (m_saveBtn) m_saveBtn->Enable(!connecting);
        if (connectionPage) {
            m_finishBtn->Enable(!m_fetchInFlight);
            m_finishBtn->SetToolTip(wxString());
            return;
        }
        const int row = m_modelList ? m_modelList->GetSelection() : wxNOT_FOUND;
        const bool usable = m_rawMode ||
            (row >= 0 && row < (int)m_rowModelIds.size() && m_modelList->IsChecked(row));
        m_finishBtn->Enable(usable && !connecting);
        m_finishBtn->SetToolTip(usable ? wxString() :
            wxString("Highlight a checked model to enable. Save keeps the list without switching."));
    }

    void LoadModelOptions()
    {
        const int row = m_modelList->GetSelection();
        const bool valid = row >= 0 && row < (int)m_rowModelIds.size();
        const bool checked = valid && !m_rawMode && m_modelList->IsChecked(row);
        const std::string selectedId = valid ? m_rowModelIds[row] : std::string();
        if (m_optionsModelId != selectedId) {
            m_optionsModelId = selectedId;
            InvalidateTest();
        }
        UpdateSaveButtons();
        m_modelNameField->Enable(checked);
        m_allowTools->Enable(checked);
        m_imageOutput->Enable(checked);
        m_testBtn->Enable(checked && !m_testInFlight);
        m_selectedModelLabel->SetLabel(valid ? wxString::FromUTF8(m_rowModelIds[row]) : wxString("Select a model to edit its options."));
        m_modelNameField->ChangeValue(wxEmptyString);
        m_allowTools->SetValue(true);
        m_imageOutput->SetValue(false);
        if (valid) {
            for (const auto& model : ModelsForPicker(m_models)) {
                if (model.id != m_rowModelIds[row]) continue;
                m_modelNameField->ChangeValue(wxString::FromUTF8(model.displayName));
                m_allowTools->SetValue(!model.noTools);
                m_imageOutput->SetValue(model.imageOutput);
                m_allowTools->Enable(checked && !model.imageOutput);
                break;
            }
        }
        const bool viaResponses = valid && DetectPreset(TrimUtf8(m_baseUrlField->GetValue())) == 0 &&
            !m_imageOutput->GetValue() &&
            lb_responses::IsResponsesPath(lb_responses::ResolveChatPath(
                TrimUtf8(m_baseUrlField->GetValue()),
                TrimUtf8(m_chatPathField->GetValue()), m_rowModelIds[row]));
        m_compatibilityHint->SetLabel(viaResponses
            ? "This model is routed through OpenAI Responses: tools and reasoning work together at any /think level. Leave Allow agent tools on."
            : "Allow agent tools makes tools available when Agent mode is on. Image generation uses a separate, tools-free request.");
        m_compatibilityHint->Wrap(FromDIP(560));
    }

    void SaveModelOptions()
    {
        const int row = m_modelList->GetSelection();
        if (row < 0 || row >= (int)m_rowModelIds.size() || !m_modelList->IsChecked(row)) return;
        for (auto& model : m_models) {
            if (model.id != m_rowModelIds[row]) continue;
            model.displayName = TrimUtf8(m_modelNameField->GetValue());
            if (model.displayName.empty()) model.displayName = PrettyNameFromId(model.id);
            model.noTools = !m_allowTools->GetValue();
            model.imageOutput = m_imageOutput->GetValue();
            m_allowTools->Enable(!model.imageOutput);
        }
        m_modelsField->ChangeValue(ModelsToText(m_models));
        InvalidateTest();
    }

    std::vector<EndpointStore::Model> m_models;
    std::string m_optionsModelId;
    std::string m_modelToUse;
    bool m_editMode = false;          // existing provider: open on Models, connection is a detour
    wxString m_seedDisplayName;
    wxButton* m_saveBtn = nullptr;
    wxButton* m_connectionBtn = nullptr;
    bool m_connectPending = false;
    bool m_manualModelsRequested = false;
    std::string m_statusText;
    wxButton* m_statusDetails = nullptr;
    wxStaticText* m_modelConnectionStatus = nullptr;

    wxSimplebook* m_pages = nullptr;
    wxScrolledWindow* m_credentialsPage = nullptr;
    wxScrolledWindow* m_modelsPage = nullptr;
    wxStaticText* m_stepLabel = nullptr;
    wxStaticText* m_selectedModelLabel = nullptr;
    wxStaticText* m_compatibilityHint = nullptr;
    wxTextCtrl* m_modelNameField = nullptr;
    wxCheckBox* m_allowTools = nullptr;
    wxCheckBox* m_imageOutput = nullptr;
    wxChoice* m_testEffort = nullptr;
    wxButton* m_finishBtn = nullptr;
    wxButton* m_backBtn = nullptr;
    EndpointStore* m_endpointStore = nullptr;
    std::string m_originalDialect;
    std::string m_seededExistingId;

    // ── Members ─────────────────────────────────────────────────
    wxChoice*      m_providerChoice = nullptr;
    wxTextCtrl*    m_idField       = nullptr;
    wxTextCtrl*    m_nameField     = nullptr;
    wxTextCtrl*    m_baseUrlField  = nullptr;
    wxTextCtrl*    m_chatPathField = nullptr;
    wxRadioButton* m_bearerRadio   = nullptr;
    wxRadioButton* m_xApiKeyRadio  = nullptr;
    wxRadioButton* m_noAuthRadio   = nullptr;
    wxChoice*      m_dialectChoice = nullptr;
    wxComboBox*    m_providerField = nullptr;
    wxComboBox*    m_keyField      = nullptr;
    wxTextCtrl*    m_apiKeyField   = nullptr;
    wxRadioButton* m_nativeRadio   = nullptr;
    wxRadioButton* m_xmlRadio      = nullptr;
    wxTextCtrl*    m_modelsField   = nullptr;
    wxStaticText*  m_resolvedUrlLabel = nullptr;
    wxStaticText*  m_connectionStatus = nullptr;

    wxCollapsiblePane* m_advancedPane = nullptr;
    wxFlexGridSizer*   m_advGrid      = nullptr;
    int                m_advLabelWidth = kLabelWidth;
    wxButton*          m_fetchBtn     = nullptr;
    wxStaticText*      m_fetchStatus  = nullptr;   // footer status line
    wxTextCtrl*        m_filterField  = nullptr;
    wxCheckListBox*    m_modelList    = nullptr;
    wxStaticText*      m_modelCountLabel = nullptr;
    wxButton*          m_rawToggleBtn = nullptr;
    bool               m_rawMode      = false;   // text editor shown

    // Add mode: id follows the display name until edited by hand.
    const bool m_isNew;
    bool       m_idEditedByUser = false;

    // Checklist row -> model id, parallel to the visible (filtered)
    // rows; rebuilt by RefreshModelChecklist.
    std::vector<std::string> m_rowModelIds;
    // Catalog returned by the last successful fetch (sorted ids).
    std::vector<std::string> m_fetchedIds;
    bool m_syncingModels = false;
    bool m_rawModelsDirty = false;
    bool m_fetchInFlight = false;
    wxButton* m_testBtn  = nullptr;
    bool      m_testInFlight   = false;
    bool      m_hasTestResult  = false;
    unsigned  m_testGeneration = 0;
    unsigned  m_testRevision = 0;
    // Catalog identity stamp: bumped whenever provider / base URL /
    // credential changes; a fetch result is applied only if its
    // launch-time stamp still matches (see OnModelsFetched).
    unsigned m_fetchGeneration    = 0;
    unsigned m_inFlightGeneration = 0;
    std::shared_ptr<std::atomic<bool>> m_alive =
        std::make_shared<std::atomic<bool>>(true);

    SecretsStore* m_secretsStore = nullptr;
    std::vector<SecretsStore::ConnectionRow> m_connections;
    std::vector<std::pair<std::string, std::string>> m_extraHeaders;
    bool m_rebuildingKeys = false;
    std::vector<wxStaticText*> m_hints;
};

}  // anonymous namespace

// ─── Event table ────────────────────────────────────────────────

wxBEGIN_EVENT_TABLE(EndpointsDialog, wxDialog)
    EVT_BUTTON(ID_EP_ADD,    EndpointsDialog::OnAdd)
    EVT_BUTTON(ID_EP_EDIT,   EndpointsDialog::OnEdit)
    EVT_BUTTON(ID_EP_DELETE, EndpointsDialog::OnDelete)
    EVT_BUTTON(wxID_CLOSE,   EndpointsDialog::OnClose)
    EVT_LIST_ITEM_ACTIVATED(ID_EP_LIST, EndpointsDialog::OnItemActivated)
    EVT_LIST_ITEM_SELECTED  (ID_EP_LIST, EndpointsDialog::OnSelectionChanged)
    EVT_LIST_ITEM_DESELECTED(ID_EP_LIST, EndpointsDialog::OnSelectionChanged)
wxEND_EVENT_TABLE()

// ─── ctor / layout ──────────────────────────────────────────────

EndpointsDialog::EndpointsDialog(wxWindow* parent,
                                 EndpointStore* store,
                                 SecretsStore* secretsStore,
                                 const ThemeData& theme)
    : wxDialog(parent, wxID_ANY, "Connections",
               wxDefaultPosition, wxSize(620, 440),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_store(store)
    , m_secretsStore(secretsStore)
    , m_theme(&theme)
{
    wxFont base = GetFont();
    base.SetPointSize(11);
    SetFont(base);

    auto* root = new wxBoxSizer(wxVERTICAL);

    auto* help = new wxStaticText(this, wxID_ANY,
        "Connect an AI provider, then choose the models you want in your model picker.");
    help->Wrap(580);
    root->Add(help, 0, wxALL, 12);

    m_list = new wxListCtrl(this, ID_EP_LIST,
                            wxDefaultPosition, wxDefaultSize,
                            wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_HRULES);
    m_list->InsertColumn(0, "AI provider", wxLIST_FORMAT_LEFT, 150);
    m_list->InsertColumn(1, "API key", wxLIST_FORMAT_LEFT, 160);
    m_list->InsertColumn(2, "Models",   wxLIST_FORMAT_LEFT, 240);
    root->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT, 12);

    auto* btnRow = new wxBoxSizer(wxHORIZONTAL);
    m_addBtn   = MakeAccentButton     (this, ID_EP_ADD,    "Add AI provider",  theme);
    m_editBtn  = MakeAccentButton     (this, ID_EP_EDIT,   "Edit...", theme);
    m_delBtn   = MakeDestructiveButton(this, ID_EP_DELETE, "Delete",  theme);
    m_closeBtn = MakeFlatButton       (this, wxID_CLOSE,   "Close",   theme);

    btnRow->Add(m_addBtn,  0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
    btnRow->Add(m_editBtn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
    btnRow->Add(m_delBtn,  0, wxALIGN_CENTER_VERTICAL);
    btnRow->AddStretchSpacer();
    btnRow->Add(m_closeBtn, 0, wxALIGN_CENTER_VERTICAL);
    root->Add(btnRow, 0, wxEXPAND | wxALL, 12);
    // Give service credentials a distinct section and an explicit action.
    root->Add(MakeHairline(this, theme), 0,
        wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    auto* servicesRow = new wxBoxSizer(wxHORIZONTAL);
    auto* servicesText = new wxBoxSizer(wxVERTICAL);
    auto* servicesTitle = new wxStaticText(this, wxID_ANY, "Service connections");
    wxFont servicesFont = servicesTitle->GetFont();
    servicesFont.SetWeight(wxFONTWEIGHT_SEMIBOLD);
    servicesTitle->SetFont(servicesFont);
    servicesText->Add(servicesTitle, 0, wxBOTTOM, FromDIP(4));
    auto* servicesHint = new wxStaticText(this, wxID_ANY,
        "API keys for Gmail, RunPod and other services used by skills.");
    servicesHint->Wrap(FromDIP(280));
    servicesText->Add(servicesHint, 0, wxEXPAND);
    servicesRow->Add(servicesText, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(16));
    auto* serviceKeys = MakeAccentButton(this, wxID_ANY,
        "Manage service &keys...", theme, FromDIP(36));
    serviceKeys->SetToolTip("Add, edit or remove saved API keys for services used by skills.");
    servicesRow->Add(serviceKeys, 0, wxALIGN_CENTER_VERTICAL);
    root->Add(servicesRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    serviceKeys->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (!m_secretsStore) return;
        ConnectionsDialog dlg(this, m_secretsStore, *m_theme);
        dlg.ShowModal();
        if (!m_secretsStore->Save()) wxMessageBox("Service keys could not be saved to disk. Changes apply only to this session.", "Save failed", wxOK | wxICON_ERROR, this);
        RebuildList();
    });

    SetSizer(root);
    SetMinSize(FromDIP(wxSize(600, 420)));
    ApplyTheme();
    // Recursive dialog theming paints every button with the input colour.
    // Restore the action colour after that pass, as for Add/Edit above.
    serviceKeys->SetBackgroundColour(theme.accentButton);
    serviceKeys->SetForegroundColour(theme.accentButtonText);
    servicesHint->SetForegroundColour(theme.textMuted);
    RebuildList();
    UpdateButtonState();

    m_closeBtn->SetDefault();
    SetEscapeId(wxID_CLOSE);
}

void EndpointsDialog::ApplyTheme()
{
    if (!m_theme) return;

    SetBackgroundColour(m_theme->bgDialogSurface);

    m_list->SetBackgroundColour(m_theme->bgInputArea);
    m_list->SetForegroundColour(m_theme->textPrimary);

    ApplyDialogThemeRecursive(this,
                              m_theme->textPrimary,
                              m_theme->bgInputField,
                              m_theme->textPrimary);

    m_addBtn  ->SetBackgroundColour(m_theme->accentButton);
    m_addBtn  ->SetForegroundColour(m_theme->accentButtonText);
    m_editBtn ->SetBackgroundColour(m_theme->accentButton);
    m_editBtn ->SetForegroundColour(m_theme->accentButtonText);
    m_delBtn  ->SetBackgroundColour(m_theme->stopButton);
    m_delBtn  ->SetForegroundColour(m_theme->stopButtonText);
    m_closeBtn->SetBackgroundColour(m_theme->bgDialogSurface);
    m_closeBtn->SetForegroundColour(m_theme->textMuted);

    ApplyDarkTitleBar(this, m_theme->name != "light");
}

// ─── List management ────────────────────────────────────────────

void EndpointsDialog::RebuildList(const wxString& selectId)
{
    m_list->DeleteAllItems();
    m_rowIds.clear();
    if (!m_store) return;

    long idx = 0;
    for (const auto& ep : m_store->Endpoints()) {
        const wxString name = wxString::FromUTF8(ep.displayName.c_str());
        long item = m_list->InsertItem(idx, name);
        const bool noAuth = ep.authScheme == EndpointStore::AuthScheme::None;
        const bool hasKey = m_secretsStore && !m_secretsStore->GetSecret(ep.secretProvider, ep.secretKey).empty();
        m_list->SetItem(item, 1, noAuth ? "Not required" : hasKey ? "Saved" : "Needs API key");

        std::string modelsCol;
        for (const auto& model : ep.models) {
            if (!model.showInPicker) continue;
            if (!modelsCol.empty()) modelsCol += ", ";
            modelsCol += model.displayName;
        }
        m_list->SetItem(item, 2, wxString::FromUTF8(modelsCol.c_str()));

        m_rowIds.push_back(ep.id);

        if (!selectId.IsEmpty() &&
            wxString::FromUTF8(ep.id.c_str()) == selectId) {
            m_list->SetItemState(item,
                                 wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                                 wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
            m_list->EnsureVisible(item);
        }
        ++idx;
    }
}

void EndpointsDialog::UpdateButtonState()
{
    bool hasSelection = (m_list->GetSelectedItemCount() > 0);
    m_editBtn->Enable(hasSelection);
    m_delBtn->Enable(hasSelection);
}

void EndpointsDialog::OnSelectionChanged(wxListEvent&)
{
    UpdateButtonState();
}

void EndpointsDialog::OnItemActivated(wxListEvent&)
{
    wxCommandEvent dummy;
    OnEdit(dummy);
}

// ─── Add / Edit / Delete ────────────────────────────────────────

namespace {
// One store's recovery prompt.  Store is EndpointStore or SecretsStore;
// both expose the same load-failure API.
template <typename Store>
bool LbResolveStoreLoadFailure(wxWindow* parent, Store* store,
                               const wxString& what, const std::string& filePath)
{
    while (store && store->LoadFailed()) {
        wxString msg;
        msg << "LlamaBoss couldn't load your saved " << what << ":\n"
            << wxString::FromUTF8(filePath) << "\n\n"
            << wxString::FromUTF8(store->LoadError()) << ".\n\n"
            << "To protect that file, LlamaBoss won't save any changes over it.\n";
        if (!store->LoadBackupPath().empty())
            msg << "A copy was kept at:\n" << wxString::FromUTF8(store->LoadBackupPath()) << "\n";
        else
            msg << "No copy could be made, so Start Fresh would replace the only copy.\n";
        msg << "\nRetry: read the file again (after fixing or restoring it).\n"
            << "Start Fresh: replace it with the current settings; anything missing must be re-entered.\n"
            << "Cancel: leave everything as it is for now.";

        wxMessageDialog dlg(parent, msg, "Saved Settings Not Loaded",
                            wxYES_NO | wxCANCEL | wxCANCEL_DEFAULT | wxICON_WARNING);
        dlg.SetYesNoCancelLabels("Retry", "Start Fresh", "Cancel");
        const int answer = dlg.ShowModal();
        if (answer == wxID_YES) { store->Load(); continue; }
        if (answer == wxID_NO)  { store->ResetAfterFailedLoad(); break; }
        return false;
    }
    return true;
}
} // namespace

bool LbEnsureConnectionStoresWritable(wxWindow* parent, EndpointStore* endpoints,
                                      SecretsStore* secrets)
{
    if (!LbResolveStoreLoadFailure(parent, endpoints, "connections",
                                   EndpointStore::GetEndpointsFilePath()))
        return false;
    return LbResolveStoreLoadFailure(parent, secrets, "API keys",
                                     SecretsStore::GetSecretsFilePath());
}

bool LbShowAIConnectionSetup(wxWindow* parent, EndpointStore* store, SecretsStore* secrets,
    const ThemeData& theme, const std::string& provider, const std::string& modelSearch, std::string* modelToUse)
{
    if (modelToUse) modelToUse->clear();
    if (!store) return false;
    if (!LbEnsureConnectionStoresWritable(parent, store, secrets)) return false;
    if (provider != "openrouter" && provider != "openai" && provider != "custom") return false;
    auto seed = MakeProviderPreset(provider == "openai" ? 0 : provider == "openrouter" ? 1 : 2);
    if (provider == "custom") { seed.id = "custom"; seed.displayName = "Custom provider"; seed.secretProvider = "custom"; }
    const auto* existing = store->FindEndpoint(seed.id);
    const bool isNew = existing == nullptr;
    if (existing) seed = *existing;
    EndpointEditDialog dlg(parent, isNew ? "Add AI provider" : "Set up AI provider", seed,
        secrets, theme, isNew, store, wxString::FromUTF8(modelSearch));
    if (dlg.ShowModal() != wxID_OK) return false;
    if (modelToUse) *modelToUse = dlg.GetModelToUse();
    return true;
}

void EndpointsDialog::OnAdd(wxCommandEvent&)
{
    if (!m_store) return;
    auto seed = MakeProviderPreset(1);
    if (const auto* existing = m_store->FindEndpoint(seed.id)) seed = *existing;
    EndpointEditDialog dlg(this, "Add AI provider", seed, m_secretsStore, *m_theme, true, m_store);
    if (dlg.ShowModal() == wxID_OK && !dlg.GetModelToUse().empty()) {
        m_modelToUse = dlg.GetModelToUse();
        EndModal(wxID_OK);
        return;
    }
    // Cancelled, or a plain Save: stay here with the refreshed list.
    RebuildList(wxString::FromUTF8(seed.id)); UpdateButtonState();
}

void EndpointsDialog::OnEdit(wxCommandEvent&)
{
    if (!m_store) return;
    const long sel = m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (sel < 0 || sel >= (long)m_rowIds.size()) return;
    const auto* existing = m_store->FindEndpoint(m_rowIds[sel]);
    if (!existing) return;
    const auto seed = *existing;
    EndpointEditDialog dlg(this, "Edit AI provider", seed, m_secretsStore, *m_theme, false, m_store);
    if (dlg.ShowModal() != wxID_OK) return;
    m_modelToUse = dlg.GetModelToUse();
    if (m_modelToUse.empty()) {
        // Plain Save: the list changed but the chat's model did not.
        RebuildList(wxString::FromUTF8(seed.id)); UpdateButtonState();
        return;
    }
    EndModal(wxID_OK);
}

void EndpointsDialog::OnDelete(wxCommandEvent&)
{
    if (!m_store) return;

    long sel = m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (sel < 0 || sel >= (long)m_rowIds.size()) return;

    const std::string id = m_rowIds[sel];
    int ans = wxMessageBox(
        wxString::Format("Delete endpoint '%s'?",
                         wxString::FromUTF8(id.c_str())),
        "Confirm", wxYES_NO | wxICON_WARNING, this);
    if (ans != wxYES) return;

    const auto* existing = m_store->FindEndpoint(id);
    if (!existing) return;
    const auto previous = *existing;
    m_store->RemoveEndpoint(id);
    if (!m_store->Save()) {
        m_store->UpsertEndpoint(previous);
        wxMessageBox("Could not save the deletion. The connection was restored.", "Save failed", wxOK | wxICON_ERROR, this);
    }
    RebuildList();
    UpdateButtonState();
}

void EndpointsDialog::OnClose(wxCommandEvent&)
{
    EndModal(wxID_CLOSE);
}
