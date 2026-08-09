// project_context_builder.cpp
// See project_context_builder.h.  Bodies moved from MyFrame; the only changes
// are the cache field renames (m_projectContextCache -> m_cache,
// ProjectContextCacheSignature -> CacheSignature) and the dependency on
// ChatHistory through the held unique_ptr reference.
#include "project_context_builder.h"

#include <wx/wx.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <system_error>
#include <vector>

#include "chat_history.h"
#include "lb_string_utils.h"   // ProjectSource_HumanBytes
#include "skill_authoring_support.h"   // LbReadSkillFrontmatterDescription

namespace {

// Last-write-time of a path in filesystem ticks, or 0 if missing/unreadable.
// Uses std::filesystem with an error_code so cache probing never triggers
// wxWidgets log popups on Windows paths that cannot expose times.
long long LbPathMTimeTicks(const std::string& path)
{
    if (path.empty()) return 0;

    std::error_code ec;

#ifdef _WIN32
    std::filesystem::path fsPath(wxString::FromUTF8(path).ToStdWstring());
#else
    std::filesystem::path fsPath = std::filesystem::u8path(path);
#endif

    const auto mtime = std::filesystem::last_write_time(fsPath, ec);
    if (ec) return 0;

    return static_cast<long long>(mtime.time_since_epoch().count());
}

// boost::hash_combine-style mixer.  This is only for in-process cache
// invalidation, not persistence or security.
unsigned long long MixHash(unsigned long long seed, unsigned long long value)
{
    return seed ^ (value + 0x9e3779b97f4a7c15ull +
                   (seed << 6) + (seed >> 2));
}

unsigned long long MixString(unsigned long long seed, const std::string& value)
{
    return MixHash(seed,
        static_cast<unsigned long long>(std::hash<std::string>{}(value)));
}

template <typename Items>
unsigned long long FingerprintItems(const Items& items)
{
    unsigned long long h = 1469598103934665603ull;
    h = MixHash(h, static_cast<unsigned long long>(items.size()));

    for (const auto& item : items) {
        // The prompt lists name/path/size, but content-relevant edits often
        // keep the same directory timestamp.  Include file mtime so edited
        // SKILL.md, workflow, source, and helper files invalidate the cached
        // system-prompt context.
        h = MixString(h, item.name);
        h = MixString(h, item.path);
        h = MixHash(h, static_cast<unsigned long long>(item.sizeBytes));
        h = MixHash(h, static_cast<unsigned long long>(LbPathMTimeTicks(item.path)));
    }

    return h;
}

} // namespace

std::string LbSkillDisplayNameFromContractPath(const SkillInfo& skill)
{
    std::string path = skill.path;
    std::replace(path.begin(), path.end(), '\\', '/');

    const size_t contractSlash = path.find_last_of('/');
    if (contractSlash != std::string::npos && contractSlash > 0) {
        const size_t folderSlash = path.find_last_of('/', contractSlash - 1);
        const size_t folderStart = folderSlash == std::string::npos ? 0 : folderSlash + 1;
        const std::string folderName = path.substr(folderStart, contractSlash - folderStart);
        if (!folderName.empty()) return folderName;
    }

    return skill.name;
}

ProjectContextBuilder::ProjectContextBuilder(std::unique_ptr<ChatHistory>& chatHistory)
    : m_chatHistory(chatHistory)
{
}

