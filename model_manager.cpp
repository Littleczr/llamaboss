// model_manager.cpp
//
// Filesystem-based model manager for LlamaBoss.
// Lists .gguf files from %LOCALAPPDATA%\LlamaBoss\models\
// Supports delete (removes the file) and opening the folder.

#include "model_manager.h"
#include "server_manager.h"
#include "theme.h"

#include <wx/filename.h>
#include <wx/msgdlg.h>

#include <sstream>
#include <iomanip>

// ── Helper: human-readable size ───────────────────────────────────
static std::string FormatSize(wxULongLong bytes)
{
    const char* units[] = { "B", "KB", "MB", "GB", "TB" };
    int idx = 0;
    double size = bytes.ToDouble();
    while (size >= 1024.0 && idx < 4) { size /= 1024.0; idx++; }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(idx == 0 ? 0 : 1) << size << " " << units[idx];
    return oss.str();
}

// ═══════════════════════════════════════════════════════════════════
//  ModelManagerDialog
// ═══════════════════════════════════════════════════════════════════

enum {
    ID_MM_DELETE     = wxID_HIGHEST + 200,
    ID_MM_REFRESH    = wxID_HIGHEST + 202,
    ID_MM_OPENFOLDER = wxID_HIGHEST + 204,
};

wxBEGIN_EVENT_TABLE(ModelManagerDialog, wxDialog)
    EVT_BUTTON(ID_MM_DELETE,     ModelManagerDialog::OnDeleteClicked)
    EVT_BUTTON(ID_MM_REFRESH,    ModelManagerDialog::OnRefreshClicked)
    EVT_BUTTON(ID_MM_OPENFOLDER, ModelManagerDialog::OnOpenFolderClicked)
    EVT_BUTTON(wxID_CLOSE,       ModelManagerDialog::OnClose)
wxEND_EVENT_TABLE()

ModelManagerDialog::ModelManagerDialog(wxWindow* parent, const ThemeData* theme)
    : wxDialog(parent, wxID_ANY, "Manage Models", wxDefaultPosition, wxSize(560, 400),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_theme(theme)
{
    CreateControls();
    RefreshModelList();
}

void ModelManagerDialog::CreateControls()
{
    auto* mainSizer = new wxBoxSizer(wxVERTICAL);

    // ── Header ─────────────────────────────────────────────────────
    auto* headerLabel = new wxStaticText(this, wxID_ANY,
        "Models folder: " + ServerManager::GetModelsDir());
    wxFont hf = headerLabel->GetFont();
    hf.SetPointSize(9);
    headerLabel->SetFont(hf);
    mainSizer->Add(headerLabel, 0, wxALL, 5);

    // ── Model list ─────────────────────────────────────────────────
    m_modelList = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 220),
                                 wxLC_REPORT | wxLC_SINGLE_SEL);
    m_modelList->AppendColumn("Model", wxLIST_FORMAT_LEFT, 320);
    m_modelList->AppendColumn("Size", wxLIST_FORMAT_RIGHT, 100);
    mainSizer->Add(m_modelList, 1, wxEXPAND | wxLEFT | wxRIGHT, 5);

    // ── Action buttons ─────────────────────────────────────────────
    auto* actionSizer = new wxBoxSizer(wxHORIZONTAL);
    m_deleteButton  = new wxButton(this, ID_MM_DELETE,     "Delete Selected");
    m_refreshButton = new wxButton(this, ID_MM_REFRESH,    "Refresh");
    auto* openBtn   = new wxButton(this, ID_MM_OPENFOLDER, "Open Folder");

    actionSizer->Add(m_deleteButton, 0, wxRIGHT, 5);
    actionSizer->Add(m_refreshButton, 0, wxRIGHT, 5);
    actionSizer->AddStretchSpacer();
    actionSizer->Add(openBtn, 0);
    mainSizer->Add(actionSizer, 0, wxEXPAND | wxALL, 5);

    // ── Status ─────────────────────────────────────────────────────
    m_statusText = new wxStaticText(this, wxID_ANY, "");
    mainSizer->Add(m_statusText, 0, wxALL, 5);

    // ── Hint ───────────────────────────────────────────────────────
    auto* hintText = new wxStaticText(this, wxID_ANY,
        "To add models, download .gguf files and place them in the models folder.");
    wxFont sf = hintText->GetFont();
    sf.SetPointSize(9);
    sf.SetStyle(wxFONTSTYLE_ITALIC);
    hintText->SetFont(sf);
    mainSizer->Add(hintText, 0, wxLEFT | wxRIGHT | wxBOTTOM, 5);

    // ── Close button ──────────────────────────────────────────────
    auto* closeSizer = new wxBoxSizer(wxHORIZONTAL);
    closeSizer->AddStretchSpacer();
    closeSizer->Add(new wxButton(this, wxID_CLOSE, "Close"), 0);
    mainSizer->Add(closeSizer, 0, wxEXPAND | wxALL, 5);

    SetSizer(mainSizer);

    // ── Apply theme colors ───────────────────────────────────────
    if (m_theme) {
        SetBackgroundColour(m_theme->bgToolbar);

        for (auto* child : GetChildren()) {
            if (auto* lbl = dynamic_cast<wxStaticText*>(child)) {
                lbl->SetForegroundColour(m_theme->textPrimary);
            } else if (auto* btn = dynamic_cast<wxButton*>(child)) {
                btn->SetBackgroundColour(m_theme->modelPillBg);
                btn->SetForegroundColour(m_theme->textPrimary);
            }
        }

        m_modelList->SetBackgroundColour(m_theme->sidebarSelected);
        m_modelList->SetForegroundColour(m_theme->textPrimary);
    }
}

