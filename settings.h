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
class SecretsStore;
class EndpointStore;

// ── Settings dialog ──────────────────────────────────────────────
// Lets the user pick the model, context length, theme, and chat font.
// Model selection scans the filesystem for .gguf files (no Ollama API).
//
// All committal buttons are plain wxButtons styled like the chat Send
// button (t.accentButton fill, t.accentButtonText label). Cancel is a
// flat borderless wxButton with muted text. See settings.cpp for the
// rationale — short version is: this dialog wears Telegram clothes,
// not terminal clothes.
class SettingsDialog : public wxDialog
{
public:
    SettingsDialog(wxWindow* parent,
                   const std::string& currentModelPath,   // configured full GGUF path
                   const std::string& loadedModelPath,    // currently running full GGUF path, if any
                   const std::string& currentTheme,
                   int currentCtxSize,
                   int currentFontSize,
                   bool currentAgentDefaultOn,
                   bool currentContextMeterOn,
                   bool currentKvCacheQ8,
                   const ThemeData& theme,
                   SecretsStore* secretsStore = nullptr,
                   EndpointStore* endpointStore = nullptr);
    ~SettingsDialog();

    // Selected values (use these after ShowModal() returns wxID_OK)
    std::string GetSelectedModel()    const { return m_selectedModel; }
    std::string GetSelectedTheme()    const { return m_selectedTheme; }
    int         GetSelectedCtxSize()  const { return m_selectedCtxSize; }
    int         GetSelectedFontSize() const { return m_selectedFontSize; }
    bool        GetSelectedAgentDefault() const { return m_selectedAgentDefault; }
    bool        GetSelectedContextMeter() const { return m_selectedContextMeter; }
    bool        GetSelectedKvCacheQ8()    const { return m_selectedKvCacheQ8; }

    // Change flags
    bool WasModelChanged()        const { return m_modelChanged; }
    bool WasThemeChanged()        const { return m_themeChanged; }
    bool WasCtxSizeChanged()      const { return m_ctxSizeChanged; }
    bool WasFontSizeChanged()     const { return m_fontSizeChanged; }
    bool WasModelsFolderChanged() const { return m_modelsFolderChanged; }
    bool WasAgentDefaultChanged() const { return m_agentDefaultChanged; }
    bool WasContextMeterChanged() const { return m_contextMeterChanged; }
    bool WasKvCacheQ8Changed()    const { return m_kvCacheQ8Changed; }

private:
    void OnOK(wxCommandEvent& event);
    void OnCancel(wxCommandEvent& event);
    void OnManageModels(wxCommandEvent& event);
    void OnOpenModelsFolder(wxCommandEvent& event);
    void OnDownloadModels(wxCommandEvent& event);
    void OnChangeFolder(wxCommandEvent& event);
    void OnResetFolder(wxCommandEvent& event);
    void OnManageConnections(wxCommandEvent& event);
    void OnManageEndpoints(wxCommandEvent& event);

    void CreateControls();
    void PopulateModelList();
    void UpdateFolderUi();        // Refreshes Location row from GetModelsDir()
    void UpdateConnectionsLabel();// Refreshes "N connections configured" hint
    void UpdateEndpointsLabel();  // Refreshes "N remote endpoints configured" hint
    void ApplyTheme();

    // Helpers for consistent styling of section headers / dividers
    wxPanel*      MakeSectionDivider(wxWindow* parent);
    wxStaticText* MakeSectionHeader(wxWindow* parent, const wxString& text);

    // Creates a Send-style accent button and registers it in m_accentBtns
    // so ApplyTheme() can re-tint it after the neutral cascade. Height
    // defaults to 32 (action-row / footer); pass 26 for inline buttons
    // that sit next to text labels.
    wxButton*     MakeAccentButton(wxWindow* parent,
                                   wxWindowID id,
                                   const wxString& label,
                                   int height = 32);

    // UI widgets
    wxComboBox*   m_modelComboBox  = nullptr;
    wxStaticText* m_statusText     = nullptr;
    wxStaticText* m_locationPath   = nullptr;   // Shows active models folder
    wxStaticText* m_defaultPathRow = nullptr;   // "Default: ..." hint, shown only when override active
    wxButton*     m_changeBtn      = nullptr;
    wxButton*     m_resetBtn       = nullptr;   // Hidden in casual mode
    wxComboBox*   m_themeComboBox  = nullptr;
    TickSlider*   m_ctxSlider      = nullptr;
    TickSlider*   m_fontSlider     = nullptr;
    wxCheckBox*   m_agentDefaultCheckBox = nullptr;
    wxCheckBox*   m_contextMeterCheckBox = nullptr;
    wxCheckBox*   m_kvCacheQ8CheckBox = nullptr;
    wxButton*     m_okBtn          = nullptr;
    wxButton*     m_cancelBtn      = nullptr;

    // Connections section — opens ConnectionsDialog on click.
    wxStaticText* m_connectionsLabel = nullptr;
    wxButton*     m_manageConnBtn    = nullptr;
    SecretsStore* m_secretsStore     = nullptr;   // non-owning

    // Remote Endpoints section — opens EndpointsDialog on click.
    wxStaticText*  m_endpointsLabel     = nullptr;
    wxButton*      m_manageEndpointsBtn = nullptr;
    EndpointStore* m_endpointStore      = nullptr;   // non-owning

    // Every button that gets the solid accent fill in ApplyTheme().
    // Populated by MakeAccentButton() as buttons are constructed.
    // Cancel deliberately stays out of this list — it's the flat one.
    std::vector<wxButton*> m_accentBtns;

    // Every section gap band (the thick darker strip between sections).
    // Populated by MakeSectionDivider() and repainted with t.bgMain in
    // ApplyTheme() so they read as slots behind the dialog surface
    // rather than borders inside it — Telegram's section-band trick.
    std::vector<wxPanel*> m_dividers;

    // Model list: display name → full path mapping
    std::vector<std::string> m_modelPaths;   // parallel to combobox items

    // Selected / original values
    std::string m_selectedModel;
    std::string m_loadedModelPath;
    std::string m_selectedTheme;
    int         m_selectedCtxSize;
    int         m_selectedFontSize;
    bool        m_selectedAgentDefault = false;
    bool        m_selectedContextMeter = true;
    bool        m_selectedKvCacheQ8 = true;

    std::string m_originalModel;
    std::string m_originalTheme;
    int         m_originalCtxSize;
    int         m_originalFontSize;
    bool        m_originalAgentDefault = false;
    bool        m_originalContextMeter = true;
    bool        m_originalKvCacheQ8 = true;

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
    bool m_contextMeterChanged = false;
    bool        m_kvCacheQ8Changed = false;

    const ThemeData* m_theme;

    wxDECLARE_EVENT_TABLE();
};
