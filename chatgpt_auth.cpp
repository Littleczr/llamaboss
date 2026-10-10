// chatgpt_auth.cpp
//
// See chatgpt_auth.h for the contract.  Protocol details follow the
// "ChatGPT plan usage" open-source client docs (developers.openai.com/siwc):
// registration and sign-in, accounts and sessions, token reference, and
// errors and recovery.  Pure helpers live in chatgpt_auth_core.h.

#include "chatgpt_auth.h"
#include "chatgpt_auth_core.h"
#include "lb_ssl.h"

#ifdef _WIN32
#include "lb_windows.h"
#include <wincrypt.h>
#include <bcrypt.h>
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "bcrypt.lib")
#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif
#else
#include <random>
#if defined(LB_CHATGPT_TEST_OPENSSL)
bool LbTestRsaSha256Verify(const std::string& n, const std::string& e,
                           const std::string& msg, const std::string& sig);
#endif
#endif

#include <Poco/Net/ServerSocket.h>
#include <Poco/Net/SocketAddress.h>
#include <Poco/Net/StreamSocket.h>

#include <ctime>

namespace lb_chatgpt {

// ═════════════════════════════════════════════════════════════════
//  Internal types
// ═════════════════════════════════════════════════════════════════

struct Record {
    std::string clientId;
    std::string email;
    std::string name;
    std::string subject;
    std::string scopes;          // space-separated, as granted
    std::string idToken;         // kept for id_token_hint on re-sign-in
    std::string accessToken;
    std::string refreshToken;
    long long   expiresAt = 0;          // epoch seconds
    long long   earliestRefreshAt = 0;  // epoch seconds, 0 = none
    long long   savedAt = 0;
};

struct AuthImpl {
    mutable std::mutex data;     // records, hostId, filePath
    std::mutex refresh;          // serializes refresh-token rotation
    std::mutex jwks;             // guards the JWKS cache below
    std::string dataDir;
    std::string filePath;
    std::string hostId;
    std::vector<Record> records;
    Poco::JSON::Array::Ptr jwksKeys;
    long long jwksFetchedAt = 0;
};

struct SignInAttempt {
    std::atomic<bool> cancelled{false};
    bool isNew = true;
    std::string clientId;           // dynamic_agent_client or the issued id
    std::string expectedSubject;    // re-sign-in: must stay the same account
    std::string state;
    std::string nonce;
    std::string verifier;
    std::string redirectUri;
    Poco::Net::ServerSocket listener;
};

namespace {

constexpr int kHttpTimeoutSeconds = 20;
constexpr int kSignInTimeoutSeconds = 10 * 60;
constexpr long long kRefreshMarginSeconds = 120;

long long NowEpoch() { return static_cast<long long>(std::time(nullptr)); }

// ── Randomness ───────────────────────────────────────────────────
std::string RandomBytes(std::size_t n)
{
    std::string out(n, '\0');
#ifdef _WIN32
    if (!NT_SUCCESS(BCryptGenRandom(nullptr,
            reinterpret_cast<PUCHAR>(&out[0]), static_cast<ULONG>(n),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        throw std::runtime_error("The system random number generator failed.");
#else
    std::random_device rd;   // non-Windows builds exist only for unit tests
    for (auto& c : out) c = static_cast<char>(rd() & 0xff);
#endif
    return out;
}

std::string RandomToken() { return Base64UrlEncode(RandomBytes(32)); }

std::string NewHostId()
{
    std::string b = RandomBytes(16);
    b[6] = static_cast<char>((b[6] & 0x0f) | 0x40);   // version 4
    b[8] = static_cast<char>((b[8] & 0x3f) | 0x80);   // RFC 4122 variant
    static const char* hex = "0123456789abcdef";
    std::string s = "urn:uuid:";
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) s += '-';
        const auto v = static_cast<unsigned char>(b[i]);
        s += hex[v >> 4];
        s += hex[v & 15];
    }
    return s;
}

// ── DPAPI ────────────────────────────────────────────────────────
// Encrypted values are stored base64url.  An empty secret stays empty.
const char kEntropy[] = "LlamaBoss.ChatGPT.credentials.v1";

std::string Protect(const std::string& plain)
{
    if (plain.empty()) return std::string();
#ifdef _WIN32
    DATA_BLOB in{ static_cast<DWORD>(plain.size()),
                  reinterpret_cast<BYTE*>(const_cast<char*>(plain.data())) };
    DATA_BLOB entropy{ static_cast<DWORD>(sizeof(kEntropy) - 1),
                       reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy)) };
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"LlamaBoss ChatGPT sign-in", &entropy,
                          nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return std::string();
    std::string enc = Base64UrlEncode(out.pbData, out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return enc;
#elif defined(__APPLE__)
    // No protected storage on macOS yet: refuse rather than write tokens
    // in recoverable form.  Sign-in is reported unavailable up front.
    return std::string();
#else
    return "plain:" + Base64UrlEncode(plain);   // unit-test builds only
#endif
}

// Returns empty when the value cannot be decrypted (another Windows
// user, another machine, corrupted file).  The caller then treats the
// account as signed out rather than failing the whole load.
std::string Unprotect(const std::string& stored)
{
    if (stored.empty()) return std::string();
#ifdef _WIN32
    std::string blob;
    if (!Base64UrlDecode(stored, blob) || blob.empty()) return std::string();
    DATA_BLOB in{ static_cast<DWORD>(blob.size()),
                  reinterpret_cast<BYTE*>(&blob[0]) };
    DATA_BLOB entropy{ static_cast<DWORD>(sizeof(kEntropy) - 1),
                       reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy)) };
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out))
        return std::string();
    std::string plain(reinterpret_cast<const char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return plain;
#elif defined(__APPLE__)
    return std::string();
#else
    if (stored.rfind("plain:", 0) != 0) return std::string();
    std::string plain;
    return Base64UrlDecode(stored.substr(6), plain) ? plain : std::string();
#endif
}

