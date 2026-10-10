// tool_path_safety.h
//
// Shared workspace-containment helpers for the file-mutation tools
// (tool_write, tool_edit, tool_delete, tool_mkdir).
//
// IsUnderCwd is the workspace sandbox boundary, so it must exist in
// exactly one place: a silent drift between per-tool copies would be a
// security hole.
//
// Note that this is the *tool-side* path safety: it operates on
// canonical absolute Windows paths produced by GetFullPathNameW
// (i.e. paths already resolved through tool_path::ResolveToolPath)
// and decides whether they fall inside the allowed write roots.
//
// Filename sanitization (path_safety::SanitizeFilename) and UTF-8 <->
// wide conversion (path_safety::Utf8ToWide / WideToUtf8) live in
// path_safety.h -- this header layers on top of them.
//
// All functions are inline and live in the tool_path_safety namespace.

#pragma once

#include <string>
#include <vector>

#include "tool_path.h"  // ResolveToolPath
#include "chat_folders.h"  // chat folder recognizer

namespace tool_path_safety {

// Lowercase a single ASCII byte; non-ASCII passes through.  Used for
// case-insensitive Windows path comparisons -- the CRT tolower is
// locale-dependent and signed-char-unsafe.
inline char LowerAscii(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
}

// Normalize a Windows-shaped path for prefix comparison: forward
// slashes -> backslash, ASCII case folded.  Caller still has to do
// the boundary check separately; this just produces comparable
// strings.
//
// Caveat: case folding is ASCII-only.  Windows file system case
// insensitivity for non-ASCII characters depends on the locale and
// volume; for paths produced by GetFullPathNameW on the same volume
// the on-disk casing matches, so the ASCII fold is enough in practice.
inline std::string NormalizeForCompare(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        char n = (c == '/') ? '\\' : c;
        out += LowerAscii(n);
    }
    return out;
}

// True iff `absPath` is the same path as `cwd` or a descendant of it.
//
// Both inputs are expected to be canonical absolute Windows paths
// produced by GetFullPathNameW (i.e. already resolved through
// ResolveToolPath).  The comparison is:
//   1. Normalize both: '/' -> '\\', ASCII lowercase.
//   2. Strip a trailing '\\' from the cwd UNLESS it's a drive root
//      ("C:\\") -- drive roots keep the trailing separator so
//      "C:\\foo" matches against "C:\\" naturally.
//   3. absPath must start with the normalized cwd.
//   4. Boundary: either lengths match, the cwd ended with '\\'
//      (root case), or the next character in absPath is '\\'.
//      The boundary check stops "C:\\foo" from being treated as
//      under "C:\\foobar".
inline bool IsUnderCwd(const std::string& absPath, const std::string& cwd)
{
    if (absPath.empty() || cwd.empty()) return false;

    std::string a = NormalizeForCompare(absPath);
    std::string r = NormalizeForCompare(cwd);

    // Strip trailing '\\' from r unless it's a drive root.  A drive
    // root has size 3 (e.g. "c:\\") and we leave it alone.
    while (r.size() > 3 && r.back() == '\\') r.pop_back();

    if (a.size() < r.size()) return false;
    if (a.compare(0, r.size(), r) != 0) return false;
    if (a.size() == r.size()) return true;
    if (r.back() == '\\') return true;
    return a[r.size()] == '\\';
}

// True iff two canonical absolute Windows paths refer to the same
// normalized path for our containment purposes.
inline bool SamePath(const std::string& aIn, const std::string& bIn)
{
    if (aIn.empty() || bIn.empty()) return false;

    std::string a = NormalizeForCompare(aIn);
    std::string b = NormalizeForCompare(bIn);

    while (a.size() > 3 && a.back() == '\\') a.pop_back();
    while (b.size() > 3 && b.back() == '\\') b.pop_back();

    return a == b;
}

// Project/Skills-aware mutation boundary.  Normal chats may modify files
// inside the conversation cwd.  When a chat is attached to a project,
// that durable project root is also an allowed mutation root, so the
// model can create project folders, workflow files, notes, and outputs
// without asking the user to /cd away from the chat Workspace first.
// Global reusable Skills are also user-authored durable assets, so the
// Skills root is writable through the same approval-gated mutation tools.

