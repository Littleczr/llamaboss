// tool_write.cpp

#include "tool_write.h"
#include "tool_path.h"
#include "tool_path_safety.h"    // IsUnderAllowedWriteRoot, Basename, ParentDir
#include "tool_staged_write.h"   // StagedTempFile, CreateStagedTempFile
#include "tool_open.h"           // ClassifyForOpen, FileRisk
#include "tool_python_syntax.h"  // tool_python_syntax::CheckFile
#include "path_safety.h"
#include "tool_mutation_guard.h"

#include "lb_windows.h"

namespace {

// ─── Limits ──────────────────────────────────────────────────────
// kWriteMaxBytes is the hard ceiling on the content of a single
// write call.  Matches the spirit of tool_read's caps -- anything
// bigger than this is suspicious for a single tool call from a
// local model, and the upper bound on a "polished" hand-emitted
// file is well under this.
constexpr size_t kWriteMaxBytes = 1 * 1024 * 1024;  // 1 MiB

// ─── Helpers (file-local) ────────────────────────────────────────

std::string HumanBytes(size_t b)
{
    std::ostringstream ss;
    ss << std::fixed;
    if (b < 1024) {
        ss << b << " B";
    } else if (b < 1024 * 1024) {
        ss.precision(1);
        ss << (b / 1024.0) << " KB";
    } else {
        ss.precision(2);
        ss << (b / (1024.0 * 1024.0)) << " MB";
    }
    return ss.str();
}

std::string ElapsedChip(std::chrono::steady_clock::time_point t0)
{
    double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();
    std::ostringstream ts;
    ts << std::fixed;
    ts.precision(elapsed < 10.0 ? 2 : 1);
    ts << elapsed << "s";
    return ts.str();
}

size_t CountLines(const std::string& s)
{
    if (s.empty()) return 0;
    size_t n = 0;
    for (char c : s) if (c == '\n') ++n;
    if (s.back() != '\n') ++n;
    return n;
}

// Split `argsBlob` into (path, content).  The first line is the
// path; everything after the first '\n' is content.  Strips the
// path of trailing '\r' / spaces / tabs (a stray line ending or the
// model padding the path with whitespace before the newline).
//
// Re-adds a trailing newline to non-empty content if missing -- the
// parser's Trim() peels off the wrapping newline before </args>, so
// a content "foo" round-trips to "foo\n" on disk.  Empty content
// stays empty (zero-byte file).
void SplitPathAndContent(const std::string& argsBlob,
                         std::string&       pathOut,
                         std::string&       contentOut)
{
    size_t nl = argsBlob.find('\n');
    if (nl == std::string::npos) {
        pathOut    = argsBlob;
        contentOut.clear();
    } else {
        pathOut    = argsBlob.substr(0, nl);
        contentOut = argsBlob.substr(nl + 1);
    }

    // Strip trailing whitespace from the path line.  Leading
    // whitespace on the path is left intact -- if the model
    // intentionally indented its path that's a model-side bug,
    // not something this tool covers up.
    while (!pathOut.empty()) {
        char c = pathOut.back();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') pathOut.pop_back();
        else break;
    }

    if (!contentOut.empty() && contentOut.back() != '\n') {
        contentOut += '\n';
    }
}


bool HasPythonExtension(const std::string& absPath)
{
    std::string lower = absPath;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return lower.size() >= 3 &&
           (lower.rfind(".py") == lower.size() - 3 ||
            (lower.size() >= 4 && lower.rfind(".pyw") == lower.size() - 4));
}

bool HasPowerShellScriptExtension(const std::string& absPath)
{
    std::string lower = absPath;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return lower.size() >= 4 && lower.rfind(".ps1") == lower.size() - 4;
}

enum class WriteRiskMode {
    NormalTextOnly,
    ApprovedPowerShellScript,
};

// Shared implementation behind WriteNewFile / OverwriteFileContent.
// Lives in the anonymous namespace (internal linkage): only the two
// public wrappers below call it, and it is not declared in
// tool_write.h.
WriteResult WriteFileContent(const std::string& argsBlob,
                             const ToolContext& ctx,
                             bool overwriteExisting,
                             WriteRiskMode riskMode = WriteRiskMode::NormalTextOnly)
{
    WriteResult r;
    auto t0 = std::chrono::steady_clock::now();

    // ── Parse ────────────────────────────────────────────────────
    std::string requestedPath, content;
    SplitPathAndContent(argsBlob, requestedPath, content);

    if (requestedPath.empty()) {
        r.chips.push_back("failed");
        r.errorBody = overwriteExisting
            ? "overwrite_file requires a path on the first line of <args>."
            : "write requires a path on the first line of <args>.";
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    // ── Size cap ─────────────────────────────────────────────────
    if (content.size() > kWriteMaxBytes) {
        r.chips.push_back("too large");
        r.errorBody = "Content exceeds write cap: " +
                      HumanBytes(content.size()) +
                      " (max " + HumanBytes(kWriteMaxBytes) + ").";
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    // ── Resolve path against the conversation CWD ────────────────
    if (ctx.cwd.empty()) {
        r.chips.push_back("blocked");
        r.errorBody = "No working directory set; refuse to write.";
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    std::string resolved = tool_path_safety::ResolveProjectAwareToolPath(requestedPath, ctx.cwd, ctx.activeProjectRoot);
    if (resolved.empty()) {
        r.chips.push_back("failed");
        r.errorBody = "Could not resolve path: " + requestedPath;
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    // ── Containment: must land inside ctx.cwd ────────────────────
    // This is where "../../etc/passwd"-style traversal dies, even
    // though GetFullPathNameW happily resolved it -- the resolved
    // absolute path won't start with ctx.cwd anymore.
    if (!tool_path_safety::IsUnderAllowedWriteRoot(
            resolved, ctx.cwd, ctx.activeProjectRoot, ctx.skillsRoot,
            ctx.additionalWriteRoots)) {
        r.chips.push_back("blocked");
        r.errorBody = "Refuses to write outside the allowed write roots."
                      "\n  resolved: " + resolved +
                      tool_path_safety::AllowedWriteRootsDiagnostic(
                          ctx.cwd, ctx.activeProjectRoot, ctx.skillsRoot,
                          ctx.additionalWriteRoots);
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    // Coordinate native mutations across chat windows and pin the real
    // directory chain before touching the target. Reparse points fail closed.
    tool_mutation_guard::Guard mutation;
    if (!mutation.Begin(resolved)) {
        r.chips.push_back("blocked");
        r.errorBody = mutation.Error();
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    // ── Filename sanitization ────────────────────────────────────
    // The basename of the resolved path must survive
    // SanitizeFilename unchanged -- otherwise we'd be silently
    // renaming the model's chosen filename, which is a footgun
    // (the model later "reads" the file under its requested name
    // and gets a not-found error).  Reject and let the model retry
    // with a safe name.
    std::string basename = tool_path_safety::Basename(resolved);
    std::string sanitized = path_safety::SanitizeFilename(basename, "");
    if (sanitized.empty() || sanitized != basename) {
        r.chips.push_back("blocked");
        r.errorBody = "Filename '" + basename + "' has characters or "
                      "a shape that aren't safe on Windows. "
                      "Try '" + (sanitized.empty() ? "another name"
                                                   : sanitized) + "'.";
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    // ── Risky-extension block ────────────────────────────────────
    // Reuses the open tool's kill-list (.exe / .bat / .ps1 / .reg
    // / .lnk / .vbs / macro Office / etc.) so write and open agree
    // on what's dangerous.  TextLike and Safe both pass.
    //
    // Project-generation workflows still need reviewable build/run
    // scripts.  Those do NOT bypass the normal write/overwrite_file
    // block; they enter through write_powershell_script, which routes
    // through approval first and then lands here with the explicit
    // ApprovedPowerShellScript mode.  Keep that lane narrow: .ps1 only,
    // no .bat/.cmd/.exe or generic risky extension pass-through.
    if (riskMode == WriteRiskMode::ApprovedPowerShellScript) {
        if (!HasPowerShellScriptExtension(resolved)) {
            r.chips.push_back("blocked");
            r.errorBody = "write_powershell_script only creates or replaces .ps1 files.";
            r.chips.push_back(ElapsedChip(t0));
            return r;
        }
    } else if (ClassifyForOpen(resolved) == FileRisk::Risky) {
        r.chips.push_back("blocked");
        r.errorBody = overwriteExisting
            ? "Refuses to overwrite files with executable or scriptable extensions. Use write_powershell_script for approved .ps1 project scripts, or edit the file manually."
            : "Refuses to create files with executable or scriptable extensions. Use write_powershell_script for approved .ps1 project scripts, or drop the file in manually.";
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    // ── Parent directory must already exist ──────────────────────
    // Single operations only; no implicit "mkdir -p".  The model
    // can call mkdir explicitly (or check with ls) before writing
    // into a new directory.
    std::string parent = tool_path_safety::ParentDir(resolved);
    if (parent.empty() || !IsDirectory(parent)) {
        r.chips.push_back("failed");
        r.errorBody = "Parent directory does not exist: " + parent +
                      ".\nUse mkdir to create it first.";
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    // ── Pre-check: create-new refuses existing targets; overwrite_file
    // replaces existing regular files but still refuses directories.
    const bool targetIsFile = IsFile(resolved);
    const bool targetIsDir  = IsDirectory(resolved);
    if (!overwriteExisting && (targetIsFile || targetIsDir)) {
        r.chips.push_back("exists");
        r.errorBody = "File or directory already exists: " + resolved +
                      ".\nUse overwrite_file to replace the whole file, or edit for a small unique text replacement.";
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }
    if (overwriteExisting && targetIsDir) {
        r.chips.push_back("blocked");
        r.errorBody = "Refuses to overwrite a directory: " + resolved;
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    // Never truncate a non-empty file to zero bytes.  Empty content here
    // is almost always a malformed call (path line only, a misnamed
    // content field) rather than a request to blank the file.
    if (overwriteExisting && targetIsFile && content.empty()) {
#ifdef _WIN32
        std::wstring wTarget = path_safety::Utf8ToWide(resolved);
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!wTarget.empty() &&
            ::GetFileAttributesExW(wTarget.c_str(), GetFileExInfoStandard, &fad) &&
            (fad.nFileSizeHigh != 0 || fad.nFileSizeLow != 0)) {
#else
        struct stat st {};
        if (::stat(resolved.c_str(), &st) == 0 && st.st_size != 0) {
#endif
            r.chips.push_back("blocked");
            r.errorBody = "overwrite_file received EMPTY content for an existing "
                          "non-empty file, so nothing was changed: " + resolved +
                          "\nSend the full new file contents (native calls: the "
                          "\"content\" string parameter; text calls: content on "
                          "the lines after the path). To make the file empty on "
                          "purpose, delete it and write it again.";
            r.chips.push_back(ElapsedChip(t0));
            return r;
        }
    }

    // ── Atomic-ish write: unique sibling temp then rename ─────────
    // Step 1: create a fresh temp file with CREATE_NEW so we never
    // overwrite a real user-owned "<path>.tmp" file.  Step 2: write
    // the bytes. Step 3: rename within the same pinned parent -- create-new
    // refuses replacement; overwrite_file may atomically replace the verified
    // existing target. A target that appeared during staging still fails.
    // No partial real file is ever visible.
    std::wstring wFinal = path_safety::Utf8ToWide(resolved);
    if (wFinal.empty()) {
        r.chips.push_back("failed");
        r.errorBody = "Path conversion failed: " + resolved;
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

#ifdef _WIN32
    tool_staged_write::StagedTempFile tmp =
        tool_staged_write::CreateStagedTempFile(resolved);
    if (tmp.handle == INVALID_HANDLE_VALUE) {
        r.chips.push_back("failed");
        r.errorBody = "Could not create unique temp file near '" +
                      resolved + "' for writing (Win32 error " +
                      std::to_string(tmp.error) + ").";
        if (!tmp.path.empty()) {
            r.errorBody += "\nLast attempted temp path: " + tmp.path;
        }
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    std::string&  tmpPath = tmp.path;
    std::wstring& wTmp    = tmp.wPath;
    HANDLE        hFile   = tmp.handle;

    // Write in one shot. WriteFile may short-write, so loop until
    // the whole buffer is flushed.  An empty content (zero-byte
    // file) skips the loop entirely and we just close the handle.
    {
        const char* data    = content.data();
        size_t      remain  = content.size();
        while (remain > 0) {
            DWORD chunk    = (remain > 0x40000000U)
                                 ? 0x40000000U
                                 : static_cast<DWORD>(remain);
            DWORD written  = 0;
            BOOL  ok = ::WriteFile(hFile, data, chunk, &written, nullptr);
            if (!ok || written == 0) {
                DWORD err = ::GetLastError();
                ::CloseHandle(hFile);
                ::DeleteFileW(wTmp.c_str()); // best-effort cleanup
                r.chips.push_back("failed");
                r.errorBody = "WriteFile failed (Win32 error " +
                              std::to_string(err) + ").";
                r.chips.push_back(ElapsedChip(t0));
                return r;
            }
            data    += written;
            remain  -= written;
        }
    }

    if (!::FlushFileBuffers(hFile)) {
        DWORD err = ::GetLastError();
        ::CloseHandle(hFile);
        ::DeleteFileW(wTmp.c_str()); // best-effort cleanup
        r.chips.push_back("failed");
        r.errorBody = "FlushFileBuffers on tmp failed (Win32 error " +
                      std::to_string(err) + ").";
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    if (!::CloseHandle(hFile)) {
        // Close failure post-write: contents were flushed, but the
        // handle disposition is undefined.  Be honest about it; the
        // .tmp is still on disk for inspection.
        DWORD err = ::GetLastError();
        r.chips.push_back("failed");
        r.errorBody = "CloseHandle on tmp failed (Win32 error " +
                      std::to_string(err) +
                      "); tmp file preserved at: " + tmpPath;
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    if (HasPythonExtension(resolved)) {
        tool_python_syntax::SyntaxCheckResult syntax =
            tool_python_syntax::CheckFile(tmpPath);
        if (!syntax.ok) {
            ::DeleteFileW(wTmp.c_str());
            r.chips.push_back("failed");
            r.chips.push_back("syntax error");
            r.errorBody = "Python syntax check failed; the file was not " +
                          std::string(overwriteExisting ? "overwritten" : "created") +
                          ".\n\n" + syntax.message;
            r.chips.push_back(ElapsedChip(t0));
            return r;
        }
    }

    if (!mutation.VerifyUnchanged()) {
        r.chips.push_back("conflict");
        r.errorBody = mutation.Error() + "\nThe proposed file is preserved at: " + tmpPath;
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }
    // Even overwrite_file must not clobber a file that appeared after a
    // create-new snapshot. Existing targets were version-checked above.
    const bool replaceExisting = overwriteExisting && mutation.TargetExisted();
    mutation.ReleaseTargetForCommit();

    BOOL movedOK = tool_staged_write::PromoteSiblingTempFile(
        wTmp, wFinal, replaceExisting);

    if (!movedOK) {
        DWORD err = ::GetLastError();
        r.chips.push_back("failed");
        if (err == ERROR_ALREADY_EXISTS || err == ERROR_FILE_EXISTS) {
            r.chips.clear();
            r.chips.push_back("exists");
            r.errorBody = "File appeared at the target path during write: " +
                          resolved +
                          ".\nThe staged content is preserved at: " + tmpPath;
        } else {
            r.errorBody = "Rename failed (Win32 error " +
                          std::to_string(err) +
                          ").\nThe staged content is preserved at: " + tmpPath;
        }
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

#else
    tool_staged_write::StagedTempFile tmp =
        tool_staged_write::CreateStagedTempFile(resolved);
    if (tmp.fd < 0) {
        r.chips.push_back("failed");
        r.errorBody = "Could not create unique temp file near '" +
                      resolved + "' for writing (" + std::strerror(tmp.error) + ").";
        if (!tmp.path.empty()) {
            r.errorBody += "\nLast attempted temp path: " + tmp.path;
        }
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }
    const std::string& tmpPath = tmp.path;

    int err = 0;
    if (!tool_staged_write::WriteAll(tmp.fd, content.data(), content.size(), err) ||
        !tool_staged_write::FlushToDisk(tmp.fd, err)) {
        tool_staged_write::DiscardStagedTempFile(tmp);
        r.chips.push_back("failed");
        r.errorBody = std::string("Writing the temp file failed (") + std::strerror(err) + ").";
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }
    if (::close(tmp.fd) != 0) {
        err = errno;
        tmp.fd = -1;
        r.chips.push_back("failed");
        r.errorBody = std::string("Closing the temp file failed (") + std::strerror(err) +
                      "); tmp file preserved at: " + tmpPath;
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }
    tmp.fd = -1;

    if (HasPythonExtension(resolved)) {
        tool_python_syntax::SyntaxCheckResult syntax =
            tool_python_syntax::CheckFile(tmpPath);
        if (!syntax.ok) {
            tool_staged_write::DiscardStagedTempFile(tmp);
            r.chips.push_back("failed");
            r.chips.push_back("syntax error");
            r.errorBody = "Python syntax check failed; the file was not " +
                          std::string(overwriteExisting ? "overwritten" : "created") +
                          ".\n\n" + syntax.message;
            r.chips.push_back(ElapsedChip(t0));
            return r;
        }
    }

    if (!mutation.VerifyUnchanged()) {
        r.chips.push_back("conflict");
        r.errorBody = mutation.Error() + "\nThe proposed file is preserved at: " + tmpPath;
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }
    const bool replaceExisting = overwriteExisting && mutation.TargetExisted();
    mutation.ReleaseTargetForCommit();

    if (!tool_staged_write::PromoteSiblingTempFile(tmpPath, resolved, replaceExisting, err)) {
        r.chips.push_back("failed");
        if (err == EEXIST) {
            r.chips.clear();
            r.chips.push_back("exists");
            r.errorBody = "File appeared at the target path during write: " +
                          resolved +
                          ".\nThe staged content is preserved at: " + tmpPath;
        } else {
            r.errorBody = std::string("Rename failed (") + std::strerror(err) +
                          ").\nThe staged content is preserved at: " + tmpPath;
        }
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }
#endif

    // ── Success ──────────────────────────────────────────────────
    const size_t bytes = content.size();
    const size_t lines = CountLines(content);

    // Structured success metadata for the UI, so the tool card can render
    // a PresentedFile chip without scraping the confirmation text.
    r.createdPath = resolved;
    r.displayName = tool_path_safety::Basename(resolved);
    r.sizeBytes   = bytes;
    r.lineCount   = static_cast<int>(lines);

    r.chips.push_back(overwriteExisting ? "overwritten" : "created");
    if (bytes == 0) {
        r.chips.push_back("empty");
    } else {
        r.chips.push_back(HumanBytes(bytes));
        r.chips.push_back(std::to_string(lines) + " lines");
    }
    r.body = std::string(overwriteExisting ? "Overwrote " : "Wrote ") +
             (bytes == 0 ? std::string("empty file") : HumanBytes(bytes)) +
             " to " + resolved + "\n";
    r.chips.push_back(ElapsedChip(t0));
    return r;
}

} // anonymous namespace

WriteResult WriteNewFile(const std::string& argsBlob,
                         const ToolContext& ctx)
{
    return WriteFileContent(argsBlob, ctx, false);
}

WriteResult OverwriteFileContent(const std::string& argsBlob,
                                 const ToolContext& ctx)
{
    return WriteFileContent(argsBlob, ctx, true);
}

WriteResult WritePowerShellScriptFile(const std::string& argsBlob,
                                      const ToolContext& ctx)
{
    return WriteFileContent(argsBlob, ctx, true,
                            WriteRiskMode::ApprovedPowerShellScript);
}
