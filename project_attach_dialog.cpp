// project_attach_dialog.cpp

#include "project_attach_dialog.h"

#include "widgets.h"   // ApplyDarkTitleBar

#include <wx/listbox.h>
#include <wx/menu.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/utils.h>    // wxLaunchDefaultApplication
#include <wx/window.h>

#include <algorithm>
#include <utility>

// Local menu/button IDs for the dialog.
enum {
    ID_DIALOG_NEW_PROJECT    = wxID_HIGHEST + 4100,
    ID_CTX_DELETE_PROJECT    = wxID_HIGHEST + 4101,
    ID_CTX_RENAME_PROJECT    = wxID_HIGHEST + 4102,
    ID_CTX_OPEN_FOLDER       = wxID_HIGHEST + 4103,
    ID_DIALOG_DETACH_PROJECT = wxID_HIGHEST + 4104
};

// ─────────────────────────────────────────────────────────────────
//  Button helpers (file-local)
// ─────────────────────────────────────────────────────────────────
//
// Same recipe as settings.cpp: plain wxButton + wxBORDER_NONE so Win11
// doesn't paint native chrome over the solid fill, semibold 10pt font
// for the slightly heavier Telegram-y read, theme palette applied in
// place (this dialog applies its theme inline at build time rather
// than via a separate ApplyTheme pass, so there's no need for the
// vector-tracking pattern used in SettingsDialog).
//
//   MakeAccentButton — t.accentButton fill, white label
//   MakeFlatButton   — borderless, dialog-surface fill, muted text
//
// Destructive actions stay off the footer: Delete lives on the list
// (Del key + right-click context menu).  Detach is footer-worthy
// because it acts on the *chat*, not on a project, and is the only
// way to express "no project" once one is attached.
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

// ═══════════════════════════════════════════════════════════════════
//  ProjectAttachDialog
// ═══════════════════════════════════════════════════════════════════