// ── RS256 signature check (BCrypt) ───────────────────────────────
bool RsaSha256Verify(const std::string& modulus, const std::string& exponent,
                     const std::string& signedBytes, const std::string& signature)
{
#ifdef _WIN32
    std::string n = modulus;
    while (n.size() > 1 && n[0] == '\0') n.erase(0, 1);
    std::string e = exponent;
    while (e.size() > 1 && e[0] == '\0') e.erase(0, 1);
    if (n.size() < 256 || n.size() > 1024 || e.empty() || e.size() > 8) return false;

    std::vector<unsigned char> blob(sizeof(BCRYPT_RSAKEY_BLOB) + e.size() + n.size());
    auto* hdr = reinterpret_cast<BCRYPT_RSAKEY_BLOB*>(blob.data());
    hdr->Magic       = BCRYPT_RSAPUBLIC_MAGIC;
    hdr->BitLength   = static_cast<ULONG>(n.size() * 8);
    hdr->cbPublicExp = static_cast<ULONG>(e.size());
    hdr->cbModulus   = static_cast<ULONG>(n.size());
    hdr->cbPrime1    = 0;
    hdr->cbPrime2    = 0;
    std::memcpy(blob.data() + sizeof(BCRYPT_RSAKEY_BLOB), e.data(), e.size());
    std::memcpy(blob.data() + sizeof(BCRYPT_RSAKEY_BLOB) + e.size(), n.data(), n.size());

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    bool ok = false;
    if (NT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_RSA_ALGORITHM, nullptr, 0)) &&
        NT_SUCCESS(BCryptImportKeyPair(alg, nullptr, BCRYPT_RSAPUBLIC_BLOB, &key,
                                       blob.data(), static_cast<ULONG>(blob.size()), 0))) {
        auto digest = Sha256(signedBytes);
        BCRYPT_PKCS1_PADDING_INFO pad{ BCRYPT_SHA256_ALGORITHM };
        ok = NT_SUCCESS(BCryptVerifySignature(key, &pad, digest.data(),
                static_cast<ULONG>(digest.size()),
                reinterpret_cast<PUCHAR>(const_cast<char*>(signature.data())),
                static_cast<ULONG>(signature.size()), BCRYPT_PAD_PKCS1));
    }
    if (key) BCryptDestroyKey(key);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
#elif defined(LB_CHATGPT_TEST_OPENSSL)
    // Unit-test builds on Linux only (never defined for LlamaBoss).
    return LbTestRsaSha256Verify(modulus, exponent, signedBytes, signature);
#else
    (void)modulus; (void)exponent; (void)signedBytes; (void)signature;
    return false;
#endif
}

// ── HTTP (worker threads only) ───────────────────────────────────
struct HttpResult {
    int status = 0;          // 0 = transport failure
    std::string body;
    std::string error;       // transport failure text
};

HttpResult HttpCall(const std::string& method, const std::string& url,
                    const std::string& contentType, const std::string& body,
                    const std::vector<std::pair<std::string, std::string>>& headers,
                    std::size_t maxBody = 1024 * 1024)
{
    HttpResult r;
    try {
        Poco::URI uri(url);
        const bool tls = uri.getScheme() == "https";
        if (tls) lb::EnsureSSLInitialized();
        std::unique_ptr<Poco::Net::HTTPClientSession> sess;
        if (tls) sess.reset(new Poco::Net::HTTPSClientSession(uri.getHost(), uri.getPort()));
        else     sess.reset(new Poco::Net::HTTPClientSession(uri.getHost(), uri.getPort()));
        sess->setTimeout(Poco::Timespan(kHttpTimeoutSeconds, 0));

        std::string path = uri.getPathAndQuery();
        if (path.empty()) path = "/";
        Poco::Net::HTTPRequest req(method, path, Poco::Net::HTTPMessage::HTTP_1_1);
        req.set("User-Agent", "LlamaBoss/1.0");
        req.set("Accept", "application/json");
        for (const auto& h : headers) req.set(h.first, h.second);
        if (method == Poco::Net::HTTPRequest::HTTP_POST) {
            req.setContentType(contentType);
            req.setContentLength(static_cast<std::streamsize>(body.size()));
            sess->sendRequest(req) << body;
        } else {
            sess->sendRequest(req);
        }
        Poco::Net::HTTPResponse resp;
        std::istream& in = sess->receiveResponse(resp);
        r.status = static_cast<int>(resp.getStatus());
        char buf[8192];
        while (in.good() && r.body.size() < maxBody) {
            in.read(buf, sizeof(buf));
            const std::streamsize got = in.gcount();
            if (got <= 0) break;
            r.body.append(buf, static_cast<std::size_t>(got));
        }
    } catch (const Poco::Exception& e) {
        r.status = 0;
        r.error = e.displayText();
    } catch (const std::exception& e) {
        r.status = 0;
        r.error = e.what();
    }
    return r;
}

