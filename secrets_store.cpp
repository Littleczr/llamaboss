// secrets_store.cpp
#define _CRT_SECURE_NO_WARNINGS

#include "secrets_store.h"

#include <wx/wx.h>
#include <wx/stdpaths.h>
#include <wx/filename.h>
#include <wx/dir.h>
#include <wx/log.h>

#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Stringifier.h>
#include <Poco/Dynamic/Var.h>

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace {

std::string TrimWhitespace(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string StripOneMatchingQuotePair(std::string s)
{
    s = TrimWhitespace(s);
    if (s.size() >= 2) {
        const char first = s.front();
        const char last  = s.back();
        if ((first == '"' && last == '"') ||
            (first == '\'' && last == '\'')) {
            s = s.substr(1, s.size() - 2);
        }
    }
    return TrimWhitespace(s);
}

// Read whole file UTF-8.  Returns empty string on any error.
std::string ReadWholeFile(const wxString& path)
{
    std::ifstream f(path.fn_str(), std::ios::in | std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Atomic-ish write: write to <path>.tmp then rename over <path>.
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
    // wxRenameFile overwrites on Windows when the third arg is true.
    return wxRenameFile(tmpPath, path, true);
}

// Accept common forms users may paste into the env-ref field:
//   SMARTSHEET_ACCESS_TOKEN
//   %SMARTSHEET_ACCESS_TOKEN%
//   $env:SMARTSHEET_ACCESS_TOKEN
// The stored JSON remains the canonical bare name.
std::string NormalizeEnvRefName(std::string envVarName)
{
    envVarName = StripOneMatchingQuotePair(std::move(envVarName));

    std::string lowered = envVarName;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    constexpr const char* psPrefix = "$env:";
    constexpr size_t psPrefixLen = 5;
    if (lowered.rfind(psPrefix, 0) == 0) {
        envVarName = TrimWhitespace(envVarName.substr(psPrefixLen));
    }
    else if (lowered.rfind("env:", 0) == 0) {
        envVarName = TrimWhitespace(envVarName.substr(4));
    }

    if (envVarName.size() >= 2 && envVarName.front() == '%' &&
        envVarName.back() == '%') {
        envVarName = TrimWhitespace(envVarName.substr(1, envVarName.size() - 2));
    }

    return envVarName;
}

bool TryParseEnvRefJson(const std::string& jsonText,
                        std::string&       envNameOut)
{
    envNameOut.clear();

    try {
        Poco::JSON::Parser p;
        auto v = p.parse(jsonText);
        auto obj = v.extract<Poco::JSON::Object::Ptr>();
        if (!obj || !obj->has("$env")) return false;

        envNameOut = NormalizeEnvRefName(obj->getValue<std::string>("$env"));
        return !envNameOut.empty();
    }
    catch (...) {
        envNameOut.clear();
        return false;
    }
}

std::string EnvNameComponent(const std::string& s)
{
    std::string out;
    out.reserve(s.size());

    bool lastWasUnderscore = false;
    for (unsigned char ch : s) {
        char c = static_cast<char>(ch);
        if (std::isalnum(ch)) {
            out.push_back(static_cast<char>(std::toupper(ch)));
            lastWasUnderscore = false;
        }
        else if (c == '_') {
            if (!lastWasUnderscore && !out.empty()) {
                out.push_back('_');
                lastWasUnderscore = true;
            }
        }
        else {
            if (!lastWasUnderscore && !out.empty()) {
                out.push_back('_');
                lastWasUnderscore = true;
            }
        }
    }

    while (!out.empty() && out.back() == '_') out.pop_back();
    return out;
}

std::string BuildInjectedEnvName(const std::string& provider,
                                 const std::string& key)
{
    const std::string p = EnvNameComponent(provider);
    const std::string k = EnvNameComponent(key);
    if (p.empty() || k.empty()) return std::string();
    return p + "_" + k;
}

// POCO's JSON::Parser is used throughout this store for JSON objects
// such as {"$env":"NAME"}.  However, the parser does not reliably
// accept a bare top-level JSON string literal in this code path.  Direct
// secrets are intentionally stored in m_providers as JSON string text
// (for example: "abc123"), so decode them by wrapping the literal in
// a tiny object first.  This preserves normal JSON escaping while avoiding
// the old parse-failure fallback that wrote direct secrets back out as
// strings containing literal quote characters.
bool TryDecodeJsonStringLiteral(const std::string& jsonText,
                                std::string& decoded)
{
    decoded.clear();
    if (jsonText.size() < 2 ||
        jsonText.front() != '"' ||
        jsonText.back() != '"') {
        return false;
    }

    try {
        Poco::JSON::Parser p;
        const std::string wrapped = std::string("{\"value\":") +
                                    jsonText + "}";
        auto v = p.parse(wrapped);
        auto obj = v.extract<Poco::JSON::Object::Ptr>();
        if (!obj || !obj->has("value")) return false;
        decoded = obj->getValue<std::string>("value");
        return true;
    }
    catch (...) {
        decoded.clear();
        return false;
    }
}

// Secret values are often copied from docs, terminals, or generated
// snippets as "token" or 'token'.  Store the actual value, not one
// extra matching wrapper-quote pair.  This intentionally removes only
// one outer pair and leaves all interior characters untouched.
std::string NormalizeDirectSecretValue(std::string value)
{
    return StripOneMatchingQuotePair(std::move(value));
}

}  // namespace

// ─── Path resolution ────────────────────────────────────────────

std::string SecretsStore::GetSecretsFilePath()
{
    // wxStandardPaths::GetUserLocalDataDir() returns
    // %LOCALAPPDATA%\LlamaBoss when SetAppName("LlamaBoss") has been
    // called (MyApp::OnInit does this).  The directory is created
    // here if it doesn't exist; the file itself is created lazily
    // on first Save().
    wxString localData = wxStandardPaths::Get().GetUserLocalDataDir();
    if (!wxDirExists(localData)) {
        wxFileName::Mkdir(localData, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    }
    wxFileName fn(localData, "secrets.json");
    return std::string(fn.GetFullPath().ToUTF8().data());
}

// ─── Load / Save ────────────────────────────────────────────────

bool SecretsStore::Load()
{
    m_providers.clear();
    m_loaded = true;

    wxString path = wxString::FromUTF8(GetSecretsFilePath().c_str());
    if (!wxFileExists(path)) {
        // No file is fine — empty store.  Save() creates it lazily.
        return true;
    }

    std::string body = ReadWholeFile(path);
    if (body.empty()) return true;

    try {
        Poco::JSON::Parser parser;
        auto val = parser.parse(body);
        auto root = val.extract<Poco::JSON::Object::Ptr>();
        if (!root) return false;

        auto providers = root->getObject("providers");
        if (!providers) return true;  // empty providers, fine

        std::vector<std::string> names;
        providers->getNames(names);
        for (const auto& name : names) {
            auto sub = providers->getObject(name);
            if (!sub) continue;

            std::map<std::string, std::string> kvs;
            std::vector<std::string> keys;
            sub->getNames(keys);
            for (const auto& k : keys) {
                Poco::Dynamic::Var v = sub->get(k);
                std::ostringstream out;
                Poco::JSON::Stringifier::stringify(v, out);
                kvs[k] = out.str();
            }
            if (!kvs.empty())
                m_providers[name] = std::move(kvs);
        }
        return true;
    }
    catch (const std::exception& e) {
        wxLogWarning("SecretsStore: failed to parse secrets.json (%s); "
                     "starting empty.", e.what());
        m_providers.clear();
        return false;
    }
}

bool SecretsStore::Save()
{
    Poco::JSON::Object::Ptr root = new Poco::JSON::Object(true);
    root->set("version", 1);

    Poco::JSON::Object::Ptr providers = new Poco::JSON::Object(true);
    for (const auto& [name, kvs] : m_providers) {
        if (kvs.empty()) continue;

        Poco::JSON::Object::Ptr sub = new Poco::JSON::Object(true);
        for (const auto& [k, jsonText] : kvs) {
            // jsonText is either a quoted direct-secret string or a
            // {"$env":"X"} object.  Decode direct strings explicitly
            // instead of handing a bare top-level JSON string to
            // Poco::JSON::Parser; that path previously failed and then
            // stored the quote characters as part of the secret value.
            //
            // The stored value is already canonical (NormalizeDirectSecretValue
            // ran at the SetSecret input boundary), so write the decoded value
            // back verbatim.  Re-normalizing here would silently strip an outer
            // quote pair from a value that legitimately contains one, mutating
            // hand-edited or loaded secrets on every round-trip.
            std::string decodedString;
            if (TryDecodeJsonStringLiteral(jsonText, decodedString)) {
                sub->set(k, std::move(decodedString));
                continue;
            }

            try {
                Poco::JSON::Parser p;
                sub->set(k, p.parse(jsonText));
            }
            catch (...) {
                // Last-resort: store as a plain string.
                sub->set(k, jsonText);
            }
        }
        providers->set(name, sub);
    }
    root->set("providers", providers);

    std::ostringstream body;
    Poco::JSON::Stringifier::stringify(root, body, 2);

    wxString path = wxString::FromUTF8(GetSecretsFilePath().c_str());
    return WriteWholeFileAtomic(path, body.str());
}

// ─── Per-secret access ──────────────────────────────────────────

std::string SecretsStore::GetSecret(const std::string& provider,
                                    const std::string& key) const
{
    auto pit = m_providers.find(provider);
    if (pit == m_providers.end()) return {};
    auto kit = pit->second.find(key);
    if (kit == pit->second.end()) return {};

    const std::string& jsonText = kit->second;

    // Fast path: direct-secret JSON string literal.  Decode via the
    // same wrapper-object trick used by Save(); a bare top-level JSON
    // string is not a reliable Poco::JSON::Parser input here.
    //
    // Return the decoded value as-is.  Quote-pair normalization happens
    // once, at the SetSecret input boundary; re-stripping here would
    // diverge the resolved value from what is stored on disk for any
    // secret that genuinely begins and ends with a matching quote.
    std::string decodedString;
    if (TryDecodeJsonStringLiteral(jsonText, decodedString)) {
        return decodedString;
    }

    // Object path: must be {"$env": "VAR"} (Phase 2 may add $file).
    std::string envName;
    if (TryParseEnvRefJson(jsonText, envName)) {
        const char* env = std::getenv(envName.c_str());
        return env ? std::string(env) : std::string();
    }

    return {};
}

void SecretsStore::SetSecret(const std::string& provider,
                             const std::string& key,
                             const std::string& value)
{
    // Encode as a JSON string literal.  Accept a single accidental
    // matching wrapper-quote pair from pasted values such as "token".
    const std::string normalized = NormalizeDirectSecretValue(value);
    std::ostringstream out;
    Poco::JSON::Stringifier::stringify(Poco::Dynamic::Var(normalized), out);
    m_providers[provider][key] = out.str();
}

void SecretsStore::SetSecretEnvRef(const std::string& provider,
                                   const std::string& key,
                                   const std::string& envVarName)
{
    Poco::JSON::Object::Ptr ref = new Poco::JSON::Object(true);
    ref->set("$env", NormalizeEnvRefName(envVarName));
    std::ostringstream out;
    Poco::JSON::Stringifier::stringify(ref, out);
    m_providers[provider][key] = out.str();
}

void SecretsStore::RemoveSecret(const std::string& provider,
                                const std::string& key)
{
    auto pit = m_providers.find(provider);
    if (pit == m_providers.end()) return;
    pit->second.erase(key);
    if (pit->second.empty())
        m_providers.erase(pit);
}

void SecretsStore::RemoveProvider(const std::string& provider)
{
    m_providers.erase(provider);
}

// ─── UI helpers ─────────────────────────────────────────────────

std::vector<SecretsStore::ConnectionRow>
SecretsStore::ListConnections() const
{
    std::vector<ConnectionRow> rows;
    for (const auto& [provider, kvs] : m_providers) {
        for (const auto& [k, jsonText] : kvs) {
            ConnectionRow r;
            r.provider = provider;
            r.key = k;

            std::string envName;
            r.isEnvRef = TryParseEnvRefJson(jsonText, envName);
            if (r.isEnvRef) {
                r.displayHint = "$env:" + envName;
            } else {
                r.displayHint = "••••••••";
            }
            rows.push_back(std::move(r));
        }
    }
    return rows;
}

// ─── Worker integration ─────────────────────────────────────────

std::vector<std::pair<std::string, std::string>>
SecretsStore::BuildEnvInjections() const
{
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& [provider, kvs] : m_providers) {
        for (const auto& [k, _] : kvs) {
            std::string value = GetSecret(provider, k);
            if (value.empty()) continue;  // skip unresolved $env refs

            std::string envName = BuildInjectedEnvName(provider, k);
            if (envName.empty()) continue;

            out.emplace_back(std::move(envName), std::move(value));
        }
    }
    return out;
}