// True iff `input` is a relative path that should be rooted at the
// active project folder instead of the conversation workspace.  Keep
// this deliberately narrow: only the standard project lanes and the
// two root project files are rebased.  Arbitrary relative paths still
// resolve against ctx.cwd so normal chat-workspace behavior stays
// intact.
inline bool IsKnownProjectRelativePath(const std::string& input)
{
    std::string s = input;

    auto trim = [](std::string& v) {
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t' ||
                              v.front() == '\r' || v.front() == '\n')) {
            v.erase(v.begin());
        }
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t' ||
                              v.back() == '\r' || v.back() == '\n')) {
            v.pop_back();
        }
    };

    trim(s);
    if (s.size() >= 2 &&
        ((s.front() == '"' && s.back() == '"') ||
         (s.front() == '\'' && s.back() == '\''))) {
        s = s.substr(1, s.size() - 2);
        trim(s);
    }
    if (s.empty()) return false;

    // Reject traversal components anywhere in the relative path, not
    // just as a prefix: "Inputs\\..\\..\\outside.txt" must not be
    // classified as a known project-lane path.  Final write containment
    // is a second line of defense, but classification itself must fail
    // closed and leave traversal-shaped inputs on the plain cwd resolver
    // path.
    {
        size_t segmentStart = 0;
        while (segmentStart <= s.size()) {
            const size_t separator = s.find_first_of("\\/", segmentStart);
            const size_t segmentEnd =
                (separator == std::string::npos) ? s.size() : separator;
            if (s.substr(segmentStart, segmentEnd - segmentStart) == "..")
                return false;
            if (separator == std::string::npos) break;
            segmentStart = separator + 1;
        }
    }

    // Absolute paths and traversal attempts keep the existing resolver
    // behavior.  This helper is only for normal project-relative lanes.
    if (s.size() >= 3 && ((s[0] >= 'A' && s[0] <= 'Z') ||
                          (s[0] >= 'a' && s[0] <= 'z')) &&
        s[1] == ':' && (s[2] == '\\' || s[2] == '/')) return false;
    if (s.size() >= 2 && (s[0] == '\\' || s[0] == '/') &&
        (s[1] == '\\' || s[1] == '/')) return false;
    if (s[0] == '\\' || s[0] == '/') return false;
    if (s.rfind("..", 0) == 0) return false;

    while (s.rfind(".\\", 0) == 0 || s.rfind("./", 0) == 0) {
        s = s.substr(2);
    }
    if (s.empty() || s.rfind("..", 0) == 0) return false;

    std::string key;
    key.reserve(s.size());
    for (char c : s) {
        char n = (c == '\\') ? '/' : LowerAscii(c);
        key += n;
    }

    // Optional human/model prefix: project/Inputs/foo.txt.
    if (key.rfind("project/", 0) == 0) {
        key = key.substr(8);
    }

    if (key == "project.md" || key == "project.json" || key == "requirements.txt") return true;

    static const char* kProjectDirs[] = {
        "inputs", "outputs", "workflows", "notes", "sources", "templates"
    };

    for (const char* dir : kProjectDirs) {
        std::string d(dir);
        if (key == d) return true;
        if (key.rfind(d + "/", 0) == 0) return true;
    }

    return false;
}

// Project-aware path resolver for tools.  In project chats, known
// project-relative paths such as `Inputs\\x.txt`, `Outputs\\report.md`,
// `Workflows\\helper.py`, `Notes\\NOTES.md`, `Sources\\policy.pdf`,
// `Templates\\form.docx`, `PROJECT.md`, `project.json`, and `requirements.txt` resolve under
// activeProjectRoot.  All other paths resolve against cwd.
inline std::string ResolveProjectAwareToolPath(const std::string& input,
                                               const std::string& cwd,
                                               const std::string& activeProjectRoot)
{
    if (!activeProjectRoot.empty() && IsKnownProjectRelativePath(input)) {
        std::string resolved = ResolveToolPath(input, activeProjectRoot);
        if (!resolved.empty()) return resolved;
    }
    return ResolveToolPath(input, cwd);
}

