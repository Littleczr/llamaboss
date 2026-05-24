// project_attach_dialog.cpp

#include "project_attach_dialog.h"

#include "widgets.h"   // ApplyDarkTitleBar

#include <wx/listbox.h>
#include <wx/menu.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/window.h>

#include <utility>

// Local menu IDs for the right-click context menu.
enum {
    ID_DIALOG_NEW_PROJECT = wxID_HIGHEST + 4100,
    ID_CTX_DELETE_PROJECT  = wxID_HIGHEST + 4101
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
// No destructive button is needed here — Delete lives on the list
// (Del key + right-click context menu), not in the footer.
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
    DeleteProjectCallback onDeleteProject)
    : wxDialog(parent, wxID_ANY, "Attach / Manage Project",
               wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_theme(theme)
    , m_projects(std::move(projects))
    , m_onCreateProject(std::move(onCreateProject))
    , m_onDeleteProject(std::move(onDeleteProject))
{
    BuildUi();
}

bool ProjectAttachDialog::GetSelectedProject(ProjectInfo& outProject) const
{
    if (!m_projectList) return false;

    const int sel = m_projectList->GetSelection();
    if (sel < 0 || static_cast<size_t>(sel) >= m_projects.size())
        return false;

    outProject = m_projects[static_cast<size_t>(sel)];
    return true;
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
    top->Add(label, 0, wxALL, 12);

    m_projectList = new wxListBox(
        this, wxID_ANY, wxDefaultPosition, wxSize(320, 140));
    m_projectList->SetBackgroundColour(m_theme.bgInputField);
    m_projectList->SetForegroundColour(m_theme.textPrimary);
    ReloadProjectList();
    top->Add(m_projectList, 1, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 12);

    // Subtle separator above the button row. Kept as a 1px hairline
    // (not the 10px section band used in Settings) — this dialog is
    // small enough that a band would dominate the layout.
    auto* line = new wxPanel(this, wxID_ANY,
                             wxDefaultPosition, wxSize(-1, 1));
    line->SetBackgroundColour(m_theme.borderSubtle);
    top->Add(line, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 12);

    // Button row:
    //   [New...]  [Select]  [Cancel]
    //
    // "New..." opens the existing project-name dialog, so it keeps
    // the ellipsis and uses the same solid accent treatment as Select.
    // Delete stays off the footer; it lives on the list itself via
    // Del key and the right-click context menu.
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);

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

    // Hook Del key globally for the dialog. Skipping on every other
    // key lets wxDialog's built-in affirmative/escape mechanism still
    // route Enter→Select and Esc→Cancel without us intercepting them.
    Bind(wxEVT_CHAR_HOOK, &ProjectAttachDialog::OnCharHook, this);

    // Right-click anywhere on the list pops the context menu. The
    // menu only contains "Delete project" today; if more list-row
    // actions appear later they slot in here.
    m_projectList->Bind(wxEVT_CONTEXT_MENU,
                        &ProjectAttachDialog::OnContextMenu, this);
    Bind(wxEVT_MENU,
         &ProjectAttachDialog::OnDeleteProject, this,
         ID_CTX_DELETE_PROJECT);

    Bind(wxEVT_BUTTON,
         &ProjectAttachDialog::OnNewProject, this,
         ID_DIALOG_NEW_PROJECT);

    m_projectList->Bind(wxEVT_LISTBOX,
                        &ProjectAttachDialog::OnListSelection, this);
    m_projectList->Bind(wxEVT_LISTBOX_DCLICK,
                        &ProjectAttachDialog::OnListDoubleClick, this);

    UpdateButtons();
}

void ProjectAttachDialog::ReloadProjectList()
{
    if (!m_projectList) return;

    m_projectList->Clear();
    for (const auto& project : m_projects) {
        m_projectList->Append(wxString::FromUTF8(project.name));
    }

    if (!m_projects.empty())
        m_projectList->SetSelection(0);
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
    const bool hasSelection =
        m_projectList &&
        m_projectList->GetSelection() != wxNOT_FOUND &&
        !m_projects.empty();

    if (m_okButton)
        m_okButton->Enable(hasSelection);

    if (m_newButton)
        m_newButton->Enable(static_cast<bool>(m_onCreateProject));
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

void ProjectAttachDialog::OnContextMenu(wxContextMenuEvent&)
{
    // Right-click on the list. Only show the menu if there's a row
    // selected to act on — otherwise the user is clicking empty
    // space and the menu would be a dead end.
    if (!m_projectList) return;
    if (m_projects.empty()) return;
    if (m_projectList->GetSelection() == wxNOT_FOUND) return;

    wxMenu menu;
    menu.Append(ID_CTX_DELETE_PROJECT, "Delete project");
    PopupMenu(&menu);
}

void ProjectAttachDialog::OnCharHook(wxKeyEvent& event)
{
    // Del on a selected row triggers delete. Every other key
    // (Enter, Esc, arrows, tab) skips through to wxDialog's
    // built-in routing so Select/Cancel/navigation still work.
    if (event.GetKeyCode() == WXK_DELETE) {
        if (m_projectList &&
            m_projectList->GetSelection() != wxNOT_FOUND &&
            !m_projects.empty())
        {
            wxCommandEvent dummy;
            OnDeleteProject(dummy);
            return;   // don't skip — we handled it
        }
    }
    event.Skip();
}

void ProjectAttachDialog::OnDeleteProject(wxCommandEvent&)
{
    if (!m_projectList) return;

    const int sel = m_projectList->GetSelection();
    if (sel < 0 || static_cast<size_t>(sel) >= m_projects.size())
        return;

    const ProjectInfo project = m_projects[static_cast<size_t>(sel)];
    if (m_onDeleteProject)
        m_onDeleteProject(project);

    // DeleteProjectByInfo owns confirmation, failure messaging, and
    // active-chat cleanup. Refresh the picker afterward either way; if
    // deletion was cancelled or failed, the same project simply remains.
    m_projects = ProjectManager::ListProjects();
    ReloadProjectList();
    UpdateButtons();

}
