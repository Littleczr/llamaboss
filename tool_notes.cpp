// tool_notes.cpp -- see tool_notes.h.

#include "tool_notes.h"
#include "path_safety.h"
#include "lb_string_utils.h"     // LbUtf8SafeTruncate
#include "tool_staged_write.h"   // CreateStagedTempFile, atomic save
#include "tool_mutation_guard.h" // shared native-mutation lock + reparse-point checks
#include "skill_authoring_support.h" // LbReadSkillFrontmatterDescription (YAML-aware)

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "lb_windows.h"

namespace {

// ─── Limits ────────────────────────────────────────────────────────

// notes_append accepts one short fact.  Anything longer belongs in its own
// subject file (created with write), which keeps paths.md cheap to read.
constexpr std::size_t kMaxEntryChars = 300;   // Unicode code points
constexpr std::size_t kMaxEntryLines = 3;

// Prompt budget for the index lines.  Past this the index switches to a
// compact form (root files + per-folder counts).
constexpr std::size_t kIndexFullBudgetBytes    = 2048;
constexpr std::size_t kIndexCompactRootBytes   = 1200;
constexpr std::size_t kDescriptionMaxBytes     = 160;

const char* const kDefaultNotesFile = "paths.md";
const char* const kDefaultNotesFileHeader =
    "---\n"
    "description: Saved paths, names, handles, and short facts (one per line)\n"
    "---\n"
    "\n";

// ─── Path helpers ──────────────────────────────────────────────────

// wxFILE_SEP_PATH is a wxChar (wchar_t in Unicode builds); streaming it
// into a std::ostream would print its numeric value, so narrow it once.
const char kSep = static_cast<char>(wxFILE_SEP_PATH);

std::string JoinPath(const std::string& a, const std::string& b)
{
    if (a.empty()) return b;
    if (a.back() == '/' || a.back() == '\\') return a + b;
    return a + std::string(1, kSep) + b;
}

bool EnsureDirectory(const std::string& dir)
{
    if (dir.empty()) return false;
    wxString wDir = wxString::FromUTF8(dir);
    if (wxDirExists(wDir)) return true;
    return wxFileName::Mkdir(wDir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
}

std::string ToUtf8(const wxString& s)
{
    const wxScopedCharBuffer b = s.ToUTF8();
    return b.data() ? std::string(b.data()) : std::string();
}

bool EndsWithMd(const std::string& name)
{
    const std::string lower = LbLowerAscii(name);
    return lower.size() > 3 && lower.compare(lower.size() - 3, 3, ".md") == 0;
}

// ─── Text helpers ──────────────────────────────────────────────────

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

std::size_t Utf8CodePoints(const std::string& s)
{
    std::size_t n = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) ++n;
    }
    return n;
}

std::string RemoveCarriageReturns(std::string s)
{
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
}

