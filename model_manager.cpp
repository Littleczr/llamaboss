// model_manager.cpp
//
// Filesystem-based model manager for LlamaBoss.
// Lists .gguf files from %LOCALAPPDATA%\LlamaBoss\models\
// Supports delete (removes the file) and opening the folder.

#include "model_manager.h"
#include "server_manager.h"
#include "theme.h"
#include "widgets.h"   // ApplyDialogThemeRecursive, ApplyDarkTitleBar
#include "lb_project_ui_actions.h"
#include "path_safety.h"

#include <wx/filename.h>
#include <wx/msgdlg.h>

#include <sstream>
#include <iomanip>
#include <utility>

// ─────────────────────────────────────────────────────────────────
//  Button helpers (file-local)
// ─────────────────────────────────────────────────────────────────
//
// Same Telegram-style recipe used across the dialog family:
// wxButton + wxBORDER_NONE + semibold 10pt + theme palette painted
// in place. This dialog applies its theme at the end of
// CreateControls() so the helpers paint the colours directly.
//
//   MakeAccentButton — t.accentButton fill, white label
//   MakeFlatButton   — borderless, dialog-surface fill, muted text
//
// No destructive button helper is needed here — Delete lives on the
// list (Del key + right-click context menu), not in the action row.
//
namespace {

wxButton* MakeAccentButton(wxWindow* parent, wxWindowID id,
                           const wxString& label, const ThemeData& t,
                           int height = 32)
{
    auto* btn = new wxButton(parent, id, label,
                             wxDefaultPosition, wxSize(-1, height),
                             wxBORDER_NONE);
    wxFont bf = btn->GetFont();
    bf.SetPointSize(10);
    bf.SetWeight(wxFONTWEIGHT_SEMIBOLD);
    btn->SetFont(bf);
    btn->SetBackgroundColour(t.accentButton);
    btn->SetForegroundColour(t.accentButtonText);
    return btn;
}

wxButton* MakeFlatButton(wxWindow* parent, wxWindowID id,
                         const wxString& label, const ThemeData& t,
                         int height = 32)
{
    auto* btn = new wxButton(parent, id, label,
                             wxDefaultPosition, wxSize(-1, height),
                             wxBORDER_NONE);
    wxFont bf = btn->GetFont();
    bf.SetPointSize(10);
    bf.SetWeight(wxFONTWEIGHT_SEMIBOLD);
    btn->SetFont(bf);
    btn->SetBackgroundColour(t.bgDialogSurface);
    btn->SetForegroundColour(t.textMuted);
    return btn;
}

}  // namespace

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
    EVT_BUTTON(ID_MM_REFRESH,    ModelManagerDialog::OnRefreshClicked)
    EVT_BUTTON(ID_MM_OPENFOLDER, ModelManagerDialog::OnOpenFolderClicked)
    EVT_BUTTON(wxID_CLOSE,       ModelManagerDialog::OnClose)
wxEND_EVENT_TABLE()

