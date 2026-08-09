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

// ── Skill type aliases ───────────────────────────────────────────
// Skills and project workflows share the same on-disk shape
// (a named folder/file with optional helper .py), so they share
// the underlying struct. The aliases below let Skill call sites
// read naturally without churning the project-workflow code path.
//
// TODO: eventually rename the underlying structs to a neutral
// name (e.g. WorkflowEntry / WorkflowScriptEntry) and keep both
// ProjectWorkflowInfo and SkillInfo as aliases for source-compat.
using SkillInfo       = ProjectWorkflowInfo;
using SkillScriptInfo = ProjectWorkflowScriptInfo;

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
    // %USERPROFILE%\LlamaBoss\Skills.  Skills follow the Agent Skills
    // standard layout: each Skill is a kebab-case folder containing a
    // `SKILL.md` contract that opens with a YAML frontmatter block
    // (name + description), plus optional helper `.py` scripts under a
    // `scripts` subfolder.  Legacy skills (no frontmatter, same-folder
    // helpers) remain fully supported by the listers/resolvers.  No
    // PROJECT.md, Sources, Templates, or Outputs scaffolding -- Skills
    // are intentionally lightweight.  Resolution precedence is
    // project-wins when a project is attached, so a project workflow
    // with the same filename shadows a Skill.
    //
    // Note: the underlying struct types are still named ProjectWorkflowInfo
    // and ProjectWorkflowScriptInfo (shared with the per-project workflow
    // lane).  Use the SkillInfo / SkillScriptInfo aliases declared at the
    // top of this header in new code.  A handful of private helpers in
    // project_manager.cpp (CreateWorkflowInternal, ListWorkflowsInDir,
    // MigrateFlatWorkflowsToFolders, etc.) also keep the generic
    // "Workflow" name because they are reused across both lanes.

    // %USERPROFILE%\LlamaBoss\Skills, with the same home/documents
    // fallbacks used by GetProjectsDir().
    static std::string GetSkillsDir();
    static bool EnsureSkillsRoot();

    // Create a new Skill folder (kebab-case stem) with a starter
    // `SKILL.md` contract under %USERPROFILE%\LlamaBoss\Skills.  No stub
    // `.py` is pre-created: the Skill draft builder decides whether a
    // helper ships and writes it into the scripts subfolder at
    // draft-save time.  Returns false if the name is blank or the Skill
    // file cannot be written.  (The old CreateSkillWithScript variant is
    // retired along with the two-entry creation menu.)
    static bool CreateSkill(const std::string& skillName,
                            SkillInfo& outSkill,
                            std::string& outError);

    static std::vector<SkillInfo> ListSkills(std::size_t maxItems = 50);

    static std::vector<SkillScriptInfo> ListSkillScripts(std::size_t maxItems = 50);

    static bool ResolveSkill(const std::string& requested,
                             SkillInfo& outSkill,
                             std::string& outError);

    static bool ResolveSkillScript(const std::string& requested,
                                   SkillScriptInfo& outScript,
                                   std::string& outError);

    // ── Skill import ─────────────────────────────────────────────
    // Load an external Agent Skills folder (SKILL.md + optional
    // scripts/references/assets) into %USERPROFILE%\LlamaBoss\Skills.
    // Two-phase so the UI can confirm with the user between validation
    // and the copy: ProbeSkillImportFolder validates and reads metadata
    // without writing anything; ImportSkillFolder re-validates and
    // performs the copy.  The destination folder stem is the authored
    // frontmatter `name` (kebab-cased), falling back to the source
    // folder name, with the Skills-lane "-N" collision suffix.  After
    // the copy, the destination SKILL.md is normalized: a frontmatter
    // block is synthesized if missing, and its `name:` is aligned to
    // the final folder stem.  Imported skills carry no special
    // execution rights -- helper runs stay behind the normal
    // python_run_script approval tier.
    struct SkillImportProbe {
        std::string proposedName;               // kebab stem, pre-collision
        std::string finalName;                  // stem after collision suffix
                                                // (advisory preview; the
                                                // import recomputes it)
        std::string description;                // frontmatter description ("" if none)
        std::size_t fileCount = 0;              // files that would be copied
        unsigned long long totalBytes = 0;      // bytes that would be copied
        bool hasFrontmatter = false;            // authored name or description found
    };

    static bool ProbeSkillImportFolder(const std::string& sourceFolder,
                                       SkillImportProbe& outProbe,
                                       std::string& outError);

    static bool ImportSkillFolder(const std::string& sourceFolder,
                                  SkillInfo& outSkill,
                                  std::string& outError);

    // Extract a Skill .zip into a fresh temp folder and locate the Skill
    // folder inside it (SKILL.md at the archive root, or inside a single
    // top-level folder).  Entry names are validated against zip-slip
    // (no absolute paths, drive letters, or ".." components) and the
    // extraction enforces the same depth/count/byte caps as the folder
    // walker, so an oversized or hostile archive fails fast.  On success
    // the caller runs the normal probe/confirm/import against
    // outSkillFolder, then ALWAYS removes outTempRoot (also on cancel).
    static bool ExtractSkillZipToTemp(const std::string& zipPath,
                                      std::string& outTempRoot,
                                      std::string& outSkillFolder,
                                      std::string& outError);

    // Zip one Skill folder (identified by its SKILL.md contract path) to
    // zipPath, rooted as "<stem>/..." inside the archive so it re-imports
    // and shares cleanly.  Uses the same bounded walker as import, so the
    // junk-dir skips and caps apply to exports too.
    static bool ExportSkillToZip(const std::string& skillContractPath,
                                 const std::string& zipPath,
                                 std::string& outError);

private:
    static std::string SanitizeId(const std::string& name);
};
