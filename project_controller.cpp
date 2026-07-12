// project_controller.cpp — see project_controller.h for the seam contract.
//
// Every method here is the verbatim body of a former MyFrame method,
// with three mechanical substitutions applied throughout:
//   - IsBusy()             -> m_cb.isBusy()
//   - RefreshProjectStrip() -> m_cb.refreshProjectStrip()
//   - the frame's `this`   -> m_frame  (dialog parent / scrim host /
//                                       LbOpenProject*ByRoot arg)
// and two member-shape changes (pointer coordinators became
// references): m_projectContextBuilder-> and m_convController-> become
// m_projectContextBuilder. and m_convController.; m_appState->GetTheme()
// becomes m_appState.GetTheme().  m_chatHistory / m_chatDisplay /
// m_sidebar remain pointers and are unchanged.  No control-flow,
// message text, or persistence behavior was altered.

#include "project_controller.h"

#include <wx/wx.h>
#include <wx/filedlg.h>
#include <wx/textdlg.h>
#include <wx/choicdlg.h>
#include <wx/utils.h>

#include <algorithm>
#include <cstddef>
#include <sstream>

#include "app_state.h"
#include "chat_display.h"
#include "chat_history.h"
#include "conversation_controller.h"
#include "conversation_sidebar.h"
#include "project_context_builder.h"
#include "project_manager.h"
#include "project_attach_dialog.h"
#include "lb_string_utils.h"        // ProjectSource_HumanBytes
#include "lb_themed_dialogs.h"      // LbThemedTextEntryDialog
#include "lb_modal_scrim.h"         // LbShowModalWithScrim
#include "lb_project_ui_actions.h"  // LbOpenProject*ByRoot, LbLaunchPathInOS

// ─────────────────────────────────────────────────────────────────────
//  Construction
// ─────────────────────────────────────────────────────────────────────

ProjectController::ProjectController(std::unique_ptr<ChatHistory>& chatHistory,
                                     ChatDisplay*                  chatDisplay,
                                     AppState&                     appState,
                                     ProjectContextBuilder&        projectContextBuilder,
                                     ConversationController&       convController,
                                     ConversationSidebar*          sidebar,
                                     wxWindow*                     parentFrame)
    : m_chatHistory(chatHistory),
      m_chatDisplay(chatDisplay),
      m_appState(appState),
      m_projectContextBuilder(projectContextBuilder),
      m_convController(convController),
      m_sidebar(sidebar),
      m_frame(parentFrame)
{
}

void ProjectController::SetCallbacks(Callbacks cb)
{
    m_cb = std::move(cb);
    // All callbacks are required; surface a wiring mistake immediately
    // in debug builds, same contract as GoalController::SetCallbacks.
    wxASSERT(m_cb.isBusy);
    wxASSERT(m_cb.refreshProjectStrip);
}

// ─────────────────────────────────────────────────────────────────────
//  Attach / create / delete
// ─────────────────────────────────────────────────────────────────────

