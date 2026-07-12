// project_controller.h — Projects subsystem extracted from MyFrame.
//
// Owns the project *action surface* that previously lived inline in
// LlamaBoss.cpp: creating / attaching / switching / clearing projects,
// deleting a project's folder, moving chats between projects (and to
// Unassigned), adding Source files, and creating / opening Workflows.
//
// Mirrors the GoalController / SkillDraftController extraction: the
// behavior moves here, the *menus* stay in the frame.  Specifically:
//
//   - The strip popups (ShowProjectPopupMenu / ShowSkillPopupMenu) keep
//     living in MyFrame because they build wxMenus out of the frame's
//     ID_PROJECT_* command ids, which are Bound to the frame's thin
//     OnProject* handlers — exactly the split already used for
//     ShowGoalPopupMenu.  After this extraction those OnProject*
//     handlers become one-line delegations into this controller, the
//     same way OnGoalPause/Resume/etc. already delegate to
//     GoalController.
//
//   - The sidebar context menus (ShowSidebarChatContextMenu /
//     ShowSidebarProjectHeaderContextMenu) also stay in MyFrame: they
//     need a live wxWindow to PopupMenu against and bind inline lambdas.
//     Those lambdas change from calling MyFrame::MoveChatsToProject /
//     AttachProjectToCurrentChat / DeleteProjectByInfo to calling the
//     identically-named methods on this controller.  No behavior moves;
//     only the callee changes.
//
// Ownership rules (same split as before, now enforced by the seam):
//   - ChatHistory owns the durable project association on the active
//     conversation (project id / name / root).  This controller only
//     reads it and calls its mutators (SetProject / ClearProject).
//   - ProjectManager owns everything on disk (project folders,
//     project.json, Sources/Workflows) and is reached only through its
//     static API, same as today.
//   - ProjectContextBuilder owns the cached project-context block and
//     the strip counts; this controller drives it via Invalidate() and
//     GetProjectStripCounts(), unchanged.
//   - MyFrame keeps the unified ProjectStatusStrip (it renders the goal
//     half too), so RefreshProjectStrip() stays in the frame and this
//     controller pokes it through Callbacks::refreshProjectStrip —
//     identical to how GoalController uses refreshGoalStatusStrip.
//
// Threading: every method must be called on the UI thread, same as the
// MyFrame methods they replace.  There are no hidden turns and no
// deferred work here, so unlike GoalController there is no callAfter
// seam — all dialogs are modal and synchronous, exactly as before.

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

class wxWindow;

class AppState;
class ChatDisplay;
class ChatHistory;
class ConversationController;
class ConversationSidebar;
class ProjectContextBuilder;
struct ProjectInfo;

class ProjectController {
public:
    // Frame-owned concerns the controller drives but does not own.
    // Wired in CreateControllersAndCallbacks(), after every coordinator
    // exists.  All callbacks are required; SetCallbacks asserts none
    // are empty in debug builds.
    struct Callbacks {
        // m_chatState != ChatState::Idle.  Project mutations are
        // blocked mid-turn exactly as before — every OnProject* body
        // guarded on IsBusy(), and MoveChatsToProject still refuses
        // while a response is streaming.
        std::function<bool()> isBusy;

        // RefreshProjectStrip().  The strip is the unified
        // project+goal renderer owned by the frame; this controller
        // only asks it to repaint after a project mutation, the same
        // way GoalController does via refreshGoalStatusStrip.
        std::function<void()> refreshProjectStrip;
    };

    // chatHistory is passed as the owning unique_ptr (house style —
    // matches GoalController / SkillDraftController) so the controller
    // always sees the live conversation even if the pointer is ever
    // swapped.  parentFrame is the MyFrame itself, used as the parent
    // for every modal dialog / message box and as the scrim host
    // (LbShowModalWithScrim(*parentFrame, dlg)); it is non-owning and
    // outlives the controller.  sidebar may be refreshed after a move /
    // delete; the controller applies the same visible-guard the frame
    // used (`if (sidebar && sidebar->IsVisible())`).
    ProjectController(std::unique_ptr<ChatHistory>& chatHistory,
                      ChatDisplay*                  chatDisplay,
                      AppState&                     appState,
                      ProjectContextBuilder&        projectContextBuilder,
                      ConversationController&       convController,
                      ConversationSidebar*          sidebar,
                      wxWindow*                     parentFrame);

