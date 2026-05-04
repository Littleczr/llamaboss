// settings.cpp
//
// Settings dialog for LlamaBoss.
// Section-organised layout with dark-themed surfaces:
//   • Model (combo, action buttons, status)
//   • Context length (snap-to-tick slider)
//   • Appearance (theme dropdown, font size slider)
//   • OK / Cancel footer
//
// Model selection scans %LOCALAPPDATA%\LlamaBoss\models\ for .gguf files.
// No Ollama API dependency.

#include "settings.h"
#include "model_downloader.h"
#include "model_manager.h"
#include "server_manager.h"
#include "theme.h"
#include "widgets.h"   // TickSlider, ApplyDialogThemeRecursive, ApplyDarkTitleBar

#include <wx/fileconf.h>
#include <wx/msgdlg.h>
#include <wx/filename.h>
#include <wx/statline.h>
#include <wx/dirdlg.h>

#include <sstream>
#include <iomanip>
#include <functional>

// ── Helper: human-readable file size ─────────────────────────────
static std::string FormatFileSize(const std::string& path)
{
    wxFileName fn(path);
    if (!fn.FileExists()) return "";

    wxULongLong size = fn.GetSize();
    if (size == wxInvalidSize) return "";

    double s = size.ToDouble();
    const char* units[] = { "B", "KB", "MB", "GB", "TB" };
    int idx = 0;
    while (s >= 1024.0 && idx < 4) { s /= 1024.0; idx++; }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(idx == 0 ? 0 : 1) << s << " " << units[idx];
    return oss.str();
}

// ── Human-readable ctx size ("8k tokens") ─────────────────────────
static std::string FormatCtxSize(int tokens)
{
    std::ostringstream oss;
    if (tokens >= 1024 && (tokens % 1024 == 0)) {
        oss << (tokens / 1024) << "k tokens";
    } else {
        oss << tokens << " tokens";
    }
    return oss.str();
}

wxBEGIN_EVENT_TABLE(SettingsDialog, wxDialog)
    EVT_BUTTON(wxID_OK,     SettingsDialog::OnOK)
    EVT_BUTTON(wxID_CANCEL, SettingsDialog::OnCancel)
wxEND_EVENT_TABLE()

SettingsDialog::SettingsDialog(wxWindow* parent,
                               const std::string& currentModelPath,
                               const std::string& currentTheme,
                               int currentCtxSize,
                               int currentFontSize,
                               bool currentAgentDefaultOn,
                               const ThemeData& theme)
    : wxDialog(parent, wxID_ANY, "Settings",
               wxDefaultPosition, wxSize(580, 700))
    , m_selectedModel(currentModelPath)
    , m_selectedTheme(currentTheme)
    , m_selectedCtxSize(currentCtxSize)
    , m_selectedFontSize(currentFontSize)
    , m_selectedAgentDefault(currentAgentDefaultOn)
    , m_originalModel(currentModelPath)
    , m_originalTheme(currentTheme)
    , m_originalCtxSize(currentCtxSize)
    , m_originalFontSize(currentFontSize)
    , m_originalAgentDefault(currentAgentDefaultOn)
    , m_originalFolderOverride(ServerManager::GetModelsDirOverride())
    , m_theme(&theme)
{
    wxFont f = GetFont();
    f.SetPointSize(10);
    f.SetWeight(wxFONTWEIGHT_NORMAL);
    SetFont(f);

    CreateControls();
    PopulateModelList();
    UpdateFolderUi();
    ApplyTheme();

    // Match the title bar to the body when dark theme is active.
    ApplyDarkTitleBar(this, theme.name != "light");

    Centre();
}