HttpResult PostForm(const std::string& url,
                    const std::vector<std::pair<std::string, std::string>>& form)
{
    return HttpCall(Poco::Net::HTTPRequest::HTTP_POST, url,
                    "application/x-www-form-urlencoded", BuildQuery(form), {});
}

// OAuth errors arrive as {"error":"invalid_grant","error_description":...}
// (RFC 6749) or as {"error":{"code":...,"message":...}}; accept both.
void ReadOAuthError(const std::string& body, std::string& code, std::string& description)
{
    code.clear();
    description.clear();
    auto root = ParseObject(body);
    if (!root) return;
    code = OptString(root, "error");
    description = OptString(root, "error_description");
    if (code.empty()) {
        try {
            if (auto err = root->getObject("error")) {
                code = OptString(err, "code");
                if (code.empty()) code = OptString(err, "type");
                description = OptString(err, "message");
            }
        } catch (...) {}
    }
    if (description.size() > 300) description.resize(300);
}

bool IsUnusableRefreshError(const std::string& code)
{
    return code == "invalid_grant" || code == "invalid_refresh_token" ||
           code == "token_expired" || code == "refresh_token_expired" ||
           code == "refresh_token_invalidated" || code == "refresh_token_reused";
}

// Some servers send earliest_refresh_at in milliseconds.
long long NormalizeEpoch(long long v)
{
    return v > 100000000000LL ? v / 1000 : v;
}

// ── Files ────────────────────────────────────────────────────────
std::filesystem::path PathFromUtf8(const std::string& s)
{
#if defined(__cpp_char8_t)
    return std::filesystem::path(reinterpret_cast<const char8_t*>(s.c_str()));
#else
    return std::filesystem::u8path(s);
#endif
}

bool ReadWholeFile(const std::string& path, std::string& out)
{
    out.clear();
    std::ifstream f(PathFromUtf8(path), std::ios::in | std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    if (f.bad()) return false;
    out = ss.str();
    return true;
}

bool WriteWholeFileAtomic(const std::string& path, const std::string& body)
{
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(PathFromUtf8(tmp), std::ios::out | std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(body.data(), static_cast<std::streamsize>(body.size()));
        f.flush();
        if (!f) return false;
    }
    std::error_code ec;
    std::filesystem::rename(PathFromUtf8(tmp), PathFromUtf8(path), ec);  // replaces on Windows (MoveFileEx)
    if (ec) {
        std::filesystem::remove(PathFromUtf8(tmp), ec);
        return false;
    }
    return true;
}

// ── Loopback callback page ───────────────────────────────────────
std::string HtmlEscape(const std::string& s)
{
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default:  out += c;
        }
    }
    return out;
}

void SendPage(Poco::Net::StreamSocket& sock, int status, bool ok, const std::string& message)
{
    const std::string title = ok ? "LlamaBoss is connected to ChatGPT" : "LlamaBoss sign-in didn't finish";
    std::string html =
        "<!doctype html><html><head><meta charset=\"utf-8\"><title>" + HtmlEscape(title) +
        "</title><style>body{font-family:Consolas,Menlo,monospace;background:#1e1e1e;color:#e6e6e6;"
        "display:flex;align-items:center;justify-content:center;height:100vh;margin:0}"
        "main{max-width:560px;padding:24px;border:1px solid #444}h1{font-size:18px;margin:0 0 12px}"
        "p{line-height:1.5;margin:0}</style></head><body><main><h1>" +
        std::string(ok ? "&#10003; " : "&#10007; ") + HtmlEscape(title) + "</h1><p>" +
        HtmlEscape(message) + "</p></main></body></html>";
    std::string head = "HTTP/1.1 " + std::to_string(status) + (status == 200 ? " OK" : " Not Found") +
        "\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\n"
        "Connection: close\r\nContent-Length: " + std::to_string(html.size()) + "\r\n\r\n";
    try {
        const std::string all = head + html;
        sock.sendBytes(all.data(), static_cast<int>(all.size()));
        sock.shutdownSend();
    } catch (...) {}
}

// Reads one HTTP request head (bounded, short timeout).
bool ReadRequestHead(Poco::Net::StreamSocket& sock, std::string& head)
{
    head.clear();
    try {
        sock.setReceiveTimeout(Poco::Timespan(5, 0));
        char buf[2048];
        while (head.size() < 16 * 1024) {
            const int got = sock.receiveBytes(buf, sizeof(buf));
            if (got <= 0) break;
            head.append(buf, static_cast<std::size_t>(got));
            if (head.find("\r\n\r\n") != std::string::npos) return true;
        }
    } catch (...) {}
    return head.find("\r\n") != std::string::npos;
}

} // namespace


// ═════════════════════════════════════════════════════════════════
//  Persistence.  Callers hold impl.data.
// ═════════════════════════════════════════════════════════════════

