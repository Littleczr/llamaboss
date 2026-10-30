// endpoints_dialog.cpp

#include "endpoints_dialog.h"
#include "lb_ascii.h"
#include "reasoning_policy.h"
#include "openai_responses.h"
#include "endpoint_store.h"
#include "secrets_store.h"
#include "theme.h"
#include "widgets.h"   // ApplyDialogThemeRecursive, ApplyDarkTitleBar
#include "connections_dialog.h"
#include "chatgpt_auth.h"        // Sign in with ChatGPT (plan usage)
#include "chatgpt_auth_core.h"
#include "lb_ssl.h"
#include <wx/simplebook.h>
#include <wx/checkbox.h>
#include "lb_windows.h"
#include "ui_event_post.h"   // LbQueueEventIfAlive (fetch-models worker)
#include "background_work.h"   // lb::BeginBackgroundWork (drained in MyApp::OnExit)

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
#include <wx/timer.h>
#include <wx/clipbrd.h>

#include <Poco/Net/HTTPMessage.h>

#include <stdexcept>
#include <cstdio>
#include <map>
#include <set>

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
    else if (selection == 3) {
        // ChatGPT plan: no API key.  The transport is fixed by the Sign
        // in with ChatGPT contract; the account is chosen by signing in.
        ep.id             = "chatgpt";
        ep.displayName    = "ChatGPT plan";
        ep.baseUrl        = lb_chatgpt::kApiBaseUrl();
        ep.chatPath       = lb_chatgpt::kResponsesPath();
        ep.authScheme     = EndpointStore::AuthScheme::ChatGpt;
        ep.secretProvider = "chatgpt";
        ep.secretKey      = "account";
    }
    else if (selection == 4) {
        // Local server: an OpenAI-compatible server on this PC (Strata,
        // LM Studio, Ollama, a second llama-server ...).  No key; the
        // address normally comes from the port scan.  OpenAI-style
        // reasoning_effort is what Strata and llama-server accept.
        ep.id               = "local_server";
        ep.displayName      = "Local server";
        ep.baseUrl          = "http://127.0.0.1:8080";
        ep.authScheme       = EndpointStore::AuthScheme::None;
        ep.secretProvider   = "local";
        ep.reasoningDialect = "openai";
    }
    // selection 2 is Custom: retain the safe struct defaults and let the
    // user fill only the values that genuinely vary by provider.
    return ep;
}

// Preset indices (stable: endpoints.json never stores them, but the
// helpers above key on them).  ChatGPT shares api.openai.com with the
// OpenAI API-key preset, so the auth scheme decides first.
constexpr int kPresetOpenAI     = 0;
constexpr int kPresetOpenRouter = 1;
constexpr int kPresetCustom     = 2;
constexpr int kPresetChatGpt    = 3;
constexpr int kPresetLocal      = 4;

// Provider row order as shown in the dialog.
constexpr int kPresetDisplayOrder[] = { kPresetLocal, kPresetOpenRouter, kPresetOpenAI,
                                        kPresetChatGpt, kPresetCustom };
const char* const kPresetDisplayNames[] = { "Local server", "OpenRouter", "OpenAI",
                                            "ChatGPT plan", "Custom" };

// "131072" -> "128K", "1000000" -> "1M", "200000" -> "200K".
std::string FormatContextTokens(int n)
{
    if (n <= 0) return {};
    if (n % 1048576 == 0) return std::to_string(n / 1048576) + "M";
    if (n % 1000000 == 0) return std::to_string(n / 1000000) + "M";
    if (n % 1024 == 0)    return std::to_string(n / 1024) + "K";
    if (n % 1000 == 0)    return std::to_string(n / 1000) + "K";
    return std::to_string(n);
}

// Accepts "131072", "131,072", "128K", "128k", "1M".  Empty -> 0 (use
// the default).  Returns false for anything else or out of range.
bool ParseContextTokens(const std::string& raw, int& out)
{
    std::string s;
    for (char c : raw) if (c != ',' && c != ' ' && c != '_') s += c;
    out = 0;
    if (s.empty()) return true;
    long long mult = 1;
    const char last = (char)lb_ascii::ToLower((unsigned char)s.back());
    if (last == 'k') { mult = 1024; s.pop_back(); }
    else if (last == 'm') { mult = 1048576; s.pop_back(); }
    if (s.empty() || s.size() > 9) return false;
    for (char c : s) if (c < '0' || c > '9') return false;
    const long long v = std::stoll(s) * mult;
    if (v < 4096 || v > 2097152) return false;
    out = (int)v;
    return true;
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
                   [](unsigned char c) { return (char)lb_ascii::ToLower(c); });
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
                       [](unsigned char c) { return (char)lb_ascii::ToLower(c); });
        if (!out.empty()) out += ' ';
        if (lower == "gpt" || lower == "ai" || lower == "vl" ||
            lower == "xl") {
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return (char)lb_ascii::ToUpper(c); });
            out += lower;
        } else {
            lower[0] = (char)lb_ascii::ToUpper((unsigned char)lower[0]);
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
                               [](unsigned char c) { return (char)lb_ascii::ToLower(c); });
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
        else {
            // The raw text format has no context field: keep the record's.
            const int keepCtx = existing->contextLimit;
            *existing = model;
            if (existing->contextLimit <= 0) existing->contextLimit = keepCtx;
        }
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
// Designed around the two personas that actually use it:
//
//   * Provider preset (OpenAI / OpenRouter) — the overwhelmingly
//     common case.  Every transport constant is known for these, so
//     the visible dialog is just: Provider, Saved connection, and a
//     fetched model checklist.  The Provider combo is the preset
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
// draft on save or return to the checklist. A 401 on the fetch doubles as
// the auth test, so a bad key shows up before the first chat message.

struct FetchModelsResult {
    bool        ok = false;
    int         status = 0;             // HTTP status (0 = transport error)
    std::string error;
    std::vector<std::string> ids;       // sorted, deduplicated
    // Display names parallel to `ids` when the catalog provides them
    // (the ChatGPT plan catalog does); empty otherwise.
    std::vector<std::string> names;
    bool credentialsVerified = false;
    // Context window per id when the catalog reports one (OpenRouter's
    // context_length, vLLM's max_model_len, llama-server's meta.n_ctx).
    std::map<std::string, int> contexts;
    std::string ownedBy;                // first entry's owned_by, if any
};

// Context window advertised for one catalog entry, 0 when absent.
// n_ctx_train is deliberately ignored: it is the training length, not
// what the server was started with.
int ContextFromModelEntry(const Poco::JSON::Object::Ptr& o)
{
    if (!o) return 0;
    auto num = [](const Poco::JSON::Object::Ptr& obj, const char* key) -> long long {
        if (!obj || !obj->has(key) || obj->isNull(key)) return 0;
        try { return obj->get(key).convert<Poco::Int64>(); } catch (...) { return 0; }
    };
    for (const char* key : { "max_model_len", "context_length", "context_window",
                             "max_context_length", "n_ctx" }) {
        const long long v = num(o, key);
        if (v > 0 && v < (1LL << 31)) return (int)v;
    }
    if (const auto meta = o->getObject("meta")) {
        const long long v = num(meta, "n_ctx");
        if (v > 0 && v < (1LL << 31)) return (int)v;
    }
    return 0;
}

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
    const std::vector<std::pair<std::string, std::string>>& extraHeaders, bool inspectKey = false,
    int timeoutSeconds = 15)
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
        sess->setTimeout(Poco::Timespan(timeoutSeconds, 0));

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
        // ChatGPT plan catalog shape: {"models":[{slug,display_name,visibility}]}.
        // Keep the server's order and its display names.
        if (Poco::JSON::Array::Ptr models = rootObj->getArray("models")) {
            std::set<std::string> seen;
            bool anyVisibility = false;
            for (size_t i = 0; i < models->size(); ++i) {
                auto o = models->getObject(i);
                if (o && o->has("visibility")) anyVisibility = true;
            }
            for (size_t i = 0; i < models->size(); ++i) {
                auto o = models->getObject(i);
                if (!o) continue;
                try {
                    if (anyVisibility && o->optValue<std::string>("visibility", "") != "list") continue;
                    std::string slug = o->optValue<std::string>("slug", "");
                    if (slug.empty()) slug = o->optValue<std::string>("id", "");
                    if (slug.empty() || !seen.insert(slug).second) continue;
                    std::string name = o->optValue<std::string>("display_name", "");
                    out.ids.push_back(slug);
                    out.names.push_back(name.empty() ? PrettyNameFromId(slug) : name);
                } catch (...) { /* non-string fields: skip the entry */ }
            }
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
                if (id.empty()) continue;
                if (const int ctx = ContextFromModelEntry(o)) out.contexts[id] = ctx;
                if (out.ownedBy.empty())
                    out.ownedBy = o->optValue<std::string>("owned_by", "");
                ids.insert(std::move(id));
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

// ChatGPT plan catalog -- worker thread only (GetAccessToken may refresh
// the sign-in over the network).  The plan route answers
//   {"models":[{"slug":...,"display_name":...,"visibility":"list"},...]}
// in the account's own order; only visibility "list" entries are meant
// for a picker.  A plain {"data":[{"id":...}]} body is accepted too, in
// case the route ever returns the standard shape.
void RedactProbeError(std::string& message, const std::string& key);   // defined below

FetchModelsResult FetchChatGptCatalog(const std::string& account)
{
    FetchModelsResult out;
    std::string token, tokenError;
    if (!lb_chatgpt::Auth::Get().GetAccessToken(account, token, tokenError)) {
        out.status = 401;
        out.error = tokenError;
        return out;
    }
    lb::EnsureSSLInitialized();   // normally done at startup; cheap no-op then
    out = FetchModelIds(lb_chatgpt::kModelsUrl(), "Authorization", "Bearer " + token, {});
    RedactProbeError(out.error, token);
    if (out.status == 401) lb_chatgpt::Auth::Get().InvalidateAccessToken(account);
    out.credentialsVerified = out.ok;
    return out;
}
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

// Runtime context of a llama-server style host (GET /props), 0 when the
// server has no such route.  Worker thread only.
int FetchPropsContext(const std::string& base)
{
    try {
        Poco::URI uri(base + "/props");
        int port = uri.getPort();
        if (port == 0) port = 80;
        Poco::Net::HTTPClientSession sess(uri.getHost(), (Poco::UInt16)port);
        sess.setTimeout(Poco::Timespan(3, 0));
        Poco::Net::HTTPRequest req(Poco::Net::HTTPRequest::HTTP_GET, uri.getPathAndQuery(),
                                   Poco::Net::HTTPMessage::HTTP_1_1);
        req.set("Accept", "application/json");
        sess.sendRequest(req);
        Poco::Net::HTTPResponse resp;
        std::istream& in = sess.receiveResponse(resp);
        if (resp.getStatus() != Poco::Net::HTTPResponse::HTTP_OK) return 0;
        std::string body;
        char buf[4096];
        while (in.good() && body.size() < 512u * 1024) {
            in.read(buf, sizeof(buf));
            if (in.gcount() <= 0) break;
            body.append(buf, (size_t)in.gcount());
        }
        Poco::JSON::Parser parser;
        const auto root = parser.parse(body).extract<Poco::JSON::Object::Ptr>();
        if (!root) return 0;
        if (const int n = ContextFromModelEntry(root->getObject("default_generation_settings"))) return n;
        return ContextFromModelEntry(root);
    } catch (...) { return 0; }
}

// ── Local server discovery ───────────────────────────────────────
struct LocalServerInfo {
    int port = 0;
    std::string baseUrl;                 // http://127.0.0.1:<port>
    std::string name;                    // best guess, user can rename
    std::vector<std::string> ids;
    std::map<std::string, int> contexts;
};

wxDEFINE_EVENT(wxEVT_EP_LOCAL_SCAN_DONE, wxThreadEvent);

// Ports used by common OpenAI-compatible servers.  LlamaBoss's own
// llama-server (8384 by default) is deliberately absent: it is already
// the local lane in the model picker.
constexpr int kLocalScanPorts[] = { 8080, 8081, 1234, 1919, 8000, 11434 };
const char* const kLocalScanPortsText = "8080, 8081, 1234, 1919, 8000 and 11434";

std::string LocalServerName(int port, const std::string& ownedBy)
{
    std::string o = ownedBy;
    std::transform(o.begin(), o.end(), o.begin(),
                   [](unsigned char c) { return (char)lb_ascii::ToLower(c); });
    if (o == "llamacpp" || o == "llama.cpp") return "llama.cpp server";
    if (o == "vllm") return "vLLM";
    if (port == 1234)  return "LM Studio";
    if (port == 11434) return "Ollama";
    if (port == 1919)  return "FreeToken";
    static const std::set<std::string> generic = {
        "", "openai", "system", "user", "organization", "organization_owner",
        "owner", "library", "local", "me" };
    if (!generic.count(o)) return PrettyNameFromId(ownedBy);
    return "Local server";
}

// Worker thread only.  Closed ports on 127.0.0.1 refuse at once, so the
// whole sweep is normally well under a second.
LocalServerInfo ProbeLocalPort(int port)
{
    LocalServerInfo s;
    s.baseUrl = "http://127.0.0.1:" + std::to_string(port);
    FetchModelsResult r = FetchModelIds(s.baseUrl + "/v1/models", "", "", {}, false, 3);
    if (!r.ok || r.ids.empty()) return s;           // port stays 0: nothing there
    s.port = port;
    s.ids = r.ids;
    s.contexts = r.contexts;
    if (s.ids.size() == 1 && !s.contexts.count(s.ids.front())) {
        if (const int n = FetchPropsContext(s.baseUrl)) s.contexts[s.ids.front()] = n;
    }
    s.name = LocalServerName(port, r.ownedBy);
    return s;
}

// Worker thread only.  All ports at once: on Windows a closed localhost
// port can take a couple of seconds to refuse, so one after another
// would add up to ten seconds or more.
std::vector<LocalServerInfo> ScanLocalServers()
{
    constexpr size_t n = sizeof(kLocalScanPorts) / sizeof(kLocalScanPorts[0]);
    std::vector<LocalServerInfo> slots(n);
    std::vector<std::thread> probes;
    probes.reserve(n);
    for (size_t i = 0; i < n; ++i)
        probes.emplace_back([&slots, i]() { slots[i] = ProbeLocalPort(kLocalScanPorts[i]); });
    for (auto& t : probes) t.join();
    std::vector<LocalServerInfo> found;
    for (auto& s : slots) if (s.port != 0) found.push_back(std::move(s));
    return found;
}

// Same host and port, treating localhost and 127.0.0.1 as one.
bool SameLocalBase(std::string a, std::string b)
{
    auto norm = [](std::string v) {
        v = NormalizeBaseUrl(v);
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c) { return (char)lb_ascii::ToLower(c); });
        const std::string lh = "://localhost";
        const size_t at = v.find(lh);
        if (at != std::string::npos) v.replace(at, lh.size(), "://127.0.0.1");
        return v;
    };
    return norm(std::move(a)) == norm(std::move(b));
}

