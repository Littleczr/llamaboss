//
// update_installer.cpp — see update_installer.h.
//
// Transport: WinHTTP (same as update_checker.cpp).
// Hashing:   Windows CNG / BCrypt SHA-256 (no Poco version dependency).
//
#include "update_installer.h"

#include "lb_windows.h"
#include <winhttp.h>
#include <bcrypt.h>
#include <shellapi.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

namespace fs = std::filesystem;

namespace {

const wchar_t* kUserAgent = L"LlamaBoss-Updater";
const int      kTimeoutMs = 15000;                              // per operation
const std::uint64_t kMaxInstallerBytes = 4ull * 1024 * 1024 * 1024; // sanity cap

// Inno Setup command line for an in-app update:
//   /SILENT              small progress window only, no wizard pages
//   /SP-                 no "This will install..." prompt
//   /SUPPRESSMSGBOXES    take default answers instead of blocking
//   /NORESTART           never reboot Windows
//   /CLOSEAPPLICATIONS   Restart Manager closes anything still holding files
//                        (e.g. a llama-server.exe that is slow to exit)
//   /LBUPDATE=1          our own flag: tells [Run] to relaunch LlamaBoss
const wchar_t* kInstallerArgs =
    L"/SILENT /SP- /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS /LBUPDATE=1";

fs::path UpdateDir()
{
    wchar_t buf[MAX_PATH + 1] = {};
    const DWORD n = GetTempPathW(MAX_PATH + 1, buf);
    fs::path base = (n > 0 && n <= MAX_PATH) ? fs::path(buf) : fs::temp_directory_path();
    return base / L"LlamaBoss_Update";
}

std::wstring SafeVersionForFilename(const std::string& v)
{
    std::wstring out;
    for (char c : v) {
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') || c == '.' || c == '-')
            out.push_back(static_cast<wchar_t>(c));
    }
    return out.empty() ? L"latest" : out;
}

std::wstring Widen(const std::string& s)
{
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// Minimal RAII for the three WinHTTP handles.
struct HInternet {
    HINTERNET h = nullptr;
    HInternet() = default;
    explicit HInternet(HINTERNET x) : h(x) {}
    ~HInternet() { if (h) WinHttpCloseHandle(h); }
    HInternet(const HInternet&) = delete;
    HInternet& operator=(const HInternet&) = delete;
    explicit operator bool() const { return h != nullptr; }
};

// Incremental SHA-256 over CNG.
class Sha256 {
public:
    Sha256()
    {
        if (BCryptOpenAlgorithmProvider(&m_alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
            return;
        if (BCryptCreateHash(m_alg, &m_hash, nullptr, 0, nullptr, 0, 0) != 0)
            m_hash = nullptr;
    }
    ~Sha256()
    {
        if (m_hash) BCryptDestroyHash(m_hash);
        if (m_alg)  BCryptCloseAlgorithmProvider(m_alg, 0);
    }
    bool Ok() const { return m_hash != nullptr; }
    bool Update(const void* data, ULONG len)
    {
        return BCryptHashData(m_hash, (PUCHAR)data, len, 0) == 0;
    }
    std::string FinishHex()
    {
        unsigned char digest[32] = {};
        if (BCryptFinishHash(m_hash, digest, sizeof(digest), 0) != 0)
            return std::string();
        static const char* hex = "0123456789abcdef";
        std::string out;
        out.reserve(64);
        for (unsigned char b : digest) {
            out.push_back(hex[b >> 4]);
            out.push_back(hex[b & 0xF]);
        }
        return out;
    }
private:
    BCRYPT_ALG_HANDLE  m_alg  = nullptr;
    BCRYPT_HASH_HANDLE m_hash = nullptr;
};

// Hashes the file as it sits on disk.  The streaming hash covers the
// bytes that came off the network; this covers what was actually saved,
// so a write that failed silently (disk full on the final flush) can't
// leave a damaged installer marked verified.
std::string HashFileHex(const fs::path& path, std::uint64_t& sizeOut)
{
    sizeOut = 0;
    Sha256 sha;
    if (!sha.Ok()) return std::string();
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::string();
    std::vector<char> buf(1024 * 1024);
    for (;;) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize n = in.gcount();
        if (n > 0) {
            if (!sha.Update(buf.data(), static_cast<ULONG>(n))) return std::string();
            sizeOut += static_cast<std::uint64_t>(n);
        }
        if (!in) break;
    }
    if (in.bad()) return std::string();
    return sha.FinishHex();
}

} // namespace

namespace UpdateInstaller {

bool IsValidSha256Hex(const std::string& s)
{
    if (s.size() != 64) return false;
    for (char c : s) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                         (c >= 'A' && c <= 'F');
        if (!hex) return false;
    }
    return true;
}

DownloadResult DownloadAndVerify(const std::string& url,
                                 const std::string& expectedSha256Hex,
                                 const std::string& version,
                                 const ProgressFn& progress,
                                 const std::atomic<bool>& cancel)
{
    DownloadResult r;

    if (!IsValidSha256Hex(expectedSha256Hex)) {
        r.error = "Update manifest has no valid SHA-256 for the installer.";
        return r;
    }
    std::string expected = expectedSha256Hex;
    for (char& c : expected)
        if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');

    // ── Crack the URL ───────────────────────────────────────────────
    const std::wstring wurl = Widen(url);
    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    uc.lpszHostName = host;  uc.dwHostNameLength = _countof(host);
    uc.lpszUrlPath  = path;  uc.dwUrlPathLength  = _countof(path);
    wchar_t extra[1024] = {};
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = _countof(extra);
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) {
        r.error = "Installer URL is not a valid https:// address.";
        return r;
    }
    const std::wstring object = std::wstring(path) + extra;

    // ── Prepare the destination ─────────────────────────────────────
    std::error_code ec;
    const fs::path dir = UpdateDir();
    fs::create_directories(dir, ec);
    // Unique names per attempt.  Closing About while a download is being
    // cancelled and retrying straight away can leave the old worker still
    // exiting (blocked in WinHttpReadData); with version-only names both
    // workers would write, rename and delete the same files.
    //
    // Sweep leftovers from earlier attempts first.  A file another worker
    // still has open can't be deleted (std::ofstream doesn't grant
    // FILE_SHARE_DELETE), so the sweep only removes abandoned files; the
    // live worker cleans up its own on the way out.
    static std::atomic<unsigned long long> s_attemptSeq{0};
    const std::wstring prefix = L"LlamaBoss_Setup_v";
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path p = it->path();
        const std::wstring fn = p.filename().wstring();
        const std::wstring ext = p.extension().wstring();
        if (fn.compare(0, prefix.size(), prefix) == 0 &&
            (ext == L".part" || ext == L".exe")) {
            std::error_code rmEc;
            fs::remove(p, rmEc);
        }
    }
    ec.clear();

