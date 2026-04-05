// test_classifier.cpp — standalone test for action_classifier
// Compile: g++ -std=c++17 -o test_classifier test_classifier.cpp action_classifier.cpp
#include "action_classifier.h"
#include <iostream>
#include <string>

static int passed = 0;
static int failed = 0;

void Check(const char* label, const ActionIntent& r,
           ActionType expectAction,
           const std::string& expectCleanedBody,
           const std::string& expectRawQuery,
           const std::string& expectCommand)
{
    bool ok = (r.action      == expectAction &&
               r.cleanedBody == expectCleanedBody &&
               r.rawQuery    == expectRawQuery &&
               r.command     == expectCommand);
    if (ok) {
        ++passed;
    } else {
        ++failed;
        auto actionStr = [](ActionType a) -> const char* {
            switch (a) {
            case ActionType::Chat:            return "Chat";
            case ActionType::WorkspaceSearch: return "WorkspaceSearch";
            case ActionType::WorkspaceSaveChat: return "WorkspaceSaveChat";
            default:                          return "?";
            }
        };
        std::cout << "FAIL: " << label << "\n"
                  << "  action:      " << actionStr(r.action)
                  << " (expected " << actionStr(expectAction) << ")\n"
                  << "  cleanedBody: \"" << r.cleanedBody
                  << "\" (expected \"" << expectCleanedBody << "\")\n"
                  << "  rawQuery:    \"" << r.rawQuery
                  << "\" (expected \"" << expectRawQuery << "\")\n"
                  << "  command:     \"" << r.command
                  << "\" (expected \"" << expectCommand << "\")\n\n";
    }
}

// Convenience: check a Chat result (rawQuery and command are always empty)
void CheckChat(const char* label, const ActionIntent& r,
               const std::string& expectCleanedBody)
{
    Check(label, r, ActionType::Chat, expectCleanedBody, "", "");
}

int main()
{
    // ── Valid /search commands ────────────────────────────────────
    {
        auto r = ClassifyAction("/search hours by employee", true);
        Check("basic search", r, ActionType::WorkspaceSearch,
              "hours by employee", "hours by employee", "search");
    }
    {
        auto r = ClassifyAction("/search SPA4 billing", true);
        Check("search SPA4", r, ActionType::WorkspaceSearch,
              "SPA4 billing", "SPA4 billing", "search");
    }
    {
        auto r = ClassifyAction("/Search Permit Expirations", true);
        Check("search mixed case", r, ActionType::WorkspaceSearch,
              "Permit Expirations", "Permit Expirations", "Search");
    }
    {
        auto r = ClassifyAction("/SEARCH overtime report", true);
        Check("search all caps", r, ActionType::WorkspaceSearch,
              "overtime report", "overtime report", "SEARCH");
    }
    {
        auto r = ClassifyAction("/search   extra   spaces", true);
        Check("search extra spaces in query", r, ActionType::WorkspaceSearch,
              "extra   spaces", "extra   spaces", "search");
    }

    // ── Normal chat messages (correctly untouched) ───────────────
    {
        auto r = ClassifyAction("What is the capital of France?", true);
        CheckChat("plain question", r, "What is the capital of France?");
    }
    {
        auto r = ClassifyAction("Can you search for information about Python?", true);
        CheckChat("search in natural language", r,
                  "Can you search for information about Python?");
    }
    {
        auto r = ClassifyAction("@qwen explain recursion", true);
        CheckChat("model-directed message", r, "@qwen explain recursion");
    }
    {
        auto r = ClassifyAction("Tell me about /search commands", true);
        CheckChat("slash in middle of message", r, "Tell me about /search commands");
    }
    {
        auto r = ClassifyAction("Hello world", true);
        CheckChat("simple greeting", r, "Hello world");
    }

    // ── Edge cases (empty, whitespace, malformed) ────────────────
    {
        auto r = ClassifyAction("", true);
        CheckChat("empty message", r, "");
    }
    {
        auto r = ClassifyAction("   ", true);
        CheckChat("whitespace only", r, "   ");
    }
    {
        auto r = ClassifyAction("/", true);
        CheckChat("lone slash", r, "/");
    }
    {
        auto r = ClassifyAction("/ search test", true);
        CheckChat("space after slash", r, "/ search test");
    }

    // ── Malformed commands ───────────────────────────────────────
    {
        auto r = ClassifyAction("/search", true);
        CheckChat("search no query", r, "/search");
    }
    {
        auto r = ClassifyAction("/search ", true);
        CheckChat("search trailing space only", r, "/search ");
    }
    {
        auto r = ClassifyAction("/searchfiles budget", true);
        CheckChat("wrong command name", r, "/searchfiles budget");
    }
    {
        auto r = ClassifyAction("/open readme.txt", true);
        CheckChat("unsupported command v1", r, "/open readme.txt");
    }

    // ── Workspace disabled (kill switch) ─────────────────────────
    {
        auto r = ClassifyAction("/search hours by employee", false);
        CheckChat("search but workspace off", r, "/search hours by employee");
    }
    {
        auto r = ClassifyAction("/search SPA4 billing", false);
        CheckChat("search SPA4 but workspace off", r, "/search SPA4 billing");
    }

    // ── Leading whitespace tolerance ─────────────────────────────
    {
        auto r = ClassifyAction("  /search overtime report", true);
        Check("leading spaces before slash", r, ActionType::WorkspaceSearch,
              "overtime report", "overtime report", "search");
    }
    {
        auto r = ClassifyAction("\t/search tab prefix", true);
        Check("leading tab before slash", r, ActionType::WorkspaceSearch,
              "tab prefix", "tab prefix", "search");
    }

    // ── /savechat command ────────────────────────────────────────
    {
        auto r = ClassifyAction("/savechat", true);
        Check("savechat no name", r, ActionType::WorkspaceSaveChat,
              "", "", "savechat");
    }
    {
        auto r = ClassifyAction("/savechat my notes", true);
        Check("savechat with name", r, ActionType::WorkspaceSaveChat,
              "my notes", "my notes", "savechat");
    }
    {
        auto r = ClassifyAction("/SaveChat Meeting Summary", true);
        Check("savechat mixed case", r, ActionType::WorkspaceSaveChat,
              "Meeting Summary", "Meeting Summary", "SaveChat");
    }
    {
        auto r = ClassifyAction("/savechat", false);
        CheckChat("savechat workspace off", r, "/savechat");
    }
    {
        auto r = ClassifyAction("/savechat my notes", false);
        CheckChat("savechat with name workspace off", r, "/savechat my notes");
    }

    // ── Custom supported actions list ────────────────────────────
    {
        // Only "search" in supported list — "open" should fail
        auto r = ClassifyAction("/open readme.txt", true, {"search"});
        CheckChat("open not in supported list", r, "/open readme.txt");
    }
    {
        // If we add "open" to supported list in the future, the
        // classifier would still return Chat because CommandToAction
        // doesn't map it yet.  This test documents that behavior.
        auto r = ClassifyAction("/open readme.txt", true, {"search", "open"});
        CheckChat("open in list but no ActionType mapping", r, "/open readme.txt");
    }

    // ── Summary ──────────────────────────────────────────────────
    std::cout << "\n" << passed << " passed, " << failed << " failed\n";
    return failed > 0 ? 1 : 0;
}