ProjectAttachDialog::ProjectAttachDialog(
    wxWindow* parent,
    const ThemeData& theme,
    std::vector<ProjectInfo> projects,
    CreateProjectCallback onCreateProject,
    DeleteProjectCallback onDeleteProject,
    ProjectInfo attachedProject,
    RenameProjectCallback onRenameProject)
    : wxDialog(parent, wxID_ANY, "Attach / Manage Project",
               wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_theme(theme)
    , m_projects(std::move(projects))
    , m_onCreateProject(std::move(onCreateProject))
    , m_onDeleteProject(std::move(onDeleteProject))
    , m_onRenameProject(std::move(onRenameProject))
    , m_attachedProject(std::move(attachedProject))
{
    BuildUi();
}

bool ProjectAttachDialog::GetSelectedProject(ProjectInfo& outProject) const
{
    const int sel = SelectedIndex();
    if (sel < 0) return false;

    outProject = m_projects[static_cast<size_t>(sel)];
    return true;
}

bool ProjectAttachDialog::HasAttachedProject() const
{
    return !m_attachedProject.id.empty() ||
           !m_attachedProject.rootPath.empty();
}

bool ProjectAttachDialog::IsAttachedProject(const ProjectInfo& p) const
{
    if (!HasAttachedProject()) return false;
    if (!m_attachedProject.id.empty() && p.id == m_attachedProject.id)
        return true;
    return !m_attachedProject.rootPath.empty() &&
           p.rootPath == m_attachedProject.rootPath;
}

int ProjectAttachDialog::SelectedIndex() const
{
    if (!m_projectList) return -1;
    const int sel = m_projectList->GetSelection();
    if (sel == wxNOT_FOUND || static_cast<size_t>(sel) >= m_projects.size())
        return -1;
    return sel;
}

void ProjectAttachDialog::BuildUi()
{
    SetBackgroundColour(m_theme.bgDialogSurface);

    // Bump the dialog default font to match the settings dialog ladder
    // (11pt body, 12pt headers, 10pt buttons). Picker dialogs lean on
    // the same hierarchy.
    wxFont base = GetFont();
    base.SetPointSize(11);
    SetFont(base);

    auto* top = new wxBoxSizer(wxVERTICAL);

    auto* label = new wxStaticText(
        this, wxID_ANY, "Select a project for the current chat, or create a new one:");
    label->SetForegroundColour(m_theme.textPrimary);
    top->Add(label, 0, wxLEFT | wxRIGHT | wxTOP, 12);

    // One quiet line of "what does attaching do" so the dialog
    // explains itself the first time someone opens it.
    auto* subtitle = new wxStaticText(
        this, wxID_ANY,
        "An attached project shares its Workflows, Documents, and "
        "context with this chat.");
    subtitle->SetForegroundColour(m_theme.textMuted);
    {
        wxFont sf = subtitle->GetFont();
        sf.SetPointSize(10);
        subtitle->SetFont(sf);
    }
    top->Add(subtitle, 0, wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, 12);

    m_projectList = new wxListBox(
        this, wxID_ANY, wxDefaultPosition, wxSize(320, 140));
    m_projectList->SetBackgroundColour(m_theme.bgInputField);
    m_projectList->SetForegroundColour(m_theme.textPrimary);
    top->Add(m_projectList, 1, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 12);

    // Empty-state explainer.  Swapped in for the list when there are
    // no projects yet, so a first-time user gets onboarding copy
    // instead of a bare rectangle.  ReloadProjectList() toggles the
    // pair's visibility.
    m_emptyLabel = new wxStaticText(
        this, wxID_ANY,
        "No projects yet.\n\n"
        "A project is a folder of Workflows, Documents, and context "
        "that any chat can attach to and build on over time.\n\n"
        "Click New... to create your first one.");
    m_emptyLabel->SetForegroundColour(m_theme.textMuted);
    m_emptyLabel->Wrap(420);
    top->Add(m_emptyLabel, 1, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 12);

    // Subtle separator above the button row. Kept as a 1px hairline
    // (not the 10px section band used in Settings) — this dialog is
    // small enough that a band would dominate the layout.
    auto* line = new wxPanel(this, wxID_ANY,
                             wxDefaultPosition, wxSize(-1, 1));
    line->SetBackgroundColour(m_theme.borderSubtle);
    top->Add(line, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 12);

    // Button row:
    //   [Detach]                [New...]  [Select]  [Cancel]
    //
    // Detach appears only when the chat already has an attached
    // project; it ends the dialog with kResultDetach and the caller
    // performs the actual detach.  "New..." opens the existing
    // project-name dialog, so it keeps the ellipsis and uses the
    // same solid accent treatment as Select.  Delete stays off the
    // footer; it lives on the list itself via Del key and the
    // right-click context menu.
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);

    if (HasAttachedProject()) {
        m_detachButton = MakeFlatButton(
            this, ID_DIALOG_DETACH_PROJECT, "Detach", m_theme);
        m_detachButton->SetMinSize(wxSize(96, 32));
        buttons->Add(m_detachButton, 0, wxALIGN_CENTER_VERTICAL);
    }
    buttons->AddStretchSpacer(1);

    m_newButton = MakeAccentButton(
        this, ID_DIALOG_NEW_PROJECT, "New...", m_theme);
    m_newButton->SetMinSize(wxSize(96, 32));

    m_okButton = MakeAccentButton(
        this, wxID_OK, "Select", m_theme);
    m_okButton->SetMinSize(wxSize(96, 32));

    auto* cancelButton = MakeFlatButton(
        this, wxID_CANCEL, "Cancel", m_theme);

    buttons->Add(m_newButton, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
    buttons->Add(m_okButton,  0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
    buttons->Add(cancelButton, 0, wxALIGN_CENTER_VERTICAL);
    top->Add(buttons, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 12);

    SetSizerAndFit(top);
    SetMinSize(wxSize(460, 320));
    CentreOnParent();
    ApplyDarkTitleBar(this, m_theme.name != "light");
    m_okButton->SetDefault();

    // Hook Del / F2 globally for the dialog. Skipping on every other
    // key lets wxDialog's built-in affirmative/escape mechanism still
    // route Enter→Select and Esc→Cancel without us intercepting them.
    Bind(wxEVT_CHAR_HOOK, &ProjectAttachDialog::OnCharHook, this);

    // Right-click on the list pops the context menu (after first
    // moving the selection to the row under the cursor — see
    // OnContextMenu).  New row actions slot in there.
    m_projectList->Bind(wxEVT_CONTEXT_MENU,
                        &ProjectAttachDialog::OnContextMenu, this);
    Bind(wxEVT_MENU,
         &ProjectAttachDialog::OnDeleteProject, this,
         ID_CTX_DELETE_PROJECT);
    Bind(wxEVT_MENU,
         &ProjectAttachDialog::OnRenameProject, this,
         ID_CTX_RENAME_PROJECT);
    Bind(wxEVT_MENU,
         &ProjectAttachDialog::OnOpenProjectFolder, this,
         ID_CTX_OPEN_FOLDER);

    Bind(wxEVT_BUTTON,
         &ProjectAttachDialog::OnNewProject, this,
         ID_DIALOG_NEW_PROJECT);
    if (m_detachButton) {
        Bind(wxEVT_BUTTON,
             &ProjectAttachDialog::OnDetachProject, this,
             ID_DIALOG_DETACH_PROJECT);
    }

    m_projectList->Bind(wxEVT_LISTBOX,
                        &ProjectAttachDialog::OnListSelection, this);
    m_projectList->Bind(wxEVT_LISTBOX_DCLICK,
                        &ProjectAttachDialog::OnListDoubleClick, this);
    m_projectList->Bind(wxEVT_MOTION,
                        &ProjectAttachDialog::OnListMotion, this);

    ReloadProjectList();

    // Pre-select the attached project so the dialog opens showing
    // the current state rather than an arbitrary first row.
    if (HasAttachedProject())
        SelectProject(m_attachedProject);

    UpdateButtons();
}

void ProjectAttachDialog::ReloadProjectList()
{
    if (!m_projectList) return;

    m_projectList->Clear();
    for (const auto& project : m_projects) {
        wxString row = wxString::FromUTF8(project.name);
        if (IsAttachedProject(project)) {
            // Same "\xE2\x97\x8F" dot vocabulary as the model pill.
            row = wxString::FromUTF8("\xE2\x97\x8F ") + row +
                  "   (attached)";
        }
        m_projectList->Append(row);
    }

    if (!m_projects.empty())
        m_projectList->SetSelection(0);

    // Swap list ↔ empty-state explainer.  Both live in the sizer at
    // proportion 1, so whichever is shown takes the same space.
    const bool empty = m_projects.empty();
    m_projectList->Show(!empty);
    if (m_emptyLabel) m_emptyLabel->Show(empty);
    Layout();

    // Row text changed under the cursor; let the next motion event
    // rebuild the tooltip instead of trusting a stale row index.
    m_tooltipItem = -1;
    m_projectList->UnsetToolTip();
}

void ProjectAttachDialog::SelectProject(const ProjectInfo& project)
{
    if (!m_projectList) return;

    for (size_t i = 0; i < m_projects.size(); ++i) {
        const ProjectInfo& candidate = m_projects[i];
        if ((!project.id.empty() && candidate.id == project.id) ||
            (!project.rootPath.empty() && candidate.rootPath == project.rootPath))
        {
            m_projectList->SetSelection(static_cast<int>(i));
            return;
        }
    }
}

void ProjectAttachDialog::UpdateButtons()
{
    const bool hasSelection = SelectedIndex() >= 0;

    if (m_okButton)
        m_okButton->Enable(hasSelection);

    if (m_newButton)
        m_newButton->Enable(static_cast<bool>(m_onCreateProject));

    // Detach acts on the chat's current attachment, not on the list
    // selection, so it stays enabled regardless of selection state.
}

void ProjectAttachDialog::OnListSelection(wxCommandEvent&)
{
    UpdateButtons();
}

void ProjectAttachDialog::OnListDoubleClick(wxCommandEvent&)
{
    if (m_okButton && m_okButton->IsEnabled())
        EndModal(wxID_OK);
}

void ProjectAttachDialog::OnListMotion(wxMouseEvent& event)
{
    // Per-row tooltip: show the project's folder path under the
    // cursor.  Projects are folder-backed, and the path answers
    // "where does this actually live?" without a click.
    if (m_projectList) {
        const int item = m_projectList->HitTest(event.GetPosition());
        if (item != m_tooltipItem) {
            m_tooltipItem = item;
            if (item != wxNOT_FOUND &&
                static_cast<size_t>(item) < m_projects.size() &&
                !m_projects[static_cast<size_t>(item)].rootPath.empty())
            {
                m_projectList->SetToolTip(wxString::FromUTF8(
                    m_projects[static_cast<size_t>(item)].rootPath));
            } else {
                m_projectList->UnsetToolTip();
            }
        }
    }
    event.Skip();
}

void ProjectAttachDialog::OnNewProject(wxCommandEvent&)
{
    if (!m_onCreateProject) return;

    ProjectInfo createdProject;
    if (!m_onCreateProject(this, createdProject))
        return;

    // Refresh from disk instead of appending locally so the dialog stays
    // consistent with ProjectManager's validation/sort behavior. Leave the
    // dialog open and select the new project; Select remains the explicit attach
    // action for this picker.
    m_projects = ProjectManager::ListProjects();
    ReloadProjectList();
    SelectProject(createdProject);
    UpdateButtons();
}

void ProjectAttachDialog::OnDetachProject(wxCommandEvent&)
{
    // The caller owns the actual detach (same division of labor as
    // attach, which is wxID_OK + GetSelectedProject on the caller's
    // side).  Nothing destructive happens to the project itself, so
    // no confirmation is needed here.
    EndModal(kResultDetach);
}

void ProjectAttachDialog::OnContextMenu(wxContextMenuEvent& event)
{
    if (!m_projectList || m_projects.empty()) return;

    // Move the selection to the row actually under the cursor before
    // showing the menu.  A native listbox does NOT select on right-
    // click, so without this, right-clicking row B while row A is
    // selected would aim every menu action at row A.  Keyboard-
    // invoked menus (Shift+F10 / menu key) arrive with the default
    // position and keep the current selection.
    const wxPoint screenPos = event.GetPosition();
    if (screenPos != wxDefaultPosition) {
        const wxPoint clientPos = m_projectList->ScreenToClient(screenPos);
        const int item = m_projectList->HitTest(clientPos);
        if (item == wxNOT_FOUND) return;   // empty space — no dead-end menu
        m_projectList->SetSelection(item);
        UpdateButtons();
    }

    if (SelectedIndex() < 0) return;

    wxMenu menu;
    menu.Append(ID_CTX_OPEN_FOLDER, "Open folder in Explorer");
    if (m_onRenameProject)
        menu.Append(ID_CTX_RENAME_PROJECT, "Rename project...\tF2");
    menu.AppendSeparator();
    menu.Append(ID_CTX_DELETE_PROJECT, "Delete project\tDel");
    PopupMenu(&menu);
}

void ProjectAttachDialog::OnCharHook(wxKeyEvent& event)
{
    // Del on a selected row triggers delete; F2 triggers rename when
    // a rename callback is wired.  Every other key (Enter, Esc,
    // arrows, tab) skips through to wxDialog's built-in routing so
    // Select/Cancel/navigation still work.
    if (event.GetKeyCode() == WXK_DELETE) {
        if (SelectedIndex() >= 0) {
            wxCommandEvent dummy;
            OnDeleteProject(dummy);
            return;   // don't skip — we handled it
        }
    }
    if (event.GetKeyCode() == WXK_F2 && m_onRenameProject) {
        if (SelectedIndex() >= 0) {
            wxCommandEvent dummy;
            OnRenameProject(dummy);
            return;
        }
    }
    event.Skip();
}

void ProjectAttachDialog::OnDeleteProject(wxCommandEvent&)
{
    const int sel = SelectedIndex();
    if (sel < 0) return;

    const ProjectInfo project = m_projects[static_cast<size_t>(sel)];
    if (m_onDeleteProject)
        m_onDeleteProject(project);

    // DeleteProjectByInfo owns confirmation, failure messaging, and
    // active-chat cleanup. Refresh the picker afterward either way; if
    // deletion was cancelled or failed, the same project simply remains.
    m_projects = ProjectManager::ListProjects();
    ReloadProjectList();

    // Keep the selection near where the user was working instead of
    // snapping back to row 0 after every delete.
    if (!m_projects.empty()) {
        const int last = static_cast<int>(m_projects.size()) - 1;
        m_projectList->SetSelection(std::min(sel, last));
    }
    UpdateButtons();
}

void ProjectAttachDialog::OnRenameProject(wxCommandEvent&)
{
    if (!m_onRenameProject) return;

    const int sel = SelectedIndex();
    if (sel < 0) return;

    const ProjectInfo project = m_projects[static_cast<size_t>(sel)];
    if (!m_onRenameProject(this, project))
        return;   // cancelled or failed — callback owns messaging

    // Same refresh-from-disk discipline as create/delete.  Re-select
    // by id (stable across rename) so the renamed row stays focused.
    m_projects = ProjectManager::ListProjects();
    ReloadProjectList();
    SelectProject(project);
    UpdateButtons();
}

void ProjectAttachDialog::OnOpenProjectFolder(wxCommandEvent&)
{
    const int sel = SelectedIndex();
    if (sel < 0) return;

    const std::string& root = m_projects[static_cast<size_t>(sel)].rootPath;
    if (root.empty()) return;

    // Launching a directory path opens it in Explorer.  Best-effort;
    // a vanished folder simply does nothing visible, and the next
    // ListProjects refresh will drop it from the picker anyway.
    wxLaunchDefaultApplication(wxString::FromUTF8(root));
}
