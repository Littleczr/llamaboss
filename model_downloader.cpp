// model_downloader.cpp
// Curated model catalog + HTTPS download dialog for LlamaBoss.
// Models are sourced from bartowski on HuggingFace — no account required.

#include "model_downloader.h"
#include "server_manager.h"
#include "theme.h"
#include "path_safety.h"
#include "lb_ssl.h"

// Poco HTTPS
#include <Poco/Net/HTTPMessage.h>
#include <Poco/Net/NetSSL.h>

#include "ui_event_post.h"

#include "lb_windows.h"

// ── Events ──────────────────────────────────────────────────────
wxDEFINE_EVENT(wxEVT_DOWNLOAD_PROGRESS, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_DOWNLOAD_COMPLETE, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_DOWNLOAD_ERROR,    wxCommandEvent);

// ── Forward declarations for file-scope helpers ─────────────────
// Defined further down; declared up here so member functions that
// reference them (OnDownloadComplete chaining the mmproj download)
// compile regardless of where they appear in the file.
static std::string BundleNameFor(const DownloadableModel& m);
static std::string BuildMmprojUrl(const DownloadableModel& m);
static std::string BuildMmprojDestPath(const DownloadableModel& m);

// ─────────────────────────────────────────────────────────────────
//  Button helpers (file-local)
// ─────────────────────────────────────────────────────────────────
//
// Same Telegram-style recipe used in settings.cpp / model_manager.cpp /
// project_attach_dialog.cpp: wxButton + wxBORDER_NONE so Win11 doesn't
// paint native chrome over the solid fill, semibold 10pt label, theme
// palette applied in place. The per-row action button uses these to
// flip between download/cancel/retry/downloaded states by reapplying
// a different palette over the same wxButton.
//
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

// Re-tint helpers for the per-row action button. The widget itself
// stays put; we only swap palette + label as the download moves
// between states.
void TintAccent(wxButton* btn, const ThemeData& t)
{
    btn->SetBackgroundColour(t.accentButton);
    btn->SetForegroundColour(t.accentButtonText);
    btn->Refresh();
}

void TintDestructive(wxButton* btn, const ThemeData& t)
{
    btn->SetBackgroundColour(t.stopButton);
    btn->SetForegroundColour(t.stopButtonText);
    btn->Refresh();
}

void TintFlatMuted(wxButton* btn, const ThemeData& t)
{
    btn->SetBackgroundColour(t.bgDialogSurface);
    btn->SetForegroundColour(t.textMuted);
    btn->Refresh();
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════
//  Curated model catalog
//
//  Ordering: ascending by quantized size, so the dialog reads as a
//  ladder of capability/cost.  The "tag" column tells the user where
//  each model fits ("Ultra-light" / "Recommended" / "Top quality").
//
//  All from bartowski on HuggingFace — publicly downloadable, no
//  account or license gate required.  Repo naming is NOT uniform:
//  bartowski's 2025–mid-2026 uploads use an "<org>_" prefix
//  (google_gemma-4-E4B-it-GGUF, openai_gpt-oss-20b-GGUF), but his newer
//  first-party uploads dropped it (gemma-4-12B-it-GGUF,
//  Qwen3.8-27B-GGUF), and the older Llama 3.2 entry never had it.
//  Copy repo + filename exactly from the HF "Files" tab; never infer.
//
//  Q4_K_M is the chosen quant for every entry — well-rounded quality
//  vs size, default-recommended by bartowski himself, and works on
//  every llama.cpp backend including Vulkan (unlike I-quants which
//  the Vulkan backend cannot run).  Projectors use the f16 file (not
//  bf16) everywhere for the same backend-compatibility reason.
//
//  Exception: gpt-oss ships as MXFP4. Its feed-forward weights don't
//  quantize well to anything else, so bartowski keeps the FFNs at
//  MXFP4 in every quant — making MXFP4 the canonical file (all the
//  other quants of that repo are the same size anyway). Smoke-test
//  against the llama.cpp Vulkan backend before shipping this entry.
//
//  Last verified against HuggingFace: 2026-09-24.  Sizes are the HF
//  listing (decimal GB) and only drive the progress bar / UI text.
// ═══════════════════════════════════════════════════════════════════
const std::vector<DownloadableModel> ModelDownloaderDialog::kModels =
{
    {
        "Gemma 3 1B",    "Ultra-light",
        "Tiny chat model. Runs on integrated GPUs and 8 GB laptops.",
        "bartowski", "google_gemma-3-1b-it-GGUF",
        "google_gemma-3-1b-it-Q4_K_M.gguf",
        "0.8 GB", 800'000'000LL
        // Text-only — no mmproj fields
    },
    {
        "Llama 3.2 3B", "Lightweight",
        "Meta's smallest chat model. Runs on CPU or minimal VRAM.",
        "bartowski", "Llama-3.2-3B-Instruct-GGUF",
        "Llama-3.2-3B-Instruct-Q4_K_M.gguf",
        "2.0 GB", 2'000'000'000LL,
        "", "", 0,    // Text-only — no mmproj fields
        true          // firstRunStarter — recommended for brand-new users
    },
    {
        "Gemma 4 E2B",   "Fast",
        "Google's compact multimodal model. Vision and audio input. Low-spec friendly.",
        "bartowski", "google_gemma-4-E2B-it-GGUF",
        "google_gemma-4-E2B-it-Q4_K_M.gguf",
        "3.5 GB", 3'460'000'000LL,
        "mmproj-google_gemma-4-E2B-it-f16.gguf", "1.0 GB", 986'000'000LL
    },
    {
        "Gemma 4 E4B",   "Recommended",
        "Multimodal — vision and audio. Best balance of speed and quality for most users.",
        "bartowski", "google_gemma-4-E4B-it-GGUF",
        "google_gemma-4-E4B-it-Q4_K_M.gguf",
        "5.4 GB", 5'410'000'000LL,
        "mmproj-google_gemma-4-E4B-it-f16.gguf", "1.0 GB", 990'000'000LL
    },
    {
        // Added 2026-09: the dense 12B Google released after the original
        // Gemma 4 launch.  Fills the 5 GB -> 12 GB gap in the ladder.
        // "Unified" = encoder-free: its projector is tiny (122 MB) because
        // image patches go straight into the LLM.  Needs a llama.cpp build
        // recent enough to know this projector type — smoke-test image
        // input on the bundled llama-server before shipping.
        "Gemma 4 12B",   "Step Up",
        "Dense 12B — big quality jump over E4B. Vision-capable. Needs 10+ GB VRAM.",
        "bartowski", "gemma-4-12B-it-GGUF",
        "gemma-4-12B-it-Q4_K_M.gguf",
        "7.7 GB", 7'660'000'000LL,
        "mmproj-gemma-4-12B-it-f16.gguf", "0.1 GB", 122'000'000LL
    },
    {
        "gpt-oss 20B",   "Reasoning",
        "OpenAI's open model. Strong reasoning, math, and code. Needs 13+ GB VRAM.",
        "bartowski", "openai_gpt-oss-20b-GGUF",
        "openai_gpt-oss-20b-MXFP4.gguf",
        "12.1 GB", 12'100'000'000LL
        // Text-only — no mmproj fields.
        // MXFP4, not Q4_K_M — see catalog header for why.
    },
    {
        "Gemma 4 26B A4B", "Fast & Powerful",
        "Mixture-of-Experts — 26B knowledge at 4B speed. Vision-capable. Needs 18+ GB VRAM.",
        "bartowski", "google_gemma-4-26B-A4B-it-GGUF",
        "google_gemma-4-26B-A4B-it-Q4_K_M.gguf",
        "17.0 GB", 17'000'000'000LL,
        "mmproj-google_gemma-4-26B-A4B-it-f16.gguf", "1.2 GB", 1'190'000'000LL
    },
    {
        // Replaces Qwen 3.6 27B (2026-09).  Same decoder shape as 3.6,
        // better weights; released Aug 14 2026, Apache 2.0.  Note the
        // repo/file names carry no "Qwen_" prefix, unlike the 3.6 repo.
        "Qwen 3.8 27B",  "Top All-Rounder",
        "Alibaba's latest. Frontier quality, vision-capable. Needs 20+ GB VRAM.",
        "bartowski", "Qwen3.8-27B-GGUF",
        "Qwen3.8-27B-Q4_K_M.gguf",
        "17.4 GB", 17'400'000'000LL,
        "mmproj-Qwen3.8-27B-f16.gguf", "0.9 GB", 928'000'000LL
    },
    {
        "Gemma 4 31B",   "Top Quality",
        "Frontier-level Gemma. Highest quality, vision-capable. Requires 20+ GB VRAM.",
        "bartowski", "google_gemma-4-31B-it-GGUF",
        "google_gemma-4-31B-it-Q4_K_M.gguf",
        "19.6 GB", 19'600'000'000LL,
        "mmproj-google_gemma-4-31B-it-f16.gguf", "1.2 GB", 1'200'000'000LL
    },
};

// ═══════════════════════════════════════════════════════════════════
//  SSL
// ═══════════════════════════════════════════════════════════════════
// Deliberately NOT initialized here.  Poco's SSLManager is a process
// singleton; initializing it here with a custom certificate handler
// would race lb_ssl.cpp for control of it (and an
// AcceptCertificateHandler would disable verification).
// DownloadThread::Entry() calls lb::EnsureSSLInitialized() like every
// other HTTPS path.
//
// Possible further hardening for downloads: publish SHA-256 hashes
// alongside the curated catalog entries and verify the completed file
// before renaming it off .download.  Transport authenticity holds;
// content authenticity would be the second layer.

static bool QuietRemoveFileUtf8(const std::string& path)
{
    if (path.empty()) return true;

    wxString wxPath = wxString::FromUTF8(path);

    // wxRemoveFile logs an error dialog if the file is already gone. During
    // failed downloads, cleanup is best-effort and must never mask the real
    // network/SSL failure.
    wxLogNull noLog;

    if (!wxFileExists(wxPath)) return true;
    return wxRemoveFile(wxPath);
}

#ifdef __WXMSW__
static std::string LastWindowsErrorUtf8()
{
    DWORD err = GetLastError();
    if (err == ERROR_SUCCESS) return std::string();

    LPWSTR buffer = nullptr;
    DWORD chars = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        err,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);

    std::string msg;
    if (chars > 0 && buffer) {
        wxString wxMsg(buffer);
        msg = std::string(wxMsg.utf8_string());
        while (!msg.empty() && (msg.back() == '\r' || msg.back() == '\n' || msg.back() == ' ' || msg.back() == '\t')) {
            msg.pop_back();
        }
    }

    if (buffer) LocalFree(buffer);

    if (msg.empty()) {
        msg = "Windows error " + std::to_string(static_cast<unsigned long>(err));
    }
    return msg;
}
#endif

