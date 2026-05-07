#define _CRT_SECURE_NO_WARNINGS

#include "project_manager.h"
#include "path_safety.h"

#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Stringifier.h>
#include <Poco/Timestamp.h>
#include <Poco/DateTimeFormatter.h>
#include <Poco/DateTimeFormat.h>

#include <wx/stdpaths.h>
#include <wx/filename.h>
#include <wx/utils.h>
#include <wx/dir.h>
#include <wx/filefn.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>
#include <utility>

namespace {

std::string JoinProjectPath(const std::string& a, const std::string& b)
{
    if (a.empty()) return b;
    if (a.back() == '/' || a.back() == '\\') return a + b;
    return a + std::string(1, wxFILE_SEP_PATH) + b;
}

std::string LlamaBossUserRootDirForProjects()
{
#ifdef __WXMSW__
    wxString userProfile;
    if (wxGetEnv("USERPROFILE", &userProfile) && !userProfile.IsEmpty()) {
        return JoinProjectPath(std::string(userProfile.ToUTF8().data()), "LlamaBoss");
    }
#endif

    wxString home = wxGetHomeDir();
    if (!home.IsEmpty()) {
        return JoinProjectPath(std::string(home.ToUTF8().data()), "LlamaBoss");
    }

    wxString docs = wxStandardPaths::Get().GetDocumentsDir();
    return JoinProjectPath(std::string(docs.ToUTF8().data()), "LlamaBoss");
}

std::string CurrentIsoTimestamp()
{
    Poco::Timestamp now;
    return Poco::DateTimeFormatter::format(now, Poco::DateTimeFormat::ISO8601_FORMAT);
}

std::string TrimAscii(std::string s)
{
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}


std::string LowerAscii(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string StripMatchingQuotes(std::string s)
{
    s = TrimAscii(std::move(s));
    if (s.size() >= 2 &&
        ((s.front() == '"' && s.back() == '"') ||
         (s.front() == '\'' && s.back() == '\''))) {
        s = s.substr(1, s.size() - 2);
    }
    return TrimAscii(std::move(s));
}

std::string NormalizeSlashPath(std::string s)
{
    std::replace(s.begin(), s.end(), '\\', '/');
    while (s.rfind("./", 0) == 0) s.erase(0, 2);
    return s;
}

std::string StripSourcesPrefix(std::string s)
{
    s = NormalizeSlashPath(StripMatchingQuotes(std::move(s)));
    std::string lower = LowerAscii(s);
    const char* prefixes[] = { "sources/", "source/", "project sources/", "project source/" };
    bool changed = true;
    while (changed) {
        changed = false;
        lower = LowerAscii(s);
        for (const char* prefix : prefixes) {
            const std::string p(prefix);
            if (lower.rfind(p, 0) == 0) {
                s.erase(0, p.size());
                changed = true;
                break;
            }
        }
    }
    return TrimAscii(s);
}

std::string StripWorkflowsPrefix(std::string s)
{
    s = NormalizeSlashPath(StripMatchingQuotes(std::move(s)));
    std::string lower = LowerAscii(s);
    const char* prefixes[] = { "skills/", "skill/", "workflows/", "workflow/", "project workflows/", "project workflow/" };
    bool changed = true;
    while (changed) {
        changed = false;
        lower = LowerAscii(s);
        for (const char* prefix : prefixes) {
            const std::string p(prefix);
            if (lower.rfind(p, 0) == 0) {
                s.erase(0, p.size());
                changed = true;
                break;
            }
        }
    }
    return TrimAscii(s);
}

std::string StripWorkflowScriptPrefix(std::string s)
{
    s = NormalizeSlashPath(StripMatchingQuotes(std::move(s)));
    std::string lower = LowerAscii(s);
    const char* prefixes[] = {
        "skill scripts/", "skill script/", "skills/", "skill/",
        "workflow scripts/", "workflow script/",
        "project workflow scripts/", "project workflow script/",
        "workflows/", "workflow/", "project workflows/", "project workflow/"
    };
    bool changed = true;
    while (changed) {
        changed = false;
        lower = LowerAscii(s);
        for (const char* prefix : prefixes) {
            const std::string p(prefix);
            if (lower.rfind(p, 0) == 0) {
                s.erase(0, p.size());
                changed = true;
                break;
            }
        }
    }
    return TrimAscii(s);
}

std::string FileStemLower(const std::string& fileName)
{
    wxFileName fn(wxString::FromUTF8(fileName));
    return LowerAscii(std::string(fn.GetName().ToUTF8().data()));
}

std::string WorkflowStemLower(const std::string& fileName)
{
    std::string stem = FileStemLower(fileName);
    const std::string suffix = ".workflow";
    if (stem.size() > suffix.size() &&
        stem.compare(stem.size() - suffix.size(), suffix.size(), suffix) == 0) {
        stem.erase(stem.size() - suffix.size());
    }
    return stem;
}

bool HasLowerSuffix(const std::string& s, const std::string& suffix)
{
    const std::string lower = LowerAscii(s);
    return lower.size() >= suffix.size() &&
           lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool WriteUtf8File(const std::string& path, const std::string& body)
{
    std::ofstream f(path_safety::Utf8ToWide(path), std::ios::out | std::ios::trunc | std::ios::binary);
    if (!f.is_open()) return false;
    f.write(body.data(), static_cast<std::streamsize>(body.size()));
    return f.good();
}

bool EnsureSubdir(const std::string& root, const char* name)
{
    std::string path = JoinProjectPath(root, name);
    bool ok = wxFileName::Mkdir(wxString::FromUTF8(path), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    return ok || wxDirExists(wxString::FromUTF8(path));
}

unsigned long long FileSizeBytes(const std::string& path)
{
    std::ifstream f(path_safety::Utf8ToWide(path), std::ios::binary | std::ios::ate);
    if (!f.is_open()) return 0;
    std::streamsize n = f.tellg();
    if (n < 0) return 0;
    return static_cast<unsigned long long>(n);
}

bool IsWorkflowDocFileName(const std::string& fileName);
std::string NormalizeExistingPathForCompare(const std::string& path);

// Scope-agnostic enumerators.  These power ListProjectWorkflows /
// ListProjectWorkflowScripts and the global-scope counterparts, which
// differ only in which directory they walk.
std::vector<ProjectWorkflowInfo> ListWorkflowsInDir(const std::string& workflowsDir,
                                                    std::size_t maxItems)
{
    std::vector<ProjectWorkflowInfo> workflows;
    if (workflowsDir.empty()) return workflows;
    if (!wxDirExists(wxString::FromUTF8(workflowsDir))) return workflows;

    wxDir dir(wxString::FromUTF8(workflowsDir));
    if (!dir.IsOpened()) return workflows;

    wxString name;
    bool cont = dir.GetFirst(&name, wxEmptyString, wxDIR_FILES);
    while (cont) {
        const std::string fileName = std::string(name.ToUTF8().data());
        const std::string lowerName = LowerAscii(fileName);
        const bool isWorkflowDoc = IsWorkflowDocFileName(lowerName);

        if (isWorkflowDoc) {
            const std::string path = JoinProjectPath(workflowsDir, fileName);
            ProjectWorkflowInfo info;
            info.name = fileName;
            info.path = path;
            info.sizeBytes = FileSizeBytes(path);
            workflows.push_back(info);
        }

        cont = dir.GetNext(&name);
    }

    std::sort(workflows.begin(), workflows.end(), [](const ProjectWorkflowInfo& a, const ProjectWorkflowInfo& b) {
        return a.name < b.name;
    });

    if (maxItems > 0 && workflows.size() > maxItems) workflows.resize(maxItems);
    return workflows;
}

std::vector<ProjectWorkflowScriptInfo> ListWorkflowScriptsInDir(const std::string& workflowsDir,
                                                                std::size_t maxItems)
{
    std::vector<ProjectWorkflowScriptInfo> scripts;
    if (workflowsDir.empty()) return scripts;
    if (!wxDirExists(wxString::FromUTF8(workflowsDir))) return scripts;

    wxDir dir(wxString::FromUTF8(workflowsDir));
    if (!dir.IsOpened()) return scripts;

    wxString name;
    bool cont = dir.GetFirst(&name, wxEmptyString, wxDIR_FILES);
    while (cont) {
        const std::string fileName = std::string(name.ToUTF8().data());
        if (HasLowerSuffix(fileName, ".py")) {
            const std::string path = JoinProjectPath(workflowsDir, fileName);
            ProjectWorkflowScriptInfo info;
            info.name = fileName;
            info.path = path;
            info.sizeBytes = FileSizeBytes(path);
            scripts.push_back(info);
        }
        cont = dir.GetNext(&name);
    }

    std::sort(scripts.begin(), scripts.end(), [](const ProjectWorkflowScriptInfo& a, const ProjectWorkflowScriptInfo& b) {
        return a.name < b.name;
    });

    if (maxItems > 0 && scripts.size() > maxItems) scripts.resize(maxItems);
    return scripts;
}

std::string LegacyGlobalWorkflowsDir()
{
    // Pre-Skills builds stored global reusable workflow files directly in
    // LlamaBoss\Workflows, mixed beside chat_<id> conversation folders.
    return JoinProjectPath(LlamaBossUserRootDirForProjects(), "Workflows");
}

bool FileOrDirExists(const std::string& path)
{
    const wxString wxPath = wxString::FromUTF8(path);
    return wxFileExists(wxPath) || wxDirExists(wxPath);
}

bool IsWorkflowDocFileName(const std::string& fileName)
{
    return HasLowerSuffix(fileName, ".workflow.md");
}

std::string UniqueWorkflowStem(const std::string& dir,
                               const std::string& baseName,
                               bool createPythonScript)
{
    for (int i = 1; i < 10000; ++i) {
        std::ostringstream stem;
        stem << baseName;
        if (i > 1) stem << " (" << i << ")";

        const std::string candidateStem = stem.str();
        const std::string workflowPath = JoinProjectPath(dir, candidateStem + ".workflow.md");
        const std::string scriptPath = JoinProjectPath(dir, candidateStem + ".py");

        if (!FileOrDirExists(workflowPath) &&
            (!createPythonScript || !FileOrDirExists(scriptPath))) {
            return candidateStem;
        }
    }

    std::ostringstream fallback;
    fallback << baseName << " (10000)";
    return fallback.str();
}

void MoveLegacySkillFilesIfNeeded(const std::string& skillsDir)
{
    const std::string legacyDir = LegacyGlobalWorkflowsDir();
    if (legacyDir.empty() || skillsDir.empty()) return;
    if (NormalizeExistingPathForCompare(legacyDir) == NormalizeExistingPathForCompare(skillsDir)) return;
    if (!wxDirExists(wxString::FromUTF8(legacyDir))) return;

    wxDir dir(wxString::FromUTF8(legacyDir));
    if (!dir.IsOpened()) return;

    wxString name;
    bool cont = dir.GetFirst(&name, wxEmptyString, wxDIR_FILES);
    while (cont) {
        const std::string fileName = std::string(name.ToUTF8().data());
        const bool shouldMove = IsWorkflowDocFileName(fileName) || HasLowerSuffix(fileName, ".py");
        if (shouldMove) {
            const std::string src = JoinProjectPath(legacyDir, fileName);
            const std::string dst = JoinProjectPath(skillsDir, fileName);
            if (!FileOrDirExists(dst)) {
                wxRenameFile(wxString::FromUTF8(src), wxString::FromUTF8(dst), false);
            }
        }
        cont = dir.GetNext(&name);
    }
}

std::string UniqueProjectFilePath(const std::string& dir, const std::string& requestedName)
{
    std::string safeName = path_safety::SanitizeFilename(requestedName, "source_file");
    wxFileName fn(wxString::FromUTF8(safeName));

    std::string stem = std::string(fn.GetName().ToUTF8().data());
    std::string ext = std::string(fn.GetExt().ToUTF8().data());
    if (stem.empty()) stem = "source_file";

    auto build = [&](int suffix) {
        std::ostringstream name;
        name << stem;
        if (suffix > 1) name << " (" << suffix << ")";
        if (!ext.empty()) name << "." << ext;
        return JoinProjectPath(dir, name.str());
    };

    for (int i = 1; i < 10000; ++i) {
        std::string candidate = build(i);
        if (!wxFileExists(wxString::FromUTF8(candidate)) &&
            !wxDirExists(wxString::FromUTF8(candidate))) {
            return candidate;
        }
    }

    return build(10000);
}


std::string NormalizeExistingPathForCompare(const std::string& path)
{
    wxFileName fn(wxString::FromUTF8(path));
    fn.Normalize(wxPATH_NORM_ABSOLUTE | wxPATH_NORM_DOTS | wxPATH_NORM_TILDE);
    std::string out = std::string(fn.GetFullPath().ToUTF8().data());
    std::replace(out.begin(), out.end(), '\\', '/');
    while (out.size() > 1 && out.back() == '/') out.pop_back();
#ifdef __WXMSW__
    out = LowerAscii(out);
#endif
    return out;
}

bool IsDirectChildOfProjectsDir(const std::string& rootPath)
{
    const std::string projectsDir = NormalizeExistingPathForCompare(ProjectManager::GetProjectsDir());
    const std::string root = NormalizeExistingPathForCompare(rootPath);
    if (projectsDir.empty() || root.empty() || root == projectsDir) return false;
    const std::string prefix = projectsDir + "/";
    if (root.rfind(prefix, 0) != 0) return false;

    // In Phase 1 project folders are direct children of Projects/. Refuse
    // nested paths here so a bad project.json cannot point deletion at an
    // arbitrary subfolder elsewhere.
    const std::string tail = root.substr(prefix.size());
    return !tail.empty() && tail.find('/') == std::string::npos;
}

} // namespace

std::string ProjectManager::GetProjectsDir()
{
    return JoinProjectPath(LlamaBossUserRootDirForProjects(), "Projects");
}

bool ProjectManager::EnsureProjectsRoot()
{
    std::string dir = GetProjectsDir();
    bool ok = wxFileName::Mkdir(wxString::FromUTF8(dir), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    return ok || wxDirExists(wxString::FromUTF8(dir));
}

std::string ProjectManager::GetGlobalWorkflowsDir()
{
    // User-facing name: Skills.  The existing API names stay as
    // GlobalWorkflow internally to keep the patch small and compatible.
    return JoinProjectPath(LlamaBossUserRootDirForProjects(), "Skills");
}

bool ProjectManager::EnsureGlobalWorkflowsRoot()
{
    std::string dir = GetGlobalWorkflowsDir();
    bool ok = wxFileName::Mkdir(wxString::FromUTF8(dir), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    const bool exists = ok || wxDirExists(wxString::FromUTF8(dir));
    if (exists) {
        MoveLegacySkillFilesIfNeeded(dir);
    }
    return exists;
}

std::string ProjectManager::ProjectJsonPath(const std::string& rootPath)
{
    return JoinProjectPath(rootPath, "project.json");
}

std::string ProjectManager::ProjectInstructionsPath(const std::string& rootPath)
{
    return JoinProjectPath(rootPath, "PROJECT.md");
}

std::string ProjectManager::ProjectSourcesPath(const std::string& rootPath)
{
    return JoinProjectPath(rootPath, "Sources");
}

std::string ProjectManager::ProjectWorkflowsPath(const std::string& rootPath)
{
    return JoinProjectPath(rootPath, "Workflows");
}

std::string ProjectManager::SanitizeId(const std::string& name)
{
    std::string id;
    bool lastUnderscore = false;

    for (unsigned char raw : name) {
        char c = static_cast<char>(raw);
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');

        const bool ok = (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9');
        if (ok) {
            id.push_back(c);
            lastUnderscore = false;
        } else if (!lastUnderscore) {
            id.push_back('_');
            lastUnderscore = true;
        }
    }

    while (!id.empty() && id.front() == '_') id.erase(id.begin());
    while (!id.empty() && id.back() == '_') id.pop_back();
    if (id.empty()) id = "project";
    return id;
}

bool ProjectManager::CreateProject(const std::string& name,
                                   ProjectInfo& outProject,
                                   std::string& outError)
{
    outProject = ProjectInfo{};
    outError.clear();

    const std::string trimmedName = TrimAscii(name);
    if (trimmedName.empty()) {
        outError = "Project name cannot be blank.";
        return false;
    }

    if (!EnsureProjectsRoot()) {
        outError = "Could not create the LlamaBoss Projects folder.";
        return false;
    }

    const std::string folderName = path_safety::SanitizeFilename(trimmedName, "Project");
    const std::string root = JoinProjectPath(GetProjectsDir(), folderName);
    if (wxDirExists(wxString::FromUTF8(root)) || wxFileExists(wxString::FromUTF8(root))) {
        outError = "A project folder with that name already exists.";
        return false;
    }

    bool rootOk = wxFileName::Mkdir(wxString::FromUTF8(root), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    if (!rootOk && !wxDirExists(wxString::FromUTF8(root))) {
        outError = "Could not create the project folder.";
        return false;
    }

    const char* kSubdirs[] = { "Sources", "Templates", "Notes", "Outputs", "Workflows" };
    for (const char* subdir : kSubdirs) {
        if (!EnsureSubdir(root, subdir)) {
            outError = std::string("Could not create project subfolder: ") + subdir;
            return false;
        }
    }

    const std::string now = CurrentIsoTimestamp();
    ProjectInfo info;
    info.id = SanitizeId(trimmedName);
    info.name = trimmedName;
    info.rootPath = root;
    info.createdAt = now;
    info.updatedAt = now;

    Poco::JSON::Object::Ptr json = new Poco::JSON::Object(true);
    json->set("version", 1);
    json->set("id", info.id);
    json->set("name", info.name);
    json->set("root_path", info.rootPath);
    json->set("created_at", info.createdAt);
    json->set("updated_at", info.updatedAt);

    std::ostringstream jsonBody;
    Poco::JSON::Stringifier::stringify(json, jsonBody, 2);
    if (!WriteUtf8File(ProjectJsonPath(root), jsonBody.str())) {
        outError = "Could not write project.json.";
        return false;
    }

    std::ostringstream md;
    md << "# " << info.name << "\n\n"
       << "This file is the project contract. When this project is attached to a chat, "
       << "LlamaBoss loads this file into the model context and uses it for project-related requests.\n\n"
       << "## Purpose\n\n"
       << "Describe what this project is for.\n\n"
       << "## Project Rules\n\n"
       << "- Answer normal unrelated questions normally; do not force every request into this project.\n"
       << "- Follow these instructions when the user's request is related to this project.\n"
       << "- Do not invent source files, templates, policies, or workflows that are not provided.\n"
       << "- Do not modify project files unless the user explicitly asks.\n\n"
       << "## Sources\n\n"
       << "Place long-lived reference files in `Sources/`.\n\n"
       << "## Templates\n\n"
       << "Place reusable forms, prompts, and templates in `Templates/`.\n\n"
       << "## Outputs\n\n"
       << "Project outputs can be saved in `Outputs/` later. Conversation-generated artifacts still save to chat workflow folders unless a workflow says otherwise.\n\n"
       << "## Workflows\n\n"
       << "Reusable project workflows live in `Workflows/` as Markdown `.workflow.md` files. A workflow is a repeatable instruction plan; tools and approvals still work normally.\n\n"
       << "## Notes\n\n"
       << "Durable project notes can live in `Notes/` in a later phase.\n";

    if (!WriteUtf8File(ProjectInstructionsPath(root), md.str())) {
        outError = "Could not write PROJECT.md.";
        return false;
    }

    outProject = info;
    return true;
}

bool ProjectManager::CopyFilesToProjectSources(const std::string& rootPath,
                                                 const std::vector<std::string>& sourcePaths,
                                                 std::vector<ProjectSourceInfo>& outCopied,
                                                 std::vector<std::string>& outSkipped,
                                                 std::string& outError)
{
    outCopied.clear();
    outSkipped.clear();
    outError.clear();

    if (rootPath.empty() || !wxDirExists(wxString::FromUTF8(rootPath))) {
        outError = "The active project folder does not exist.";
        return false;
    }

    const std::string sourcesDir = ProjectSourcesPath(rootPath);
    bool sourcesOk = wxFileName::Mkdir(wxString::FromUTF8(sourcesDir), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    if (!sourcesOk && !wxDirExists(wxString::FromUTF8(sourcesDir))) {
        outError = "Could not create the project Sources folder.";
        return false;
    }

    for (const std::string& src : sourcePaths) {
        if (src.empty() || !wxFileExists(wxString::FromUTF8(src))) {
            outSkipped.push_back(src.empty() ? std::string("<blank path>") : src);
            continue;
        }

        wxFileName srcFn(wxString::FromUTF8(src));
        const std::string srcName = std::string(srcFn.GetFullName().ToUTF8().data());
        const std::string dest = UniqueProjectFilePath(sourcesDir, srcName);

        if (!wxCopyFile(wxString::FromUTF8(src), wxString::FromUTF8(dest), false)) {
            outSkipped.push_back(src);
            continue;
        }

        wxFileName destFn(wxString::FromUTF8(dest));
        ProjectSourceInfo info;
        info.name = std::string(destFn.GetFullName().ToUTF8().data());
        info.path = dest;
        info.sizeBytes = FileSizeBytes(dest);
        outCopied.push_back(info);
    }

    if (outCopied.empty() && !sourcePaths.empty()) {
        outError = "No files were copied into the project Sources folder.";
        return false;
    }

    return true;
}

std::vector<ProjectSourceInfo> ProjectManager::ListProjectSources(const std::string& rootPath,
                                                                  std::size_t maxItems)
{
    std::vector<ProjectSourceInfo> sources;
    const std::string sourcesDir = ProjectSourcesPath(rootPath);
    if (!wxDirExists(wxString::FromUTF8(sourcesDir))) return sources;

    wxDir dir(wxString::FromUTF8(sourcesDir));
    if (!dir.IsOpened()) return sources;

    wxString name;
    bool cont = dir.GetFirst(&name, wxEmptyString, wxDIR_FILES);
    while (cont) {
        const std::string fileName = std::string(name.ToUTF8().data());
        const std::string path = JoinProjectPath(sourcesDir, fileName);

        ProjectSourceInfo info;
        info.name = fileName;
        info.path = path;
        info.sizeBytes = FileSizeBytes(path);
        sources.push_back(info);

        cont = dir.GetNext(&name);
    }

    std::sort(sources.begin(), sources.end(), [](const ProjectSourceInfo& a, const ProjectSourceInfo& b) {
        return a.name < b.name;
    });

    if (maxItems > 0 && sources.size() > maxItems) {
        sources.resize(maxItems);
    }
    return sources;
}

bool ProjectManager::ResolveProjectSource(const std::string& rootPath,
                                         const std::string& requested,
                                         ProjectSourceInfo& outSource,
                                         std::string& outError)
{
    outSource = ProjectSourceInfo{};
    outError.clear();

    std::string query = StripSourcesPrefix(requested);
    if (query.empty()) {
        outError = "Project source reference is blank.";
        return false;
    }

    // Keep this resolver scoped to the project Sources folder. Absolute paths
    // and cwd-relative paths should continue to be handled by the normal tool
    // path resolver before callers fall back to this function.
    if (query.find(':') != std::string::npos ||
        query.rfind("//", 0) == 0 || query.rfind("/", 0) == 0) {
        outError = "Not a project source reference.";
        return false;
    }

    const auto sources = ListProjectSources(rootPath, 0);
    if (sources.empty()) {
        outError = "This project has no source files in Sources/.";
        return false;
    }

    const std::string qLower = LowerAscii(NormalizeSlashPath(query));
    std::vector<ProjectSourceInfo> matches;

    auto addMatch = [&](const ProjectSourceInfo& src) {
        for (const auto& existing : matches) {
            if (LowerAscii(existing.path) == LowerAscii(src.path)) return;
        }
        matches.push_back(src);
    };

    // 1) Exact filename match, case-insensitive. This is the safest and most
    // common path: zayra.pdf -> Sources/zayra.pdf.
    for (const auto& src : sources) {
        if (LowerAscii(src.name) == qLower) addMatch(src);
    }
    if (matches.size() == 1) { outSource = matches.front(); return true; }
    if (matches.size() > 1) {
        outError = "Project source reference is ambiguous: " + query;
        return false;
    }

    // 2) Exact normalized relative tail match. Useful if the caller passes
    // Sources/name.ext or a future nested Sources path.
    for (const auto& src : sources) {
        const std::string sourceRel = LowerAscii(NormalizeSlashPath(src.name));
        if (sourceRel == qLower) addMatch(src);
    }
    if (matches.size() == 1) { outSource = matches.front(); return true; }
    if (matches.size() > 1) {
        outError = "Project source reference is ambiguous: " + query;
        return false;
    }

    // 3) Exact stem match. Capture -> Capture.PNG; zayra -> zayra.pdf.
    for (const auto& src : sources) {
        if (FileStemLower(src.name) == qLower) addMatch(src);
    }
    if (matches.size() == 1) { outSource = matches.front(); return true; }
    if (matches.size() > 1) {
        outError = "Project source reference is ambiguous: " + query;
        return false;
    }

    // 4) Unique partial filename/stem match. Helpful for natural language tool
    // calls but still refuses ambiguity.
    for (const auto& src : sources) {
        const std::string nameLower = LowerAscii(src.name);
        const std::string stemLower = FileStemLower(src.name);
        if (nameLower.find(qLower) != std::string::npos ||
            stemLower.find(qLower) != std::string::npos) {
            addMatch(src);
        }
    }
    if (matches.size() == 1) { outSource = matches.front(); return true; }
    if (matches.size() > 1) {
        outError = "Project source reference is ambiguous: " + query;
        return false;
    }

    outError = "No matching project source found in Sources/: " + query;
    return false;
}


namespace {

bool CreateWorkflowInternal(const std::string& workflowsDir,
                            bool               isGlobal,
                            const std::string& workflowName,
                            bool               createPythonScript,
                            ProjectWorkflowInfo& outWorkflow,
                            ProjectWorkflowScriptInfo* outScript,
                            std::string& outError)
{
    outWorkflow = ProjectWorkflowInfo{};
    if (outScript) *outScript = ProjectWorkflowScriptInfo{};
    outError.clear();

    const std::string trimmedName = TrimAscii(workflowName);
    if (trimmedName.empty()) {
        outError = isGlobal ? "Skill name cannot be blank." : "Workflow name cannot be blank.";
        return false;
    }

    if (workflowsDir.empty()) {
        outError = isGlobal
            ? std::string("Could not resolve the LlamaBoss Skills folder.")
            : std::string("The active project folder does not exist.");
        return false;
    }

    bool workflowsOk = wxFileName::Mkdir(wxString::FromUTF8(workflowsDir), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    if (!workflowsOk && !wxDirExists(wxString::FromUTF8(workflowsDir))) {
        outError = isGlobal
            ? std::string("Could not create the LlamaBoss Skills folder.")
            : std::string("Could not create the project Workflows folder.");
        return false;
    }

    std::string baseName = path_safety::SanitizeFilename(trimmedName, "workflow");
    wxFileName baseFn(wxString::FromUTF8(baseName));
    baseName = std::string(baseFn.GetName().ToUTF8().data());
    if (baseName.empty()) baseName = "workflow";

    const std::string uniqueStem = UniqueWorkflowStem(workflowsDir, baseName, createPythonScript);
    const std::string workflowPath = JoinProjectPath(workflowsDir, uniqueStem + ".workflow.md");

    std::string scriptName;
    std::string scriptPath;
    if (createPythonScript) {
        scriptName = uniqueStem + ".py";
        scriptPath = JoinProjectPath(workflowsDir, scriptName);
    }

    std::ostringstream md;
    md << "# " << trimmedName << (isGlobal ? " Skill" : " Workflow") << "\n\n";
    if (isGlobal) {
        md << "This file defines a reusable LlamaBoss Skill. A Skill is available across all "
              "conversations whether or not a project is attached. When the user asks to run or "
              "use this Skill, LlamaBoss should read this file first, then follow the steps "
              "using normal tools/approval rules. If a project is attached and that project has a "
              "Skill file with the same name, the project workflow takes precedence.\n\n";
    } else {
        md << "This file is a reusable project workflow. When the user asks to run or use this workflow, "
              "LlamaBoss should read this file first, then follow the steps using the active project context, "
              "PROJECT.md, Project Sources, and normal tools/approval rules.\n\n";
    }
    md << "## Trigger Phrases\n\n"
       << "- Run the " << trimmedName << (isGlobal ? " skill\n" : " workflow\n")
       << "- Use the " << trimmedName << (isGlobal ? " skill\n" : " workflow\n");
    if (isGlobal) {
        md << "- Run the " << trimmedName << " workflow\n";
    }
    md << "\n## Inputs to Ask For\n\n"
       << "- List the details this " << (isGlobal ? "Skill" : "workflow") << " needs from the user.\n\n"
       << "## Steps\n\n";
    if (isGlobal) {
        md << "1. Read this Skill file before doing anything else.\n"
           << "2. Use the appropriate LlamaBoss tools for each step.\n";
    } else {
        md << "1. Confirm the active project and read relevant project sources if needed.\n"
           << "2. Use the appropriate LlamaBoss tools for each step.\n";
    }

    if (createPythonScript) {
        md << "3. If the helper script is needed, run `python_run_script " << scriptName << "`.\n"
           << "4. Present generated files as artifact cards.\n"
           << "5. Summarize what was created and where it was saved.\n\n";
    } else {
        md << "3. Present generated files as artifact cards.\n"
           << "4. Summarize what was created and where it was saved.\n\n";
    }

    md << "## Output Expectations\n\n"
       << "- Describe the final artifact or answer this workflow should produce.\n\n";

    if (createPythonScript) {
        md << "## Optional Python Script\n\n"
           << "- Helper script: `" << scriptName << "`\n"
           << (isGlobal ? "- Read this Skill before running the script.\n" : "- Read this workflow before running the script.\n")
           << (isGlobal ? "- Run it only when the Skill steps call for it or the user asks to run this Skill.\n" : "- Run it only when the workflow steps call for it or the user asks to run this workflow.\n");
        if (isGlobal) {
            md << "- The script runs from the current conversation workspace. It can infer the LlamaBoss Skills folder from its own file path.\n";
        } else {
            md << "- The script runs from the current conversation workspace. It can infer the project root from its own file path.\n";
        }
        md << "- Normal LlamaBoss safety and approval rules still apply.\n\n";
    }

    md << "## Safety / Approval Notes\n\n"
       << "- Use LlamaBoss approval cards for writes, edits, deletes, Python runs, or other mutating actions.\n";
    if (isGlobal) {
        md << "- Do not modify project files unless the user explicitly asks. Treat any active project as read-only unless the Skill explicitly says otherwise.\n";
    } else {
        md << "- Do not modify project files unless the user explicitly asks.\n";
    }

    if (!WriteUtf8File(workflowPath, md.str())) {
        outError = "Could not write workflow file.";
        return false;
    }

    if (createPythonScript) {
        std::ostringstream py;
        py << "\"\"\"Reusable helper script for this "
           << (isGlobal ? "Skill" : "project workflow")
           << ".\n\n";
        if (isGlobal) {
            py << "LlamaBoss runs Skill helper scripts from the current conversation workspace.\n"
               << "This script can infer its Skills folder from its own file location.\n";
        } else {
            py << "LlamaBoss runs project workflow scripts from the current conversation workspace.\n"
               << "This script can infer its project folders from its own file location.\n";
        }
        py << "Edit this file to implement the repeatable workflow action.\n\"\"\"\n\n"
           << "from pathlib import Path\n\n"
           << "SCRIPT_PATH = Path(__file__).resolve()\n"
           << "WORKFLOWS_DIR = SCRIPT_PATH.parent\n";
        if (isGlobal) {
            py << "LLAMABOSS_USER_ROOT = WORKFLOWS_DIR.parent\n"
               << "PROJECTS_ROOT = LLAMABOSS_USER_ROOT / \"Projects\"\n";
        } else {
            py << "PROJECT_ROOT = WORKFLOWS_DIR.parent\n"
               << "SOURCES_DIR = PROJECT_ROOT / \"Sources\"\n"
               << "TEMPLATES_DIR = PROJECT_ROOT / \"Templates\"\n"
               << "OUTPUTS_DIR = PROJECT_ROOT / \"Outputs\"\n";
        }
        py << "CHAT_WORKSPACE = Path.cwd()\n\n"
           << "def main() -> None:\n"
           << "    print(\"Workflow helper script placeholder\")\n";
        if (isGlobal) {
            py << "    print(f\"Skills folder: {WORKFLOWS_DIR}\")\n"
               << "    print(f\"LlamaBoss user root: {LLAMABOSS_USER_ROOT}\")\n";
        } else {
            py << "    print(f\"Project root: {PROJECT_ROOT}\")\n"
               << "    print(f\"Sources folder: {SOURCES_DIR}\")\n"
               << "    print(f\"Templates folder: {TEMPLATES_DIR}\")\n"
               << "    print(f\"Outputs folder: {OUTPUTS_DIR}\")\n";
        }
        py << "    print(f\"Conversation workspace: {CHAT_WORKSPACE}\")\n"
           << "    print(\"Edit this script to implement the workflow.\")\n\n"
           << "if __name__ == \"__main__\":\n"
           << "    main()\n";

        if (!WriteUtf8File(scriptPath, py.str())) {
            outError = "Workflow file was created, but the Python script could not be written.";
            return false;
        }

        if (outScript) {
            wxFileName sfn(wxString::FromUTF8(scriptPath));
            outScript->name = std::string(sfn.GetFullName().ToUTF8().data());
            outScript->path = scriptPath;
            outScript->sizeBytes = FileSizeBytes(scriptPath);
        }
    }

    wxFileName fn(wxString::FromUTF8(workflowPath));
    outWorkflow.name = std::string(fn.GetFullName().ToUTF8().data());
    outWorkflow.path = workflowPath;
    outWorkflow.sizeBytes = FileSizeBytes(workflowPath);
    return true;
}

// Project-scope wrapper -- preserves the prior contract (rootPath in,
// project Workflows/ folder used) on top of the scope-agnostic helper.
bool CreateProjectWorkflowInternal(const std::string& rootPath,
                                   const std::string& workflowName,
                                   bool createPythonScript,
                                   ProjectWorkflowInfo& outWorkflow,
                                   ProjectWorkflowScriptInfo* outScript,
                                   std::string& outError)
{
    if (rootPath.empty() || !wxDirExists(wxString::FromUTF8(rootPath))) {
        outError = "The active project folder does not exist.";
        if (outScript) *outScript = ProjectWorkflowScriptInfo{};
        outWorkflow = ProjectWorkflowInfo{};
        return false;
    }
    return CreateWorkflowInternal(ProjectManager::ProjectWorkflowsPath(rootPath),
                                  /*isGlobal=*/false,
                                  workflowName,
                                  createPythonScript,
                                  outWorkflow,
                                  outScript,
                                  outError);
}

// Scope-agnostic workflow resolver.  Matches by exact filename, then
// exact stem (with or without the ".workflow" suffix), then by unique
// case-insensitive substring -- the same 4-stage match used by the
// per-project resolver, hoisted so the global lane can share it.
// |kindLabel| is the user-facing scope name in error strings, e.g.
// "project workflow" or "Skill".
bool ResolveWorkflowInList(const std::vector<ProjectWorkflowInfo>& workflows,
                           const std::string& query,
                           const std::string& kindLabel,
                           ProjectWorkflowInfo& outWorkflow,
                           std::string& outError)
{
    outWorkflow = ProjectWorkflowInfo{};
    outError.clear();

    if (workflows.empty()) {
        outError = "No matching " + kindLabel + " found.";
        return false;
    }

    const std::string qLower = LowerAscii(NormalizeSlashPath(query));
    std::vector<ProjectWorkflowInfo> matches;

    auto addMatch = [&](const ProjectWorkflowInfo& wf) {
        for (const auto& existing : matches) {
            if (LowerAscii(existing.path) == LowerAscii(wf.path)) return;
        }
        matches.push_back(wf);
    };

    for (const auto& wf : workflows) {
        if (LowerAscii(wf.name) == qLower) addMatch(wf);
    }
    if (matches.size() == 1) { outWorkflow = matches.front(); return true; }
    if (matches.size() > 1) { outError = kindLabel + " reference is ambiguous: " + query; return false; }

    for (const auto& wf : workflows) {
        if (WorkflowStemLower(wf.name) == qLower || FileStemLower(wf.name) == qLower) addMatch(wf);
    }
    if (matches.size() == 1) { outWorkflow = matches.front(); return true; }
    if (matches.size() > 1) { outError = kindLabel + " reference is ambiguous: " + query; return false; }

    for (const auto& wf : workflows) {
        const std::string nameLower = LowerAscii(wf.name);
        const std::string stemLower = FileStemLower(wf.name);
        const std::string workflowStemLower = WorkflowStemLower(wf.name);
        if (nameLower.find(qLower) != std::string::npos ||
            stemLower.find(qLower) != std::string::npos ||
            workflowStemLower.find(qLower) != std::string::npos) {
            addMatch(wf);
        }
    }
    if (matches.size() == 1) { outWorkflow = matches.front(); return true; }
    if (matches.size() > 1) { outError = kindLabel + " reference is ambiguous: " + query; return false; }

    outError = "No matching " + kindLabel + " found: " + query;
    return false;
}

// Same idea for .py helper scripts.
bool ResolveWorkflowScriptInList(const std::vector<ProjectWorkflowScriptInfo>& scripts,
                                 const std::string& query,
                                 const std::string& kindLabel,
                                 ProjectWorkflowScriptInfo& outScript,
                                 std::string& outError)
{
    outScript = ProjectWorkflowScriptInfo{};
    outError.clear();

    if (scripts.empty()) {
        outError = "No matching " + kindLabel + " found.";
        return false;
    }

    const std::string qLower = LowerAscii(NormalizeSlashPath(query));
    const std::string qStemLower = FileStemLower(query);
    std::vector<ProjectWorkflowScriptInfo> matches;

    auto addMatch = [&](const ProjectWorkflowScriptInfo& script) {
        for (const auto& existing : matches) {
            if (LowerAscii(existing.path) == LowerAscii(script.path)) return;
        }
        matches.push_back(script);
    };

    for (const auto& script : scripts) {
        if (LowerAscii(script.name) == qLower) addMatch(script);
    }
    if (matches.size() == 1) { outScript = matches.front(); return true; }
    if (matches.size() > 1) { outError = kindLabel + " reference is ambiguous: " + query; return false; }

    for (const auto& script : scripts) {
        if (FileStemLower(script.name) == qStemLower) addMatch(script);
    }
    if (matches.size() == 1) { outScript = matches.front(); return true; }
    if (matches.size() > 1) { outError = kindLabel + " reference is ambiguous: " + query; return false; }

    for (const auto& script : scripts) {
        const std::string nameLower = LowerAscii(script.name);
        const std::string stemLower = FileStemLower(script.name);
        if (nameLower.find(qLower) != std::string::npos ||
            stemLower.find(qStemLower) != std::string::npos) {
            addMatch(script);
        }
    }
    if (matches.size() == 1) { outScript = matches.front(); return true; }
    if (matches.size() > 1) { outError = kindLabel + " reference is ambiguous: " + query; return false; }

    outError = "No matching " + kindLabel + " found: " + query;
    return false;
}

} // namespace

bool ProjectManager::CreateProjectWorkflow(const std::string& rootPath,
                                           const std::string& workflowName,
                                           ProjectWorkflowInfo& outWorkflow,
                                           std::string& outError)
{
    return CreateProjectWorkflowInternal(rootPath, workflowName, false, outWorkflow, nullptr, outError);
}

bool ProjectManager::CreateProjectWorkflowWithScript(const std::string& rootPath,
                                                     const std::string& workflowName,
                                                     ProjectWorkflowInfo& outWorkflow,
                                                     ProjectWorkflowScriptInfo& outScript,
                                                     std::string& outError)
{
    return CreateProjectWorkflowInternal(rootPath, workflowName, true, outWorkflow, &outScript, outError);
}

std::vector<ProjectWorkflowInfo> ProjectManager::ListProjectWorkflows(const std::string& rootPath,
                                                                      std::size_t maxItems)
{
    return ListWorkflowsInDir(ProjectWorkflowsPath(rootPath), maxItems);
}

bool ProjectManager::ResolveProjectWorkflow(const std::string& rootPath,
                                            const std::string& requested,
                                            ProjectWorkflowInfo& outWorkflow,
                                            std::string& outError)
{
    outWorkflow = ProjectWorkflowInfo{};
    outError.clear();

    std::string query = StripWorkflowsPrefix(requested);
    if (query.empty()) {
        outError = "Project workflow reference is blank.";
        return false;
    }

    if (query.find(':') != std::string::npos ||
        query.rfind("//", 0) == 0 || query.rfind("/", 0) == 0) {
        outError = "Not a project workflow reference.";
        return false;
    }

    const auto workflows = ListProjectWorkflows(rootPath, 0);
    if (workflows.empty()) {
        outError = "This project has no workflow files in Workflows/.";
        return false;
    }

    return ResolveWorkflowInList(workflows, query, "project workflow", outWorkflow, outError);
}


std::vector<ProjectWorkflowScriptInfo> ProjectManager::ListProjectWorkflowScripts(const std::string& rootPath,
                                                                                  std::size_t maxItems)
{
    return ListWorkflowScriptsInDir(ProjectWorkflowsPath(rootPath), maxItems);
}

bool ProjectManager::ResolveProjectWorkflowScript(const std::string& rootPath,
                                                  const std::string& requested,
                                                  ProjectWorkflowScriptInfo& outScript,
                                                  std::string& outError)
{
    outScript = ProjectWorkflowScriptInfo{};
    outError.clear();

    std::string query = StripWorkflowScriptPrefix(requested);
    if (query.empty()) {
        outError = "Project workflow script reference is blank.";
        return false;
    }

    if (query.find(':') != std::string::npos ||
        query.rfind("//", 0) == 0 || query.rfind("/", 0) == 0) {
        outError = "Not a project workflow script reference.";
        return false;
    }

    if (query.find('/') != std::string::npos || query.find('\\') != std::string::npos) {
        outError = "Project workflow script reference must be a filename, not a nested path.";
        return false;
    }

    if (query.find('.') == std::string::npos) query += ".py";
    if (!HasLowerSuffix(query, ".py")) {
        outError = "Project workflow scripts must be .py files.";
        return false;
    }

    const auto scripts = ListProjectWorkflowScripts(rootPath, 0);
    if (scripts.empty()) {
        outError = "This project has no Python Skill scripts in Workflows/.";
        return false;
    }

    return ResolveWorkflowScriptInList(scripts, query, "project workflow script", outScript, outError);
}

// ── Skills public API ─────────────────────────────────────────────
// User-facing Skills are the global reusable lane.  Internally this
// still uses the existing GlobalWorkflow function names to keep the
// patch small, but files now live under %USERPROFILE%\LlamaBoss\Skills.

bool ProjectManager::CreateGlobalWorkflow(const std::string& workflowName,
                                          ProjectWorkflowInfo& outWorkflow,
                                          std::string& outError)
{
    if (!EnsureGlobalWorkflowsRoot()) {
        outError = "Could not create the LlamaBoss Skills folder.";
        outWorkflow = ProjectWorkflowInfo{};
        return false;
    }
    return CreateWorkflowInternal(GetGlobalWorkflowsDir(),
                                  /*isGlobal=*/true,
                                  workflowName,
                                  /*createPythonScript=*/false,
                                  outWorkflow,
                                  nullptr,
                                  outError);
}

bool ProjectManager::CreateGlobalWorkflowWithScript(const std::string& workflowName,
                                                    ProjectWorkflowInfo& outWorkflow,
                                                    ProjectWorkflowScriptInfo& outScript,
                                                    std::string& outError)
{
    if (!EnsureGlobalWorkflowsRoot()) {
        outError = "Could not create the LlamaBoss Skills folder.";
        outWorkflow = ProjectWorkflowInfo{};
        outScript = ProjectWorkflowScriptInfo{};
        return false;
    }
    return CreateWorkflowInternal(GetGlobalWorkflowsDir(),
                                  /*isGlobal=*/true,
                                  workflowName,
                                  /*createPythonScript=*/true,
                                  outWorkflow,
                                  &outScript,
                                  outError);
}

std::vector<ProjectWorkflowInfo> ProjectManager::ListGlobalWorkflows(std::size_t maxItems)
{
    EnsureGlobalWorkflowsRoot();
    return ListWorkflowsInDir(GetGlobalWorkflowsDir(), maxItems);
}

std::vector<ProjectWorkflowScriptInfo> ProjectManager::ListGlobalWorkflowScripts(std::size_t maxItems)
{
    EnsureGlobalWorkflowsRoot();
    return ListWorkflowScriptsInDir(GetGlobalWorkflowsDir(), maxItems);
}

bool ProjectManager::ResolveGlobalWorkflow(const std::string& requested,
                                           ProjectWorkflowInfo& outWorkflow,
                                           std::string& outError)
{
    outWorkflow = ProjectWorkflowInfo{};
    outError.clear();

    std::string query = StripWorkflowsPrefix(requested);
    if (query.empty()) {
        outError = "Skill reference is blank.";
        return false;
    }

    if (query.find(':') != std::string::npos ||
        query.rfind("//", 0) == 0 || query.rfind("/", 0) == 0) {
        outError = "Not a Skill reference.";
        return false;
    }

    const auto workflows = ListGlobalWorkflows(0);
    if (workflows.empty()) {
        outError = "There are no Skill files in LlamaBoss\\Skills.";
        return false;
    }

    return ResolveWorkflowInList(workflows, query, "Skill", outWorkflow, outError);
}

bool ProjectManager::ResolveGlobalWorkflowScript(const std::string& requested,
                                                 ProjectWorkflowScriptInfo& outScript,
                                                 std::string& outError)
{
    outScript = ProjectWorkflowScriptInfo{};
    outError.clear();

    std::string query = StripWorkflowScriptPrefix(requested);
    if (query.empty()) {
        outError = "Skill script reference is blank.";
        return false;
    }

    if (query.find(':') != std::string::npos ||
        query.rfind("//", 0) == 0 || query.rfind("/", 0) == 0) {
        outError = "Not a Skill script reference.";
        return false;
    }

    if (query.find('/') != std::string::npos || query.find('\\') != std::string::npos) {
        outError = "Skill script reference must be a filename, not a nested path.";
        return false;
    }

    if (query.find('.') == std::string::npos) query += ".py";
    if (!HasLowerSuffix(query, ".py")) {
        outError = "Skill scripts must be .py files.";
        return false;
    }

    const auto scripts = ListGlobalWorkflowScripts(0);
    if (scripts.empty()) {
        outError = "There are no Python Skill scripts in LlamaBoss\\Skills.";
        return false;
    }

    return ResolveWorkflowScriptInList(scripts, query, "Skill script", outScript, outError);
}

bool ProjectManager::ReadProjectInstructions(const std::string& rootPath,
                                             std::string& outText,
                                             std::string& outStatus,
                                             std::size_t maxChars)
{
    outText.clear();
    outStatus.clear();

    const std::string path = ProjectInstructionsPath(rootPath);
    if (!wxFileExists(wxString::FromUTF8(path))) {
        outStatus = "PROJECT.md is missing.";
        return false;
    }

    std::ifstream f(path_safety::Utf8ToWide(path), std::ios::in | std::ios::binary);
    if (!f.is_open()) {
        outStatus = "PROJECT.md could not be opened.";
        return false;
    }

    std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    // Strip UTF-8 BOM if a user edited the file in an editor that writes one.
    if (body.size() >= 3 &&
        static_cast<unsigned char>(body[0]) == 0xEF &&
        static_cast<unsigned char>(body[1]) == 0xBB &&
        static_cast<unsigned char>(body[2]) == 0xBF) {
        body.erase(0, 3);
    }

    if (TrimAscii(body).empty()) {
        outStatus = "PROJECT.md is blank.";
        return false;
    }

    if (maxChars == 0) maxChars = 8000;
    if (body.size() > maxChars) {
        outText = body.substr(0, maxChars);
        outStatus = "PROJECT.md was truncated to the first " + std::to_string(maxChars) + " characters.";
        return true;
    }

    outText = body;
    return true;
}

bool ProjectManager::LoadProjectByRoot(const std::string& rootPath,
                                       ProjectInfo& outProject)
{
    outProject = ProjectInfo{};
    std::ifstream f(path_safety::Utf8ToWide(ProjectJsonPath(rootPath)), std::ios::in | std::ios::binary);
    if (!f.is_open()) return false;

    std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (body.empty()) return false;

    try {
        Poco::JSON::Parser parser;
        auto result = parser.parse(body);
        auto json = result.extract<Poco::JSON::Object::Ptr>();

        ProjectInfo info;
        if (json->has("id"))         info.id = json->getValue<std::string>("id");
        if (json->has("name"))       info.name = json->getValue<std::string>("name");
        if (json->has("root_path"))  info.rootPath = json->getValue<std::string>("root_path");
        if (json->has("created_at")) info.createdAt = json->getValue<std::string>("created_at");
        if (json->has("updated_at")) info.updatedAt = json->getValue<std::string>("updated_at");

        if (info.rootPath.empty()) info.rootPath = rootPath;
        if (info.id.empty()) info.id = SanitizeId(info.name.empty() ? rootPath : info.name);
        if (info.name.empty()) {
            wxFileName fn(wxString::FromUTF8(rootPath));
            info.name = std::string(fn.GetName().ToUTF8().data());
        }

        outProject = info;
        return !outProject.name.empty() && !outProject.rootPath.empty();
    } catch (...) {
        return false;
    }
}

std::vector<ProjectInfo> ProjectManager::ListProjects()
{
    std::vector<ProjectInfo> projects;
    if (!EnsureProjectsRoot()) return projects;

    wxDir dir(wxString::FromUTF8(GetProjectsDir()));
    if (!dir.IsOpened()) return projects;

    wxString name;
    bool cont = dir.GetFirst(&name, wxEmptyString, wxDIR_DIRS);
    while (cont) {
        const std::string root = JoinProjectPath(GetProjectsDir(), std::string(name.ToUTF8().data()));
        ProjectInfo info;
        if (LoadProjectByRoot(root, info)) {
            projects.push_back(info);
        }
        cont = dir.GetNext(&name);
    }

    std::sort(projects.begin(), projects.end(), [](const ProjectInfo& a, const ProjectInfo& b) {
        return a.name < b.name;
    });
    return projects;
}

bool ProjectManager::LoadProjectById(const std::string& id,
                                     ProjectInfo& outProject)
{
    const auto projects = ListProjects();
    for (const auto& p : projects) {
        if (p.id == id) {
            outProject = p;
            return true;
        }
    }
    outProject = ProjectInfo{};
    return false;
}


bool ProjectManager::DeleteProject(const ProjectInfo& project,
                                   std::string& outError)
{
    outError.clear();

    if (project.rootPath.empty()) {
        outError = "Project root path is blank.";
        return false;
    }

    if (!IsDirectChildOfProjectsDir(project.rootPath)) {
        outError = "Refusing to delete project because its folder is not a direct child of the LlamaBoss Projects folder.";
        return false;
    }

    if (!wxDirExists(wxString::FromUTF8(project.rootPath))) {
        outError = "Project folder no longer exists.";
        return false;
    }

    // Extra guard: only delete folders that still look like LlamaBoss projects.
    if (!wxFileExists(wxString::FromUTF8(ProjectJsonPath(project.rootPath)))) {
        outError = "Refusing to delete this folder because it does not contain project.json.";
        return false;
    }

    if (!wxFileName::Rmdir(wxString::FromUTF8(project.rootPath), wxPATH_RMDIR_RECURSIVE)) {
        outError = "Could not delete the project folder. It may be open in another program.";
        return false;
    }

    return true;
}
