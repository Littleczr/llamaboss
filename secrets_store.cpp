// secrets_store.cpp

#include "secrets_store.h"
#include "chatgpt_auth_core.h"   // lb_chatgpt::Base64UrlEncode / Base64UrlDecode

#include "lb_windows.h"
#ifdef _WIN32
#include <wincrypt.h>
#else
#include "secrets_backend.h"
#include <openssl/evp.h>
#include <openssl/rand.h>
#endif
#pragma comment(lib, "crypt32.lib")

#include <wx/datetime.h>

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

// ── DPAPI at-rest protection (file version 2) ────────────────────
// Plaintext secrets.json was readable by the approval-free read/grep
// tools and by auto-run PowerShell (Get-Content), and web_fetch_url
// could then send the keys anywhere without an approval card.  The
// whole v1 body is now encrypted with DPAPI (current-user scope), the
// same protection chatgpt_auth.cpp already uses.  Decrypting needs a
// CryptUnprotectData call, which only approval-gated paths (py,
// PowerShell with [type]:: / parentheses) can make.
const char kSecretsEntropy[] = "LlamaBoss.Secrets.v2";
constexpr int kEncryptedFileVersion = 2;

#ifdef _WIN32
bool ProtectSecretsBody(const std::string& plain, std::string& encOut)
{
    encOut.clear();
    DATA_BLOB in{ static_cast<DWORD>(plain.size()),
                  reinterpret_cast<BYTE*>(const_cast<char*>(plain.data())) };
    DATA_BLOB entropy{ static_cast<DWORD>(sizeof(kSecretsEntropy) - 1),
                       reinterpret_cast<BYTE*>(const_cast<char*>(kSecretsEntropy)) };
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"LlamaBoss secrets", &entropy, nullptr,
                          nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return false;
    encOut = lb_chatgpt::Base64UrlEncode(
        reinterpret_cast<const std::uint8_t*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return !encOut.empty();
}

bool UnprotectSecretsBody(const std::string& enc, std::string& plainOut)
{
    plainOut.clear();
    std::string blob;
    if (!lb_chatgpt::Base64UrlDecode(enc, blob) || blob.empty()) return false;
    DATA_BLOB in{ static_cast<DWORD>(blob.size()),
                  reinterpret_cast<BYTE*>(&blob[0]) };
    DATA_BLOB entropy{ static_cast<DWORD>(sizeof(kSecretsEntropy) - 1),
                       reinterpret_cast<BYTE*>(const_cast<char*>(kSecretsEntropy)) };
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out))
        return false;
    plainOut.assign(reinterpret_cast<const char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
}

#else
// macOS: AES-256-GCM with a random per-user key held in the login
// Keychain (readable only by this app's signature without a prompt).
// Blob layout: 12-byte nonce | ciphertext | 16-byte tag, base64url'd.
const char kSecretsKeyAccount[] = "secrets-file-key";
constexpr const char* kProtectionLabel = "keychain-aes256gcm";

bool SecretsKey(std::string& key, bool create)
{
    if (auto existing = secrets_backend::Get(kSecretsKeyAccount)) {
        if (existing->size() == 32) { key = *existing; return true; }
    }
    if (!create) return false;
    key.assign(32, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char*>(&key[0]), 32) != 1) return false;
    return secrets_backend::Set(kSecretsKeyAccount, key);
}

bool ProtectSecretsBody(const std::string& plain, std::string& encOut)
{
    encOut.clear();
    std::string key;
    if (!SecretsKey(key, /*create=*/true)) return false;

    std::string blob(12 + plain.size() + 16, '\0');
    auto* nonce = reinterpret_cast<unsigned char*>(&blob[0]);
    auto* cipher = nonce + 12;
    if (RAND_bytes(nonce, 12) != 1) return false;

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    int len = 0, total = 0;
    bool ok = ctx &&
        EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr,
                           reinterpret_cast<const unsigned char*>(key.data()), nonce) == 1 &&
        EVP_EncryptUpdate(ctx, nullptr, &len,
                          reinterpret_cast<const unsigned char*>(kSecretsEntropy),
                          static_cast<int>(sizeof(kSecretsEntropy) - 1)) == 1 &&
        EVP_EncryptUpdate(ctx, cipher, &len,
                          reinterpret_cast<const unsigned char*>(plain.data()),
                          static_cast<int>(plain.size())) == 1;
    total = len;
    ok = ok && EVP_EncryptFinal_ex(ctx, cipher + total, &len) == 1;
    total += len;
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, cipher + total) == 1;
    EVP_CIPHER_CTX_free(ctx);
    std::fill(key.begin(), key.end(), '\0');
    if (!ok) return false;

    encOut = lb_chatgpt::Base64UrlEncode(
        reinterpret_cast<const std::uint8_t*>(blob.data()), blob.size());
    return !encOut.empty();
}