static bool PromoteDownloadUtf8(const std::string& tempPath,
                                const std::string& destPath,
                                std::string* errorOut)
{
    if (errorOut) errorOut->clear();

#ifdef __WXMSW__
    std::wstring tempWide = path_safety::Utf8ToWide(tempPath);
    std::wstring destWide = path_safety::Utf8ToWide(destPath);

    // Important: do not delete the existing model before the replacement is
    // guaranteed. MoveFileExW with MOVEFILE_REPLACE_EXISTING preserves the
    // old final file when the replacement fails, such as when antivirus,
    // permissions, or a running llama.cpp server has the file locked.
    if (MoveFileExW(tempWide.c_str(),
                    destWide.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }

    if (errorOut) *errorOut = LastWindowsErrorUtf8();
    return false;
#else
    wxLogNull noLog;

    // Non-Windows fallback: wxRenameFile(..., true) requests overwrite without
    // pre-deleting the destination, avoiding a destructive remove-first
    // sequence.
    if (wxRenameFile(wxString::FromUTF8(tempPath),
                     wxString::FromUTF8(destPath),
                     true)) {
        return true;
    }

    if (errorOut) *errorOut = "rename failed";
    return false;
#endif
}

static wxString FriendlyDownloadError(const std::string& raw)
{
    wxString msg = wxString::FromUTF8(raw);

    if (raw.find("certificate verify failed") != std::string::npos ||
        raw.find("SSL routines") != std::string::npos)
    {
        msg += "\n\nThis looks like an HTTPS certificate problem. It can happen on work/company networks, antivirus web filtering, or proxy-inspected traffic.";
        msg += "\n\nTry downloading the model in your browser and placing the .gguf file in the LlamaBoss models folder, or try again from another network.";
    }

    return msg;
}

// Every GGUF file starts with these four bytes.  Checked on the first
// bytes of every download so an HTML error page or login page saved
// under a .gguf name is caught immediately, not when llama-server
// later refuses to load it.
static bool HasGgufMagic(const std::string& head)
{
    return head.size() >= 4 && head.compare(0, 4, "GGUF") == 0;
}

static const char* kNotGgufMessage =
    "The server sent something that isn't a GGUF model file (often a web "
    "page or an error message). Check that the link points to a .gguf file.";

// Plain-language reason for a failed HTTP status.  `url` is the URL that
// returned the status: the gated-model wording only applies when Hugging
// Face itself refused, not when its storage CDN rejected a signed link.
static std::string HttpFailureMessage(int status, const std::string& reason,
                                      const std::string& url)
{
    std::string host;
    try { host = Poco::URI(url).getHost(); } catch (...) {}
    for (char& c : host) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const bool hf = host == "huggingface.co" || host == "www.huggingface.co" || host == "hf.co";

    if ((status == 401 || status == 403) && hf)
        return "This model is gated: Hugging Face requires signing in and "
               "accepting its license before downloading, and LlamaBoss can't "
               "sign in for you. Look for a community upload of the same model "
               "(for example from bartowski or unsloth).";
    if (status == 401 || status == 403)
        return "The server refused access (HTTP " + std::to_string(status) + ").";
    if (status == 404)
        return "File not found (HTTP 404). Check the link: it should point to "
               "a single .gguf file.";
    if (status == 429)
        return "The server is rate-limiting downloads (HTTP 429). Wait a few "
               "minutes and try again.";
    return "Server returned HTTP " + std::to_string(status) + " " + reason;
}
// ═══════════════════════════════════════════════════════════════════
//  DownloadThread
// ═══════════════════════════════════════════════════════════════════

DownloadThread::DownloadThread(wxEvtHandler*   handler,
                               const std::string& url,
                               const std::string& destPath,
                               long long          expectedBytes,
                               std::shared_ptr<std::atomic<bool>> cancelFlag,
                               std::weak_ptr<std::atomic<bool>>   aliveToken,
                               long               generation,
                               bool               expectedBytesExact)
    : wxThread(wxTHREAD_DETACHED)
    , m_handler(handler)
    , m_url(url)
    , m_destPath(destPath)
    , m_expectedBytes(expectedBytes)
    , m_expectedBytesExact(expectedBytesExact && expectedBytes > 0)
    , m_cancelFlag(cancelFlag)
    , m_aliveToken(aliveToken)
    , m_generation(generation)
{}

bool DownloadThread::SafePost(wxCommandEvent* ev)
{
    // Stamp the operation identity so the dialog can reject a late event
    // from a cancelled/superseded worker. Done here so every event this
    // worker posts carries it, without touching each construction site.
    ev->SetInt(static_cast<int>(m_generation));

    // Cancelled? Drop the event on the floor.
    if (m_cancelFlag->load()) { delete ev; return false; }

    // Verify the handler is still alive and queue under the shared UI-post
    // mutex. This closes the old check-then-wxQueueEvent race when the
    // dialog is being destroyed while a detached download thread finishes.
    return LbQueueEventIfAlive(m_handler, m_aliveToken, ev);
}

wxThread::ExitCode DownloadThread::Entry()
{
    lb::EnsureSSLInitialized();

    // Per-operation scratch path. A cancelled worker can still be blocked
    // in socket I/O for up to its 60s receive timeout, and it removes its
    // own temp file on the way out. A fresh retry must therefore NOT reuse
    // the same name, or the old worker's cleanup can delete (or its final
    // write can truncate) the new attempt's file.
    //
    // The name must NOT come from m_generation: that counter starts at 0 in
    // every new dialog, so closing and reopening the downloader can pair a
    // still-blocked old worker with a new one on the same ".download.1".
    // A process-wide sequence (plus PID, in case two instances share a
    // models folder) is unique for every worker ever started.
    static std::atomic<unsigned long long> s_tempSeq{0};
    const unsigned long long seq = ++s_tempSeq;
    std::string tempPath = m_destPath + ".download." +
        std::to_string(static_cast<unsigned long>(wxGetProcessId())) + "." +
        std::to_string(seq);

    try
    {
        std::string currentUrl = m_url;
        const int kMaxHops = 8;

        for (int hop = 0; hop <= kMaxHops; ++hop)
        {
            if (m_cancelFlag->load()) return (ExitCode)0;

            Poco::URI uri(currentUrl);
            // HTTPS at every hop, not just the probe: a server can answer
            // the 4-byte probe and the real transfer with different
            // redirects, so the worker enforces the rule itself.
            if (uri.getScheme() != "https") {
                auto* ev = new wxCommandEvent(wxEVT_DOWNLOAD_ERROR);
                ev->SetString(hop == 0
                    ? "Use an https:// link. Plain http downloads are not allowed."
                    : "The link redirected to a non-https address; download refused.");
                SafePost(ev);
                return (ExitCode)0;
            }
            int port = uri.getPort();
            if (port == 0) port = 443;

            // ── Open session ─────────────────────────────────────
            std::unique_ptr<Poco::Net::HTTPClientSession> sess;
            {
                auto* s = new Poco::Net::HTTPSClientSession(uri.getHost(), port);
                s->setTimeout(Poco::Timespan(60, 0));
                sess.reset(s);
            }

            std::string path = uri.getPathAndQuery();
            if (path.empty()) path = "/";

            // ── Send request ─────────────────────────────────────
            Poco::Net::HTTPRequest req(
                Poco::Net::HTTPRequest::HTTP_GET, path,
                Poco::Net::HTTPMessage::HTTP_1_1);
            req.set("User-Agent", "LlamaBoss/1.0");
            req.set("Accept",     "*/*");
            req.set("Host",       uri.getHost());
            sess->sendRequest(req);

            Poco::Net::HTTPResponse resp;
            std::istream& in = sess->receiveResponse(resp);
            int status = resp.getStatus();

            // ── Follow redirects ──────────────────────────────────
            if (status == 301 || status == 302 || status == 303 ||
                status == 307 || status == 308)
            {
                if (!resp.has("Location")) {
                    auto* ev = new wxCommandEvent(wxEVT_DOWNLOAD_ERROR);
                    ev->SetString("Redirect with no Location header");
                    SafePost(ev);
                    return (ExitCode)0;
                }
                std::string loc = resp.get("Location");
                // Resolve relative URLs against the current base
                if (loc.size() < 4 || loc.substr(0, 4) != "http") {
                    Poco::URI base(currentUrl);
                    Poco::URI rel(loc);
                    base.resolve(rel);
                    currentUrl = base.toString();
                } else {
                    currentUrl = loc;
                }
                continue;
            }

            // ── Error response ───────────────────────────────────
            if (status != 200) {
                auto* ev = new wxCommandEvent(wxEVT_DOWNLOAD_ERROR);
                ev->SetString(wxString::FromUTF8(
                    HttpFailureMessage(status, resp.getReason(), currentUrl)));
                SafePost(ev);
                return (ExitCode)0;
            }

            // ── 200 OK — stream to temp file ─────────────────────
            long long totalBytes    = m_expectedBytes;
            long long declaredBytes = -1;

            if (resp.has("Content-Length")) {
                try {
                    declaredBytes = std::stoll(resp.get("Content-Length"));
                    totalBytes = declaredBytes;
                }
                catch (...) {
                    declaredBytes = -1;
                }
            }

            // The probe already told us the exact size.  A response that
            // declares a different length is refused before any data moves.
            if (m_expectedBytesExact && declaredBytes >= 0 &&
                declaredBytes != m_expectedBytes) {
                auto* ev = new wxCommandEvent(wxEVT_DOWNLOAD_ERROR);
                ev->SetString(
                    "The server changed the file size between the check and "
                    "the download (expected " + std::to_string(m_expectedBytes) +
                    " bytes, now " + std::to_string(declaredBytes) +
                    "). Nothing was saved. Try again.");
                SafePost(ev);
                return (ExitCode)0;
            }
            if (m_expectedBytesExact) totalBytes = m_expectedBytes;

            std::ofstream out(std::filesystem::path(path_safety::Utf8ToWide(tempPath)), std::ios::binary | std::ios::trunc);
            if (!out.is_open()) {
                auto* ev = new wxCommandEvent(wxEVT_DOWNLOAD_ERROR);
                ev->SetString("Cannot create temporary file:\n" + tempPath);
                SafePost(ev);
                return (ExitCode)0;
            }

            char      buf[65536];
            long long received   = 0;
            long long lastReport = -1;
            std::string head;            // first 4 bytes, for the GGUF check
            bool      magicChecked = false;

            while (!m_cancelFlag->load())
            {
                in.read(buf, sizeof(buf));
                std::streamsize n = in.gcount();

                if (n > 0) {
                    if (!magicChecked) {
                        head.append(buf, static_cast<size_t>(
                            std::min<std::streamsize>(n, 4 - static_cast<std::streamsize>(head.size()))));
                        if (head.size() >= 4) {
                            magicChecked = true;
                            if (!HasGgufMagic(head)) {
                                out.close();
                                QuietRemoveFileUtf8(tempPath);
                                auto* ev = new wxCommandEvent(wxEVT_DOWNLOAD_ERROR);
                                ev->SetString(kNotGgufMessage);
                                SafePost(ev);
                                return (ExitCode)0;
                            }
                        }
                    }

                    out.write(buf, n);

                    if (!out.good()) {
                        out.close();
                        QuietRemoveFileUtf8(tempPath);

                        auto* ev = new wxCommandEvent(wxEVT_DOWNLOAD_ERROR);
                        ev->SetString("Disk write failed while downloading:\n" + m_destPath);
                        SafePost(ev);
                        return (ExitCode)0;
                    }

                    received += static_cast<long long>(n);

                    // Report progress approximately every 2 MB.
                    if (received - lastReport >= 2LL * 1024 * 1024 || lastReport < 0)
                    {
                        lastReport = received;

                        int pct = 0;
                        if (totalBytes > 0) {
                            pct = static_cast<int>(received * 100LL / totalBytes);
                            pct = std::max(0, std::min(100, pct));
                        }

                        auto* ev = new wxCommandEvent(wxEVT_DOWNLOAD_PROGRESS);
                        ev->SetExtraLong(static_cast<long>(pct));
                        ev->SetString(wxString::Format("%lld|%lld", received, totalBytes));

                        if (!SafePost(ev)) {
                            out.close();
                            QuietRemoveFileUtf8(tempPath);
                            return (ExitCode)0;
                        }
                    }
                }

                if (n <= 0) break;
            }

            if (in.bad()) {
                out.close();
                QuietRemoveFileUtf8(tempPath);

                auto* ev = new wxCommandEvent(wxEVT_DOWNLOAD_ERROR);
                ev->SetString("Network read failed before download completed.");
                SafePost(ev);
                return (ExitCode)0;
            }

            out.flush();
            if (!out.good()) {
                out.close();
                QuietRemoveFileUtf8(tempPath);

                auto* ev = new wxCommandEvent(wxEVT_DOWNLOAD_ERROR);
                ev->SetString("Disk flush failed while saving:\n" + m_destPath);
                SafePost(ev);
                return (ExitCode)0;
            }

            out.close();

            if (m_cancelFlag->load()) {
                QuietRemoveFileUtf8(tempPath);
                return (ExitCode)0;
            }

            if (received <= 0) {
                QuietRemoveFileUtf8(tempPath);

                auto* ev = new wxCommandEvent(wxEVT_DOWNLOAD_ERROR);
                ev->SetString("Download produced an empty file.");
                SafePost(ev);
                return (ExitCode)0;
            }

            // Fewer than 4 bytes arrived, so the check above never ran.
            if (!magicChecked) {
                QuietRemoveFileUtf8(tempPath);

                auto* ev = new wxCommandEvent(wxEVT_DOWNLOAD_ERROR);
                ev->SetString(kNotGgufMessage);
                SafePost(ev);
                return (ExitCode)0;
            }

            // Integrity check: if the server told us the exact Content-Length,
            // the received byte count must match exactly before we rename the file.
            if (declaredBytes >= 0 && received != declaredBytes) {
                QuietRemoveFileUtf8(tempPath);

                auto* ev = new wxCommandEvent(wxEVT_DOWNLOAD_ERROR);
                ev->SetString(
                    "Incomplete download.\nExpected " + std::to_string(declaredBytes) +
                    " bytes, received " + std::to_string(received) + " bytes.");
                SafePost(ev);
                return (ExitCode)0;
            }

            // Exact size from the probe: enforced even when this response
            // sent no Content-Length, or one that matched a short body.
            if (m_expectedBytesExact && received != m_expectedBytes) {
                QuietRemoveFileUtf8(tempPath);

                auto* ev = new wxCommandEvent(wxEVT_DOWNLOAD_ERROR);
                ev->SetString(
                    "Incomplete download.\nExpected " +
                    std::to_string(m_expectedBytes) + " bytes, received " +
                    std::to_string(received) + " bytes. The file was not saved.");
                SafePost(ev);
                return (ExitCode)0;
            }

            // Second integrity check: verify the temp file on disk matches what
            // we believe we wrote before promoting it to the final .gguf path.
            {
                std::ifstream verify(std::filesystem::path(path_safety::Utf8ToWide(tempPath)), std::ios::binary | std::ios::ate);
                if (!verify.is_open()) {
                    QuietRemoveFileUtf8(tempPath);

                    auto* ev = new wxCommandEvent(wxEVT_DOWNLOAD_ERROR);
                    ev->SetString("Could not verify temporary file:\n" + tempPath);
                    SafePost(ev);
                    return (ExitCode)0;
                }

                std::streamoff diskSize = verify.tellg();
                verify.close();

                if (diskSize < 0 || static_cast<long long>(diskSize) != received) {
                    QuietRemoveFileUtf8(tempPath);

                    auto* ev = new wxCommandEvent(wxEVT_DOWNLOAD_ERROR);
                    ev->SetString(
                        "Downloaded file size check failed.\nExpected " +
                        std::to_string(received) + " bytes on disk.");
                    SafePost(ev);
                    return (ExitCode)0;
                }
            }

            // ── Promote temp → final ────────────────────────────
            // Never remove the existing final model first. If replacement fails,
            // the old working .gguf must remain intact and the .download file is
            // left in place for troubleshooting or possible manual recovery.
            std::string promoteError;
            if (!PromoteDownloadUtf8(tempPath, m_destPath, &promoteError)) {
                auto* ev = new wxCommandEvent(wxEVT_DOWNLOAD_ERROR);

                std::string msg = "Could not save downloaded file to:\n" + m_destPath;
                if (!promoteError.empty()) {
                    msg += "\n\nReason: " + promoteError;
                }
                msg += "\n\nYour existing model file was not removed. The temporary download remains at:\n" + tempPath;

                ev->SetString(msg);
                SafePost(ev);
                return (ExitCode)0;
            }
            auto* ev = new wxCommandEvent(wxEVT_DOWNLOAD_COMPLETE);
            ev->SetString(wxString::FromUTF8(m_destPath));
            SafePost(ev);
            return (ExitCode)0;
        }

        // Exceeded redirect limit
        auto* ev = new wxCommandEvent(wxEVT_DOWNLOAD_ERROR);
        ev->SetString("Too many redirects — download failed");
        SafePost(ev);
    }
    catch (const Poco::Exception& ex)
    {
        QuietRemoveFileUtf8(tempPath);
        if (!m_cancelFlag->load()) {
            auto* ev = new wxCommandEvent(wxEVT_DOWNLOAD_ERROR);
            ev->SetString(FriendlyDownloadError(ex.displayText()));
            SafePost(ev);
        }
    }
    catch (const std::exception& ex)
    {
        QuietRemoveFileUtf8(tempPath);
        if (!m_cancelFlag->load()) {
            auto* ev = new wxCommandEvent(wxEVT_DOWNLOAD_ERROR);
            ev->SetString(FriendlyDownloadError(ex.what()));
            SafePost(ev);
        }
    }

    return (ExitCode)0;
}

// ═══════════════════════════════════════════════════════════════════
//  ProbeModelUrl — size + GGUF check before a pasted-link download
// ═══════════════════════════════════════════════════════════════════
//
// Asks for only the first 4 bytes (Range: bytes=0-3).  That follows the
// same redirects a real download does (Hugging Face -> its storage CDN,
// whose signed links accept GET but not always HEAD), returns the total
// size in Content-Range, and gives us the GGUF magic bytes -- so a bad
// link is rejected before a single gigabyte moves.  A server that
// ignores Range answers 200 with the whole file; we read 4 bytes and
// drop the connection.
ModelUrlProbe ProbeModelUrl(const std::string& url)
{
    lb::EnsureSSLInitialized();
    ModelUrlProbe out;
    try {
        std::string currentUrl = url;
        for (int hop = 0; hop <= 8; ++hop) {
            Poco::URI uri(currentUrl);
            if (uri.getScheme() != "https") {
                out.error = "The link redirected to a non-https address; download refused.";
                return out;
            }
            int port = uri.getPort();
            if (port == 0) port = 443;
            Poco::Net::HTTPSClientSession sess(uri.getHost(), static_cast<Poco::UInt16>(port));
            sess.setTimeout(Poco::Timespan(30, 0));

            std::string path = uri.getPathAndQuery();
            if (path.empty()) path = "/";
            Poco::Net::HTTPRequest req(Poco::Net::HTTPRequest::HTTP_GET, path,
                                       Poco::Net::HTTPMessage::HTTP_1_1);
            req.set("User-Agent", "LlamaBoss/1.0");
            req.set("Accept", "*/*");
            req.set("Range", "bytes=0-3");
            sess.sendRequest(req);

            Poco::Net::HTTPResponse resp;
            std::istream& in = sess.receiveResponse(resp);
            const int status = static_cast<int>(resp.getStatus());

            if (status == 301 || status == 302 || status == 303 ||
                status == 307 || status == 308) {
                if (!resp.has("Location")) { out.error = "Redirect with no Location header."; return out; }
                const std::string loc = resp.get("Location");
                if (loc.rfind("http", 0) != 0) {
                    Poco::URI base(currentUrl);
                    base.resolve(Poco::URI(loc));
                    currentUrl = base.toString();
                } else {
                    currentUrl = loc;
                }
                continue;
            }

            if (status == 206) {
                // Content-Range: bytes 0-3/123456789
                if (resp.has("Content-Range")) {
                    const std::string cr = resp.get("Content-Range");
                    const size_t slash = cr.rfind('/');
                    if (slash != std::string::npos) {
                        try { out.sizeBytes = std::stoll(cr.substr(slash + 1)); } catch (...) {}
                    }
                }
            } else if (status == 200) {
                if (resp.has("Content-Length")) {
                    try { out.sizeBytes = std::stoll(resp.get("Content-Length")); } catch (...) {}
                }
            } else {
                out.error = HttpFailureMessage(status, resp.getReason(), currentUrl);
                return out;
            }

            char head[4] = {};
            in.read(head, 4);
            if (!HasGgufMagic(std::string(head, static_cast<size_t>(in.gcount())))) {
                out.error = kNotGgufMessage;
                return out;
            }
            out.ok = true;
            return out;   // session destructor closes the socket
        }
        out.error = "Too many redirects.";
    }
    catch (const Poco::Exception& ex) {
        out.error = std::string(FriendlyDownloadError(ex.displayText()).utf8_str());
    }
    catch (const std::exception& ex) {
        out.error = std::string(FriendlyDownloadError(ex.what()).utf8_str());
    }
    return out;
}

// ═══════════════════════════════════════════════════════════════════
//  ModelDownloaderDialog — construction
// ═══════════════════════════════════════════════════════════════════

ModelDownloaderDialog::ModelDownloaderDialog(wxWindow* parent,
                                             const ThemeData* theme,
                                             bool firstRunMode)
    : wxDialog(parent, wxID_ANY,
               firstRunMode ? "Welcome to LlamaBoss" : "Download Models",
               wxDefaultPosition, wxSize(660, 560),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_theme(theme)
    , m_handlerAlive(std::make_shared<std::atomic<bool>>(true))
    , m_firstRunMode(firstRunMode)
    , m_autoCloseTimer(this)
{
    // Build display order: starter first in first-run mode, catalog order
    // otherwise. m_rows is still indexed by catalog position, so existing
    // event handlers (OnDownloadClicked, OnDownloadComplete, etc.) work
    // unchanged — only the visual order of rows differs.
    m_displayOrder.reserve(kModels.size());
    if (m_firstRunMode) {
        // Starter(s) first, preserving relative order within each group.
        for (size_t i = 0; i < kModels.size(); ++i)
            if (kModels[i].firstRunStarter) m_displayOrder.push_back(i);
        for (size_t i = 0; i < kModels.size(); ++i)
            if (!kModels[i].firstRunStarter) m_displayOrder.push_back(i);
    } else {
        for (size_t i = 0; i < kModels.size(); ++i)
            m_displayOrder.push_back(i);
    }

    BuildUI();
    CentreOnParent();

    Bind(wxEVT_DOWNLOAD_PROGRESS, &ModelDownloaderDialog::OnDownloadProgress, this);
    Bind(wxEVT_DOWNLOAD_COMPLETE, &ModelDownloaderDialog::OnDownloadComplete, this);
    Bind(wxEVT_DOWNLOAD_ERROR,    &ModelDownloaderDialog::OnDownloadError,    this);
    Bind(wxEVT_CLOSE_WINDOW,      &ModelDownloaderDialog::OnClose,            this);
    Bind(wxEVT_TIMER,             &ModelDownloaderDialog::OnAutoCloseTimer,   this,
         m_autoCloseTimer.GetId());
}

ModelDownloaderDialog::~ModelDownloaderDialog()
{
    // Order matters: flip the handler-alive sentinel BEFORE setting the
    // cancel flag. If we cancel first, a progress/complete event could
    // already be on its way through SafePost — the cancel check inside
    // that function is a no-op once wxQueueEvent has been called. By
    // flipping m_handlerAlive first, any worker thread that's about to
    // post sees a dead handler and bails out cleanly.
    LbMarkUiEventTargetDead(m_handlerAlive);
    if (m_cancelFlag)   m_cancelFlag->store(true);
}

// ─────────────────────────────────────────────────────────────────
//  UI construction
// ─────────────────────────────────────────────────────────────────

void ModelDownloaderDialog::BuildUI()
{
    const wxColour dialogSurface = m_theme ? m_theme->bgDialogSurface : GetBackgroundColour();
    const wxColour bgToolbar     = m_theme ? m_theme->bgToolbar       : GetBackgroundColour();
    const wxColour textPri   = m_theme ? m_theme->textPrimary : GetForegroundColour();
    const wxColour textMuted = m_theme ? m_theme->textMuted   : wxColour(128,128,128);
    const wxColour border    = m_theme ? m_theme->borderSubtle: wxColour(200,200,200);

    if (m_theme) SetBackgroundColour(dialogSurface);

    auto* outer = new wxBoxSizer(wxVERTICAL);

    // ── Header panel ─────────────────────────────────────────────
    auto* hdrPanel = new wxPanel(this);
    hdrPanel->SetBackgroundColour(bgToolbar);
    auto* hdrSizer = new wxBoxSizer(wxVERTICAL);

    // Title + subtitle copy shifts in first-run mode to frame the screen
    // as an onboarding step rather than a neutral download utility.
    // Returning users still see the plain "Download Models" heading.
    const char* titleText = m_firstRunMode
        ? "Welcome to LlamaBoss"
        : "Download Models";
    auto* titleLbl = new wxStaticText(hdrPanel, wxID_ANY, titleText);
    wxFont tf = titleLbl->GetFont();
    tf.SetPointSize(11); tf.SetWeight(wxFONTWEIGHT_BOLD);
    titleLbl->SetFont(tf);
    titleLbl->SetForegroundColour(textPri);

    std::string subText;
    if (m_firstRunMode) {
        subText =
            "LlamaBoss runs AI models locally on your computer — nothing leaves this device.\n"
            "Pick a model below to get started. We recommend the one marked \"Start here\".";
    } else {
        subText =
            "All models are free to download. No HuggingFace account required.\n"
            "Files are saved to:  " + ServerManager::GetModelsDir();
    }
    auto* subLbl = new wxStaticText(hdrPanel, wxID_ANY,
        wxString::FromUTF8(subText));
    subLbl->SetForegroundColour(textMuted);

    hdrSizer->Add(titleLbl, 0, wxLEFT | wxTOP | wxRIGHT, 14);
    hdrSizer->AddSpacer(4);
    hdrSizer->Add(subLbl,   0, wxLEFT | wxBOTTOM | wxRIGHT, 14);
    hdrPanel->SetSizer(hdrSizer);
    outer->Add(hdrPanel, 0, wxEXPAND);

    // Header separator
    auto* hdrSep = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1,1));
    hdrSep->SetBackgroundColour(border);
    outer->Add(hdrSep, 0, wxEXPAND);

    // ── Scrolled list ────────────────────────────────────────────
    m_scroll = new wxScrolledWindow(this, wxID_ANY,
        wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxBORDER_NONE);
    m_scroll->SetScrollRate(0, 14);
    m_scroll->SetBackgroundColour(dialogSurface);

    auto* listSizer = new wxBoxSizer(wxVERTICAL);
    m_rows.resize(kModels.size());

    // Iterate m_displayOrder, not the catalog directly. In first-run mode
    // this floats the starter model to the top while preserving m_rows'
    // indexing by catalog position (so click handlers stay correct).
    for (size_t pos = 0; pos < m_displayOrder.size(); ++pos) {
        size_t catalogIdx = m_displayOrder[pos];
        BuildModelRow(listSizer, catalogIdx, kModels[catalogIdx]);
        // Thin separator between rows (not after the last one)
        if (pos + 1 < m_displayOrder.size()) {
            auto* rowSep = new wxPanel(m_scroll, wxID_ANY,
                wxDefaultPosition, wxSize(-1,1));
            rowSep->SetBackgroundColour(border);
            listSizer->Add(rowSep, 0, wxEXPAND);
        }
    }
    listSizer->AddSpacer(8);

    m_scroll->SetSizer(listSizer);
    m_scroll->FitInside();
    outer->Add(m_scroll, 1, wxEXPAND);

    // Bottom separator
    auto* botSep = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1,1));
    botSep->SetBackgroundColour(border);
    outer->Add(botSep, 0, wxEXPAND);

    // ── Close button bar ─────────────────────────────────────────
    auto* btnPanel = new wxPanel(this);
    btnPanel->SetBackgroundColour(dialogSurface);
    auto* btnSizer = new wxBoxSizer(wxHORIZONTAL);
    btnSizer->AddStretchSpacer();
    const ThemeData fallback = ThemeManager::GetDarkTheme();
    const ThemeData& tCloseTheme = m_theme ? *m_theme : fallback;
    auto* closeBtn = MakeFlatButton(btnPanel, wxID_CANCEL, "Close",
                                    tCloseTheme);
    btnSizer->Add(closeBtn, 0, wxALIGN_CENTER_VERTICAL | wxALL, 14);
    btnPanel->SetSizer(btnSizer);
    outer->Add(btnPanel, 0, wxEXPAND);

    SetSizer(outer);
}

