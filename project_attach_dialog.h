// project_attach_dialog.h
//
// Picker dialog for attaching a project to the current chat, plus
// lightweight management of the project list itself.
//
// ─── Capabilities ────────────────────────────────────────────────
//   Attach   : select a project (double-click / Enter / Select).
//   Detach   : footer button, shown only when the chat already has
//              an attached project.  The dialog does NOT perform the
//              detach; it returns kResultDetach from ShowModal() and
//              the caller acts on it (mirrors how Select returns
//              wxID_OK and the caller performs the attach).
//   Create   : "New..." button → onCreateProject callback.
//   Delete   : Del key or right-click menu → onDeleteProject
//              callback (callback owns confirmation + cleanup).
//   Rename   : right-click menu or F2 → onRenameProject callback.
//              The menu item / key only activates when the callback
//              is provided, so callers can adopt it incrementally.
//   Open     : right-click "Open folder in Explorer" — no callback
//              needed; uses ProjectInfo::rootPath directly.
//
// ─── Attached-project awareness ──────────────────────────────────
// Pass the currently-attached project (if any) as `attachedProject`.
// The dialog marks it in the list ("● name   (attached)"),
// pre-selects it, and shows the Detach button.  Pass a default-
// constructed ProjectInfo (the default) for "nothing attached" —
// detection keys off id/rootPath being non-empty.
//
// ─── ShowModal() results ─────────────────────────────────────────
//   wxID_OK        → attach GetSelectedProject()
//   wxID_CANCEL    → no change
//   kResultDetach  → detach the currently-attached project
//
#pragma once

#include <wx/dialog.h>

#include <functional>
#include <vector>

#include "project_manager.h"   // ProjectInfo, ProjectManager
#include "theme.h"             // ThemeData

class wxButton;
class wxListBox;
class wxStaticText;
class wxCommandEvent;
class wxContextMenuEvent;
class wxKeyEvent;
class wxMouseEvent;

class ProjectAttachDialog : public wxDialog
{
public:
    using CreateProjectCallback =
        std::function<bool(wxWindow* parent, ProjectInfo& createdOut)>;
    using DeleteProjectCallback =
        std::function<void(const ProjectInfo& project)>;
    // Returns true iff the rename happened (dialog refreshes its list).
    // The callback owns the name prompt, validation, and the actual
    // ProjectManager rename — same division of labor as delete.
    using RenameProjectCallback =
        std::function<bool(wxWindow* parent, const ProjectInfo& project)>;

    // Custom ShowModal() result for the Detach button.  Chosen well
    // clear of the stock wxID_* return values.
    static constexpr int kResultDetach = wxID_HIGHEST + 4150;

    ProjectAttachDialog(wxWindow* parent,
                        const ThemeData& theme,
                        std::vector<ProjectInfo> projects,
                        CreateProjectCallback onCreateProject,
                        DeleteProjectCallback onDeleteProject,
                        ProjectInfo attachedProject = {},
                        RenameProjectCallback onRenameProject = {});

    // Valid when ShowModal() returned wxID_OK.
    bool GetSelectedProject(ProjectInfo& outProject) const;

    // Select a specific project in the list (matched by id, falling
    // back to rootPath).  No-op if it isn't in the list.
    void SelectProject(const ProjectInfo& project);

private:
    void BuildUi();
    void ReloadProjectList();
    void UpdateButtons();

    bool HasAttachedProject() const;
    bool IsAttachedProject(const ProjectInfo& p) const;
    int  SelectedIndex() const;   // -1 when nothing usable is selected

    // ── Event handlers ───────────────────────────────────────────
    void OnListSelection(wxCommandEvent& event);
    void OnListDoubleClick(wxCommandEvent& event);
    void OnListMotion(wxMouseEvent& event);       // per-row path tooltip
    void OnNewProject(wxCommandEvent& event);
    void OnDetachProject(wxCommandEvent& event);
    void OnContextMenu(wxContextMenuEvent& event);
    void OnCharHook(wxKeyEvent& event);
    void OnDeleteProject(wxCommandEvent& event);
    void OnRenameProject(wxCommandEvent& event);
    void OnOpenProjectFolder(wxCommandEvent& event);

    ThemeData                m_theme;
    std::vector<ProjectInfo> m_projects;
    CreateProjectCallback    m_onCreateProject;
    DeleteProjectCallback    m_onDeleteProject;
    RenameProjectCallback    m_onRenameProject;
    ProjectInfo              m_attachedProject;   // empty id+rootPath = none

    wxListBox*    m_projectList  = nullptr;
    wxStaticText* m_emptyLabel   = nullptr;
    wxButton*     m_okButton     = nullptr;
    wxButton*     m_newButton    = nullptr;
    wxButton*     m_detachButton = nullptr;   // nullptr when nothing attached

    int m_tooltipItem = -1;   // last list row the hover tooltip was set for
};