void ProjectController::AttachProjectToCurrentChat(const ProjectInfo& project,
                                                   bool justCreated)
{
    const bool alreadyAttached =
        m_chatHistory->HasProject() &&
        m_chatHistory->GetProjectId() == project.id &&
        m_chatHistory->GetProjectRoot() == project.rootPath;

    if (alreadyAttached) {
        m_chatDisplay->DisplaySystemMessage(
            "This chat is already attached to project: " + project.name +
            "\n" + project.rootPath);
        m_cb.refreshProjectStrip();
        m_convController.UpdateWindowTitle();
        return;
    }

    m_chatHistory->SetProject(project.id, project.name, project.rootPath);
    m_projectContextBuilder.Invalidate();

    std::string msg;
    if (justCreated) {
        // A freshly created project is empty by definition, so orient the
        // user toward the first useful action instead of a bare one-liner.
        msg  = "Created project: " + project.name + "\n";
        msg += project.rootPath + "\n\n";
        msg += "This project is empty. It has Sources/, Templates/, Workflows/, "
               "Outputs/, and Notes/ folders, plus a placeholder PROJECT.md.\n\n";
        msg += "To get started:\n";
        msg += "  - Tell me what this project is for and ask me to fill in PROJECT.md.\n";
        msg += "  - Drag reference files into the chat to add them as Sources.\n";
        msg += "  - Use [ Change... ] in the project strip to switch or detach.";
    } else {
        // Existing project: reflect what is already in it so re-attaching a
        // populated project does not read the same as a brand-new one.
        const ProjectStripCounts counts = m_projectContextBuilder.GetProjectStripCounts(project.rootPath);
        const std::size_t sourceCount =
            static_cast<std::size_t>(counts.sourceCount);
        const std::size_t workflowCount =
            static_cast<std::size_t>(counts.workflowCount);

        msg  = "Attached this chat to project: " + project.name + "\n";
        msg += project.rootPath + "\n\n";
        msg += std::to_string(sourceCount) +
               (sourceCount == 1 ? " source" : " sources") + " \xC2\xB7 " +
               std::to_string(workflowCount) +
               (workflowCount == 1 ? " workflow" : " workflows") +
               " \xC2\xB7 PROJECT.md loaded as the project contract.";
    }
    m_chatDisplay->DisplaySystemMessage(msg);
    m_convController.UpdateWindowTitle();
    m_cb.refreshProjectStrip();

    // Deliberately do NOT persist here.  A project attachment on a
    // message-less chat is metadata, not content worth a history entry;
    // eager-saving it left an empty chat in the sidebar whenever the user
    // attached/created a project and then hit New Chat (or closed)
    // without typing.  The association still persists on the first real
    // activity: the first sent message auto-saves the chat, and switching
    // to another conversation saves it via LoadConversationFromPath's
    // persistable-content check.  New Chat and app close are both
    // message-based, so abandoning a blank project chat now leaves no
    // trace — matching the no-project case.
}

void ProjectController::DeleteProjectByInfo(const ProjectInfo& project)
{
    wxString warning;
    warning << "Delete this project and ALL files inside it?\n\n";
    warning << wxString::FromUTF8(project.name) << "\n";
    warning << wxString::FromUTF8(project.rootPath) << "\n\n";
    warning << "This will permanently delete the project folder and everything in it (Sources, Workflows, Outputs, Notes, PROJECT.md, and project.json).\n";
    warning << "This cannot be undone.";

    int answer = wxMessageBox(warning, "Delete Project",
                              wxYES_NO | wxNO_DEFAULT | wxICON_WARNING,
                              m_frame);
    if (answer != wxYES) return;

    const bool deletingActive =
        m_chatHistory->HasProject() &&
        (m_chatHistory->GetProjectId() == project.id ||
         m_chatHistory->GetProjectRoot() == project.rootPath);

    std::string error;
    if (!ProjectManager::DeleteProject(project, error)) {
        wxString msg = "Could not delete project.";
        if (!error.empty()) {
            msg += "\n\n";
            msg += wxString::FromUTF8(error);
        }
        wxMessageBox(msg, "Delete Project", wxOK | wxICON_ERROR, m_frame);
        return;
    }

    if (deletingActive) {
        m_chatHistory->ClearProject();
        m_projectContextBuilder.Invalidate();
        m_convController.UpdateWindowTitle();
        m_convController.AutoSaveConversation();
        m_cb.refreshProjectStrip();
    }

    m_chatDisplay->DisplaySystemMessage(
        "Deleted project: " + project.name + "\n" + project.rootPath);

    // Sidebar will re-bucket any remaining chats that referenced
    // this project under Unassigned on its next refresh.
    if (m_sidebar && m_sidebar->IsVisible()) {
        m_sidebar->Refresh(m_chatHistory->GetFilePath());
    }
}

