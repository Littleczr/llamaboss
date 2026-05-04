#pragma once
// settings.h

#include <wx/wx.h>
#include <wx/dialog.h>
#include <wx/combobox.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <string>
#include <vector>

// Forward declarations
struct ThemeData;
class TickSlider;

// ── Settings dialog ──────────────────────────────────────────────
// Lets the user pick the model, context length, theme, and chat font.
// Model selection scans the filesystem for .gguf files (no Ollama API).
class SettingsDialog : public wxDialog
{
public:
    SettingsDialog(wxWindow* parent,
                   const std::string& currentModelPath,   // full GGUF path
                   const std::string& currentTheme,
                   int currentCtxSize,
                   int currentFontSize,
                   bool currentAgentDefaultOn,
                   const ThemeData& theme);
    ~SettingsDialog();

    // Selected values (use these after ShowModal() returns wxID_OK)
    std::string GetSelectedModel()    const { return m_selectedModel; }
    std::string GetSelectedTheme()    const { return m_selectedTheme; }
    int         GetSelectedCtxSize()  const { return m_selectedCtxSize; }
    int         GetSelectedFontSize() const { return m_selectedFontSize; }
    bool        GetSelectedAgentDefault() const { return m_selectedAgentDefault; }

    // Change flags
    bool WasModelChanged()        const { return m_modelChanged; }
    bool WasThemeChanged()        const { return m_themeChanged; }
    bool WasCtxSizeChanged()      const { return m_ctxSizeChanged; }
    bool WasFontSizeChanged()     const { return m_fontSizeChanged; }
    bool WasModelsFolderChanged() const { return m_modelsFolderChanged; }
    bool WasAgentDefaultChanged() const { return m_agentDefaultChanged; }

private:
    void OnOK(wxCommandEvent& event);
    void OnCancel(wxCommandEvent& event);
    void OnManageModels(wxCommandEvent& event);
    void OnOpenModelsFolder(wxCommandEvent& event);
    void OnDownloadModels(wxCommandEvent& event);
    void OnChangeFolder(wxCommandEvent& event);
    void OnResetFolder(wxCommandEvent& event);

    void CreateControls();
    void PopulateModelList();
    void UpdateFolderUi();        // Refreshes Location row from GetModelsDir()
    void ApplyTheme();

    // Helpers for consistent styling of custom buttons
    wxButton* MakeNeutralButton(wxWindow* parent, const wxString& label);
    wxButton* MakePrimaryButton(wxWindow* parent, const wxString& label);
    wxPanel*  MakeSectionDivider(wxWindow* parent);
    wxStaticText* MakeSectionHeader(wxWindow* parent, const wxString& text);

    // UI widgets
    wxComboBox*   m_modelComboBox = nullptr;
    wxStaticText* m_statusText    = nullptr;
    wxStaticText* m_locationPath  = nullptr;   // Shows active models folder
    wxStaticText* m_defaultPathRow = nullptr;  // "Default: ..." hint, shown only when override active
    wxButton*     m_changeBtn     = nullptr;
    wxButton*     m_resetBtn      = nullptr;   // Hidden in casual mode
    wxComboBox*   m_themeComboBox = nullptr;
    TickSlider*   m_ctxSlider     = nullptr;
    TickSlider*   m_fontSlider    = nullptr;
    wxCheckBox*   m_agentDefaultCheckBox = nullptr;
    wxButton*     m_okBtn         = nullptr;
    wxButton*     m_cancelBtn     = nullptr;

    // Model list: display name → full path mapping
    std::vector<std::string> m_modelPaths;   // parallel to combobox items

    // Selected / original values
    std::string m_selectedModel;
    std::string m_selectedTheme;
    int         m_selectedCtxSize;
    int         m_selectedFontSize;
    bool        m_selectedAgentDefault = false;

    std::string m_originalModel;
    std::string m_originalTheme;
    int         m_originalCtxSize;
    int         m_originalFontSize;
    bool        m_originalAgentDefault = false;

    // Folder-override change tracking.
    // Change/Reset commit to wxFileConfig immediately so the combo can scan
    // the new location live. The destructor reverts if OK was never pressed.
    std::string m_originalFolderOverride;     // captured at construction
    bool        m_confirmedOk = false;        // set by OnOK; suppresses revert

    // Change flags (set in OnOK)
    bool m_modelChanged        = false;
    bool m_themeChanged        = false;
    bool m_ctxSizeChanged      = false;
    bool m_fontSizeChanged     = false;
    bool m_modelsFolderChanged = false;
    bool m_agentDefaultChanged = false;

    const ThemeData* m_theme;

    wxDECLARE_EVENT_TABLE();
};
