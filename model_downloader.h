#pragma once
// model_downloader.h
// Dialog for downloading curated GGUF models directly from HuggingFace.
// All listed models are sourced from bartowski — no HF account required.

#include <wx/wx.h>
#include <wx/button.h>
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

    // ── Optional vision projector (mmproj) ───────────────────────
    // If the model is vision-capable, populate mmprojFilename with the
    // projector's .gguf filename in the same HF repo. When non-empty
    // and we're in casual mode, the downloader will pull the projector
    // immediately after the main weights and save both in the bundle
    // folder — so vision works out of the box with zero user knowledge
    // of what an mmproj is. Leave empty for text-only models.
    std::string mmprojFilename;
    std::string mmprojSizeDisplay;  // "0.8 GB" — shown in status text
    long long   mmprojSizeBytes = 0;

    // ── First-run starter flag ───────────────────────────────────
    // When true, this entry is the recommended starter model for
    // brand-new users. In first-run mode the downloader reorders it
    // to the top of the list and renders a "Start here" badge next
    // to its name. Exactly one catalog entry should carry this flag.
    bool firstRunStarter = false;
};

// ── Pasted-link pre-check (Manage Models) ────────────────────────
// Blocking: call from a worker thread.  Fetches only the first 4 bytes
// of `url` (following redirects) to learn the file size and confirm the
// GGUF magic before a real download starts.  sizeBytes is -1 when the
// server didn't say.  On failure, `error` is a user-facing sentence.
struct ModelUrlProbe
{
    bool        ok = false;
    long long   sizeBytes = -1;
    std::string error;
};
ModelUrlProbe ProbeModelUrl(const std::string& url);

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
//
// Thread safety: m_handler is a raw pointer to the dialog that
// receives events. Since this is a detached thread and the dialog
// can be destroyed mid-download (user closes Settings, app exits),
// we also carry a weak_ptr to a "handler is alive" sentinel owned
// by the dialog. SafePost locks it before every wxQueueEvent —
// same pattern as ChatWorkerThread uses against MyFrame's m_alive.
class DownloadThread : public wxThread
{
public:
    DownloadThread(wxEvtHandler*   handler,
                   const std::string& url,
                   const std::string& destPath,
                   long long          expectedBytes,
                   std::shared_ptr<std::atomic<bool>> cancelFlag,
                   std::weak_ptr<std::atomic<bool>>   aliveToken,
                   long               generation,
                   bool               expectedBytesExact = false);
protected:
    ExitCode Entry() override;
private:
    bool SafePost(wxCommandEvent* ev);

    wxEvtHandler* m_handler;
    std::string   m_url;
    std::string   m_destPath;
    long long     m_expectedBytes;
    // True when m_expectedBytes came from the server itself (the pasted-link
    // probe's Content-Range / Content-Length) rather than an approximate
    // catalog figure.  Exact sizes are enforced before the file is promoted:
    // a later request that returns a shorter body -- even one whose own
    // Content-Length matches it -- must not install a truncated model.
    bool          m_expectedBytesExact = false;
    std::shared_ptr<std::atomic<bool>> m_cancelFlag;
    std::weak_ptr<std::atomic<bool>>   m_aliveToken;

    // Identifies which download operation this worker belongs to. Stamped
    // onto every event via wxCommandEvent::SetInt so the dialog can reject
    // a late event from a cancelled/superseded worker instead of applying
    // it to whatever row is active now. Also disambiguates the temp file
    // (see Entry): a cancelled worker may still be blocked in socket I/O
    // for up to its receive timeout, so a fresh retry must not share its
    // ".download" scratch path.
    long          m_generation;
};

