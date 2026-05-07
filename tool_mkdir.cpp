// tool_mkdir.cpp

#include "tool_mkdir.h"
#include "tool_path.h"
#include "tool_path_safety.h"   // IsUnderCwd, Basename, ParentDir
#include "path_safety.h"

#include <chrono>
#include <sstream>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {

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

std::string Trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

} // anonymous namespace

MkdirResult MakeDirectory(const std::string& pathIn,
                          const ToolContext& ctx)
{
    MkdirResult r;
    auto t0 = std::chrono::steady_clock::now();

    const std::string requested = Trim(pathIn);
    if (requested.empty()) {
        r.chips.push_back("failed");
        r.errorBody = "mkdir requires a directory path in <args>.";
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    if (ctx.cwd.empty()) {
        r.chips.push_back("blocked");
        r.errorBody = "No working directory set; refuse to create.";
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    std::string resolved = ResolveToolPath(requested, ctx.cwd);
    if (resolved.empty()) {
        r.chips.push_back("failed");
        r.errorBody = "Could not resolve path: " + requested;
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    // ── Containment ──────────────────────────────────────────────
    if (!tool_path_safety::IsUnderCwd(resolved, ctx.cwd)) {
        r.chips.push_back("blocked");
        r.errorBody = "Refuses to create directories outside the "
                      "working directory.\n  resolved: " + resolved +
                      "\n  cwd:      " + ctx.cwd;
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    // ── Filename sanitization (leaf only) ────────────────────────
    std::string basename  = tool_path_safety::Basename(resolved);
    std::string sanitized = path_safety::SanitizeFilename(basename, "");
    if (sanitized.empty() || sanitized != basename) {
        r.chips.push_back("blocked");
        r.errorBody = "Directory name '" + basename +
                      "' has characters or a shape that aren't "
                      "safe on Windows.";
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    // ── Idempotency: already a directory is fine ─────────────────
    if (IsDirectory(resolved)) {
        r.chips.push_back("exists");
        r.body = "Directory already exists: " + resolved + "\n";
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    // Already a file at the target path: refuse rather than
    // silently shadowing.
    if (IsFile(resolved)) {
        r.chips.push_back("failed");
        r.errorBody = "A file already exists at this path: " + resolved;
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    // ── Parent directory must exist ──────────────────────────────
    std::string parent = tool_path_safety::ParentDir(resolved);
    if (parent.empty() || !IsDirectory(parent)) {
        r.chips.push_back("failed");
        r.errorBody = "Parent directory does not exist: " + parent +
                      ".\nCreate intermediate directories one level "
                      "at a time.";
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    // ── CreateDirectoryW ─────────────────────────────────────────
    std::wstring wResolved = path_safety::Utf8ToWide(resolved);
    if (wResolved.empty()) {
        r.chips.push_back("failed");
        r.errorBody = "Path conversion failed: " + resolved;
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    if (!::CreateDirectoryW(wResolved.c_str(), nullptr)) {
        DWORD err = ::GetLastError();
        if (err == ERROR_ALREADY_EXISTS) {
            // Race between IsDirectory check and CreateDirectoryW.
            // Re-check shape and treat as idempotent if it's now a
            // directory; otherwise it's a file collision.
            if (IsDirectory(resolved)) {
                r.chips.push_back("exists");
                r.body = "Directory already exists: " + resolved + "\n";
                r.chips.push_back(ElapsedChip(t0));
                return r;
            }
            r.chips.push_back("failed");
            r.errorBody = "A file appeared at this path during the "
                          "operation: " + resolved;
        } else {
            r.chips.push_back("failed");
            r.errorBody = "CreateDirectory failed (Win32 error " +
                          std::to_string(err) + ").";
        }
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    r.chips.push_back("created");
    r.body = "Created directory " + resolved + "\n";
    r.chips.push_back(ElapsedChip(t0));
    return r;
}