std::string CollapseWhitespaceOneLine(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    bool priorSpace = false;
    for (char c : s) {
        const bool space = (c == ' ' || c == '\t' || c == '\r' || c == '\n');
        if (space) {
            if (!priorSpace && !out.empty()) out.push_back(' ');
            priorSpace = true;
        } else {
            out.push_back(c);
            priorSpace = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string ClipForIndex(const std::string& s)
{
    std::string line = CollapseWhitespaceOneLine(s);
    if (line.size() > kDescriptionMaxBytes) {
        // UTF-8-safe cut: this text lands in the system prompt, and a
        // split multi-byte character makes llama-server reject the request.
        line = LbUtf8SafeTruncate(line, kDescriptionMaxBytes) + "...";
    }
    return line;
}

std::string FormatByteSize(std::uint64_t bytes)
{
    if (bytes < 1024) return std::to_string(bytes) + " B";
    const double kb = static_cast<double>(bytes) / 1024.0;
    char buf[32];
    if (kb < 1024.0) {
        std::snprintf(buf, sizeof(buf), "%.1f KB", kb);
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f MB", kb / 1024.0);
    }
    return std::string(buf);
}

// ─── File I/O ──────────────────────────────────────────────────────

bool ReadFileUtf8(const std::string& path, std::string& out, std::size_t maxBytes = 0)
{
    std::ifstream file(std::filesystem::path(path_safety::Utf8ToWide(path)), std::ios::in | std::ios::binary);
    if (!file.is_open()) return false;

    if (maxBytes == 0) {
        std::ostringstream ss;
        ss << file.rdbuf();
        out = ss.str();
        return true;
    }

    out.assign(maxBytes, '\0');
    file.read(&out[0], static_cast<std::streamsize>(maxBytes));
    out.resize(static_cast<std::size_t>(file.gcount()));
    return true;
}

// Atomic whole-file replace: staged temp + MoveFileExW, matching
// ChatHistory::SaveToFile.  Staged temp is cleaned up on any failure.
bool AtomicWriteFile(const std::string& finalPath, const std::string& body)
{
#ifndef _WIN32
    return tool_staged_write::AtomicReplaceFile(finalPath, body, /*durable=*/false);
#else
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
#endif
}

// ─── Index helpers ─────────────────────────────────────────────────

// Description for the index: frontmatter `description:` when present,
// otherwise the first content line (markdown markers stripped).
std::string DescribeNoteFile(const std::string& path)
{
    // Same frontmatter reader as SKILL.md descriptions: handles quoted,
    // folded (>-) and literal (|) values and cuts UTF-8 safely.
    {
        const std::string fm = LbReadSkillFrontmatterDescription(path, kDescriptionMaxBytes);
        if (!fm.empty()) return CollapseWhitespaceOneLine(fm);
    }

    std::string head;
    if (!ReadFileUtf8(path, head, 8192)) return std::string();
    head = RemoveCarriageReturns(head);
    if (head.size() >= 3 &&
        static_cast<unsigned char>(head[0]) == 0xEF &&
        static_cast<unsigned char>(head[1]) == 0xBB &&
        static_cast<unsigned char>(head[2]) == 0xBF) {
        head.erase(0, 3);   // UTF-8 BOM
    }

    std::istringstream lines(head);
    std::string line;
    bool inFrontmatter = false;
    bool sawFirstLine  = false;

    while (std::getline(lines, line)) {
        const std::string t = Trim(line);

        if (!sawFirstLine) {
            sawFirstLine = true;
            if (t == "---") { inFrontmatter = true; continue; }
        }

        if (inFrontmatter) {
            if (t == "---") { inFrontmatter = false; continue; }
            if (LbLowerAscii(t).rfind("description:", 0) == 0) {
                std::string v = Trim(t.substr(12));
                if (v.size() >= 2 &&
                    ((v.front() == '"' && v.back() == '"') ||
                     (v.front() == '\'' && v.back() == '\''))) {
                    v = v.substr(1, v.size() - 2);
                }
                if (!Trim(v).empty()) return ClipForIndex(v);
            }
            continue;
        }

        // Fallback: first content line.
        std::string s = t;
        size_t k = 0;
        while (k < s.size() && (s[k] == '#' || s[k] == '-' || s[k] == '*' ||
                                s[k] == '>' || s[k] == ' ' || s[k] == '\t'))
            ++k;
        s = Trim(s.substr(k));
        if (!s.empty()) return ClipForIndex(s);
    }
    return std::string();
}

void ListMdFilesIn(const std::string& dirPath,
                   const std::string& relPrefix,
                   std::vector<NoteFileInfo>& out)
{
    if (!wxDirExists(wxString::FromUTF8(dirPath))) return;
    wxDir dir(wxString::FromUTF8(dirPath));
    if (!dir.IsOpened()) return;

    wxString name;
    bool cont = dir.GetFirst(&name, wxEmptyString, wxDIR_FILES);   // no hidden
    while (cont) {
        const std::string fileName = ToUtf8(name);
        if (!fileName.empty() && fileName[0] != '.' && EndsWithMd(fileName)) {
            NoteFileInfo info;
            info.name = relPrefix + fileName;
            info.path = JoinPath(dirPath, fileName);
            const wxULongLong size = wxFileName::GetSize(wxString::FromUTF8(info.path));
            info.sizeBytes = (size == wxInvalidSize) ? 0 : size.GetValue();
            out.push_back(std::move(info));
        }
        cont = dir.GetNext(&name);
    }
}

std::vector<std::string> ListSubfolders(const std::string& dirPath)
{
    std::vector<std::string> folders;
    if (!wxDirExists(wxString::FromUTF8(dirPath))) return folders;
    wxDir dir(wxString::FromUTF8(dirPath));
    if (!dir.IsOpened()) return folders;

    wxString name;
    bool cont = dir.GetFirst(&name, wxEmptyString, wxDIR_DIRS);    // no hidden
    while (cont) {
        const std::string folder = ToUtf8(name);
        if (!folder.empty() && folder[0] != '.') folders.push_back(folder);
        cont = dir.GetNext(&name);
    }
    std::sort(folders.begin(), folders.end());
    return folders;
}

bool IsRootFile(const NoteFileInfo& f)
{
    return f.name.find_first_of("\\/") == std::string::npos;
}

// ─── notes_append argument handling ───────────────────────────────

struct AppendArgs {
    std::string relFile;          // normalized, native separators
    bool        explicitFile = false;
    std::string entry;            // trimmed, \r removed
    std::string error;
};

// Validates a notes-relative file name: at most "folder\file.md", no
// traversal, no drive or root prefixes, no reserved characters.  A name
// without an extension gets ".md".
bool NormalizeNotesRelPath(const std::string& input, std::string& out, std::string& err)
{
    std::string s = Trim(input);
    if (s.size() >= 2 &&
        ((s.front() == '"' && s.back() == '"') ||
         (s.front() == '\'' && s.back() == '\'') ||
         (s.front() == '`' && s.back() == '`'))) {
        s = Trim(s.substr(1, s.size() - 2));
    }

    // Tolerate a "Notes\" prefix -- models often repeat the folder name.
    {
        const std::string lower = LbLowerAscii(s);
        if (lower.rfind("notes\\", 0) == 0 || lower.rfind("notes/", 0) == 0)
            s = s.substr(6);
    }

    if (s.empty()) { err = "empty file name"; return false; }
    if (s.find(':') != std::string::npos || s[0] == '\\' || s[0] == '/') {
        err = "use a name relative to the Notes folder (for example paths.md or mac-mini\\setup-log.md), not an absolute path";
        return false;
    }

    std::vector<std::string> parts;
    {
        std::string cur;
        for (char c : s) {
            if (c == '\\' || c == '/') { parts.push_back(cur); cur.clear(); }
            else cur.push_back(c);
        }
        parts.push_back(cur);
    }

    if (parts.size() > 2) {
        err = "notes files may be at most one subfolder deep (folder\\file.md)";
        return false;
    }
    for (std::string& p : parts) {
        p = Trim(p);
        if (p.empty() || p == "." || p == ".." || p[0] == '.') {
            err = "invalid path component in '" + s + "'";
            return false;
        }
        for (unsigned char c : p) {
            if (c < 0x20 || c == '<' || c == '>' || c == '"' || c == '|' ||
                c == '?' || c == '*') {
                err = "file name contains a character Windows does not allow";
                return false;
            }
        }
    }

    std::string& fileName = parts.back();
    if (fileName.find('.') == std::string::npos) fileName += ".md";
    if (!EndsWithMd(fileName)) {
        err = "notes files must be .md files";
        return false;
    }

    out = parts[0];
    for (size_t i = 1; i < parts.size(); ++i) {
        out += std::string(1, kSep) + parts[i];
    }
    return true;
}

AppendArgs ParseAppendArgs(const std::string& raw)
{
    AppendArgs a;
    std::string text = RemoveCarriageReturns(raw);

    // Optional first line: "file: <relative>.md"
    {
        const size_t lead = text.find_first_not_of(" \t\n");
        if (lead != std::string::npos) {
            const size_t eol = text.find('\n', lead);
            const std::string first =
                Trim(text.substr(lead, eol == std::string::npos ? std::string::npos : eol - lead));
            if (LbLowerAscii(first).rfind("file:", 0) == 0) {
                std::string norm, err;
                if (!NormalizeNotesRelPath(first.substr(5), norm, err)) {
                    a.error = "notes_append: bad 'file:' line -- " + err + ".";
                    return a;
                }
                a.relFile      = norm;
                a.explicitFile = true;
                text = (eol == std::string::npos) ? std::string() : text.substr(eol + 1);
            }
        }
    }

    if (a.relFile.empty()) a.relFile = kDefaultNotesFile;
    a.entry = Trim(text);
    return a;
}

// "- first line\n  second line\n  third line"
std::string FormatBullet(const std::string& entry)
{
    std::istringstream lines(entry);
    std::string line;
    std::ostringstream out;
    bool first = true;
    while (std::getline(lines, line)) {
        line = Trim(line);
        if (line.empty()) continue;
        out << (first ? "- " : "\n  ") << line;
        first = false;
    }
    return out.str();
}

std::size_t CountNonEmptyLines(const std::string& s)
{
    std::istringstream lines(s);
    std::string line;
    std::size_t n = 0;
    while (std::getline(lines, line)) {
        if (!Trim(line).empty()) ++n;
    }
    return n;
}

} // namespace

// ═══════════════════════════════════════════════════════════════════
//  Public entry points
// ═══════════════════════════════════════════════════════════════════

std::string GetLlamaBossUserRootDir()
{
#ifdef __WXMSW__
    wxString userProfile;
    if (wxGetEnv("USERPROFILE", &userProfile) && !userProfile.IsEmpty()) {
        return JoinPath(ToUtf8(userProfile), "LlamaBoss");
    }
#endif

    wxString home = wxGetHomeDir();
    if (!home.IsEmpty()) {
        return JoinPath(ToUtf8(home), "LlamaBoss");
    }

    wxString docs = wxStandardPaths::Get().GetDocumentsDir();
    return JoinPath(ToUtf8(docs), "LlamaBoss");
}

std::string GetNotesRootDir()
{
    return JoinPath(GetLlamaBossUserRootDir(), "Notes");
}

std::string GetLegacyNotesPath()
{
    return JoinPath(GetLlamaBossUserRootDir(), "NOTES.md");
}

std::vector<NoteFileInfo> ListNoteFiles()
{
    std::vector<NoteFileInfo> files;
    const std::string root = GetNotesRootDir();

    ListMdFilesIn(root, std::string(), files);
    std::sort(files.begin(), files.end(),
              [](const NoteFileInfo& a, const NoteFileInfo& b) {
                  return LbLowerAscii(a.name) < LbLowerAscii(b.name);
              });

    for (const std::string& folder : ListSubfolders(root)) {
        std::vector<NoteFileInfo> sub;
        ListMdFilesIn(JoinPath(root, folder),
                      folder + std::string(1, kSep), sub);
        std::sort(sub.begin(), sub.end(),
                  [](const NoteFileInfo& a, const NoteFileInfo& b) {
                      return LbLowerAscii(a.name) < LbLowerAscii(b.name);
                  });
        files.insert(files.end(), sub.begin(), sub.end());
    }
    return files;
}

std::string BuildNotesIndexBlock()
{
    return BuildNotesIndexBlock(ListNoteFiles());
}

std::string BuildNotesIndexBlock(const std::vector<NoteFileInfo>& files)
{
    const std::string root = GetNotesRootDir();

    std::ostringstream p;
    p << "User notes (cross-conversation memory; only this index is loaded -- read a file only when the task needs it):\n"
      << "  Folder: " << root << "\n";

    if (files.empty()) {
        p << "  (no notes files yet -- notes_append creates paths.md on the first save)\n";
    } else {
        std::vector<std::string> lines;
        lines.reserve(files.size());
        std::size_t total = 0;
        for (const NoteFileInfo& f : files) {
            std::string desc = DescribeNoteFile(f.path);
            if (desc.empty()) desc = "(no description)";
            lines.push_back("    - " + f.name + " -- " + desc + "\n");
            total += lines.back().size();
        }

        if (total <= kIndexFullBudgetBytes) {
            p << "  Files (name relative to the folder -- description):\n";
            for (const std::string& line : lines) p << line;
        } else {
            // Compact form: root files with descriptions up to a budget,
            // then one line per subfolder with its file count.
            p << "  Files (index is large, so subfolders are summarized -- ls a subfolder to see its files):\n";
            std::size_t used = 0;
            std::size_t rootSkipped = 0;
            for (size_t i = 0; i < files.size(); ++i) {
                if (!IsRootFile(files[i])) continue;
                if (used + lines[i].size() > kIndexCompactRootBytes) { ++rootSkipped; continue; }
                p << lines[i];
                used += lines[i].size();
            }
            if (rootSkipped > 0) {
                p << "    - (" << rootSkipped << " more files in the folder root -- ls the folder)\n";
            }

            std::string currentFolder;
            std::size_t count = 0;
            auto flush = [&]() {
                if (!currentFolder.empty()) {
                    p << "    - " << currentFolder << kSep << " ("
                      << count << (count == 1 ? " file" : " files") << ")\n";
                }
            };
            for (const NoteFileInfo& f : files) {
                if (IsRootFile(f)) continue;
                const std::string folder = f.name.substr(0, f.name.find_first_of("\\/"));
                if (folder != currentFolder) { flush(); currentFolder = folder; count = 0; }
                ++count;
            }
            flush();
        }
    }

    if (wxFileExists(wxString::FromUTF8(GetLegacyNotesPath()))) {
        p << "  Legacy notes (older single-file notes, not yet sorted into the folder): "
          << GetLegacyNotesPath()
          << " -- read it only when the notes files above do not answer.\n";
    }
    return p.str();
}

const char* NotesAgentPromptGuidance()
{
    return
        "Notes (cross-conversation memory):\n"
        "  The user's notes are small markdown files in their Notes folder, one subject per file. The folder path and a one-line index of every file are in the 'User notes' section at the end of this prompt. Only the index is loaded: open a notes file with read, using its full path (folder + file name), only when the task needs it.\n"
        "  Check notes first when: the user references their notes or asks whether you remember something; the user uses a possessive handle you have no context for (\"my LISA workflow\", \"the mac mini share\", \"my work folder\"); or the user asks about their code, projects, or saved locations (\"my repo\", \"my movies folder\") -- read the matching file (usually paths.md) before ls or guessing paths. Also check notes once when ls/read comes back empty for a likely path, before asking the user. Skip notes for greetings and casual chat.\n"
        "  Save ONLY when the user explicitly asks (\"save this to notes\", \"remember that\", \"from now on my X is Y\"). notes_append saves one short fact (at most 300 characters, 3 lines) -- a path, name, handle, or preference -- to paths.md, or to another existing notes file named on a first args line 'file: <name>.md'. It refuses longer text. Longer material (setup history, project details, procedures) goes in its own notes file, at most one subfolder deep (for example mac-mini\\setup-log.md or projects\\<project>.md), created with write and starting with a frontmatter block: a line ---, a line description: <one line saying what the file covers>, then a line ---. The index picks up new files and descriptions automatically.\n"
        "  To correct or remove a saved fact, edit that line in its file -- never append a correction. Never save conversation summaries or session reports. Repeatable multi-step procedures belong in a Skill, not in notes.\n";
}

NotesResult NotesAppend(const std::string& args, const ToolContext& /*ctx*/)
{
    NotesResult r;

    const AppendArgs a = ParseAppendArgs(args);
    if (!a.error.empty()) {
        r.errorBody = a.error;
        r.chips.push_back("failed");
        return r;
    }
    if (a.entry.empty()) {
        r.errorBody = "notes_append requires a non-empty entry";
        r.chips.push_back("failed");
        return r;
    }

    const std::size_t chars = Utf8CodePoints(a.entry);
    const std::size_t lines = CountNonEmptyLines(a.entry);
    if (chars > kMaxEntryChars || lines > kMaxEntryLines) {
        std::ostringstream e;
        e << "notes_append holds short facts only (max " << kMaxEntryChars
          << " characters and " << kMaxEntryLines << " lines; this entry has "
          << chars << " characters, " << lines << " lines). Nothing was saved.\n"
          << "If this is one fact, shorten it. If it is longer material (setup history, "
             "project details, a procedure), put it in its own notes file instead: create "
             "it with write under " << GetNotesRootDir()
          << " (for example mac-mini\\setup-log.md), starting with a frontmatter "
             "description line. The notes index lists it automatically.";
        r.errorBody = e.str();
        r.chips.push_back("too long");
        return r;
    }

    const std::string root = GetNotesRootDir();
    const std::string path = JoinPath(root, a.relFile);
    const bool exists = wxFileExists(wxString::FromUTF8(path));

    if (!exists && a.explicitFile && a.relFile != kDefaultNotesFile) {
        r.errorBody = "Notes file '" + a.relFile + "' does not exist. Nothing was saved.\n"
                      "Short facts go to paths.md (omit the file: line). To start a new "
                      "subject file, create it with write under " + root +
                      " with a frontmatter description, then append to it.";
        r.chips.push_back("failed");
        return r;
    }

    if (!EnsureDirectory(root)) {
        r.errorBody = "Could not create the notes folder: " + root;
        r.chips.push_back("failed");
        return r;
    }

    // Same protections as edit/write: the process-wide native-mutation lock
    // (so this never interleaves with a guarded edit of the same file), the
    // parent-directory pin that refuses junctions/symlinks/reparse points
    // anywhere on the path, and an unchanged-target check before commit.
    tool_mutation_guard::Guard mutation;
    if (!mutation.Begin(path)) {
        r.errorBody = mutation.Error() + "\nNothing was saved.";
        r.chips.push_back(mutation.MissingFolder() ? "failed" : "blocked");
        return r;
    }

    std::string existing;
    if (mutation.TargetExisted() && !ReadFileUtf8(path, existing)) {
        r.errorBody = "Could not read notes file before appending: " + path;
        r.chips.push_back("failed");
        return r;
    }

    const std::string bullet = FormatBullet(a.entry);

    // Exact duplicate: report success without writing a second copy.
    {
        const std::string normalized = RemoveCarriageReturns(existing);
        const size_t at = normalized.find(bullet);
        if (at != std::string::npos &&
            (at == 0 || normalized[at - 1] == '\n') &&
            (at + bullet.size() == normalized.size() || normalized[at + bullet.size()] == '\n')) {
            r.body     = bullet + "\n";
            r.bodyLang = "markdown";
            r.chips.push_back("already saved");
            r.chips.push_back(a.relFile);
            return r;
        }
    }

    std::string newBody = existing;
    if (newBody.empty()) {
        if (a.relFile == kDefaultNotesFile) newBody = kDefaultNotesFileHeader;
    } else if (newBody.back() != '\n') {
        newBody += "\n";
    }
    newBody += bullet + "\n";

    if (!mutation.VerifyUnchanged(mutation.TargetExisted() ? &existing : nullptr)) {
        r.errorBody = mutation.Error() + "\nNothing was saved.";
        r.chips.push_back("conflict");
        return r;
    }
    mutation.ReleaseTargetForCommit();

    if (!AtomicWriteFile(path, newBody)) {
        r.errorBody = "Could not write notes file (atomic save failed): " + path;
        r.chips.push_back("failed");
        return r;
    }

    r.body     = bullet + "\n";
    r.bodyLang = "markdown";
    r.chips.push_back("saved");
    r.chips.push_back(a.relFile);
    r.chips.push_back(FormatByteSize(newBody.size()) + " total");
    return r;
}
