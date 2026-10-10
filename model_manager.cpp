// model_manager.cpp
//
// Filesystem-based model manager for LlamaBoss.
// Lists .gguf files from %LOCALAPPDATA%\LlamaBoss\models\
// Supports delete (removes the file), opening the folder, and
// downloading a model from a pasted link.

#include "model_manager.h"
#include "server_manager.h"
#include "theme.h"
#include "widgets.h"   // ApplyDialogThemeRecursive, ApplyDarkTitleBar
#include "lb_project_ui_actions.h"
#include "path_safety.h"
#include "model_downloader.h"   // DownloadThread, ProbeModelUrl, download events
#include "model_url.h"          // pasted-link parsing (pure, unit-tested)
#include "ui_event_post.h"

#include <wx/msgdlg.h>

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
    ID_MM_URL_BUTTON = wxID_HIGHEST + 206,
};

// Posted by the link pre-check worker.  Int = probe generation,
// payload = ModelUrlProbe.
wxDEFINE_EVENT(wxEVT_MM_PROBE_DONE, wxThreadEvent);

// Headroom kept free on the drive beyond the model itself.
static constexpr long long kDiskHeadroomBytes = 1LL << 30;   // 1 GB

wxBEGIN_EVENT_TABLE(ModelManagerDialog, wxDialog)
    EVT_BUTTON(ID_MM_REFRESH,    ModelManagerDialog::OnRefreshClicked)
    EVT_BUTTON(ID_MM_OPENFOLDER, ModelManagerDialog::OnOpenFolderClicked)
    EVT_BUTTON(wxID_CLOSE,       ModelManagerDialog::OnClose)
wxEND_EVENT_TABLE()

ModelManagerDialog::ModelManagerDialog(wxWindow* parent,
                                       const ThemeData* theme,
                                       std::string loadedModelPath,
                                       std::string configuredModelPath)
    : wxDialog(parent, wxID_ANY, "Manage Models", wxDefaultPosition, wxSize(660, 580),
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

    Bind(wxEVT_CLOSE_WINDOW, &ModelManagerDialog::OnCloseWindow, this);
    Bind(wxEVT_MM_PROBE_DONE, &ModelManagerDialog::OnProbeDone, this);
    Bind(wxEVT_DOWNLOAD_PROGRESS, &ModelManagerDialog::OnDownloadProgress, this);
    Bind(wxEVT_DOWNLOAD_COMPLETE, &ModelManagerDialog::OnDownloadComplete, this);
    Bind(wxEVT_DOWNLOAD_ERROR,    &ModelManagerDialog::OnDownloadError,    this);

    if (m_theme)
        ApplyDarkTitleBar(this, m_theme->name != "light");

    Centre();
}