    void SetCallbacks(Callbacks cb);

    // ── Strip / menu command verbs ──────────────────────────────────
    // Each is the body of a former MyFrame::OnProject* handler.  After
    // extraction the frame's OnProject*(wxCommandEvent&) become
    // one-liners that forward here, so the ID_PROJECT_* Bind table in
    // BindMenuCommands() is untouched.
    void NewProject();                       // Was: OnProjectNew
    void AttachOrSwitchProject();            // Was: OnProjectAttach
    void DeleteProjectViaPicker();           // Was: OnProjectDelete (reuses the attach dialog)
    void OpenProjectFolder();                // Was: OnProjectOpenFolder
    void OpenProjectInstructions();          // Was: OnProjectOpenInstructions
    void AddSourceFiles();                   // Was: OnProjectAddSources
    void OpenSourcesFolder();                // Was: OnProjectOpenSourcesFolder
    void NewWorkflow(bool withPythonScript); // Was: CreateProjectWorkflowFromMenu
    void OpenWorkflow();                      // Was: OnProjectOpenWorkflow
    void OpenWorkflowsFolder();              // Was: OnProjectOpenWorkflowsFolder
    void OpenProjectsRootFolder();           // Was: OnProjectsOpenRootFolder
    void ClearProjectFromChat();             // Was: OnProjectClear

    // ── Verbs invoked by the frame's sidebar context menus & dialogs ─
    // Public so the menu-builder lambdas in MyFrame (which keep owning
    // the wxMenu / PopupMenu plumbing) can call straight in, and so the
    // ProjectAttachDialog create/delete callbacks can bind to them.

    // Was: MyFrame::AttachProjectToCurrentChat.  justCreated switches
    // the system-message copy between the "empty new project" orientation
    // and the "existing project, here's what's in it" summary.  Does NOT
    // persist (a project association on a message-less chat is metadata,
    // not content) — same deliberate no-save behavior as today.
    void AttachProjectToCurrentChat(const ProjectInfo& project,
                                    bool justCreated = false);

    // Was: MyFrame::DeleteProjectByInfo.  Same warning + side-effect
    // dance the attach dialog's delete button used: confirm, delete the
    // folder via ProjectManager, and if the deleted project was the
    // active one, clear it from the chat, invalidate the context cache,
    // re-title, auto-save, and refresh the strip.  Refreshes the sidebar
    // so orphaned chats re-bucket under Unassigned.
    void DeleteProjectByInfo(const ProjectInfo& project);

    // Was: MyFrame::MoveChatsToProject.  Empty targetProjectId ⇒ move to
    // Unassigned (clear).  Active chat is mutated in-memory + auto-saved
    // (sidebar refresh deferred); other chats are round-tripped through a
    // throwaway ChatHistory.  Single sidebar refresh + strip refresh at
    // the end, then a summary system message.  Refuses while busy.
    void MoveChatsToProject(const std::vector<std::string>& paths,
                            const std::string& targetProjectId);

    // Was: MyFrame::PromptCreateProject.  Themed name-entry dialog shown
    // with the scrim; on OK, creates the project via ProjectManager and
    // returns it in outProject.  parentWindow defaults to the frame; the
    // attach dialog passes itself so the prompt stacks over the picker.
    bool PromptCreateProject(ProjectInfo& outProject,
                             wxWindow* parentWindow = nullptr);

private:
    // ── Services (non-owning / by-reference) ────────────────────────
    std::unique_ptr<ChatHistory>& m_chatHistory;
    ChatDisplay*                  m_chatDisplay;
    AppState&                     m_appState;
    ProjectContextBuilder&        m_projectContextBuilder;
    ConversationController&       m_convController;
    ConversationSidebar*          m_sidebar;       // may be null / hidden
    wxWindow*                     m_frame;         // dialog parent + scrim host
    Callbacks                     m_cb;
};