void ModelDownloaderDialog::BuildModelRow(wxSizer* listSizer,
                                           size_t idx,
                                           const DownloadableModel& model)
{
    const wxColour dialogSurface = m_theme ? m_theme->bgDialogSurface : GetBackgroundColour();
    const wxColour textPri       = m_theme ? m_theme->textPrimary     : GetForegroundColour();
    const wxColour textMuted = m_theme ? m_theme->textMuted    : wxColour(128,128,128);
    const wxColour accent    = m_theme ? m_theme->accentButton : wxColour(60,120,220);

    ModelRow& row = m_rows[idx];
    row.rowPanel = new wxPanel(m_scroll);
    row.rowPanel->SetBackgroundColour(dialogSurface);

    auto* rowSizer = new wxBoxSizer(wxVERTICAL);

    // ── Top line: name · tag ── [Start here badge] ── size ── [button] ──
    auto* topLine = new wxBoxSizer(wxHORIZONTAL);

    std::string nameStr = model.displayName
        + "  \xe2\x80\xa2  "   // · (UTF-8 bullet)
        + model.tag;

    // Sub-sizer for "name + optional badge" so they sit side-by-side
    // without the badge being shoved to the far right by the name's
    // flex weight. The sub-sizer takes the flex instead.
    auto* nameArea = new wxBoxSizer(wxHORIZONTAL);

    // EscapeMnemonics: wxStaticText treats '&' as an accelerator marker,
    // which ate the ampersand in "Fast & Powerful" (rendered "Fast  Powerful").
    row.nameLabel = new wxStaticText(row.rowPanel, wxID_ANY,
        wxControl::EscapeMnemonics(wxString::FromUTF8(nameStr)));
    wxFont nf = row.nameLabel->GetFont();
    nf.SetWeight(wxFONTWEIGHT_BOLD);
    row.nameLabel->SetFont(nf);
    row.nameLabel->SetForegroundColour(textPri);
    nameArea->Add(row.nameLabel, 0, wxALIGN_CENTER_VERTICAL);

    // "Start here" badge — only in first-run mode, only on the starter
    // entry. Rendered as bold accent-colored text immediately to the
    // right of the model name so the visual weight sits with the model
    // being recommended, not floating independently.
    if (m_firstRunMode && model.firstRunStarter) {
        auto* starterBadge = new wxStaticText(row.rowPanel, wxID_ANY,
            "Start here");
        wxFont sbf = starterBadge->GetFont();
        sbf.SetWeight(wxFONTWEIGHT_BOLD);
        starterBadge->SetFont(sbf);
        starterBadge->SetForegroundColour(accent);
        nameArea->Add(starterBadge, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 12);
    }

    topLine->Add(nameArea, 1, wxALIGN_CENTER_VERTICAL);

    row.sizeLabel = new wxStaticText(row.rowPanel, wxID_ANY,
        wxString::FromUTF8(model.sizeDisplay));
    row.sizeLabel->SetForegroundColour(textMuted);
    topLine->Add(row.sizeLabel, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 14);

    // Single button — label and palette change with state. Handler
    // checks row state so we never need to rebind. Width is pinned so
    // the column stays aligned regardless of label length
    // ("Download" vs "✓ Downloaded" vs "Retry").
    bool alreadyDone = IsAlreadyDownloaded(model);
    row.downloaded   = alreadyDone;

    const ThemeData fallback = ThemeManager::GetDarkTheme();
    const ThemeData& t = m_theme ? *m_theme : fallback;

    // Weights present but the vision projector missing (an earlier
    // projector download failed or was cancelled): offer to fetch only
    // the missing part instead of presenting the model as complete.
    const bool visionMissing = !alreadyDone && IsWeightsDownloaded(model);

    std::string btnLabel = alreadyDone
        ? "\xe2\x9c\x93 Downloaded"
        : (visionMissing ? "Get vision" : "Download");

    if (alreadyDone) {
        // Already-done rows start in the flat-muted disabled state.
        row.actionBtn = MakeFlatButton(row.rowPanel, wxID_ANY,
            wxString::FromUTF8(btnLabel), t, 32);
        row.actionBtn->Enable(false);
    } else {
        // Available rows start in the accent state.
        row.actionBtn = MakeAccentButton(row.rowPanel, wxID_ANY,
            wxString::FromUTF8(btnLabel), t, 32);
    }
    row.actionBtn->SetMinSize(wxSize(140, 32));

    // Single permanent handler — checks state at click time
    row.actionBtn->Bind(wxEVT_BUTTON, [this, idx](wxCommandEvent&) {
        if (m_rows[idx].downloading)
            OnCancelClicked();
        else if (!m_rows[idx].downloaded)
            OnDownloadClicked(idx);
    });

    topLine->Add(row.actionBtn, 0, wxALIGN_CENTER_VERTICAL);
    rowSizer->Add(topLine, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 14);

    // ── Description / status line ────────────────────────────────
    row.statusLabel = new wxStaticText(row.rowPanel, wxID_ANY,
        wxString::FromUTF8(visionMissing
            ? "Model downloaded; vision component missing (" +
              model.mmprojSizeDisplay + "). Text chat works now."
            : model.description),
        wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    row.statusLabel->SetForegroundColour(textMuted);
    rowSizer->Add(row.statusLabel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 14);

    // ── Progress gauge (hidden until download starts) ─────────────
    row.gauge = new wxGauge(row.rowPanel, wxID_ANY, 100,
        wxDefaultPosition, wxSize(-1, 8));
    row.gauge->Hide();
    rowSizer->Add(row.gauge, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);

    rowSizer->AddSpacer(14);
    row.rowPanel->SetSizer(rowSizer);
    listSizer->Add(row.rowPanel, 0, wxEXPAND);
}

// ─────────────────────────────────────────────────────────────────
//  Row state transitions
// ─────────────────────────────────────────────────────────────────

void ModelDownloaderDialog::SetRowDownloading(size_t idx)
{
    ModelRow& row = m_rows[idx];
    const ThemeData fallback = ThemeManager::GetDarkTheme();
    const ThemeData& t = m_theme ? *m_theme : fallback;

    row.downloading = true;

    row.gauge->SetValue(0);
    row.gauge->Show();
    row.statusLabel->SetLabel("Connecting...");
    row.statusLabel->SetForegroundColour(t.textMuted);

    // Flip the action button to "Cancel" with the destructive (red)
    // palette — solid red signals the stop nature without surrendering
    // the click target. Same widget, fresh palette + label.
    row.actionBtn->SetLabel("Cancel");
    row.actionBtn->Enable(true);
    TintDestructive(row.actionBtn, t);

    row.rowPanel->Layout();
    m_scroll->FitInside();
}

void ModelDownloaderDialog::SetRowComplete(size_t idx)
{
    ModelRow& row = m_rows[idx];
    const ThemeData fallback = ThemeManager::GetDarkTheme();
    const ThemeData& t = m_theme ? *m_theme : fallback;
    const wxColour success = m_theme ? m_theme->chatAssistant
                                     : wxColour(80,180,80);

    row.downloading = false;
    row.downloaded  = true;

    row.gauge->Hide();
    row.statusLabel->SetLabel(wxString::FromUTF8("\xe2\x9c\x93 Downloaded successfully"));
    row.statusLabel->SetForegroundColour(success);

    // Settled state: flat muted, disabled. The "✓ Downloaded" label
    // reads as a finished marker rather than an actionable button.
    row.actionBtn->SetLabel(wxString::FromUTF8("\xe2\x9c\x93 Downloaded"));
    TintFlatMuted(row.actionBtn, t);
    row.actionBtn->Enable(false);

    row.rowPanel->Layout();
    m_scroll->FitInside();
}

void ModelDownloaderDialog::SetRowError(size_t idx, const std::string& msg)
{
    ModelRow& row = m_rows[idx];
    const ThemeData fallback = ThemeManager::GetDarkTheme();
    const ThemeData& t = m_theme ? *m_theme : fallback;

    row.downloading = false;

    row.gauge->Hide();
    row.statusLabel->SetLabel("Error: " + msg);
    row.statusLabel->SetForegroundColour(wxColour(200, 60, 60));

    // Retry — back to accent palette, "Retry" label.
    row.actionBtn->SetLabel("Retry");
    row.actionBtn->Enable(true);
    TintAccent(row.actionBtn, t);

    row.rowPanel->Layout();
    m_scroll->FitInside();
}

// ─────────────────────────────────────────────────────────────────
//  Download control
// ─────────────────────────────────────────────────────────────────

void ModelDownloaderDialog::OnDownloadClicked(size_t idx)
{
    if (m_activeRow >= 0) {
        // Another download is already running — tell the user
        wxMessageBox("Please wait for the current download to finish\n"
                     "or cancel it before starting another.",
                     "Download in Progress", wxOK | wxICON_INFORMATION, this);
        return;
    }

    const DownloadableModel& model = kModels[idx];

    // Weights already on disk and only the projector missing (a failed
    // or cancelled vision stage, now or in an earlier session): resume
    // at the projector instead of re-downloading the multi-GB weights.
    if (!model.mmprojFilename.empty() &&
        IsWeightsDownloaded(model) && !IsProjectorDownloaded(model)) {
        wxFileName mmFn = wxFileName::FileName(
            wxString::FromUTF8(BuildMmprojDestPath(model)));
        wxFileName::Mkdir(mmFn.GetPath(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);

        m_activeRow = static_cast<int>(idx);
        SetRowDownloading(idx);
        StartMmprojStage(idx);
        return;
    }

    std::string url      = BuildUrl(model);
    std::string destPath = BuildDestPath(model);

    // Ensure the destination directory exists. In casual mode this is
    // the bundle subfolder (created here, per-model); in power mode
    // this is the shared models root. wxPATH_MKDIR_FULL handles both
    // by creating parents as needed — idempotent if already present.
    wxFileName destFn = wxFileName::FileName(wxString::FromUTF8(destPath));
    wxFileName::Mkdir(destFn.GetPath(), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);

    m_activeRow  = static_cast<int>(idx);
    m_cancelFlag = std::make_shared<std::atomic<bool>>(false);
    // Reset the mmproj flag — this is a fresh download sequence, and
    // the .gguf comes first regardless of whether an mmproj is chained.
    m_downloadingMmproj = false;

    // New operation: bump the generation so any still-unwinding worker
    // from a just-cancelled download can no longer post into this row,
    // and so this worker's temp file cannot collide with the old one's.
    ++m_downloadGeneration;

    SetRowDownloading(idx);

    auto* thread = new DownloadThread(
        this, url, destPath, model.sizeBytes,
        m_cancelFlag,
        std::weak_ptr<std::atomic<bool>>(m_handlerAlive),
        m_downloadGeneration);

    if (thread->Run() != wxTHREAD_NO_ERROR) {
        delete thread;
        m_cancelFlag.reset();
        m_activeRow = -1;
        SetRowError(idx, "Failed to start download thread");
    }
}

bool ModelDownloaderDialog::StartMmprojStage(size_t idx)
{
    const DownloadableModel& model = kModels[idx];

    m_downloadingMmproj = true;
    m_cancelFlag = std::make_shared<std::atomic<bool>>(false);

    // Chained-but-distinct operation: give the projector stage its own
    // generation so a cancel during the weights stage cannot let its
    // worker post into the mmproj stage (or share its temp path).
    ++m_downloadGeneration;

    // Show the sub-stage to the user so progress makes sense.
    ModelRow& row = m_rows[idx];
    row.statusLabel->SetLabel(wxString::FromUTF8(
        "Downloading vision component (" + model.mmprojSizeDisplay + ")..."));
    row.gauge->SetValue(0);
    row.gauge->Show();
    row.rowPanel->Layout();

    auto* thread = new DownloadThread(
        this,
        BuildMmprojUrl(model),
        BuildMmprojDestPath(model),
        model.mmprojSizeBytes,
        m_cancelFlag,
        std::weak_ptr<std::atomic<bool>>(m_handlerAlive),
        m_downloadGeneration);

    if (thread->Run() != wxTHREAD_NO_ERROR) {
        delete thread;
        m_cancelFlag.reset();
        m_downloadingMmproj = false;
        m_activeRow = -1;
        // Not complete: don't mark the row "✓ Downloaded".  The weights
        // stay on disk, so Retry fetches only the projector.
        SetRowError(idx, "vision component could not start. The model "
                         "works for text; Retry fetches only the vision part.");
        return false;
    }
    return true;
}

void ModelDownloaderDialog::OnCancelClicked()
{
    if (m_activeRow < 0) return;

    if (m_cancelFlag) m_cancelFlag->store(true);
    m_cancelFlag.reset();

    size_t idx  = static_cast<size_t>(m_activeRow);
    m_activeRow = -1;

    // Restore row to its original idle state
    ModelRow& row = m_rows[idx];
    const ThemeData fallback = ThemeManager::GetDarkTheme();
    const ThemeData& t = m_theme ? *m_theme : fallback;

    row.downloading = false;
    row.gauge->Hide();
    row.statusLabel->SetLabel(wxString::FromUTF8(kModels[idx].description));
    row.statusLabel->SetForegroundColour(t.textMuted);
    row.actionBtn->SetLabel("Download");
    row.actionBtn->Enable(true);
    TintAccent(row.actionBtn, t);
    row.rowPanel->Layout();
    m_scroll->FitInside();
}

// ─────────────────────────────────────────────────────────────────
//  Thread event handlers
// ─────────────────────────────────────────────────────────────────

void ModelDownloaderDialog::OnDownloadProgress(wxCommandEvent& ev)
{
    if (m_activeRow < 0) return;
    // Ignore a stale event from a cancelled/superseded worker.
    if (ev.GetInt() != static_cast<int>(m_downloadGeneration)) return;
    ModelRow& row = m_rows[static_cast<size_t>(m_activeRow)];

    int pct = static_cast<int>(ev.GetExtraLong());
    row.gauge->SetValue(std::max(0, std::min(100, pct)));

    // Decode "receivedBytes|totalBytes" from the event string
    long long received = 0, total = 0;
    wxString data = ev.GetString();
    data.BeforeFirst('|').ToLongLong(&received);
    data.AfterFirst('|').ToLongLong(&total);

    wxString label;
    if (total > 0)
        label = wxString::Format("%s / %s  (%d%%)",
            FormatBytes(received), FormatBytes(total), pct);
    else
        label = wxString::Format("%s downloaded", FormatBytes(received));

    row.statusLabel->SetLabel(label);
}

void ModelDownloaderDialog::OnDownloadComplete(wxCommandEvent& ev)
{
    if (m_activeRow < 0) return;
    // Ignore a stale completion from a cancelled/superseded worker — it
    // must not mark a later row complete or chain that row's mmproj.
    if (ev.GetInt() != static_cast<int>(m_downloadGeneration)) return;
    size_t idx  = static_cast<size_t>(m_activeRow);

    // If we just finished the main .gguf and the catalog entry ships an
    // mmproj (vision model), chain into the projector download without
    // releasing the active-row lock. The user sees one continuous flow:
    // "Downloading weights 4.2 GB / 7.8 GB" → "Downloading vision 0.4 GB / 0.8 GB"
    // → "Downloaded successfully". They never type the word "mmproj".
    const DownloadableModel& model = kModels[idx];
    const bool needsMmproj = !m_downloadingMmproj && !model.mmprojFilename.empty();

    if (needsMmproj) {
        // The weights are on disk and loadable for text chat, so this is
        // already a successful download for the caller (Settings refreshes
        // its model list on it), whatever happens to the projector stage.
        m_hadSuccess = true;
        StartMmprojStage(idx);
        return;
    }

    // Either a text-only entry or we just finished the mmproj stage.
    // In both cases the row is fully done.
    m_activeRow = -1;
    m_downloadingMmproj = false;
    m_cancelFlag.reset();
    m_hadSuccess = true;
    SetRowComplete(idx);

    // ── First-run handoff ────────────────────────────────────────
    // Capture the path of the weights we just downloaded (always the
    // .gguf, never the mmproj — the mmproj path is a companion to the
    // weights, not what gets loaded by name). Then start a 1-second
    // timer so the user has a beat to register the "✓ Downloaded
    // successfully" state before the dialog closes itself and the
    // caller kicks off model load. Outside first-run mode the user
    // dismisses the dialog manually.
    if (m_firstRunMode) {
        m_downloadedPath = BuildDestPath(kModels[idx]);
        m_autoCloseTimer.StartOnce(1000);
    }
}

void ModelDownloaderDialog::OnDownloadError(wxCommandEvent& ev)
{
    if (m_activeRow < 0) return;
    // Ignore a stale error from a cancelled/superseded worker — otherwise
    // a worker dying on the abort could flip a later download's row to
    // "Retry" mid-flight.
    if (ev.GetInt() != static_cast<int>(m_downloadGeneration)) return;
    size_t idx  = static_cast<size_t>(m_activeRow);
    m_activeRow = -1;
    m_cancelFlag.reset();
    SetRowError(idx, std::string(ev.GetString().ToUTF8().data()));
}

void ModelDownloaderDialog::OnClose(wxCloseEvent& ev)
{
    LbMarkUiEventTargetDead(m_handlerAlive);
    if (m_cancelFlag) m_cancelFlag->store(true);
    ev.Skip();
}

// ─────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────

bool ModelDownloaderDialog::IsWeightsDownloaded(const DownloadableModel& m) const
{
    return wxFileExists(wxString::FromUTF8(BuildDestPath(m)));
}

bool ModelDownloaderDialog::IsProjectorDownloaded(const DownloadableModel& m) const
{
    if (m.mmprojFilename.empty()) return true;   // text-only: nothing to fetch
    return wxFileExists(wxString::FromUTF8(BuildMmprojDestPath(m)));
}

bool ModelDownloaderDialog::IsAlreadyDownloaded(const DownloadableModel& m) const
{
    // Both components.  Checking only the .gguf marked a vision model
    // whose projector failed or was cancelled as "✓ Downloaded" with the
    // button disabled, leaving no way to fetch the projector.
    return IsWeightsDownloaded(m) && IsProjectorDownloaded(m);
}

std::string ModelDownloaderDialog::BuildUrl(const DownloadableModel& m) const
{
    return "https://huggingface.co/"
         + m.author + "/" + m.repo
         + "/resolve/main/" + m.filename;
}

// Bundle folder name for a catalog entry. Derived from the .gguf stem
// (filename minus extension) so the bundle folder name matches what
// ModelDisplayName will return once the model is loaded — the user sees
// one consistent name from download → sidebar → About dialog.
static std::string BundleNameFor(const DownloadableModel& m)
{
    std::string stem = m.filename;
    size_t dot = stem.rfind('.');
    if (dot != std::string::npos) stem = stem.substr(0, dot);
    return stem;
}

std::string ModelDownloaderDialog::BuildDestPath(const DownloadableModel& m) const
{
    // Casual mode: drop the .gguf into its own bundle subfolder alongside
    // any mmproj. This gives vision pairing deterministic semantics and
    // lets the UI display a clean bundle name.
    //
    // Power mode (user set a custom folder): save loose. Power users
    // organize their own way — we stay out of it.
    std::string root = ServerManager::GetModelsDir();
    const char sep = static_cast<char>(wxFILE_SEP_PATH);

    if (ServerManager::IsCasualMode()) {
        return root + sep + BundleNameFor(m) + sep + m.filename;
    }
    return root + sep + m.filename;
}

// URL + destination for the optional mmproj projector. Returns "" when
// the catalog entry has no mmproj (text-only model).
static std::string BuildMmprojUrl(const DownloadableModel& m)
{
    if (m.mmprojFilename.empty()) return "";
    return "https://huggingface.co/"
         + m.author + "/" + m.repo
         + "/resolve/main/" + m.mmprojFilename;
}

static std::string BuildMmprojDestPath(const DownloadableModel& m)
{
    if (m.mmprojFilename.empty()) return "";
    std::string root = ServerManager::GetModelsDir();
    const char sep = static_cast<char>(wxFILE_SEP_PATH);

    // In casual mode the mmproj lives inside the model's bundle folder —
    // same place as the .gguf — so pairing is unambiguous. In power
    // mode it lands loose at the root with a predictable name, and
    // the existing filename-heuristic pairing will find it.
    if (ServerManager::IsCasualMode()) {
        return root + sep + BundleNameFor(m) + sep + m.mmprojFilename;
    }
    return root + sep + m.mmprojFilename;
}

std::string ModelDownloaderDialog::FormatBytes(long long bytes) const
{
    if (bytes < 0) return "?";
    const char* units[] = { "B", "KB", "MB", "GB" };
    double val = static_cast<double>(bytes);
    int idx = 0;
    while (val >= 1024.0 && idx < 3) { val /= 1024.0; ++idx; }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(idx == 0 ? 0 : 1)
        << val << " " << units[idx];
    return oss.str();
}

// ─────────────────────────────────────────────────────────────────
//  First-run auto-close
// ─────────────────────────────────────────────────────────────────
//
// Fired once, 1 second after a download reaches its terminal success
// state in first-run mode. That delay is deliberate — it's long enough
// for the user to see the row flip to "✓ Downloaded successfully"
// (registering the win) but short enough to feel like the app is taking
// them somewhere, not making them wait. EndModal returns wxID_OK to the
// caller, who reads GetDownloadedModelPath() and drives model-load.
void ModelDownloaderDialog::OnAutoCloseTimer(wxTimerEvent&)
{
    if (IsModal())
        EndModal(wxID_OK);
    else
        Close();
}
