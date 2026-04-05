// settings.h
#ifndef SETTINGS_H
#define SETTINGS_H

#include <wx/wx.h>
#include <wx/dialog.h>
#include <wx/textctrl.h>
#include <wx/button.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/combobox.h>
#include <wx/gauge.h>
#include <wx/thread.h>
#include <string>
#include <vector>
#include <memory>
#include <atomic>

// ── Thread for fetching models from Ollama ───────────────────────
// [STEP 3] No longer holds a raw SettingsDialog pointer.  Instead it
// posts results via wxQueueEvent through a generic wxEvtHandler* and
// uses a shared cancel flag (same pattern as ChatWorkerThread).
class ModelFetchThread : public wxThread
{
public:
    ModelFetchThread(wxEvtHandler* handler,
                     const std::string& apiUrl,
                     std::shared_ptr<std::atomic<bool>> cancelFlag);

protected:
    virtual ExitCode Entry() override;

private:
    wxEvtHandler* m_handler;
    std::string m_apiUrl;
    std::shared_ptr<std::atomic<bool>> m_cancelFlag;

    // Post event only if not cancelled; deletes event on cancel.
    bool SafePost(wxCommandEvent* event);
};

// ── Settings dialog ──────────────────────────────────────────────
class SettingsDialog : public wxDialog
{
public:
    SettingsDialog(wxWindow* parent, const std::string& currentModel,
                   const std::string& currentApiUrl, const std::string& currentTheme,
                   bool currentWorkspaceEnabled, const std::string& currentWorkspacePath);
    ~SettingsDialog();

    std::string GetSelectedModel() const { return m_selectedModel; }
    std::string GetSelectedApiUrl() const { return m_selectedApiUrl; }
    std::string GetSelectedTheme() const { return m_selectedTheme; }
    bool GetWorkspaceEnabled() const { return m_workspaceEnabled; }
    std::string GetWorkspacePath() const { return m_workspacePath; }
    bool WasModelChanged() const { return m_modelChanged; }
    bool WasApiUrlChanged() const { return m_apiUrlChanged; }
    bool WasThemeChanged() const { return m_themeChanged; }
    bool WasWorkspaceChanged() const { return m_workspaceChanged; }

    // [STEP 3] PostModelsReceived / PostModelsFetchError removed —
    // the thread now sends data entirely through events, so the
    // dialog no longer needs public methods callable from threads.

private:
    void OnOK(wxCommandEvent& event);
    void OnCancel(wxCommandEvent& event);
    void OnRefreshModels(wxCommandEvent& event);
    void OnApiUrlChanged(wxCommandEvent& event);
    void OnModelsReceived(wxCommandEvent& event);
    void OnModelsFetchError(wxCommandEvent& event);
    void OnBrowseWorkspace(wxCommandEvent& event);

    void CreateControls();
    void StartModelFetch();
    void SetFetchingState(bool fetching);

    wxComboBox* m_modelComboBox;
    wxTextCtrl* m_apiUrlTextCtrl;
    wxButton* m_refreshButton;
    wxGauge* m_progressGauge;
    wxStaticText* m_statusText;
    wxComboBox* m_themeComboBox;
    wxCheckBox* m_workspaceCheckBox;
    wxTextCtrl* m_workspacePathCtrl;
    wxButton* m_workspaceBrowseButton;

    std::string m_selectedModel;
    std::string m_selectedApiUrl;
    std::string m_selectedTheme;
    std::string m_originalModel;
    std::string m_originalApiUrl;
    std::string m_originalTheme;

    bool m_workspaceEnabled;
    std::string m_workspacePath;
    bool m_originalWorkspaceEnabled;
    std::string m_originalWorkspacePath;

    bool m_modelChanged;
    bool m_apiUrlChanged;
    bool m_themeChanged;
    bool m_workspaceChanged;
    bool m_isFetching;

    // [STEP 3/4] Replaced ModelFetchThread* m_fetchThread with a
    // shared cancel flag.  We never store the thread pointer — the
    // detached thread owns itself.  Cancellation is communicated
    // exclusively through this atomic flag.
    std::shared_ptr<std::atomic<bool>> m_cancelFlag;

    wxDECLARE_EVENT_TABLE();
};

// Custom events
wxDECLARE_EVENT(wxEVT_MODELS_RECEIVED, wxCommandEvent);
wxDECLARE_EVENT(wxEVT_MODELS_FETCH_ERROR, wxCommandEvent);

#endif // SETTINGS_H