namespace {

Record* FindRecord(AuthImpl& impl, const std::string& clientId)
{
    for (auto& r : impl.records)
        if (r.clientId == clientId) return &r;
    return nullptr;
}

AccountSummary Summarize(const Record& r)
{
    AccountSummary s;
    s.clientId = r.clientId;
    s.email = r.email;
    s.name = r.name;
    s.signedIn = !r.refreshToken.empty();
    s.planEnabled = HasScope(r.scopes, kPlanScope());
    return s;
}

bool SaveLocked(const AuthImpl& impl)
{
    if (impl.filePath.empty()) return false;
    Poco::JSON::Object::Ptr root = new Poco::JSON::Object(true);
    root->set("version", 1);
    root->set("note", "ChatGPT sign-in for LlamaBoss. Tokens are encrypted for this Windows user (DPAPI). "
                      "Delete this file to sign out everywhere on this computer.");
    root->set("host_id", impl.hostId);
    Poco::JSON::Array::Ptr arr = new Poco::JSON::Array;
    for (const auto& r : impl.records) {
        Poco::JSON::Object::Ptr o = new Poco::JSON::Object(true);
        o->set("client_id", r.clientId);
        o->set("email", r.email);
        o->set("name", r.name);
        o->set("subject", r.subject);
        o->set("scopes", r.scopes);
        o->set("expires_at", static_cast<Poco::Int64>(r.expiresAt));
        o->set("earliest_refresh_at", static_cast<Poco::Int64>(r.earliestRefreshAt));
        o->set("saved_at", static_cast<Poco::Int64>(r.savedAt));
        o->set("id_token_enc", Protect(r.idToken));
        o->set("access_token_enc", Protect(r.accessToken));
        o->set("refresh_token_enc", Protect(r.refreshToken));
        arr->add(o);
    }
    root->set("accounts", arr);
    std::ostringstream body;
    Poco::JSON::Stringifier::stringify(root, body, 2);
    return WriteWholeFileAtomic(impl.filePath, body.str());
}

// A missing or unreadable file starts empty.  Nothing in it is
// irreplaceable: the worst case is signing in again (the host id is
// regenerated, which ChatGPT simply treats as a new computer).  An
// unreadable file is kept aside rather than overwritten.
void LoadLocked(AuthImpl& impl)
{
    impl.records.clear();
    impl.hostId.clear();
    std::string body;
    std::error_code ec;
    if (!std::filesystem::exists(PathFromUtf8(impl.filePath), ec)) return;
    auto root = ReadWholeFile(impl.filePath, body) ? ParseObject(body) : Poco::JSON::Object::Ptr();
    if (!root) {
        std::filesystem::rename(PathFromUtf8(impl.filePath),
                                PathFromUtf8(impl.filePath + ".unreadable.bak"), ec);
        return;
    }
    impl.hostId = OptString(root, "host_id");
    Poco::JSON::Array::Ptr arr;
    try { arr = root->getArray("accounts"); } catch (...) {}
    if (!arr) return;
    for (std::size_t i = 0; i < arr->size(); ++i) {
        Poco::JSON::Object::Ptr o;
        try { o = arr->getObject(static_cast<unsigned>(i)); } catch (...) {}
        if (!o) continue;
        Record r;
        r.clientId = OptString(o, "client_id");
        if (r.clientId.empty() || r.clientId == kDynamicClientId()) continue;
        r.email = OptString(o, "email");
        r.name = OptString(o, "name");
        r.subject = OptString(o, "subject");
        r.scopes = OptString(o, "scopes");
        r.expiresAt = OptInt(o, "expires_at", 0);
        r.earliestRefreshAt = OptInt(o, "earliest_refresh_at", 0);
        r.savedAt = OptInt(o, "saved_at", 0);
        r.idToken = Unprotect(OptString(o, "id_token_enc"));
        r.accessToken = Unprotect(OptString(o, "access_token_enc"));
        r.refreshToken = Unprotect(OptString(o, "refresh_token_enc"));
        if (r.refreshToken.empty()) r.accessToken.clear();   // undecryptable = signed out
        impl.records.push_back(std::move(r));
    }
}

// ═════════════════════════════════════════════════════════════════
//  ID token verification (worker threads)
// ═════════════════════════════════════════════════════════════════

Poco::JSON::Object::Ptr FindJwk(AuthImpl& impl, const std::string& kid, std::string& error)
{
    std::lock_guard<std::mutex> lock(impl.jwks);
    auto search = [&]() -> Poco::JSON::Object::Ptr {
        if (!impl.jwksKeys) return Poco::JSON::Object::Ptr();
        for (std::size_t i = 0; i < impl.jwksKeys->size(); ++i) {
            Poco::JSON::Object::Ptr k;
            try { k = impl.jwksKeys->getObject(static_cast<unsigned>(i)); } catch (...) {}
            if (k && OptString(k, "kid") == kid && OptString(k, "kty") == "RSA") return k;
        }
        return Poco::JSON::Object::Ptr();
    };
    if (auto k = search()) return k;
    // Unknown kid: OpenAI rotated keys, or nothing is cached yet.  Fetch
    // at most once a minute so a bad token cannot hammer the endpoint.
    if (impl.jwksKeys && NowEpoch() - impl.jwksFetchedAt < 60) {
        error = "the ID token was signed with an unknown key";
        return Poco::JSON::Object::Ptr();
    }
    const HttpResult r = HttpCall(Poco::Net::HTTPRequest::HTTP_GET, kJwksUrl(), "", "", {});
    if (r.status != 200) {
        error = "OpenAI's signing keys could not be downloaded (" +
                (r.status ? "HTTP " + std::to_string(r.status) : r.error) + ")";
        return Poco::JSON::Object::Ptr();
    }
    auto root = ParseObject(r.body);
    Poco::JSON::Array::Ptr keys;
    try { if (root) keys = root->getArray("keys"); } catch (...) {}
    if (!keys) { error = "OpenAI's signing key list was malformed"; return Poco::JSON::Object::Ptr(); }
    impl.jwksKeys = keys;
    impl.jwksFetchedAt = NowEpoch();
    if (auto k = search()) return k;
    error = "the ID token was signed with an unknown key";
    return Poco::JSON::Object::Ptr();
}

// Verifies signature + claims.  On success fills `claims`.
bool VerifyIdToken(AuthImpl& impl, const std::string& jwt, const std::string& clientId,
                   const std::string& nonce, Poco::JSON::Object::Ptr& claims, std::string& error)
{
    JwtParts parts;
    if (!SplitJwt(jwt, parts)) { error = "the ID token is malformed"; return false; }
    auto header = ParseObject(parts.headerJson);
    if (!header || OptString(header, "alg") != "RS256") {
        error = "the ID token uses an unexpected signature algorithm";
        return false;
    }
    auto jwk = FindJwk(impl, OptString(header, "kid"), error);
    if (!jwk) return false;
    std::string n, e;
    if (!Base64UrlDecode(OptString(jwk, "n"), n) || !Base64UrlDecode(OptString(jwk, "e"), e) ||
        !RsaSha256Verify(n, e, parts.signingInput, parts.signature)) {
        error = "the ID token signature did not verify";
        return false;
    }
    claims = ParseObject(parts.payloadJson);
    error = CheckIdTokenClaims(claims, clientId, nonce, NowEpoch());
    return error.empty();
}

} // namespace

