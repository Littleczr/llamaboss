// chatgpt_auth_core.h
//
// Portable, side-effect-free pieces of "Sign in with ChatGPT" (ChatGPT
// plan usage for open-source clients).  Everything here is pure string
// and byte work -- no sockets, no files, no Windows APIs -- so it is
// shared by chatgpt_auth.cpp (the real flow) and unit-testable on its
// own.  The Windows-only parts (DPAPI, BCrypt RSA, random bytes) live
// in chatgpt_auth.cpp.
//
// Protocol reference (developers.openai.com/siwc, Oct 2026):
//   authorize   https://auth.openai.com/api/accounts/authorize
//   token       https://auth.openai.com/api/accounts/oauth/token
//   revoke      https://auth.openai.com/api/accounts/oauth/revoke
//   jwks        https://auth.openai.com/.well-known/jwks.json
//   inference   POST https://api.openai.com/v1/responses  (store:false, stream:true)
//   models      GET  https://api.openai.com/v1/models     ({"models":[{slug,display_name,visibility}]})
//
// Header-only so the existing Visual Studio project needs no new
// compiled item for it.
#pragma once

#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Object.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace lb_chatgpt {

// ── Fixed protocol constants ─────────────────────────────────────
inline const char* kIssuer()        { return "https://auth.openai.com"; }
inline const char* kAuthorizeUrl()  { return "https://auth.openai.com/api/accounts/authorize"; }
inline const char* kTokenUrl()      { return "https://auth.openai.com/api/accounts/oauth/token"; }
inline const char* kRevokeUrl()     { return "https://auth.openai.com/api/accounts/oauth/revoke"; }
inline const char* kJwksUrl()       { return "https://auth.openai.com/.well-known/jwks.json"; }
inline const char* kResource()      { return "https://api.openai.com/v1"; }
inline const char* kApiBaseUrl()    { return "https://api.openai.com"; }
inline const char* kResponsesPath() { return "/v1/responses"; }
inline const char* kModelsUrl()     { return "https://api.openai.com/v1/models"; }
inline const char* kCallbackPath()  { return "/auth/callback"; }
// First-time registration entry point.  NEVER saved or used for token
// exchange: the callback returns the real issued client id.
inline const char* kDynamicClientId() { return "dynamic_agent_client"; }
// Shown to the user on the consent screen; same on every install.
inline const char* kAgentNameHint() { return "LlamaBoss"; }
inline const char* kScopes()
{ return "openid profile email offline_access resource.invoke chatgpt.tokens.use.direct"; }
// The grant that actually authorizes plan usage.  A valid ID token
// alone does not.
inline const char* kPlanScope()     { return "chatgpt.tokens.use.direct"; }
// Preferred loopback port (same default the Codex CLI uses); any free
// port works -- only the port may vary between sign-ins.
constexpr unsigned short kPreferredCallbackPort = 1455;
// Function tools on the plan route must be grouped in a namespace.
inline const char* kToolNamespace() { return "llamaboss"; }