ModelManagerDialog::~ModelManagerDialog()
{
    // Same order as ModelDownloaderDialog: mark dead first so no worker
    // can queue into a freed dialog, then stop any running download
    // (DownloadThread removes its own temp file when cancelled).
    LbMarkUiEventTargetDead(m_alive);
    if (m_cancelFlag) m_cancelFlag->store(true);
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

    // ── Add a model from a link ─────────────────────────────────
    // Replaces the old "download .gguf files and place them in the
    // models folder" hint: paste the link, the file lands in that folder.
    auto* urlLabel = new wxStaticText(body, wxID_ANY, "Add a model from a link");
    {
        wxFont lf = urlLabel->GetFont();
        lf.SetPointSize(10);
        lf.SetWeight(wxFONTWEIGHT_SEMIBOLD);
        urlLabel->SetFont(lf);
    }
    bodySizer->Add(urlLabel, 0, wxTOP | wxBOTTOM, 6);

    auto* urlRow = new wxBoxSizer(wxHORIZONTAL);
    m_urlField = new wxTextCtrl(body, wxID_ANY, wxEmptyString,
                                wxDefaultPosition, wxSize(-1, 30),
                                wxTE_PROCESS_ENTER | wxBORDER_NONE);
    m_urlField->SetHint("Paste a Hugging Face link to a .gguf file");
    m_urlField->SetBackgroundColour(t.bgInputField);
    m_urlField->SetForegroundColour(t.textPrimary);
    urlRow->Add(m_urlField, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
    m_urlButton = MakeAccentButton(body, ID_MM_URL_BUTTON, "Download", t);
    urlRow->Add(m_urlButton, 0, wxALIGN_CENTER_VERTICAL);
    bodySizer->Add(urlRow, 0, wxEXPAND | wxBOTTOM, 6);

    m_urlGauge = new wxGauge(body, wxID_ANY, 100, wxDefaultPosition,
                             wxSize(-1, 6), wxGA_HORIZONTAL | wxGA_SMOOTH);
    m_urlGauge->Hide();
    bodySizer->Add(m_urlGauge, 0, wxEXPAND | wxBOTTOM, 4);

    m_urlStatus = new wxStaticText(body, wxID_ANY,
        "The file is saved to the models folder above.");
    {
        wxFont uf = m_urlStatus->GetFont();
        uf.SetPointSize(10);
        m_urlStatus->SetFont(uf);
    }
    m_urlStatus->SetForegroundColour(t.textMuted);
    m_urlStatus->SetMinSize(wxSize(40, -1));
    bodySizer->Add(m_urlStatus, 0, wxEXPAND | wxBOTTOM, 4);

    m_urlButton->Bind(wxEVT_BUTTON, &ModelManagerDialog::OnUrlButton, this);
    m_urlField->Bind(wxEVT_TEXT_ENTER, &ModelManagerDialog::OnUrlButton, this);

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
        m_urlButton->SetBackgroundColour(t.accentButton);
        m_urlButton->SetForegroundColour(t.accentButtonText);

        // Status + header + hints should remain muted (the recursive
        // helper painted them with textPrimary).
        headerLabel->SetForegroundColour(t.textMuted);
        m_statusText->SetForegroundColour(t.textMuted);
        deleteHint->SetForegroundColour(t.textMuted);
        m_urlStatus->SetForegroundColour(t.textMuted);
        m_urlField->SetBackgroundColour(t.bgInputField);
        m_urlField->SetForegroundColour(t.textPrimary);

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
    m_modelList->Bind(wxEVT_SIZE,   &ModelManagerDialog::OnListSize,   this);
    m_modelList->Bind(wxEVT_MOTION, &ModelManagerDialog::OnListMotion, this);
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
    m_tipRow = -2;
    m_modelList->UnsetToolTip();
    FitNameColumn();
}

// ── Name column: fill the width, full name on hover ──────────────
// A fixed-width Model column cuts long names ("...Aggressive-Q4_...")
// while an empty third column sits to the right.

void ModelManagerDialog::FitNameColumn()
{
    if (!m_modelList || m_modelList->GetColumnCount() < 2) return;
    const int sizeCol = m_modelList->GetColumnWidth(1);
    // Leave room for a vertical scrollbar so no horizontal one appears.
    const int avail = m_modelList->GetClientSize().GetWidth() - sizeCol
                    - wxSystemSettings::GetMetric(wxSYS_VSCROLL_X, m_modelList);
    m_modelList->SetColumnWidth(0, std::max(FromDIP(160), avail));
}

void ModelManagerDialog::OnListSize(wxSizeEvent& ev)
{
    ev.Skip();
    CallAfter([this] { FitNameColumn(); });
}

void ModelManagerDialog::OnListMotion(wxMouseEvent& ev)
{
    ev.Skip();
    int flags = 0;
    const long hit = m_modelList->HitTest(ev.GetPosition(), flags);
    const long row = (hit >= 0 && (flags & wxLIST_HITTEST_ONITEM)) ? hit : -1;
    if (row == m_tipRow) return;
    m_tipRow = row;
    if (row >= 0 && row < static_cast<long>(m_modelPaths.size()))
        m_modelList->SetToolTip(wxFileName(wxString::FromUTF8(m_modelPaths[row])).GetFullName());
    else
        m_modelList->UnsetToolTip();
}

void ModelManagerDialog::SelectModelPath(const std::string& path)
{
    for (size_t i = 0; i < m_modelPaths.size(); ++i) {
        if (path_safety::SameModelPath(m_modelPaths[i], path)) {
            const long row = static_cast<long>(i);
            m_modelList->SetItemState(row,
                wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
            m_modelList->EnsureVisible(row);
            return;
        }
    }
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
    // Del on a selected row (list focused) triggers delete. Every other key skips
    // through so wxDialog's built-in routing (Esc, Tab, arrows) still
    // works as expected.
    // Only when the model list itself has focus: Delete in the URL field
    // (or any other control) must edit text, not delete a model.
    if (event.GetKeyCode() == WXK_DELETE) {
        if (m_modelList && !m_modelPaths.empty() &&
            wxWindow::FindFocus() == m_modelList) {
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
    if (!ConfirmLeaveDuringDownload()) return;
    EndModal(wxID_CLOSE);
}

void ModelManagerDialog::OnCloseWindow(wxCloseEvent& ev)
{
    if (ev.CanVeto() && !ConfirmLeaveDuringDownload()) {
        ev.Veto();
        return;
    }
    ev.Skip();
}

bool ModelManagerDialog::ConfirmLeaveDuringDownload()
{
    if (m_urlState != UrlState::Downloading) return true;
    const int answer = wxMessageBox(
        "A model is still downloading. Closing this window cancels the "
        "download.\n\nCancel the download and close?",
        "Download in Progress", wxYES_NO | wxICON_WARNING, this);
    if (answer != wxYES) return false;
    CancelUrlDownload();
    return true;
}

// ═══════════════════════════════════════════════════════════════════
//  Download from a pasted link
// ═══════════════════════════════════════════════════════════════════
//
//  Idle --Download--> Checking (ProbeModelUrl on a worker: size, GGUF
//  magic, gated/404 errors) --confirm size + free space--> Downloading
//  (DownloadThread) --> Idle.  Each stage is generation-stamped so a
//  late event from a cancelled stage is ignored.
//
//  Where the file lands:
//    casual mode, model weights  -> models\<stem>\<file>.gguf (a bundle,
//                                   same layout as the catalog downloader)
//    casual mode, mmproj / draft -> models\<file>.gguf (see
//                                   model_url::IsCompanionFilename)
//    power mode (custom folder)  -> <folder>\<file>.gguf (flat)
//  Before downloading, the root AND every bundle folder are checked for
//  the same file name, so a model already filed in a folder is selected
//  instead of being downloaded a second time.

namespace {

// Existing copy of `filename` in the models root or (when
// `searchBundles`) any first-level bundle folder.  Empty when none.
// File-name matching is the filesystem's: case-insensitive on Windows.
std::string FindExistingModelFile(const wxString& root,
                                  const wxString& filename,
                                  bool searchBundles)
{
    wxFileName loose(root, filename);
    if (loose.FileExists())
        return std::string(loose.GetFullPath().utf8_str());
    if (!searchBundles || !wxDir::Exists(root)) return "";

    wxDir dir(root);
    if (!dir.IsOpened()) return "";
    wxString sub;
    bool more = dir.GetFirst(&sub, wxEmptyString, wxDIR_DIRS);
    while (more) {
        wxFileName inBundle(root + wxFILE_SEP_PATH + sub, filename);
        if (inBundle.FileExists())
            return std::string(inBundle.GetFullPath().utf8_str());
        more = dir.GetNext(&sub);
    }
    return "";
}

// True when `folder` holds a model weight file (.gguf that is not an
// mmproj / draft companion).  Adding a second weight would make the
// bundle ambiguous and ScanBundle would hide BOTH models.
bool FolderHasModelWeights(const wxString& folder)
{
    wxDir dir(folder);
    if (!dir.IsOpened()) return false;
    wxString name;
    bool more = dir.GetFirst(&name, "*.gguf", wxDIR_FILES);
    while (more) {
        if (!model_url::IsCompanionFilename(std::string(name.utf8_str())))
            return true;
        more = dir.GetNext(&name);
    }
    return false;
}

// Destination for a link download (see the table above).  In casual
// mode picks the first of "<stem>", "<stem> (2)", ... that is free or
// holds no model weights yet (an empty folder left by a cancelled
// download is reused).  Does not create anything.
std::string ChooseLinkDestPath(const wxString& root, const std::string& filename)
{
    const wxString file = wxString::FromUTF8(filename);
    if (!ServerManager::IsCasualMode() || model_url::IsCompanionFilename(filename))
        return std::string(wxFileName(root, file).GetFullPath().utf8_str());

    for (int attempt = 1; attempt <= 99; ++attempt) {
        const wxString folder = root + wxFILE_SEP_PATH +
            wxString::FromUTF8(model_url::BundleFolderName(filename, attempt));
        if (wxFileName::FileExists(folder)) continue;          // a file, not a folder
        if (wxDir::Exists(folder) && FolderHasModelWeights(folder)) continue;
        return std::string(wxFileName(folder, file).GetFullPath().utf8_str());
    }
    // 99 clashing folders: give up on bundling rather than fail.
    return std::string(wxFileName(root, file).GetFullPath().utf8_str());
}

// Folder name to show the user for a path inside the models root:
// "" for a loose file, "<bundle>" for models\<bundle>\file.gguf.
wxString BundleLabelFor(const wxString& root, const std::string& path)
{
    wxFileName fn(wxString::FromUTF8(path));
    wxFileName rootFn = wxFileName::DirName(root);
    wxFileName parent = wxFileName::DirName(fn.GetPath());
    if (parent.SameAs(rootFn)) return "";
    return parent.GetDirs().empty() ? wxString() : parent.GetDirs().Last();
}

// Remove `folder` only if it is empty (an aborted download's bundle).
void RemoveFolderIfEmpty(const wxString& folder)
{
    if (folder.empty() || !wxDir::Exists(folder)) return;
    {
        wxDir dir(folder);
        if (!dir.IsOpened() || dir.HasFiles() || dir.HasSubDirs()) return;
    }
    wxLogNull quiet;   // a failed rmdir is harmless; don't pop a log dialog
    wxFileName::Rmdir(folder);
}

} // namespace

void ModelManagerDialog::SetUrlStatus(const wxString& text, bool isError)
{
    const ThemeData fallback = ThemeManager::GetDarkTheme();
    const ThemeData& t = m_theme ? *m_theme : fallback;
    m_urlStatus->SetForegroundColour(isError ? t.stopButton : t.textMuted);
    m_urlStatus->SetLabel(text);
    // Wrap to the panel width so long errors (gated model, disk space)
    // stay readable; the model list above is the elastic element.
    wxWindow* panel = m_urlStatus->GetParent();
    m_urlStatus->Wrap(std::max(FromDIP(200), panel->GetClientSize().GetWidth()));
    panel->Layout();
    m_urlStatus->Refresh();
}

void ModelManagerDialog::SetUrlState(UrlState state)
{
    const ThemeData fallback = ThemeManager::GetDarkTheme();
    const ThemeData& t = m_theme ? *m_theme : fallback;
    m_urlState = state;

    m_urlField->Enable(state == UrlState::Idle);
    m_urlButton->Enable(state != UrlState::Checking);
    if (state == UrlState::Downloading) {
        m_urlButton->SetLabel("Cancel");
        m_urlButton->SetBackgroundColour(t.stopButton);
        m_urlButton->SetForegroundColour(t.stopButtonText);
    } else {
        m_urlButton->SetLabel(state == UrlState::Checking ? "Checking..." : "Download");
        m_urlButton->SetBackgroundColour(t.accentButton);
        m_urlButton->SetForegroundColour(t.accentButtonText);
    }
    m_urlButton->Refresh();

    m_urlGauge->Show(state == UrlState::Downloading);
    if (state != UrlState::Downloading) m_urlGauge->SetValue(0);
    m_urlGauge->GetParent()->Layout();
}

void ModelManagerDialog::OnUrlButton(wxCommandEvent&)
{
    if (m_urlState == UrlState::Downloading) {
        CancelUrlDownload();
        SetUrlStatus("Download cancelled.");
        return;
    }
    if (m_urlState == UrlState::Checking) return;

    const auto parsed = model_url::Parse(
        std::string(m_urlField->GetValue().utf8_str()));
    if (!parsed.ok) {
        SetUrlStatus(wxString::FromUTF8(parsed.error), true);
        m_urlField->SetFocus();
        return;
    }

    ServerManager::EnsureDataDirs();
    const wxString root = wxString::FromUTF8(ServerManager::GetModelsDir());

    // Already downloaded?  Look in the root and, in casual mode, every
    // bundle folder — the copy may have been filed into a folder.
    // Companions (mmproj / draft) only check the root: their names are
    // generic ("mmproj-F16.gguf"), so one found inside some other
    // model's bundle is very likely a different file.
    const bool searchBundles = ServerManager::IsCasualMode() &&
                               !model_url::IsCompanionFilename(parsed.filename);
    const std::string existing = FindExistingModelFile(
        root, wxString::FromUTF8(parsed.filename), searchBundles);
    if (!existing.empty()) {
        SelectModelPath(existing);
        const wxString where = BundleLabelFor(root, existing);
        SetUrlStatus(wxString::FromUTF8(parsed.filename) +
                     " is already in your models folder" +
                     (where.empty() ? wxString(".") : " (in " + where + ")."));
        return;
    }

    m_pendingFilename = parsed.filename;
    m_pendingDestPath = ChooseLinkDestPath(root, parsed.filename);
    m_pendingCreatedDir.clear();
    const unsigned gen = ++m_probeGeneration;
    SetUrlState(UrlState::Checking);
    SetUrlStatus("Checking " + wxString::FromUTF8(parsed.filename) + "...");

    wxEvtHandler* target = this;
    std::weak_ptr<std::atomic<bool>> alive = m_alive;
    const std::string url = parsed.url;
    std::thread([target, alive, url, gen]() {
        ModelUrlProbe result = ProbeModelUrl(url);
        auto* ev = new wxThreadEvent(wxEVT_MM_PROBE_DONE);
        ev->SetInt(static_cast<int>(gen));
        ev->SetString(wxString::FromUTF8(url));
        ev->SetPayload(result);
        LbQueueEventIfAlive(target, alive, ev);
    }).detach();
}

void ModelManagerDialog::OnProbeDone(wxThreadEvent& ev)
{
    if (static_cast<unsigned>(ev.GetInt()) != m_probeGeneration ||
        m_urlState != UrlState::Checking)
        return;

    const ModelUrlProbe probe = ev.GetPayload<ModelUrlProbe>();
    const std::string url = std::string(ev.GetString().utf8_str());
    const wxString name = wxString::FromUTF8(m_pendingFilename);

    if (!probe.ok) {
        SetUrlState(UrlState::Idle);
        SetUrlStatus(wxString::FromUTF8(probe.error), true);
        return;
    }

    // Size + free-space confirmation before anything large moves.
    wxString sizeText = probe.sizeBytes > 0
        ? wxString::FromUTF8(FormatSize(wxULongLong(static_cast<wxULongLong_t>(probe.sizeBytes))))
        : wxString("unknown size");
    wxString confirm = "Download " + name + " (" + sizeText + ")?\n\n";

    wxDiskspaceSize_t freeBytes = 0;
    const bool haveFree = wxGetDiskSpace(
        wxString::FromUTF8(ServerManager::GetModelsDir()), nullptr, &freeBytes);
    if (haveFree) {
        const long long freeLL = static_cast<long long>(freeBytes.GetValue());
        if (probe.sizeBytes > 0 && freeLL < probe.sizeBytes + kDiskHeadroomBytes) {
            SetUrlState(UrlState::Idle);
            SetUrlStatus("Not enough disk space: " + name + " needs " + sizeText +
                         " and the drive has " +
                         wxString::FromUTF8(FormatSize(wxULongLong(static_cast<wxULongLong_t>(freeLL)))) +
                         " free.", true);
            return;
        }
        confirm += "Free space on the drive: " +
                   wxString::FromUTF8(FormatSize(wxULongLong(static_cast<wxULongLong_t>(freeLL)))) + "\n";
    }
    confirm += "Saved to: " + wxFileName(wxString::FromUTF8(m_pendingDestPath)).GetPath();

    if (wxMessageBox(confirm, "Download Model", wxYES_NO | wxICON_QUESTION, this) != wxYES) {
        SetUrlState(UrlState::Idle);
        SetUrlStatus("Download not started.");
        return;
    }

    // Create the bundle folder now (not before the confirm), so saying
    // No leaves nothing behind.  Remember it if WE made it, so a failed
    // download can tidy up the empty folder.
    {
        const wxString destDir = wxFileName(wxString::FromUTF8(m_pendingDestPath)).GetPath();
        if (!wxDir::Exists(destDir)) {
            if (!wxFileName::Mkdir(destDir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) {
                SetUrlState(UrlState::Idle);
                SetUrlStatus("Could not create the folder " + destDir, true);
                return;
            }
            m_pendingCreatedDir = std::string(destDir.utf8_str());
        }
    }

    m_cancelFlag = std::make_shared<std::atomic<bool>>(false);
    ++m_downloadGeneration;
    SetUrlState(UrlState::Downloading);
    SetUrlStatus("Downloading " + name + "...");

    auto* thread = new DownloadThread(
        this, url, m_pendingDestPath, probe.sizeBytes > 0 ? probe.sizeBytes : 0,
        m_cancelFlag, std::weak_ptr<std::atomic<bool>>(m_alive),
        m_downloadGeneration,
        /*expectedBytesExact=*/probe.sizeBytes > 0);   // server-reported, not a catalog estimate
    if (thread->Run() != wxTHREAD_NO_ERROR) {
        delete thread;
        m_cancelFlag.reset();
        SetUrlState(UrlState::Idle);
        RemoveFolderIfEmpty(wxString::FromUTF8(m_pendingCreatedDir));
        m_pendingCreatedDir.clear();
        SetUrlStatus("Could not start the download.", true);
    }
}

void ModelManagerDialog::CancelUrlDownload()
{
    if (m_cancelFlag) m_cancelFlag->store(true);
    m_cancelFlag.reset();
    ++m_downloadGeneration;   // ignore anything the old worker still posts
    SetUrlState(UrlState::Idle);
}

void ModelManagerDialog::OnDownloadProgress(wxCommandEvent& ev)
{
    if (ev.GetInt() != static_cast<int>(m_downloadGeneration) ||
        m_urlState != UrlState::Downloading)
        return;

    const int pct = static_cast<int>(ev.GetExtraLong());
    m_urlGauge->SetValue(std::max(0, std::min(100, pct)));

    long long received = 0, total = 0;
    const std::string parts = std::string(ev.GetString().utf8_str());
    const size_t bar = parts.find('|');
    if (bar != std::string::npos) {
        try {
            received = std::stoll(parts.substr(0, bar));
            total    = std::stoll(parts.substr(bar + 1));
        } catch (...) {}
    }
    wxString text = "Downloading " + wxString::FromUTF8(m_pendingFilename) + " - " +
        wxString::FromUTF8(FormatSize(wxULongLong(static_cast<wxULongLong_t>(received))));
    if (total > 0)
        text += " of " + wxString::FromUTF8(FormatSize(wxULongLong(static_cast<wxULongLong_t>(total)))) +
                wxString::Format(" (%d%%)", pct);
    SetUrlStatus(text);
}

void ModelManagerDialog::OnDownloadComplete(wxCommandEvent& ev)
{
    if (ev.GetInt() != static_cast<int>(m_downloadGeneration) ||
        m_urlState != UrlState::Downloading)
        return;

    m_cancelFlag.reset();
    m_pendingCreatedDir.clear();   // it holds the model now
    SetUrlState(UrlState::Idle);
    m_urlField->Clear();
    RefreshModelList();
    SelectModelPath(m_pendingDestPath);
    SetUrlStatus("Downloaded " + wxString::FromUTF8(m_pendingFilename) +
                 ". It's now in your model picker.");
}

void ModelManagerDialog::OnDownloadError(wxCommandEvent& ev)
{
    if (ev.GetInt() != static_cast<int>(m_downloadGeneration) ||
        m_urlState != UrlState::Downloading)
        return;

    m_cancelFlag.reset();
    SetUrlState(UrlState::Idle);
    // The worker removes its temp file before posting the error, so a
    // folder we created for this download is empty again: drop it.
    RemoveFolderIfEmpty(wxString::FromUTF8(m_pendingCreatedDir));
    m_pendingCreatedDir.clear();
    // Download errors can be multi-line; the status line is one line.
    wxString msg = ev.GetString();
    msg.Replace("\n\n", " ");
    msg.Replace("\n", " ");
    SetUrlStatus(msg, true);
}
