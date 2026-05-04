#define _CRT_SECURE_NO_WARNINGS

// tool_notes.cpp

#include "tool_notes.h"
#include "path_safety.h"
#include "tool_staged_write.h"   // CreateStagedTempFile, atomic save

#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>             // wxGetEnv, wxGetHomeDir

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <sstream>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

// ─── Path helpers ──────────────────────────────────────────────────
//
// LlamaBossUserRootDir() is duplicated here from chat_history.cpp on
// purpose. There it is a `static` helper inside an anonymous block,
// not exposed via the header, and it would feel wrong to widen
// ChatHistory's API just so an unrelated tool can reuse the resolution
// logic. The function is small and has no state; the duplication cost
// is minimal compared to coupling notes to ChatHistory.
//
// If a third user shows up, this lifts cleanly into a small shared
// path-helper TU.

std::string JoinPath(const std::string& a, const std::string& b)
{
    if (a.empty()) return b;
    if (a.back() == '/' || a.back() == '\\') return a + b;
    return a + std::string(1, wxFILE_SEP_PATH) + b;
}

std::string LlamaBossUserRootDir()
{
#ifdef __WXMSW__
    wxString userProfile;
    if (wxGetEnv("USERPROFILE", &userProfile) && !userProfile.IsEmpty()) {
        return JoinPath(std::string(userProfile.ToUTF8().data()), "LlamaBoss");
    }
#endif

    wxString home = wxGetHomeDir();
    if (!home.IsEmpty()) {
        return JoinPath(std::string(home.ToUTF8().data()), "LlamaBoss");
    }

    wxString docs = wxStandardPaths::Get().GetDocumentsDir();
    return JoinPath(std::string(docs.ToUTF8().data()), "LlamaBoss");
}

// Ensures the LlamaBoss user-root directory exists.  Returns true on
// success or if it already existed.  Used right before NotesAppend
// stages its temp file — CreateStagedTempFile needs the parent dir
// present.
bool EnsureDirectory(const std::string& dir)
{
    if (dir.empty()) return false;

    wxString wDir = wxString::FromUTF8(dir);
    if (wxDirExists(wDir)) return true;

    return wxFileName::Mkdir(wDir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
}

std::string ProjectNotesDir(const ToolContext& ctx)
{
    if (ctx.activeProjectRoot.empty()) return std::string();
    return JoinPath(ctx.activeProjectRoot, "Notes");
}


// ─── Time / formatting helpers ─────────────────────────────────────

// "YYYY-MM-DD HH:MM" in local time.  Used as the per-entry header.
// Local time matches the user's expectation when they hand-edit
// NOTES.md — they'll see entries dated by their wall clock, not UTC.
std::string LocalTimestampHeader()
{
    std::time_t now = std::time(nullptr);
    std::tm tmLocal{};
#ifdef _MSC_VER
    localtime_s(&tmLocal, &now);
#else
    tmLocal = *std::localtime(&now);
#endif

    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tmLocal);
    return std::string(buf);
}