// ── Per-row UI handles ────────────────────────────────────────────
//
// row.actionBtn is a plain wxButton whose label and colour are swapped
// in place by SetRowDownloading / SetRowComplete / SetRowError /
// OnCancelClicked to reflect the download lifecycle:
//   idle      → "Download" (accent fill)
//   active    → "Cancel"   (red fill, destructive)
//   complete  → "✓ Downloaded" (flat muted, disabled)
//   error     → "Retry"    (accent fill)
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
    // firstRunMode: when true, the dialog reorders the starter model to
    // the top, renders a "Start here" badge next to it, and auto-closes
    // ~1s after the first successful download so the caller can seamlessly
    // hand off to model-load without the user hunting for a Close button.
    ModelDownloaderDialog(wxWindow* parent,
                          const ThemeData* theme = nullptr,
                          bool firstRunMode = false);
    ~ModelDownloaderDialog();

    // True if at least one model was downloaded this session.
    // Caller uses this to decide whether to refresh the model list.
    bool HadSuccessfulDownload() const { return m_hadSuccess; }

    // The full GGUF path of the model downloaded this session, or ""
    // if none succeeded. Set when a download reaches its terminal state
    // (either the standalone .gguf completed, or the chained mmproj did).
    // First-run mode uses this to auto-load the model after close.
    std::string GetDownloadedModelPath() const { return m_downloadedPath; }

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
    // Starts the vision-projector stage for row `idx` (m_activeRow must
    // already be idx).  On thread-start failure puts the row in the
    // Retry state and returns false.
    bool StartMmprojStage(size_t idx);

    // Thread event handlers
    void OnDownloadProgress(wxCommandEvent& ev);
    void OnDownloadComplete(wxCommandEvent& ev);
    void OnDownloadError(wxCommandEvent&    ev);
    void OnClose(wxCloseEvent& ev);

    // Helpers
    // Complete = weights AND (for vision entries) the projector are on
    // disk.  Downloads land via temp file + rename after a size check,
    // so a file at the destination path is a finished download.
    bool        IsAlreadyDownloaded(const DownloadableModel& m) const;
    bool        IsWeightsDownloaded(const DownloadableModel& m) const;
    bool        IsProjectorDownloaded(const DownloadableModel& m) const;
    std::string BuildUrl(const DownloadableModel& m) const;
    std::string BuildDestPath(const DownloadableModel& m) const;
    std::string FormatBytes(long long bytes) const;

    const ThemeData*   m_theme;
    wxScrolledWindow*  m_scroll = nullptr;

    std::vector<ModelRow>              m_rows;
    std::shared_ptr<std::atomic<bool>> m_cancelFlag;

    // Sentinel for the worker thread's SafePost check. Flipped to false
    // in the destructor so any in-flight event on a detached download
    // thread bails out instead of dispatching to this freed handler.
    // Lifetime: exactly the dialog's — created in ctor, cleared in dtor.
    std::shared_ptr<std::atomic<bool>> m_handlerAlive;

    int                                m_activeRow = -1;
    bool                               m_hadSuccess = false;

    // Monotonic download-operation counter. Bumped every time a new
    // download (or a chained mmproj stage) starts. The current value is
    // handed to the DownloadThread and echoed back on every event; the
    // three event handlers ignore any event whose generation != this,
    // so a stale event from a cancelled worker that is still unwinding
    // can never be applied to a later download's row. Starts at 0; the
    // first real operation is generation 1, so a default-constructed
    // event (GetInt() == 0) is never mistaken for a live one.
    long                               m_downloadGeneration = 0;

    // True while the active row's mmproj (projector) download is in flight.
    // The main .gguf download completes first, then we kick off the mmproj
    // as a chained second download — both under the same row's progress UI.
    bool                               m_downloadingMmproj = false;

    // ── First-run mode state ─────────────────────────────────────
    // When m_firstRunMode is true, the dialog:
    //   • reorders the starter model to the top of the visible list
    //   • renders a "Start here" badge next to the starter's name
    //   • captures the path of the first successfully-downloaded .gguf
    //   • auto-closes ~1s after that terminal success via m_autoCloseTimer
    // When false, these behaviors are all suppressed and the dialog
    // behaves identically to its pre-first-run form. Every first-run
    // check sites against this single flag.
    bool        m_firstRunMode = false;
    std::string m_downloadedPath;     // Full GGUF path, set on terminal success
    wxTimer     m_autoCloseTimer;     // Fires once after terminal success

    // Display order of catalog entries — indices into kModels. Built in
    // the constructor: in first-run mode the starter is moved to the
    // front; otherwise it matches the catalog order 1:1. m_rows stays
    // indexed by catalog position, so existing handlers are untouched.
    std::vector<size_t> m_displayOrder;

    // Timer handler — closes the dialog with wxID_OK.
    void OnAutoCloseTimer(wxTimerEvent& ev);
};
