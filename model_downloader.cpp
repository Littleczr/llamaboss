#define _CRT_SECURE_NO_WARNINGS

// model_downloader.cpp
// Curated model catalog + HTTPS download dialog for LlamaBoss.
// Models are sourced from bartowski on HuggingFace — no account required.

#include "model_downloader.h"
#include "server_manager.h"
#include "theme.h"

#include <wx/filename.h>

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

// ═══════════════════════════════════════════════════════════════════
//  Curated model catalog
//  All from bartowski on HuggingFace — publicly downloadable,
//  no account or license gate required.
// ═══════════════════════════════════════════════════════════════════
const std::vector<DownloadableModel> ModelDownloaderDialog::kModels =
{
    {
        "Gemma 3 4B",    "Fast",
        "Google's compact model. Quick responses, low VRAM. Great for older GPUs.",
        "bartowski", "gemma-3-4b-it-GGUF",
        "gemma-3-4b-it-Q4_K_M.gguf",
        "3.3 GB", 3'300'000'000LL
    },
    {
        "Gemma 3 12B",   "Recommended",
        "Best balance of speed and quality. Ideal starting point for most users.",
        "bartowski", "gemma-3-12b-it-GGUF",
        "gemma-3-12b-it-Q4_K_M.gguf",
        "7.8 GB", 7'800'000'000LL
    },
    {
        "Gemma 3 27B",   "Powerful",
        "Highest quality Gemma. Requires 20+ GB VRAM for full GPU offload.",
        "bartowski", "gemma-3-27b-it-GGUF",
        "gemma-3-27b-it-Q4_K_M.gguf",
        "17.2 GB", 17'200'000'000LL
    },
    {
        "Phi-4 14B",     "Reasoning",
        "Microsoft's reasoning model. Excellent at math, code, and logic.",
        "bartowski", "phi-4-GGUF",
        "phi-4-Q4_K_M.gguf",
        "8.1 GB", 8'100'000'000LL
    },
    {
        "Qwen 2.5 7B",   "Versatile",
        "Alibaba's capable all-rounder. Fast with strong instruction following.",
        "bartowski", "Qwen2.5-7B-Instruct-GGUF",
        "Qwen2.5-7B-Instruct-Q4_K_M.gguf",
        "4.7 GB", 4'700'000'000LL
    },
    {
        "Llama 3.2 3B",  "Lightweight",
        "Meta's smallest chat model. Runs on CPU or minimal VRAM.",
        "bartowski", "Llama-3.2-3B-Instruct-GGUF",
        "Llama-3.2-3B-Instruct-Q4_K_M.gguf",
        "2.0 GB", 2'000'000'000LL
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
        // Accept all certificates — HuggingFace uses a valid CA-signed cert,
        // but this keeps things working even behind corporate proxies.
        Poco::SharedPtr<Poco::Net::InvalidCertificateHandler> pCert(
            new Poco::Net::AcceptCertificateHandler(false));
        Poco::Net::Context::Ptr pCtx(new Poco::Net::Context(
            Poco::Net::Context::CLIENT_USE, ""));
        Poco::Net::SSLManager::instance().initializeClient(
            nullptr, pCert, pCtx);
    });
}

// ═══════════════════════════════════════════════════════════════════
//  DownloadThread
// ═══════════════════════════════════════════════════════════════════

DownloadThread::DownloadThread(wxEvtHandler*   handler,
                               const std::string& url,
                               const std::string& destPath,
                               long long          expectedBytes,
                               std::shared_ptr<std::atomic<bool>> cancelFlag)
    : wxThread(wxTHREAD_DETACHED)
    , m_handler(handler)
    , m_url(url)
    , m_destPath(destPath)
    , m_expectedBytes(expectedBytes)
    , m_cancelFlag(cancelFlag)
{}

