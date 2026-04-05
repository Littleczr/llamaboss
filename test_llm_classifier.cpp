// test_llm_classifier.cpp — interactive test for llm_classifier
//
// Requires a running Ollama instance with a model loaded.
//
// Build (Visual Studio Developer Command Prompt):
//   cl /EHsc /std:c++17 /I<poco_include_path> test_llm_classifier.cpp
//      llm_classifier.cpp /link <poco_lib_path>/PocoFoundation.lib
//      <poco_lib_path>/PocoNet.lib <poco_lib_path>/PocoJSON.lib
//
// Usage:
//   test_llm_classifier.exe [api_url] [model]
//
// Defaults:
//   api_url = http://127.0.0.1:11434
//   model   = gemma4:e4b-it-bf16

#include "llm_classifier.h"
#include <iostream>
#include <string>
#include <vector>
#include <chrono>

struct TestCase
{
    std::string message;
    std::string expectedAction;
};

int main(int argc, char* argv[])
{
    std::string apiUrl = "http://127.0.0.1:11434";
    std::string model  = "gemma4:e4b-it-bf16";

    if (argc >= 2) apiUrl = argv[1];
    if (argc >= 3) model  = argv[2];

    std::cout << "LLM Classifier Test\n"
              << "  API:   " << apiUrl << "\n"
              << "  Model: " << model << "\n"
              << "  Timeout: 5 seconds\n\n";

    // ── Predefined test cases ────────────────────────────────────
    std::vector<TestCase> tests = {
        // Should classify as chat
        { "Hello, how are you?",                           "chat" },
        { "What is recursion?",                            "chat" },
        { "Explain how to create a CSV in Python",         "chat" },
        { "What is the capital of France?",                "chat" },
        { "How do I save files in C++?",                   "chat" },

        // Should classify as save_chat
        { "Save this chat",                                "save_chat" },
        { "Save this conversation as notes",               "save_chat" },
        { "Export this chat to a file",                    "save_chat" },
        { "Keep a copy of this conversation",              "save_chat" },
        { "Save our discussion as meeting notes",          "save_chat" },

        // Should classify as workspace_search
        { "Find the budget file",                          "workspace_search" },
        { "Search for the changelog",                      "workspace_search" },
        { "Do I have any spreadsheets in my workspace?",   "workspace_search" },
        { "Look for the hours report",                     "workspace_search" },
        { "What files do I have about payroll?",           "workspace_search" },
    };

    int passed = 0;
    int failed = 0;
    int errors = 0;

    for (const auto& tc : tests) {
        std::cout << "── Test: \"" << tc.message << "\"\n";
        std::cout << "   Expected: " << tc.expectedAction << "\n";

        auto startTime = std::chrono::steady_clock::now();

        LLMClassifyResult result = ClassifyWithLLM(tc.message, apiUrl, model, 10);

        auto endTime = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            endTime - startTime).count();

        if (!result.success) {
            std::cout << "   ERROR: " << result.errorMessage << "\n";
            ++errors;
        } else {
            std::cout << "   Got:      " << result.action;
            if (!result.topic.empty())
                std::cout << "  topic=\"" << result.topic << "\"";
            if (!result.filenameHint.empty())
                std::cout << "  filename=\"" << result.filenameHint << "\"";
            std::cout << "\n";

            if (result.action == tc.expectedAction) {
                std::cout << "   PASS";
                ++passed;
            } else {
                std::cout << "   FAIL";
                ++failed;
            }
            std::cout << "  (" << elapsed << "ms)\n";
        }
        std::cout << "\n";
    }

    // ── Interactive mode ─────────────────────────────────────────
    std::cout << "════════════════════════════════════════════\n"
              << "Results: " << passed << " passed, "
              << failed << " failed, "
              << errors << " errors\n"
              << "════════════════════════════════════════════\n\n"
              << "Interactive mode (type a message, or 'quit' to exit):\n\n";

    std::string line;
    while (true) {
        std::cout << "> ";
        if (!std::getline(std::cin, line)) break;
        if (line == "quit" || line == "exit") break;
        if (line.empty()) continue;

        auto startTime = std::chrono::steady_clock::now();
        LLMClassifyResult result = ClassifyWithLLM(line, apiUrl, model, 10);
        auto endTime = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            endTime - startTime).count();

        if (!result.success) {
            std::cout << "  ERROR: " << result.errorMessage << "\n";
        } else {
            std::cout << "  Action: " << result.action;
            if (!result.topic.empty())
                std::cout << "  topic=\"" << result.topic << "\"";
            if (!result.filenameHint.empty())
                std::cout << "  filename=\"" << result.filenameHint << "\"";
            std::cout << "  (" << elapsed << "ms)\n";
        }
        std::cout << "\n";
    }

    return failed > 0 ? 1 : 0;
}