bool UnprotectSecretsBody(const std::string& enc, std::string& plainOut)
{
    plainOut.clear();
    std::string blob;
    if (!lb_chatgpt::Base64UrlDecode(enc, blob) || blob.size() < 12 + 16) return false;
    std::string key;
    if (!SecretsKey(key, /*create=*/false)) return false;

    const auto* nonce = reinterpret_cast<const unsigned char*>(blob.data());
    const auto* cipher = nonce + 12;
    const int cipherLen = static_cast<int>(blob.size() - 12 - 16);
    std::string tag = blob.substr(blob.size() - 16);
    std::string plain(static_cast<size_t>(cipherLen), '\0');

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    int len = 0, total = 0;
    bool ok = ctx &&
        EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr,
                           reinterpret_cast<const unsigned char*>(key.data()), nonce) == 1 &&
        EVP_DecryptUpdate(ctx, nullptr, &len,
                          reinterpret_cast<const unsigned char*>(kSecretsEntropy),
                          static_cast<int>(sizeof(kSecretsEntropy) - 1)) == 1 &&
        EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char*>(&plain[0]), &len,
                          cipher, cipherLen) == 1;
    total = len;
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, &tag[0]) == 1 &&
         EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(&plain[0]) + total, &len) == 1;
    EVP_CIPHER_CTX_free(ctx);
    std::fill(key.begin(), key.end(), '\0');
    if (!ok) return false;
    plainOut = std::move(plain);
    return true;
}
#endif

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
    m_loadFailed = false;
    m_loadError.clear();
    m_loadBackupPath.clear();

    wxString path = wxString::FromUTF8(GetSecretsFilePath().c_str());
    if (!wxFileExists(path)) {
        // No file is fine — empty store.  Save() creates it lazily.
        return true;
    }

    // A present file that can't be read or parsed is NOT an empty store:
    // saving the in-memory state over it would silently erase every key
    // it holds.  Record the failure, keep a copy, and block Save() until
    // a reload succeeds or the user explicitly starts fresh.
    auto fail = [&](const std::string& why) {
        m_providers.clear();
        m_loadFailed = true;
        m_loadError = why;
        m_loadBackupPath = BackupUnreadableFile(path);
        wxLogWarning("SecretsStore: %s; saving is disabled until this is resolved.",
                     wxString::FromUTF8(why));
        return false;
    };

    std::string body;
    if (!ReadWholeFile(path, body))
        return fail("secrets.json could not be read");
    if (body.empty()) return true;

    bool wasPlaintext = false;
    try {
        Poco::JSON::Parser parser;
        auto val = parser.parse(body);
        auto root = val.extract<Poco::JSON::Object::Ptr>();
        if (!root) return fail("secrets.json is not a JSON object");

        // Version 2: the v1 document is DPAPI-encrypted inside "data".
        if (root->has("data")) {
            std::string plain;
            if (!UnprotectSecretsBody(root->getValue<std::string>("data"), plain))
                return fail("secrets.json could not be decrypted (was it "
                            "created by another Windows user or on another PC?)");
            Poco::JSON::Parser inner;
            auto innerVal = inner.parse(plain);
            wxSecureZeroMemory(&plain[0], plain.size());
            root = innerVal.extract<Poco::JSON::Object::Ptr>();
            if (!root) return fail("secrets.json decrypted to a non-object");
        } else {
            wasPlaintext = true;
        }

        if (!root->has("providers")) return true;  // no providers, fine
        auto providers = root->getObject("providers");
        if (!providers) return fail("secrets.json has a malformed \"providers\" section");

        std::vector<std::string> names;
        providers->getNames(names);
        for (const auto& name : names) {
            auto sub = providers->getObject(name);
            if (!sub) return fail("secrets.json has a malformed entry for \"" + name + "\"");

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

        // One-time migration: re-save a legacy plaintext file encrypted.
        // Best effort: a failed save leaves the plaintext file in place
        // and the keys loaded; the next successful Save() encrypts it.
        if (wasPlaintext && !m_providers.empty() && !Save()) {
            wxLogWarning("SecretsStore: could not re-save secrets.json "
                         "encrypted; it remains plaintext for now.");
        }
        return true;
    }
    catch (const std::exception& e) {
        return fail(std::string("secrets.json could not be parsed (") + e.what() + ")");
    }
}