void ModelManagerDialog::RefreshModelList()
{
    m_modelList->DeleteAllItems();
    m_modelPaths.clear();

    auto models = ServerManager::ScanModels();

    long row = 0;
    for (const auto& path : models) {
        wxFileName fn(path);
        std::string displayName = fn.GetName().ToUTF8().data();

        wxULongLong fileSize = fn.GetSize();
        std::string sizeStr = (fileSize != wxInvalidSize)
                              ? FormatSize(fileSize) : "?";

        long idx = m_modelList->InsertItem(row, wxString::FromUTF8(displayName));
        m_modelList->SetItem(idx, 1, wxString::FromUTF8(sizeStr));

        m_modelPaths.push_back(path);
        row++;
    }

    m_statusText->SetLabel(wxString::Format("%ld model(s) found", row));
}

// ── Event handlers ───────────────────────────────────────────────

void ModelManagerDialog::OnDeleteClicked(wxCommandEvent&)
{
    long sel = m_modelList->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (sel < 0) {
        m_statusText->SetLabel("Select a model to delete");
        return;
    }

    if (sel >= (long)m_modelPaths.size()) return;

    std::string modelPath = m_modelPaths[sel];
    std::string displayName = m_modelList->GetItemText(sel).ToUTF8().data();

    if (wxMessageBox(
            wxString::Format("Delete \"%s\"?\n\nFile: %s\n\nThis cannot be undone.",
                             displayName, modelPath),
            "Confirm Delete", wxYES_NO | wxICON_WARNING, this) != wxYES)
        return;

    if (wxRemoveFile(wxString::FromUTF8(modelPath))) {
        m_statusText->SetLabel("Deleted: " + displayName);
        RefreshModelList();
    } else {
        m_statusText->SetLabel("Failed to delete: " + displayName);
    }
}

void ModelManagerDialog::OnRefreshClicked(wxCommandEvent&)
{
    RefreshModelList();
}

void ModelManagerDialog::OnOpenFolderClicked(wxCommandEvent&)
{
    ServerManager::EnsureDataDirs();
#ifdef __WXMSW__
    wxExecute("explorer \"" + wxString::FromUTF8(ServerManager::GetModelsDir()) + "\"",
              wxEXEC_ASYNC);
#endif
}

void ModelManagerDialog::OnClose(wxCommandEvent&)
{
    EndModal(wxID_CLOSE);
}
