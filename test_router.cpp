// test_router.cpp — standalone test for message_router
// Compile: g++ -std=c++17 -o test_router test_router.cpp message_router.cpp
#include "message_router.h"
#include <iostream>
#include <cassert>
#include <string>

static int passed = 0;
static int failed = 0;

void Check(const char* label, const RouteResult& r,
           const std::string& expectTarget,
           const std::string& expectBody,
           bool expectDirected)
{
    bool ok = (r.targetModel == expectTarget &&
               r.cleanedBody == expectBody &&
               r.isDirected  == expectDirected);
    if (ok) {
        ++passed;
    } else {
        ++failed;
        std::cout << "FAIL: " << label << "\n"
                  << "  target:   \"" << r.targetModel << "\" (expected \"" << expectTarget << "\")\n"
                  << "  body:     \"" << r.cleanedBody << "\" (expected \"" << expectBody << "\")\n"
                  << "  directed: " << r.isDirected << " (expected " << expectDirected << ")\n\n";
    }
}

int main()
{
    // Two models loaded — typical group chat setup
    std::vector<std::string> models = {
        "gemma4:e2b-it-q4_K_M",
        "qwen3.5:latest"
    };

    // ── Group turns (no addressing) ──────────────────────────────
    {
        auto r = RouteMessage("What are your strengths?", models);
        Check("plain question", r, "", "What are your strengths?", false);
    }
    {
        auto r = RouteMessage("Compare qwen and gemma models", models);
        Check("name in middle (no prefix pattern)", r, "", "Compare qwen and gemma models", false);
    }
    {
        auto r = RouteMessage("", models);
        Check("empty message", r, "", "", false);
    }
    {
        auto r = RouteMessage("   ", models);
        Check("whitespace only", r, "", "   ", false);
    }

    // ── @ prefix addressing ──────────────────────────────────────
    {
        auto r = RouteMessage("@qwen what are your strengths?", models);
        Check("@qwen prefix", r, "qwen3.5:latest", "what are your strengths?", true);
    }
    {
        auto r = RouteMessage("@gemma summarize that", models);
        Check("@gemma prefix", r, "gemma4:e2b-it-q4_K_M", "summarize that", true);
    }
    {
        auto r = RouteMessage("@gemma4 summarize that", models);
        Check("@gemma4 longer prefix", r, "gemma4:e2b-it-q4_K_M", "summarize that", true);
    }
    {
        auto r = RouteMessage("@qwen3.5 explain this", models);
        Check("@qwen3.5 full short name", r, "qwen3.5:latest", "explain this", true);
    }
    {
        auto r = RouteMessage("@QWEN case insensitive", models);
        Check("@QWEN uppercase", r, "qwen3.5:latest", "case insensitive", true);
    }

    // ── Comma addressing ─────────────────────────────────────────
    {
        auto r = RouteMessage("qwen, what are your strengths?", models);
        Check("qwen comma", r, "qwen3.5:latest", "what are your strengths?", true);
    }
    {
        auto r = RouteMessage("Gemma, summarize that", models);
        Check("Gemma comma capitalized", r, "gemma4:e2b-it-q4_K_M", "summarize that", true);
    }

    // ── Colon addressing ─────────────────────────────────────────
    {
        auto r = RouteMessage("qwen: what are your strengths?", models);
        Check("qwen colon", r, "qwen3.5:latest", "what are your strengths?", true);
    }
    {
        auto r = RouteMessage("gemma4: tell me more", models);
        Check("gemma4 colon", r, "gemma4:e2b-it-q4_K_M", "tell me more", true);
    }

    // ── Group keywords ───────────────────────────────────────────
    {
        auto r = RouteMessage("@all introduce yourselves", models);
        Check("@all group keyword", r, "", "introduce yourselves", false);
    }
    {
        auto r = RouteMessage("@everyone brainstorm ideas", models);
        Check("@everyone group keyword", r, "", "brainstorm ideas", false);
    }
    {
        auto r = RouteMessage("@both what do you think?", models);
        Check("@both group keyword", r, "", "what do you think?", false);
    }
    {
        auto r = RouteMessage("@ALL caps group keyword", models);
        Check("@ALL caps", r, "", "caps group keyword", false);
    }

    // ── No match (non-model name with delimiter) ─────────────────
    {
        auto r = RouteMessage("John, can you help?", models);
        Check("unknown name with comma", r, "", "John, can you help?", false);
    }
    {
        auto r = RouteMessage("@steve what do you think?", models);
        Check("@unknown name", r, "", "@steve what do you think?", false);
    }

    // ── Single model (routing doesn't apply) ─────────────────────
    {
        std::vector<std::string> single = { "qwen3.5:latest" };
        auto r = RouteMessage("@qwen what are your strengths?", single);
        Check("single model ignores routing", r, "", "@qwen what are your strengths?", false);
    }

    // ── Namespace-stripped model name matching ────────────────────
    {
        std::vector<std::string> nsModels = {
            "pidrilkin/gemma3_27b_abliterated:Q4_K_M",
            "qwen3.5:latest"
        };
        auto r = RouteMessage("@gemma3 what do you know?", nsModels);
        Check("namespace stripped match", r, "pidrilkin/gemma3_27b_abliterated:Q4_K_M",
              "what do you know?", true);
    }

    // ── Edge: address with no body ───────────────────────────────
    {
        auto r = RouteMessage("@qwen", models);
        Check("address only no body", r, "qwen3.5:latest", "", true);
    }
    {
        auto r = RouteMessage("qwen,", models);
        Check("comma address no body", r, "qwen3.5:latest", "", true);
    }

    // ── Summary ──────────────────────────────────────────────────
    std::cout << "\n" << passed << " passed, " << failed << " failed\n";
    return failed > 0 ? 1 : 0;
}