// ═══════════════════════════════════════════════════════════════════
//  Destructor — revert folder override if dialog wasn't confirmed
// ═══════════════════════════════════════════════════════════════════
//
// Change/Reset persist to wxFileConfig immediately so PopulateModelList()
// can see the new location. If the user closes the dialog without clicking
// OK (Cancel, ESC, X, anything), we roll that back here. One safety net
// covers every exit path.
SettingsDialog::~SettingsDialog()
{
    if (!m_confirmedOk) {
        const std::string current = ServerManager::GetModelsDirOverride();
        if (current != m_originalFolderOverride) {
            ServerManager::SetModelsDirOverride(m_originalFolderOverride);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Construction helpers
// ═══════════════════════════════════════════════════════════════════

wxButton* SettingsDialog::MakeNeutralButton(wxWindow* parent, const wxString& label)
{
    auto* btn = new wxButton(parent, wxID_ANY, label,
        wxDefaultPosition, wxSize(-1, 30), wxBORDER_NONE);
    wxFont bf = btn->GetFont();
    bf.SetPointSize(9);
    btn->SetFont(bf);
    return btn;
}

wxButton* SettingsDialog::MakePrimaryButton(wxWindow* parent, const wxString& label)
{
    auto* btn = new wxButton(parent, wxID_ANY, label,
        wxDefaultPosition, wxSize(-1, 30), wxBORDER_NONE);
    wxFont bf = btn->GetFont();
    bf.SetPointSize(9);
    bf.SetWeight(wxFONTWEIGHT_SEMIBOLD);
    btn->SetFont(bf);
    return btn;
}

wxPanel* SettingsDialog::MakeSectionDivider(wxWindow* parent)
{
    auto* line = new wxPanel(parent, wxID_ANY,
        wxDefaultPosition, wxSize(-1, 1));
    return line;
}

wxStaticText* SettingsDialog::MakeSectionHeader(wxWindow* parent, const wxString& text)
{
    auto* lbl = new wxStaticText(parent, wxID_ANY, text);
    wxFont hf = lbl->GetFont();
    hf.SetPointSize(10);
    hf.SetWeight(wxFONTWEIGHT_SEMIBOLD);
    lbl->SetFont(hf);
    return lbl;
}

// ═══════════════════════════════════════════════════════════════════
//  Layout
// ═══════════════════════════════════════════════════════════════════

void SettingsDialog::CreateControls()
{
    auto* rootSizer = new wxBoxSizer(wxVERTICAL);

    // Content lives inside a padded body panel so we can give the dialog
    // consistent 18px margins without every child having to specify them.
    auto* body = new wxPanel(this, wxID_ANY);
    auto* bodySizer = new wxBoxSizer(wxVERTICAL);

    // ─────────────────────────────────────────────────────────────
    //  SECTION 1 — MODEL
    // ─────────────────────────────────────────────────────────────
    bodySizer->Add(MakeSectionHeader(body, "Model"), 0, wxBOTTOM, 6);

    m_modelComboBox = new wxComboBox(body, wxID_ANY, "",
        wxDefaultPosition, wxSize(-1, 28), 0, nullptr,
        wxCB_DROPDOWN | wxCB_READONLY);
    bodySizer->Add(m_modelComboBox, 0, wxEXPAND | wxBOTTOM, 8);

    // Action buttons row
    auto* actionSizer = new wxBoxSizer(wxHORIZONTAL);
    auto* downloadBtn = MakeNeutralButton(body, "Download Models...");
    auto* manageBtn   = MakeNeutralButton(body, "Manage Models...");
    auto* openBtn     = MakeNeutralButton(body, "Open Models Folder");
    downloadBtn->Bind(wxEVT_BUTTON, &SettingsDialog::OnDownloadModels, this);
    manageBtn  ->Bind(wxEVT_BUTTON, &SettingsDialog::OnManageModels,   this);
    openBtn    ->Bind(wxEVT_BUTTON, &SettingsDialog::OnOpenModelsFolder, this);
    actionSizer->Add(downloadBtn, 0, wxRIGHT, 6);
    actionSizer->Add(manageBtn,   0, wxRIGHT, 6);
    actionSizer->Add(openBtn,     0);
    bodySizer->Add(actionSizer, 0, wxBOTTOM, 10);

    // ── Location row ─────────────────────────────────────────────
    // Shows the active models folder with Change/Reset affordances.
    // Reset is hidden when no override is active (casual mode). The
    // muted "Default: ..." line below is likewise shown only when
    // the user has pointed at a custom folder.
    auto* locRow = new wxBoxSizer(wxHORIZONTAL);

    auto* locLabel = new wxStaticText(body, wxID_ANY, "Location:");
    { wxFont lf = locLabel->GetFont(); lf.SetPointSize(9); locLabel->SetFont(lf); }
    locRow->Add(locLabel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);

    m_locationPath = new wxStaticText(body, wxID_ANY, "",
        wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_MIDDLE);
    { wxFont pf = m_locationPath->GetFont(); pf.SetPointSize(9); m_locationPath->SetFont(pf); }
    locRow->Add(m_locationPath, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

    m_changeBtn = MakeNeutralButton(body, "Change...");
    m_resetBtn  = MakeNeutralButton(body, "Reset");
    m_changeBtn->Bind(wxEVT_BUTTON, &SettingsDialog::OnChangeFolder, this);
    m_resetBtn ->Bind(wxEVT_BUTTON, &SettingsDialog::OnResetFolder,  this);
    locRow->Add(m_changeBtn, 0, wxRIGHT, 6);
    locRow->Add(m_resetBtn,  0);

    bodySizer->Add(locRow, 0, wxEXPAND | wxBOTTOM, 4);

    // Muted "Default: <path>" row — hidden when no override is active.
    m_defaultPathRow = new wxStaticText(body, wxID_ANY, "",
        wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_MIDDLE);
    { wxFont df = m_defaultPathRow->GetFont(); df.SetPointSize(9); m_defaultPathRow->SetFont(df); }
    bodySizer->Add(m_defaultPathRow, 0, wxEXPAND | wxBOTTOM, 6);

    // Status line (muted)
    m_statusText = new wxStaticText(body, wxID_ANY, "");
    wxFont sf = m_statusText->GetFont();
    sf.SetPointSize(9);
    m_statusText->SetFont(sf);
    bodySizer->Add(m_statusText, 0, wxBOTTOM, 14);

    // ─────────────────────────────────────────────────────────────
    //  SECTION 2 — CONTEXT LENGTH
    // ─────────────────────────────────────────────────────────────
    bodySizer->Add(MakeSectionDivider(body), 0, wxEXPAND | wxBOTTOM, 14);

    auto* ctxHeaderRow = new wxBoxSizer(wxHORIZONTAL);
    ctxHeaderRow->Add(MakeSectionHeader(body, "Context length"), 0);
    ctxHeaderRow->AddStretchSpacer();
    auto* ctxValueLabel = new wxStaticText(body, wxID_ANY,
        wxString::FromUTF8(FormatCtxSize(m_selectedCtxSize)));
    wxFont cvf = ctxValueLabel->GetFont();
    cvf.SetPointSize(9);
    cvf.SetWeight(wxFONTWEIGHT_SEMIBOLD);
    ctxValueLabel->SetFont(cvf);
    ctxHeaderRow->Add(ctxValueLabel, 0, wxALIGN_CENTER_VERTICAL);
    bodySizer->Add(ctxHeaderRow, 0, wxEXPAND | wxBOTTOM, 4);

    auto* ctxHint = new wxStaticText(body, wxID_ANY,
        "How much of the conversation the model can keep in memory.");
    wxFont chf = ctxHint->GetFont();
    chf.SetPointSize(9);
    ctxHint->SetFont(chf);
    bodySizer->Add(ctxHint, 0, wxBOTTOM, 8);

    m_ctxSlider = new TickSlider(
        body,
        { 4096, 8192, 16384, 32768, 65536, 131072, 262144 },
        { "4k", "8k", "16k", "32k", "64k", "128k", "256k" },
        m_selectedCtxSize,
        [this, ctxValueLabel](int newVal) {
            m_selectedCtxSize = newVal;
            ctxValueLabel->SetLabel(wxString::FromUTF8(FormatCtxSize(newVal)));
        });
    bodySizer->Add(m_ctxSlider, 0, wxEXPAND | wxBOTTOM, 14);

    // ─────────────────────────────────────────────────────────────
    //  SECTION 3 — BEHAVIOR
    // ─────────────────────────────────────────────────────────────
    //  Controls how chats behave. Currently only hosts the agent-mode
    //  default; this section is the natural home for future toggles
    //  like auto-titling, streaming, or tool-call confirmation.
    bodySizer->Add(MakeSectionDivider(body), 0, wxEXPAND | wxBOTTOM, 14);
    bodySizer->Add(MakeSectionHeader(body, "Behavior"), 0, wxBOTTOM, 8);

    m_agentDefaultCheckBox = new wxCheckBox(
        body, wxID_ANY, "Start new chats with agent mode enabled");
    { wxFont acf = m_agentDefaultCheckBox->GetFont();
      acf.SetPointSize(10);
      m_agentDefaultCheckBox->SetFont(acf); }
    m_agentDefaultCheckBox->SetValue(m_selectedAgentDefault);
    bodySizer->Add(m_agentDefaultCheckBox, 0, wxBOTTOM, 4);

    auto* agentHint = new wxStaticText(body, wxID_ANY,
        "The robot button toggles agent mode for the current chat.");
    { wxFont ah = agentHint->GetFont(); ah.SetPointSize(9); agentHint->SetFont(ah); }
    bodySizer->Add(agentHint, 0, wxBOTTOM, 14);

    // ─────────────────────────────────────────────────────────────
    //  SECTION 4 — APPEARANCE
    // ─────────────────────────────────────────────────────────────
    bodySizer->Add(MakeSectionDivider(body), 0, wxEXPAND | wxBOTTOM, 14);
    bodySizer->Add(MakeSectionHeader(body, "Appearance"), 0, wxBOTTOM, 8);

    // Theme row
    auto* themeRow = new wxBoxSizer(wxHORIZONTAL);
    auto* themeLabel = new wxStaticText(body, wxID_ANY, "Theme");
    themeLabel->SetMinSize(wxSize(90, -1));
    themeRow->Add(themeLabel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);

    wxArrayString themeChoices;
    themeChoices.Add("Dark");
    themeChoices.Add("Light");
    themeChoices.Add("System");
    m_themeComboBox = new wxComboBox(body, wxID_ANY, "",
        wxDefaultPosition, wxSize(-1, 28), themeChoices,
        wxCB_DROPDOWN | wxCB_READONLY);
    if (m_selectedTheme == "light")       m_themeComboBox->SetSelection(1);
    else if (m_selectedTheme == "system") m_themeComboBox->SetSelection(2);
    else                                  m_themeComboBox->SetSelection(0);
    themeRow->Add(m_themeComboBox, 1);
    bodySizer->Add(themeRow, 0, wxEXPAND | wxBOTTOM, 14);

    // Font size row (header + live value)
    auto* fontHeaderRow = new wxBoxSizer(wxHORIZONTAL);
    auto* fontLabel = new wxStaticText(body, wxID_ANY, "Chat font size");
    { wxFont ff = fontLabel->GetFont(); ff.SetWeight(wxFONTWEIGHT_SEMIBOLD); fontLabel->SetFont(ff); }
    fontHeaderRow->Add(fontLabel, 0, wxALIGN_CENTER_VERTICAL);
    fontHeaderRow->AddStretchSpacer();
    auto* fontValueLabel = new wxStaticText(body, wxID_ANY,
        wxString::Format("%dpt", m_selectedFontSize));
    wxFont fvf = fontValueLabel->GetFont();
    fvf.SetPointSize(9);
    fvf.SetWeight(wxFONTWEIGHT_SEMIBOLD);
    fontValueLabel->SetFont(fvf);
    fontHeaderRow->Add(fontValueLabel, 0, wxALIGN_CENTER_VERTICAL);
    bodySizer->Add(fontHeaderRow, 0, wxEXPAND | wxBOTTOM, 8);

    m_fontSlider = new TickSlider(
        body,
        { 10, 12, 14, 16, 18, 20, 22, 24 },
        { "10", "12", "14", "16", "18", "20", "22", "24" },
        m_selectedFontSize,
        [this, fontValueLabel](int newVal) {
            m_selectedFontSize = newVal;
            fontValueLabel->SetLabel(wxString::Format("%dpt", newVal));
        });
    bodySizer->Add(m_fontSlider, 0, wxEXPAND | wxBOTTOM, 14);

    body->SetSizer(bodySizer);
    rootSizer->Add(body, 1, wxEXPAND | wxALL, 18);

    // ─────────────────────────────────────────────────────────────
    //  FOOTER — OK / Cancel (right-aligned)
    // ─────────────────────────────────────────────────────────────
    auto* footer = new wxPanel(this, wxID_ANY);
    auto* footSizer = new wxBoxSizer(wxHORIZONTAL);
    footSizer->AddStretchSpacer();

    m_cancelBtn = new wxButton(footer, wxID_CANCEL, "Cancel",
        wxDefaultPosition, wxSize(90, 32), wxBORDER_NONE);
    m_okBtn     = new wxButton(footer, wxID_OK,     "OK",
        wxDefaultPosition, wxSize(90, 32), wxBORDER_NONE);
    wxFont bf = m_okBtn->GetFont();
    bf.SetPointSize(9);
    bf.SetWeight(wxFONTWEIGHT_SEMIBOLD);
    m_okBtn->SetFont(bf);
    m_cancelBtn->SetFont(bf);

    footSizer->Add(m_cancelBtn, 0, wxRIGHT, 8);
    footSizer->Add(m_okBtn,     0);
    footer->SetSizer(footSizer);
    rootSizer->Add(footer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 18);

    SetSizer(rootSizer);
    m_okBtn->SetDefault();
}

// ═══════════════════════════════════════════════════════════════════
//  Theming
// ═══════════════════════════════════════════════════════════════════

void SettingsDialog::ApplyTheme()
{
    if (!m_theme) return;
    const ThemeData& t = *m_theme;

    SetBackgroundColour(t.bgMain);

    for (auto* child : GetChildren()) {
        if (auto* p = dynamic_cast<wxPanel*>(child))
            p->SetBackgroundColour(t.bgMain);
    }

    // Apply label colors + neutral button fill to every widget recursively.
    // This fixes the old dialog's bug: GetChildren() isn't recursive, so
    // nested buttons (OK/Cancel) never got themed.
    ApplyDialogThemeRecursive(this, t.textPrimary, t.bgInputField, t.textPrimary);

    // Section dividers: 1px panels that got painted with textPrimary by the
    // cascade. Retarget them to borderSubtle by finding them by height.
    std::function<void(wxWindow*)> fixDividers = [&](wxWindow* w) {
        for (auto* child : w->GetChildren()) {
            if (auto* p = dynamic_cast<wxPanel*>(child)) {
                if (p->GetSize().y == 1)
                    p->SetBackgroundColour(t.borderSubtle);
            }
            if (child->GetChildren().GetCount() > 0)
                fixDividers(child);
        }
    };
    fixDividers(this);

    // Combo boxes — Windows still renders OS chrome on the dropdown arrow,
    // but at least the edit field matches the dark surfaces.
    if (m_modelComboBox) {
        m_modelComboBox->SetBackgroundColour(t.bgInputField);
        m_modelComboBox->SetForegroundColour(t.textPrimary);
    }
    if (m_themeComboBox) {
        m_themeComboBox->SetBackgroundColour(t.bgInputField);
        m_themeComboBox->SetForegroundColour(t.textPrimary);
    }

    // Status line gets the muted text color
    if (m_statusText)
        m_statusText->SetForegroundColour(t.textMuted);

    // "Default: <path>" hint is muted metadata, not primary content.
    if (m_defaultPathRow)
        m_defaultPathRow->SetForegroundColour(t.textMuted);

    // wxCheckBox isn't touched by ApplyDialogThemeRecursive (it handles
    // wxStaticText and wxButton only), so tint the label + surface here.
    if (m_agentDefaultCheckBox) {
        m_agentDefaultCheckBox->SetForegroundColour(t.textPrimary);
        m_agentDefaultCheckBox->SetBackgroundColour(t.bgMain);
    }

    // Hand the sliders our theme palette
    if (m_ctxSlider) {
        m_ctxSlider->SetColors(
            t.borderSubtle,     // track
            t.accentButton,     // fill
            t.accentButton,     // knob
            t.textPrimary,      // active label
            t.textMuted);       // dimmed labels
    }
    if (m_fontSlider) {
        m_fontSlider->SetColors(
            t.borderSubtle,
            t.accentButton,
            t.accentButton,
            t.textPrimary,
            t.textMuted);
    }

    // Primary button gets accent treatment (the recursive helper made it
    // neutral like everything else).
    if (m_okBtn) {
        m_okBtn->SetBackgroundColour(t.accentButton);
        m_okBtn->SetForegroundColour(t.accentButtonText);
    }

    Refresh();
}

// ═══════════════════════════════════════════════════════════════════
//  Populating / reading selection
// ═══════════════════════════════════════════════════════════════════

void SettingsDialog::PopulateModelList()
{
    m_modelComboBox->Clear();
    m_modelPaths.clear();

    auto models = ServerManager::ScanModelPaths();

    if (models.empty()) {
        m_statusText->SetLabel("No .gguf files found in: " +
            ServerManager::GetModelsDir());
        return;
    }

    int selectedIdx = -1;
    for (size_t i = 0; i < models.size(); ++i) {
        const auto& path = models[i];
        std::string displayName = ServerManager::ModelDisplayName(path);
        std::string sizeStr = FormatFileSize(path);
        if (!sizeStr.empty())
            displayName += "  (" + sizeStr + ")";

        m_modelComboBox->Append(wxString::FromUTF8(displayName));
        m_modelPaths.push_back(path);

        if (path == m_selectedModel)
            selectedIdx = (int)i;
    }

    if (selectedIdx >= 0)
        m_modelComboBox->SetSelection(selectedIdx);
    else if (m_modelComboBox->GetCount() > 0)
        m_modelComboBox->SetSelection(0);

    m_statusText->SetLabel(wxString::Format("Found %zu model(s)", models.size()));
}

// ═══════════════════════════════════════════════════════════════════
//  Event handlers
// ═══════════════════════════════════════════════════════════════════

void SettingsDialog::OnOK(wxCommandEvent&)
{
    int sel = m_modelComboBox->GetSelection();
    if (sel >= 0 && sel < (int)m_modelPaths.size()) {
        m_selectedModel = m_modelPaths[sel];
    }

    int themeSel = m_themeComboBox->GetSelection();
    m_selectedTheme = (themeSel == 2) ? "system"
                    : (themeSel == 1) ? "light"
                    :                   "dark";

    // Checkbox is authoritative — the live value may have been toggled
    // since construction without m_selectedAgentDefault being updated.
    if (m_agentDefaultCheckBox)
        m_selectedAgentDefault = m_agentDefaultCheckBox->GetValue();

    // Sliders keep m_selectedCtxSize / m_selectedFontSize live via their
    // onChange callbacks — nothing to do here beyond diff'ing.
    m_modelChanged        = (m_selectedModel         != m_originalModel);
    m_themeChanged        = (m_selectedTheme         != m_originalTheme);
    m_ctxSizeChanged      = (m_selectedCtxSize       != m_originalCtxSize);
    m_fontSizeChanged     = (m_selectedFontSize      != m_originalFontSize);
    m_agentDefaultChanged = (m_selectedAgentDefault  != m_originalAgentDefault);

    // Folder override was committed live by Change/Reset; compare current
    // value to what we captured at construction.
    m_modelsFolderChanged =
        (ServerManager::GetModelsDirOverride() != m_originalFolderOverride);

    // Tell the destructor the override stands as-is — don't revert.
    m_confirmedOk = true;

    EndModal(wxID_OK);
}

void SettingsDialog::OnCancel(wxCommandEvent&)
{
    EndModal(wxID_CANCEL);
}

void SettingsDialog::OnDownloadModels(wxCommandEvent&)
{
    ModelDownloaderDialog dlg(this, m_theme);
    dlg.ShowModal();

    if (dlg.HadSuccessfulDownload())
        PopulateModelList();
}

void SettingsDialog::OnManageModels(wxCommandEvent&)
{
    ModelManagerDialog dlg(this, m_theme);
    dlg.ShowModal();
    PopulateModelList();
}

void SettingsDialog::OnOpenModelsFolder(wxCommandEvent&)
{
    std::string modelsDir = ServerManager::GetModelsDir();
    ServerManager::EnsureDataDirs();

#ifdef __WXMSW__
    wxExecute("explorer \"" + wxString::FromUTF8(modelsDir) + "\"", wxEXEC_ASYNC);
#endif
}

// ═══════════════════════════════════════════════════════════════════
//  Folder-override UI
// ═══════════════════════════════════════════════════════════════════
//
// Reflects the current override state into the Location row. Called
// from the constructor and after every Change/Reset. Hides the Reset
// button and the "Default:" hint when we're in casual mode.
void SettingsDialog::UpdateFolderUi()
{
    if (!m_locationPath) return;

    const std::string active = ServerManager::GetModelsDir();
    const std::string def    = ServerManager::GetDefaultModelsDir();
    const bool        casual = ServerManager::IsCasualMode();

    m_locationPath->SetLabel(wxString::FromUTF8(active));

    if (m_resetBtn)
        m_resetBtn->Show(!casual);

    if (m_defaultPathRow) {
        if (casual) {
            m_defaultPathRow->SetLabel("");
            m_defaultPathRow->Show(false);
        } else {
            m_defaultPathRow->SetLabel(
                wxString::FromUTF8("Default:  " + def));
            m_defaultPathRow->Show(true);
        }
    }

    // Re-lay the dialog so the collapsed/shown rows reclaim space.
    Layout();
}

void SettingsDialog::OnChangeFolder(wxCommandEvent&)
{
    wxDirDialog dlg(this,
        "Choose models folder",
        wxString::FromUTF8(ServerManager::GetModelsDir()),
        wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);

    if (dlg.ShowModal() != wxID_OK) return;

    const std::string picked(dlg.GetPath().ToUTF8().data());

    // Normalize: if the user picked the exact default path, drop the
    // override entirely — they're effectively back in casual mode and
    // should get bundle semantics + deterministic mmproj pairing.
    wxFileName pickedFn(wxString::FromUTF8(picked));
    wxFileName defaultFn(wxString::FromUTF8(ServerManager::GetDefaultModelsDir()));
    if (pickedFn.SameAs(defaultFn))
        ServerManager::SetModelsDirOverride("");
    else
        ServerManager::SetModelsDirOverride(picked);

    UpdateFolderUi();
    PopulateModelList();
}

void SettingsDialog::OnResetFolder(wxCommandEvent&)
{
    ServerManager::SetModelsDirOverride("");
    UpdateFolderUi();
    PopulateModelList();
}