    const std::wstring name = prefix + SafeVersionForFilename(version) + L"." +
        std::to_wstring(GetCurrentProcessId()) + L"." +
        std::to_wstring(++s_attemptSeq);
    const fs::path partPath  = dir / (name + L".part");
    const fs::path finalPath = dir / (name + L".exe");

    // ── Request ─────────────────────────────────────────────────────
    HInternet session(WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) { r.error = "Could not initialize networking."; return r; }
    WinHttpSetTimeouts(session.h, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);

    HInternet connect(WinHttpConnect(session.h, host, uc.nPort, 0));
    if (!connect) { r.error = "Could not connect to the download server."; return r; }

    HInternet request(WinHttpOpenRequest(connect.h, L"GET", object.c_str(), nullptr,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE));
    if (!request) { r.error = "Could not build the download request."; return r; }

    if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr)) {
        r.error = "Could not reach the download server (no connection?).";
        return r;
    }

    DWORD status = 0, sz = sizeof(status);
    if (!WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                             WINHTTP_NO_HEADER_INDEX) || status != 200) {
        r.error = "Installer download returned HTTP " + std::to_string((int)status) + ".";
        return r;
    }

    std::uint64_t total = 0;
    {
        wchar_t lenBuf[32] = {};
        DWORD lenSz = sizeof(lenBuf);
        if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH,
                                WINHTTP_HEADER_NAME_BY_INDEX, lenBuf, &lenSz,
                                WINHTTP_NO_HEADER_INDEX))
            total = _wcstoui64(lenBuf, nullptr, 10);
    }
    if (total > kMaxInstallerBytes) {
        r.error = "Installer is unexpectedly large; download refused.";
        return r;
    }

    // ── Stream to disk + hash ───────────────────────────────────────
    Sha256 sha;
    if (!sha.Ok()) { r.error = "SHA-256 is unavailable on this system."; return r; }

    bool writeOk = true;
    std::uint64_t done = 0;
    {
        std::ofstream out(partPath, std::ios::binary | std::ios::trunc);
        if (!out) { r.error = "Could not write to the temp folder."; return r; }

        std::vector<char> buf(256 * 1024);
        std::uint64_t lastReported = 0;
        for (;;) {
            if (cancel.load(std::memory_order_acquire)) { r.cancelled = true; break; }

            DWORD read = 0;
            if (!WinHttpReadData(request.h, buf.data(), (DWORD)buf.size(), &read)) {
                r.error = "Download interrupted.";
                break;
            }
            if (read == 0) break;   // end of body

            out.write(buf.data(), read);
            if (!out) { r.error = "Disk write failed (disk full?)."; writeOk = false; break; }
            sha.Update(buf.data(), read);
            done += read;

            if (done > kMaxInstallerBytes) {
                r.error = "Installer is unexpectedly large; download stopped.";
                break;
            }
            if (progress && (done - lastReported >= 512 * 1024)) {
                lastReported = done;
                progress(done, total);
            }
        }

        // Close explicitly and check: the destructor would swallow a
        // failure of the final buffered write (disk full on flush).
        if (!r.cancelled && r.error.empty() && writeOk) {
            out.flush();
            const bool flushed = out.good();
            out.close();
            if (!flushed || out.fail()) {
                r.error = "Disk write failed while saving the installer (disk full?).";
                writeOk = false;
            }
        }
    }

    const bool finished = !r.cancelled && r.error.empty() && writeOk;
    if (!finished) {
        fs::remove(partPath, ec);
        return r;
    }
    if (total != 0 && done != total) {
        fs::remove(partPath, ec);
        r.error = "Download was incomplete.";
        return r;
    }
    if (progress) progress(done, total);

    // ── Verify ──────────────────────────────────────────────────────
    const std::string actual = sha.FinishHex();
    if (actual.empty() || actual != expected) {
        fs::remove(partPath, ec);
        r.error = "Downloaded installer failed the SHA-256 check and was deleted.";
        return r;
    }

    // Verify the saved file too, not just the network stream.
    {
        std::uint64_t diskSize = 0;
        const std::string onDisk = HashFileHex(partPath, diskSize);
        if (onDisk.empty() || diskSize != done || onDisk != expected) {
            fs::remove(partPath, ec);
            r.error = "The saved installer didn't match what was downloaded "
                      "(disk problem?) and was deleted.";
            return r;
        }
    }

    // Cancel can arrive during the hash check; honour it before the
    // installer is finalized and reported as ready.
    if (cancel.load(std::memory_order_acquire)) {
        fs::remove(partPath, ec);
        r.cancelled = true;
        return r;
    }

    fs::rename(partPath, finalPath, ec);
    if (ec) {
        fs::remove(partPath, ec);
        r.error = "Could not finalize the downloaded installer.";
        return r;
    }

    r.ok = true;
    r.path = finalPath.wstring();
    return r;
}

bool LaunchInstaller(const std::wstring& installerPath, std::string& error)
{
    // Keep an install log next to the installer — handy when a user
    // reports "the update didn't work".
    const std::wstring logPath = (UpdateDir() / L"install.log").wstring();
    const std::wstring args = std::wstring(kInstallerArgs) + L" /LOG=\"" + logPath + L"\"";

    SHELLEXECUTEINFOW sei = {};
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb       = L"open";
    sei.lpFile       = installerPath.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow        = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei)) {
        error = "Could not start the installer (error " +
                std::to_string((unsigned long)GetLastError()) + ").";
        return false;
    }
    return true;
}

void CleanupStaleDownloads()
{
    std::error_code ec;
    const fs::path dir = UpdateDir();
    if (!fs::exists(dir, ec)) return;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        // Keep the last install.log (tiny, overwritten next update) so an
        // "update didn't work" report can still be diagnosed after the
        // relaunch.  A still-running installer keeps its own lock.
        if (entry.path().filename() == L"install.log") continue;
        std::error_code ignore;
        fs::remove(entry.path(), ignore);
    }
}

} // namespace UpdateInstaller
