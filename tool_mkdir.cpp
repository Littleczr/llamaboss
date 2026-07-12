// tool_mkdir.cpp

#include "tool_mkdir.h"
#include "tool_path.h"
#include "tool_path_safety.h"   // IsUnderAllowedWriteRoot, Basename, ParentDir
#include "path_safety.h"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <vector>

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

    std::string resolved = tool_path_safety::ResolveProjectAwareToolPath(requested, ctx.cwd, ctx.activeProjectRoot);
    if (resolved.empty()) {
        r.chips.push_back("failed");
        r.errorBody = "Could not resolve path: " + requested;
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    // ── Containment ──────────────────────────────────────────────
    if (!tool_path_safety::IsUnderAllowedWriteRoot(resolved, ctx.cwd, ctx.activeProjectRoot, ctx.skillsRoot)) {
        r.chips.push_back("blocked");
        r.errorBody = "Refuses to create directories outside the allowed write roots."
                      "\n  resolved: " + resolved +
                      tool_path_safety::AllowedWriteRootsDiagnostic(ctx.cwd, ctx.activeProjectRoot, ctx.skillsRoot);
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

    // ── Build missing chain (safe mkdir -p) ──────────────────────
    // Older mkdir required the immediate parent to exist. Project
    // scaffolding burns tool steps and produces avoidable failures under
    // that rule (Outputs/Pong/src before Outputs/Pong). We now create
    // the missing chain, but every new segment is still sanitized and
    // the already-resolved final path still has to be inside the allowed
    // write roots.
    std::vector<std::string> toCreate;
    std::string cur = resolved;
    while (!cur.empty() && !IsDirectory(cur)) {
        if (IsFile(cur)) {
            r.chips.push_back("failed");
            r.errorBody = "A file exists where a directory is needed: " + cur;
            r.chips.push_back(ElapsedChip(t0));
            return r;
        }

        std::string basename  = tool_path_safety::Basename(cur);
        std::string sanitized = path_safety::SanitizeFilename(basename, "");
        if (sanitized.empty() || sanitized != basename) {
            r.chips.push_back("blocked");
            r.errorBody = "Directory name '" + basename +
                          "' has characters or a shape that aren't "
                          "safe on Windows.";
            r.chips.push_back(ElapsedChip(t0));
            return r;
        }

        toCreate.push_back(cur);
        std::string parent = tool_path_safety::ParentDir(cur);
        if (parent.empty() || parent == cur) {
            r.chips.push_back("failed");
            r.errorBody = "Could not find an existing parent while creating: " + resolved;
            r.chips.push_back(ElapsedChip(t0));
            return r;
        }
        cur = parent;
    }

    if (cur.empty() || !IsDirectory(cur)) {
        r.chips.push_back("failed");
        r.errorBody = "Could not find an existing parent while creating: " + resolved;
        r.chips.push_back(ElapsedChip(t0));
        return r;
    }

    std::reverse(toCreate.begin(), toCreate.end());

    size_t createdCount = 0;
    for (const std::string& dir : toCreate) {
        std::wstring wDir = path_safety::Utf8ToWide(dir);
        if (wDir.empty()) {
            r.chips.push_back("failed");
            r.errorBody = "Path conversion failed: " + dir;
            r.chips.push_back(ElapsedChip(t0));
            return r;
        }

        if (!::CreateDirectoryW(wDir.c_str(), nullptr)) {
            DWORD err = ::GetLastError();
            if (err == ERROR_ALREADY_EXISTS && IsDirectory(dir)) {
                // Benign race: another caller created this segment first.
                continue;
            }
            r.chips.push_back("failed");
            if (err == ERROR_ALREADY_EXISTS) {
                r.errorBody = "A file appeared where a directory is needed: " + dir;
            } else {
                r.errorBody = "CreateDirectory failed for " + dir +
                              " (Win32 error " + std::to_string(err) + ").";
            }
            r.chips.push_back(ElapsedChip(t0));
            return r;
        }
        ++createdCount;
    }

    r.chips.push_back(createdCount == 0 ? "exists" : "created");
    if (createdCount == 0) {
        r.body = "Directory already exists: " + resolved + "\n";
    } else {
        r.body = "Created directory " + resolved;
        if (createdCount > 1) {
            r.body += " (including " + std::to_string(createdCount - 1) +
                      " intermediate director" +
                      (createdCount == 2 ? "y" : "ies") + ")";
        }
        r.body += "\n";
    }
    r.chips.push_back(ElapsedChip(t0));
    return r;
}
