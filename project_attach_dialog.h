#pragma once
// project_attach_dialog.h
//
// Extracted Attach / Manage Project dialog.
// Keeps project-picking UI and in-dialog delete/reload behavior out of
// LlamaBoss.cpp, while the frame remains responsible for chat/project
// side effects through the delete callback and final attach action.
//
// Button styling matches the rest of the dialog family: Select is a solid
// accent wxButton, Cancel is a flat borderless wxButton with muted
// text. New and Select use the solid accent treatment because both are
// primary actions in this picker. Delete lives where it belongs — on
// the list itself — and is reached via Del key or right-click context
// menu, not a footer button. See project_attach_dialog.cpp for the helpers.

#include <wx/dialog.h>
#include <wx/button.h>

#include <functional>
#include <vector>

#include "project_manager.h"
#include "theme.h"

class wxListBox;
class wxCommandEvent;

class ProjectAttachDialog final : public wxDialog
{
public:
    using CreateProjectCallback = std::function<bool(wxWindow*, ProjectInfo&)>;
    using DeleteProjectCallback = std::function<void(const ProjectInfo&)>;

    ProjectAttachDialog(wxWindow* parent,
                        const ThemeData& theme,
                        std::vector<ProjectInfo> projects,
                        CreateProjectCallback onCreateProject,
                        DeleteProjectCallback onDeleteProject);

    bool GetSelectedProject(ProjectInfo& outProject) const;

private:
    void BuildUi();
    void ReloadProjectList();
    void SelectProject(const ProjectInfo& project);
    void UpdateButtons();

    void OnListSelection(wxCommandEvent& event);
    void OnListDoubleClick(wxCommandEvent& event);
    void OnNewProject(wxCommandEvent& event);
    void OnDeleteProject(wxCommandEvent& event);
    void OnContextMenu(wxContextMenuEvent& event);   // Right-click on list
    void OnCharHook(wxKeyEvent& event);              // Del key catch

    ThemeData m_theme;
    std::vector<ProjectInfo> m_projects;
    CreateProjectCallback m_onCreateProject;
    DeleteProjectCallback m_onDeleteProject;

    wxListBox* m_projectList = nullptr;
    wxButton*  m_newButton   = nullptr;   // solid accent create action
    wxButton*  m_okButton    = nullptr;   // solid accent select/attach action
};