ProjectContextBuilder::CacheSignature
ProjectContextBuilder::BuildCacheSignature() const
{
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();
    if (m_cache.hasRecentProbe &&
        now - m_cache.recentProbeAt < std::chrono::seconds(2)) {
        return m_cache.recentProbeSig;
    }

    CacheSignature sig;
    sig.hasProject = m_chatHistory->HasProject();
    sig.skillsDirMTime =
        LbPathMTimeTicks(ProjectManager::GetSkillsDir());

    // Directory mtime catches add/remove/rename.  The listed-file
    // fingerprints catch edits to existing files that may not update the
    // parent directory timestamp.  Keep the list cap aligned with
    // AppendSkillsBlock() so the cache only tracks prompt-visible items.
    sig.skillsListHash = FingerprintItems(
        ProjectManager::ListSkills(30));
    sig.skillScriptsListHash = FingerprintItems(
        ProjectManager::ListSkillScripts(30));

    if (sig.hasProject) {
        sig.projectRoot = m_chatHistory->GetProjectRoot();
        sig.projectName = m_chatHistory->GetProjectName();
        sig.projectJsonMTime =
            LbPathMTimeTicks(ProjectManager::ProjectJsonPath(sig.projectRoot));
        sig.projectMdMTime =
            LbPathMTimeTicks(ProjectManager::ProjectInstructionsPath(sig.projectRoot));
        sig.sourcesDirMTime =
            LbPathMTimeTicks(ProjectManager::ProjectSourcesPath(sig.projectRoot));
        sig.sourcesListHash = FingerprintItems(
            ProjectManager::ListProjectSources(sig.projectRoot, 30));
        sig.workflowsDirMTime =
            LbPathMTimeTicks(ProjectManager::ProjectWorkflowsPath(sig.projectRoot));
        sig.workflowsListHash = FingerprintItems(
            ProjectManager::ListProjectWorkflows(sig.projectRoot, 30));
        sig.workflowScriptsListHash = FingerprintItems(
            ProjectManager::ListProjectWorkflowScripts(sig.projectRoot, 30));
    }

    m_cache.recentProbeSig = sig;
    m_cache.recentProbeAt = now;
    m_cache.hasRecentProbe = true;
    return sig;
}

void ProjectContextBuilder::Invalidate() const
{
    // Project identity changes are also caught by BuildCacheSignature();
    // explicit invalidation is kept for same-project content edits and as
    // cheap belt-and-suspenders clarity at project mutation call sites.
    m_cache.valid = false;
    m_cache.hasRecentProbe = false;
    m_cache.hasRecentStripCounts = false;
}

ProjectStripCounts
ProjectContextBuilder::GetProjectStripCounts(const std::string& root) const
{
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();

    if (m_cache.hasRecentStripCounts &&
        m_cache.recentStripCountsRoot == root &&
        now - m_cache.recentStripCountsAt < std::chrono::seconds(2)) {
        return m_cache.recentStripCounts;
    }

    ProjectStripCounts counts;
    counts.sourceCount = static_cast<int>(
        ProjectManager::ListProjectSources(root, 0).size());
    counts.workflowCount = static_cast<int>(
        ProjectManager::ListProjectWorkflows(root, 0).size());
    counts.scriptCount = static_cast<int>(
        ProjectManager::ListProjectWorkflowScripts(root, 0).size());

    m_cache.recentStripCountsRoot = root;
    m_cache.recentStripCounts = counts;
    m_cache.recentStripCountsAt = now;
    m_cache.hasRecentStripCounts = true;
    return counts;
}