// Resolve an explicit chat-folder lane path for read-only tools.
//
// Conversation artifacts intentionally live beside the default Workspace:
//
//   ...\Chats\<date>_<slug>_<id>\Workspace
//   ...\Chats\<date>_<slug>_<id>\Extracted
//   ...\Chats\<date>_<slug>_<id>\Scripts
//
// A model naturally addresses those artifacts as `Extracted\repo\file.cpp`
// or `Scripts\helper.py`. Resolving such a path against cwd first points at
// the nonexistent `Workspace\Extracted` / `Workspace\Scripts` tree and used
// to cost a failed tool call plus an absolute-path retry. Keep the alias
// narrow and read-only: only known lane prefixes, only the recognized
// conversation/default Workspace shape, never traversal, and only after the
// ordinary cwd/project path failed to name an existing item.
inline std::string TryResolveConversationLanePath(const std::string& input,
                                                  const std::string& cwd)
{
    auto trim = [](std::string s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return std::string();
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    };
    auto trimTrailingSeparators = [](std::string s) {
        while (!s.empty() && (s.back() == '\\' || s.back() == '/'))
            s.pop_back();
        return s;
    };
    auto parentDir = [&](const std::string& path) {
        std::string s = trimTrailingSeparators(path);
        size_t pos = s.find_last_of("\\/");
        return pos == std::string::npos ? std::string() : s.substr(0, pos);
    };
    auto baseName = [&](const std::string& path) {
        std::string s = trimTrailingSeparators(path);
        size_t pos = s.find_last_of("\\/");
        return pos == std::string::npos ? s : s.substr(pos + 1);
    };
    auto lower = [](std::string s) {
        for (char& c : s) c = LowerAscii(c);
        return s;
    };

    std::string requested = trim(input);
    if (requested.size() >= 2 &&
        ((requested.front() == '"' && requested.back() == '"') ||
         (requested.front() == '\'' && requested.back() == '\''))) {
        requested = trim(requested.substr(1, requested.size() - 2));
    }
    while (requested.rfind(".\\", 0) == 0 ||
           requested.rfind("./", 0) == 0) {
        requested = requested.substr(2);
    }
    if (requested.empty() || requested.front() == '\\' ||
        requested.front() == '/') return std::string();
    if (requested.size() >= 3 &&
        ((requested[0] >= 'A' && requested[0] <= 'Z') ||
         (requested[0] >= 'a' && requested[0] <= 'z')) &&
        requested[1] == ':' &&
        (requested[2] == '\\' || requested[2] == '/')) {
        return std::string();
    }

    // Reject traversal components before rebasing the path into a sibling
    // lane. ResolveToolPath canonicalizes too, but the alias classifier itself
    // should fail closed instead of accepting an escape-shaped request.
    size_t segmentStart = 0;
    while (segmentStart <= requested.size()) {
        size_t separator = requested.find_first_of("\\/", segmentStart);
        size_t segmentEnd = separator == std::string::npos
            ? requested.size() : separator;
        if (requested.substr(segmentStart, segmentEnd - segmentStart) == "..")
            return std::string();
        if (separator == std::string::npos) break;
        segmentStart = separator + 1;
    }

    const size_t firstSep = requested.find_first_of("\\/");
    const std::string first = lower(requested.substr(0, firstSep));
    std::string lane;
    if      (first == "extracted")    lane = "Extracted";
    else if (first == "scripts")      lane = "Scripts";
    else if (first == "documents")    lane = "Documents";
    else if (first == "spreadsheets") lane = "Spreadsheets";
    else if (first == "pdfs")         lane = "PDFs";
    else if (first == "filled forms") lane = "Filled Forms";
    else if (first == "word")         lane = "Word";
    else if (first == "tooloutputs")  lane = "ToolOutputs";
    else return std::string();

    const std::string cleanCwd = trimTrailingSeparators(cwd);
    if (lower(baseName(cleanCwd)) != "workspace") return std::string();

    const std::string workspaceParent = parentDir(cleanCwd);
    if (workspaceParent.empty()) return std::string();

    std::string laneBase = chat_folders::ChatFolderFromWorkspaceCwd(cleanCwd);
    if (laneBase.empty()) {
        // Unsaved/default Workspace layout:
        //   %USERPROFILE%\LlamaBoss\Shared\Workspace   (current)
        //   %USERPROFILE%\LlamaBoss\Workspace           (pre-Shared builds)
        const std::string parentBase = lower(baseName(workspaceParent));
        const bool sharedLayout = parentBase == "shared" &&
            lower(baseName(parentDir(workspaceParent))) == "llamaboss";
        if (!sharedLayout && parentBase != "llamaboss")
            return std::string();
        laneBase = workspaceParent;
    }

    std::string laneRoot = ResolveToolPath(lane, laneBase);
    if (laneRoot.empty()) return std::string();

    std::string remainder;
    if (firstSep != std::string::npos)
        remainder = requested.substr(firstSep + 1);

    std::string resolved = remainder.empty()
        ? laneRoot
        : ResolveToolPath(remainder, laneRoot);
    if (resolved.empty() || !IsUnderCwd(resolved, laneRoot))
        return std::string();
    return resolved;
}

