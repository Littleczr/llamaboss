// settings.cpp
//
// Settings dialog for LlamaBoss.
// Model selection scans %LOCALAPPDATA%\LlamaBoss\models\ for .gguf files.
// No Ollama API dependency.

#include "settings.h"
#include "model_downloader.h"
#include "model_manager.h"
#include "server_manager.h"
#include "theme.h"

#include <wx/fileconf.h>
#include <wx/msgdlg.h>
#include <wx/filename.h>

#include <sstream>
#include <iomanip>

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

wxBEGIN_EVENT_TABLE(SettingsDialog, wxDialog)
    EVT_BUTTON(wxID_OK,     SettingsDialog::OnOK)
    EVT_BUTTON(wxID_CANCEL, SettingsDialog::OnCancel)
wxEND_EVENT_TABLE()

SettingsDialog::SettingsDialog(wxWindow* parent,
                               const std::string& currentModelPath,
                               const std::string& currentTheme,
                               const ThemeData& theme)
    : wxDialog(parent, wxID_ANY, "Settings", wxDefaultPosition, wxSize(550, 320))
    , m_selectedModel(currentModelPath)
    , m_selectedTheme(currentTheme)
    , m_originalModel(currentModelPath)
    , m_originalTheme(currentTheme)
    , m_modelChanged(false)
    , m_themeChanged(false)
    , m_theme(&theme)
{
    wxFont f = GetFont();
    f.SetPointSize(10);
    f.SetWeight(wxFONTWEIGHT_SEMIBOLD);
    SetFont(f);

    CreateControls();
    PopulateModelList();
}

void SettingsDialog::CreateControls()
{
    auto* mainSizer = new wxBoxSizer(wxVERTICAL);

    // ── Model selection ──────────────────────────────────────────
    auto* modelLabel = new wxStaticText(this, wxID_ANY, "Model (GGUF):");
    mainSizer->Add(modelLabel, 0, wxALL, 5);

    m_modelComboBox = new wxComboBox(this, wxID_ANY, "",
        wxDefaultPosition, wxDefaultSize, 0, nullptr,
        wxCB_DROPDOWN | wxCB_READONLY);
    mainSizer->Add(m_modelComboBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);

    // ── Action buttons row ───────────────────────────────────────
    auto* actionSizer = new wxBoxSizer(wxHORIZONTAL);

    auto* downloadBtn = new wxButton(this, wxID_ANY, "Download Models...");
    downloadBtn->Bind(wxEVT_BUTTON, &SettingsDialog::OnDownloadModels, this);
    actionSizer->Add(downloadBtn, 0, wxRIGHT, 5);

    auto* manageBtn = new wxButton(this, wxID_ANY, "Manage Models...");
    manageBtn->Bind(wxEVT_BUTTON, &SettingsDialog::OnManageModels, this);
    actionSizer->Add(manageBtn, 0, wxRIGHT, 5);

    auto* openFolderBtn = new wxButton(this, wxID_ANY, "Open Models Folder");
    openFolderBtn->Bind(wxEVT_BUTTON, &SettingsDialog::OnOpenModelsFolder, this);
    actionSizer->Add(openFolderBtn, 0);

    mainSizer->Add(actionSizer, 0, wxLEFT | wxRIGHT | wxBOTTOM, 5);

    // ── Status text ──────────────────────────────────────────────
    m_statusText = new wxStaticText(this, wxID_ANY, "");
    mainSizer->Add(m_statusText, 0, wxLEFT | wxRIGHT, 5);

    // ── Theme selection ──────────────────────────────────────────
    mainSizer->AddSpacer(10);
    auto* themeLabel = new wxStaticText(this, wxID_ANY, "Theme:");
    mainSizer->Add(themeLabel, 0, wxLEFT | wxRIGHT | wxTOP, 5);

    wxArrayString themeChoices;
    themeChoices.Add("Dark");
    themeChoices.Add("Light");
    themeChoices.Add("System");
    m_themeComboBox = new wxComboBox(this, wxID_ANY, "",
        wxDefaultPosition, wxDefaultSize, themeChoices,
        wxCB_DROPDOWN | wxCB_READONLY);

    if (m_selectedTheme == "light")       m_themeComboBox->SetSelection(1);
    else if (m_selectedTheme == "system") m_themeComboBox->SetSelection(2);
    else                                  m_themeComboBox->SetSelection(0);

    mainSizer->Add(m_themeComboBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 5);

    // ── OK / Cancel ──────────────────────────────────────────────
    auto* buttonSizer = CreateButtonSizer(wxOK | wxCANCEL);
    mainSizer->Add(buttonSizer, 0, wxEXPAND | wxALL, 5);

    SetSizer(mainSizer);

    // ── Apply theme colors ───────────────────────────────────────
    if (m_theme) {
        SetBackgroundColour(m_theme->sidebarSelected);

        for (auto* child : GetChildren()) {
            if (auto* lbl = dynamic_cast<wxStaticText*>(child))
                lbl->SetForegroundColour(m_theme->textPrimary);
            else if (auto* btn = dynamic_cast<wxButton*>(child))
            {
                btn->SetBackgroundColour(m_theme->modelPillBg);
                btn->SetForegroundColour(m_theme->textPrimary);
            }
            else if (auto* cb = dynamic_cast<wxComboBox*>(child))
            {
                cb->SetBackgroundColour(*wxWHITE);
                cb->SetForegroundColour(*wxBLACK);
            }
        }
    }
}

void SettingsDialog::PopulateModelList()
{
    m_modelComboBox->Clear();
    m_modelPaths.clear();

    auto models = ServerManager::ScanModels();

    if (models.empty()) {
        m_statusText->SetLabel("No .gguf files found in: " + ServerManager::GetModelsDir());
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

void SettingsDialog::OnOK(wxCommandEvent&)
{
    int sel = m_modelComboBox->GetSelection();
    if (sel >= 0 && sel < (int)m_modelPaths.size()) {
        m_selectedModel = m_modelPaths[sel];
    }

    int themeSel = m_themeComboBox->GetSelection();
    m_selectedTheme = (themeSel == 2) ? "system" : (themeSel == 1) ? "light" : "dark";

    m_modelChanged = (m_selectedModel != m_originalModel);
    m_themeChanged = (m_selectedTheme != m_originalTheme);

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

    // If the user downloaded something, refresh the model list so it
    // appears immediately in the combo box without reopening Settings.
    if (dlg.HadSuccessfulDownload())
        PopulateModelList();
}

void SettingsDialog::OnManageModels(wxCommandEvent&)
{
    ModelManagerDialog dlg(this, m_theme);
    dlg.ShowModal();

    // Refresh model list after managing
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