ModelManagerDialog::ModelManagerDialog(wxWindow* parent,
                                       const ThemeData* theme,
                                       std::string loadedModelPath,
                                       std::string configuredModelPath)
    : wxDialog(parent, wxID_ANY, "Manage Models", wxDefaultPosition, wxSize(640, 480),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_theme(theme)
    , m_loadedModelPath(std::move(loadedModelPath))
    , m_configuredModelPath(std::move(configuredModelPath))
{
    // Bump dialog default font to match the rest of the dialog family.
    wxFont f = GetFont();
    f.SetPointSize(11);
    f.SetWeight(wxFONTWEIGHT_NORMAL);
    SetFont(f);

    SetEscapeId(wxID_CLOSE);

    CreateControls();
    RefreshModelList();

    if (m_theme)
        ApplyDarkTitleBar(this, m_theme->name != "light");

    Centre();
}

void ModelManagerDialog::CreateControls()
{
    // Fallback theme — used when m_theme is null. Keeps the helpers
    // honest in the degenerate case (no theme passed in).
    const ThemeData fallback = ThemeManager::GetDarkTheme();
    const ThemeData& t = m_theme ? *m_theme : fallback;

    auto* rootSizer = new wxBoxSizer(wxVERTICAL);

    // Body panel gives us consistent padding around everything
    auto* body = new wxPanel(this, wxID_ANY);
    body->SetBackgroundColour(t.bgDialogSurface);
    auto* bodySizer = new wxBoxSizer(wxVERTICAL);

    // ── Header: muted folder path line ──────────────────────────
    auto* headerLabel = new wxStaticText(body, wxID_ANY,
        "Models folder: " + wxString::FromUTF8(ServerManager::GetModelsDir()));
    wxFont hf = headerLabel->GetFont();
    hf.SetPointSize(10);
    headerLabel->SetFont(hf);
    headerLabel->SetForegroundColour(t.textMuted);
    bodySizer->Add(headerLabel, 0, wxBOTTOM, 12);

    // ── Model list ──────────────────────────────────────────────
    m_modelList = new wxListCtrl(body, wxID_ANY, wxDefaultPosition, wxSize(-1, 260),
                                 wxLC_REPORT | wxLC_SINGLE_SEL | wxBORDER_NONE);
    m_modelList->AppendColumn("Model", wxLIST_FORMAT_LEFT,  380);
    m_modelList->AppendColumn("Size",  wxLIST_FORMAT_RIGHT, 100);
    m_modelList->SetBackgroundColour(t.bgInputField);
    m_modelList->SetForegroundColour(t.textPrimary);
    bodySizer->Add(m_modelList, 1, wxEXPAND | wxBOTTOM, 12);

    // ── Action row: Refresh (accent left), Open folder (accent right) ──
    //  Delete is deliberately NOT in this row. It lives on the list
    //  itself — Del key while a row is selected, or right-click for a
    //  context menu. Same pattern as ProjectAttachDialog. The italic
    //  hint between the two action buttons tells the user where to
    //  find it. Keeps the destructive action accessible without
    //  letting a red button visually dominate the dialog.
    auto* actionSizer = new wxBoxSizer(wxHORIZONTAL);
    m_refreshButton = MakeAccentButton(body, ID_MM_REFRESH, "Refresh", t);
    auto* openBtn   = MakeAccentButton(body, ID_MM_OPENFOLDER, "Open folder", t);

    auto* deleteHint = new wxStaticText(body, wxID_ANY,
        "Press Del or right-click to delete");
    {
        wxFont hf = deleteHint->GetFont();
        hf.SetPointSize(10);
        hf.SetStyle(wxFONTSTYLE_ITALIC);
        deleteHint->SetFont(hf);
        deleteHint->SetForegroundColour(t.textMuted);
    }

    actionSizer->Add(m_refreshButton, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 14);
    actionSizer->Add(deleteHint,      0, wxALIGN_CENTER_VERTICAL);
    actionSizer->AddStretchSpacer();
    actionSizer->Add(openBtn,         0, wxALIGN_CENTER_VERTICAL);
    bodySizer->Add(actionSizer, 0, wxEXPAND | wxBOTTOM, 10);

    // ── Status line ──────────────────────────────────────────────
    m_statusText = new wxStaticText(body, wxID_ANY, "");
    wxFont sf = m_statusText->GetFont();
    sf.SetPointSize(10);
    m_statusText->SetFont(sf);
    m_statusText->SetForegroundColour(t.textMuted);
    bodySizer->Add(m_statusText, 0, wxBOTTOM, 4);

    // ── Hint ─────────────────────────────────────────────────────
    auto* hintText = new wxStaticText(body, wxID_ANY,
        "To add models, download .gguf files and place them in the models folder.");
    wxFont hint = hintText->GetFont();
    hint.SetPointSize(10);
    hint.SetStyle(wxFONTSTYLE_ITALIC);
    hintText->SetFont(hint);
    hintText->SetForegroundColour(t.textMuted);
    bodySizer->Add(hintText, 0, wxBOTTOM, 4);

    body->SetSizer(bodySizer);
    rootSizer->Add(body, 1, wxEXPAND | wxALL, 18);

    // ── Footer: flat Close button ────────────────────────────────
    auto* footer = new wxPanel(this, wxID_ANY);
    footer->SetBackgroundColour(t.bgDialogSurface);
    auto* footSizer = new wxBoxSizer(wxHORIZONTAL);
    footSizer->AddStretchSpacer();
    auto* closeBtn = MakeFlatButton(footer, wxID_CLOSE, "Close", t);
    footSizer->Add(closeBtn, 0, wxALIGN_CENTER_VERTICAL);
    footer->SetSizer(footSizer);
    rootSizer->Add(footer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 18);

    SetSizer(rootSizer);

    // ═════════════════════════════════════════════════════════════
    //  Final theming pass
    // ═════════════════════════════════════════════════════════════
    //
    // Buttons already carry their accent/destructive/flat palette from
    // the helpers above. We just need to set the dialog surface and let
    // the recursive helper colour static labels — but we DON'T let it
    // tint our buttons (it would overwrite the helper-applied colours),
    // so we re-paint them afterwards.
    if (m_theme) {
        SetBackgroundColour(t.bgDialogSurface);

        // ApplyDialogThemeRecursive will paint every wxButton it finds
        // with t.bgInputField — re-tint our two accent buttons and the
        // flat close button afterwards.
        ApplyDialogThemeRecursive(this, t.textPrimary, t.bgInputField, t.textPrimary);

        m_refreshButton->SetBackgroundColour(t.accentButton);
        m_refreshButton->SetForegroundColour(t.accentButtonText);
        openBtn->SetBackgroundColour(t.accentButton);
        openBtn->SetForegroundColour(t.accentButtonText);
        closeBtn->SetBackgroundColour(t.bgDialogSurface);
        closeBtn->SetForegroundColour(t.textMuted);

        // Status + hint + header should remain muted (the recursive
        // helper painted them with textPrimary).
        headerLabel->SetForegroundColour(t.textMuted);
        m_statusText->SetForegroundColour(t.textMuted);
        hintText->SetForegroundColour(t.textMuted);
        deleteHint->SetForegroundColour(t.textMuted);

        m_modelList->SetBackgroundColour(t.bgInputField);
        m_modelList->SetForegroundColour(t.textPrimary);

        Refresh();
    }

    // ── Del key + right-click → delete ──────────────────────────
    // Same idiom as ProjectAttachDialog. Dialog-level CHAR_HOOK catches
    // Del everywhere and routes to OnDeleteClicked when a row is
    // selected; other keys skip through so wxDialog defaults still work.
    // wxEVT_CONTEXT_MENU on the list pops a one-item menu using
    // ID_MM_DELETE, which we bind to OnDeleteClicked via wxEVT_MENU.
    Bind(wxEVT_CHAR_HOOK, &ModelManagerDialog::OnCharHook, this);
    m_modelList->Bind(wxEVT_CONTEXT_MENU,
                      &ModelManagerDialog::OnContextMenu, this);
    Bind(wxEVT_MENU,
         &ModelManagerDialog::OnDeleteClicked, this, ID_MM_DELETE);
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

std::string ModelManagerDialog::SelectedModelPath() const
{
    if (!m_modelList) return {};
    const long sel = m_modelList->GetNextItem(-1, wxLIST_NEXT_ALL,
                                              wxLIST_STATE_SELECTED);
    if (sel < 0 || sel >= static_cast<long>(m_modelPaths.size())) return {};
    return m_modelPaths[static_cast<size_t>(sel)];
}

bool ModelManagerDialog::IsLoadedModelPath(const std::string& path) const
{
    return !path.empty() &&
           !m_loadedModelPath.empty() &&
           path_safety::SameModelPath(path, m_loadedModelPath);
}

bool ModelManagerDialog::IsConfiguredModelPath(const std::string& path) const
{
    return !path.empty() &&
           !m_configuredModelPath.empty() &&
           path_safety::SameModelPath(path, m_configuredModelPath);
}

// ── Event handlers ───────────────────────────────────────────────

void ModelManagerDialog::OnContextMenu(wxContextMenuEvent& event)
{
    // Mouse right-click should target the row under the pointer, not
    // whatever row happened to be selected before the click.  Keyboard
    // context-menu events report (-1,-1), so those intentionally fall
    // back to the current selection.
    if (!m_modelList || m_modelPaths.empty()) return;

    wxPoint popupAt;
    const wxPoint screenPos = event.GetPosition();
    if (screenPos.x == -1 && screenPos.y == -1) {
        long sel = m_modelList->GetNextItem(-1, wxLIST_NEXT_ALL,
                                            wxLIST_STATE_SELECTED);
        if (sel < 0 || sel >= static_cast<long>(m_modelPaths.size())) return;
        if (!m_modelList->GetItemPosition(sel, popupAt))
            popupAt = wxPoint(8, 8);
    } else {
        const wxPoint pt = m_modelList->ScreenToClient(screenPos);
        int flags = 0;
        const long hit = m_modelList->HitTest(pt, flags);
        if (hit < 0 || hit >= static_cast<long>(m_modelPaths.size())) return;
        if ((flags & wxLIST_HITTEST_ONITEM) == 0) return;

        m_modelList->SetItemState(hit,
            wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
            wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
        popupAt = pt;
    }

    wxMenu menu;
    menu.Append(ID_MM_DELETE, "Delete model");
    m_modelList->PopupMenu(&menu, popupAt);
}

void ModelManagerDialog::OnCharHook(wxKeyEvent& event)
{
    // Del on a selected row triggers delete. Every other key skips
    // through so wxDialog's built-in routing (Esc, Tab, arrows) still
    // works as expected.
    if (event.GetKeyCode() == WXK_DELETE) {
        if (m_modelList && !m_modelPaths.empty()) {
            long sel = m_modelList->GetNextItem(-1, wxLIST_NEXT_ALL,
                                                wxLIST_STATE_SELECTED);
            if (sel >= 0) {
                wxCommandEvent dummy;
                OnDeleteClicked(dummy);
                return;   // handled
            }
        }
    }
    event.Skip();
}

void ModelManagerDialog::OnDeleteClicked(wxCommandEvent&)
{
    long sel = m_modelList->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (sel < 0) {
        m_statusText->SetLabel("Select a model to delete");
        return;
    }

    if (sel >= static_cast<long>(m_modelPaths.size())) return;

    const std::string modelPath = m_modelPaths[static_cast<size_t>(sel)];
    const wxString displayName = m_modelList->GetItemText(sel);
    const wxString modelPathWx = wxString::FromUTF8(modelPath);

    if (IsLoadedModelPath(modelPath)) {
        wxMessageBox(
            "This model is currently loaded by llama-server.\n\n"
            "Switch to another model before deleting it.",
            "Model In Use", wxOK | wxICON_INFORMATION, this);
        wxString status = "Cannot delete loaded model: ";
        status += displayName;
        m_statusText->SetLabel(status);
        return;
    }

    wxString confirm = "Delete \"";
    confirm += displayName;
    confirm += "\"?\n\nFile: ";
    confirm += modelPathWx;
    confirm += "\n\n";
    if (IsConfiguredModelPath(modelPath)) {
        confirm +=
            "This model is currently selected/configured for LlamaBoss. "
            "If you delete it, choose another model before sending your next message.\n\n";
    }
    confirm += "This cannot be undone.";

    if (wxMessageBox(confirm, "Confirm Delete",
                     wxYES_NO | wxICON_WARNING, this) != wxYES) {
        return;
    }

    if (wxRemoveFile(modelPathWx)) {
        wxString status = "Deleted: ";
        status += displayName;
        m_statusText->SetLabel(status);
        RefreshModelList();
    } else {
        wxString status = "Failed to delete: ";
        status += displayName;
        status += " - file may be in use by llama-server";
        m_statusText->SetLabel(status);
    }
}

void ModelManagerDialog::OnRefreshClicked(wxCommandEvent&)
{
    RefreshModelList();
}

void ModelManagerDialog::OnOpenFolderClicked(wxCommandEvent&)
{
    ServerManager::EnsureDataDirs();
    LbLaunchPathInOS(this, ServerManager::GetModelsDir(), "models folder");
}

void ModelManagerDialog::OnClose(wxCommandEvent&)
{
    EndModal(wxID_CLOSE);
}
