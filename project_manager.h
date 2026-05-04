#pragma once
// project_manager.h
//
// Projects foundation.  A Project is a long-lived user folder
// under %USERPROFILE%\LlamaBoss\Projects that can be attached to one or
// more conversations.  Phase 2 loads PROJECT.md as the trusted project
// contract; workflows and skills still come later.

#include <string>
#include <vector>
#include <cstddef>

struct ProjectInfo
{
    std::string id;
    std::string name;
    std::string rootPath;
    std::string createdAt;
    std::string updatedAt;
};

struct ProjectSourceInfo
{
    std::string name;
    std::string path;
    unsigned long long sizeBytes = 0;
};

struct ProjectWorkflowInfo
{
    std::string name;
    std::string path;
    unsigned long long sizeBytes = 0;
};

struct ProjectWorkflowScriptInfo
{
    std::string name;
    std::string path;
    unsigned long long sizeBytes = 0;
};

class ProjectManager
{
public:
    // %USERPROFILE%\LlamaBoss\Projects, with home/documents fallbacks.
    static std::string GetProjectsDir();
    static bool EnsureProjectsRoot();

    // Create a new project folder and scaffold. Returns false if name is
    // blank, the folder already exists, or any required file/dir fails.
    static bool CreateProject(const std::string& name,
                              ProjectInfo& outProject,
                              std::string& outError);

    // List projects that have project.json metadata.  Folders without
    // project.json are ignored in Phase 1 to avoid guessing user intent.
    static std::vector<ProjectInfo> ListProjects();

    static bool LoadProjectByRoot(const std::string& rootPath,
                                  ProjectInfo& outProject);
    static bool LoadProjectById(const std::string& id,
                                ProjectInfo& outProject);

    // Permanently delete a project folder and all files under it. This is
    // intentionally scoped to folders under the LlamaBoss Projects root and
    // requires a valid project.json-backed ProjectInfo from ListProjects().
    static bool DeleteProject(const ProjectInfo& project,
                              std::string& outError);

    static std::string ProjectJsonPath(const std::string& rootPath);
    static std::string ProjectInstructionsPath(const std::string& rootPath);
    static std::string ProjectSourcesPath(const std::string& rootPath);
    static std::string ProjectWorkflowsPath(const std::string& rootPath);

    // Projects Phase 3: copy user-selected long-lived source files into
    // Sources/ and expose a lightweight file listing to the prompt.
    static bool CopyFilesToProjectSources(const std::string& rootPath,
                                          const std::vector<std::string>& sourcePaths,
                                          std::vector<ProjectSourceInfo>& outCopied,
                                          std::vector<std::string>& outSkipped,
                                          std::string& outError);

    static std::vector<ProjectSourceInfo> ListProjectSources(const std::string& rootPath,
                                                             std::size_t maxItems = 50);

    // Projects Phase 4: resolve a user/model-supplied source reference
    // against Sources/.  Supports exact filename, Sources/<name>, stem-only,
    // and unique case-insensitive partial matches. Returns false when there
    // is no match or the reference is ambiguous.
    static bool ResolveProjectSource(const std::string& rootPath,
                                     const std::string& requested,
                                     ProjectSourceInfo& outSource,
                                     std::string& outError);

    // Projects Phase 5: lightweight workflow files live in Workflows/.
    // A workflow is a Markdown instruction/plan file, not a separate engine.
    static bool CreateProjectWorkflow(const std::string& rootPath,
                                      const std::string& workflowName,
                                      ProjectWorkflowInfo& outWorkflow,
                                      std::string& outError);

    // Projects Phase 5.5: optional project workflow helper script. The
    // workflow remains a Markdown plan; the .py file is an optional helper
    // that can be run with python_run_script from an active project.
    static bool CreateProjectWorkflowWithScript(const std::string& rootPath,
                                                const std::string& workflowName,
                                                ProjectWorkflowInfo& outWorkflow,
                                                ProjectWorkflowScriptInfo& outScript,
                                                std::string& outError);

    static std::vector<ProjectWorkflowInfo> ListProjectWorkflows(const std::string& rootPath,
                                                                 std::size_t maxItems = 50);

    static std::vector<ProjectWorkflowScriptInfo> ListProjectWorkflowScripts(const std::string& rootPath,
                                                                             std::size_t maxItems = 50);

    static bool ResolveProjectWorkflow(const std::string& rootPath,
                                       const std::string& requested,
                                       ProjectWorkflowInfo& outWorkflow,
                                       std::string& outError);

    static bool ResolveProjectWorkflowScript(const std::string& rootPath,
                                             const std::string& requested,
                                             ProjectWorkflowScriptInfo& outScript,
                                             std::string& outError);

    // Projects Phase 2: load the trusted project contract from PROJECT.md.
    // The body is capped so a very large project file cannot crowd out the
    // rest of the chat/tool prompt. outStatus is informational, for example
    // when PROJECT.md is missing, blank, unreadable, or truncated.
    static bool ReadProjectInstructions(const std::string& rootPath,
                                        std::string& outText,
                                        std::string& outStatus,
                                        std::size_t maxChars = 8000);

    // ── Skills ───────────────────────────────────────────────────
    // User-facing Skills are cross-project, reusable abilities that live at
    // %USERPROFILE%\LlamaBoss\Skills.  Internally these still use the
    // GlobalWorkflow function names so existing call sites stay small. Each
    // Skill is a `.workflow.md` instruction contract plus an optional same-
    // stem `.py` helper script.  No PROJECT.md, Sources, Templates, or
    // Outputs scaffolding -- Skills are intentionally lightweight.  Resolution
    // precedence is project-wins when a project is attached, so a project
    // workflow with the same filename shadows a Skill.

    // %USERPROFILE%\LlamaBoss\Skills, with the same home/documents
    // fallbacks used by GetProjectsDir().
    static std::string GetGlobalWorkflowsDir();
    static bool EnsureGlobalWorkflowsRoot();

    // Create a new Skill `.workflow.md` contract and, optionally, a same-stem
    // `.py` helper under %USERPROFILE%\LlamaBoss\Skills.  Returns false if
    // the name is blank or the workflow file cannot be written.
    static bool CreateGlobalWorkflow(const std::string& workflowName,
                                     ProjectWorkflowInfo& outWorkflow,
                                     std::string& outError);

    static bool CreateGlobalWorkflowWithScript(const std::string& workflowName,
                                               ProjectWorkflowInfo& outWorkflow,
                                               ProjectWorkflowScriptInfo& outScript,
                                               std::string& outError);

    static std::vector<ProjectWorkflowInfo> ListGlobalWorkflows(std::size_t maxItems = 50);

    static std::vector<ProjectWorkflowScriptInfo> ListGlobalWorkflowScripts(std::size_t maxItems = 50);

    static bool ResolveGlobalWorkflow(const std::string& requested,
                                      ProjectWorkflowInfo& outWorkflow,
                                      std::string& outError);

    static bool ResolveGlobalWorkflowScript(const std::string& requested,
                                            ProjectWorkflowScriptInfo& outScript,
                                            std::string& outError);

private:
    static std::string SanitizeId(const std::string& name);
};