void ProjectContextBuilder::AppendSkillsBlock(std::ostringstream& p) const
{
    const auto skills = ProjectManager::ListSkills(30);
    const auto skillScripts = ProjectManager::ListSkillScripts(30);
    if (skills.empty() && skillScripts.empty()) return;

    p << "LlamaBoss Skills (cross-project; available even when no project is attached):\n"
      << "  Folder: " << ProjectManager::GetSkillsDir() << "\n"
      << "  Skill execution grounding rule: when the user asks to use, run, invoke, or continue a named Skill, the first tool call for that Skill request MUST read the listed SKILL.md contract path, even if that Skill was just drafted or used earlier in this same chat. Do not run a Skill helper, PowerShell step, or one-off replacement before reading the saved Skill contract.\n"
      << "  After the saved SKILL.md is read, treat that contract and the saved files in the Skill folder as the source of truth. Do not substitute temporary workspace scripts, design-time test scripts, or remembered helper names from an earlier Skill-design conversation unless the saved SKILL.md explicitly directs that exact file.\n"
      << "  Use a Skill by reading its SKILL.md contract first, then following its steps using normal tools and approval rules. python_run_script can run a Skill's .py helper (same folder or its scripts subfolder) by filename or in-lane path. If the helper needs runtime inputs, put the helper filename/path on the first args line and each optional command-line argument on its own later line; never call python_run_script with only data arguments and no .py script filename/path. Skill helper scripts are files, not tool names: never call the .py filename or Skill name directly as a tool. If an active project has a project workflow with the same name, the project workflow takes precedence.\n";

    if (!skills.empty()) {
        p << "  Skills (read the listed SKILL.md contract path before using one):\n";
        for (const auto& skill : skills) {
            p << "    - " << LbSkillDisplayNameFromContractPath(skill)
              << " contract (" << ProjectSource_HumanBytes(skill.sizeBytes) << ")\n"
              << "      SKILL.md: " << skill.path << "\n";

            // Agent Skills progressive disclosure: surface each skill's
            // frontmatter description so the model can pick the right
            // skill from the list without reading every contract.  The
            // full SKILL.md is still read on use (grounding rule above).
            // Legacy skills without frontmatter return empty and print
            // nothing.  Cache safety: this runs only on context-cache
            // rebuilds, and FingerprintItems already mixes each SKILL.md
            // mtime into the signature, so an edited description
            // invalidates the cached prompt.
            const std::string description =
                LbReadSkillFrontmatterDescription(skill.path);
            if (!description.empty()) {
                p << "      Description: " << description << "\n";
            }
        }
    }

    if (!skillScripts.empty()) {
        p << "  Skill Python helper scripts (run only after reading the matching Skill and only when needed):\n";
        for (const auto& script : skillScripts) {
            p << "    - " << script.name
              << " (" << ProjectSource_HumanBytes(script.sizeBytes) << ")\n"
              << "      " << script.path << "\n";
        }
    }
}