// ── SHA-256 (FIPS 180-4) ─────────────────────────────────────────
// Used for the PKCE S256 challenge.  Portable so the challenge can be
// tested against the RFC 7636 appendix B vector on any platform.
inline std::array<std::uint8_t, 32> Sha256(const std::string& data)
{
    static const std::uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
    std::uint32_t h[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                           0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    auto rotr = [](std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };

    std::string msg = data;
    const std::uint64_t bitLen = static_cast<std::uint64_t>(data.size()) * 8u;
    msg.push_back(static_cast<char>(0x80));
    while (msg.size() % 64 != 56) msg.push_back('\0');
    for (int i = 7; i >= 0; --i)
        msg.push_back(static_cast<char>((bitLen >> (i * 8)) & 0xff));

    for (std::size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            const auto* p = reinterpret_cast<const unsigned char*>(msg.data() + chunk + i * 4);
            w[i] = (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
                   (std::uint32_t(p[2]) << 8)  |  std::uint32_t(p[3]);
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
            const std::uint32_t s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3],
                      e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t t1 = hh + S1 + ch + k[i] + w[i];
            const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    std::array<std::uint8_t, 32> out{};
    for (int i = 0; i < 8; ++i) {
        out[i*4]   = static_cast<std::uint8_t>(h[i] >> 24);
        out[i*4+1] = static_cast<std::uint8_t>(h[i] >> 16);
        out[i*4+2] = static_cast<std::uint8_t>(h[i] >> 8);
        out[i*4+3] = static_cast<std::uint8_t>(h[i]);
    }
    return out;
}

// ── base64url (RFC 4648 §5, no padding) ──────────────────────────
inline std::string Base64UrlEncode(const std::uint8_t* data, std::size_t len)
{
    static const char* tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((len * 4 + 2) / 3);
    std::size_t i = 0;
    for (; i + 2 < len; i += 3) {
        const std::uint32_t v = (std::uint32_t(data[i]) << 16) |
                                (std::uint32_t(data[i+1]) << 8) | data[i+2];
        out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];  out += tbl[v & 63];
    }
    if (len - i == 1) {
        const std::uint32_t v = std::uint32_t(data[i]) << 16;
        out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63];
    } else if (len - i == 2) {
        const std::uint32_t v = (std::uint32_t(data[i]) << 16) | (std::uint32_t(data[i+1]) << 8);
        out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63];
    }
    return out;
}

inline std::string Base64UrlEncode(const std::string& bytes)
{
    return Base64UrlEncode(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
}

// Accepts both alphabets and optional '=' padding (JWKS values and JWT
// segments are unpadded base64url; be lenient about what servers send).
inline bool Base64UrlDecode(const std::string& in, std::string& out)
{
    out.clear();
    std::uint32_t buf = 0;
    int bits = 0;
    for (char c : in) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '-' || c == '+') v = 62;
        else if (c == '_' || c == '/') v = 63;
        else if (c == '=') break;
        else return false;
        buf = (buf << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buf >> bits) & 0xff));
        }
    }
    // A single leftover 6-bit group cannot encode a byte: malformed.
    return bits < 6;
}

// ── PKCE (RFC 7636, S256) ────────────────────────────────────────
inline std::string PkceChallenge(const std::string& verifier)
{
    const auto digest = Sha256(verifier);
    return Base64UrlEncode(digest.data(), digest.size());
}