// ═════════════════════════════════════════════════════════════════
//  Auth
// ═════════════════════════════════════════════════════════════════

Auth& Auth::Get()
{
    static Auth instance;
    return instance;
}

std::shared_ptr<AuthImpl> Auth::Impl() const
{
    std::lock_guard<std::mutex> lock(m_initMutex);
    return m_impl;
}

void Auth::Init(const std::string& dataDir)
{
    std::lock_guard<std::mutex> lock(m_initMutex);
    if (m_impl) return;
    auto impl = std::make_shared<AuthImpl>();
    impl->dataDir = dataDir;
    std::string path = dataDir;
    if (!path.empty() && path.back() != '\\' && path.back() != '/') path += '\\';
#ifndef _WIN32
    if (!path.empty() && path.back() == '\\') path.back() = '/';
#endif
    impl->filePath = path + "chatgpt_accounts.json";
    {
        std::lock_guard<std::mutex> dataLock(impl->data);
        LoadLocked(*impl);
    }
    m_impl = impl;
}

bool Auth::IsInitialized() const { return Impl() != nullptr; }

std::vector<AccountSummary> Auth::Accounts() const
{
    std::vector<AccountSummary> out;
    auto impl = Impl();
    if (!impl) return out;
    std::lock_guard<std::mutex> lock(impl->data);
    for (const auto& r : impl->records) out.push_back(Summarize(r));
    return out;
}

bool Auth::FindAccount(const std::string& clientId, AccountSummary& out) const
{
    auto impl = Impl();
    if (!impl || clientId.empty()) return false;
    std::lock_guard<std::mutex> lock(impl->data);
    const Record* r = FindRecord(*impl, clientId);
    if (!r) return false;
    out = Summarize(*r);
    return true;
}