std::string ProjectContextBuilder::BuildActiveProjectContextBlockFresh() const
{
    std::ostringstream p;
    AppendSkillsBlock(p);

    if (!m_chatHistory->HasProject()) return p.str();

    const std::string projectRoot = m_chatHistory->GetProjectRoot();
    const std::string projectMdPath = ProjectManager::ProjectInstructionsPath(projectRoot);

    std::string projectInstructions;
    std::string projectInstructionsStatus;
    const bool loadedProjectInstructions =
        ProjectManager::ReadProjectInstructions(projectRoot, projectInstructions, projectInstructionsStatus);

    p << "Active project:\n"
      << "  Name: " << m_chatHistory->GetProjectName() << "\n"
      << "  Root: " << projectRoot << "\n"
      << "  PROJECT.md: " << projectMdPath << "\n"
      << "  Sources folder: " << ProjectManager::ProjectSourcesPath(projectRoot) << "\n"
      << "  Notes folder: " << projectRoot << "\\Notes\n"
      << "  Project Workflows folder: " << ProjectManager::ProjectWorkflowsPath(projectRoot) << "\n"
      << "  This is a long-lived project folder attached to the current chat. Chat workspace files remain separate from project files.\n"
      << "  PROJECT.md is loaded into this system prompt on every request while the project is attached. Treat it as the project contract for project-related work.\n"
      << "  Project-relative paths are supported by tools for the standard project lanes: Inputs\\, Outputs\\, Workflows\\, Notes\\, Sources\\, Templates\\, PROJECT.md, and project.json resolve under the active project root. Use those short project-relative paths for durable project files instead of writing them to the chat workspace.\n"
      << "  The active project root is an allowed write root for write/mkdir/edit/delete. Other arbitrary relative paths still resolve against the chat workspace, so prefer the standard project lane prefixes above for project files.\n"
      << "  Follow PROJECT.md for project-related requests. For unrelated general questions or casual chat, answer normally and do not force the request into this project.\n"
      << "  Do not invent project sources, templates, workflows, or policies that are not provided. Do not modify PROJECT.md or other project files unless the user explicitly asks.\n"
      << "  Project-aware file use: when the user asks to inspect, summarize, open, extract, report on, or fill a file that appears in Project Sources, use the listed project source path or the source filename with the appropriate read/open/helper tool. Source files are read-only reference inputs; built-in helper artifacts still save to conversation workflow folders unless a workflow or user explicitly asks for a project output path.\n"
      << "  Project workflows: workflow files are reusable Markdown instruction plans in Workflows/. When the user asks to run or use a workflow, read the relevant workflow file first, then follow its steps using normal tools and approval rules. A workflow file is not automatic code execution by itself.\n"
      << "  Project workflow Python scripts: optional .py helper scripts may live in Workflows/. Do not run a project workflow script unless the workflow file or user request calls for it. python_run_script can run an active project's workflow script by filename or in-lane path; if the script needs runtime inputs, put the script filename/path on the first args line and each optional command-line argument on its own later line. read/open/ls/write/edit/delete/mkdir can use Workflows\\... project-relative paths; scripts run from the conversation workspace and can infer the project root from their own file path.\n"
      << "  Project notes: durable project-specific memory lives in Notes/NOTES.md. If the user says save this to my notes while this project is active, use notes_append so the full note is saved to project notes and a compact pointer is saved to global NOTES.md. If the user specifically says project notes, use project_notes_append.\n";

    const auto projectSources = ProjectManager::ListProjectSources(projectRoot, 30);
    if (projectSources.empty()) {
        p << "  Project sources: none attached yet.\n";
    } else {
        p << "  Project source files in Sources/ (names and paths only; use tools/helpers to inspect contents when needed):\n";
        for (const auto& src : projectSources) {
            p << "    - " << src.name
              << " (" << ProjectSource_HumanBytes(src.sizeBytes) << ")\n"
              << "      " << src.path << "\n";
        }
    }

    const auto projectWorkflows = ProjectManager::ListProjectWorkflows(projectRoot, 30);
    if (projectWorkflows.empty()) {
        p << "  Project workflows: none created yet.\n";
    } else {
        p << "  Project workflow files in Workflows/ (read the workflow file before running/using it):\n";
        for (const auto& wf : projectWorkflows) {
            p << "    - " << wf.name
              << " (" << ProjectSource_HumanBytes(wf.sizeBytes) << ")\n"
              << "      " << wf.path << "\n";
        }
    }

    const auto projectWorkflowScripts = ProjectManager::ListProjectWorkflowScripts(projectRoot, 30);
    if (projectWorkflowScripts.empty()) {
        p << "  Project workflow Python scripts: none created yet.\n";
    } else {
        p << "  Project workflow Python scripts in Workflows/ (run only after reading the matching workflow and only when needed):\n";
        for (const auto& script : projectWorkflowScripts) {
            p << "    - " << script.name
              << " (" << ProjectSource_HumanBytes(script.sizeBytes) << ")\n"
              << "      " << script.path << "\n";
        }
    }

    if (loadedProjectInstructions) {
        p << "\nProject contract loaded from PROJECT.md:\n"
          << "--- PROJECT.md START ---\n"
          << projectInstructions;
        if (!projectInstructions.empty() && projectInstructions.back() != '\n') {
            p << "\n";
        }
        p << "--- PROJECT.md END ---\n";

        if (!projectInstructionsStatus.empty()) {
            p << "Project contract note: " << projectInstructionsStatus << "\n";
        }
    } else {
        p << "PROJECT.md was not loaded";
        if (!projectInstructionsStatus.empty()) {
            p << ": " << projectInstructionsStatus;
        }
        p << "\n";
    }

    return p.str();
}

std::string ProjectContextBuilder::BuildActiveProjectContextBlock() const
{
    CacheSignature sig = BuildCacheSignature();
    if (m_cache.valid && m_cache.sig == sig) {
        return m_cache.block;
    }

    m_cache.block = BuildActiveProjectContextBlockFresh();
    m_cache.sig = std::move(sig);
    m_cache.valid = true;
    return m_cache.block;
}