// "sk-or-v1-abcdef...a91f" -> "sk-or-••••••••a91f".  Never shows more
// than the leading 6 and trailing 4 characters, and nothing for short keys.
std::string MaskKey(const std::string& key)
{
    const std::string dots = "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2"
                             "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2";
    if (key.size() < 20) return dots;
    return key.substr(0, 6) + dots + key.substr(key.size() - 4);
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
// Sign in with ChatGPT: CompleteSignIn finished (payload SignInResult).
wxDEFINE_EVENT(wxEVT_EP_CHATGPT_SIGNED_IN, wxThreadEvent);

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

// ─── Add / Edit AI provider dialog ──────────────────────────────
//
// One window, no wizard.  A provider row across the top; the
// connection on the left, the models on the right, a fixed footer.
//
//   * Local server  - scans the usual ports on 127.0.0.1 and lists
//                     what answers; picking one loads its models.  No
//                     key fields at all.
//   * OpenRouter / OpenAI - a saved-key card ("Replace key"), or the
//                     key box when no key is saved yet.
//   * ChatGPT plan  - the browser sign-in panel.
//   * Custom        - Base URL + key, Advanced opens by itself.
//
// Before the first successful connect (Add mode) the accent button is
// "Connect"; afterwards it is "Save and use <model>".  Edit mode starts
// connected.  A failed connect never resets anything the user typed;
// for key-free servers that refuse the connection, an optional 5 s
// retry fills the window in once the server comes up.
//
// Fetch models: GET <base>/v1/models with the endpoint's own auth, on a
// detached worker thread.  Checking/unchecking changes membership on
// structured model records; the advanced text editor projects selected
// records and imports its final draft on save or return to the list.
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
                       const wxString& modelSearch = wxEmptyString,
                       int initialPreset = -1)
        : wxDialog(parent, wxID_ANY, title,
                   wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
        , m_isNew(isNew)
        , m_editMode(!isNew)
        , m_theme(&theme)
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

        m_preset = initialPreset >= 0 ? initialPreset
                 : seed.authScheme == EndpointStore::AuthScheme::ChatGpt
                     ? kPresetChatGpt : DetectPreset(seed.baseUrl, seed.authScheme);

        auto* outer = new wxBoxSizer(wxVERTICAL);

        // ── Provider row ─────────────────────────────────────────
        auto* header = new wxBoxSizer(wxHORIZONTAL);
        header->Add(SectionLabel(this, "PROVIDER"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(14));
        m_presetTrack = new wxPanel(this, wxID_ANY);
        auto* track = new wxBoxSizer(wxHORIZONTAL);
        for (size_t i = 0; i < sizeof(kPresetDisplayOrder) / sizeof(kPresetDisplayOrder[0]); ++i) {
            auto* b = new wxButton(m_presetTrack, wxID_ANY, kPresetDisplayNames[i],
                                   wxDefaultPosition, wxSize(-1, FromDIP(30)), wxBORDER_NONE);
            wxFont bf = b->GetFont(); bf.SetPointSize(10); bf.SetWeight(wxFONTWEIGHT_SEMIBOLD); b->SetFont(bf);
            // Room for the bold (selected) label plus side padding.
            b->SetMinSize(wxSize(b->GetTextExtent(kPresetDisplayNames[i]).x + FromDIP(28), FromDIP(30)));
            const int preset = kPresetDisplayOrder[i];
            b->Bind(wxEVT_BUTTON, [this, preset](wxCommandEvent&) { OnPresetClicked(preset); });
            track->Add(b, 0, wxALL, FromDIP(3));
            m_presetBtns.push_back(b);
        }
        m_presetTrack->SetSizer(track);
        header->Add(m_presetTrack, 0, wxALIGN_CENTER_VERTICAL);
        outer->Add(header, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));
        outer->Add(MakeHairline(this, theme), 0, wxEXPAND | wxTOP, FromDIP(12));

        // ── Body: connection | models ────────────────────────────
        auto* body = new wxBoxSizer(wxHORIZONTAL);
        BuildLeftPane(seed, theme);
        body->Add(m_leftPane, 0, wxEXPAND);
        auto* divider = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(1, -1));
        divider->SetBackgroundColour(theme.borderSubtle);
        body->Add(divider, 0, wxEXPAND);
        BuildRightPane(seed, theme, modelSearch);
        body->Add(m_rightPane, 1, wxEXPAND);
        outer->Add(body, 1, wxEXPAND);

        // ── Footer ───────────────────────────────────────────────
        outer->Add(MakeHairline(this, theme), 0, wxEXPAND);
        auto* footer = new wxBoxSizer(wxHORIZONTAL);
        // One line, cut with "..." when it doesn't fit; Details shows all of it.
        m_fetchStatus = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                         wxDefaultSize, wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
        m_fetchStatus->SetMinSize(FromDIP(wxSize(100, -1)));
        footer->Add(m_fetchStatus, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        m_statusDetails = MakeFlatButton(this, wxID_ANY, "Details...", theme);
        footer->Add(m_statusDetails, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        m_statusDetails->Hide();
        m_statusDetails->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            wxMessageBox(wxString::FromUTF8(m_statusText), "Connection details", wxOK | wxICON_INFORMATION, this);
        });
        m_cancelBtn = MakeFlatButton(this, wxID_CANCEL, "Cancel", theme);
        // "Save" keeps the checked list (and any new key) without touching
        // the current chat's model.  The accent button is "Connect" until
        // a connection is checked, then "Save and use <model>".
        m_saveBtn = MakeFlatButton(this, wxID_ANY, "Save", theme);
        m_saveBtn->SetToolTip("Save the checked models and connection without changing the current chat's model");
        m_finishBtn = MakeAccentButton(this, wxID_OK, "Connect", theme);
        footer->Add(m_cancelBtn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        footer->Add(m_saveBtn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        footer->Add(m_finishBtn, 0, wxALIGN_CENTER_VERTICAL);
        outer->Add(footer, 0, wxEXPAND | wxALL, FromDIP(12));

        // ── Wiring ──────────────────────────────────────────────
        m_saveBtn->Bind(wxEVT_BUTTON, &EndpointEditDialog::OnSaveOnly, this);
        m_finishBtn->Bind(wxEVT_BUTTON, &EndpointEditDialog::OnOK, this);
        m_modelList->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) { LoadModelOptions(); });
        m_modelList->Bind(wxEVT_CHECKLISTBOX, &EndpointEditDialog::OnModelToggled, this);
        // Clicking a row's name adds it to the picker.  The checkbox
        // itself still toggles both ways (wx handles that click before
        // the deferred check below runs, and the check sees the result).
        m_modelList->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
            e.Skip();
            if (m_rawMode) return;
            const int row = m_modelList->HitTest(e.GetPosition());
            if (row < 0 || row >= (int)m_rowModelIds.size()) return;
            if (m_modelList->IsChecked((unsigned)row)) return;
            const std::string id = m_rowModelIds[(size_t)row];
            CallAfter([this, id]() { if (!IsInPicker(id)) SetInPicker(id, true); });
        });
        m_modelNameField->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { SaveModelOptions(); });
        m_ctxField->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { SaveModelOptions(); });
        m_allowTools->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { SaveModelOptions(); });
        m_imageOutput->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { SaveModelOptions(); });
        m_testEffort->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { InvalidateTest(); });

        m_nameField->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { SyncIdFromName(); });
        m_idField->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
            // Only USER edits reach here; programmatic updates use
            // ChangeValue.  From now on the id is theirs.
            m_idEditedByUser = true;
        });
        m_baseUrlField->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
            // The catalog is keyed on the base URL: editing it orphans
            // any in-flight fetch and any scanned-server selection.
            for (auto* rb : m_scanRadios) rb->SetValue(false);
            InvalidateFetch();
            UpdateResolvedUrl();
        });
        m_chatPathField->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { InvalidateTest(); UpdateResolvedUrl(); });
        m_providerField->Bind(wxEVT_COMBOBOX, &EndpointEditDialog::OnProviderChanged, this);
        m_providerField->Bind(wxEVT_TEXT, &EndpointEditDialog::OnProviderChanged, this);
        m_keyField->Bind(wxEVT_COMBOBOX, &EndpointEditDialog::OnKeyChanged, this);
        m_keyField->Bind(wxEVT_TEXT, &EndpointEditDialog::OnKeyChanged, this);
        m_apiKeyField->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
            InvalidateFetch();
            UpdateKeyCard();
        });
        {
            // Switching the auth scheme changes the catalog's credential
            // identity (the fetch doubles as the auth test) and whether
            // the key fields matter at all.
            auto onAuthScheme = [this](wxCommandEvent&) {
                InvalidateFetch();
                UpdateAuthFieldEnables();
                ApplyProviderMode();
            };
            m_bearerRadio ->Bind(wxEVT_RADIOBUTTON, onAuthScheme);
            m_xApiKeyRadio->Bind(wxEVT_RADIOBUTTON, onAuthScheme);
            m_noAuthRadio ->Bind(wxEVT_RADIOBUTTON, onAuthScheme);
        }
        m_dialectChoice->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
            m_originalDialect.clear(); InvalidateTest(); UpdateAdvancedSummary();
        });
        m_nativeRadio->Bind(wxEVT_RADIOBUTTON, [this](wxCommandEvent&) { InvalidateTest(); UpdateAdvancedSummary(); });
        m_xmlRadio->Bind(wxEVT_RADIOBUTTON, [this](wxCommandEvent&) { InvalidateTest(); UpdateAdvancedSummary(); });
        m_chatgptSignInBtn->Bind(wxEVT_BUTTON, &EndpointEditDialog::OnChatGptSignIn, this);
        m_chatgptSignOutBtn->Bind(wxEVT_BUTTON, &EndpointEditDialog::OnChatGptSignOut, this);
        Bind(wxEVT_EP_CHATGPT_SIGNED_IN, &EndpointEditDialog::OnChatGptSignedIn, this);
        m_fetchBtn->Bind(wxEVT_BUTTON, &EndpointEditDialog::OnFetchModels, this);
        Bind(wxEVT_EP_MODELS_FETCHED, &EndpointEditDialog::OnModelsFetched, this);
        Bind(wxEVT_EP_LOCAL_SCAN_DONE, &EndpointEditDialog::OnLocalScanDone, this);
        m_testBtn->Bind(wxEVT_BUTTON, &EndpointEditDialog::OnTestConnection, this);
        Bind(wxEVT_EP_CONNECTION_TESTED, &EndpointEditDialog::OnConnectionTested, this);
        m_filterField->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { RefreshModelChecklist(); });
        m_modelsField->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
            if (!m_syncingModels) {
                // Import only the completed draft. Importing every
                // keystroke would retain partially typed IDs as hidden models.
                m_rawModelsDirty = true;
                InvalidateTest();
            }
        });
        m_rawToggleBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ToggleRawEditor(); });
        m_advancedPane->Bind(wxEVT_COLLAPSIBLEPANE_CHANGED,
            [this](wxCollapsiblePaneEvent&) { RefreshLayout(); });
        m_scanBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { if (!m_scanInFlight) StartLocalScan(); });
        m_rescanTimer.SetOwner(this);
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
            if (!m_scanInFlight && !m_connected && !m_fetchInFlight) StartLocalScan(/*quiet*/ true);
        }, m_rescanTimer.GetId());
        m_retryTimer.SetOwner(this);
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
            if (!m_fetchInFlight && m_keepChecking->GetValue()) {
                wxCommandEvent e;
                OnFetchModels(e);
            }
        }, m_retryTimer.GetId());
        m_keepChecking->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent& e) {
            if (!e.IsChecked()) m_retryTimer.Stop();
            else if (m_retryPanel->IsShown() && !m_fetchInFlight) m_retryTimer.StartOnce(5000);
        });

        // ── Initial state ───────────────────────────────────────
        m_chatgptAccount = seed.chatgptAccount;
        UpdatePresetButtons();
        UpdateAuthFieldEnables();
        ApplyProviderMode(/*relayout*/ false);
        UpdateResolvedUrl();
        UpdateAdvancedSummary();
        UpdateKeyCard();
        SetFetchStatus(m_editMode ? std::string() : std::string("Connect to check the connection and load models."));
        RefreshModelChecklist();
        if (m_preset == kPresetCustom && m_isNew) m_advancedPane->Expand();

        SetSizer(outer);

        int winW = FromDIP(940), winH = FromDIP(680);
        const int displayIndex = wxDisplay::GetFromWindow(
            GetParent() ? GetParent() : static_cast<wxWindow*>(this));
        if (displayIndex != wxNOT_FOUND) {
            const wxRect workArea =
                wxDisplay(static_cast<unsigned>(displayIndex)).GetClientArea();
            winW = std::min(winW, workArea.GetWidth()  * 9 / 10);
            winH = std::min(winH, workArea.GetHeight() * 9 / 10);
        }
        SetSize(wxSize(winW, winH));
        SetMinSize(wxSize(std::min(winW, FromDIP(760)), std::min(winH, FromDIP(520))));
        Layout();
        AlignAdvancedLabels();
        m_finishBtn->SetDefault();

        // ── Theming ─────────────────────────────────────────────
        ApplyDialogThemeRecursive(this, theme.textPrimary, theme.bgInputField, theme.textPrimary);
        ApplyThemeColors();
        ApplyDarkTitleBar(this, theme.name != "light");

        if (m_editMode) {
            // A provider that already works: refresh its catalog, which
            // doubles as the credential check.
            if (IsChatGpt()) {
                if (ChatGptSignedIn()) {
                    SetModelStatus("Loading your ChatGPT models...", StatusTone::Neutral);
                    CallAfter([this]() { if (!m_fetchInFlight) { wxCommandEvent e; OnFetchModels(e); } });
                } else {
                    SetModelStatus(kCross + "Not signed in to ChatGPT. Sign in on the left.", StatusTone::Bad);
                }
            } else if (UsingNoAuth() || !EffectiveApiKey().empty()) {
                SetModelStatus("Loading the model catalog...", StatusTone::Neutral);
                CallAfter([this]() { if (!m_fetchInFlight) { wxCommandEvent e; OnFetchModels(e); } });
            } else {
                SetModelStatus(kCross + "No saved key for this connection. Paste one on the left.", StatusTone::Bad);
            }
        } else if (m_preset == kPresetLocal) {
            SetModelStatus("Models show up here once a server answers.", StatusTone::Neutral);
            CallAfter([this]() { StartLocalScan(); });
        } else {
            SetModelStatus(IsChatGpt() ? "Sign in to load your ChatGPT models."
                                       : "Connect to load the model catalog.", StatusTone::Neutral);
            if (!IsChatGpt() && m_apiKeyField->IsShown()) m_apiKeyField->SetFocus();
        }
        UpdateSaveButtons();
    }

    ~EndpointEditDialog() override
    {
        m_retryTimer.Stop();
        m_rescanTimer.Stop();
        // Gate for the detached workers.  MUST go through
        // LbMarkUiEventTargetDead — it shares a mutex with
        // LbQueueEventIfAlive, closing the window where the worker has
        // passed the alive check but not yet queued the event to this
        // (about-to-be-destroyed) handler.
        LbMarkUiEventTargetDead(m_alive);
        // A browser sign-in still waiting: stop its loopback listener.
        if (m_signInAttempt) lb_chatgpt::Auth::CancelSignIn(m_signInAttempt);
    }

