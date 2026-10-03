// model_manager.h
#pragma once

#include <wx/wx.h>
#include <wx/dialog.h>
#include <wx/listctrl.h>
#include <wx/button.h>
#include <wx/gauge.h>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

// Forward declarations
struct ThemeData;

// ── Model Manager dialog ──────────────────────────────────────────
// Lists .gguf files from the models directory.
// Supports delete (removes file), opening the models folder, and
// downloading a model from a pasted link (Hugging Face page links are
// accepted; see model_url.h).  One download at a time; it runs on the
// same DownloadThread the curated downloader uses.
//
// Button styling matches the rest of the dialog family:
//   Refresh / Open folder → solid accent wxButtons
//   Close → flat borderless wxButton with muted text
//
// Delete lives on the list itself — Del key while a row is selected,
// or right-click context menu. Same pattern as ProjectAttachDialog,
// so destructive actions don't dominate the action row visually.
class ModelManagerDialog : public wxDialog
{
public:
    ModelManagerDialog(wxWindow* parent,
                       const ThemeData* theme = nullptr,
                       std::string loadedModelPath = {},
                       std::string configuredModelPath = {});
    ~ModelManagerDialog();

private:
    void CreateControls();
    void RefreshModelList();
    bool IsLoadedModelPath(const std::string& path) const;
    bool IsConfiguredModelPath(const std::string& path) const;

    // Event handlers
    void OnDeleteClicked(wxCommandEvent& ev);
    void OnRefreshClicked(wxCommandEvent& ev);
    void OnOpenFolderClicked(wxCommandEvent& ev);
    void OnClose(wxCommandEvent& ev);
    void OnContextMenu(wxContextMenuEvent& ev);   // Right-click on list
    void OnCharHook(wxKeyEvent& ev);              // Del key catch
    void OnCloseWindow(wxCloseEvent& ev);         // title-bar X

    // Name column fills the list width; hovering shows the full name.
    void OnListSize(wxSizeEvent& ev);
    void OnListMotion(wxMouseEvent& ev);
    void FitNameColumn();

    // ── Download from a pasted link ─────────────────────────────
    enum class UrlState { Idle, Checking, Downloading };
    void OnUrlButton(wxCommandEvent& ev);
    void OnProbeDone(wxThreadEvent& ev);
    void OnDownloadProgress(wxCommandEvent& ev);
    void OnDownloadComplete(wxCommandEvent& ev);
    void OnDownloadError(wxCommandEvent& ev);
    void SetUrlState(UrlState state);
    void SetUrlStatus(const wxString& text, bool isError = false);
    void CancelUrlDownload();
    bool ConfirmLeaveDuringDownload();   // true = OK to close
    void SelectModelPath(const std::string& path);

    wxListCtrl*   m_modelList     = nullptr;
    wxButton*     m_refreshButton = nullptr;  // solid accent
    wxStaticText* m_statusText    = nullptr;
    long          m_tipRow        = -2;       // row whose name is the current tooltip

    wxTextCtrl*   m_urlField  = nullptr;
    wxButton*     m_urlButton = nullptr;      // Download / Cancel
    wxGauge*      m_urlGauge  = nullptr;
    wxStaticText* m_urlStatus = nullptr;
    UrlState      m_urlState  = UrlState::Idle;

    // Worker threads post back only while this is true (ui_event_post.h).
    std::shared_ptr<std::atomic<bool>> m_alive =
        std::make_shared<std::atomic<bool>>(true);
    std::shared_ptr<std::atomic<bool>> m_cancelFlag;
    unsigned    m_probeGeneration    = 0;   // stale-probe guard
    long        m_downloadGeneration = 0;   // stale-download guard (DownloadThread stamps it)
    std::string m_pendingFilename;
    std::string m_pendingDestPath;
    // Bundle folder this dialog created for the current link download
    // (empty if it already existed).  Removed again if the download
    // fails and leaves it empty.  A cancelled download's folder can't be
    // removed synchronously (the worker deletes its temp file async), so
    // it stays; ChooseLinkDestPath reuses an empty folder next time.
    std::string m_pendingCreatedDir;

    const ThemeData* m_theme;
    std::string m_loadedModelPath;      // Currently running llama-server model, if any
    std::string m_configuredModelPath;  // App/deferred model selection, if any

    // Parallel to list rows: full GGUF paths
    std::vector<std::string> m_modelPaths;

    wxDECLARE_EVENT_TABLE();
};