bool DownloadThread::SafePost(wxCommandEvent* ev)
{
    if (m_cancelFlag->load()) { delete ev; return false; }
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
            long long totalBytes = m_expectedBytes;
            if (resp.has("Content-Length")) {
                try { totalBytes = std::stoll(resp.get("Content-Length")); }
                catch (...) {}
            }

            std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) {
                auto* ev = new wxCommandEvent(wxEVT_DOWNLOAD_ERROR);
                ev->SetString("Cannot create temporary file:\n" + tempPath);
                SafePost(ev);
                return (ExitCode)0;
            }

            char    buf[65536];
            long long received   = 0;
            long long lastReport = -1;

            while (!m_cancelFlag->load())
            {
                in.read(buf, sizeof(buf));
                std::streamsize n = in.gcount();
                if (n <= 0) break;

                out.write(buf, n);
                received += static_cast<long long>(n);

                // Report progress approximately every 2 MB
                if (received - lastReport >= 2LL * 1024 * 1024 || lastReport < 0)
                {
                    lastReport = received;
                    int pct = (totalBytes > 0)
                              ? static_cast<int>(received * 100LL / totalBytes) : 0;

                    auto* ev = new wxCommandEvent(wxEVT_DOWNLOAD_PROGRESS);
                    ev->SetExtraLong(static_cast<long>(pct));
                    ev->SetString(wxString::Format("%lld|%lld", received, totalBytes));
                    if (!SafePost(ev)) {
                        out.close();
                        wxRemoveFile(tempPath);
                        return (ExitCode)0;
                    }
                }
            }
            out.close();

            if (m_cancelFlag->load()) {
                wxRemoveFile(tempPath);
                return (ExitCode)0;
            }

            // ── Rename temp → final ──────────────────────────────
            if (wxFileExists(m_destPath)) wxRemoveFile(m_destPath);
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
        wxRemoveFile(tempPath);
        if (!m_cancelFlag->load()) {
            auto* ev = new wxCommandEvent(wxEVT_DOWNLOAD_ERROR);
            ev->SetString(wxString::FromUTF8(ex.displayText()));
            SafePost(ev);
        }
    }
    catch (const std::exception& ex)
    {
        wxRemoveFile(tempPath);
        if (!m_cancelFlag->load()) {
            auto* ev = new wxCommandEvent(wxEVT_DOWNLOAD_ERROR);
            ev->SetString(wxString::FromUTF8(ex.what()));
            SafePost(ev);
        }
    }

    return (ExitCode)0;
}

// ═══════════════════════════════════════════════════════════════════
//  ModelDownloaderDialog — construction
// ═══════════════════════════════════════════════════════════════════

ModelDownloaderDialog::ModelDownloaderDialog(wxWindow* parent,
                                             const ThemeData* theme)
    : wxDialog(parent, wxID_ANY, "Download Models",
               wxDefaultPosition, wxSize(660, 560),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_theme(theme)
{
    BuildUI();
    CentreOnParent();

    Bind(wxEVT_DOWNLOAD_PROGRESS, &ModelDownloaderDialog::OnDownloadProgress, this);
    Bind(wxEVT_DOWNLOAD_COMPLETE, &ModelDownloaderDialog::OnDownloadComplete, this);
    Bind(wxEVT_DOWNLOAD_ERROR,    &ModelDownloaderDialog::OnDownloadError,    this);
    Bind(wxEVT_CLOSE_WINDOW,      &ModelDownloaderDialog::OnClose,            this);
}

ModelDownloaderDialog::~ModelDownloaderDialog()
{
    // Signal any running thread to stop; it cleans up the temp file itself.
    if (m_cancelFlag) m_cancelFlag->store(true);
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

    auto* titleLbl = new wxStaticText(hdrPanel, wxID_ANY, "Download Models");
    wxFont tf = titleLbl->GetFont();
    tf.SetPointSize(11); tf.SetWeight(wxFONTWEIGHT_BOLD);
    titleLbl->SetFont(tf);
    titleLbl->SetForegroundColour(textPri);

    auto* subLbl = new wxStaticText(hdrPanel, wxID_ANY,
        "All models are free to download. No HuggingFace account required.\n"
        "Files are saved to:  Documents\\LlamaBoss\\models");
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

    for (size_t i = 0; i < kModels.size(); ++i) {
        BuildModelRow(listSizer, i, kModels[i]);
        // Thin separator between rows (not after the last one)
        if (i + 1 < kModels.size()) {
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

    // ── Top line: name · tag ── size ── [button] ──────────────────
    auto* topLine = new wxBoxSizer(wxHORIZONTAL);

    std::string nameStr = model.displayName
        + "  \xe2\x80\xa2  "   // · (UTF-8 bullet)
        + model.tag;
    row.nameLabel = new wxStaticText(row.rowPanel, wxID_ANY,
        wxString::FromUTF8(nameStr));
    wxFont nf = row.nameLabel->GetFont();
    nf.SetWeight(wxFONTWEIGHT_BOLD);
    row.nameLabel->SetFont(nf);
    row.nameLabel->SetForegroundColour(textPri);
    topLine->Add(row.nameLabel, 1, wxALIGN_CENTER_VERTICAL);

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

    // Ensure models directory exists
    wxFileName::Mkdir(wxString::FromUTF8(ServerManager::GetModelsDir()),
                      wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);

    m_activeRow  = static_cast<int>(idx);
    m_cancelFlag = std::make_shared<std::atomic<bool>>(false);

    SetRowDownloading(idx);

    auto* thread = new DownloadThread(
        this, url, destPath, model.sizeBytes, m_cancelFlag);

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
    m_activeRow = -1;
    m_cancelFlag.reset();
    m_hadSuccess = true;
    SetRowComplete(idx);
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

std::string ModelDownloaderDialog::BuildDestPath(const DownloadableModel& m) const
{
    return ServerManager::GetModelsDir()
         + std::string(1, wxFILE_SEP_PATH) + m.filename;
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