std::shared_ptr<SignInAttempt> Auth::BeginSignIn(const std::string& reauthClientId,
                                                 std::string& authorizeUrl,
                                                 std::string& error)
{
    authorizeUrl.clear();
    error.clear();
#ifdef __APPLE__
    // Token protection and ID-token verification are Windows-only so far.
    error = "Sign in with ChatGPT is not available on macOS yet. "
            "Use an API key endpoint instead.";
    return nullptr;
#endif
    auto impl = Impl();
    if (!impl) { error = "ChatGPT sign-in storage is not ready. Restart LlamaBoss and try again."; return nullptr; }

    auto attempt = std::make_shared<SignInAttempt>();
    std::string idTokenHint, loginHint, hostId;
    {
        std::lock_guard<std::mutex> lock(impl->data);
        if (impl->hostId.empty()) {
            impl->hostId = NewHostId();
            if (!SaveLocked(*impl)) {
                error = "Could not write the ChatGPT sign-in file in the LlamaBoss data folder.";
                impl->hostId.clear();
                return nullptr;
            }
        }
        hostId = impl->hostId;
        const Record* r = reauthClientId.empty() ? nullptr : FindRecord(*impl, reauthClientId);
        if (r) {
            attempt->isNew = false;
            attempt->clientId = r->clientId;
            attempt->expectedSubject = r->subject;
            idTokenHint = r->idToken;
            loginHint = r->email;
        } else {
            attempt->isNew = true;
            attempt->clientId = kDynamicClientId();
        }
    }

    try {
        attempt->state = RandomToken();
        attempt->nonce = RandomToken();
        attempt->verifier = RandomToken();   // 43 chars: the PKCE minimum length
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }

    // Loopback only, never "localhost" (the docs require the literal
    // address).  SO_REUSEADDR stays off so a second process cannot share
    // the port.  Fall back to any free port: only the port may vary.
    unsigned short port = 0;
    for (unsigned short candidate : { kPreferredCallbackPort, static_cast<unsigned short>(0) }) {
        try {
            attempt->listener.bind(Poco::Net::SocketAddress("127.0.0.1", candidate), false, false);
            attempt->listener.listen(8);
            port = attempt->listener.address().port();
            break;
        } catch (const Poco::Exception&) {
            attempt->listener.close();
            attempt->listener = Poco::Net::ServerSocket();
        }
    }
    if (port == 0) {
        error = "Could not open a local port for the ChatGPT sign-in callback.";
        return nullptr;
    }
    attempt->redirectUri = "http://127.0.0.1:" + std::to_string(port) + kCallbackPath();

    auto build = [&](bool withIdTokenHint) {
        std::vector<std::pair<std::string, std::string>> q;
        q.emplace_back("client_id", attempt->clientId);
        if (attempt->isNew) q.emplace_back("agent_name_hint", kAgentNameHint());
        q.emplace_back("ext_agent_host_id", hostId);
        if (withIdTokenHint && !attempt->isNew && !idTokenHint.empty())
            q.emplace_back("id_token_hint", idTokenHint);
        if (!attempt->isNew && !loginHint.empty()) q.emplace_back("login_hint", loginHint);
        q.emplace_back("response_type", "code");
        q.emplace_back("redirect_uri", attempt->redirectUri);
        q.emplace_back("scope", kScopes());
        q.emplace_back("resource", kResource());
        q.emplace_back("state", attempt->state);
        q.emplace_back("nonce", attempt->nonce);
        q.emplace_back("code_challenge_method", "S256");
        q.emplace_back("code_challenge", PkceChallenge(attempt->verifier));
        return std::string(kAuthorizeUrl()) + "?" + BuildQuery(q);
    };
    // id_token_hint only skips the account picker; an ID token can be long
    // enough to push the URL past what ShellExecute reliably hands to a
    // browser (~2 KB).  login_hint still pre-selects the account.
    authorizeUrl = build(true);
    if (authorizeUrl.size() > 1900) authorizeUrl = build(false);
    return attempt;
}

void Auth::CancelSignIn(const std::shared_ptr<SignInAttempt>& attempt)
{
    if (attempt) attempt->cancelled.store(true);
}