void SecretsStore::ResetAfterFailedLoad()
{
    // Explicit user choice: keep the current in-memory keys and let the
    // next Save() replace the unreadable file (a copy was kept if possible).
    m_loadFailed = false;
    m_loadError.clear();
}

bool SecretsStore::Save()
{
    // Never overwrite a file that failed to load (see Load()).
    if (m_loadFailed) return false;

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
            // Poco::JSON::Parser, which fails on it and would lead to the
            // quote characters being stored as part of the secret value.
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

    // Encrypt the whole v1 document.  Fail closed: never fall back to
    // writing plaintext if DPAPI is unavailable.
    std::string plain = body.str();
    std::string enc;
    const bool protectedOk = ProtectSecretsBody(plain, enc);
    if (!plain.empty()) wxSecureZeroMemory(&plain[0], plain.size());
    if (!protectedOk) {
        wxLogWarning("SecretsStore: DPAPI encryption failed; secrets.json was not written.");
        return false;
    }

    Poco::JSON::Object::Ptr outer = new Poco::JSON::Object(true);
    outer->set("version", kEncryptedFileVersion);
#ifdef _WIN32
    outer->set("protection", "dpapi-current-user");
#else
    outer->set("protection", kProtectionLabel);
#endif
    outer->set("note", "LlamaBoss Connection keys, encrypted for this Windows "
                       "user (DPAPI). Edit them in LlamaBoss > Connections; "
                       "this file is useless on another account or PC.");
    outer->set("data", enc);

    std::ostringstream outBody;
    Poco::JSON::Stringifier::stringify(outer, outBody, 2);

    wxString path = wxString::FromUTF8(GetSecretsFilePath().c_str());
    return WriteWholeFileAtomic(path, outBody.str());
}

// ─── Per-secret access ──────────────────────────────────────────

bool SecretsStore::HasSecret(const std::string& provider,
                             const std::string& key) const
{
    auto pit = m_providers.find(provider);
    if (pit == m_providers.end()) return false;
    return pit->second.find(key) != pit->second.end();
}

bool SecretsStore::TryGetSecretEntry(const std::string& provider,
                                     const std::string& key,
                                     SecretEntry& out) const
{
    out = SecretEntry();

    auto pit = m_providers.find(provider);
    if (pit == m_providers.end()) return false;

    auto kit = pit->second.find(key);
    if (kit == pit->second.end()) return false;

    const std::string& jsonText = kit->second;

    std::string envName;
    if (TryParseEnvRefJson(jsonText, envName)) {
        out.isEnvRef = true;
        out.value = std::move(envName);
        return true;
    }

    std::string decodedString;
    if (TryDecodeJsonStringLiteral(jsonText, decodedString)) {
        out.isEnvRef = false;
        out.value = std::move(decodedString);
        return true;
    }

    return false;
}

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

    // Object path: must be {"$env": "VAR"}.
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
    std::set<std::string> emittedEnvNames;

    for (const auto& [provider, kvs] : m_providers) {
        for (const auto& [k, _] : kvs) {
            std::string value = GetSecret(provider, k);
            if (value.empty()) continue;  // skip unresolved $env refs

            std::string envName = BuildInjectedEnvName(provider, k);
            if (envName.empty()) continue;

            if (!emittedEnvNames.insert(envName).second) {
                wxLogWarning("SecretsStore: duplicate injected env var '%s' from %s.%s skipped.",
                             envName.c_str(), provider.c_str(), k.c_str());
                continue;
            }

            out.emplace_back(std::move(envName), std::move(value));
        }
    }
    return out;
}