private:
    enum class StatusTone { Neutral, Good, Bad };

    // ── Layout builders ─────────────────────────────────────────
    wxStaticText* SectionLabel(wxWindow* parent, const wxString& text)
    {
        auto* l = new wxStaticText(parent, wxID_ANY, text);
        wxFont f = l->GetFont(); f.SetPointSize(9); f.SetWeight(wxFONTWEIGHT_SEMIBOLD); l->SetFont(f);
        m_sectionLabels.push_back(l);
        return l;
    }

    wxStaticText* Muted(wxWindow* parent, const wxString& text, int wrap = 0)
    {
        auto* l = new wxStaticText(parent, wxID_ANY, text);
        wxFont f = l->GetFont(); f.SetPointSize(9); l->SetFont(f);
        if (wrap > 0) l->Wrap(wrap);
        m_hints.push_back(l);
        return l;
    }

    void BuildLeftPane(const EndpointStore::Endpoint& seed, const ThemeData& theme)
    {
        auto* left = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(350), -1));
        left->SetScrollRate(0, FromDIP(12));
        left->SetMinSize(wxSize(FromDIP(350), -1));
        m_leftPane = left;
        const int wrap = FromDIP(310);
        auto* L = new wxBoxSizer(wxVERTICAL);

        // Local server: what answered on this PC.
        m_scanGroup = new wxPanel(left, wxID_ANY);
        {
            auto* g = new wxBoxSizer(wxVERTICAL);
            auto* head = new wxBoxSizer(wxHORIZONTAL);
            head->Add(SectionLabel(m_scanGroup, "SERVERS FOUND ON THIS PC"), 1, wxALIGN_CENTER_VERTICAL);
            m_scanBtn = MakeFlatButton(m_scanGroup, wxID_ANY, "Scan again", theme, FromDIP(26));
            head->Add(m_scanBtn, 0, wxALIGN_CENTER_VERTICAL);
            g->Add(head, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
            m_scanList = new wxPanel(m_scanGroup, wxID_ANY);
            m_scanList->SetSizer(new wxBoxSizer(wxVERTICAL));
            g->Add(m_scanList, 0, wxEXPAND);
            m_scanHint = Muted(m_scanGroup, wxString::Format(
                "Checked ports %s. LlamaBoss's own local server is left out.", kLocalScanPortsText), wrap);
            g->Add(m_scanHint, 0, wxEXPAND | wxTOP, FromDIP(6));
            m_scanGroup->SetSizer(g);
        }
        L->Add(m_scanGroup, 0, wxEXPAND | wxBOTTOM, FromDIP(14));

        // Address (Local) / Base URL (Custom).
        m_baseUrlLabel = SectionLabel(left, "BASE URL");
        L->Add(m_baseUrlLabel, 0, wxBOTTOM, FromDIP(4));
        m_baseUrlField = new wxTextCtrl(left, wxID_ANY, wxString::FromUTF8(seed.baseUrl));
        m_baseUrlField->SetHint("https://api.example.com");
        L->Add(m_baseUrlField, 0, wxEXPAND | wxBOTTOM, FromDIP(10));

        // Nothing answered: what to do, and an optional auto-retry.
        m_retryPanel = new wxPanel(left, wxID_ANY);
        {
            auto* r = new wxBoxSizer(wxVERTICAL);
            m_retryTitle = new wxStaticText(m_retryPanel, wxID_ANY, wxEmptyString);
            wxFont tf = m_retryTitle->GetFont(); tf.SetWeight(wxFONTWEIGHT_SEMIBOLD); m_retryTitle->SetFont(tf);
            r->Add(m_retryTitle, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
            m_retryBody = new wxStaticText(m_retryPanel, wxID_ANY,
                "The server isn't running, or it stopped when its window closed. "
                "Start it, wait for its ready line, then retry.");
            m_retryBody->Wrap(wrap - FromDIP(20));
            r->Add(m_retryBody, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
            m_keepChecking = new wxCheckBox(m_retryPanel, wxID_ANY, "Keep checking every 5 seconds");
            m_keepChecking->SetValue(true);
            r->Add(m_keepChecking, 0);
            auto* pad = new wxBoxSizer(wxVERTICAL);
            pad->Add(r, 1, wxEXPAND | wxALL, FromDIP(10));
            m_retryPanel->SetSizer(pad);
        }
        L->Add(m_retryPanel, 0, wxEXPAND | wxBOTTOM, FromDIP(12));
        m_retryPanel->Hide();

        // API key: saved-key card, or the key box.
        m_keyGroup = new wxPanel(left, wxID_ANY);
        {
            auto* k = new wxBoxSizer(wxVERTICAL);
            k->Add(SectionLabel(m_keyGroup, "API KEY"), 0, wxBOTTOM, FromDIP(4));
            m_keyCard = new wxPanel(m_keyGroup, wxID_ANY);
            {
                auto* c = new wxBoxSizer(wxVERTICAL);
                m_keyCardTitle = new wxStaticText(m_keyCard, wxID_ANY, wxEmptyString);
                c->Add(m_keyCardTitle, 0, wxBOTTOM, FromDIP(2));
                m_keyCardMasked = new wxStaticText(m_keyCard, wxID_ANY, wxEmptyString);
                m_keyCardMasked->SetFont(wxFont(wxFontInfo(10).Family(wxFONTFAMILY_TELETYPE)));
                c->Add(m_keyCardMasked, 0, wxBOTTOM, FromDIP(2));
                m_keyCardDetail = Muted(m_keyCard, wxEmptyString);
                c->Add(m_keyCardDetail, 0, wxBOTTOM, FromDIP(6));
                m_replaceKeyBtn = MakeFlatButton(m_keyCard, wxID_ANY, "Replace key", theme, FromDIP(26));
                c->Add(m_replaceKeyBtn, 0);
                auto* pad = new wxBoxSizer(wxVERTICAL);
                pad->Add(c, 1, wxEXPAND | wxALL, FromDIP(10));
                m_keyCard->SetSizer(pad);
            }
            k->Add(m_keyCard, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
            m_keyEntry = new wxPanel(m_keyGroup, wxID_ANY);
            {
                auto* e = new wxBoxSizer(wxVERTICAL);
                m_apiKeyField = new wxTextCtrl(m_keyEntry, wxID_ANY, wxEmptyString,
                    wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
                m_apiKeyField->SetHint("Paste your API key");
                e->Add(m_apiKeyField, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
                auto* row = new wxBoxSizer(wxHORIZONTAL);
                m_showKeyBox = new wxCheckBox(m_keyEntry, wxID_ANY, "Show key");
                row->Add(m_showKeyBox, 0, wxALIGN_CENTER_VERTICAL);
                row->AddStretchSpacer();
                m_getKeyBtn = MakeFlatButton(m_keyEntry, wxID_ANY, "Get a key", theme, FromDIP(26));
                row->Add(m_getKeyBtn, 0, wxALIGN_CENTER_VERTICAL);
                e->Add(row, 0, wxEXPAND);
                m_keyEntry->SetSizer(e);
            }
            k->Add(m_keyEntry, 0, wxEXPAND);
            k->Add(Muted(m_keyGroup, "Keys are typed here only, never in chat.", wrap), 0, wxTOP, FromDIP(6));
            m_keyGroup->SetSizer(k);
        }
        L->Add(m_keyGroup, 0, wxEXPAND | wxBOTTOM, FromDIP(14));
        m_replaceKeyBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            m_replacingKey = true;
            UpdateKeyCard();
            m_apiKeyField->SetFocus();
        });
        m_getKeyBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            if (m_preset == kPresetOpenAI) wxLaunchDefaultBrowser("https://platform.openai.com/api-keys");
            else if (m_preset == kPresetOpenRouter) wxLaunchDefaultBrowser("https://openrouter.ai/settings/keys");
            else SetFetchStatus("Get an API key from your provider's website, then paste it here.");
        });
        m_showKeyBox->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent& event) {
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

        // ChatGPT plan sign-in.
        m_chatgptPanel = new wxPanel(left, wxID_ANY);
        {
            auto* cg = new wxBoxSizer(wxVERTICAL);
            cg->Add(SectionLabel(m_chatgptPanel, "CHATGPT PLAN"), 0, wxBOTTOM, FromDIP(4));
            auto* intro = Muted(m_chatgptPanel,
                "Use your ChatGPT Plus or Pro plan instead of an API key. Requests count toward "
                "your plan's usage (the same allowance as Codex); you can cap LlamaBoss under "
                "ChatGPT Settings > Usage.", wrap);
            cg->Add(intro, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            m_chatgptSignInBtn = MakeAccentButton(m_chatgptPanel, wxID_ANY, "Continue with ChatGPT", theme, FromDIP(32));
            m_chatgptSignInBtn->SetToolTip("Opens ChatGPT in your browser to sign in and allow LlamaBoss to use your plan.");
            m_chatgptSignOutBtn = MakeFlatButton(m_chatgptPanel, wxID_ANY, "Sign out", theme);
            m_chatgptSignOutBtn->SetToolTip("Forget this ChatGPT sign-in on this computer and revoke it.");
            row->Add(m_chatgptSignInBtn, 0, wxRIGHT, FromDIP(8));
            row->Add(m_chatgptSignOutBtn, 0);
            cg->Add(row, 0);
            m_chatgptStatus = new wxStaticText(m_chatgptPanel, wxID_ANY, wxEmptyString);
            cg->Add(m_chatgptStatus, 0, wxEXPAND | wxTOP, FromDIP(8));
            m_chatgptPanel->SetSizer(cg);
        }
        L->Add(m_chatgptPanel, 0, wxEXPAND | wxBOTTOM, FromDIP(14));

        // Name shown in the picker.
        L->Add(SectionLabel(left, "NAME IN THE PICKER"), 0, wxBOTTOM, FromDIP(4));
        m_nameField = new wxTextCtrl(left, wxID_ANY, wxString::FromUTF8(seed.displayName));
        L->Add(m_nameField, 0, wxEXPAND | wxBOTTOM, FromDIP(14));

        // Advanced.
        m_advancedPane = new wxCollapsiblePane(left, wxID_ANY, "Advanced",
            wxDefaultPosition, wxDefaultSize, wxCP_DEFAULT_STYLE | wxCP_NO_TLW_RESIZE);
        wxWindow* adv = m_advancedPane->GetPane();
        auto* advSizer = new wxBoxSizer(wxVERTICAL);
        auto* grid = new wxFlexGridSizer(2, FromDIP(8), FromDIP(8));
        grid->AddGrowableCol(1);
        m_advGrid = grid;
        auto fieldLabel = [&](const wxString& text) {
            // Explicit width: inside a collapsed pane the native best size
            // can come back short and clip the label.
            auto* l = new wxStaticText(adv, wxID_ANY, text);
            l->SetMinSize(wxSize(l->GetTextExtent("Reasoning").x + FromDIP(12), -1));
            grid->Add(l, 0, wxALIGN_CENTER_VERTICAL);
        };
        fieldLabel("Id");
        m_idField = new wxTextCtrl(adv, wxID_ANY, wxString::FromUTF8(seed.id));
        m_idField->Enable(m_isNew);
        grid->Add(m_idField, 1, wxEXPAND);
        m_originalDialect = seed.reasoningDialect;
        fieldLabel("Chat path");
        m_chatPathField = new wxTextCtrl(adv, wxID_ANY, wxString::FromUTF8(seed.chatPath));
        grid->Add(m_chatPathField, 1, wxEXPAND);
        wxArrayString savedProviders, savedKeys;
        std::set<std::string> seen;
        for (const auto& connection : m_connections) {
            if (seen.insert(connection.provider).second)
                savedProviders.Add(wxString::FromUTF8(connection.provider));
            if (connection.provider == seed.secretProvider)
                savedKeys.Add(wxString::FromUTF8(connection.key));
        }
        fieldLabel("Key set");
        m_providerField = new wxComboBox(adv, wxID_ANY, wxString::FromUTF8(seed.secretProvider),
            wxDefaultPosition, wxDefaultSize, savedProviders, wxCB_DROPDOWN);
        grid->Add(m_providerField, 1, wxEXPAND);
        fieldLabel("Key name");
        m_keyField = new wxComboBox(adv, wxID_ANY, wxString::FromUTF8(seed.secretKey),
            wxDefaultPosition, wxDefaultSize, savedKeys, wxCB_DROPDOWN);
        grid->Add(m_keyField, 1, wxEXPAND);
        fieldLabel("Key type");
        auto* auth = new wxBoxSizer(wxVERTICAL);
        m_bearerRadio = new wxRadioButton(adv, wxID_ANY, "Bearer token",
            wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
        m_xApiKeyRadio = new wxRadioButton(adv, wxID_ANY, "x-api-key header");
        m_noAuthRadio = new wxRadioButton(adv, wxID_ANY, "No key (local server)");
        m_bearerRadio->SetValue(seed.authScheme == EndpointStore::AuthScheme::Bearer);
        m_xApiKeyRadio->SetValue(seed.authScheme == EndpointStore::AuthScheme::XApiKey);
        m_noAuthRadio->SetValue(seed.authScheme == EndpointStore::AuthScheme::None);
        for (auto* control : {m_bearerRadio, m_xApiKeyRadio, m_noAuthRadio}) auth->Add(control, 0, wxBOTTOM, FromDIP(4));
        grid->Add(auth, 0, wxEXPAND);
        fieldLabel("Tool calls");
        auto* protocol = new wxBoxSizer(wxVERTICAL);
        m_nativeRadio = new wxRadioButton(adv, wxID_ANY, "Native function calls",
            wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
        m_xmlRadio = new wxRadioButton(adv, wxID_ANY, "XML fallback");
        m_nativeRadio->SetValue(seed.protocol != ToolProtocol::Xml);
        m_xmlRadio->SetValue(seed.protocol == ToolProtocol::Xml);
        protocol->Add(m_nativeRadio, 0, wxBOTTOM, FromDIP(4)); protocol->Add(m_xmlRadio);
        grid->Add(protocol, 0, wxEXPAND);
        fieldLabel("Reasoning");
        wxArrayString dialects;
        dialects.Add("Auto (detect from host)"); dialects.Add("OpenAI");
        dialects.Add("OpenRouter"); dialects.Add("Chat template");
        m_dialectChoice = new wxChoice(adv, wxID_ANY, wxDefaultPosition, wxDefaultSize, dialects);
        m_dialectChoice->SetSelection(DialectIndex(seed.reasoningDialect));
        grid->Add(m_dialectChoice, 0, wxEXPAND);
        advSizer->Add(grid, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(8));
        m_resolvedUrlLabel = Muted(adv, wxEmptyString);
        advSizer->Add(m_resolvedUrlLabel, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
        auto* manualModels = MakeFlatButton(adv, wxID_ANY, "Type model IDs without connecting...", theme);
        manualModels->SetToolTip("For services without a model catalog. The connection is not checked by this shortcut.");
        advSizer->Add(manualModels, 0, wxBOTTOM, FromDIP(8));
        manualModels->Bind(wxEVT_BUTTON, [this](wxCommandEvent& event) {
            m_manualModelsRequested = true;
            OnOK(event);
            m_manualModelsRequested = false;
        });
        m_manualModelsBtn = manualModels;
        adv->SetSizer(advSizer);
        L->Add(m_advancedPane, 0, wxEXPAND);

        auto* margins = new wxBoxSizer(wxVERTICAL);
        margins->Add(L, 1, wxEXPAND | wxALL, FromDIP(16));
        left->SetSizer(margins);
    }

    void BuildRightPane(const EndpointStore::Endpoint& seed, const ThemeData& theme, const wxString& modelSearch)
    {
        auto* right = new wxPanel(this, wxID_ANY);
        m_rightPane = right;
        auto* R = new wxBoxSizer(wxVERTICAL);

        m_modelConnectionStatus = new wxStaticText(right, wxID_ANY, wxEmptyString,
            wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
        m_modelConnectionStatus->SetMinSize(wxSize(FromDIP(40), -1));
        R->Add(m_modelConnectionStatus, 0, wxEXPAND | wxBOTTOM, FromDIP(8));

        auto* searchRow = new wxBoxSizer(wxHORIZONTAL);
        m_filterField = new wxTextCtrl(right, wxID_ANY, modelSearch);
        m_filterField->SetHint("Search models by name or id");
        searchRow->Add(m_filterField, 1, wxEXPAND | wxRIGHT, FromDIP(8));
        m_fetchBtn = MakeFlatButton(right, wxID_ANY, "Refresh models", theme);
        searchRow->Add(m_fetchBtn, 0);
        R->Add(searchRow, 0, wxEXPAND | wxBOTTOM, FromDIP(8));

        m_modelList = new wxCheckListBox(right, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(-1, 150)));
        R->Add(m_modelList, 1, wxEXPAND);
        m_modelsField = new wxTextCtrl(right, wxID_ANY, ModelsToText(seed.models),
            wxDefaultPosition, FromDIP(wxSize(-1, 150)), wxTE_MULTILINE | wxHSCROLL);
        m_modelsField->SetFont(wxFont(wxFontInfo(10).Family(wxFONTFAMILY_TELETYPE)));
        R->Add(m_modelsField, 1, wxEXPAND); m_modelsField->Hide();

        auto* listFooter = new wxBoxSizer(wxHORIZONTAL);
        m_modelCountLabel = Muted(right, wxEmptyString);
        listFooter->Add(m_modelCountLabel, 1, wxALIGN_CENTER_VERTICAL);
        m_rawToggleBtn = MakeFlatButton(right, wxID_ANY, "Edit as text", theme, FromDIP(26));
        listFooter->Add(m_rawToggleBtn, 0);
        R->Add(listFooter, 0, wxEXPAND | wxTOP, FromDIP(4));

        R->Add(MakeHairline(right, theme), 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(10));

        // Options for the highlighted model.
        m_selectedModelLabel = SectionLabel(right, "SELECTED MODEL");
        R->Add(m_selectedModelLabel, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
        auto* opts = new wxFlexGridSizer(2, FromDIP(6), FromDIP(16));
        opts->AddGrowableCol(0, 1);
        opts->Add(SectionLabel(right, "DISPLAY NAME"), 0);
        opts->Add(SectionLabel(right, "CONTEXT LIMIT"), 0);
        m_modelNameField = new wxTextCtrl(right, wxID_ANY);
        m_modelNameField->SetHint("Name in the model picker");
        opts->Add(m_modelNameField, 1, wxEXPAND);
        m_ctxField = new wxTextCtrl(right, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(130), -1));
        m_ctxField->SetHint("256K default");
        m_ctxField->SetToolTip("Tokens LlamaBoss budgets for this model: 131072 or 128K. "
                               "Leave empty for the 256K remote default.");
        opts->Add(m_ctxField, 0);
        R->Add(opts, 0, wxEXPAND);
        m_ctxHint = Muted(right, wxEmptyString);
        R->Add(m_ctxHint, 0, wxEXPAND | wxTOP, FromDIP(4));

        auto* can = new wxBoxSizer(wxHORIZONTAL);
        m_allowTools = new wxCheckBox(right, wxID_ANY, "Agent tools");
        m_imageOutput = new wxCheckBox(right, wxID_ANY, "Generate images");
        can->Add(m_allowTools, 0, wxRIGHT, FromDIP(18));
        can->Add(m_imageOutput, 0);
        R->Add(can, 0, wxTOP, FromDIP(8));
        m_compatibilityHint = Muted(right, wxEmptyString);
        R->Add(m_compatibilityHint, 0, wxEXPAND | wxTOP, FromDIP(6));

        auto* testRow = new wxBoxSizer(wxHORIZONTAL);
        m_testBtn = MakeFlatButton(right, wxID_ANY, "Test model", theme);
        testRow->Add(m_testBtn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        testRow->Add(new wxStaticText(right, wxID_ANY, "at"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        wxArrayString efforts; efforts.Add("Auto"); efforts.Add("Off"); efforts.Add("Low"); efforts.Add("Medium"); efforts.Add("High");
        m_testEffort = new wxChoice(right, wxID_ANY, wxDefaultPosition, wxDefaultSize, efforts);
        m_testEffort->SetSelection(0);
        m_testEffort->SetToolTip("Reasoning level used for the test");
        testRow->Add(m_testEffort, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
        testRow->Add(new wxStaticText(right, wxID_ANY, "reasoning"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
        m_testResult = new wxStaticText(right, wxID_ANY, wxEmptyString,
            wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
        testRow->Add(m_testResult, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
        m_testTools = new wxStaticText(right, wxID_ANY, wxEmptyString);
        testRow->Add(m_testTools, 1, wxALIGN_CENTER_VERTICAL);
        R->Add(testRow, 0, wxEXPAND | wxTOP, FromDIP(10));
        m_testHelp = Muted(right, wxEmptyString);
        R->Add(m_testHelp, 0, wxEXPAND | wxTOP, FromDIP(4));

        auto* margins = new wxBoxSizer(wxVERTICAL);
        margins->Add(R, 1, wxEXPAND | wxALL, FromDIP(16));
        right->SetSizer(margins);
    }

    // Colours the generic recursive pass gets wrong.  Called once after
    // it, and again whenever the provider selection changes.
    void ApplyThemeColors()
    {
        const ThemeData& t = *m_theme;
        m_presetTrack->SetBackgroundColour(t.bgMain);
        UpdatePresetButtons();
        for (wxWindow* card : { static_cast<wxWindow*>(m_keyCard), static_cast<wxWindow*>(m_retryPanel) })
            card->SetBackgroundColour(t.bgMain);
        m_finishBtn->SetBackgroundColour(t.accentButton);
        m_finishBtn->SetForegroundColour(t.accentButtonText);
        m_chatgptSignInBtn->SetBackgroundColour(t.accentButton);
        m_chatgptSignInBtn->SetForegroundColour(t.accentButtonText);
        for (wxButton* b : { m_cancelBtn, m_statusDetails, m_chatgptSignOutBtn, m_scanBtn,
                             m_rawToggleBtn, m_manualModelsBtn }) {
            b->SetBackgroundColour(t.bgDialogSurface);
            b->SetForegroundColour(t.textMuted);
        }
        for (wxButton* b : { m_saveBtn, m_fetchBtn, m_testBtn, m_getKeyBtn }) {
            b->SetBackgroundColour(t.bgInputField);
            b->SetForegroundColour(t.textPrimary);
        }
        m_replaceKeyBtn->SetBackgroundColour(t.bgMain);
        m_replaceKeyBtn->SetForegroundColour(t.accentButton);
        m_retryTitle->SetForegroundColour(t.mdCode);
        m_modelList->SetBackgroundColour(t.bgInputField);
        m_modelList->SetForegroundColour(t.textPrimary);
        m_advancedPane->SetBackgroundColour(t.bgDialogSurface);
        m_advancedPane->SetForegroundColour(t.textPrimary);
        m_advancedPane->GetPane()->SetBackgroundColour(t.bgDialogSurface);
        for (auto* h : m_hints) h->SetForegroundColour(t.textMuted);
        for (auto* h : m_sectionLabels) h->SetForegroundColour(t.textMuted);
        for (wxRadioButton* rb : { m_bearerRadio, m_xApiKeyRadio, m_noAuthRadio, m_nativeRadio, m_xmlRadio }) {
            rb->SetForegroundColour(t.textPrimary);
            rb->SetBackgroundColour(t.bgDialogSurface);
        }
        for (wxCheckBox* cb : { m_showKeyBox, m_allowTools, m_imageOutput }) {
            cb->SetForegroundColour(t.textPrimary);
            cb->SetBackgroundColour(t.bgDialogSurface);
        }
        m_keepChecking->SetForegroundColour(t.textPrimary);
        m_keepChecking->SetBackgroundColour(t.bgMain);
        Refresh();
    }

    void UpdatePresetButtons()
    {
        const ThemeData& t = *m_theme;
        for (size_t i = 0; i < m_presetBtns.size(); ++i) {
            const bool on = kPresetDisplayOrder[i] == m_preset;
            wxButton* b = m_presetBtns[i];
            b->SetBackgroundColour(on ? t.accentButton : t.bgMain);
            // Locked (Edit mode) choices read as unavailable.
            b->SetForegroundColour(on ? t.accentButtonText : m_isNew ? t.textPrimary : t.chatThought);
            wxFont f = b->GetFont(); f.SetWeight(on ? wxFONTWEIGHT_SEMIBOLD : wxFONTWEIGHT_NORMAL); b->SetFont(f);
            // The provider is fixed once a connection exists: conversations
            // key their model entries on its id.
            b->Enable(m_isNew || on);
            b->Refresh();
        }
    }

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
        if (!host.empty() && host.front() == '[') {          // [::1]:8080
            const size_t close = host.find(']');
            host = close == std::string::npos ? host.substr(1) : host.substr(1, close - 1);
        } else {
            const size_t port = host.rfind(':');
            if (port != std::string::npos) host.erase(port);
        }
        std::transform(host.begin(), host.end(), host.begin(),
                       [](unsigned char c) { return (char)lb_ascii::ToLower(c); });
        return host;
    }

    static int DetectPreset(const std::string& baseUrl,
                            EndpointStore::AuthScheme auth = EndpointStore::AuthScheme::Bearer)
    {
        const std::string host = HostOfUrl(baseUrl);
        if (host == "api.openai.com")  return kPresetOpenAI;
        if (host == "openrouter.ai")   return kPresetOpenRouter;
        if (auth == EndpointStore::AuthScheme::None &&
            (host == "127.0.0.1" || host == "localhost" || host == "::1"))
            return kPresetLocal;
        return kPresetCustom;
    }

    static int DialectIndex(const std::string& d)
    {
        return d == "openai" ? 1 : d == "openrouter" ? 2 : d == "template" ? 3 : 0;
    }

    bool IsLocal() const { return m_preset == kPresetLocal; }
    bool UsingNoAuth() const { return m_noAuthRadio && m_noAuthRadio->GetValue(); }
    bool ReadyToSave() const { return m_editMode || m_connected || m_manualMode; }

    void OnPresetClicked(int preset)
    {
        if (!m_isNew || preset == m_preset || m_signInAttempt) return;
        SeedFromPreset(preset);
    }

    void SeedFromPreset(int preset)
    {
        m_rescanTimer.Stop();
        ++m_scanGeneration;          // drop a scan still running for the old preset
        m_scanInFlight = false;
        m_scanBtn->Enable(true);
        m_preset = preset;
        auto p = MakeProviderPreset(preset);
        if (preset == kPresetCustom) { p.id = "custom"; p.displayName = "Custom provider"; p.secretProvider = "custom"; }
        const auto* configured = m_endpointStore ? m_endpointStore->FindEndpoint(p.id) : nullptr;
        if (configured) p = *configured;
        m_manualMode = false;
        m_scanResults.clear();
        RebuildScanList();
        LoadEndpointFields(p, configured != nullptr);
        SetModelStatus(preset == kPresetLocal ? "Models show up here once a server answers."
                     : preset == kPresetChatGpt ? "Sign in to load your ChatGPT models."
                     : "Connect to load the model catalog.", StatusTone::Neutral);
        UpdatePresetButtons();
        if (m_advancedPane->IsExpanded()) m_advancedPane->Collapse();
        if (preset == kPresetCustom) m_advancedPane->Expand();
        ApplyProviderMode();
        if (preset == kPresetLocal) StartLocalScan();
        else if (m_apiKeyField->IsShown()) m_apiKeyField->SetFocus();
    }

    // Puts one endpoint's settings into every field.  Used by the provider
    // row and by picking a scanned server that is already saved.
    void LoadEndpointFields(const EndpointStore::Endpoint& p, bool existing)
    {
        const std::string oldProvider = TrimUtf8(m_providerField->GetValue());

        // ChangeValue everywhere: apply without firing wxEVT_TEXT, so the
        // refreshes below run once, after all fields are consistent.
        if (m_isNew) m_idField->ChangeValue(wxString::FromUTF8(p.id));
        m_nameField->ChangeValue(wxString::FromUTF8(p.displayName));
        m_idEditedByUser = existing;   // a saved id never follows the name
        m_baseUrlField->ChangeValue(wxString::FromUTF8(p.baseUrl));
        m_chatPathField->ChangeValue(wxString::FromUTF8(p.chatPath));
        m_bearerRadio ->SetValue(p.authScheme == EndpointStore::AuthScheme::Bearer);
        m_xApiKeyRadio->SetValue(p.authScheme == EndpointStore::AuthScheme::XApiKey);
        m_noAuthRadio ->SetValue(p.authScheme == EndpointStore::AuthScheme::None);
        UpdateAuthFieldEnables();
        m_originalDialect = p.reasoningDialect;
        m_dialectChoice->SetSelection(DialectIndex(p.reasoningDialect));
        const bool xml = (p.protocol == ToolProtocol::Xml);
        m_nativeRadio->SetValue(!xml);
        m_xmlRadio   ->SetValue(xml);
        m_extraHeaders = p.extraHeaders;

        m_providerField->ChangeValue(wxString::FromUTF8(p.secretProvider));
        if (oldProvider != p.secretProvider) m_apiKeyField->ChangeValue(wxEmptyString);
        m_keyField->ChangeValue(wxString::FromUTF8(p.secretKey));
        RebuildKeyChoices();
        m_replacingKey = false;
        m_keyVerified = false;

        m_seededExistingId = existing ? p.id : std::string();
        // Model ids configured for one provider are not valid on another
        // (OpenAI's bare ids vs OpenRouter's vendor-prefixed ones), so the
        // list is re-seeded too.  Safe because the dialog is transactional:
        // Cancel discards everything.
        m_syncingModels = true;
        m_models = p.models;
        m_rawModelsDirty = false;
        m_modelsField->ChangeValue(ModelsToText(m_models));
        m_syncingModels = false;

        m_chatgptAccount = p.chatgptAccount;
        m_fetchedIds.clear();
        m_fetchedCtx.clear();
        m_fetchedNames.clear();
        InvalidateFetch();
        SetModelStatus(std::string(), StatusTone::Neutral);
        SetFetchStatus(existing ? "Saved connection loaded. Your models are kept."
                     : IsChatGpt() ? "Sign in with your ChatGPT account to load your models."
                     : "Connect to check the connection and load models.");
        UpdateResolvedUrl();
        UpdateAdvancedSummary();
        UpdateKeyCard();
        RefreshModelChecklist();
    }

    // Shows only what the selected provider needs.
    void ApplyProviderMode(bool relayout = true)
    {
        const bool chatgpt = IsChatGpt();
        const bool local = IsLocal();
        const bool noAuth = UsingNoAuth();
        m_scanGroup->Show(local && m_isNew);
        m_baseUrlLabel->SetLabel(local ? (m_isNew ? "OR TYPE AN ADDRESS" : "ADDRESS") : "BASE URL");
        const bool showUrl = local || m_preset == kPresetCustom;
        m_baseUrlLabel->Show(showUrl);
        m_baseUrlField->Show(showUrl);
        m_keyGroup->Show(!chatgpt && !noAuth);
        m_chatgptPanel->Show(chatgpt);
        m_advancedPane->Show(!chatgpt);
        if (!noAuth || chatgpt) HideRetry();
        m_ctxField->SetHint(local || noAuth ? "server default" : "256K default");
        UpdateChatGptStatus();
        UpdateKeyCard();
        UpdateTestHelp();
        UpdateSaveButtons();
        if (relayout) RefreshLayout();
    }

    // ── Local server scan ───────────────────────────────────────
    // quiet: the automatic 5 s re-check while nothing has been found.  It
    // leaves the list alone instead of flashing "Scanning...".
    void StartLocalScan(bool quiet = false)
    {
        if (!IsLocal() || !m_isNew) return;
        m_rescanTimer.Stop();
        const unsigned gen = ++m_scanGeneration;
        m_scanInFlight = true;
        if (!quiet) {
            m_scanResults.clear();
            RebuildScanList();
            m_scanHint->SetLabel("Looking for servers on this PC...");
        }
        m_scanBtn->Enable(false);
        wxEvtHandler* target = this;
        std::weak_ptr<std::atomic<bool>> alive = m_alive;
        std::thread([work = lb::BeginBackgroundWork(), target, alive, gen]() {
            std::vector<LocalServerInfo> found = ScanLocalServers();
            auto* ev = new wxThreadEvent(wxEVT_EP_LOCAL_SCAN_DONE);
            ev->SetPayload(found);
            ev->SetInt((int)gen);
            LbQueueEventIfAlive(target, alive, ev);
        }).detach();
    }

    void OnLocalScanDone(wxThreadEvent& event)
    {
        if ((unsigned)event.GetInt() != m_scanGeneration) return;
        m_scanInFlight = false;
        m_scanBtn->Enable(true);
        if (!IsLocal()) return;
        m_scanResults = event.GetPayload<std::vector<LocalServerInfo>>();
        RebuildScanList();
        // Nothing yet: keep looking, so starting the server is all it takes.
        const bool keepLooking = m_scanResults.empty() && !m_connected;
        m_scanHint->SetLabel(wxString::Format(keepLooking
            ? "Checked ports %s. Still looking every 5 seconds; start your server and it will show up here."
            : "Checked ports %s. LlamaBoss's own local server is left out.", kLocalScanPortsText));
        m_scanHint->Wrap(FromDIP(310));
        if (keepLooking) m_rescanTimer.StartOnce(5000);
        // Pick the first one unless the user is already typing an address.
        if (!m_scanResults.empty() && !m_connected && !m_fetchInFlight)
            SelectScannedServer(0);
        RefreshLayout();
    }

    void RebuildScanList()
    {
        m_scanList->DestroyChildren();
        m_scanRadios.clear();
        auto* s = m_scanList->GetSizer();
        s->Clear();
        if (m_scanResults.empty()) {
            auto* none = new wxStaticText(m_scanList, wxID_ANY,
                m_scanInFlight ? wxString("Scanning...") : wxString("None answered on the usual ports."));
            none->SetForegroundColour(m_theme->textPrimary);
            s->Add(none, 0, wxALL, FromDIP(4));
        }
        for (size_t i = 0; i < m_scanResults.size(); ++i) {
            const auto& srv = m_scanResults[i];
            auto* rb = new wxRadioButton(m_scanList, wxID_ANY, wxString::FromUTF8(srv.name),
                wxDefaultPosition, wxDefaultSize, i == 0 ? wxRB_GROUP : 0);
            wxFont f = rb->GetFont(); f.SetWeight(wxFONTWEIGHT_SEMIBOLD); rb->SetFont(f);
            rb->SetForegroundColour(m_theme->textPrimary);
            rb->SetBackgroundColour(m_theme->bgDialogSurface);
            rb->SetValue(false);
            rb->Bind(wxEVT_RADIOBUTTON, [this, i](wxCommandEvent&) { SelectScannedServer(i); });
            s->Add(rb, 0, wxTOP, FromDIP(i == 0 ? 2 : 8));
            std::string detail = "127.0.0.1:" + std::to_string(srv.port) + "  \xC2\xB7  " +
                std::to_string(srv.ids.size()) + (srv.ids.size() == 1 ? " model" : " models");
            if (srv.ids.size() == 1) {
                const auto ctx = srv.contexts.find(srv.ids.front());
                if (ctx != srv.contexts.end()) detail += "  \xC2\xB7  " + FormatContextTokens(ctx->second) + " context";
            }
            if (const auto* saved = FindSavedServer(srv.baseUrl))
                detail += "  \xC2\xB7  saved as " + saved->displayName;
            auto* d = new wxStaticText(m_scanList, wxID_ANY, wxString::FromUTF8(detail));
            wxFont df = d->GetFont(); df.SetPointSize(9); d->SetFont(df);
            d->Wrap(FromDIP(280));
            d->SetForegroundColour(m_theme->textMuted);
            s->Add(d, 0, wxLEFT, FromDIP(22));
            m_scanRadios.push_back(rb);
        }
        m_scanList->Layout();
    }

    const EndpointStore::Endpoint* FindSavedServer(const std::string& baseUrl) const
    {
        if (!m_endpointStore) return nullptr;
        for (const auto& ep : m_endpointStore->Endpoints())
            if (ep.authScheme == EndpointStore::AuthScheme::None && SameLocalBase(ep.baseUrl, baseUrl))
                return &ep;
        return nullptr;
    }

    // The scan already fetched the catalog, so picking a server is a
    // complete connect: no Connect click needed.
    void SelectScannedServer(size_t index)
    {
        if (index >= m_scanResults.size()) return;
        m_rescanTimer.Stop();
        const LocalServerInfo srv = m_scanResults[index];
        if (index < m_scanRadios.size()) m_scanRadios[index]->SetValue(true);
        m_retryTimer.Stop();
        HideRetry();

        if (const auto* saved = FindSavedServer(srv.baseUrl)) {
            // Already saved: edit that connection rather than add a twin.
            const EndpointStore::Endpoint copy = *saved;
            LoadEndpointFields(copy, true);
        } else {
            InvalidateFetch();
            m_baseUrlField->ChangeValue(wxString::FromUTF8(srv.baseUrl));
            const std::string name = srv.name == "Local server"
                ? "Local server " + std::to_string(srv.port) : srv.name;
            m_nameField->ChangeValue(wxString::FromUTF8(name));
            if (m_isNew && !m_seededExistingId.empty()) {
                // The preset seed was an existing "local_server" record;
                // a different server gets its own id.
                m_seededExistingId.clear();
                m_idEditedByUser = false;
            }
            SyncIdFromName();
            m_noAuthRadio->SetValue(true);
            UpdateAuthFieldEnables();
            m_syncingModels = true;
            m_models.clear();
            m_modelsField->ChangeValue(wxEmptyString);
            m_rawModelsDirty = false;
            m_syncingModels = false;
            UpdateResolvedUrl();
        }
        // Treat the scan's catalog as fetched.
        ++m_fetchGeneration;
        m_fetchedIds = srv.ids;
        m_fetchedCtx = srv.contexts;
        m_fetchedNames.clear();
        if (srv.ids.size() == 1 && !IsInPicker(srv.ids.front()))
            SetModelPickerVisibility(m_models, srv.ids.front(), true);
        for (const auto& id : srv.ids) if (IsInPicker(id)) ApplyDetectedContext(id);
        m_modelsField->ChangeValue(ModelsToText(m_models));
        m_connected = true;
        SetModelStatus(kCheck + srv.name + " answered on port " + std::to_string(srv.port) +
                       ". No key needed.", StatusTone::Good);
        RefreshModelChecklist();
        SelectFirstChecked();
        SetFetchStatus(PickerSummary());
        ApplyProviderMode();
    }

    // ── Retry callout (key-free servers that refuse the connection) ─
    void ShowRetry(const std::string& base)
    {
        std::string where = HostOfUrl(base);
        try { Poco::URI uri(base); where = "port " + std::to_string(uri.getPort()); } catch (...) {}
        m_retryTitle->SetLabel(wxString::FromUTF8("Nothing is listening on " + where));
        m_retryPanel->Show();
        if (m_keepChecking->GetValue()) m_retryTimer.StartOnce(5000);
        RefreshLayout();
    }

    void HideRetry()
    {
        m_retryTimer.Stop();
        if (m_retryPanel && m_retryPanel->IsShown()) { m_retryPanel->Hide(); RefreshLayout(); }
    }

    // ── API key card ────────────────────────────────────────────
    std::string SavedKey() const
    {
        const std::string provider = TrimUtf8(m_providerField->GetValue());
        const std::string keyName  = TrimUtf8(m_keyField->GetValue());
        if (m_secretsStore && !provider.empty() && !keyName.empty() &&
            m_secretsStore->HasSecret(provider, keyName))
            return m_secretsStore->GetSecret(provider, keyName);
        return {};
    }

    std::string ProviderLabel() const
    {
        if (m_preset == kPresetOpenAI) return "OpenAI";
        if (m_preset == kPresetOpenRouter) return "OpenRouter";
        const std::string n = TrimUtf8(m_nameField->GetValue());
        return n.empty() ? std::string("the provider") : n;
    }

    void UpdateKeyCard()
    {
        if (!m_keyCard || m_rebuildingKeys) return;
        const std::string saved = SavedKey();
        const bool typed = !TrimUtf8(m_apiKeyField->GetValue()).empty();
        const bool hasSaved = !saved.empty();
        m_keyCard->Show(hasSaved && !m_replacingKey && !typed);
        m_keyEntry->Show(!hasSaved || m_replacingKey || typed);
        m_getKeyBtn->Show(!hasSaved);
        if (hasSaved) {
            m_keyCardTitle->SetLabel(wxString::FromUTF8(kCheck + "Saved key"));
            m_keyCardMasked->SetLabel(wxString::FromUTF8(MaskKey(saved)));
            m_keyCardDetail->SetLabel(wxString::FromUTF8(m_keyVerified
                ? "Accepted by " + ProviderLabel() + " just now"
                : "Saved as " + TrimUtf8(m_providerField->GetValue()) + " / " + TrimUtf8(m_keyField->GetValue())));
        }
        m_apiKeyField->SetHint(hasSaved ? "Paste the new key" : "Paste your API key");
        m_keyGroup->Layout();
        if (m_leftPane) { m_leftPane->Layout(); m_leftPane->FitInside(); }
    }

    // ── ChatGPT plan (Sign in with ChatGPT) ─────────────────────
    bool IsChatGpt() const { return m_preset == kPresetChatGpt; }

    bool ChatGptSignedIn(lb_chatgpt::AccountSummary* out = nullptr) const
    {
        lb_chatgpt::AccountSummary a;
        const bool ok = !m_chatgptAccount.empty() &&
            lb_chatgpt::Auth::Get().FindAccount(m_chatgptAccount, a) &&
            a.signedIn && a.planEnabled;
        if (out) *out = a;
        return ok;
    }

    void UpdateChatGptStatus()
    {
        if (!m_chatgptStatus) return;
        lb_chatgpt::AccountSummary a;
        const bool known = !m_chatgptAccount.empty() &&
            lb_chatgpt::Auth::Get().FindAccount(m_chatgptAccount, a);
        const std::string who = !a.email.empty() ? a.email
                              : !a.name.empty() ? a.name : std::string("your ChatGPT account");
        std::string text;
        if (m_signInAttempt)
            text = "Waiting for you to finish in your browser... (sign in, then allow LlamaBoss to use your plan)";
        else if (known && a.signedIn && a.planEnabled)
            text = kCheck + "Signed in as " + who + ".";
        else if (known && a.signedIn)
            text = kCross + "Signed in as " + who + ", but plan use wasn't allowed. "
                   "Choose Continue with ChatGPT and allow it.";
        else if (known)
            text = "Signed out (" + who + ").";
        else
            text = "Not signed in.";
        m_chatgptStatus->SetLabel(wxString::FromUTF8(text));
        m_chatgptStatus->Wrap(FromDIP(310));
        m_chatgptStatus->SetToolTip(wxString::FromUTF8(text));
        m_chatgptSignInBtn->SetLabel(m_signInAttempt ? "Cancel sign-in" : "Continue with ChatGPT");
        m_chatgptSignOutBtn->Show(known && a.signedIn && !m_signInAttempt);
        m_chatgptPanel->Layout();
    }

    void OnChatGptSignIn(wxCommandEvent&)
    {
        // The same button cancels a sign-in that is waiting on the browser.
        if (m_signInAttempt) {
            lb_chatgpt::Auth::CancelSignIn(m_signInAttempt);
            return;
        }
        // A known account re-authorizes in place (same registration, no
        // consent screen).  After Sign out the connection has no account,
        // so this registers fresh and the browser offers an account picker.
        std::string reauth;
        lb_chatgpt::AccountSummary existing;
        if (!m_chatgptAccount.empty() &&
            lb_chatgpt::Auth::Get().FindAccount(m_chatgptAccount, existing))
            reauth = m_chatgptAccount;

        std::string url, error;
        auto attempt = lb_chatgpt::Auth::Get().BeginSignIn(reauth, url, error);
        if (!attempt) {
            SetFetchStatus(kCross + error);
            return;
        }
        m_signInAttempt = attempt;
        const unsigned gen = ++m_signInGeneration;

        if (wxLaunchDefaultBrowser(wxString::FromUTF8(url))) {
            SetFetchStatus("ChatGPT opened in your browser. Sign in, allow LlamaBoss to use your plan, "
                           "then come back here.");
        } else {
            if (wxTheClipboard->Open()) {
                wxTheClipboard->SetData(new wxTextDataObject(wxString::FromUTF8(url)));
                wxTheClipboard->Close();
            }
            SetFetchStatus("Couldn't open your browser. The sign-in link is on the clipboard: paste it "
                           "into a browser on this computer.");
        }
        UpdateChatGptStatus();
        UpdateSaveButtons();

        wxEvtHandler* target = this;
        std::weak_ptr<std::atomic<bool>> alive = m_alive;
        std::thread([work = lb::BeginBackgroundWork(), target, alive, attempt, gen]() {
            lb_chatgpt::SignInResult r = lb_chatgpt::Auth::Get().CompleteSignIn(attempt);
            auto* ev = new wxThreadEvent(wxEVT_EP_CHATGPT_SIGNED_IN);
            ev->SetPayload(r);
            ev->SetInt(static_cast<int>(gen));
            LbQueueEventIfAlive(target, alive, ev);
        }).detach();
    }

    void OnChatGptSignedIn(wxThreadEvent& event)
    {
        if (static_cast<unsigned>(event.GetInt()) != m_signInGeneration) return;
        m_signInAttempt.reset();
        const auto r = event.GetPayload<lb_chatgpt::SignInResult>();
        UpdateSaveButtons();
        if (r.cancelled) {
            UpdateChatGptStatus();
            SetFetchStatus("Sign-in cancelled.");
            return;
        }
        if (!r.ok) {
            UpdateChatGptStatus();
            SetFetchStatus(kCross + r.error);
            return;
        }
        m_chatgptAccount = r.account.clientId;
        InvalidateFetch();          // a different credential = a different catalog
        UpdateChatGptStatus();
        Raise();
        if (!r.account.planEnabled) {
            SetFetchStatus(kCross + "Signed in, but ChatGPT plan use wasn't allowed, so LlamaBoss can't "
                           "use your plan. Choose Continue with ChatGPT again and allow it.");
            return;
        }
        m_connectPending = !ReadyToSave();
        wxCommandEvent e;
        OnFetchModels(e);
    }

    void OnChatGptSignOut(wxCommandEvent&)
    {
        if (m_chatgptAccount.empty()) return;
        if (wxMessageBox("Sign LlamaBoss out of ChatGPT on this computer?\n\n"
                         "Chats using this connection will stop until you sign in again.",
                         "Sign out of ChatGPT", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES)
            return;
        lb_chatgpt::Auth::Get().SignOut(m_chatgptAccount);
        m_chatgptAccount.clear();
        InvalidateFetch();
        UpdateChatGptStatus();
        SetFetchStatus("Signed out of ChatGPT. Choose Continue with ChatGPT to sign in again.");
        RefreshLayout();
    }

    void RefreshLayout()
    {
        if (m_advancedPane && m_advancedPane->GetParent())
            m_advancedPane->GetParent()->Layout();
        if (m_leftPane) m_leftPane->FitInside();
        if (m_rightPane) m_rightPane->Layout();
        Layout();
        AlignAdvancedLabels();
    }

    // wxCollapsiblePane insets its child panel by a few px (platform and
    // theme dependent).  Keep the Advanced labels one width regardless.
    void AlignAdvancedLabels()
    {
        if (!m_advGrid || !m_advancedPane || !m_advancedPane->IsExpanded()) return;
        m_advancedPane->GetPane()->Layout();
    }

    void UpdateAdvancedSummary()
    {
        if (!m_advancedPane) return;
        // Short enough for the left pane: only what differs from a plain
        // native-tools connection is worth a glance.
        std::string summary = "Advanced";
        if (!IsChatGpt()) {
            static const char* names[] = { "auto reasoning", "OpenAI reasoning",
                                           "OpenRouter reasoning", "template reasoning" };
            const int d = std::max(0, m_dialectChoice->GetSelection());
            summary += std::string("  \xC2\xB7  ") + names[d];
            if (m_xmlRadio->GetValue()) summary += ", XML tools";
        }
        m_advancedPane->SetLabel(wxString::FromUTF8(summary));
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
            const char c = (char)lb_ascii::ToLower((unsigned char)cp);
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
        m_rawToggleBtn->SetLabel(m_rawMode ? "Show model list" : "Edit as text");
        if (m_rawMode) {
            m_modelCountLabel->SetLabel(
                "id = \"name\"   [image]   [text]   [no-tools]   - one model per line");
        }
        RefreshModelChecklist();   // re-derives the count label and options
        m_rightPane->Layout();
        if (m_rawMode) m_modelsField->SetFocus();
    }

    void UpdateModelCountLabel(size_t configured, size_t shown)
    {
        if (!m_modelCountLabel || m_rawMode) return;
        wxString text = !m_fetchedIds.empty()
            ? wxString::Format("%lu of %lu in your picker", (unsigned long)configured,
                               (unsigned long)m_fetchedIds.size())
            : configured == 1 ? wxString("1 model in your picker")
            : wxString::Format("%lu models in your picker", (unsigned long)configured);
        if (!TrimUtf8(m_filterField->GetValue()).empty())
            text += wxString::Format("  (%lu shown)", (unsigned long)shown);
        m_modelCountLabel->SetLabel(text);
    }

    // The key fields are meaningless with no key: disable the Advanced
    // ones (the key box itself is hidden by ApplyProviderMode).
    void UpdateAuthFieldEnables()
    {
        const bool useKeys = !UsingNoAuth();
        m_providerField->Enable(useKeys);
        m_keyField->Enable(useKeys);
        m_apiKeyField->Enable(useKeys);
    }

    std::string EffectiveApiKey() const
    {
        // No-auth endpoints never send a key — even one sitting in the
        // fields from before the scheme was switched.
        if (UsingNoAuth()) return {};
        const std::string entered = TrimUtf8(m_apiKeyField->GetValue());
        if (!entered.empty()) return entered;
        return SavedKey();
    }

    // ── Status lines ────────────────────────────────────────────
    void SetModelStatus(const std::string& text, StatusTone tone)
    {
        const wxString label = wxString::FromUTF8(text);
        m_modelConnectionStatus->SetLabel(label);
        m_modelConnectionStatus->SetToolTip(label);
        m_modelConnectionStatus->SetForegroundColour(
            tone == StatusTone::Good ? wxColour(163, 190, 140)     // aurora green
          : tone == StatusTone::Bad  ? m_theme->stopButton
                                     : m_theme->textPrimary);
        m_modelConnectionStatus->Refresh();
        if (m_rightPane) m_rightPane->Layout();
    }

    std::string PickerSummary() const
    {
        const size_t n = ModelsForPicker(m_models).size();
        if (n == 0) return "Click a model to add it to your picker.";
        return std::to_string(n) + (n == 1 ? " model" : " models") +
               " in your picker. Click a row to add it, untick to remove it.";
    }

    // ── Fetch models ────────────────────────────────────────────
    // Anything that changes which endpoint/credential a catalog belongs
    // to bumps the generation; a completed fetch is applied only if its
    // stamped generation still matches, so a result for the OLD
    // provider/URL/key can never populate the NEW one.
    void InvalidateTest()
    {
        ++m_testRevision;
        if (m_testResult) m_testResult->SetLabel(wxEmptyString);
        if (m_testTools) m_testTools->SetLabel(wxEmptyString);
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
        // Keep the user's model records, drop the remote catalog.
        m_fetchedIds.clear();
        m_fetchedCtx.clear();
        if (!m_editMode) m_connected = false;
        m_keyVerified = false;
        if (m_modelConnectionStatus && !m_modelConnectionStatus->GetLabel().IsEmpty())
            SetModelStatus("Connection changed - not checked yet.", StatusTone::Neutral);
        if (m_editMode) SetFetchStatus("Connection changed. Refresh models to check it.");
        RefreshModelChecklist();
        UpdateSaveButtons();
    }

    void SetFetchStatus(const std::string& text)
    {
        if (!m_fetchStatus) return;
        m_statusText = text;
        const wxString full = wxString::FromUTF8(text);
        const bool longText = full.length() > 60;
        m_fetchStatus->SetLabel(full);
        m_fetchStatus->SetToolTip(full);
        if (m_statusDetails) m_statusDetails->Show(longText);
        if (GetSizer()) Layout();
    }

    void OnFetchModels(wxCommandEvent&)
    {
        if (m_fetchInFlight) return;
        m_retryTimer.Stop();

        if (IsChatGpt()) {
            if (!ChatGptSignedIn()) {
                m_connectPending = false;
                UpdateSaveButtons();
                SetFetchStatus("Choose Continue with ChatGPT to sign in first.");
                return;
            }
            const std::string account = m_chatgptAccount;
            m_fetchInFlight = true;
            m_fetchBtn->Enable(false);
            UpdateSaveButtons();
            const unsigned gen = m_fetchGeneration;
            m_inFlightGeneration = gen;
            if (!m_testInFlight && !m_hasTestResult)
                SetFetchStatus(m_connectPending ? "Loading your ChatGPT models..." : "Refreshing models...");
            wxEvtHandler* target = this;
            std::weak_ptr<std::atomic<bool>> alive = m_alive;
            std::thread([work = lb::BeginBackgroundWork(), target, alive, account, gen]() {
                FetchModelsResult r = FetchChatGptCatalog(account);
                auto* ev = new wxThreadEvent(wxEVT_EP_MODELS_FETCHED);
                ev->SetPayload(r);
                ev->SetInt((int)gen);
                LbQueueEventIfAlive(target, alive, ev);
            }).detach();
            return;
        }

        const std::string base = NormalizeBaseUrl(TrimUtf8(m_baseUrlField->GetValue()));
        if (base.rfind("http://", 0) != 0 && base.rfind("https://", 0) != 0) {
            SetFetchStatus(kCross + "The address must start with http:// or https://.");
            if (m_baseUrlField->IsShown()) m_baseUrlField->SetFocus();
            else m_advancedPane->Expand();
            return;
        }

        // Resolve auth material HERE — SecretsStore is UI-thread-only,
        // so the worker receives a ready-made header, never the store.
        const std::string key = EffectiveApiKey();
        if (!UsingNoAuth() && key.empty()) {
            SetFetchStatus("Paste an API key first.");
            m_replacingKey = true; UpdateKeyCard(); m_apiKeyField->SetFocus();
            return;
        }
        std::string headerName, headerValue;
        if (!key.empty()) {
            if (m_xApiKeyRadio->GetValue()) { headerName = "x-api-key"; headerValue = key; }
            else { headerName = "Authorization"; headerValue = "Bearer " + key; }
        }

        const std::string url = base + "/v1/models";
        const auto extra = m_extraHeaders;
        m_fetchInFlight = true;
        m_fetchBtn->Enable(false);
        UpdateSaveButtons();
        const unsigned gen = m_fetchGeneration;
        m_inFlightGeneration = gen;
        const bool noAuth = UsingNoAuth();
        if (!m_testInFlight && !m_hasTestResult)
            SetFetchStatus(m_connectPending ? "Checking the connection and loading models..." : "Refreshing models...");
        wxEvtHandler* target = this;
        std::weak_ptr<std::atomic<bool>> alive = m_alive;
        std::thread([work = lb::BeginBackgroundWork(), target, alive, url, headerName, headerValue, extra,
                     gen, key, base, noAuth]() {
            FetchModelsResult r = FetchConnectionCatalog(base, url, headerName, headerValue, extra, noAuth);
            // A key-free server with one model: ask /props for its window
            // when the catalog didn't say (llama-server style hosts).
            if (r.ok && noAuth && r.ids.size() == 1 && !r.contexts.count(r.ids.front())) {
                if (const int n = FetchPropsContext(base)) r.contexts[r.ids.front()] = n;
            }
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
        const bool wasConnect = m_connectPending;
        m_connectPending = false;
        m_fetchInFlight = false;
        m_fetchBtn->Enable(true);

        // Stale: provider/URL/credential changed while this request was in
        // flight.  Discard rather than populate the wrong endpoint's list.
        if (gen != m_fetchGeneration) {
            UpdateSaveButtons();
            if (!m_testInFlight && !m_hasTestResult)
                SetFetchStatus("The connection changed while loading. Connect or refresh again.");
            return;
        }

        FetchModelsResult r = event.GetPayload<FetchModelsResult>();
        if (!r.ok) {
            UpdateSaveButtons();
            const bool authFail = r.status == 401 || r.status == 403;
            if (IsChatGpt()) {
                SetModelStatus(kCross + (authFail ? "ChatGPT sign-in needs attention. Sign in again on the left."
                                                  : "Could not load your ChatGPT models."), StatusTone::Bad);
                SetFetchStatus(kCross + (r.error.empty() ? std::string("Could not load your ChatGPT models.") : r.error));
            } else if (authFail) {
                SetModelStatus(kCross + "The key was rejected. Replace it on the left.", StatusTone::Bad);
                SetFetchStatus(kCross + "Key check failed (" + r.error + "). Chat requests use this same key.");
                m_replacingKey = true;
                UpdateKeyCard();
            } else if (r.status == 0 && UsingNoAuth()) {
                // Nothing answered.  Nothing the user typed is touched.
                SetModelStatus(kCross + "No answer from " +
                    NormalizeBaseUrl(TrimUtf8(m_baseUrlField->GetValue())) + ".", StatusTone::Bad);
                SetFetchStatus(kCross + r.error + ". Nothing you typed was lost.");
                ShowRetry(NormalizeBaseUrl(TrimUtf8(m_baseUrlField->GetValue())));
            } else {
                SetModelStatus(kCross + (m_editMode ? "Could not load the catalog. Your checked models are still shown."
                                                    : "Could not load the catalog."), StatusTone::Bad);
                SetFetchStatus(kCross + "Fetch failed: " + r.error);
            }
            (void)wasConnect;
            return;
        }

        HideRetry();
        m_rescanTimer.Stop();
        m_connected = true;
        m_fetchedNames.clear();
        for (size_t i = 0; i < r.ids.size() && i < r.names.size(); ++i)
            m_fetchedNames[r.ids[i]] = r.names[i];
        m_fetchedIds = std::move(r.ids);
        m_fetchedCtx = std::move(r.contexts);
        // A key-free server with a single model: add it right away.
        if (UsingNoAuth() && m_fetchedIds.size() == 1 && !IsInPicker(m_fetchedIds.front()))
            SetModelPickerVisibility(m_models, m_fetchedIds.front(), true);
        for (const auto& model : ModelsForPicker(m_models)) ApplyDetectedContext(model.id);
        m_modelsField->ChangeValue(ModelsToText(m_models));

        if (IsChatGpt()) {
            SetModelStatus(kCheck + "Signed in to ChatGPT.", StatusTone::Good);
        } else if (UsingNoAuth()) {
            std::string where = HostOfUrl(TrimUtf8(m_baseUrlField->GetValue()));
            int port = 0;
            try { port = Poco::URI(NormalizeBaseUrl(TrimUtf8(m_baseUrlField->GetValue()))).getPort(); } catch (...) {}
            if (port > 0) where += ":" + std::to_string(port);
            // Typed address on a new connection: name it after what answered.
            if (m_isNew && IsLocal() && TrimUtf8(m_nameField->GetValue()) == "Local server") {
                const std::string guess = LocalServerName(port, r.ownedBy);
                if (guess != "Local server") { m_nameField->ChangeValue(wxString::FromUTF8(guess)); SyncIdFromName(); }
            }
            SetModelStatus(kCheck + where + " answered. No key used.", StatusTone::Good);
        } else if (r.credentialsVerified) {
            m_keyVerified = true;
            m_replacingKey = false;
            SetModelStatus(kCheck + "Connected to " + ProviderLabel() + ".", StatusTone::Good);
        } else {
            SetModelStatus(kCheck + "Catalog loaded. This service doesn't confirm keys separately; "
                           "Test model checks it.", StatusTone::Good);
        }
        UpdateKeyCard();
        RefreshModelChecklist();
        SelectFirstChecked();
        UpdateSaveButtons();
        if (!m_testInFlight && !m_hasTestResult) SetFetchStatus(m_fetchedIds.empty()
            ? "The catalog is empty. Use Edit as text if you know the model ID."
            : PickerSummary());
    }

    // Known context for an id from the last catalog, applied to the
    // record only for key-free (local) servers: on paid providers the
    // 256K default is a cost ceiling and a 1M catalog value must not
    // silently lift it.
    void ApplyDetectedContext(const std::string& id)
    {
        if (!UsingNoAuth()) return;
        const auto it = m_fetchedCtx.find(id);
        if (it == m_fetchedCtx.end()) return;
        for (auto& model : m_models)
            if (model.id == id && model.contextLimit <= 0 && it->second >= 4096)
                model.contextLimit = std::min(it->second, 2097152);
    }

    // ── Test connection ─────────────────────────────────────────
    void OnTestConnection(wxCommandEvent&)
    {
        if (m_testInFlight) return;
        const std::string base = NormalizeBaseUrl(TrimUtf8(m_baseUrlField->GetValue()));
        if (base.rfind("http://", 0) != 0 && base.rfind("https://", 0) != 0) {
            SetFetchStatus(kCross + "The address must start with http:// or https://.");
            return;
        }
        const auto models = ModelsForPicker(m_models);
        const std::string selectedId = SelectedRowId();
        auto selected = std::find_if(models.begin(), models.end(), [&](const auto& m) { return m.id == selectedId; });
        if (selected == models.end()) { SetFetchStatus("Select a checked model to test."); return; }
        const std::string model = selected->id;
        if (selected->imageOutput) { SetFetchStatus("This test checks text and tools. Test image generation in a chat."); return; }
        const std::string resolvedPath = lb_responses::ResolveChatPath(
            base, TrimUtf8(m_chatPathField->GetValue()), model);
        if (lb_responses::IsResponsesPath(resolvedPath)) {
            SetFetchStatus("This model uses OpenAI Responses. Test it by sending a chat message (and a tool "
                           "request with Agent mode on); this test only speaks Chat Completions.");
            return;
        }
        const bool testTools = !selected->noTools && m_nativeRadio->GetValue();
        const std::string skipReason = selected->noTools ? "Agent tools is off" : "XML fallback is selected";
        static const char* efforts[] = {"auto", "none", "low", "medium", "high"};
        const std::string effort = efforts[std::max(0, m_testEffort->GetSelection())];
        std::string dialect = GetEndpoint().reasoningDialect;
        if (dialect.empty()) dialect = DetectPreset(base) == kPresetOpenAI ? "openai" : "openrouter";
        if (effort == "none" && lb_reasoning::RequiresReasoning(model, dialect == "openai")) {
            SetFetchStatus("This model requires reasoning. Pick Low or Auto for the test.");
            m_testEffort->SetFocus();
            return;
        }

        const std::string key = EffectiveApiKey();
        if (!UsingNoAuth() && key.empty()) { SetFetchStatus("Paste an API key first."); return; }
        std::string headerName, headerValue;
        if (!key.empty()) {
            if (m_xApiKeyRadio->GetValue()) { headerName = "x-api-key"; headerValue = key; }
            else { headerName = "Authorization"; headerValue = "Bearer " + key; }
        }
        const std::string url = JoinEndpointUrl(base, TrimUtf8(m_chatPathField->GetValue()));
        const auto extra = m_extraHeaders;

        m_testInFlight = true;
        m_hasTestResult = false;
        m_testBtn->Enable(false);
        m_testResult->SetForegroundColour(m_theme->textMuted);
        m_testResult->SetLabel("Testing...");
        m_testTools->SetLabel(wxEmptyString);
        const unsigned gen = m_testRevision;
        m_testGeneration = gen;
        SetFetchStatus("Testing " + model + ": plain chat, then chat with a tool...");

        wxEvtHandler* target = this;
        std::weak_ptr<std::atomic<bool>> alive = m_alive;
        std::thread([work = lb::BeginBackgroundWork(), target, alive, url, model, headerName, headerValue,
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
                    // Models often wrap the marker in a sentence ("The ping
                    // returned LLAMABOSS_PROBE_OK."); finding it is the proof.
                    if (r.tools.ok && r.tools.text.find("LLAMABOSS_PROBE_OK") == std::string::npos) {
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
            SetFetchStatus("Discarded a test started before the settings changed. Test again.");
            return;
        }

        m_hasTestResult = true;
        const TestConnectionResult r = event.GetPayload<TestConnectionResult>();
        auto secs = [](long ms) {
            char buf[32]; std::snprintf(buf, sizeof(buf), "%.1f s", ms / 1000.0); return std::string(buf);
        };
        const std::string level = r.effort == "none" ? "off" : r.effort;

        if (!r.chat.ok) {
            std::string why = r.chat.error;
            if (r.chat.status == 401 || r.chat.status == 403) why += " (key rejected)";
            else if (r.chat.status == 404) why += " (check the address and the chat path under Advanced)";
            m_testResult->SetForegroundColour(m_theme->stopButton);
            m_testResult->SetLabel(wxString::FromUTF8(kCross + "Chat failed"));
            m_testTools->SetLabel(wxEmptyString);
            SetFetchStatus(kCross + "Chat failed for " + r.model + ": " + why);
            return;
        }
        const wxColour good(163, 190, 140);
        m_testResult->SetForegroundColour(good);
        m_testResult->SetLabel(wxString::FromUTF8(kCheck + "Chat " + secs(r.chat.millis)));
        std::string tools;
        if (!r.toolsRun) {
            tools = "tools skipped";
            m_testTools->SetForegroundColour(m_theme->textMuted);
            SetFetchStatus(kCheck + r.model + ": chat passed. Tool test skipped: " + r.skipReason + ".");
        } else if (r.tools.ok) {
            tools = kCheck + "Tool call " + secs(r.tools.millis);
            m_testTools->SetForegroundColour(good);
            SetFetchStatus(kCheck + r.model + ": chat and tool round trip passed at " + level +
                           " reasoning. This checks a test tool, not every agent tool.");
        } else {
            tools = kCross + "Tool call";
            m_testTools->SetForegroundColour(m_theme->stopButton);
            SetFetchStatus("Chat passed; the tool round trip failed at " + level + " reasoning: " + r.tools.error);
        }
        m_testTools->SetLabel(wxString::FromUTF8(tools));
        m_testTools->SetToolTip(wxString::FromUTF8(r.toolsRun && !r.tools.ok ? r.tools.error : std::string()));
        m_rightPane->Layout();
    }

    void UpdateTestHelp()
    {
        std::string text;
        if (IsChatGpt()) text = "Runs on your ChatGPT plan through OpenAI Responses.";
        else if (UsingNoAuth()) text = "The test sends two tiny requests to this server.";
        else text = "The test sends two tiny requests, billed by " + ProviderLabel() + ".";
        m_testHelp->SetLabel(wxString::FromUTF8(text));
    }

    // ── Model list ──────────────────────────────────────────────
    // Structured model records are authoritative. Checked records first,
    // retained unchecked records next, new catalog entries last.
    std::string SelectedRowId() const
    {
        const int row = m_modelList ? m_modelList->GetSelection() : wxNOT_FOUND;
        return row >= 0 && row < (int)m_rowModelIds.size() ? m_rowModelIds[(size_t)row] : std::string();
    }

    bool IsInPicker(const std::string& id) const
    {
        return std::any_of(m_models.begin(), m_models.end(),
            [&](const auto& m) { return m.id == id && m.showInPicker; });
    }

    int KnownContext(const EndpointStore::Model* record, const std::string& id) const
    {
        if (record && record->contextLimit > 0) return record->contextLimit;
        const auto it = m_fetchedCtx.find(id);
        return it != m_fetchedCtx.end() ? it->second : 0;
    }

    void RefreshModelChecklist()
    {
        if (!m_modelList) return;

        const auto configured = ModelsForPicker(m_models);
        const std::string selectedId = SelectedRowId();

        std::string filter = TrimUtf8(m_filterField->GetValue());
        auto lower = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return (char)lb_ascii::ToLower(c); });
            return s;
        };
        filter = lower(filter);
        auto matches = [&](const std::string& id, const std::string& name) {
            if (filter.empty()) return true;
            return lower(id).find(filter) != std::string::npos ||
                   lower(name).find(filter) != std::string::npos;
        };
        auto withContext = [&](std::string label, int ctx) {
            if (ctx > 0) label += "   \xC2\xB7 " + FormatContextTokens(ctx);
            return label;
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
                m.displayName != PrettyNameFromId(m.id))
                label += "  =  " + m.displayName;
            if (m.imageOutput) label += "   [image]";
            if (m.noTools)     label += "   (chat only)";
            label = withContext(label, KnownContext(&m, m.id));
            const int idx = (int)m_modelList->Append(wxString::FromUTF8(label));
            m_modelList->Check((unsigned)idx, m.showInPicker);
            m_rowModelIds.push_back(m.id);
        };
        for (const auto& model : configured) appendModel(model);
        for (const auto& model : m_models)
            if (!model.showInPicker) appendModel(model);

        for (const auto& id : m_fetchedIds) {
            if (configuredIds.count(id)) continue;
            const auto named = m_fetchedNames.find(id);
            const std::string fetchedName = named != m_fetchedNames.end() ? named->second : PrettyNameFromId(id);
            if (!matches(id, fetchedName)) continue;
            std::string rowLabel = id;
            if (fetchedName != PrettyNameFromId(id)) rowLabel += "  =  " + fetchedName;
            rowLabel = withContext(rowLabel, KnownContext(nullptr, id));
            const int idx = (int)m_modelList->Append(wxString::FromUTF8(rowLabel));
            m_modelList->Check((unsigned)idx, false);
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

    void SelectFirstChecked()
    {
        for (size_t i = 0; i < m_rowModelIds.size(); ++i) {
            if (m_modelList->IsChecked((unsigned)i)) {
                m_modelList->SetSelection((int)i);
                m_modelList->EnsureVisible((int)i);
                break;
            }
        }
        LoadModelOptions();
    }

    // Adds a model to (or removes it from) the picker and keeps it
    // highlighted after the list reorders.
    void SetInPicker(const std::string& id, bool visible)
    {
        SetModelPickerVisibility(m_models, id, visible);
        if (visible) {
            const auto named = m_fetchedNames.find(id);
            if (named != m_fetchedNames.end() && !named->second.empty()) {
                for (auto& model : m_models)
                    if (model.id == id && model.displayName == PrettyNameFromId(id))
                        model.displayName = named->second;
            }
            ApplyDetectedContext(id);
        }
        m_modelsField->ChangeValue(ModelsToText(m_models));
        InvalidateTest();
        RefreshModelChecklist();
        auto it = std::find(m_rowModelIds.begin(), m_rowModelIds.end(), id);
        if (it != m_rowModelIds.end()) m_modelList->SetSelection((int)(it - m_rowModelIds.begin()));
        LoadModelOptions();
        if (ReadyToSave() && !m_testInFlight && !m_hasTestResult) SetFetchStatus(PickerSummary());
    }

    void OnModelToggled(wxCommandEvent& event)
    {
        const int idx = event.GetInt();
        if (idx < 0 || idx >= (int)m_rowModelIds.size()) return;
        SetInPicker(m_rowModelIds[(size_t)idx], m_modelList->IsChecked((unsigned)idx));
    }

    void UpdateResolvedUrl()
    {
        if (!m_resolvedUrlLabel) return;
        const std::string resolved = JoinEndpointUrl(
            TrimUtf8(m_baseUrlField->GetValue()), TrimUtf8(m_chatPathField->GetValue()));
        m_resolvedUrlLabel->SetLabel(resolved.empty() ? wxString("POST <base URL + chat path>")
                                                      : wxString::FromUTF8("POST " + resolved));
        m_resolvedUrlLabel->SetToolTip(m_resolvedUrlLabel->GetLabel());
    }

    void RebuildKeyChoices()
    {
        if (!m_keyField || m_rebuildingKeys) return;
        const std::string provider = TrimUtf8(m_providerField->GetValue());
        const wxString previous = m_keyField->GetValue();
        std::set<std::string> uniqueKeys;
        for (const auto& row : m_connections)
            if (row.provider == provider) uniqueKeys.insert(row.key);

        m_rebuildingKeys = true;
        m_keyField->Freeze();
        m_keyField->Clear();
        bool previousExists = false;
        for (const auto& key : uniqueKeys) {
            const wxString item = wxString::FromUTF8(key.c_str());
            m_keyField->Append(item);
            if (item == previous) previousExists = true;
        }
        if (previousExists) m_keyField->SetValue(previous);
        else if (m_keyField->GetCount() > 0) m_keyField->SetSelection(0);
        else if (!previous.IsEmpty()) m_keyField->SetValue(previous);
        else m_keyField->SetValue("api_key");
        m_keyField->Thaw();
        m_rebuildingKeys = false;
    }

    void OnProviderChanged(wxCommandEvent&)
    {
        // A different credential can see a different catalog (and the
        // fetch doubles as its auth test) — orphan any in-flight fetch.
        InvalidateFetch();
        RebuildKeyChoices();
        m_replacingKey = false;
        UpdateKeyCard();
    }

    void OnKeyChanged(wxCommandEvent&)
    {
        InvalidateFetch();
        m_replacingKey = false;
        UpdateKeyCard();
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
        if (UsingNoAuth())
            ep.authScheme = EndpointStore::AuthScheme::None;
        else
            ep.authScheme = m_xApiKeyRadio->GetValue() ? EndpointStore::AuthScheme::XApiKey
                                                      : EndpointStore::AuthScheme::Bearer;
        ep.secretProvider = TrimUtf8(m_providerField->GetValue());
        ep.secretKey      = TrimUtf8(m_keyField->GetValue());
        if (ep.secretKey.empty()) ep.secretKey = "api_key";
        ep.protocol    = m_xmlRadio->GetValue() ? ToolProtocol::Xml : ToolProtocol::Native;
        switch (m_dialectChoice->GetSelection()) {
            case 1:  ep.reasoningDialect = "openai";     break;
            case 2:  ep.reasoningDialect = "openrouter"; break;
            case 3:  ep.reasoningDialect = "template";   break;
            default: ep.reasoningDialect = m_originalDialect; break;  // preserve unknown overrides until edited
        }
        ep.models      = m_models;
        if (m_rawModelsDirty)
            MergePickerModels(ep.models, ParseModelsText(m_modelsField->GetValue()));
        ep.extraHeaders = m_extraHeaders;
        if (IsChatGpt()) {
            // Fixed by the Sign in with ChatGPT contract; nothing here is
            // user-editable, so stale field values can never leak in.
            ep.authScheme       = EndpointStore::AuthScheme::ChatGpt;
            ep.baseUrl          = lb_chatgpt::kApiBaseUrl();
            ep.chatPath         = lb_chatgpt::kResponsesPath();
            ep.secretProvider   = "chatgpt";
            ep.secretKey        = "account";
            ep.protocol         = ToolProtocol::Native;
            ep.reasoningDialect.clear();
            ep.extraHeaders.clear();
            ep.chatgptAccount   = m_chatgptAccount;
            for (auto& model : ep.models) model.imageOutput = false;
        }
        return ep;
    }

private:
    // Field checks shared by Connect / Save / Save and use.  On failure
    // the message goes to the status line; returns false.
    bool ValidateConnectionFields(EndpointStore::Endpoint& ep, bool& noAuth)
    {
        ep = GetEndpoint();
        auto connectionError = [this](const std::string& message) {
            if (!m_advancedPane->IsExpanded()) { m_advancedPane->Expand(); RefreshLayout(); }
            SetFetchStatus(kCross + message);
        };
        if (!IsSafeIdentifier(wxString::FromUTF8(ep.id))) {
            connectionError("Choose an id under Advanced using lowercase letters, digits or underscores.");
            return false;
        }
        if (IsChatGpt()) {
            // No key to check or save: the credential is the sign-in.
            noAuth = true;
            if (!ChatGptSignedIn()) {
                SetFetchStatus("Choose Continue with ChatGPT to sign in first.");
                m_chatgptSignInBtn->SetFocus();
                return false;
            }
            if (!m_endpointStore) {
                SetFetchStatus("Connection storage is unavailable. Restart LlamaBoss and try again."); return false;
            }
            return true;
        }
        try {
            Poco::URI uri(ep.baseUrl);
            if ((uri.getScheme() != "http" && uri.getScheme() != "https") || uri.getHost().empty() ||
                !uri.getUserInfo().empty() || !uri.getQuery().empty() || !uri.getFragment().empty())
                throw std::runtime_error("Invalid base URL");
        } catch (...) {
            SetFetchStatus(kCross + "Enter a valid http:// or https:// address, without a user name, query or fragment.");
            if (m_baseUrlField->IsShown()) m_baseUrlField->SetFocus();
            return false;
        }
        noAuth = ep.authScheme == EndpointStore::AuthScheme::None;
        if (!noAuth && (!IsSafeIdentifier(wxString::FromUTF8(ep.secretProvider)) ||
                        !IsSafeIdentifier(wxString::FromUTF8(ep.secretKey)))) {
            connectionError("Choose a key set and key name under Advanced (lowercase letters, digits or underscores).");
            return false;
        }
        if (!noAuth && EffectiveApiKey().empty()) {
            SetFetchStatus("Paste your API key to continue.");
            m_replacingKey = true; UpdateKeyCard();
            m_apiKeyField->SetFocus();
            return false;
        }
        if (!m_endpointStore || (!noAuth && !m_secretsStore)) {
            SetFetchStatus("Connection storage is unavailable. Restart LlamaBoss and try again."); return false;
        }
        return true;
    }

    // Persist the endpoint and (if one was typed) its key, with rollback.
    // Returns true when everything is on disk.
    bool CommitEndpoint(const EndpointStore::Endpoint& ep, bool noAuth)
    {
        EndpointStore::Endpoint previousEndpoint;
        const auto* existing = m_endpointStore->FindEndpoint(ep.id);
        const bool hadEndpoint = existing != nullptr;
        if (hadEndpoint) previousEndpoint = *existing;
        if (m_isNew && hadEndpoint && m_seededExistingId != ep.id && wxMessageBox(
            "A connection with this id already exists. Replace its models and settings?\n\n"
            "Choose No and change the name (or the id under Advanced) to keep both.",
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
        if (m_rawMode && m_rawModelsDirty) {
            MergePickerModels(m_models, ParseModelsText(m_modelsField->GetValue()));
            m_rawModelsDirty = false;
        }
        EndpointStore::Endpoint ep; bool noAuth = false;
        if (!ValidateConnectionFields(ep, noAuth)) return;
        if (!CommitEndpoint(ep, noAuth)) return;
        m_modelToUse.clear();
        EndModal(wxID_OK);
    }

    // Accent button: "Connect" until connected, then "Save and use".
    void OnOK(wxCommandEvent&)
    {
        if (m_signInAttempt) return;   // finish (or cancel) the browser sign-in first
        if (IsChatGpt() && !ChatGptSignedIn()) {
            wxCommandEvent e;
            OnChatGptSignIn(e);
            return;
        }
        EndpointStore::Endpoint ep; bool noAuth = false;
        if (!ValidateConnectionFields(ep, noAuth)) return;
        if (!ReadyToSave() || m_manualModelsRequested) {
            if (m_fetchInFlight) return;
            if (m_manualModelsRequested) {
                m_manualMode = true;
                SetModelStatus("Typed model IDs: the connection hasn't been checked.", StatusTone::Neutral);
                if (!m_rawMode) ToggleRawEditor();
                SetFetchStatus("Type one model ID per line, then Show model list to pick the one to use.");
                UpdateSaveButtons();
                return;
            }
            m_connectPending = true;
            wxCommandEvent e;
            OnFetchModels(e);
            return;
        }
        if (m_rawMode) {
            ToggleRawEditor();
            SetFetchStatus("Highlight the checked model to use, then Save and use. Save keeps the list without switching.");
            return;
        }
        const std::string selectedId = SelectedRowId();
        const int row = m_modelList->GetSelection();
        if (selectedId.empty() || !m_modelList->IsChecked((unsigned)row)) {
            SetFetchStatus("Highlight a checked model to use. Save keeps the list without switching."); return;
        }
        if (std::none_of(ep.models.begin(), ep.models.end(),
                [&](const auto& model) { return model.id == selectedId && model.showInPicker; })) {
            SetFetchStatus("Select a checked model from the list before saving."); return;
        }
        if (!CommitEndpoint(ep, noAuth)) return;
        m_modelToUse = "remote:" + ep.id + "/" + selectedId;
        EndModal(wxID_OK);
    }

    // Footer buttons follow the state: Connect before the first check,
    // then Save + "Save and use <model>" (needs a highlighted checked row).
    void UpdateSaveButtons()
    {
        if (!m_finishBtn) return;
        const bool connecting = m_connectPending && m_fetchInFlight;
        if (!ReadyToSave()) {
            m_saveBtn->Hide();
            m_finishBtn->SetLabel(connecting ? "Connecting..." : IsChatGpt() && !ChatGptSignedIn()
                                                  ? "Continue with ChatGPT" : "Connect");
            m_finishBtn->Enable(!m_fetchInFlight && !m_signInAttempt);
            m_finishBtn->SetToolTip(wxString());
        } else {
            m_saveBtn->Show();
            m_saveBtn->Enable(!connecting);
            wxString label = "Save and use model";
            bool usable = m_rawMode;
            const int row = m_modelList ? m_modelList->GetSelection() : wxNOT_FOUND;
            if (!m_rawMode && row >= 0 && row < (int)m_rowModelIds.size() && m_modelList->IsChecked((unsigned)row)) {
                usable = true;
                for (const auto& m : m_models) {
                    if (m.id != m_rowModelIds[(size_t)row]) continue;
                    wxString name = wxString::FromUTF8(m.displayName.empty() ? m.id : m.displayName);
                    if (name.length() > 26) name = name.Left(25) + wxString::FromUTF8("\xE2\x80\xA6");
                    label = "Save and use " + name;
                    break;
                }
            }
            m_finishBtn->SetLabel(label);
            m_finishBtn->Enable(usable && !connecting);
            m_finishBtn->SetToolTip(usable ? wxString()
                : wxString("Highlight a checked model to enable. Save keeps the list without switching."));
        }
        if (GetSizer()) Layout();
    }

    void LoadModelOptions()
    {
        const int row = m_modelList->GetSelection();
        const bool valid = !m_rawMode && row >= 0 && row < (int)m_rowModelIds.size();
        const bool checked = valid && m_modelList->IsChecked((unsigned)row);
        const std::string selectedId = valid ? m_rowModelIds[(size_t)row] : std::string();
        if (m_optionsModelId != selectedId) {
            m_optionsModelId = selectedId;
            InvalidateTest();
        }
        UpdateSaveButtons();
        m_loadingOptions = true;
        m_modelNameField->Enable(checked);
        m_ctxField->Enable(checked);
        m_allowTools->Enable(checked);
        m_imageOutput->Enable(checked && !IsChatGpt());
        m_testBtn->Enable(checked && !m_testInFlight);
        m_selectedModelLabel->SetLabel(valid ? wxString::FromUTF8(selectedId)
            : wxString("SELECTED MODEL"));
        m_modelNameField->ChangeValue(wxEmptyString);
        m_ctxField->ChangeValue(wxEmptyString);
        m_allowTools->SetValue(true);
        m_imageOutput->SetValue(false);
        std::string ctxHint;
        if (valid) {
            for (const auto& model : m_models) {
                if (model.id != selectedId || !model.showInPicker) continue;
                m_modelNameField->ChangeValue(wxString::FromUTF8(model.displayName));
                m_ctxField->ChangeValue(wxString::FromUTF8(FormatContextTokens(model.contextLimit)));
                m_allowTools->SetValue(!model.noTools);
                m_imageOutput->SetValue(model.imageOutput);
                m_allowTools->Enable(checked && !model.imageOutput);
                break;
            }
            const auto detected = m_fetchedCtx.find(selectedId);
            if (detected != m_fetchedCtx.end())
                ctxHint = (UsingNoAuth() ? "Read from the server: " : "Provider lists ") +
                          FormatContextTokens(detected->second) + ".";
            if (!checked) ctxHint = "Tick the model to set its options.";
        }
        if (ctxHint.empty() && checked)
            ctxHint = UsingNoAuth() ? "Empty uses the 256K remote default. Set the server's window."
                                    : "Empty uses the 256K default, a cost ceiling.";
        m_ctxHint->SetLabel(wxString::FromUTF8(ctxHint));
        m_ctxHint->SetForegroundColour(m_theme->textMuted);
        m_loadingOptions = false;

        const std::string base = TrimUtf8(m_baseUrlField->GetValue());
        const bool viaResponses = valid && DetectPreset(base) == kPresetOpenAI &&
            !m_imageOutput->GetValue() &&
            lb_responses::IsResponsesPath(lb_responses::ResolveChatPath(
                base, TrimUtf8(m_chatPathField->GetValue()), selectedId));
        m_compatibilityHint->SetLabel(IsChatGpt()
            ? "Runs through OpenAI Responses: tools and reasoning work together. Image generation isn't available here."
            : viaResponses
            ? "Routed through OpenAI Responses: tools and reasoning work together at any /think level."
            : "Agent tools are offered when Agent mode is on. Image models use a separate, tools-free request.");
        m_compatibilityHint->Wrap(std::max(FromDIP(520), m_rightPane->GetClientSize().x - FromDIP(40)));
        m_rightPane->Layout();
    }

    void SaveModelOptions()
    {
        if (m_loadingOptions) return;
        const int row = m_modelList->GetSelection();
        if (row < 0 || row >= (int)m_rowModelIds.size() || !m_modelList->IsChecked((unsigned)row)) return;
        int ctx = 0;
        const bool ctxOk = ParseContextTokens(TrimUtf8(m_ctxField->GetValue()), ctx);
        for (auto& model : m_models) {
            if (model.id != m_rowModelIds[(size_t)row]) continue;
            model.displayName = TrimUtf8(m_modelNameField->GetValue());
            if (model.displayName.empty()) model.displayName = PrettyNameFromId(model.id);
            model.noTools = !m_allowTools->GetValue();
            model.imageOutput = m_imageOutput->GetValue();
            if (ctxOk) model.contextLimit = ctx;
            m_allowTools->Enable(!model.imageOutput);
        }
        if (!ctxOk) {
            m_ctxHint->SetLabel("Use a number from 4096 to 2M, such as 131072 or 128K.");
            m_ctxHint->SetForegroundColour(m_theme->stopButton);
        } else {
            m_ctxHint->SetForegroundColour(m_theme->textMuted);
        }
        m_modelsField->ChangeValue(ModelsToText(m_models));
        InvalidateTest();
        UpdateSaveButtons();
    }

    // ── Members ─────────────────────────────────────────────────
    const bool m_isNew;               // Add mode: id follows the display name
    bool m_editMode = false;          // existing provider: starts connected
    const ThemeData* m_theme = nullptr;
    EndpointStore* m_endpointStore = nullptr;
    SecretsStore* m_secretsStore = nullptr;
    std::vector<SecretsStore::ConnectionRow> m_connections;
    std::vector<std::pair<std::string, std::string>> m_extraHeaders;

    int  m_preset = kPresetOpenRouter;
    bool m_connected = false;          // Add mode: a catalog was loaded for these settings
    bool m_manualMode = false;         // "Type model IDs without connecting"
    bool m_connectPending = false;
    bool m_manualModelsRequested = false;
    bool m_idEditedByUser = false;
    bool m_replacingKey = false;
    bool m_keyVerified = false;
    bool m_loadingOptions = false;

    std::vector<EndpointStore::Model> m_models;
    std::string m_optionsModelId;
    std::string m_modelToUse;
    std::string m_originalDialect;
    std::string m_seededExistingId;
    std::string m_statusText;

    // Provider row.
    wxPanel* m_presetTrack = nullptr;
    std::vector<wxButton*> m_presetBtns;

    // Left pane.
    wxScrolledWindow* m_leftPane = nullptr;
    wxPanel*      m_scanGroup = nullptr;
    wxPanel*      m_scanList  = nullptr;
    wxStaticText* m_scanHint  = nullptr;
    wxButton*     m_scanBtn   = nullptr;
    std::vector<wxRadioButton*> m_scanRadios;
    std::vector<LocalServerInfo> m_scanResults;
    bool          m_scanInFlight = false;
    unsigned      m_scanGeneration = 0;
    wxStaticText* m_baseUrlLabel = nullptr;
    wxTextCtrl*   m_baseUrlField = nullptr;
    wxPanel*      m_retryPanel = nullptr;
    wxStaticText* m_retryTitle = nullptr;
    wxStaticText* m_retryBody  = nullptr;
    wxCheckBox*   m_keepChecking = nullptr;
    wxTimer       m_retryTimer;
    wxTimer       m_rescanTimer;     // quiet port re-check while nothing is found
    wxPanel*      m_keyGroup = nullptr;
    wxPanel*      m_keyCard  = nullptr;
    wxStaticText* m_keyCardTitle  = nullptr;
    wxStaticText* m_keyCardMasked = nullptr;
    wxStaticText* m_keyCardDetail = nullptr;
    wxButton*     m_replaceKeyBtn = nullptr;
    wxPanel*      m_keyEntry = nullptr;
    wxTextCtrl*   m_apiKeyField = nullptr;
    wxCheckBox*   m_showKeyBox  = nullptr;
    wxButton*     m_getKeyBtn   = nullptr;
    wxPanel*      m_chatgptPanel      = nullptr;
    wxButton*     m_chatgptSignInBtn  = nullptr;
    wxButton*     m_chatgptSignOutBtn = nullptr;
    wxStaticText* m_chatgptStatus     = nullptr;
    std::string   m_chatgptAccount;              // issued client id, "" = none
    std::shared_ptr<lb_chatgpt::SignInAttempt> m_signInAttempt;
    unsigned      m_signInGeneration = 0;
    wxTextCtrl*   m_nameField = nullptr;
    wxCollapsiblePane* m_advancedPane = nullptr;
    wxFlexGridSizer*   m_advGrid = nullptr;
    wxTextCtrl*    m_idField       = nullptr;
    wxTextCtrl*    m_chatPathField = nullptr;
    wxComboBox*    m_providerField = nullptr;
    wxComboBox*    m_keyField      = nullptr;
    wxRadioButton* m_bearerRadio   = nullptr;
    wxRadioButton* m_xApiKeyRadio  = nullptr;
    wxRadioButton* m_noAuthRadio   = nullptr;
    wxRadioButton* m_nativeRadio   = nullptr;
    wxRadioButton* m_xmlRadio      = nullptr;
    wxChoice*      m_dialectChoice = nullptr;
    wxStaticText*  m_resolvedUrlLabel = nullptr;
    wxButton*      m_manualModelsBtn = nullptr;
    bool           m_rebuildingKeys = false;

    // Right pane.
    wxPanel*        m_rightPane = nullptr;
    wxStaticText*   m_modelConnectionStatus = nullptr;
    wxTextCtrl*     m_filterField  = nullptr;
    wxButton*       m_fetchBtn     = nullptr;
    wxCheckListBox* m_modelList    = nullptr;
    wxTextCtrl*     m_modelsField  = nullptr;
    wxStaticText*   m_modelCountLabel = nullptr;
    wxButton*       m_rawToggleBtn = nullptr;
    bool            m_rawMode      = false;
    wxStaticText*   m_selectedModelLabel = nullptr;
    wxTextCtrl*     m_modelNameField = nullptr;
    wxTextCtrl*     m_ctxField = nullptr;
    wxStaticText*   m_ctxHint  = nullptr;
    wxCheckBox*     m_allowTools  = nullptr;
    wxCheckBox*     m_imageOutput = nullptr;
    wxStaticText*   m_compatibilityHint = nullptr;
    wxButton*       m_testBtn = nullptr;
    wxChoice*       m_testEffort = nullptr;
    wxStaticText*   m_testResult = nullptr;   // chat half of the test result
    wxStaticText*   m_testTools  = nullptr;   // tool half, coloured on its own
    wxStaticText*   m_testHelp = nullptr;

    // Footer.
    wxStaticText* m_fetchStatus   = nullptr;
    wxButton*     m_statusDetails = nullptr;
    wxButton*     m_cancelBtn = nullptr;
    wxButton*     m_saveBtn   = nullptr;
    wxButton*     m_finishBtn = nullptr;

    std::vector<wxStaticText*> m_hints;
    std::vector<wxStaticText*> m_sectionLabels;

    // Model list state.
    std::vector<std::string> m_rowModelIds;   // visible row -> model id
    std::vector<std::string> m_fetchedIds;    // last catalog (sorted)
    std::map<std::string, std::string> m_fetchedNames;
    std::map<std::string, int> m_fetchedCtx;
    bool m_syncingModels = false;
    bool m_rawModelsDirty = false;
    bool m_fetchInFlight = false;
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
        if (ep.authScheme == EndpointStore::AuthScheme::ChatGpt) {
            lb_chatgpt::AccountSummary account;
            const bool known = !ep.chatgptAccount.empty() &&
                lb_chatgpt::Auth::Get().FindAccount(ep.chatgptAccount, account);
            std::string label = "ChatGPT: sign in needed";
            if (known && account.signedIn && account.planEnabled)
                label = "ChatGPT: " + (account.email.empty() ? std::string("signed in") : account.email);
            else if (known && account.signedIn)
                label = "ChatGPT: plan use not allowed";
            m_list->SetItem(item, 1, wxString::FromUTF8(label));
        } else {
            const bool hasKey = m_secretsStore && !m_secretsStore->GetSecret(ep.secretProvider, ep.secretKey).empty();
            m_list->SetItem(item, 1, noAuth ? "Not required" : hasKey ? "Saved" : "Needs API key");
        }

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
    if (provider != "openrouter" && provider != "openai" && provider != "custom" &&
        provider != "chatgpt" && provider != "local") return false;
    const int preset = provider == "openai" ? kPresetOpenAI : provider == "openrouter" ? kPresetOpenRouter :
                       provider == "chatgpt" ? kPresetChatGpt : provider == "local" ? kPresetLocal : kPresetCustom;
    auto seed = MakeProviderPreset(preset);
    if (provider == "custom") { seed.id = "custom"; seed.displayName = "Custom provider"; seed.secretProvider = "custom"; }
    const auto* existing = store->FindEndpoint(seed.id);
    const bool isNew = existing == nullptr;
    if (existing) seed = *existing;
    EndpointEditDialog dlg(parent, isNew ? "Add AI provider" : "Set up AI provider", seed,
        secrets, theme, isNew, store, wxString::FromUTF8(modelSearch), isNew ? preset : -1);
    if (dlg.ShowModal() != wxID_OK) return false;
    if (modelToUse) *modelToUse = dlg.GetModelToUse();
    return true;
}

void EndpointsDialog::OnAdd(wxCommandEvent&)
{
    if (!m_store) return;
    // Add opens on Local server, which scans this PC straight away; the
    // provider row is one click from OpenRouter, OpenAI and the rest.
    auto seed = MakeProviderPreset(kPresetLocal);
    if (const auto* existing = m_store->FindEndpoint(seed.id)) seed = *existing;
    EndpointEditDialog dlg(this, "Add AI provider", seed, m_secretsStore, *m_theme, true, m_store,
                           wxEmptyString, kPresetLocal);
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
    const auto* target = m_store->FindEndpoint(id);
    const bool chatgpt = target && target->authScheme == EndpointStore::AuthScheme::ChatGpt &&
                         !target->chatgptAccount.empty();
    wxString question = wxString::Format("Delete endpoint '%s'?", wxString::FromUTF8(id.c_str()));
    if (chatgpt) question += "\n\nThis also signs LlamaBoss out of ChatGPT on this computer.";
    int ans = wxMessageBox(question, "Confirm", wxYES_NO | wxICON_WARNING, this);
    if (ans != wxYES) return;

    const auto* existing = m_store->FindEndpoint(id);
    if (!existing) return;
    const auto previous = *existing;
    m_store->RemoveEndpoint(id);
    if (!m_store->Save()) {
        m_store->UpsertEndpoint(previous);
        wxMessageBox("Could not save the deletion. The connection was restored.", "Save failed", wxOK | wxICON_ERROR, this);
    } else if (chatgpt) {
        // Only when no other connection still uses the same sign-in.
        bool shared = false;
        for (const auto& other : m_store->Endpoints())
            if (other.authScheme == EndpointStore::AuthScheme::ChatGpt &&
                other.chatgptAccount == previous.chatgptAccount) shared = true;
        if (!shared) lb_chatgpt::Auth::Get().SignOut(previous.chatgptAccount);
    }
    RebuildList();
    UpdateButtonState();
}

void EndpointsDialog::OnClose(wxCommandEvent&)
{
    EndModal(wxID_CLOSE);
}
