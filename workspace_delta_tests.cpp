// Regression harness for workspace_delta.h + ps_command_hints.h
// (2026-06-11).  Exercises the POSIX branch of the scanner; the
// Windows branch mirrors the FindFirstFileW pattern already proven in
// python_runner.cpp.

#include "workspace_delta.h"
#include "ps_command_hints.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

static int g_failures = 0;
#define CHECK(cond, label)                                                  \
    do {                                                                    \
        if (cond) { std::cout << "PASS  " << label << "\n"; }               \
        else      { std::cout << "FAIL  " << label << "\n"; ++g_failures; } \
    } while (0)

static void WriteFile(const std::string& path, const std::string& content)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << content;
}

int main()
{
    char tmpl[] = "/tmp/wsdelta_XXXXXX";
    std::string root = mkdtemp(tmpl);

    // ── Baseline tree ──
    WriteFile(root + "/existing.txt", "hello");
    mkdir((root + "/sub").c_str(), 0755);
    WriteFile(root + "/sub/old.csv", "a,b\n1,2\n");
    mkdir((root + "/.git").c_str(), 0755);
    WriteFile(root + "/.git/HEAD", "ref: refs/heads/main");
    mkdir((root + "/__pycache__").c_str(), 0755);
    WriteFile(root + "/__pycache__/mod.pyc", "xxxx");

    workspace_delta::Snapshot before = workspace_delta::TakeSnapshot(root);
    CHECK(before.files.size() == 2,
          "snapshot: skips .git and __pycache__ (got " +
          std::to_string(before.files.size()) + " files)");
    CHECK(!before.capped, "snapshot: not capped on small tree");

    // ── No-change diff: the explicit negative ──
    {
        workspace_delta::Snapshot after = workspace_delta::TakeSnapshot(root);
        workspace_delta::Delta d = workspace_delta::Diff(before, after);
        CHECK(d.created.empty() && d.modified.empty(),
              "diff: clean run shows no changes");
        std::string m = workspace_delta::FormatManifest(d, after, root);
        CHECK(m.find("[workspace changes]") != std::string::npos &&
              m.find("no files were created or modified") != std::string::npos,
              "manifest: explicit negative on no-op run");
    }

    // ── Created + modified ──
    {
        WriteFile(root + "/LlamaBoss_files.zip", std::string(2048, 'Z'));
        WriteFile(root + "/sub/report.md", "# report\n");
        WriteFile(root + "/existing.txt", "hello world, now longer");

        workspace_delta::Snapshot after = workspace_delta::TakeSnapshot(root);
        workspace_delta::Delta d = workspace_delta::Diff(before, after);

        CHECK(d.created.size() == 2, "diff: two created files detected");
        CHECK(d.modified.size() == 1, "diff: one modified file detected");

        std::string m = workspace_delta::FormatManifest(d, after, root);
        CHECK(m.find("created: LlamaBoss_files.zip (2.0 KB)") != std::string::npos,
              "manifest: created file listed with size, path relative to root");
        CHECK(m.find("created: sub/report.md") != std::string::npos ||
              m.find("created: sub\\report.md") != std::string::npos,
              "manifest: created file in subfolder listed");
        CHECK(m.find("modified: existing.txt") != std::string::npos,
              "manifest: modified file listed by name");
        CHECK(m.find("no files were created") == std::string::npos,
              "manifest: negative line absent when changes exist");
    }

    // ── Size-only change with same mtime second is still caught (size
    //    differs); mtime-only change (touch) is caught via mtime ──
    {
        workspace_delta::Snapshot s1 = workspace_delta::TakeSnapshot(root);
        // Force an mtime bump without size change.
        struct timespec times[2];
        times[0].tv_sec = 1000000000; times[0].tv_nsec = 0;
        times[1].tv_sec = 1000000000; times[1].tv_nsec = 0;
        utimensat(AT_FDCWD, (root + "/sub/old.csv").c_str(), times, 0);
        workspace_delta::Snapshot s2 = workspace_delta::TakeSnapshot(root);
        workspace_delta::Delta d = workspace_delta::Diff(s1, s2);
        bool found = false;
        for (const auto& p : d.modified) {
            if (p.find("old.csv") != std::string::npos) found = true;
        }
        CHECK(found, "diff: mtime-only change detected as modified");
    }

    // ── Entry cap ──
    {
        std::string capDir = root + "/capped";
        mkdir(capDir.c_str(), 0755);
        for (size_t i = 0; i < workspace_delta::kMaxScanEntries + 50; ++i) {
            WriteFile(capDir + "/f" + std::to_string(i) + ".txt", "x");
        }
        workspace_delta::Snapshot s = workspace_delta::TakeSnapshot(capDir);
        CHECK(s.capped, "snapshot: cap flag set on oversized tree");
        CHECK(s.files.size() == workspace_delta::kMaxScanEntries,
              "snapshot: entry count clamped to cap");
        workspace_delta::Delta d =
            workspace_delta::Diff(workspace_delta::Snapshot{}, s);
        std::string m = workspace_delta::FormatManifest(d, s, capDir);
        CHECK(m.find("scan capped") != std::string::npos,
              "manifest: cap disclosed");
        CHECK(m.find("...and ") != std::string::npos,
              "manifest: created list truncated with count");
    }

    // ── Empty / bogus root ──
    {
        workspace_delta::Snapshot s = workspace_delta::TakeSnapshot("");
        CHECK(s.files.empty(), "snapshot: empty root yields empty snapshot");
        s = workspace_delta::TakeSnapshot("/nonexistent/path/zzz");
        CHECK(s.files.empty(), "snapshot: bogus root yields empty snapshot");
    }

    // ── ps_command_hints: the exact failing command from the transcript ──
    {
        std::string cmd =
            "Get-ChildItem -Path \"C:\\Users\\Cesar\\source\\repos\\LlamaBoss\" "
            "-Include \"*.h\", \"*.cpp\" -File | Compress-Archive "
            "-DestinationPath \"C:\\Users\\Cesar\\LlamaBoss\\Workflows\\chat_fa4700de\\Workspace\\LlamaBoss_files.zip\"";
        std::string hint = ps_command_hints::GetChildItemIncludeHint(cmd);
        CHECK(!hint.empty(), "hint: fires on the transcript's exact command");
        CHECK(hint.find("-Recurse") != std::string::npos &&
              hint.find("\\*") != std::string::npos,
              "hint: text names both fixes");
    }

    // ── ps_command_hints: correct variants must NOT fire ──
    {
        CHECK(ps_command_hints::GetChildItemIncludeHint(
                  "Get-ChildItem -Path \"C:\\dir\\*\" -Include *.h,*.cpp").empty(),
              "hint: silent when -Path ends with wildcard");
        CHECK(ps_command_hints::GetChildItemIncludeHint(
                  "Get-ChildItem -Path C:\\dir -Include *.h -Recurse").empty(),
              "hint: silent when -Recurse present");
        CHECK(ps_command_hints::GetChildItemIncludeHint(
                  "Get-ChildItem -Path C:\\dir -Filter *.h").empty(),
              "hint: silent without -Include");
        CHECK(ps_command_hints::GetChildItemIncludeHint(
                  "Compress-Archive -Path C:\\dir\\* -DestinationPath out.zip").empty(),
              "hint: silent on non-gci commands");
        CHECK(ps_command_hints::GetChildItemIncludeHint(
                  "logcint -Include foo").empty(),
              "hint: token match does not fire inside other words");
        CHECK(!ps_command_hints::GetChildItemIncludeHint(
                  "gci -Include *.h | Measure-Object").empty(),
              "hint: fires on gci alias with implicit path");
        CHECK(!ps_command_hints::GetChildItemIncludeHint(
                  "GET-CHILDITEM -PATH 'C:\\Dir' -INCLUDE *.CPP").empty(),
              "hint: case-insensitive");
    }

    // ── ps_command_hints: wildcard -Path + -Recurse -File (2026-09-30) ──
    {
        using ps_command_hints::GetChildItemWildcardRecurseHint;
        // Exact commands a Qwen model ran against D: in the transcripts.
        std::string h1 = GetChildItemWildcardRecurseHint(
            "Get-ChildItem -Path 'D:\\*' -Recurse -File -Filter '*Child Support*' "
            "-ErrorAction SilentlyContinue | Select-Object FullName, Length, "
            "LastWriteTime | Format-Table -AutoSize | Out-String -Width 300");
        CHECK(!h1.empty(), "wildcard hint: fires on transcript -Filter search");
        CHECK(h1.find("-Path 'D:\\' -Recurse -File") != std::string::npos,
              "wildcard hint: suggests the same path without the wildcard");
        CHECK(!GetChildItemWildcardRecurseHint(
                  "Get-ChildItem -Path 'D:\\*' -Recurse -File -ErrorAction "
                  "SilentlyContinue | Where-Object Name -match 'child.?support'").empty(),
              "wildcard hint: fires on transcript Where-Object search");
        CHECK(!GetChildItemWildcardRecurseHint(
                  "gci 'D:\\*' -Recurse -File -EA 0").empty(),
              "wildcard hint: fires on positional path + gci alias");
        CHECK(!GetChildItemWildcardRecurseHint(
                  "GET-CHILDITEM -PATH \"C:\\Docs\\*\" -RECURSE -FILE").empty(),
              "wildcard hint: case-insensitive, double quotes");

        // Measured-good or unrelated forms must stay silent.
        CHECK(GetChildItemWildcardRecurseHint(
                  "Get-ChildItem -Path 'D:\\' -Recurse -File -Filter '*x*'").empty(),
              "wildcard hint: silent on plain folder path");
        CHECK(GetChildItemWildcardRecurseHint(
                  "Get-ChildItem -Path 'D:\\*' -Recurse -Include *.txt").empty(),
              "wildcard hint: silent without -File (measured OK)");
        CHECK(GetChildItemWildcardRecurseHint(
                  "Get-ChildItem -Path 'D:\\*' -Recurse -Filter *.txt").empty(),
              "wildcard hint: -Filter is not mistaken for -File");
        CHECK(GetChildItemWildcardRecurseHint(
                  "Get-ChildItem -Path 'D:\\*' -File").empty(),
              "wildcard hint: silent without -Recurse");
        CHECK(GetChildItemWildcardRecurseHint(
                  "Get-ChildItem -LiteralPath 'D:\\odd*name' -Recurse -File").empty(),
              "wildcard hint: silent for -LiteralPath");
        CHECK(GetChildItemWildcardRecurseHint(
                  "Copy-Item -Path 'D:\\*' -Recurse -Destination E:\\").empty(),
              "wildcard hint: silent on non-gci commands");
        CHECK(GetChildItemWildcardRecurseHint(
                  "gci -Recurse -File | Measure-Object").empty(),
              "wildcard hint: silent with implicit path");
    }

    // The -Include hint must no longer suggest adding -Recurse to a \* path.
    {
        std::string hint = ps_command_hints::GetChildItemIncludeHint(
            "gci -Path C:\\dir -Include *.h");
        CHECK(hint.find("no trailing") != std::string::npos,
              "include hint: recursive advice says no trailing \\*");
    }

    std::cout << (g_failures == 0 ? "\nALL TESTS PASSED\n"
                                  : "\nFAILURES: " + std::to_string(g_failures) + "\n");
    return g_failures == 0 ? 0 : 1;
}