bool ProjectController::PromptCreateProject(ProjectInfo& outProject,
                                            wxWindow* parentWindow)
{
    wxWindow* parent = parentWindow ? parentWindow : m_frame;

    LbThemedTextEntryDialog dlg(
        parent,
        m_appState.GetTheme(),
        "New LlamaBoss Project",
        "Project name:",
        "Create");
    if (LbShowModalWithScrim(*m_frame, dlg) != wxID_OK) return false;

    const std::string name = std::string(dlg.GetValue().ToUTF8().data());
    ProjectInfo project;
    std::string error;
    if (!ProjectManager::CreateProject(name, project, error)) {
        std::string errorMsg = error.empty()
            ? std::string("Could not create project.")
            : error;
        wxMessageBox(wxString::FromUTF8(errorMsg.c_str()),
                     "Project Error",
                     wxOK | wxICON_ERROR,
                     parent);
        return false;
    }

    outProject = project;
    return true;
}

// ─────────────────────────────────────────────────────────────────────
//  Move chats between projects (or to Unassigned)
// ─────────────────────────────────────────────────────────────────────

void ProjectController::MoveChatsToProject(const std::vector<std::string>& paths,
                                           const std::string& targetProjectId)
{
    if (paths.empty()) return;
    if (m_cb.isBusy()) {
        wxMessageBox(
            "Stop the current response before reassigning chats.",
            "Move to Project", wxOK | wxICON_INFORMATION, m_frame);
        return;
    }

    // Resolve the destination project (if any) up front so we
    // don't re-list on every iteration.  Empty target = clear.
    const bool toUnassigned = targetProjectId.empty();
    ProjectInfo targetProject;
    if (!toUnassigned) {
        if (!ProjectManager::LoadProjectById(targetProjectId, targetProject)) {
            wxMessageBox(
                "The destination project no longer exists.",
                "Move to Project", wxOK | wxICON_WARNING, m_frame);
            return;
        }
    }

    const std::string activePath =
        m_chatHistory ? m_chatHistory->GetFilePath() : std::string();

    size_t moved = 0;
    size_t skipped = 0;

    for (const auto& path : paths) {
        if (path.empty()) continue;

        const bool isActive = (!activePath.empty() && path == activePath);

        if (isActive) {
            // No-op when the current chat already has the target
            // project — saves a redundant rewrite of the JSON.
            const bool sameAlready = toUnassigned
                ? !m_chatHistory->HasProject()
                : (m_chatHistory->HasProject() &&
                   m_chatHistory->GetProjectId() == targetProject.id);
            if (sameAlready) { ++skipped; continue; }

            if (toUnassigned) {
                m_chatHistory->ClearProject();
            }
            else {
                m_chatHistory->SetProject(targetProject.id,
                                          targetProject.name,
                                          targetProject.rootPath);
            }
            m_projectContextBuilder.Invalidate();
            m_convController.UpdateWindowTitle();
            m_convController.AutoSaveConversation(/*refreshSidebar=*/false);
            ++moved;
        }
        else {
            // Throwaway ChatHistory — load, mutate, save.  Models
            // are round-tripped through LoadFromFile/SaveToFile so
            // we don't accidentally rewrite the file with no model
            // recorded.
            ChatHistory tmp;
            std::vector<std::string> models;
            if (!tmp.LoadFromFile(path, models)) {
                ++skipped;
                continue;
            }

            const bool sameAlready = toUnassigned
                ? !tmp.HasProject()
                : (tmp.HasProject() &&
                   tmp.GetProjectId() == targetProject.id);
            if (sameAlready) { ++skipped; continue; }

            if (toUnassigned) {
                tmp.ClearProject();
            }
            else {
                tmp.SetProject(targetProject.id,
                               targetProject.name,
                               targetProject.rootPath);
            }
            if (!tmp.SaveToFile(path, models)) {
                ++skipped;
                continue;
            }
            ++moved;
        }
    }

    // Single sidebar refresh at the end picks up every reassignment.
    if (m_sidebar && m_sidebar->IsVisible()) {
        m_sidebar->Refresh(m_chatHistory->GetFilePath());
    }
    m_cb.refreshProjectStrip();

    // Brief system message so the user gets visible confirmation.
    // Only emitted when there's something interesting to report.
    if (moved > 0) {
        std::string destLabel = toUnassigned
            ? std::string("Unassigned")
            : targetProject.name;
        std::string msg = "Moved " + std::to_string(moved) +
                          (moved == 1 ? " chat" : " chats") +
                          " to: " + destLabel;
        if (skipped > 0) {
            msg += "\n(" + std::to_string(skipped) +
                   (skipped == 1 ? " chat" : " chats") +
                   " skipped — already at destination or unreadable)";
        }
        m_chatDisplay->DisplaySystemMessage(msg);
    }
}