// ── URL encoding ─────────────────────────────────────────────────
// Strict: only RFC 3986 unreserved characters pass through, so the
// same function is safe for query strings and form bodies.
inline std::string PercentEncode(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

// Query-string decoding: '+' is a space, %XX is a byte.  Malformed
// escapes are kept literally rather than rejected.
inline std::string PercentDecode(const std::string& s)
{
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '+') { out += ' '; continue; }
        if (c == '%' && i + 2 < s.size()) {
            const int hi = hexVal(s[i+1]), lo = hexVal(s[i+2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += c;
    }
    return out;
}

inline std::string BuildQuery(const std::vector<std::pair<std::string, std::string>>& params)
{
    std::string out;
    for (const auto& kv : params) {
        if (!out.empty()) out += '&';
        out += PercentEncode(kv.first);
        out += '=';
        out += PercentEncode(kv.second);
    }
    return out;
}

// First value wins for a repeated key: a callback must never be able to
// smuggle a second state/code past validation.
inline std::map<std::string, std::string> ParseQuery(const std::string& query)
{
    std::map<std::string, std::string> out;
    std::size_t pos = 0;
    while (pos <= query.size()) {
        std::size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        const std::string pair = query.substr(pos, amp - pos);
        if (!pair.empty()) {
            const std::size_t eq = pair.find('=');
            const std::string key = PercentDecode(pair.substr(0, eq));
            const std::string val = eq == std::string::npos ? std::string()
                                                            : PercentDecode(pair.substr(eq + 1));
            out.emplace(key, val);
        }
        pos = amp + 1;
    }
    return out;
}

// ── Loopback callback request ────────────────────────────────────
// Parses the request line of the browser's redirect:
//   GET /auth/callback?code=...&state=... HTTP/1.1
// Returns false unless it is a GET for exactly kCallbackPath().
inline bool ParseCallbackRequestLine(const std::string& requestHead,
                                     std::map<std::string, std::string>& query)
{
    query.clear();
    const std::size_t eol = requestHead.find("\r\n");
    const std::string line = requestHead.substr(0, eol);
    if (line.compare(0, 4, "GET ") != 0) return false;
    const std::size_t sp = line.find(' ', 4);
    if (sp == std::string::npos) return false;
    const std::string target = line.substr(4, sp - 4);
    const std::size_t q = target.find('?');
    const std::string path = target.substr(0, q);
    if (path != kCallbackPath()) return false;
    if (q != std::string::npos) query = ParseQuery(target.substr(q + 1));
    return true;
}

// ── Scopes ───────────────────────────────────────────────────────
inline bool HasScope(const std::string& spaceSeparated, const std::string& scope)
{
    std::size_t pos = 0;
    while (pos < spaceSeparated.size()) {
        while (pos < spaceSeparated.size() && spaceSeparated[pos] == ' ') ++pos;
        std::size_t end = spaceSeparated.find(' ', pos);
        if (end == std::string::npos) end = spaceSeparated.size();
        if (spaceSeparated.compare(pos, end - pos, scope) == 0 && end - pos == scope.size())
            return true;
        pos = end;
    }
    return false;
}

// ── JWT ──────────────────────────────────────────────────────────
struct JwtParts {
    std::string headerJson;
    std::string payloadJson;
    std::string signingInput;   // "<b64 header>.<b64 payload>", the signed bytes
    std::string signature;      // raw signature bytes
};

inline bool SplitJwt(const std::string& jwt, JwtParts& out)
{
    const std::size_t a = jwt.find('.');
    if (a == std::string::npos) return false;
    const std::size_t b = jwt.find('.', a + 1);
    if (b == std::string::npos || jwt.find('.', b + 1) != std::string::npos) return false;
    out.signingInput = jwt.substr(0, b);
    return Base64UrlDecode(jwt.substr(0, a), out.headerJson) &&
           Base64UrlDecode(jwt.substr(a + 1, b - a - 1), out.payloadJson) &&
           Base64UrlDecode(jwt.substr(b + 1), out.signature) &&
           !out.headerJson.empty() && !out.payloadJson.empty() && !out.signature.empty();
}

inline Poco::JSON::Object::Ptr ParseObject(const std::string& json)
{
    try {
        Poco::JSON::Parser parser;
        return parser.parse(json).extract<Poco::JSON::Object::Ptr>();
    } catch (...) {
        return Poco::JSON::Object::Ptr();
    }
}

inline std::string OptString(const Poco::JSON::Object::Ptr& obj, const char* key)
{
    try {
        if (obj && obj->has(key) && !obj->isNull(key) && obj->get(key).isString())
            return obj->getValue<std::string>(key);
    } catch (...) {}
    return std::string();
}

// Integer claim (exp, iat, expires_in, ...).  Accepts integer or
// integral-valued numbers; returns fallback when absent/invalid.
inline long long OptInt(const Poco::JSON::Object::Ptr& obj, const char* key, long long fallback)
{
    try {
        if (!obj || !obj->has(key) || obj->isNull(key)) return fallback;
        const auto v = obj->get(key);
        if (v.isInteger()) return v.convert<long long>();
        if (v.isNumeric()) return static_cast<long long>(v.convert<double>());
        if (v.isString()) {
            const std::string s = v.convert<std::string>();
            if (!s.empty() && s.find_first_not_of("0123456789") == std::string::npos)
                return std::stoll(s);
        }
    } catch (...) {}
    return fallback;
}

// ID-token claim checks, done after the signature is verified.
// Returns an empty string on success, else a short reason.
inline std::string CheckIdTokenClaims(const Poco::JSON::Object::Ptr& claims,
                                      const std::string& expectedClientId,
                                      const std::string& expectedNonce,
                                      long long nowEpoch)
{
    if (!claims) return "the ID token has no claims";
    if (OptString(claims, "iss") != kIssuer()) return "the ID token issuer is wrong";
    bool audOk = false;
    try {
        if (claims->has("aud")) {
            const auto aud = claims->get("aud");
            if (aud.isString()) {
                audOk = aud.convert<std::string>() == expectedClientId;
            } else if (auto arr = claims->getArray("aud")) {
                for (std::size_t i = 0; i < arr->size(); ++i)
                    if (arr->getElement<std::string>(static_cast<unsigned>(i)) == expectedClientId)
                        audOk = true;
            }
        }
    } catch (...) { audOk = false; }
    if (!audOk) return "the ID token was issued for a different client";
    constexpr long long kSkew = 300;   // tolerate a few minutes of clock drift
    const long long exp = OptInt(claims, "exp", -1);
    if (exp < 0 || exp + kSkew < nowEpoch) return "the ID token has expired";
    if (!expectedNonce.empty() && OptString(claims, "nonce") != expectedNonce)
        return "the ID token nonce does not match this sign-in";
    if (OptString(claims, "sub").empty()) return "the ID token has no subject";
    return std::string();
}

// ── Plan error vocabulary ────────────────────────────────────────
// Structured codes from the plan route (siwc errors-and-recovery).
// Returns an empty string for codes this table does not know.
inline std::string DescribePlanErrorCode(const std::string& code)
{
    if (code == "subscription_sharing_usage_limit_exceeded")
        return "You've reached the ChatGPT plan usage limit for LlamaBoss. Check usage under ChatGPT Settings > Usage, then try again later.";
    if (code == "subscription_sharing_usage_unavailable" ||
        code == "subscription_sharing_user_unavailable")
        return "ChatGPT plan usage is temporarily unavailable. Try again in a minute.";
    if (code == "subscription_sharing_user_not_eligible")
        return "This ChatGPT account or workspace isn't eligible to use its plan in other apps (Plus or Pro is required, and workspace policy can block it).";
    if (code == "subscription_sharing_unsupported_capability")
        return "The ChatGPT plan route doesn't support part of this request (model, input type or feature).";
    if (code == "subscription_sharing_route_not_supported")
        return "The ChatGPT plan route rejected this request type.";
    if (code == "subscription_sharing_invalid_user" ||
        code == "chatpass_v2_scope_not_authorized" ||
        code == "chatpass_v2_invalid_authorization_context")
        return "ChatGPT didn't accept LlamaBoss's sign-in for plan usage. Open Settings > Connections, edit ChatGPT plan, and choose Continue with ChatGPT again.";
    return std::string();
}

// Pre-stream HTTP failure on the plan route -> one readable sentence,
// with the provider's own message/code kept for diagnosis.
inline std::string DescribePlanHttpError(int status, const std::string& body)
{
    std::string code, message, param;
    if (auto root = ParseObject(body)) {
        Poco::JSON::Object::Ptr err;
        try { err = root->getObject("error"); } catch (...) {}
        if (err) {
            code = OptString(err, "code");
            message = OptString(err, "message");
            param = OptString(err, "param");
        }
    }
    std::string text = DescribePlanErrorCode(code);
    if (text.empty()) {
        if (status == 401)
            text = "ChatGPT sign-in was not accepted. Open Settings > Connections, edit ChatGPT plan, and choose Continue with ChatGPT again.";
        else if (status == 403)
            text = "ChatGPT plan usage was blocked by an account, workspace or region policy.";
        else if (status == 429)
            text = "ChatGPT plan rate limit reached. Wait a moment and try again.";
        else if (status == 503)
            text = "ChatGPT plan usage is temporarily unavailable. Try again in a minute.";
        else
            text = "ChatGPT plan request failed (HTTP " + std::to_string(status) + ").";
    }
    std::string detail;
    if (!message.empty()) detail = message;
    if (!code.empty()) detail += (detail.empty() ? "" : " ") + std::string("[") + code + "]";
    if (!param.empty()) detail += " (param: " + param + ")";
    if (detail.size() > 400) detail.resize(400);
    if (!detail.empty()) text += " Details: " + detail;
    return text;
}

} // namespace lb_chatgpt
