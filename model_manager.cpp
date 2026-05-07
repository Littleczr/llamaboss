// model_manager.cpp
//
// Filesystem-based model manager for LlamaBoss.
// Lists .gguf files from %LOCALAPPDATA%\LlamaBoss\models\
// Supports delete (removes the file) and opening the folder.

#include "model_manager.h"
#include "server_manager.h"
#include "theme.h"
#include "widgets.h"   // ApplyDialogThemeRecursive, ApplyDarkTitleBar

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
    : wxDialog(parent, wxID_ANY, "Manage Models", wxDefaultPosition, wxSize(600, 440),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_theme(theme)
{
    CreateControls();
    RefreshModelList();

    if (m_theme)
        ApplyDarkTitleBar(this, m_theme->name != "light");

    Centre();
}

void ModelManagerDialog::CreateControls()
{
    auto* rootSizer = new wxBoxSizer(wxVERTICAL);

    // Body panel gives us consistent padding around everything
    auto* body = new wxPanel(this, wxID_ANY);
    auto* bodySizer = new wxBoxSizer(wxVERTICAL);

    // ── Header: muted folder path line ──────────────────────────
    auto* headerLabel = new wxStaticText(body, wxID_ANY,
        "Models folder: " + ServerManager::GetModelsDir());
    wxFont hf = headerLabel->GetFont();
    hf.SetPointSize(9);
    headerLabel->SetFont(hf);
    bodySizer->Add(headerLabel, 0, wxBOTTOM, 10);

    // ── Model list ──────────────────────────────────────────────
    m_modelList = new wxListCtrl(body, wxID_ANY, wxDefaultPosition, wxSize(-1, 240),
                                 wxLC_REPORT | wxLC_SINGLE_SEL | wxBORDER_NONE);
    m_modelList->AppendColumn("Model", wxLIST_FORMAT_LEFT,  360);
    m_modelList->AppendColumn("Size",  wxLIST_FORMAT_RIGHT, 100);
    bodySizer->Add(m_modelList, 1, wxEXPAND | wxBOTTOM, 10);

    // ── Action row: Delete (destructive), Refresh (neutral), Open (neutral) ──
    auto* actionSizer = new wxBoxSizer(wxHORIZONTAL);
    m_deleteButton  = new wxButton(body, ID_MM_DELETE, "Delete Selected",
        wxDefaultPosition, wxSize(-1, 30), wxBORDER_NONE);
    m_refreshButton = new wxButton(body, ID_MM_REFRESH, "Refresh",
        wxDefaultPosition, wxSize(-1, 30), wxBORDER_NONE);
    auto* openBtn   = new wxButton(body, ID_MM_OPENFOLDER, "Open Folder",
        wxDefaultPosition, wxSize(-1, 30), wxBORDER_NONE);

    wxFont btnFont = m_deleteButton->GetFont();
    btnFont.SetPointSize(9);
    m_deleteButton->SetFont(btnFont);
    m_refreshButton->SetFont(btnFont);
    openBtn->SetFont(btnFont);

    actionSizer->Add(m_deleteButton, 0, wxRIGHT, 6);
    actionSizer->Add(m_refreshButton, 0, wxRIGHT, 6);
    actionSizer->AddStretchSpacer();
    actionSizer->Add(openBtn, 0);
    bodySizer->Add(actionSizer, 0, wxEXPAND | wxBOTTOM, 10);

    // ── Status line ──────────────────────────────────────────────
    m_statusText = new wxStaticText(body, wxID_ANY, "");
    wxFont sf = m_statusText->GetFont();
    sf.SetPointSize(9);
    m_statusText->SetFont(sf);
    bodySizer->Add(m_statusText, 0, wxBOTTOM, 4);

    // ── Hint ─────────────────────────────────────────────────────
    auto* hintText = new wxStaticText(body, wxID_ANY,
        "To add models, download .gguf files and place them in the models folder.");
    wxFont hint = hintText->GetFont();
    hint.SetPointSize(9);
    hint.SetStyle(wxFONTSTYLE_ITALIC);
    hintText->SetFont(hint);
    bodySizer->Add(hintText, 0, wxBOTTOM, 4);

    body->SetSizer(bodySizer);
    rootSizer->Add(body, 1, wxEXPAND | wxALL, 18);

    // ── Footer: Close button ─────────────────────────────────────
    auto* footer = new wxPanel(this, wxID_ANY);
    auto* footSizer = new wxBoxSizer(wxHORIZONTAL);
    footSizer->AddStretchSpacer();
    auto* closeBtn = new wxButton(footer, wxID_CLOSE, "Close",
        wxDefaultPosition, wxSize(90, 32), wxBORDER_NONE);
    wxFont cbf = closeBtn->GetFont();
    cbf.SetPointSize(9);
    cbf.SetWeight(wxFONTWEIGHT_SEMIBOLD);
    closeBtn->SetFont(cbf);
    footSizer->Add(closeBtn, 0);
    footer->SetSizer(footSizer);
    rootSizer->Add(footer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 18);

    SetSizer(rootSizer);
    closeBtn->SetDefault();

    // ═════════════════════════════════════════════════════════════
    //  Apply theme
    // ═════════════════════════════════════════════════════════════
    if (m_theme) {
        const ThemeData& t = *m_theme;

        SetBackgroundColour(t.bgMain);
        body->SetBackgroundColour(t.bgMain);
        footer->SetBackgroundColour(t.bgMain);

        // Recursively paint labels + neutral buttons
        ApplyDialogThemeRecursive(this, t.textPrimary, t.bgInputField, t.textPrimary);

        // Header + hint + status = muted text
        headerLabel->SetForegroundColour(t.textMuted);
        m_statusText->SetForegroundColour(t.textMuted);
        hintText->SetForegroundColour(t.textMuted);

        // List control — input-field surface with primary text
        m_modelList->SetBackgroundColour(t.bgInputField);
        m_modelList->SetForegroundColour(t.textPrimary);

        // Delete button is destructive — stopButton red
        m_deleteButton->SetBackgroundColour(t.stopButton);
        m_deleteButton->SetForegroundColour(t.stopButtonText);

        // Close button is the primary footer action — accent blue
        closeBtn->SetBackgroundColour(t.accentButton);
        closeBtn->SetForegroundColour(t.accentButtonText);
    }
}

void ModelManagerDialog::RefreshModelList()
{
    m_modelList->DeleteAllItems();
    m_modelPaths.clear();

    auto models = ServerManager::ScanModelPaths();

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
