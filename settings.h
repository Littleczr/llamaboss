#pragma once
// settings.h

#include <wx/wx.h>
#include <wx/dialog.h>
#include <wx/combobox.h>
#include <wx/button.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <string>
#include <vector>

// Forward declarations
struct ThemeData;

// ── Settings dialog ──────────────────────────────────────────────
// Model selection now scans the filesystem for .gguf files
// instead of fetching from an Ollama API.
class SettingsDialog : public wxDialog
{
public:
    SettingsDialog(wxWindow* parent,
                   const std::string& currentModelPath,  // full GGUF path
                   const std::string& currentTheme,
                   const ThemeData& theme);
    ~SettingsDialog() = default;

    // Returns the full GGUF path of the selected model
    std::string GetSelectedModel() const { return m_selectedModel; }
    std::string GetSelectedTheme() const { return m_selectedTheme; }
    bool WasModelChanged() const { return m_modelChanged; }
    bool WasThemeChanged() const { return m_themeChanged; }

private:
    void OnOK(wxCommandEvent& event);
    void OnCancel(wxCommandEvent& event);
    void OnManageModels(wxCommandEvent& event);
    void OnOpenModelsFolder(wxCommandEvent& event);
    void OnDownloadModels(wxCommandEvent& event);

    void CreateControls();
    void PopulateModelList();

    wxComboBox*   m_modelComboBox;
    wxStaticText* m_statusText;
    wxComboBox*   m_themeComboBox;

    // Model list: display name -> full path mapping
    std::vector<std::string> m_modelPaths;   // parallel to combobox items
    std::string m_selectedModel;
    std::string m_selectedTheme;
    std::string m_originalModel;
    std::string m_originalTheme;

    bool m_modelChanged;
    bool m_themeChanged;
    const ThemeData* m_theme;

    wxDECLARE_EVENT_TABLE();
};
