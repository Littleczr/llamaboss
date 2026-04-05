// test_executor.cpp — standalone test for workspace_executor
// Compile: g++ -std=c++17 -o test_executor test_executor.cpp workspace_executor.cpp
#include "workspace_executor.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int passed = 0;
static int failed = 0;

void Assert(const char* label, bool condition)
{
    if (condition) {
        ++passed;
    } else {
        ++failed;
        std::cout << "FAIL: " << label << "\n";
    }
}

// Create a test file with some content
void CreateTestFile(const fs::path& path, const std::string& content = "test")
{
    fs::create_directories(path.parent_path());
    std::ofstream f(path);
    f << content;
}

int main()
{
    // ── Set up test workspace ────────────────────────────────────
    fs::path testDir = fs::temp_directory_path() / "llamaboss_test_workspace";

    // Clean up any previous run
    fs::remove_all(testDir);
    fs::create_directories(testDir);

    // Create test files
    CreateTestFile(testDir / "employee_hours.xlsx", std::string(3072, 'x'));     // ~3 KB
    CreateTestFile(testDir / "SPA4_billing_report.csv", std::string(1024, 'x')); // ~1 KB
    CreateTestFile(testDir / "permit_expirations.xlsx", std::string(2048, 'x')); // ~2 KB
    CreateTestFile(testDir / "README.md", std::string(512, 'x'));                // ~0.5 KB
    CreateTestFile(testDir / "notes.txt", std::string(100, 'x'));
    CreateTestFile(testDir / "reports" / "quarterly_hours.pdf", std::string(5120, 'x'));
    CreateTestFile(testDir / "reports" / "budget_2026.xlsx", std::string(4096, 'x'));
    CreateTestFile(testDir / "archive" / "old_billing.csv", std::string(800, 'x'));

    std::string wsPath = testDir.string();

    // ── Test: basic single-term search ───────────────────────────
    {
        auto r = ExecuteWorkspaceSearch("hours", wsPath);
        Assert("hours: success", r.success);
        Assert("hours: found 2", r.matches.size() == 2);
        // Should find employee_hours.xlsx and reports/quarterly_hours.pdf
        bool foundEmployee = false, foundQuarterly = false;
        for (const auto& m : r.matches) {
            if (m.fileName == "employee_hours.xlsx") foundEmployee = true;
            if (m.fileName == "quarterly_hours.pdf") foundQuarterly = true;
        }
        Assert("hours: found employee_hours.xlsx", foundEmployee);
        Assert("hours: found quarterly_hours.pdf", foundQuarterly);
        Assert("hours: scanned all files", r.totalFilesScanned == 8);
    }

    // ── Test: multi-term search (ANY match) ──────────────────────
    {
        auto r = ExecuteWorkspaceSearch("billing budget", wsPath);
        Assert("billing budget: success", r.success);
        Assert("billing budget: found 3", r.matches.size() == 3);
        // Should find SPA4_billing_report.csv, budget_2026.xlsx, old_billing.csv
    }

    // ── Test: case insensitivity ─────────────────────────────────
    {
        auto r = ExecuteWorkspaceSearch("README", wsPath);
        Assert("README uppercase: success", r.success);
        Assert("README uppercase: found 1", r.matches.size() == 1);
    }
    {
        auto r = ExecuteWorkspaceSearch("readme", wsPath);
        Assert("readme lowercase: success", r.success);
        Assert("readme lowercase: found 1", r.matches.size() == 1);
    }

    // ── Test: no matches ─────────────────────────────────────────
    {
        auto r = ExecuteWorkspaceSearch("nonexistent", wsPath);
        Assert("no match: success", r.success);
        Assert("no match: empty results", r.matches.empty());
        Assert("no match: scanned files", r.totalFilesScanned == 8);
    }

    // ── Test: subdirectory files found ───────────────────────────
    {
        auto r = ExecuteWorkspaceSearch("budget", wsPath);
        Assert("subdir: success", r.success);
        Assert("subdir: found 1", r.matches.size() == 1);
        if (!r.matches.empty()) {
            // Relative path should include the subdirectory
            std::string rel = r.matches[0].relativePath;
            Assert("subdir: relative path has reports/",
                   rel.find("reports") != std::string::npos);
        }
    }

    // ── Test: file sizes populated ───────────────────────────────
    {
        auto r = ExecuteWorkspaceSearch("employee_hours", wsPath);
        Assert("filesize: success", r.success);
        Assert("filesize: found 1", r.matches.size() == 1);
        if (!r.matches.empty()) {
            Assert("filesize: size > 0", r.matches[0].fileSize > 0);
        }
    }

    // ── Test: max results cap ────────────────────────────────────
    {
        // Search for ".xlsx" which should match 3 files, but cap at 2
        auto r = ExecuteWorkspaceSearch("xlsx", wsPath, 2);
        Assert("maxResults: success", r.success);
        Assert("maxResults: capped at 2", r.matches.size() <= 2);
    }

    // ── Test: workspace folder does not exist ────────────────────
    {
        auto r = ExecuteWorkspaceSearch("test", "/nonexistent/path/nowhere");
        Assert("missing dir: not success", !r.success);
        Assert("missing dir: has error", !r.errorMessage.empty());
    }

    // ── Test: empty query ────────────────────────────────────────
    {
        auto r = ExecuteWorkspaceSearch("", wsPath);
        Assert("empty query: not success", !r.success);
        Assert("empty query: has error", !r.errorMessage.empty());
    }
    {
        auto r = ExecuteWorkspaceSearch("   ", wsPath);
        Assert("whitespace query: not success", !r.success);
        Assert("whitespace query: has error", !r.errorMessage.empty());
    }

    // ── Test: FormatFileSize ─────────────────────────────────────
    Assert("format 0 bytes", FormatFileSize(0) == "0 bytes");
    Assert("format 500 bytes", FormatFileSize(500) == "500 bytes");
    Assert("format 1 KB", FormatFileSize(1024) == "1.0 KB");
    Assert("format 3 KB", FormatFileSize(3072) == "3.0 KB");
    Assert("format 1 MB", FormatFileSize(1048576) == "1.0 MB");
    Assert("format 1.5 MB", FormatFileSize(1572864) == "1.5 MB");

    // ── Test: FormatSearchResult ─────────────────────────────────
    {
        auto r = ExecuteWorkspaceSearch("hours", wsPath);
        std::string formatted = FormatSearchResult(r, "hours", wsPath);
        Assert("format: contains Found", formatted.find("Found") != std::string::npos);
        Assert("format: contains query", formatted.find("hours") != std::string::npos);
        Assert("format: contains Scanned", formatted.find("Scanned") != std::string::npos);
    }
    {
        auto r = ExecuteWorkspaceSearch("nonexistent", wsPath);
        std::string formatted = FormatSearchResult(r, "nonexistent", wsPath);
        Assert("format no match: contains No files",
               formatted.find("No files") != std::string::npos);
    }
    {
        WorkspaceSearchResult errResult;
        errResult.success = false;
        errResult.errorMessage = "Test error";
        std::string formatted = FormatSearchResult(errResult, "test", wsPath);
        Assert("format error: contains failed",
               formatted.find("failed") != std::string::npos);
    }

    // ── Clean up ─────────────────────────────────────────────────
    fs::remove_all(testDir);

    // ═════════════════════════════════════════════════════════════
    //  Save Chat tests
    // ═════════════════════════════════════════════════════════════

    // Create a fresh test directory for save tests
    fs::path saveTestDir = fs::temp_directory_path() / "llamaboss_test_savechat";
    fs::remove_all(saveTestDir);
    fs::create_directories(saveTestDir);
    std::string savePath = saveTestDir.string();

    // Build a small test conversation
    std::vector<ChatMessage> testConvo = {
        { "user",      "Hello, what is C++?", "" },
        { "assistant", "C++ is a general-purpose programming language.", "gemma4:e4b-it-bf16" },
        { "user",      "Can you give me an example?", "" },
        { "assistant", "Sure! Here is a simple hello world:\n```cpp\n#include <iostream>\nint main() { std::cout << \"Hello\"; }\n```", "gemma4:e4b-it-bf16" },
    };

    // ── Test: save with auto-generated name ──────────────────────
    {
        auto r = ExecuteWorkspaceSaveChat(testConvo, savePath, "", "gemma4:e4b-it-bf16");
        Assert("save auto: success", r.success);
        Assert("save auto: has filename", !r.savedFileName.empty());
        Assert("save auto: is .md", r.savedFileName.find(".md") != std::string::npos);
        Assert("save auto: has path", !r.savedFilePath.empty());
        Assert("save auto: 4 messages", r.messageCount == 4);
        Assert("save auto: file exists", fs::exists(r.savedFilePath));

        // Verify content
        std::ifstream f(r.savedFilePath);
        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
        Assert("save auto: has header", content.find("# Chat Export") != std::string::npos);
        Assert("save auto: has model", content.find("gemma4") != std::string::npos);
        Assert("save auto: has user msg", content.find("Hello, what is C++?") != std::string::npos);
        Assert("save auto: has assistant msg", content.find("general-purpose") != std::string::npos);
    }

    // ── Test: save with custom name ──────────────────────────────
    {
        auto r = ExecuteWorkspaceSaveChat(testConvo, savePath, "My CPP Notes", "gemma4:e4b-it-bf16");
        Assert("save custom: success", r.success);
        Assert("save custom: correct filename", r.savedFileName == "My_CPP_Notes.md");
        Assert("save custom: file exists", fs::exists(r.savedFilePath));
    }

    // ── Test: duplicate name gets suffix ─────────────────────────
    {
        auto r = ExecuteWorkspaceSaveChat(testConvo, savePath, "My CPP Notes", "gemma4:e4b-it-bf16");
        Assert("save dupe: success", r.success);
        Assert("save dupe: has suffix", r.savedFileName == "My_CPP_Notes_1.md");
        Assert("save dupe: file exists", fs::exists(r.savedFilePath));
    }

    // ── Test: save with special characters in name ───────────────
    {
        auto r = ExecuteWorkspaceSaveChat(testConvo, savePath, "Q3 Report!! @#$", "gemma4:e4b-it-bf16");
        Assert("save special: success", r.success);
        Assert("save special: sanitized name", r.savedFileName == "Q3_Report.md");
    }

    // ── Test: save to non-existent workspace (auto-creates) ──────
    {
        fs::path newDir = saveTestDir / "new_subfolder";
        auto r = ExecuteWorkspaceSaveChat(testConvo, newDir.string(), "test", "gemma4:e4b-it-bf16");
        Assert("save new dir: success", r.success);
        Assert("save new dir: folder created", fs::is_directory(newDir));
        Assert("save new dir: file exists", fs::exists(r.savedFilePath));
    }

    // ── Test: save empty conversation ────────────────────────────
    {
        std::vector<ChatMessage> empty;
        auto r = ExecuteWorkspaceSaveChat(empty, savePath);
        Assert("save empty: not success", !r.success);
        Assert("save empty: has error", !r.errorMessage.empty());
    }

    // ── Test: FormatSaveChatResult ───────────────────────────────
    {
        auto r = ExecuteWorkspaceSaveChat(testConvo, savePath, "format test", "gemma4:e4b-it-bf16");
        std::string formatted = FormatSaveChatResult(r);
        Assert("format save: has success", formatted.find("successfully") != std::string::npos);
        Assert("format save: has filename", formatted.find("format_test.md") != std::string::npos);
    }
    {
        WorkspaceSaveChatResult errResult;
        errResult.success = false;
        errResult.errorMessage = "Test error";
        std::string formatted = FormatSaveChatResult(errResult);
        Assert("format save err: has failed", formatted.find("failed") != std::string::npos);
    }

    // ── Clean up save tests ──────────────────────────────────────
    fs::remove_all(saveTestDir);

    // ── Summary ──────────────────────────────────────────────────
    std::cout << "\n" << passed << " passed, " << failed << " failed\n";
    return failed > 0 ? 1 : 0;
}
