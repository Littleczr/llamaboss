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
#include <wx/wfstream.h>   // wxFFileInputStream/OutputStream (skill zip import/export)
#include <wx/zipstrm.h>    // wxZipInputStream/OutputStream (skill zip import/export)

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <sstream>
#include <utility>

#include "skill_authoring_support.h"   // frontmatter read/ensure/rewrite for skill import

#ifdef __WXMSW__
#include <windows.h>   // GetFileAttributesW — reparse-point check in skill import
#endif

namespace {

// Per-lane instruction-file names.  Global Skills use SKILL.md and keep it
// pure, so external / standard SKILL.md skills can be imported cleanly later.
// Per-project workflows use WORKFLOW.md so the two lanes never collide on
// disk and a project workflow is never mistaken for a Skill.
const char* const kSkillDocName    = "SKILL.md";
const char* const kWorkflowDocName = "WORKFLOW.md";

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
bool FileOrDirExists(const std::string& path);
std::string NormalizeExistingPathForCompare(const std::string& path);

// Scope-agnostic enumerators.  These power ListProjectWorkflows /
// ListProjectWorkflowScripts and the global-scope counterparts, which
// differ only in which directory they walk and which instruction-doc
// name they expect (`docName`): WORKFLOW.md for project workflows,
// SKILL.md for global Skills.
// Phase 2 layout: each skill / workflow is a subfolder containing its
// instruction doc (plus optional .py scripts and other supporting files).
// This helper walks the subfolders of `workflowsDir` and returns one
// ProjectWorkflowInfo per folder that has `docName` inside.
//
// The display `name` stays in the legacy `<stem>.workflow.md` shape so
// ResolveWorkflowInList's fuzzy matching (which checks full name, the
// .workflow.md-stripped stem, and the .ext-stripped stem) keeps working
// without changes.  The on-disk `path` points at the real instruction
// doc so callers that read or open the file land in the right place.
//
// Idempotent against a transitional state where the folder exists but
// `docName` is missing -- such folders are silently skipped.
std::vector<ProjectWorkflowInfo> ListWorkflowsInDir(const std::string& workflowsDir,
                                                    const std::string& docName,
                                                    std::size_t maxItems)
{
    std::vector<ProjectWorkflowInfo> workflows;
    if (workflowsDir.empty()) return workflows;
    if (!wxDirExists(wxString::FromUTF8(workflowsDir))) return workflows;

    wxDir dir(wxString::FromUTF8(workflowsDir));
    if (!dir.IsOpened()) return workflows;

    wxString name;
    bool cont = dir.GetFirst(&name, wxEmptyString, wxDIR_DIRS);
    while (cont) {
        const std::string folderName = std::string(name.ToUTF8().data());
        const std::string folderPath = JoinProjectPath(workflowsDir, folderName);
        const std::string docPath = JoinProjectPath(folderPath, docName);

        if (FileOrDirExists(docPath)) {
            ProjectWorkflowInfo info;
            // Synthetic "<stem>.workflow.md" display name keeps the
            // fuzzy resolver working with no changes -- it already
            // strips the .workflow.md suffix when matching.
            info.name = folderName + ".workflow.md";
            info.path = docPath;
            info.sizeBytes = FileSizeBytes(docPath);
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

// Phase 2 layout: helper .py scripts live INSIDE each skill folder,
// alongside the instruction doc.  This helper iterates entry folders and reports
// every .py file found within them.  The display `name` is the bare
// filename ("skill_test.py"), matching the legacy contract so the
// fuzzy resolver doesn't need updating.  Path collisions across
// different skill folders are tolerated -- the resolver flags them
// as ambiguous; in practice a skill author should keep script names
// unique enough to be self-identifying.
std::vector<ProjectWorkflowScriptInfo> ListWorkflowScriptsInDir(const std::string& workflowsDir,
                                                                const std::string& docName,
                                                                std::size_t maxItems)
{
    std::vector<ProjectWorkflowScriptInfo> scripts;
    if (workflowsDir.empty()) return scripts;
    if (!wxDirExists(wxString::FromUTF8(workflowsDir))) return scripts;

    wxDir dir(wxString::FromUTF8(workflowsDir));
    if (!dir.IsOpened()) return scripts;

    // Flat top-level *.py files are first-class runnable scripts.
    // python_create_script in a project chat writes DIRECTLY into the
    // project's Workflows folder (tool_router: "create reusable
    // workflow scripts directly in the active project's Workflows
    // folder"), and models also drop scripts there with
    // write/overwrite_file.  Before this scan, the resolver only saw
    // per-workflow FOLDERS, so a freshly created flat script was
    // unfindable by bare name or Workflows\<name>.py — the creator and
    // the resolver disagreed about the on-disk layout (observed
    // 2026-08-03: create → run → "not found" → PowerShell
    // copy-to-Scripts workaround, repeated after every edit).  The
    // flat lane has no instruction-doc requirement: a runnable .py is
    // the whole contract.
    {
        wxString fileName;
        bool moreFiles = dir.GetFirst(&fileName, wxEmptyString, wxDIR_FILES);
        while (moreFiles) {
            const std::string fileUtf8 = std::string(fileName.ToUTF8().data());
            if (HasLowerSuffix(fileUtf8, ".py")) {
                const std::string path = JoinProjectPath(workflowsDir, fileUtf8);
                ProjectWorkflowScriptInfo info;
                info.name = fileUtf8;
                info.path = path;
                info.sizeBytes = FileSizeBytes(path);
                scripts.push_back(info);
            }
            moreFiles = dir.GetNext(&fileName);
        }
    }

    wxString folderName;
    bool cont = dir.GetFirst(&folderName, wxEmptyString, wxDIR_DIRS);
    while (cont) {
        const std::string folderUtf8 = std::string(folderName.ToUTF8().data());
        const std::string folderPath = JoinProjectPath(workflowsDir, folderUtf8);

        // Skip subfolders that aren't proper entries (no instruction doc).
        const std::string docPath = JoinProjectPath(folderPath, docName);
        if (!FileOrDirExists(docPath)) {
            cont = dir.GetNext(&folderName);
            continue;
        }

        wxDir inner(wxString::FromUTF8(folderPath));
        if (inner.IsOpened()) {
            wxString fileName;
            bool innerCont = inner.GetFirst(&fileName, wxEmptyString, wxDIR_FILES);
            while (innerCont) {
                const std::string fileUtf8 = std::string(fileName.ToUTF8().data());
                if (HasLowerSuffix(fileUtf8, ".py")) {
                    const std::string path = JoinProjectPath(folderPath, fileUtf8);
                    ProjectWorkflowScriptInfo info;
                    info.name = fileUtf8;
                    info.path = path;
                    info.sizeBytes = FileSizeBytes(path);
                    scripts.push_back(info);
                }
                innerCont = inner.GetNext(&fileName);
            }
        }

        // Also enumerate the conventional scripts\ subfolder.  Larger
        // skills keep their helpers there (Skills\runPod\scripts\
        // runpod_ssh.py); before this scan, those helpers were invisible
        // to bare-name and Skills\-prefixed resolution and could only be
        // run through an absolute path (observed 2026-08-03).  info.name
        // stays the bare filename so resolution-by-name is unchanged;
        // same-named scripts across skills surface as the existing
        // "ambiguous" error rather than a silent pick.
        {
            const std::string scriptsSubdir =
                JoinProjectPath(folderPath, "scripts");
            if (wxDirExists(wxString::FromUTF8(scriptsSubdir))) {
                wxDir scriptsDir(wxString::FromUTF8(scriptsSubdir));
                if (scriptsDir.IsOpened()) {
                    wxString fileName;
                    bool moreFiles = scriptsDir.GetFirst(
                        &fileName, wxEmptyString, wxDIR_FILES);
                    while (moreFiles) {
                        const std::string fileUtf8 =
                            std::string(fileName.ToUTF8().data());
                        if (HasLowerSuffix(fileUtf8, ".py")) {
                            const std::string path =
                                JoinProjectPath(scriptsSubdir, fileUtf8);
                            ProjectWorkflowScriptInfo info;
                            info.name = fileUtf8;
                            info.path = path;
                            info.sizeBytes = FileSizeBytes(path);
                            scripts.push_back(info);
                        }
                        moreFiles = scriptsDir.GetNext(&fileName);
                    }
                }
            }
        }

        cont = dir.GetNext(&folderName);
    }

    std::sort(scripts.begin(), scripts.end(), [](const ProjectWorkflowScriptInfo& a, const ProjectWorkflowScriptInfo& b) {
        return a.name < b.name;
    });

    if (maxItems > 0 && scripts.size() > maxItems) scripts.resize(maxItems);
    return scripts;
}

// LegacyGlobalWorkflowsDir: the function name intentionally keeps the
// historical "GlobalWorkflows" wording because the folder it returns is
// literally named "Workflows" on disk in pre-Skills installs. Used only
// by MoveLegacySkillFilesIfNeeded() during one-time migration into the
// new LlamaBoss\Skills root. Do not rename this.
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
                               bool kebabSuffix)
{
    // Phase 2 layout: each skill lives in its own folder, so the
    // collision check is "does <dir>/<stem>/ already exist?" rather
    // than the old "does <dir>/<stem>.workflow.md already exist?".
    // kebabSuffix (Skills lane) makes the collision suffix "-2"
    // instead of " (2)" so skill folder names stay valid Agent Skills
    // kebab-case names; project workflows keep the legacy suffix.
    for (int i = 1; i < 10000; ++i) {
        std::ostringstream stem;
        stem << baseName;
        if (i > 1) {
            if (kebabSuffix) stem << "-" << i;
            else             stem << " (" << i << ")";
        }

        const std::string candidateStem = stem.str();
        const std::string folderPath = JoinProjectPath(dir, candidateStem);

        if (!FileOrDirExists(folderPath)) {
            return candidateStem;
        }
    }

    std::ostringstream fallback;
    fallback << baseName;
    if (kebabSuffix) fallback << "-10000";
    else             fallback << " (10000)";
    return fallback.str();
}

// Agent Skills `name` rule: lowercase letters, digits, and hyphens.
// Applied to the Skills lane only, at creation time, after
// SanitizeFilename has already removed filesystem-illegal characters.
// Legacy skill folders keep their existing names; the listers and
// resolvers do not require kebab-case.
std::string KebabCaseAsciiStem(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (char ch : in) {
        const unsigned char u = static_cast<unsigned char>(ch);
        if ((u >= 'a' && u <= 'z') || (u >= '0' && u <= '9')) {
            out.push_back(ch);
        } else if (u >= 'A' && u <= 'Z') {
            out.push_back(static_cast<char>(u - 'A' + 'a'));
        } else {
            // Spaces, underscores, dots, and any other separator all
            // collapse to a single hyphen.
            if (!out.empty() && out.back() != '-') out.push_back('-');
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    // Agent Skills caps `name` at 64 chars.
    if (out.size() > 64) {
        out.resize(64);
        while (!out.empty() && out.back() == '-') out.pop_back();
    }
    if (out.empty()) out = "skill";
    return out;
}

// ── Skill import walk ─────────────────────────────────────────────
// Bounded recursive walk shared by ProbeSkillImportFolder (counting)
// and ImportSkillFolder (copying).  Deliberately conservative: skips
// reparse points / junctions (a linked folder must not smuggle content
// from elsewhere on disk into Skills), skips VCS/cache junk, and caps
// depth, file count, and total bytes so picking the wrong folder (a
// repo root, a Downloads dir) fails fast with a clear error instead of
// copying gigabytes.

constexpr int                kSkillImportMaxDepth    = 8;
constexpr std::size_t        kSkillImportMaxFiles    = 400;
constexpr unsigned long long kSkillImportMaxBytes    = 64ull * 1024 * 1024;

bool IsReparsePointOrSymlink(const std::string& path)
{
#ifdef __WXMSW__
    const DWORD attrs =
        ::GetFileAttributesW(path_safety::Utf8ToWide(path).c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;
    return (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    (void)path;
    return false;
#endif
}

bool SkillImportSkipsDirName(const std::string& name)
{
    const std::string lower = LowerAscii(name);
    return lower == ".git" || lower == ".vs" || lower == "__pycache__" ||
           lower == "node_modules" || lower == ".venv" || lower == "venv";
}

// Walk `dirPath` recursively.  relPrefix is "" at the root and grows
// "sub\" per level.  onDir/onFile may be null (probe mode).  Returns
// false with outError set on any cap violation or callback failure.
bool WalkSkillImportTree(
    const std::string& dirPath,
    const std::string& relPrefix,
    int depth,
    std::size_t& fileCount,
    unsigned long long& totalBytes,
    const std::function<bool(const std::string& relDir)>& onDir,
    const std::function<bool(const std::string& absFile,
                             const std::string& relFile)>& onFile,
    std::string& outError)
{
    if (depth > kSkillImportMaxDepth) {
        outError = "Skill folder is nested deeper than " +
                   std::to_string(kSkillImportMaxDepth) +
                   " levels; this does not look like a Skill folder.";
        return false;
    }

    wxDir dir(wxString::FromUTF8(dirPath));
    if (!dir.IsOpened()) {
        outError = "Could not open folder: " + dirPath;
        return false;
    }

    wxString name;
    bool cont = dir.GetFirst(&name, wxEmptyString, wxDIR_FILES);
    while (cont) {
        const std::string fileUtf8 = std::string(name.ToUTF8().data());
        const std::string absFile = JoinProjectPath(dirPath, fileUtf8);
        if (!IsReparsePointOrSymlink(absFile)) {
            ++fileCount;
            totalBytes += FileSizeBytes(absFile);
            if (fileCount > kSkillImportMaxFiles) {
                outError = "Skill folder has more than " +
                           std::to_string(kSkillImportMaxFiles) +
                           " files; this does not look like a Skill folder.";
                return false;
            }
            if (totalBytes > kSkillImportMaxBytes) {
                outError = "Skill folder is larger than 64 MB; this does "
                           "not look like a Skill folder.";
                return false;
            }
            if (onFile && !onFile(absFile, relPrefix + fileUtf8)) {
                outError = "Could not copy file: " + absFile;
                return false;
            }
        }
        cont = dir.GetNext(&name);
    }

    cont = dir.GetFirst(&name, wxEmptyString, wxDIR_DIRS);
    while (cont) {
        const std::string dirUtf8 = std::string(name.ToUTF8().data());
        const std::string absDir = JoinProjectPath(dirPath, dirUtf8);
        if (!SkillImportSkipsDirName(dirUtf8) &&
            !IsReparsePointOrSymlink(absDir)) {
            const std::string relDir = relPrefix + dirUtf8;
            if (onDir && !onDir(relDir)) {
                outError = "Could not create folder: " + relDir;
                return false;
            }
            if (!WalkSkillImportTree(absDir,
                                     relDir + std::string(1, wxFILE_SEP_PATH),
                                     depth + 1,
                                     fileCount,
                                     totalBytes,
                                     onDir,
                                     onFile,
                                     outError)) {
                return false;
            }
        }
        cont = dir.GetNext(&name);
    }

    return true;
}

// Leaf folder name of a directory path ("C:\x\My Skill" -> "My Skill").
std::string LeafFolderName(const std::string& dirPath)
{
    std::string p = dirPath;
    while (!p.empty() && (p.back() == '\\' || p.back() == '/')) p.pop_back();
    const size_t sep = p.find_last_of("\\/");
    return sep == std::string::npos ? p : p.substr(sep + 1);
}

// ── Skill zip entry validation ────────────────────────────────────
// Zip entry names come from the archive, i.e. from outside the trust
// boundary.  Normalize backslashes (Windows-made archives use them) and
// reject anything that could escape the extraction root: absolute
// paths, drive letters, empty/dot/dot-dot components, or names deeper
// than the import depth cap.  On success, outRelPath is the cleaned
// forward-slash relative path.
bool SkillZipEntryNameIsSafe(const std::string& rawName,
                             std::string& outRelPath)
{
    outRelPath.clear();
    if (rawName.empty()) return false;

    std::string name = rawName;
    std::replace(name.begin(), name.end(), '\\', '/');

    if (name.front() == '/') return false;              // absolute
    if (name.find(':') != std::string::npos) return false; // drive / ADS

    // Component-wise validation.
    int depth = 0;
    size_t pos = 0;
    while (pos <= name.size()) {
        const size_t sep = name.find('/', pos);
        const std::string part =
            name.substr(pos, sep == std::string::npos
                                 ? std::string::npos
                                 : sep - pos);
        const bool last = sep == std::string::npos;
        if (part.empty()) {
            // Empty component: "a//b" anywhere, or a trailing "/" that
            // marks a directory entry (allowed only at the end).
            if (!last) return false;
        } else if (part == "." || part == "..") {
            return false;
        } else {
            ++depth;
            if (depth > kSkillImportMaxDepth) return false;
        }
        if (last) break;
        pos = sep + 1;
    }
    if (depth == 0) return false;

    while (!name.empty() && name.back() == '/') name.pop_back();
    outRelPath = name;
    return !outRelPath.empty();
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

// One-time (idempotent) migration from the Phase 1 flat layout to
// the Phase 2 folder-per-skill layout.  For every "<stem>.workflow.md"
// file at the top of `workflowsDir`, this:
//
//   1. Creates "<workflowsDir>/<stem>/" if it doesn't already exist
//   2. Renames "<workflowsDir>/<stem>.workflow.md"
//                 -> "<workflowsDir>/<stem>/<docName>"
//   3. If "<workflowsDir>/<stem>.py" exists, renames it
//                 -> "<workflowsDir>/<stem>/<stem>.py"
//
// Conservative: only matched .workflow.md (with optional same-stem .py)
// are migrated.  Orphan .py files at the top level are left alone --
// users may keep loose helper scripts there for their own purposes.
// If the destination folder already exists with content (mid-migration
// crash, manual move, etc.), this leaves it untouched and moves on so
// the user can resolve manually.
void MigrateFlatWorkflowsToFolders(const std::string& workflowsDir,
                                   const std::string& docName)
{
    if (workflowsDir.empty()) return;
    if (!wxDirExists(wxString::FromUTF8(workflowsDir))) return;

    // Collect names first so we don't trip wxDir's iterator with
    // renames while we walk.
    std::vector<std::string> flatWorkflowDocs;
    {
        wxDir scan(wxString::FromUTF8(workflowsDir));
        if (!scan.IsOpened()) return;
        wxString fname;
        bool cont = scan.GetFirst(&fname, wxEmptyString, wxDIR_FILES);
        while (cont) {
            const std::string name = std::string(fname.ToUTF8().data());
            if (IsWorkflowDocFileName(LowerAscii(name))) {
                flatWorkflowDocs.push_back(name);
            }
            cont = scan.GetNext(&fname);
        }
    }

    for (const std::string& docFileName : flatWorkflowDocs) {
        // Stem = filename minus the ".workflow.md" suffix.  Case-
        // insensitive strip matches IsWorkflowDocFileName's intent.
        std::string stem = docFileName;
        const std::string suffix = ".workflow.md";
        if (stem.size() > suffix.size()) {
            std::string tail = LowerAscii(stem.substr(stem.size() - suffix.size()));
            if (tail == suffix) {
                stem.resize(stem.size() - suffix.size());
            }
        }
        if (stem.empty()) continue;

        const std::string srcDoc    = JoinProjectPath(workflowsDir, docFileName);
        const std::string srcScript = JoinProjectPath(workflowsDir, stem + ".py");
        const std::string folder    = JoinProjectPath(workflowsDir, stem);
        const std::string dstDoc    = JoinProjectPath(folder, docName);
        const std::string dstScript = JoinProjectPath(folder, stem + ".py");

        // Skip if a folder already exists -- treat as already
        // migrated (or user-managed; don't clobber).
        if (FileOrDirExists(folder)) continue;

        if (!wxFileName::Mkdir(wxString::FromUTF8(folder),
                               wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) {
            continue;  // best-effort -- skip this one, try next
        }

        // Move the .workflow.md (now becomes <docName> inside the folder).
        // wxRenameFile with overwrite=false fails if destination
        // exists, but we just created the folder so SKILL.md can't
        // be there yet.
        if (!wxRenameFile(wxString::FromUTF8(srcDoc),
                          wxString::FromUTF8(dstDoc), false)) {
            // Couldn't move; roll back the empty folder we just made
            // so the next launch will retry instead of seeing the
            // folder and skipping (per the "Skip if folder exists"
            // guard above).
            wxFileName::Rmdir(wxString::FromUTF8(folder));
            continue;
        }

        // Move the matching .py if present.  Failure is non-fatal:
        // the SKILL.md already migrated, the user can re-create the
        // script or move it manually.
        if (FileOrDirExists(srcScript)) {
            wxRenameFile(wxString::FromUTF8(srcScript),
                         wxString::FromUTF8(dstScript), false);
        }
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

std::string ProjectManager::GetSkillsDir()
{
    // Returns %USERPROFILE%\LlamaBoss\Skills with the same home/documents
    // fallbacks used by GetProjectsDir(). A handful of private helpers in
    // this file still use the generic "Workflow" name because they are
    // reused for both Skills and per-project workflows.
    return JoinProjectPath(LlamaBossUserRootDirForProjects(), "Skills");
}

bool ProjectManager::EnsureSkillsRoot()
{
    std::string dir = GetSkillsDir();
    bool ok = wxFileName::Mkdir(wxString::FromUTF8(dir), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    const bool exists = ok || wxDirExists(wxString::FromUTF8(dir));
    if (exists) {
        // Order matters: pull files in from the legacy Workflows/
        // root first (pre-Skills builds), THEN run the flat ->
        // folder migrator so anything that lands here gets wrapped
        // into Phase 2's folder-per-skill layout in the same launch.
        MoveLegacySkillFilesIfNeeded(dir);
        MigrateFlatWorkflowsToFolders(dir, kSkillDocName);
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

    // Lean scaffold: only create folders we can't safely make on demand.
    //   Sources/, Workflows/ -- populated by explicit user/agent actions.
    //   Outputs/              -- kept eager; its deliverable-write path isn't
    //                            confirmed to mkdir-on-demand (CreateStagedTempFile
    //                            needs the parent present), so dropping it could
    //                            break Outputs writes.
    // Dropped: Templates/ (never auto-written) and Notes/ (NotesAppend calls
    // EnsureDirectory before writing, so Notes/ is created on first save).
    const char* kSubdirs[] = { "Sources", "Workflows", "Outputs" };
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
       << "This file is the project contract. While this project is attached to a chat, "
       << "LlamaBoss loads it into the model context on every request and treats it as the "
       << "source of truth for project-related work.\n\n"
       << "> New project: replace the placeholder below. Tell LlamaBoss what this project "
       << "is for and ask it to fill in PROJECT.md, or edit this file directly.\n\n"
       << "## Purpose\n\n"
       << "_Replace this line with what the project is for, who it serves, and what a good "
       << "result looks like._\n\n"
       << "## Project Rules\n\n"
       << "- Answer normal unrelated questions normally; do not force every request into this project.\n"
       << "- Follow these instructions when the user's request is related to this project.\n"
       << "- Do not invent sources, policies, or workflows that are not provided.\n"
       << "- Do not modify project files unless the user explicitly asks.\n\n"
       << "## Layout\n\n"
       << "Reference files live in `Sources/` (add them by dragging files into the chat while "
       << "this project is attached). Reusable instruction plans live in `Workflows/`. Durable "
       << "notes go to `Notes/NOTES.md` (say \"save this to project notes\"). Deliverables can be "
       << "written to `Outputs/` when a request asks for it.\n";

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

    // Skills lane: Agent Skills standard names are kebab-case, and the
    // folder stem IS the skill's `name` (mirrored into the frontmatter
    // below).  Project workflows keep their user-typed names.
    if (isGlobal) baseName = KebabCaseAsciiStem(baseName);

    const std::string uniqueStem =
        UniqueWorkflowStem(workflowsDir, baseName, /*kebabSuffix=*/isGlobal);

    // Phase 2 layout: skill / workflow lives in its own folder.
    //     <workflowsDir>/<uniqueStem>/<docName>   (SKILL.md or WORKFLOW.md)
    //     <workflowsDir>/<uniqueStem>/<uniqueStem>.py   (optional)
    // Folder gets created here; the instruction doc and the script are
    // written into it below.
    const std::string skillFolder = JoinProjectPath(workflowsDir, uniqueStem);
    if (!wxFileName::Mkdir(wxString::FromUTF8(skillFolder),
                           wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)
        && !wxDirExists(wxString::FromUTF8(skillFolder))) {
        outError = isGlobal
            ? std::string("Could not create the Skill folder.")
            : std::string("Could not create the workflow folder.");
        return false;
    }

    const std::string workflowPath =
        JoinProjectPath(skillFolder, isGlobal ? kSkillDocName : kWorkflowDocName);

    std::string scriptName;
    std::string scriptPath;
    if (createPythonScript) {
        scriptName = uniqueStem + ".py";
        scriptPath = JoinProjectPath(skillFolder, scriptName);
    }

    std::ostringstream md;
    if (isGlobal) {
        // Agent Skills frontmatter.  This starter description is a
        // deliberate placeholder: the Skill draft builder overwrites the
        // whole file (frontmatter included) with a real description at
        // draft time.  It matters only for skills that never get
        // drafted, where it still tells the model what the file is.
        // uniqueStem (not the raw typed name) keeps the plain YAML
        // scalar safe: no colons, quotes, or leading indicators.
        md << "---\n"
           << "name: " << uniqueStem << "\n"
           << "description: Starter contract for the " << uniqueStem
           << " Skill. Not yet designed; the description is filled in "
              "when the Skill is drafted.\n"
           << "---\n\n";
    }
    md << "# " << trimmedName << (isGlobal ? " Skill" : " Workflow") << "\n\n";
    if (isGlobal) {
        md << "This file defines a reusable LlamaBoss Skill. A Skill is available across all "
              "conversations whether or not a project is attached. When the user asks to run or "
              "use this Skill, LlamaBoss should read this file first, then follow the steps "
              "using normal tools/approval rules. If a project is attached and that project has a "
              "project workflow with the same name, the project workflow takes precedence.\n\n";
    } else {
        md << "This file is a reusable project workflow. When the user asks to run or use this workflow, "
              "LlamaBoss should read this file first, then follow the steps using the active project context, "
              "PROJECT.md, Project Sources, and normal tools/approval rules.\n\n";
    }
    md << "## Trigger Phrases\n\n"
       << "- Run the " << trimmedName << (isGlobal ? " skill\n" : " workflow\n")
       << "- Use the " << trimmedName << (isGlobal ? " skill\n" : " workflow\n");
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
       << "- Describe the final artifact or answer this "
       << (isGlobal ? "Skill" : "workflow")
       << " should produce.\n\n";

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
        outError = isGlobal
            ? std::string("Could not write Skill file.")
            : std::string("Could not write workflow file.");
        return false;
    }

    if (createPythonScript) {
        std::ostringstream py;
        py << "\"\"\"Reusable helper script for this "
           << (isGlobal ? "Skill" : "project workflow")
           << ".\n\n";
        if (isGlobal) {
            py << "LlamaBoss runs Skill helper scripts from the current conversation workspace.\n"
               << "This script can infer its Skill folder, the global Skills root, and the\n"
               << "LlamaBoss user root from its own file location.\n";
        } else {
            py << "LlamaBoss runs project workflow scripts from the current conversation workspace.\n"
               << "This script can infer its workflow folder, the project Workflows root, and\n"
               << "the project root from its own file location.\n";
        }
        py << "Edit this file to implement the repeatable "
           << (isGlobal ? "Skill" : "workflow")
           << " action.\n\"\"\"\n\n"
           << "from pathlib import Path\n\n"
           << "SCRIPT_PATH = Path(__file__).resolve()\n";
        if (isGlobal) {
            // Folder-per-skill layout:
            //   <user>/LlamaBoss/Skills/<skill>/<script>.py
            // .parent ->  the <skill>/ folder
            // .parent.parent ->  the Skills/ root
            // .parent.parent.parent -> the LlamaBoss/ user root
            py << "SKILL_DIR = SCRIPT_PATH.parent\n"
               << "SKILLS_ROOT = SKILL_DIR.parent\n"
               << "LLAMABOSS_USER_ROOT = SKILLS_ROOT.parent\n"
               << "PROJECTS_ROOT = LLAMABOSS_USER_ROOT / \"Projects\"\n";
        } else {
            // Project folder-per-skill layout:
            //   <project>/Workflows/<workflow>/<script>.py
            // .parent -> the <workflow>/ folder
            // .parent.parent -> the project's Workflows/ folder
            // .parent.parent.parent -> the project root
            py << "WORKFLOW_DIR = SCRIPT_PATH.parent\n"
               << "WORKFLOWS_ROOT = WORKFLOW_DIR.parent\n"
               << "PROJECT_ROOT = WORKFLOWS_ROOT.parent\n"
               << "SOURCES_DIR = PROJECT_ROOT / \"Sources\"\n"
               << "TEMPLATES_DIR = PROJECT_ROOT / \"Templates\"\n"
               << "OUTPUTS_DIR = PROJECT_ROOT / \"Outputs\"\n";
        }
        py << "CHAT_WORKSPACE = Path.cwd()\n\n"
           << "def main() -> None:\n"
           << "    print(\"Workflow helper script placeholder\")\n";
        if (isGlobal) {
            py << "    print(f\"Skill folder: {SKILL_DIR}\")\n"
               << "    print(f\"Skills root: {SKILLS_ROOT}\")\n"
               << "    print(f\"LlamaBoss user root: {LLAMABOSS_USER_ROOT}\")\n";
        } else {
            py << "    print(f\"Workflow folder: {WORKFLOW_DIR}\")\n"
               << "    print(f\"Project root: {PROJECT_ROOT}\")\n"
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
    // Display name keeps the legacy "<stem>.workflow.md" shape so
    // ResolveWorkflowInList's fuzzy match (which strips .workflow.md
    // and .ext) keeps working without changes -- and so every skill
    // doesn't collapse to the literal "SKILL.md" name.
    outWorkflow.name = uniqueStem + ".workflow.md";
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

    // Migrate any flat <stem>.workflow.md / <stem>.py pairs in this
    // project's Workflows/ to folders before we create a new one.
    // Keeps the directory shape consistent and avoids the new skill
    // being created in a folder while older siblings stay flat.
    MigrateFlatWorkflowsToFolders(ProjectManager::ProjectWorkflowsPath(rootPath), kWorkflowDocName);

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
    // Idempotent: migrates any flat workflows in this project's
    // Workflows/ to the folder-per-skill layout before listing.
    // No-op if there's nothing flat to migrate, so the cost is one
    // directory scan per call.
    MigrateFlatWorkflowsToFolders(ProjectWorkflowsPath(rootPath), kWorkflowDocName);
    return ListWorkflowsInDir(ProjectWorkflowsPath(rootPath), kWorkflowDocName, maxItems);
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
    // Same idempotent migration as ListProjectWorkflows -- ensures
    // any flat <stem>.py / <stem>.workflow.md pairs get wrapped
    // into <stem>/ folders before we try to enumerate scripts
    // (which now only looks inside skill folders).
    MigrateFlatWorkflowsToFolders(ProjectWorkflowsPath(rootPath), kWorkflowDocName);
    return ListWorkflowScriptsInDir(ProjectWorkflowsPath(rootPath), kWorkflowDocName, maxItems);
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

    // Nested forms (Workflows\<folder>\<script>.py) are the layout the
    // flat-to-folder migration itself creates, so a model that /ls'es
    // the Workflows folder will naturally emit them.  Mirror the
    // Skills-lane rule: remember the FIRST component as the pinned
    // folder, resolve the LAST component through the normal search,
    // then verify the resolved script lives under the pinned folder.
    std::string pinnedFolder;
    {
        const size_t firstSep = query.find_first_of("\\/");
        if (firstSep != std::string::npos) {
            pinnedFolder = query.substr(0, firstSep);
            const size_t lastSep = query.find_last_of("\\/");
            query = query.substr(lastSep + 1);
            if (pinnedFolder.empty() || query.empty()) {
                outError = "Project workflow script path must end in a "
                           "script filename, e.g. "
                           "Workflows\\<folder>\\<script>.py.";
                return false;
            }
        }
    }

    if (query.find('.') == std::string::npos) query += ".py";
    if (!HasLowerSuffix(query, ".py")) {
        outError = "Project workflow scripts must be .py files.";
        return false;
    }

    const auto scripts = ListProjectWorkflowScripts(rootPath, 0);
    if (scripts.empty()) {
        outError = "This project has no runnable workflow scripts (.py) "
                   "in Workflows/ — checked flat files, workflow folders, "
                   "and their scripts\\ subfolders. Create one with "
                   "python_create_script.";
        return false;
    }

    if (!ResolveWorkflowScriptInList(scripts, query, "project workflow script",
                                     outScript, outError)) {
        return false;
    }

    if (!pinnedFolder.empty()) {
        std::string resolvedLower = LowerAscii(outScript.path);
        std::replace(resolvedLower.begin(), resolvedLower.end(), '/', '\\');
        const std::string needle = "\\" + LowerAscii(pinnedFolder) + "\\";
        if (resolvedLower.find(needle) == std::string::npos) {
            outError = "Project workflow script " + query +
                       " resolved outside the requested Workflows\\" +
                       pinnedFolder + "\\ folder (found: " + outScript.path +
                       "). Use the bare form " + query +
                       " or the script's absolute path.";
            outScript = ProjectWorkflowScriptInfo{};
            return false;
        }
    }
    return true;
}

// ── Skills public API ─────────────────────────────────────────────
// User-facing Skills are the global reusable lane and live under
// %USERPROFILE%\LlamaBoss\Skills.  The internal helpers used below
// (CreateWorkflowInternal, ListWorkflowsInDir, etc.) keep the generic
// "Workflow" name on purpose: they are shared with the per-project
// workflow lane.  The shared on-disk structs (ProjectWorkflowInfo /
// ProjectWorkflowScriptInfo) are aliased to SkillInfo / SkillScriptInfo
// in project_manager.h -- prefer the Skill aliases at new call sites.

bool ProjectManager::CreateSkill(const std::string& skillName,
                                 SkillInfo& outSkill,
                                 std::string& outError)
{
    if (!EnsureSkillsRoot()) {
        outError = "Could not create the LlamaBoss Skills folder.";
        outSkill = SkillInfo{};
        return false;
    }
    return CreateWorkflowInternal(GetSkillsDir(),
                                  /*isGlobal=*/true,
                                  skillName,
                                  /*createPythonScript=*/false,
                                  outSkill,
                                  nullptr,
                                  outError);
}

std::vector<SkillInfo> ProjectManager::ListSkills(std::size_t maxItems)
{
    EnsureSkillsRoot();
    return ListWorkflowsInDir(GetSkillsDir(), kSkillDocName, maxItems);
}

std::vector<SkillScriptInfo> ProjectManager::ListSkillScripts(std::size_t maxItems)
{
    EnsureSkillsRoot();
    return ListWorkflowScriptsInDir(GetSkillsDir(), kSkillDocName, maxItems);
}

bool ProjectManager::ResolveSkill(const std::string& requested,
                                  SkillInfo& outSkill,
                                  std::string& outError)
{
    outSkill = SkillInfo{};
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

    const auto skills = ListSkills(0);
    if (skills.empty()) {
        outError = "There are no Skill files in LlamaBoss\\Skills.";
        return false;
    }

    return ResolveWorkflowInList(skills, query, "Skill", outSkill, outError);
}

bool ProjectManager::ResolveSkillScript(const std::string& requested,
                                        SkillScriptInfo& outScript,
                                        std::string& outError)
{
    outScript = SkillScriptInfo{};
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

    const auto scripts = ListSkillScripts(0);
    if (scripts.empty()) {
        outError = "There are no Python Skill scripts in LlamaBoss\\Skills.";
        return false;
    }

    return ResolveWorkflowScriptInList(scripts, query, "Skill script", outScript, outError);
}

// ── Skill import ──────────────────────────────────────────────────

bool ProjectManager::ProbeSkillImportFolder(const std::string& sourceFolder,
                                            SkillImportProbe& outProbe,
                                            std::string& outError)
{
    outProbe = SkillImportProbe{};
    outError.clear();

    if (sourceFolder.empty() ||
        !wxDirExists(wxString::FromUTF8(sourceFolder))) {
        outError = "The selected path is not a folder.";
        return false;
    }

    const std::string docPath = JoinProjectPath(sourceFolder, kSkillDocName);
    if (!wxFileExists(wxString::FromUTF8(docPath))) {
        outError = "The selected folder has no SKILL.md, so it is not a "
                   "Skill folder. Pick the folder that contains SKILL.md "
                   "directly (not its parent).";
        return false;
    }

    // Refuse sources that overlap the Skills root in either direction:
    // a folder already inside Skills is already loaded, and a folder
    // CONTAINING Skills would copy Skills into itself mid-walk.
    const std::string skillsNorm =
        NormalizeExistingPathForCompare(GetSkillsDir());
    const std::string srcNorm =
        NormalizeExistingPathForCompare(sourceFolder);
    if (!skillsNorm.empty() && !srcNorm.empty()) {
        if (srcNorm == skillsNorm ||
            srcNorm.rfind(skillsNorm + "/", 0) == 0) {
            outError = "That folder is already inside your LlamaBoss "
                       "Skills folder.";
            return false;
        }
        if (skillsNorm.rfind(srcNorm + "/", 0) == 0) {
            outError = "That folder contains your LlamaBoss Skills "
                       "folder; pick the single Skill folder instead.";
            return false;
        }
    }

    // Authored Agent Skills name wins; the source folder name is the
    // fallback.  Either way the LlamaBoss folder stem is kebab-cased.
    const std::string fmName = LbReadSkillFrontmatterName(docPath);
    outProbe.description = LbReadSkillFrontmatterDescription(docPath);
    outProbe.hasFrontmatter =
        !fmName.empty() || !outProbe.description.empty();
    outProbe.proposedName = KebabCaseAsciiStem(
        !fmName.empty() ? fmName : LeafFolderName(sourceFolder));
    // Advisory collision preview for the confirm dialog; the import
    // recomputes the stem authoritatively at copy time.
    outProbe.finalName = UniqueWorkflowStem(GetSkillsDir(),
                                            outProbe.proposedName,
                                            /*kebabSuffix=*/true);

    // Count what a copy would take; the walker enforces the caps.
    return WalkSkillImportTree(sourceFolder,
                               std::string(),
                               /*depth=*/1,
                               outProbe.fileCount,
                               outProbe.totalBytes,
                               /*onDir=*/nullptr,
                               /*onFile=*/nullptr,
                               outError);
}

bool ProjectManager::ImportSkillFolder(const std::string& sourceFolder,
                                       SkillInfo& outSkill,
                                       std::string& outError)
{
    outSkill = SkillInfo{};
    outError.clear();

    SkillImportProbe probe;
    if (!ProbeSkillImportFolder(sourceFolder, probe, outError)) return false;

    if (!EnsureSkillsRoot()) {
        outError = "Could not create the LlamaBoss Skills folder.";
        return false;
    }

    const std::string skillsDir = GetSkillsDir();
    const std::string destStem =
        UniqueWorkflowStem(skillsDir, probe.proposedName, /*kebabSuffix=*/true);
    const std::string destRoot = JoinProjectPath(skillsDir, destStem);

    if (!wxFileName::Mkdir(wxString::FromUTF8(destRoot),
                           wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)
        && !wxDirExists(wxString::FromUTF8(destRoot))) {
        outError = "Could not create the Skill folder: " + destRoot;
        return false;
    }

    // Copy.  On any failure, remove the partial destination so a
    // half-imported skill never shows up in the Skills list.
    std::size_t fileCount = 0;
    unsigned long long totalBytes = 0;
    const bool copied = WalkSkillImportTree(
        sourceFolder,
        std::string(),
        /*depth=*/1,
        fileCount,
        totalBytes,
        /*onDir=*/[&destRoot](const std::string& relDir) {
            const std::string abs = JoinProjectPath(destRoot, relDir);
            return wxFileName::Mkdir(wxString::FromUTF8(abs),
                                     wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)
                   || wxDirExists(wxString::FromUTF8(abs));
        },
        /*onFile=*/[&destRoot](const std::string& absFile,
                               const std::string& relFile) {
            const std::string abs = JoinProjectPath(destRoot, relFile);
            return wxCopyFile(wxString::FromUTF8(absFile),
                              wxString::FromUTF8(abs),
                              /*overwrite=*/true);
        },
        outError);

    if (!copied) {
        wxFileName::Rmdir(wxString::FromUTF8(destRoot),
                          wxPATH_RMDIR_RECURSIVE);
        if (outError.empty()) outError = "Copying the Skill folder failed.";
        return false;
    }

    // Normalize the imported contract: synthesize frontmatter when the
    // source has none, and align `name:` with the final folder stem
    // (which is authoritative in LlamaBoss and may carry a "-N"
    // collision suffix the author could not have known about).
    const std::string destDoc = JoinProjectPath(destRoot, kSkillDocName);
    {
        std::ifstream f(path_safety::Utf8ToWide(destDoc),
                        std::ios::in | std::ios::binary);
        if (f.is_open()) {
            std::string body((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
            f.close();
            std::string normalized =
                LbRewriteSkillFrontmatterName(
                    LbEnsureSkillFrontmatter(body, destStem),
                    destStem);
            if (normalized != body) {
                // Best-effort: a failed rewrite leaves the verbatim
                // import, which is still a working (legacy-style) skill.
                WriteUtf8File(destDoc, normalized);
            }
        }
    }

    // Same synthetic display-name shape ListWorkflowsInDir produces.
    outSkill.name = destStem + ".workflow.md";
    outSkill.path = destDoc;
    outSkill.sizeBytes = FileSizeBytes(destDoc);
    return true;
}

bool ProjectManager::ExtractSkillZipToTemp(const std::string& zipPath,
                                           std::string& outTempRoot,
                                           std::string& outSkillFolder,
                                           std::string& outError)
{
    outTempRoot.clear();
    outSkillFolder.clear();
    outError.clear();

    if (zipPath.empty() || !wxFileExists(wxString::FromUTF8(zipPath))) {
        outError = "The selected .zip file does not exist.";
        return false;
    }

    // Fresh temp root per extraction; the caller always removes it.
    const std::string tempBase =
        std::string(wxFileName::GetTempDir().ToUTF8().data());
    std::string tempRoot;
    for (int i = 0; i < 64; ++i) {
        std::ostringstream stem;
        stem << "LbSkillImport-" << ::wxGetProcessId() << "-" << i;
        const std::string candidate = JoinProjectPath(tempBase, stem.str());
        if (!FileOrDirExists(candidate)) { tempRoot = candidate; break; }
    }
    if (tempRoot.empty() ||
        (!wxFileName::Mkdir(wxString::FromUTF8(tempRoot),
                            wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)
         && !wxDirExists(wxString::FromUTF8(tempRoot)))) {
        outError = "Could not create a temporary extraction folder.";
        return false;
    }

    // Stream the archive out with the same caps the folder walker
    // enforces, counting UNCOMPRESSED bytes so a zip bomb trips the cap
    // instead of filling the disk.
    bool ok = true;
    std::size_t fileCount = 0;
    unsigned long long totalBytes = 0;
    {
        wxFFileInputStream in(wxString::FromUTF8(zipPath));
        if (!in.IsOk()) {
            outError = "Could not open the .zip file.";
            ok = false;
        } else {
            wxZipInputStream zip(in);
            std::unique_ptr<wxZipEntry> entry;
            while (ok && (entry.reset(zip.GetNextEntry()), entry)) {
                std::string rel;
                const std::string rawName =
                    std::string(entry->GetName(wxPATH_UNIX).ToUTF8().data());
                if (!SkillZipEntryNameIsSafe(rawName, rel)) {
                    outError = "The archive contains an unsafe entry name: " +
                               rawName;
                    ok = false;
                    break;
                }

                const std::string dest = JoinProjectPath(tempRoot, rel);
                if (entry->IsDir()) {
                    if (!wxFileName::Mkdir(wxString::FromUTF8(dest),
                                           wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)
                        && !wxDirExists(wxString::FromUTF8(dest))) {
                        outError = "Could not create folder: " + dest;
                        ok = false;
                    }
                    continue;
                }

                ++fileCount;
                if (fileCount > kSkillImportMaxFiles) {
                    outError = "The archive has more than " +
                               std::to_string(kSkillImportMaxFiles) +
                               " files; this does not look like a Skill.";
                    ok = false;
                    break;
                }

                // Parent dirs may not have their own entries.
                {
                    const wxFileName fn(wxString::FromUTF8(dest));
                    const wxString parent = fn.GetPath();
                    if (!parent.IsEmpty() && !wxDirExists(parent)) {
                        wxFileName::Mkdir(parent, wxS_DIR_DEFAULT,
                                          wxPATH_MKDIR_FULL);
                    }
                }

                std::ofstream outFile(path_safety::Utf8ToWide(dest),
                                      std::ios::binary | std::ios::trunc);
                if (!outFile.is_open()) {
                    outError = "Could not write file: " + dest;
                    ok = false;
                    break;
                }
                char buffer[64 * 1024];
                while (ok && !zip.Eof()) {
                    zip.Read(buffer, sizeof(buffer));
                    const size_t got = zip.LastRead();
                    if (got == 0) break;
                    totalBytes += got;
                    if (totalBytes > kSkillImportMaxBytes) {
                        outError = "The archive expands past 64 MB; this "
                                   "does not look like a Skill.";
                        ok = false;
                        break;
                    }
                    outFile.write(buffer, static_cast<std::streamsize>(got));
                    if (!outFile) {
                        outError = "Could not write file: " + dest;
                        ok = false;
                        break;
                    }
                }
            }
            if (ok && fileCount == 0) {
                outError = "The archive contains no files.";
                ok = false;
            }
        }
    }

    if (!ok) {
        wxFileName::Rmdir(wxString::FromUTF8(tempRoot),
                          wxPATH_RMDIR_RECURSIVE);
        if (outError.empty()) outError = "Extracting the .zip failed.";
        return false;
    }

    // Locate the skill folder: SKILL.md at the extraction root, or inside
    // exactly one top-level folder (the layout ExportSkillToZip writes).
    std::string skillFolder;
    if (wxFileExists(wxString::FromUTF8(
            JoinProjectPath(tempRoot, kSkillDocName)))) {
        skillFolder = tempRoot;
    } else {
        wxDir dir(wxString::FromUTF8(tempRoot));
        if (dir.IsOpened()) {
            wxString name;
            bool cont = dir.GetFirst(&name, wxEmptyString, wxDIR_DIRS);
            while (cont) {
                const std::string sub =
                    JoinProjectPath(tempRoot,
                                    std::string(name.ToUTF8().data()));
                if (wxFileExists(wxString::FromUTF8(
                        JoinProjectPath(sub, kSkillDocName)))) {
                    if (!skillFolder.empty()) {
                        skillFolder.clear();   // ambiguous: several skills
                        break;
                    }
                    skillFolder = sub;
                }
                cont = dir.GetNext(&name);
            }
        }
    }

    if (skillFolder.empty()) {
        wxFileName::Rmdir(wxString::FromUTF8(tempRoot),
                          wxPATH_RMDIR_RECURSIVE);
        outError = "The archive does not contain exactly one Skill "
                   "(no SKILL.md at the root or in a single top-level "
                   "folder).";
        return false;
    }

    outTempRoot = tempRoot;
    outSkillFolder = skillFolder;
    return true;
}

bool ProjectManager::ExportSkillToZip(const std::string& skillContractPath,
                                      const std::string& zipPath,
                                      std::string& outError)
{
    outError.clear();

    if (zipPath.empty()) {
        outError = "No destination .zip path was given.";
        return false;
    }

    std::string skillFolder;
    {
        const size_t sep = skillContractPath.find_last_of("\\/");
        if (sep == std::string::npos) {
            outError = "Not a Skill contract path.";
            return false;
        }
        skillFolder = skillContractPath.substr(0, sep);
    }
    if (skillFolder.empty() ||
        !wxDirExists(wxString::FromUTF8(skillFolder)) ||
        !wxFileExists(wxString::FromUTF8(
            JoinProjectPath(skillFolder, kSkillDocName)))) {
        outError = "The Skill folder or its SKILL.md could not be found.";
        return false;
    }
    const std::string stem = LeafFolderName(skillFolder);

    wxFFileOutputStream fileOut(wxString::FromUTF8(zipPath));
    if (!fileOut.IsOk()) {
        outError = "Could not create the .zip file: " + zipPath;
        return false;
    }
    wxZipOutputStream zip(fileOut);

    // Reuse the bounded import walker: same junk-dir skips, reparse-point
    // skips, and caps apply to exports.  Entries are rooted "<stem>/..."
    // so the archive re-imports via the single-top-level-folder path.
    std::size_t fileCount = 0;
    unsigned long long totalBytes = 0;
    const bool walked = WalkSkillImportTree(
        skillFolder,
        std::string(),
        /*depth=*/1,
        fileCount,
        totalBytes,
        /*onDir=*/nullptr,   // wxZipOutputStream creates parents implicitly
        /*onFile=*/[&zip, &stem](const std::string& absFile,
                                 const std::string& relFile) {
            std::string rel = relFile;
            std::replace(rel.begin(), rel.end(), '\\', '/');
            if (!zip.PutNextEntry(
                    wxString::FromUTF8(stem + "/" + rel))) {
                return false;
            }
            std::ifstream in(path_safety::Utf8ToWide(absFile),
                             std::ios::binary);
            if (!in.is_open()) return false;
            char buffer[64 * 1024];
            while (in) {
                in.read(buffer, sizeof(buffer));
                const std::streamsize got = in.gcount();
                if (got <= 0) break;
                if (!zip.WriteAll(buffer, static_cast<size_t>(got)))
                    return false;
            }
            return !in.bad();
        },
        outError);

    if (!walked) {
        zip.Close();
        fileOut.Close();
        ::wxRemoveFile(wxString::FromUTF8(zipPath));
        if (outError.empty()) outError = "Zipping the Skill folder failed.";
        return false;
    }

    if (!zip.Close() || !fileOut.Close()) {
        ::wxRemoveFile(wxString::FromUTF8(zipPath));
        outError = "Finalizing the .zip file failed.";
        return false;
    }
    return true;
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