// ─────────────────────────────────────────────────────────────────────
//  Strip / menu command verbs  (former OnProject* bodies)
// ─────────────────────────────────────────────────────────────────────

void ProjectController::NewProject()
{
    if (m_cb.isBusy()) {
        m_chatDisplay->DisplaySystemMessage(
            "Finish the current response or tool run before creating a project.");
        return;
    }

    ProjectInfo project;
    if (!PromptCreateProject(project)) return;

    AttachProjectToCurrentChat(project, /*justCreated=*/true);
}

void ProjectController::AttachOrSwitchProject()
{
    if (m_cb.isBusy()) {
        m_chatDisplay->DisplaySystemMessage(
            "Finish the current response or tool run before changing projects.");
        return;
    }

    auto projects = ProjectManager::ListProjects();

    ProjectAttachDialog dlg(
        m_frame,
        m_appState.GetTheme(),
        std::move(projects),
        [this](wxWindow* parent, ProjectInfo& project) {
            return PromptCreateProject(project, parent);
        },
        [this](const ProjectInfo& project) {
            DeleteProjectByInfo(project);
        });

    const int dialogResult = LbShowModalWithScrim(*m_frame, dlg);

    if (dialogResult != wxID_OK) return;

    ProjectInfo selectedProject;
    if (!dlg.GetSelectedProject(selectedProject)) return;
    AttachProjectToCurrentChat(selectedProject);
}

void ProjectController::DeleteProjectViaPicker()
{
    if (m_cb.isBusy()) {
        m_chatDisplay->DisplaySystemMessage(
            "Finish the current response or tool run before deleting a project.");
        return;
    }

    // Reuse the attach/switch dialog because it now includes a safe
    // Delete Project... action next to the project list.
    AttachOrSwitchProject();
}

void ProjectController::OpenProjectFolder()
{
    if (!m_chatHistory->HasProject()) {
        wxMessageBox(
            "No project is attached to the current chat.",
            "Projects",
            wxOK | wxICON_INFORMATION,
            m_frame);
        return;
    }
    LbOpenProjectFolderByRoot(m_frame, m_chatHistory->GetProjectRoot());
}

void ProjectController::OpenProjectInstructions()
{
    if (!m_chatHistory->HasProject()) {
        m_chatDisplay->DisplaySystemMessage("No project is attached to the current chat.");
        wxMessageBox(
            "No project is attached to the current chat.",
            "Projects", wxOK | wxICON_INFORMATION, m_frame);
        return;
    }
    LbOpenProjectInstructionsByRoot(m_frame, m_chatHistory->GetProjectRoot());
}

