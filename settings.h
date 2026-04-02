// settings.h (updated)
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

// Forward declaration for thread
class SettingsDialog;

// Thread for fetching models from Ollama
class ModelFetchThread : public wxThread
{
public:
    ModelFetchThread(SettingsDialog* dialog, const std::string& apiUrl);

protected:
    virtual ExitCode Entry() override;

private:
    SettingsDialog* m_dialog;
    std::string m_apiUrl;
};

class SettingsDialog : public wxDialog
{
public:
    SettingsDialog(wxWindow* parent, const std::string& currentModel,
                   const std::string& currentApiUrl, const std::string& currentTheme);
    ~SettingsDialog();

    std::string GetSelectedModel() const { return m_selectedModel; }
    std::string GetSelectedApiUrl() const { return m_selectedApiUrl; }
    std::string GetSelectedTheme() const { return m_selectedTheme; }
    bool WasModelChanged() const { return m_modelChanged; }
    bool WasApiUrlChanged() const { return m_apiUrlChanged; }
    bool WasThemeChanged() const { return m_themeChanged; }

    // Thread-safe methods for model fetching
    void PostModelsReceived(const std::vector<std::string>& models);
    void PostModelsFetchError(const std::string& error);

private:
    void OnOK(wxCommandEvent& event);
    void OnCancel(wxCommandEvent& event);
    void OnRefreshModels(wxCommandEvent& event);
    void OnApiUrlChanged(wxCommandEvent& event);
    void OnModelsReceived(wxCommandEvent& event);
    void OnModelsFetchError(wxCommandEvent& event);

    void CreateControls();
    void StartModelFetch();
    void SetFetchingState(bool fetching);

    wxComboBox* m_modelComboBox;
    wxTextCtrl* m_apiUrlTextCtrl;
    wxButton* m_refreshButton;
    wxGauge* m_progressGauge;
    wxStaticText* m_statusText;
    wxComboBox* m_themeComboBox;

    std::string m_selectedModel;
    std::string m_selectedApiUrl;
    std::string m_selectedTheme;
    std::string m_originalModel;
    std::string m_originalApiUrl;
    std::string m_originalTheme;

    bool m_modelChanged;
    bool m_apiUrlChanged;
    bool m_themeChanged;
    bool m_isFetching;

    ModelFetchThread* m_fetchThread;
    std::vector<std::string> m_availableModels;

    wxDECLARE_EVENT_TABLE();
};

// Custom events
wxDECLARE_EVENT(wxEVT_MODELS_RECEIVED, wxCommandEvent);
wxDECLARE_EVENT(wxEVT_MODELS_FETCH_ERROR, wxCommandEvent);

#endif // SETTINGS_H