// Trim leading/trailing whitespace.  Internal whitespace (including
// newlines) is preserved so multi-line entries survive intact.
std::string Trim(const std::string& s)
{
    size_t a = 0;
    while (a < s.size() &&
           (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n'))
        ++a;
    size_t b = s.size();
    while (b > a &&
           (s[b - 1] == ' ' || s[b - 1] == '\t' ||
            s[b - 1] == '\r' || s[b - 1] == '\n'))
        --b;
    return s.substr(a, b - a);
}

std::string FirstNonEmptyLine(const std::string& s)
{
    std::istringstream lines(s);
    std::string line;
    while (std::getline(lines, line)) {
        line = Trim(line);
        if (!line.empty()) return line;
    }
    return std::string();
}

std::string OneLineSummary(const std::string& s, size_t maxLen = 180)
{
    std::string line = FirstNonEmptyLine(s);
    if (line.empty()) line = Trim(s);

    for (char& c : line) {
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    }

    // Collapse repeated spaces so the global index entry stays compact.
    std::string collapsed;
    collapsed.reserve(line.size());
    bool priorSpace = false;
    for (char c : line) {
        const bool isSpace = (c == ' ');
        if (isSpace && priorSpace) continue;
        collapsed.push_back(c);
        priorSpace = isSpace;
    }

    if (collapsed.size() > maxLen) {
        collapsed.resize(maxLen);
        collapsed += "...";
    }
    return collapsed;
}

std::string EscapeMarkdownScalar(const std::string& s)
{
    // Keep this intentionally small.  The path/name are still readable in
    // Markdown while avoiding accidental table/list structure from newlines.
    std::string out = s;
    for (char& c : out) {
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    }
    return out;
}

// Cheap byte-size chip for the read result.  Mirrors the style used by
// tool_read so the chips read consistently across tools.
std::string FormatByteSize(size_t bytes)
{
    if (bytes < 1024) return std::to_string(bytes) + " B";
    double kb = static_cast<double>(bytes) / 1024.0;
    if (kb < 1024.0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f KB", kb);
        return std::string(buf);
    }
    double mb = kb / 1024.0;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f MB", mb);
    return std::string(buf);
}

size_t CountLines(const std::string& s)
{
    if (s.empty()) return 0;
    size_t lines = 0;
    for (char c : s) {
        if (c == '\n') ++lines;
    }
    // If file doesn't end with a newline, count the trailing partial line.
    if (!s.empty() && s.back() != '\n') ++lines;
    return lines;
}

// ─── Atomic write of full new file body ────────────────────────────
//
// Mirrors the body-write loop in ChatHistory::SaveToFile.  Returns
// true on a fully-committed write, false if any stage failed (the
// staged temp is best-effort cleaned up on failure).

bool AtomicWriteFile(const std::string& finalPath, const std::string& body)
{
    tool_staged_write::StagedTempFile tmp =
        tool_staged_write::CreateStagedTempFile(finalPath);
    if (tmp.handle == INVALID_HANDLE_VALUE) return false;

    {
        const char* data   = body.data();
        size_t      remain = body.size();
        while (remain > 0) {
            DWORD chunk = (remain > 0x40000000U)
                              ? 0x40000000U
                              : static_cast<DWORD>(remain);
            DWORD written = 0;
            BOOL  ok = ::WriteFile(tmp.handle, data, chunk, &written, nullptr);
            if (!ok || written == 0) {
                ::CloseHandle(tmp.handle);
                ::DeleteFileW(tmp.wPath.c_str());
                return false;
            }
            data   += written;
            remain -= written;
        }
    }

    if (!::CloseHandle(tmp.handle)) {
        ::DeleteFileW(tmp.wPath.c_str());
        return false;
    }

    std::wstring wFinal = path_safety::Utf8ToWide(finalPath);
    if (wFinal.empty()) {
        ::DeleteFileW(tmp.wPath.c_str());
        return false;
    }

    if (!::MoveFileExW(tmp.wPath.c_str(), wFinal.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ::DeleteFileW(tmp.wPath.c_str());
        return false;
    }

    return true;
}

// Read a UTF-8 text file in full.  Returns true and fills `out` on
// success.  Missing file is treated as a distinct success state by the
// caller — see NotesRead's branch on wxFileExists.
bool ReadFileUtf8(const std::string& path, std::string& out)
{
    std::ifstream file(path_safety::Utf8ToWide(path), std::ios::in | std::ios::binary);
    if (!file.is_open()) return false;

    std::ostringstream ss;
    ss << file.rdbuf();
    out = ss.str();
    return true;
}

bool ReadNotesFile(const std::string& path, NotesResult& r)
{
    wxString wPath = wxString::FromUTF8(path);
    if (!wxFileExists(wPath)) {
        r.chips.push_back("no notes yet");
        return true;
    }

    std::string contents;
    if (!ReadFileUtf8(path, contents)) {
        r.errorBody = "Could not open notes file for reading: " + path;
        r.chips.push_back("failed");
        return false;
    }

    if (contents.empty()) {
        r.chips.push_back("no notes yet");
        return true;
    }

    r.body     = std::move(contents);
    r.bodyLang = "markdown";
    r.chips.push_back(FormatByteSize(r.body.size()));
    r.chips.push_back(std::to_string(CountLines(r.body)) + " lines");
    return true;
}

bool AppendEntryToFile(const std::string& path,
                       const std::string& parentDir,
                       const std::string& timestamp,
                       const std::string& body,
                       NotesResult&       r,
                       std::string*       appendedEntryOut,
                       std::size_t*       newSizeOut)
{
    if (!EnsureDirectory(parentDir)) {
        r.errorBody = "Could not create notes directory: " + parentDir;
        r.chips.push_back("failed");
        return false;
    }

    std::string existing;
    wxString wPath = wxString::FromUTF8(path);
    if (wxFileExists(wPath)) {
        if (!ReadFileUtf8(path, existing)) {
            r.errorBody = "Could not read existing notes file before append: " + path;
            r.chips.push_back("failed");
            return false;
        }
    }

    if (!existing.empty()) {
        size_t end = existing.size();
        while (end > 0 && existing[end - 1] == '\n') --end;
        existing.resize(end);
        existing += "\n\n";
    }

    std::ostringstream entry;
    entry << "## " << timestamp << "\n"
          << body << "\n";

    const std::string newBody = existing + entry.str();

    if (!AtomicWriteFile(path, newBody)) {
        r.errorBody = "Could not write notes file (atomic save failed): " + path;
        r.chips.push_back("failed");
        return false;
    }

    if (appendedEntryOut) *appendedEntryOut = entry.str();
    if (newSizeOut) *newSizeOut = newBody.size();
    return true;
}

std::string BuildGlobalProjectPointerEntry(const ToolContext& ctx,
                                           const std::string& projectNotesPath,
                                           const std::string& noteText)
{
    const std::string projectName = ctx.activeProjectName.empty()
        ? std::string("Active project")
        : ctx.activeProjectName;

    std::ostringstream ss;
    ss << "Project note saved\n"
       << "- Project: " << EscapeMarkdownScalar(projectName) << "\n"
       << "- Project notes: " << EscapeMarkdownScalar(projectNotesPath) << "\n"
       << "- Summary: " << OneLineSummary(noteText) << "\n"
       << "- Pointer only: the full note is stored in the project notes file.";
    return ss.str();
}

} // namespace

// ═══════════════════════════════════════════════════════════════════
//  Public entry points
// ═══════════════════════════════════════════════════════════════════

std::string GetNotesPath()
{
    return JoinPath(LlamaBossUserRootDir(), "NOTES.md");
}

NotesResult NotesRead(const ToolContext& /*ctx*/)
{
    NotesResult r;
    ReadNotesFile(GetNotesPath(), r);
    return r;
}

std::string GetProjectNotesPath(const ToolContext& ctx)
{
    const std::string dir = ProjectNotesDir(ctx);
    if (dir.empty()) return std::string();
    return JoinPath(dir, "NOTES.md");
}

NotesResult ProjectNotesRead(const ToolContext& ctx)
{
    NotesResult r;
    const std::string path = GetProjectNotesPath(ctx);
    if (path.empty()) {
        r.errorBody = "No active project is attached to this chat; project notes are unavailable.";
        r.chips.push_back("failed");
        return r;
    }

    ReadNotesFile(path, r);
    return r;
}

NotesResult ProjectNotesAppend(const std::string& entryText,
                               const ToolContext& ctx)
{
    NotesResult r;

    const std::string trimmed = Trim(entryText);
    if (trimmed.empty()) {
        r.errorBody = "project_notes_append requires a non-empty entry body";
        r.chips.push_back("failed");
        return r;
    }

    const std::string projectNotesPath = GetProjectNotesPath(ctx);
    const std::string projectNotesDir  = ProjectNotesDir(ctx);
    if (projectNotesPath.empty() || projectNotesDir.empty()) {
        r.errorBody = "No active project is attached to this chat; project notes are unavailable.";
        r.chips.push_back("failed");
        return r;
    }

    std::string projectEntry;
    std::size_t projectSize = 0;
    const std::string timestamp = LocalTimestampHeader();
    if (!AppendEntryToFile(projectNotesPath, projectNotesDir, timestamp, trimmed,
                           r, &projectEntry, &projectSize)) {
        return r;
    }

    r.body     = projectEntry;
    r.bodyLang = "markdown";
    r.chips.push_back("project note");
    r.chips.push_back(FormatByteSize(projectSize) + " total");
    return r;
}

NotesResult NotesAppend(const std::string& entryText,
                        const ToolContext& ctx)
{
    NotesResult r;

    const std::string trimmed = Trim(entryText);
    if (trimmed.empty()) {
        r.errorBody = "notes_append requires a non-empty entry body";
        r.chips.push_back("failed");
        return r;
    }

    const std::string timestamp = LocalTimestampHeader();

    // Project-aware behavior:
    //   - With an active project: full note goes to Project/Notes/NOTES.md,
    //     while global NOTES.md receives a compact pointer/index entry.
    //   - Without an active project: preserve the original global-only behavior.
    const bool hasActiveProject = !GetProjectNotesPath(ctx).empty();
    if (hasActiveProject) {
        const std::string projectNotesPath = GetProjectNotesPath(ctx);
        const std::string projectNotesDir  = ProjectNotesDir(ctx);

        std::string projectEntry;
        std::size_t projectSize = 0;
        if (!AppendEntryToFile(projectNotesPath, projectNotesDir, timestamp, trimmed,
                               r, &projectEntry, &projectSize)) {
            return r;
        }

        const std::string pointerBody = BuildGlobalProjectPointerEntry(ctx, projectNotesPath, trimmed);
        std::string pointerEntry;
        std::size_t globalSize = 0;
        if (!AppendEntryToFile(GetNotesPath(), LlamaBossUserRootDir(), timestamp, pointerBody,
                               r, &pointerEntry, &globalSize)) {
            return r;
        }

        std::ostringstream body;
        body << "Project notes entry:\n\n"
             << projectEntry
             << "\nGlobal NOTES.md pointer:\n\n"
             << pointerEntry;

        r.body     = body.str();
        r.bodyLang = "markdown";
        r.chips.push_back("project note");
        r.chips.push_back("global pointer");
        r.chips.push_back(FormatByteSize(projectSize) + " project");
        r.chips.push_back(FormatByteSize(globalSize) + " global");
        return r;
    }

    std::string globalEntry;
    std::size_t globalSize = 0;
    if (!AppendEntryToFile(GetNotesPath(), LlamaBossUserRootDir(), timestamp, trimmed,
                           r, &globalEntry, &globalSize)) {
        return r;
    }

    // Success — show what was appended in the body so the user (and the
    // model) can see the entry that just landed, and chip with the new
    // file size for context.
    r.body     = globalEntry;
    r.bodyLang = "markdown";
    r.chips.push_back("appended");
    r.chips.push_back(FormatByteSize(globalSize) + " total");
    return r;
}