SignInResult Auth::CompleteSignIn(const std::shared_ptr<SignInAttempt>& attempt)
{
    SignInResult result;
    auto impl = Impl();
    if (!attempt || !impl) { result.error = "No sign-in is in progress."; return result; }

    // ── 1. Wait for the browser ──────────────────────────────────
    std::map<std::string, std::string> query;
    Poco::Net::StreamSocket conn;
    bool gotCallback = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kSignInTimeoutSeconds);
    try {
        while (!attempt->cancelled.load()) {
            if (std::chrono::steady_clock::now() > deadline) break;
            if (!attempt->listener.poll(Poco::Timespan(0, 250 * 1000), Poco::Net::Socket::SELECT_READ))
                continue;
            Poco::Net::StreamSocket s = attempt->listener.acceptConnection();
            std::string head;
            if (!ReadRequestHead(s, head)) continue;
            if (!ParseCallbackRequestLine(head, query)) {
                // favicon.ico, a browser prefetch, a stray local request.
                SendPage(s, 404, false, "This address only accepts the ChatGPT sign-in callback.");
                continue;
            }
            conn = s;
            gotCallback = true;
            break;
        }
    } catch (const Poco::Exception& e) {
        result.error = "The sign-in callback listener failed: " + e.displayText();
    }
    try { attempt->listener.close(); } catch (...) {}
    if (attempt->cancelled.load()) {
        result.cancelled = true;
        if (gotCallback) SendPage(conn, 200, false, "Sign-in was cancelled in LlamaBoss. You can close this tab.");
        return result;
    }
    if (!gotCallback) {
        if (result.error.empty()) result.error = "Sign-in timed out. Choose Continue with ChatGPT to try again.";
        return result;
    }

    auto finish = [&](const std::string& error) {
        result.error = error;
        SendPage(conn, 200, false, error + " Return to LlamaBoss to try again.");
        return result;
    };

    // ── 2. Validate the callback ─────────────────────────────────
    const std::string state = query.count("state") ? query["state"] : std::string();
    if (state != attempt->state)
        return finish("The sign-in response didn't match this request.");
    if (query.count("error")) {
        const std::string err = query["error"];
        if (err == "access_denied")
            return finish("Sign-in was cancelled, or ChatGPT plan use wasn't allowed.");
        std::string desc = query.count("error_description") ? query["error_description"] : std::string();
        if (desc.size() > 200) desc.resize(200);
        return finish("ChatGPT sign-in failed (" + err + (desc.empty() ? "" : ": " + desc) + ").");
    }
    const std::string code = query.count("code") ? query["code"] : std::string();
    if (code.empty()) return finish("The sign-in response had no authorization code.");

    std::string clientId = attempt->clientId;
    const std::string returnedClient = query.count("client_id") ? query["client_id"] : std::string();
    if (attempt->isNew) {
        if (returnedClient.empty() || returnedClient == kDynamicClientId())
            return finish("ChatGPT didn't finish registering LlamaBoss (no client id was returned).");
        clientId = returnedClient;
    } else if (!returnedClient.empty() && returnedClient != attempt->clientId) {
        return finish("ChatGPT returned a different registration than the account being signed in.");
    }

    // ── 3. Exchange the code ─────────────────────────────────────
    const HttpResult tr = PostForm(kTokenUrl(), {
        { "grant_type", "authorization_code" },
        { "client_id", clientId },
        { "code", code },
        { "code_verifier", attempt->verifier },
        { "redirect_uri", attempt->redirectUri },
        { "resource", kResource() },
    });
    if (tr.status != 200) {
        std::string oc, od;
        ReadOAuthError(tr.body, oc, od);
        if (tr.status == 0) return finish("Couldn't reach ChatGPT to finish sign-in (" + tr.error + ").");
        return finish("ChatGPT rejected the sign-in (HTTP " + std::to_string(tr.status) +
                      (oc.empty() ? "" : ", " + oc) + (od.empty() ? "" : ": " + od) + ").");
    }
    auto tok = ParseObject(tr.body);
    const std::string access = OptString(tok, "access_token");
    const std::string refresh = OptString(tok, "refresh_token");
    const std::string idToken = OptString(tok, "id_token");
    if (access.empty() || refresh.empty() || idToken.empty())
        return finish("ChatGPT's token response was incomplete.");
    std::string scopes = OptString(tok, "scope");
    if (scopes.empty() && query.count("scope")) scopes = query["scope"];

    // ── 4. Verify identity ───────────────────────────────────────
    Poco::JSON::Object::Ptr claims;
    std::string verifyError;
    if (!VerifyIdToken(*impl, idToken, clientId, attempt->nonce, claims, verifyError))
        return finish("Sign-in couldn't be verified: " + verifyError + ".");
    const std::string subject = OptString(claims, "sub");
    if (!attempt->expectedSubject.empty() && subject != attempt->expectedSubject)
        return finish("You signed in to a different ChatGPT account than this connection uses. "
                      "Sign out of the connection first to switch accounts.");

    // ── 5. Save ──────────────────────────────────────────────────
    Record rec;
    rec.clientId = clientId;
    rec.subject = subject;
    rec.email = OptString(claims, "email");
    rec.name = OptString(claims, "name");
    rec.scopes = scopes;
    rec.idToken = idToken;
    rec.accessToken = access;
    rec.refreshToken = refresh;
    rec.savedAt = NowEpoch();
    rec.expiresAt = rec.savedAt + (std::max<long long>)(60, OptInt(tok, "expires_in", 3600));
    rec.earliestRefreshAt = NormalizeEpoch(OptInt(tok, "earliest_refresh_at", 0));
    bool saved = false;
    {
        std::lock_guard<std::mutex> lock(impl->data);
        Record* existing = FindRecord(*impl, clientId);
        if (existing) {
            if (rec.email.empty()) rec.email = existing->email;
            if (rec.name.empty()) rec.name = existing->name;
            *existing = rec;
        } else {
            impl->records.push_back(rec);
        }
        saved = SaveLocked(*impl);
    }
    // The account is usable for this session either way; say so honestly.
    if (!saved)
        return finish("Signed in, but the sign-in couldn't be saved in the LlamaBoss data folder, "
                      "so you'll need to sign in again after restarting.");

    result.ok = true;
    result.account = Summarize(rec);
    if (!result.account.planEnabled) {
        SendPage(conn, 200, true, "You're signed in, but ChatGPT plan use wasn't allowed, so LlamaBoss can't "
                                  "send requests on your plan yet. Return to LlamaBoss to try again.");
    } else {
        SendPage(conn, 200, true, "You can close this tab and return to LlamaBoss. Requests on this "
                                  "connection now use your ChatGPT plan.");
    }
    return result;
}