void ProjectController::AddSourceFiles()
{
    if (m_cb.isBusy()) return;

    if (!m_chatHistory->HasProject()) {
        m_chatDisplay->DisplaySystemMessage(
            "No project is attached to the current chat.");
        wxMessageBox(
            "No project is attached to the current chat.",
            "Projects", wxOK | wxICON_INFORMATION, m_frame);
        return;
    }

    wxFileDialog dlg(
        m_frame,
        "Add files to project Sources",
        wxEmptyString,
        wxEmptyString,
        "All files (*.*)|*.*",
        wxFD_OPEN | wxFD_FILE_MUST_EXIST | wxFD_MULTIPLE);
    if (dlg.ShowModal() != wxID_OK) return;

    wxArrayString selected;
    dlg.GetPaths(selected);
    if (selected.IsEmpty()) return;

    std::vector<std::string> sourcePaths;
    sourcePaths.reserve(selected.GetCount());
    for (const auto& path : selected) {
        sourcePaths.push_back(std::string(path.ToUTF8().data()));
    }

    std::vector<ProjectSourceInfo> copied;
    std::vector<std::string> skipped;
    std::string error;
    const bool ok = ProjectManager::CopyFilesToProjectSources(
        m_chatHistory->GetProjectRoot(), sourcePaths, copied, skipped, error);

    if (!ok) {
        std::string msg = error.empty() ? "Could not add files to project Sources." : error;
        if (!skipped.empty()) {
            msg += "\n\nSkipped:";
            for (const auto& path : skipped) msg += "\n- " + path;
        }
        wxMessageBox(wxString::FromUTF8(msg.c_str()),
                     "Projects", wxOK | wxICON_ERROR, m_frame);
        return;
    }

    std::ostringstream body;
    body << "Added " << copied.size() << " file" << (copied.size() == 1 ? "" : "s")
         << " to project Sources for: " << m_chatHistory->GetProjectName() << "\n"
         << ProjectManager::ProjectSourcesPath(m_chatHistory->GetProjectRoot());
    for (const auto& file : copied) {
        body << "\n- " << file.name << " (" << ProjectSource_HumanBytes(file.sizeBytes) << ")";
    }
    if (!skipped.empty()) {
        body << "\n\nSkipped:";
        for (const auto& path : skipped) body << "\n- " << path;
    }
    m_chatDisplay->DisplaySystemMessage(body.str());
    m_projectContextBuilder.Invalidate();
    m_cb.refreshProjectStrip();
}

void ProjectController::OpenSourcesFolder()
{
    if (!m_chatHistory->HasProject()) {
        m_chatDisplay->DisplaySystemMessage("No project is attached to the current chat.");
        wxMessageBox(
            "No project is attached to the current chat.",
            "Projects", wxOK | wxICON_INFORMATION, m_frame);
        return;
    }
    LbOpenProjectSourcesFolderByRoot(m_frame, m_chatHistory->GetProjectRoot());
}

void ProjectController::NewWorkflow(bool withPythonScript)
{
    if (m_cb.isBusy()) return;

    if (!m_chatHistory->HasProject()) {
        m_chatDisplay->DisplaySystemMessage("No project is attached to the current chat.");
        wxMessageBox(
            "No project is attached to the current chat.",
            "Projects", wxOK | wxICON_INFORMATION, m_frame);
        return;
    }

    wxTextEntryDialog dlg(
        m_frame,
        "Workflow name:",
        withPythonScript ? "New Project Workflow with Python Script" : "New Project Workflow");
    if (dlg.ShowModal() != wxID_OK) return;

    const std::string name = std::string(dlg.GetValue().ToUTF8().data());
    ProjectWorkflowInfo workflow;
    ProjectWorkflowScriptInfo script;
    std::string error;
    bool ok = false;
    if (withPythonScript) {
        ok = ProjectManager::CreateProjectWorkflowWithScript(
            m_chatHistory->GetProjectRoot(), name, workflow, script, error);
    } else {
        ok = ProjectManager::CreateProjectWorkflow(
            m_chatHistory->GetProjectRoot(), name, workflow, error);
    }

    if (!ok) {
        std::string msg = error.empty()
            ? std::string("Could not create project workflow.")
            : error;
        wxMessageBox(wxString::FromUTF8(msg.c_str()),
                     "Projects", wxOK | wxICON_ERROR, m_frame);
        return;
    }

    std::ostringstream body;
    body << "Created project workflow for: " << m_chatHistory->GetProjectName() << "\n"
         << workflow.path;
    if (withPythonScript && !script.path.empty()) {
        body << "\n\nCreated optional Python helper script:\n"
             << script.path;
    }
    body << "\n\nEdit the workflow file to define trigger phrases, required inputs, steps, and output expectations.";
    if (withPythonScript) {
        body << "\nEdit the Python helper script only for repeatable mechanical work this workflow needs.";
    }
    m_chatDisplay->DisplaySystemMessage(body.str());
    m_projectContextBuilder.Invalidate();

    // Open the workflow immediately so the user can edit the contract.
    wxLaunchDefaultApplication(wxString::FromUTF8(workflow.path));
    if (withPythonScript && !script.path.empty()) {
        wxLaunchDefaultApplication(wxString::FromUTF8(script.path));
    }
    m_cb.refreshProjectStrip();
}

