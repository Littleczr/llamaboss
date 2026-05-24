// tool_path_safety.h
//
// Shared workspace-containment helpers for the file-mutation tools
// (tool_write, tool_edit, tool_delete, tool_mkdir).
//
// Before this header existed, each of those four .cpp files carried
// its own byte-identical copy of LowerAscii / NormalizeForCompare /
// IsUnderCwd.  IsUnderCwd is the workspace sandbox boundary -- a
// silent drift between the four copies is a security hole.  Lifting
// the helpers here makes that class of regression impossible.
//
// Note that this is the *tool-side* path safety: it operates on
// canonical absolute Windows paths produced by GetFullPathNameW
// (i.e. paths already resolved through tool_path::ResolveToolPath)
// and decides whether they fall inside the conversation cwd.
//
// Filename sanitization (path_safety::SanitizeFilename) and UTF-8 <->
// wide conversion (path_safety::Utf8ToWide / WideToUtf8) live in the
// existing path_safety.h -- this header layers on top of them.
//
// All functions are inline and live in the tool_path_safety namespace
// so the symbols don't collide with the anonymous-namespace versions
// they replace.

#pragma once

#include <string>

#include "tool_path.h"  // ResolveToolPath

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
// activeProjectRoot.  All other paths preserve legacy behavior and resolve
// against cwd.
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

inline bool IsUnderAllowedWriteRoot(const std::string& absPath,
                                    const std::string& cwd,
                                    const std::string& activeProjectRoot,
                                    const std::string& skillsRoot)
{
    if (IsUnderCwd(absPath, cwd)) return true;
    if (!activeProjectRoot.empty() && IsUnderCwd(absPath, activeProjectRoot)) return true;
    if (!skillsRoot.empty() && IsUnderCwd(absPath, skillsRoot)) return true;
    return false;
}

inline std::string AllowedWriteRootsDiagnostic(const std::string& cwd,
                                               const std::string& activeProjectRoot,
                                               const std::string& skillsRoot)
{
    std::string s = "\n  cwd:      " + cwd;
    if (!activeProjectRoot.empty()) {
        s += "\n  project:  " + activeProjectRoot;
    }
    if (!skillsRoot.empty()) {
        s += "\n  skills:   " + skillsRoot;
    }
    return s;
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