bool Auth::GetAccessToken(const std::string& clientId, std::string& accessToken, std::string& error)
{
    accessToken.clear();
    error.clear();
    auto impl = Impl();
    if (!impl) { error = "ChatGPT sign-in storage is not ready. Restart LlamaBoss and try again."; return false; }
    const std::string signInAgain =
        " Open Settings > Connections, edit ChatGPT plan, and choose Continue with ChatGPT.";

    // One refresh at a time, process-wide.  The data lock is only taken
    // for short reads/writes, never across the network call.
    std::lock_guard<std::mutex> refreshLock(impl->refresh);

    Record snapshot;
    {
        std::lock_guard<std::mutex> lock(impl->data);
        const Record* r = FindRecord(*impl, clientId);
        if (!r || r->refreshToken.empty()) {
            error = "This connection isn't signed in to ChatGPT." + signInAgain;
            return false;
        }
        if (!HasScope(r->scopes, kPlanScope())) {
            error = "ChatGPT plan use wasn't allowed when you signed in, so LlamaBoss can't use your plan." + signInAgain;
            return false;
        }
        snapshot = *r;
    }
    const long long now = NowEpoch();
    const bool accessValid = !snapshot.accessToken.empty() && now < snapshot.expiresAt - 5;
    if (accessValid && (now < snapshot.expiresAt - kRefreshMarginSeconds ||
                        now < snapshot.earliestRefreshAt)) {
        accessToken = snapshot.accessToken;
        return true;
    }

    const HttpResult r = PostForm(kTokenUrl(), {
        { "grant_type", "refresh_token" },
        { "client_id", clientId },
        { "refresh_token", snapshot.refreshToken },
        { "resource", kResource() },
    });
    if (r.status != 200) {
        std::string code, desc;
        ReadOAuthError(r.body, code, desc);
        if (IsUnusableRefreshError(code)) {
            std::lock_guard<std::mutex> lock(impl->data);
            if (Record* rec = FindRecord(*impl, clientId)) {
                if (rec->refreshToken == snapshot.refreshToken) {
                    rec->accessToken.clear();
                    rec->refreshToken.clear();
                    rec->expiresAt = 0;
                    SaveLocked(*impl);
                }
            }
            error = "Your ChatGPT sign-in has expired." + signInAgain;
            return false;
        }
        if (code == "invalid_client") {
            error = "ChatGPT no longer recognizes this LlamaBoss registration (invalid_client). "
                    "Sign out of the ChatGPT plan connection and sign in again.";
            return false;
        }
        // Transient (network, 5xx): an access token that hasn't actually
        // expired is still worth trying.
        if (accessValid) {
            accessToken = snapshot.accessToken;
            return true;
        }
        error = r.status == 0
            ? "Couldn't reach ChatGPT to renew the sign-in (" + r.error + ")."
            : "ChatGPT couldn't renew the sign-in (HTTP " + std::to_string(r.status) +
              (code.empty() ? "" : ", " + code) + "). Try again in a minute.";
        return false;
    }

    auto tok = ParseObject(r.body);
    const std::string access = OptString(tok, "access_token");
    if (access.empty()) {
        error = "ChatGPT's token renewal response had no access token. Try again.";
        return false;
    }
    const std::string newRefresh = OptString(tok, "refresh_token");
    const std::string newScopes = OptString(tok, "scope");
    const std::string newIdToken = OptString(tok, "id_token");
    std::string acceptedIdToken;
    if (!newIdToken.empty()) {
        Poco::JSON::Object::Ptr claims;
        std::string ignore;
        if (VerifyIdToken(*impl, newIdToken, clientId, std::string(), claims, ignore) &&
            OptString(claims, "sub") == snapshot.subject)
            acceptedIdToken = newIdToken;
    }
    {
        std::lock_guard<std::mutex> lock(impl->data);
        Record* rec = FindRecord(*impl, clientId);
        if (!rec) { error = "This ChatGPT connection was signed out."; return false; }
        rec->accessToken = access;
        // Rotation: replace the access token, expiry, scopes and the new
        // refresh token together.  The old refresh token is now dead.
        if (!newRefresh.empty()) rec->refreshToken = newRefresh;
        if (!newScopes.empty()) rec->scopes = newScopes;
        if (!acceptedIdToken.empty()) rec->idToken = acceptedIdToken;
        rec->savedAt = NowEpoch();
        rec->expiresAt = rec->savedAt + (std::max<long long>)(60, OptInt(tok, "expires_in", 3600));
        rec->earliestRefreshAt = NormalizeEpoch(OptInt(tok, "earliest_refresh_at", 0));
        SaveLocked(*impl);   // a failed write still leaves memory correct for this session
        if (!HasScope(rec->scopes, kPlanScope())) {
            error = "ChatGPT plan use is no longer allowed for LlamaBoss." + signInAgain;
            return false;
        }
    }
    accessToken = access;
    return true;
}

void Auth::InvalidateAccessToken(const std::string& clientId)
{
    auto impl = Impl();
    if (!impl) return;
    std::lock_guard<std::mutex> lock(impl->data);
    if (Record* r = FindRecord(*impl, clientId)) {
        r->accessToken.clear();
        r->expiresAt = 0;
        r->earliestRefreshAt = 0;
    }
}

void Auth::SignOut(const std::string& clientId)
{
    auto impl = Impl();
    if (!impl) return;
    std::string refresh;
    {
        std::lock_guard<std::mutex> lock(impl->data);
        Record* r = FindRecord(*impl, clientId);
        if (!r) return;
        refresh = r->refreshToken;
        r->accessToken.clear();
        r->refreshToken.clear();
        r->idToken.clear();          // the docs: omit id_token_hint after signing out
        r->expiresAt = 0;
        r->earliestRefreshAt = 0;
        SaveLocked(*impl);
    }
    if (refresh.empty()) return;
    std::thread([clientId, refresh]() {
        for (int attempt = 0; attempt < 3; ++attempt) {
            const HttpResult r = PostForm(kRevokeUrl(), {
                { "token", refresh },
                { "token_type_hint", "refresh_token" },
                { "client_id", clientId },
            });
            if (r.status == 200) return;
            std::this_thread::sleep_for(std::chrono::seconds(2 << attempt));
        }
    }).detach();
}

} // namespace lb_chatgpt