void ProjectController::OpenWorkflow()
{
    if (!m_chatHistory->HasProject()) {
        m_chatDisplay->DisplaySystemMessage("No project is attached to the current chat.");
        wxMessageBox(
            "No project is attached to the current chat.",
            "Projects", wxOK | wxICON_INFORMATION, m_frame);
        return;
    }

    auto workflows = ProjectManager::ListProjectWorkflows(m_chatHistory->GetProjectRoot(), 0);
    if (workflows.empty()) {
        wxMessageBox(
            "No project workflows found yet. Use Projects > New Project Workflow first.",
            "Projects", wxOK | wxICON_INFORMATION, m_frame);
        return;
    }

    wxArrayString choices;
    for (const auto& wf : workflows) {
        choices.Add(wxString::FromUTF8(wf.name));
    }

    wxSingleChoiceDialog dlg(
        m_frame,
        "Select a project workflow to open:",
        "Open Project Workflow",
        choices);
    if (dlg.ShowModal() != wxID_OK) return;

    int sel = dlg.GetSelection();
    if (sel < 0 || static_cast<size_t>(sel) >= workflows.size()) return;

    const std::string path = workflows[static_cast<size_t>(sel)].path;
    if (!wxLaunchDefaultApplication(wxString::FromUTF8(path))) {
        wxMessageBox("Could not open the selected project workflow.",
                     "Projects", wxOK | wxICON_ERROR, m_frame);
    }
}

void ProjectController::OpenWorkflowsFolder()
{
    if (!m_chatHistory->HasProject()) {
        m_chatDisplay->DisplaySystemMessage("No project is attached to the current chat.");
        wxMessageBox(
            "No project is attached to the current chat.",
            "Projects", wxOK | wxICON_INFORMATION, m_frame);
        return;
    }
    LbOpenProjectWorkflowsFolderByRoot(m_frame, m_chatHistory->GetProjectRoot());
}

void ProjectController::OpenProjectsRootFolder()
{
    // No-project menu: there's no active project to open, so this opens
    // the Projects root (the parallel to "Open Skills Folder").  Ensure
    // it exists first so a brand-new install still opens cleanly.
    ProjectManager::EnsureProjectsRoot();
    const std::string dir = ProjectManager::GetProjectsDir();
    LbLaunchPathInOS(m_frame, dir, "LlamaBoss Projects folder");
}

void ProjectController::ClearProjectFromChat()
{
    if (m_cb.isBusy()) return;

    if (!m_chatHistory->HasProject()) {
        m_chatDisplay->DisplaySystemMessage(
            "No project is attached to the current chat.");
        return;
    }

    std::string name = m_chatHistory->GetProjectName();
    m_chatHistory->ClearProject();
    m_projectContextBuilder.Invalidate();
    m_chatDisplay->DisplaySystemMessage(
        "Cleared project from this chat: " + name);
    m_convController.UpdateWindowTitle();

    // If this chat has messages, persist the cleared association.
    // A brand-new empty chat without project metadata has nothing to save.
    m_convController.AutoSaveConversation();
}
