#define _CRT_SECURE_NO_WARNINGS

// model_downloader.cpp
// Curated model catalog + HTTPS download dialog for LlamaBoss.
// Models are sourced from bartowski on HuggingFace — no account required.

#include "model_downloader.h"
#include "server_manager.h"
#include "theme.h"
#include "path_safety.h"

#include <wx/filename.h>
#include <wx/log.h>

// Poco HTTPS
#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPSClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/Net/HTTPMessage.h>
#include <Poco/Net/SSLManager.h>
#include <Poco/Net/AcceptCertificateHandler.h>
#include <Poco/Net/Context.h>
#include <Poco/Net/NetSSL.h>
#include <Poco/URI.h>
#include <Poco/Exception.h>

#include <fstream>
#include <mutex>
#include <sstream>
#include "ui_event_post.h"
#include <iomanip>
#include <algorithm>

#ifdef __WXMSW__
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#endif

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
//  account or license gate required.  Bartowski's newer uploads use
//  the "<org>_" repo prefix (google_, Qwen_, openai_); the older
//  Llama 3.2 entry keeps its original repo name.
//
//  Q4_K_M is the chosen quant for every entry — well-rounded quality
//  vs size, default-recommended by bartowski himself, and works on
//  every llama.cpp backend including Vulkan (unlike I-quants which
//  the Vulkan backend cannot run).
//
//  Exception: gpt-oss ships as MXFP4. Its feed-forward weights don't
//  quantize well to anything else, so bartowski keeps the FFNs at
//  MXFP4 in every quant — making MXFP4 the canonical file (all the
//  other quants of that repo are the same size anyway). Smoke-test
//  against the llama.cpp Vulkan backend before shipping this entry.
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
        "3.1 GB", 3'100'000'000LL,
        "mmproj-google_gemma-4-E2B-it-f16.gguf", "0.8 GB", 800'000'000LL
    },
    {
        "Gemma 4 E4B",   "Recommended",
        "Multimodal — vision and audio. Best balance of speed and quality for most users.",
        "bartowski", "google_gemma-4-E4B-it-GGUF",
        "google_gemma-4-E4B-it-Q4_K_M.gguf",
        "5.0 GB", 5'000'000'000LL,
        "mmproj-google_gemma-4-E4B-it-f16.gguf", "1.0 GB", 1'000'000'000LL
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
        "Mixture-of-Experts — 26B knowledge at 4B speed. Vision-capable. Needs 16+ GB VRAM.",
        "bartowski", "google_gemma-4-26B-A4B-it-GGUF",
        "google_gemma-4-26B-A4B-it-Q4_K_M.gguf",
        "16.0 GB", 16'000'000'000LL,
        "mmproj-google_gemma-4-26B-A4B-it-f16.gguf", "1.2 GB", 1'200'000'000LL
    },
    {
        "Qwen 3.6 27B",  "Top All-Rounder",
        "Alibaba's flagship. Frontier quality, vision-capable. Needs 20+ GB VRAM.",
        "bartowski", "Qwen_Qwen3.6-27B-GGUF",
        "Qwen_Qwen3.6-27B-Q4_K_M.gguf",
        "17.5 GB", 17'530'000'000LL,
        // NOTE: this repo's projector is bf16, not the f16 naming the
        // Gemma entries use. Size below is approximate — confirm on
        // first test download and tighten if needed.
        "mmproj-Qwen_Qwen3.6-27B-bf16.gguf", "1.5 GB", 1'500'000'000LL
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
//  SSL — initialized once for the lifetime of the process
// ═══════════════════════════════════════════════════════════════════
static void EnsureSSLInitialized()
{
    static std::once_flag s_flag;
    std::call_once(s_flag, []()
    {
        Poco::Net::initializeSSL();

        // Practical Windows/default for the current LlamaBoss downloader:
        // allow downloads to keep working on corporate/work laptops where
        // HTTPS traffic may be inspected by a local security proxy.
        //
        // The 4/21 build used this tolerant behavior and downloaded correctly
        // on Cesar's work laptop. The stricter RejectCertificateHandler +
        // VERIFY_STRICT version can fail with:
        //   SSL routines::certificate verify failed
        // even when the browser can download the same model.
        //
        // Future hardening path: switch model downloads to WinHTTP/WinINet or
        // add a user-visible Advanced setting plus checksums. For now, restore
        // the known-working behavior and keep the UI error handling clean.
        Poco::SharedPtr<Poco::Net::InvalidCertificateHandler> pCert(
            new Poco::Net::AcceptCertificateHandler(false));

        Poco::Net::Context::Ptr pCtx(new Poco::Net::Context(
            Poco::Net::Context::CLIENT_USE, ""));

        Poco::Net::SSLManager::instance().initializeClient(
            nullptr, pCert, pCtx);
    });
}

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
    // pre-deleting the destination. This keeps the old behavior portable while
    // avoiding the destructive remove-first sequence.
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
// ═══════════════════════════════════════════════════════════════════
//  DownloadThread
// ═══════════════════════════════════════════════════════════════════

DownloadThread::DownloadThread(wxEvtHandler*   handler,
                               const std::string& url,
                               const std::string& destPath,
                               long long          expectedBytes,
                               std::shared_ptr<std::atomic<bool>> cancelFlag,
                               std::weak_ptr<std::atomic<bool>>   aliveToken)
    : wxThread(wxTHREAD_DETACHED)
    , m_handler(handler)
    , m_url(url)
    , m_destPath(destPath)
    , m_expectedBytes(expectedBytes)
    , m_cancelFlag(cancelFlag)
    , m_aliveToken(aliveToken)
{}

bool DownloadThread::SafePost(wxCommandEvent* ev)
{
    // Cancelled? Drop the event on the floor.
    if (m_cancelFlag->load()) { delete ev; return false; }

    // Verify the handler is still alive and queue under the shared UI-post
    // mutex. This closes the old check-then-wxQueueEvent race when the
    // dialog is being destroyed while a detached download thread finishes.
    return LbQueueEventIfAlive(m_handler, m_aliveToken, ev);
}

wxThread::ExitCode DownloadThread::Entry()
{
    EnsureSSLInitialized();

    std::string tempPath = m_destPath + ".download";

    try
    {
        std::string currentUrl = m_url;
        const int kMaxHops = 8;

        for (int hop = 0; hop <= kMaxHops; ++hop)
        {
            if (m_cancelFlag->load()) return (ExitCode)0;

            Poco::URI uri(currentUrl);
            std::string scheme = uri.getScheme();
            int port = uri.getPort();
            if (port == 0) port = (scheme == "https") ? 443 : 80;

            // ── Open session ─────────────────────────────────────
            std::unique_ptr<Poco::Net::HTTPClientSession> sess;
            if (scheme == "https") {
                auto* s = new Poco::Net::HTTPSClientSession(uri.getHost(), port);
                s->setTimeout(Poco::Timespan(60, 0));
                sess.reset(s);
            } else {
                auto* s = new Poco::Net::HTTPClientSession(uri.getHost(), port);
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
                ev->SetString("Server returned HTTP "
                    + std::to_string(status) + " " + resp.getReason());
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

            std::ofstream out(path_safety::Utf8ToWide(tempPath), std::ios::binary | std::ios::trunc);
            if (!out.is_open()) {
                auto* ev = new wxCommandEvent(wxEVT_DOWNLOAD_ERROR);
                ev->SetString("Cannot create temporary file:\n" + tempPath);
                SafePost(ev);
                return (ExitCode)0;
            }

            char      buf[65536];
            long long received   = 0;
            long long lastReport = -1;

            while (!m_cancelFlag->load())
            {
                in.read(buf, sizeof(buf));
                std::streamsize n = in.gcount();

                if (n > 0) {
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

            // Second integrity check: verify the temp file on disk matches what
            // we believe we wrote before promoting it to the final .gguf path.
            {
                std::ifstream verify(path_safety::Utf8ToWide(tempPath), std::ios::binary | std::ios::ate);
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

    row.nameLabel = new wxStaticText(row.rowPanel, wxID_ANY,
        wxString::FromUTF8(nameStr));
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

    std::string btnLabel = alreadyDone
        ? "\xe2\x9c\x93 Downloaded" : "Download";

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
        wxString::FromUTF8(model.description),
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

    SetRowDownloading(idx);

    auto* thread = new DownloadThread(
        this, url, destPath, model.sizeBytes,
        m_cancelFlag,
        std::weak_ptr<std::atomic<bool>>(m_handlerAlive));

    if (thread->Run() != wxTHREAD_NO_ERROR) {
        delete thread;
        m_cancelFlag.reset();
        m_activeRow = -1;
        SetRowError(idx, "Failed to start download thread");
    }
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

void ModelDownloaderDialog::OnDownloadComplete(wxCommandEvent&)
{
    if (m_activeRow < 0) return;
    size_t idx  = static_cast<size_t>(m_activeRow);

    // If we just finished the main .gguf and the catalog entry ships an
    // mmproj (vision model), chain into the projector download without
    // releasing the active-row lock. The user sees one continuous flow:
    // "Downloading weights 4.2 GB / 7.8 GB" → "Downloading vision 0.4 GB / 0.8 GB"
    // → "Downloaded successfully". They never type the word "mmproj".
    const DownloadableModel& model = kModels[idx];
    const bool needsMmproj = !m_downloadingMmproj && !model.mmprojFilename.empty();

    if (needsMmproj) {
        m_downloadingMmproj = true;
        m_cancelFlag = std::make_shared<std::atomic<bool>>(false);

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
            std::weak_ptr<std::atomic<bool>>(m_handlerAlive));

        if (thread->Run() != wxTHREAD_NO_ERROR) {
            delete thread;
            m_cancelFlag.reset();
            m_downloadingMmproj = false;
            // Main weights are already on disk — treat the failure as
            // a warning, not a total failure. User can download the
            // projector later; text chat still works.
            m_activeRow = -1;
            m_hadSuccess = true;
            SetRowComplete(idx);
            wxMessageBox("Main model downloaded, but the vision component "
                         "thread could not be started. You can retry later.",
                         "Partial Download", wxOK | wxICON_INFORMATION, this);
        }
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
    // caller kicks off model load. Non-first-run mode is unaffected —
    // those users dismiss the dialog manually as before.
    if (m_firstRunMode) {
        m_downloadedPath = BuildDestPath(kModels[idx]);
        m_autoCloseTimer.StartOnce(1000);
    }
}

void ModelDownloaderDialog::OnDownloadError(wxCommandEvent& ev)
{
    if (m_activeRow < 0) return;
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

bool ModelDownloaderDialog::IsAlreadyDownloaded(const DownloadableModel& m) const
{
    return wxFileExists(wxString::FromUTF8(BuildDestPath(m)));
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