// Read-only resolver with collision-safe precedence:
//   1. existing project/cwd path;
//   2. explicit conversation-lane alias;
//   3. unresolved ordinary path, so the caller can report its normal error.
// `usedConversationLane` is optional and lets presentation code add a chip or
// preserve an absolute helper argument only when the alias actually won.
inline std::string ResolveReadOnlyToolPath(const std::string& input,
                                           const std::string& cwd,
                                           const std::string& activeProjectRoot,
                                           bool* usedConversationLane = nullptr)
{
    if (usedConversationLane) *usedConversationLane = false;

    std::string primary = ResolveProjectAwareToolPath(
        input, cwd, activeProjectRoot);
    if (!primary.empty() && (IsFile(primary) || IsDirectory(primary)))
        return primary;

    std::string lane = TryResolveConversationLanePath(input, cwd);
    if (!lane.empty() && (IsFile(lane) || IsDirectory(lane))) {
        if (usedConversationLane) *usedConversationLane = true;
        return lane;
    }

    return primary;
}

inline bool IsUnderAllowedWriteRoot(const std::string& absPath,
                                    const std::string& cwd,
                                    const std::string& activeProjectRoot,
                                    const std::string& skillsRoot,
                                    const std::vector<std::string>& additionalWriteRoots = {})
{
    if (IsUnderCwd(absPath, cwd)) return true;
    if (!activeProjectRoot.empty() && IsUnderCwd(absPath, activeProjectRoot)) return true;
    if (!skillsRoot.empty() && IsUnderCwd(absPath, skillsRoot)) return true;
    for (const std::string& root : additionalWriteRoots) {
        if (!root.empty() && IsUnderCwd(absPath, root)) return true;
    }
    return false;
}

inline std::string AllowedWriteRootsDiagnostic(const std::string& cwd,
                                               const std::string& activeProjectRoot,
                                               const std::string& skillsRoot,
                                               const std::vector<std::string>& additionalWriteRoots = {})
{
    std::string s = "\n  cwd:      " + cwd;
    if (!activeProjectRoot.empty()) {
        s += "\n  project:  " + activeProjectRoot;
    }
    if (!skillsRoot.empty()) {
        s += "\n  skills:   " + skillsRoot;
    }
    for (const std::string& root : additionalWriteRoots) {
        if (!root.empty()) s += "\n  granted:  " + root;
    }
    return s;
}

// Drive roots are deliberately too broad for a one-click chat grant.  When
// the parent of a target is a drive root, the approval layer grants the exact
// target path instead (a file-sized capability, or the directory being
// created) rather than silently granting all of C:\\ or D:\\.
inline bool IsDriveRoot(const std::string& path)
{
    const std::string n = NormalizeForCompare(path);
    return n.size() == 3 &&
           n[1] == ':' &&
           n[2] == '\\' &&
           ((n[0] >= 'a' && n[0] <= 'z') ||
            (n[0] >= 'A' && n[0] <= 'Z'));
}

// Returns the basename portion of an absolute Windows path.  Empty
// if the input has no separator (shouldn't happen for an absolute
// path, but defensive).
inline std::string Basename(const std::string& absPath)
{
    size_t p = absPath.find_last_of("\\/");
    return (p == std::string::npos) ? absPath : absPath.substr(p + 1);
}

// Returns the parent directory portion of an absolute Windows path,
// without the trailing separator (except for drive roots, which
// keep their trailing '\\').  Empty if the input has no separator.
inline std::string ParentDir(const std::string& absPath)
{
    size_t p = absPath.find_last_of("\\/");
    if (p == std::string::npos) return {};
    if (p == 2 && absPath.size() >= 3 &&
        absPath[1] == ':' &&
        (absPath[2] == '\\' || absPath[2] == '/')) {
        // Parent of "C:\\foo" is "C:\\" with trailing slash.
        return absPath.substr(0, 3);
    }
    return absPath.substr(0, p);
}

} // namespace tool_path_safety
