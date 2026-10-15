// project_context_builder.h
// Builds the "active project" context block injected into the system prompt,
// plus the cross-project Skills and User notes sections, and caches both against a content
// fingerprint so rapid agent iterations do not re-stat the project tree every
// turn.  Extracted from MyFrame; the only live frame state it reads is the
// current ChatHistory's project identity.
//
// The same brief cache also memoizes the source/workflow/script counts shown
// in the project status strip, since many call sites refresh the strip and
// would otherwise re-list the project tree on the UI thread.
#pragma once

#include <chrono>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "project_manager.h"   // ProjectManager (static API), SkillInfo
#include "tool_notes.h"        // NoteFileInfo

class ChatHistory;

// Derives a friendly Skill display name from a SKILL.md contract path (the
// containing folder name, falling back to the listed name).  Lives here with
// the Skills-rendering logic; the frame's Skill picker shares it.
std::string LbSkillDisplayNameFromContractPath(const SkillInfo& skill);

// Source/workflow/script counts for the project status strip.  Kept at file
// scope (not nested) so existing call sites in the frame compile unchanged.
struct ProjectStripCounts {
    int sourceCount = 0;
    int workflowCount = 0;
    int scriptCount = 0;
};

class ProjectContextBuilder
{
public:
    explicit ProjectContextBuilder(std::unique_ptr<ChatHistory>& chatHistory);

    // Cached project-context block for the system prompt.  Recomputed only
    // when the content fingerprint changes.
    std::string BuildActiveProjectContextBlock() const;

    // Emits the optional "LlamaBoss Skills" header section when at least one
    // Skill contract or helper script exists.  Runs regardless of whether a
    // project is attached — Skills are cross-project by design.
    void AppendSkillsBlock(std::ostringstream& p) const;

    // Brief-cached (2s) counts for the status strip.
    ProjectStripCounts GetProjectStripCounts(const std::string& root) const;

    // Drops cached block / probe / strip-count state.  Called at project and
    // Skill mutation sites for same-project content edits.
    void Invalidate() const;

private:
    struct CacheSignature {
        bool        hasProject = false;
        std::string projectRoot;
        std::string projectName;
        long long   skillsDirMTime = 0;
        unsigned long long skillsListHash = 0;
        long long   notesDirMTime = 0;
        unsigned long long notesListHash = 0;
        long long   legacyNotesMTime = 0;
        long long   projectJsonMTime = 0;
        long long   projectMdMTime = 0;
        long long   sourcesDirMTime = 0;
        unsigned long long sourcesListHash = 0;
        long long   workflowsDirMTime = 0;
        unsigned long long workflowsListHash = 0;
        unsigned long long workflowScriptsListHash = 0;

        bool operator==(const CacheSignature& o) const
        {
            return hasProject == o.hasProject &&
                   projectRoot == o.projectRoot &&
                   projectName == o.projectName &&
                   skillsDirMTime == o.skillsDirMTime &&
                   skillsListHash == o.skillsListHash &&
                   notesDirMTime == o.notesDirMTime &&
                   notesListHash == o.notesListHash &&
                   legacyNotesMTime == o.legacyNotesMTime &&
                   projectJsonMTime == o.projectJsonMTime &&
                   projectMdMTime == o.projectMdMTime &&
                   sourcesDirMTime == o.sourcesDirMTime &&
                   sourcesListHash == o.sourcesListHash &&
                   workflowsDirMTime == o.workflowsDirMTime &&
                   workflowsListHash == o.workflowsListHash &&
                   workflowScriptsListHash == o.workflowScriptsListHash;
        }
    };

    // One directory listing per probe, shared by the cache signature, the
    // prompt block and the status-strip counts (these used to list the same
    // folders up to four times per rebuild).  Lists are uncapped; prompt
    // rendering applies its own 30-item caps.
    struct Inventory {
        std::vector<SkillInfo>                 skills;
        std::vector<NoteFileInfo>              notes;
        std::string                            projectRoot;   // empty = no project
        std::vector<ProjectSourceInfo>         sources;
        std::vector<ProjectWorkflowInfo>       workflows;
        std::vector<ProjectWorkflowScriptInfo> workflowScripts;
    };

    struct Cache {
        bool valid = false;
        CacheSignature sig;
        std::string block;

        // Signature probing hits several directories and visible project
        // files.  Cache the probe briefly so rapid agent iterations do not
        // re-stat the same project/Skill lists multiple times per second.
        // Explicit invalidations still clear this and force a fresh probe.
        bool hasRecentProbe = false;
        CacheSignature recentProbeSig;
        Inventory      recentInventory;
        std::chrono::steady_clock::time_point recentProbeAt{};

        // The project strip is refreshed from many call sites.  Reuse exact
        // display counts briefly so refresh churn does not repeatedly
        // materialize full project source/workflow lists on the UI thread.
        bool hasRecentStripCounts = false;
        std::string recentStripCountsRoot;
        ProjectStripCounts recentStripCounts;
        std::chrono::steady_clock::time_point recentStripCountsAt{};
    };

    CacheSignature BuildCacheSignature() const;   // also refreshes m_cache.recentInventory
    std::string    BuildActiveProjectContextBlockFresh(const Inventory& inv) const;
    void           AppendSkillsBlockFrom(std::ostringstream& p,
                                         const std::vector<SkillInfo>& skills) const;

    std::unique_ptr<ChatHistory>& m_chatHistory;
    mutable Cache                 m_cache;
};
