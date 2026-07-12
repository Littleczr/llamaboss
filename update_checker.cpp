//
// update_checker.cpp — see update_checker.h.
//
// Transport: WinHTTP (link winhttp.lib; pragma below handles MSVC).
// Parse:     Poco::JSON (already linked by the app).
//
#include "update_checker.h"

#include <windows.h>
#include <winhttp.h>

#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Object.h>
#include <Poco/Dynamic/Var.h>

#include <vector>
#include <cstdlib>

#pragma comment(lib, "winhttp.lib")

namespace {

// ── Update manifest location ─────────────────────────────────────────
// Astro serves files in /public at the site root, so dropping
// version.json into the site's public/ folder publishes it here.
const wchar_t* kHost = L"llamaboss.com";
const wchar_t* kPath = L"/version.json";
const wchar_t* kUserAgent = L"LlamaBoss-UpdateCheck";

// Network timeouts (ms): resolve / connect / send / receive.
const int kTimeoutMs = 8000;

// Split a version string into its numeric core components. Strips a leading
// 'v' and everything from the first '-' or '+' (pre-release / build metadata).
// IsNewer() compares this core first, then handles the common pre-release
// promotion case separately (for example: 0.1.5 final > 0.1.5-beta1).
std::vector<int> CoreParts(std::string v)
{
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V'))
        v.erase(0, 1);

    const size_t cut = v.find_first_of("-+");
    if (cut != std::string::npos)
        v = v.substr(0, cut);

    std::vector<int> parts;
    std::string cur;
    for (char c : v) {
        if (c == '.') {
            parts.push_back(cur.empty() ? 0 : std::atoi(cur.c_str()));
            cur.clear();
        } else if (c >= '0' && c <= '9') {
            cur.push_back(c);
        }
        // stray characters are ignored
    }
    parts.push_back(cur.empty() ? 0 : std::atoi(cur.c_str()));
    return parts;
}

// Blocking HTTPS GET via WinHTTP. Returns true on HTTP 200, filling 'body'.
// On failure returns false and fills 'error'.
bool HttpsGet(const wchar_t* host, const wchar_t* path,
              std::string& body, std::string& error)
{
    body.clear();
    error.clear();

    HINTERNET hSession = WinHttpOpen(
        kUserAgent,
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { error = "Could not initialize networking."; return false; }

    WinHttpSetTimeouts(hSession, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);

    HINTERNET hConnect = WinHttpConnect(
        hSession, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        error = "Could not connect to llamaboss.com.";
        WinHttpCloseHandle(hSession);
        return false;
    }

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"GET", path,
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        error = "Could not build the update request.";
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    bool ok = false;
    do {
        if (!WinHttpSendRequest(hRequest,
                WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            error = "Could not reach llamaboss.com (no connection?).";
            break;
        }
        if (!WinHttpReceiveResponse(hRequest, nullptr)) {
            error = "No response from llamaboss.com.";
            break;
        }

        // Status code.  If the status query itself fails, do not fall
        // through and parse the body as a successful manifest.
        DWORD status = 0, statusSize = sizeof(status);
        if (!WinHttpQueryHeaders(hRequest,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                WINHTTP_NO_HEADER_INDEX)) {
            error = "Could not read the update response status.";
            break;
        }
        if (status != 200) {
            error = "Update manifest returned HTTP " +
                    std::to_string(static_cast<int>(status)) + ".";
            break;
        }

        // Body.
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &avail)) {
                error = "Failed while reading the update manifest.";
                break;
            }
            if (avail == 0) { ok = true; break; }

            std::vector<char> buf(avail);
            DWORD read = 0;
            if (!WinHttpReadData(hRequest, buf.data(), avail, &read)) {
                error = "Failed while reading the update manifest.";
                break;
            }
            body.append(buf.data(), read);

            // Cap the manifest size defensively (~256 KB).
            if (body.size() > 256u * 1024u) {
                error = "Update manifest is unexpectedly large.";
                ok = false;
                break;
            }
        }
    } while (false);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok;
}

std::string JsonStringOrEmpty(const Poco::JSON::Object::Ptr& obj,
                              const std::string& key)
{
    if (obj->has(key)) {
        try { return obj->getValue<std::string>(key); }
        catch (...) {}
    }
    return std::string();
}

} // namespace

namespace UpdateChecker {

bool IsNewer(const std::string& remote, const std::string& local)
{
    const std::vector<int> r = CoreParts(remote);
    const std::vector<int> l = CoreParts(local);
    const size_t n = r.size() > l.size() ? r.size() : l.size();
    for (size_t i = 0; i < n; ++i) {
        const int rv = i < r.size() ? r[i] : 0;
        const int lv = i < l.size() ? l[i] : 0;
        if (rv != lv) return rv > lv;
    }

    // Equal numeric core: treat a final release as newer than a local
    // pre-release with the same core, e.g. 0.1.5 > 0.1.5-beta1.
    // Build metadata (+build7) alone is not a pre-release marker.
    const bool remotePre = remote.find('-') != std::string::npos;
    const bool localPre  = local.find('-')  != std::string::npos;
    return localPre && !remotePre;
}

UpdateInfo CheckBlocking(const std::string& currentVersion)
{
    UpdateInfo info;

    std::string body;
    if (!HttpsGet(kHost, kPath, body, info.error)) {
        info.ok = false;
        return info;
    }

    // A manifest saved by a Windows editor may carry a UTF-8 BOM
    // (EF BB BF) that Poco's JSON parser rejects. Strip it.
    if (body.size() >= 3 &&
        static_cast<unsigned char>(body[0]) == 0xEF &&
        static_cast<unsigned char>(body[1]) == 0xBB &&
        static_cast<unsigned char>(body[2]) == 0xBF) {
        body.erase(0, 3);
    }

    const size_t firstNonWs = body.find_first_not_of(" \t\r\n");
    if (firstNonWs == std::string::npos) {
        info.ok = false;
        info.error = "Update manifest was empty.";
        return info;
    }

    // If the host returned a web page (200 + HTML) instead of the JSON
    // file, say so plainly rather than emitting a generic parse error.
    if (body[firstNonWs] == '<') {
        info.ok = false;
        info.error = "llamaboss.com/version.json returned a web page, not "
                     "JSON (is version.json deployed at the site root?).";
        return info;
    }

    try {
        Poco::JSON::Parser parser;
        Poco::Dynamic::Var root = parser.parse(body);
        Poco::JSON::Object::Ptr obj = root.extract<Poco::JSON::Object::Ptr>();

        info.latest = JsonStringOrEmpty(obj, "version");
        info.url    = JsonStringOrEmpty(obj, "url");
        info.notes  = JsonStringOrEmpty(obj, "notes");

        if (info.latest.empty()) {
            info.ok = false;
            info.error = "Update manifest is missing a version field.";
            return info;
        }

        info.ok = true;
        info.available = IsNewer(info.latest, currentVersion);
    } catch (const std::exception& e) {
        info.ok = false;
        info.error = std::string("Could not parse update manifest: ") + e.what();
    } catch (...) {
        info.ok = false;
        info.error = "Could not parse update manifest.";
    }

    return info;
}

} // namespace UpdateChecker
