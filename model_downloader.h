#pragma once
// model_downloader.h
// Dialog for downloading curated GGUF models directly from HuggingFace.
// All listed models are sourced from bartowski — no HF account required.

#include <wx/wx.h>
#include <wx/gauge.h>
#include <wx/scrolwin.h>
#include <wx/thread.h>

#include <string>
#include <vector>
#include <memory>
#include <atomic>

struct ThemeData;

// ── Catalog entry ────────────────────────────────────────────────
struct DownloadableModel
{
    std::string displayName;   // "Gemma 3 12B"
    std::string tag;           // "Recommended" / "Fast" / etc.
    std::string description;   // one-line blurb shown under the name
    std::string author;        // HuggingFace author slug
    std::string repo;          // HuggingFace repo slug
    std::string filename;      // exact GGUF filename to download
    std::string sizeDisplay;   // "7.8 GB" (shown in UI)
    long long   sizeBytes;     // approximate, used for progress bar
};

// ── Events posted to the dialog by the download thread ───────────
// Progress: ExtraLong = 0–100 pct; String = "receivedBytes|totalBytes"
// Complete: String = final file path
// Error:    String = error message
wxDECLARE_EVENT(wxEVT_DOWNLOAD_PROGRESS, wxCommandEvent);
wxDECLARE_EVENT(wxEVT_DOWNLOAD_COMPLETE, wxCommandEvent);
wxDECLARE_EVENT(wxEVT_DOWNLOAD_ERROR,    wxCommandEvent);

// ── Background HTTPS download thread ────────────────────────────
// Follows up to 8 redirects (HuggingFace CDN uses 1–2).
// Downloads to destPath + ".download", then renames on success.
// Cancellable via the shared atomic flag.
class DownloadThread : public wxThread
{
public:
    DownloadThread(wxEvtHandler*   handler,
                   const std::string& url,
                   const std::string& destPath,
                   long long          expectedBytes,
                   std::shared_ptr<std::atomic<bool>> cancelFlag);
protected:
    ExitCode Entry() override;
private:
    bool SafePost(wxCommandEvent* ev);

    wxEvtHandler* m_handler;
    std::string   m_url;
    std::string   m_destPath;
    long long     m_expectedBytes;
    std::shared_ptr<std::atomic<bool>> m_cancelFlag;
};

// ── Per-row UI handles ────────────────────────────────────────────
struct ModelRow
{
    wxPanel*      rowPanel    = nullptr;
    wxStaticText* nameLabel   = nullptr;
    wxStaticText* sizeLabel   = nullptr;
    wxStaticText* statusLabel = nullptr;
    wxGauge*      gauge       = nullptr;
    wxButton*     actionBtn   = nullptr;
    bool          downloaded  = false;
    bool          downloading = false;
};

// ── Model downloader dialog ──────────────────────────────────────
class ModelDownloaderDialog : public wxDialog
{
public:
    ModelDownloaderDialog(wxWindow* parent, const ThemeData* theme = nullptr);
    ~ModelDownloaderDialog();

    // True if at least one model was downloaded this session.
    // Caller uses this to decide whether to refresh the model list.
    bool HadSuccessfulDownload() const { return m_hadSuccess; }

    // The curated catalog — defined in model_downloader.cpp
    static const std::vector<DownloadableModel> kModels;

private:
    // UI construction
    void BuildUI();
    void BuildModelRow(wxSizer* listSizer, size_t idx,
                       const DownloadableModel& model);

    // Row state transitions
    void SetRowDownloading(size_t idx);
    void SetRowComplete(size_t idx);
    void SetRowError(size_t idx, const std::string& msg);
    void SetRowIdle(size_t idx);

    // Download control
    void OnDownloadClicked(size_t idx);
    void OnCancelClicked();

    // Thread event handlers
    void OnDownloadProgress(wxCommandEvent& ev);
    void OnDownloadComplete(wxCommandEvent& ev);
    void OnDownloadError(wxCommandEvent&    ev);
    void OnClose(wxCloseEvent& ev);

    // Helpers
    bool        IsAlreadyDownloaded(const DownloadableModel& m) const;
    std::string BuildUrl(const DownloadableModel& m) const;
    std::string BuildDestPath(const DownloadableModel& m) const;
    std::string FormatBytes(long long bytes) const;

    const ThemeData*   m_theme;
    wxScrolledWindow*  m_scroll = nullptr;

    std::vector<ModelRow>              m_rows;
    std::shared_ptr<std::atomic<bool>> m_cancelFlag;
    int                                m_activeRow = -1;
    bool                               m_hadSuccess = false;
};
