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
#include <iomanip>
#include <algorithm>

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

// ═══════════════════════════════════════════════════════════════════
//  Curated model catalog
//
//  Ordering: ascending by quantized size, so the dialog reads as a
//  ladder of capability/cost.  The "tag" column tells the user where
//  each model fits ("Ultra-light" / "Recommended" / "Top quality").
//
//  All from bartowski on HuggingFace — publicly downloadable, no
//  account or license gate required.  Bartowski's newer Gemma uploads
//  use the "google_" repo prefix; older entries (Phi-4, Qwen 2.5,
//  Llama 3.2) keep their original repo names.
//
//  Q4_K_M is the chosen quant for every entry — well-rounded quality
//  vs size, default-recommended by bartowski himself, and works on
//  every llama.cpp backend including Vulkan (unlike I-quants which
//  the Vulkan backend cannot run).
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
        "Qwen 2.5 7B",   "Versatile",
        "Alibaba's capable all-rounder. Fast with strong instruction following.",
        "bartowski", "Qwen2.5-7B-Instruct-GGUF",
        "Qwen2.5-7B-Instruct-Q4_K_M.gguf",
        "4.7 GB", 4'700'000'000LL
        // Text-only — no mmproj fields
    },
    {
        "Phi-4 14B",     "Reasoning",
        "Microsoft's reasoning model. Excellent at math, code, and logic.",
        "bartowski", "phi-4-GGUF",
        "phi-4-Q4_K_M.gguf",
        "9.0 GB", 9'050'000'000LL
        // Text-only — no mmproj fields
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

    // Verify the handler is still alive. The dialog's destructor flips
    // this sentinel to false, so a detached thread that's mid-read when
    // the user closes the dialog won't post to a freed wxEvtHandler.
    // Small race remains between this check and wxQueueEvent, but the
    // window is vanishingly small compared to the previous (seconds-long)
    // gap while the thread was still writing to disk after dialog close.
    auto alive = m_aliveToken.lock();
    if (!alive || !alive->load()) { delete ev; return false; }

    wxQueueEvent(m_handler, ev);
    return true;
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

            // ── Rename temp → final ──────────────────────────────
            if (wxFileExists(wxString::FromUTF8(m_destPath))) QuietRemoveFileUtf8(m_destPath);
            if (!wxRenameFile(tempPath, m_destPath)) {
                auto* ev = new wxCommandEvent(wxEVT_DOWNLOAD_ERROR);
                ev->SetString("Could not save file to:\n" + m_destPath);
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
    if (m_handlerAlive) m_handlerAlive->store(false);
    if (m_cancelFlag)   m_cancelFlag->store(true);
}

// ─────────────────────────────────────────────────────────────────
//  UI construction
// ─────────────────────────────────────────────────────────────────

void ModelDownloaderDialog::BuildUI()
{
    const wxColour bgMain    = m_theme ? m_theme->bgMain      : GetBackgroundColour();
    const wxColour bgToolbar = m_theme ? m_theme->bgToolbar   : GetBackgroundColour();
    const wxColour textPri   = m_theme ? m_theme->textPrimary : GetForegroundColour();
    const wxColour textMuted = m_theme ? m_theme->textMuted   : wxColour(128,128,128);
    const wxColour border    = m_theme ? m_theme->borderSubtle: wxColour(200,200,200);

    if (m_theme) SetBackgroundColour(bgMain);

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
    m_scroll->SetBackgroundColour(bgMain);

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
    btnPanel->SetBackgroundColour(bgMain);
    auto* btnSizer = new wxBoxSizer(wxHORIZONTAL);
    btnSizer->AddStretchSpacer();
    auto* closeBtn = new wxButton(btnPanel, wxID_CANCEL, "Close");
    if (m_theme) {
        closeBtn->SetBackgroundColour(m_theme->modelPillBg);
        closeBtn->SetForegroundColour(textPri);
    }
    btnSizer->Add(closeBtn, 0, wxALL, 10);
    btnPanel->SetSizer(btnSizer);
    outer->Add(btnPanel, 0, wxEXPAND);

    SetSizer(outer);
}

void ModelDownloaderDialog::BuildModelRow(wxSizer* listSizer,
                                           size_t idx,
                                           const DownloadableModel& model)
{
    const wxColour bgMain    = m_theme ? m_theme->bgMain       : GetBackgroundColour();
    const wxColour textPri   = m_theme ? m_theme->textPrimary  : GetForegroundColour();
    const wxColour textMuted = m_theme ? m_theme->textMuted    : wxColour(128,128,128);
    const wxColour accent    = m_theme ? m_theme->accentButton : wxColour(60,120,220);
    const wxColour accentTxt = m_theme ? m_theme->accentButtonText : *wxWHITE;

    ModelRow& row = m_rows[idx];
    row.rowPanel = new wxPanel(m_scroll);
    row.rowPanel->SetBackgroundColour(bgMain);

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

    // Single button — label and style change with state.
    // Handler checks row state so we never need to rebind.
    bool alreadyDone = IsAlreadyDownloaded(model);
    row.downloaded   = alreadyDone;

    std::string btnLabel = alreadyDone
        ? "\xe2\x9c\x93 Downloaded" : "Download";      // ✓ or plain
    row.actionBtn = new wxButton(row.rowPanel, wxID_ANY,
        wxString::FromUTF8(btnLabel), wxDefaultPosition, wxSize(120,-1));

    if (alreadyDone) {
        row.actionBtn->Enable(false);
        if (m_theme) {
            row.actionBtn->SetBackgroundColour(m_theme->bgInputArea);
            row.actionBtn->SetForegroundColour(textMuted);
        }
    } else {
        if (m_theme) {
            row.actionBtn->SetBackgroundColour(accent);
            row.actionBtn->SetForegroundColour(accentTxt);
        }
    }

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
    const wxColour stopBg  = m_theme ? m_theme->stopButton     : wxColour(180,50,50);
    const wxColour stopTxt = m_theme ? m_theme->stopButtonText : *wxWHITE;
    const wxColour muted   = m_theme ? m_theme->textMuted      : wxColour(128,128,128);

    row.downloading = true;

    row.gauge->SetValue(0);
    row.gauge->Show();
    row.statusLabel->SetLabel("Connecting...");
    row.statusLabel->SetForegroundColour(muted);

    row.actionBtn->SetLabel("Cancel");
    row.actionBtn->Enable(true);
    if (m_theme) {
        row.actionBtn->SetBackgroundColour(stopBg);
        row.actionBtn->SetForegroundColour(stopTxt);
    }

    row.rowPanel->Layout();
    m_scroll->FitInside();
}

void ModelDownloaderDialog::SetRowComplete(size_t idx)
{
    ModelRow& row = m_rows[idx];
    const wxColour muted   = m_theme ? m_theme->textMuted   : wxColour(128,128,128);
    const wxColour success = m_theme ? m_theme->chatAssistant: wxColour(80,180,80);

    row.downloading = false;
    row.downloaded  = true;

    row.gauge->Hide();
    row.statusLabel->SetLabel(wxString::FromUTF8("\xe2\x9c\x93 Downloaded successfully"));
    row.statusLabel->SetForegroundColour(success);

    row.actionBtn->SetLabel(wxString::FromUTF8("\xe2\x9c\x93 Downloaded"));
    row.actionBtn->Enable(false);
    if (m_theme) {
        row.actionBtn->SetBackgroundColour(m_theme->bgInputArea);
        row.actionBtn->SetForegroundColour(muted);
    }

    row.rowPanel->Layout();
    m_scroll->FitInside();
}

void ModelDownloaderDialog::SetRowError(size_t idx, const std::string& msg)
{
    ModelRow& row = m_rows[idx];
    const wxColour accent    = m_theme ? m_theme->accentButton    : wxColour(60,120,220);
    const wxColour accentTxt = m_theme ? m_theme->accentButtonText: *wxWHITE;

    row.downloading = false;

    row.gauge->Hide();
    row.statusLabel->SetLabel("Error: " + msg);
    row.statusLabel->SetForegroundColour(wxColour(200, 60, 60));

    row.actionBtn->SetLabel("Retry");
    row.actionBtn->Enable(true);
    if (m_theme) {
        row.actionBtn->SetBackgroundColour(accent);
        row.actionBtn->SetForegroundColour(accentTxt);
    }

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
    const wxColour muted     = m_theme ? m_theme->textMuted       : wxColour(128,128,128);
    const wxColour accent    = m_theme ? m_theme->accentButton    : wxColour(60,120,220);
    const wxColour accentTxt = m_theme ? m_theme->accentButtonText: *wxWHITE;

    row.downloading = false;
    row.gauge->Hide();
    row.statusLabel->SetLabel(wxString::FromUTF8(kModels[idx].description));
    row.statusLabel->SetForegroundColour(muted);
    row.actionBtn->SetLabel("Download");
    row.actionBtn->Enable(true);
    if (m_theme) {
        row.actionBtn->SetBackgroundColour(accent);
        row.actionBtn->SetForegroundColour(accentTxt);
    }
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
