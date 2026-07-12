// Regression harness for tool_call_parser changes (2026-06-11).
// Compiles against the real tool_call_parser.cpp with stubbed
// validators (the real ones delegate to the tool router / wx app).

#include "tool_call_parser.h"
#include <cassert>
#include <iostream>
#include <string>

// ── Stubs for tool_invocation.cpp (router-backed in the real app) ──
bool IsKnownToolName(const std::string& name)
{
    return name == "write" || name == "overwrite_file" || name == "read" ||
           name == "pwd" || name == "ls" || name == "edit" ||
           name == "python_run_script" || name == "python_install_package";
}
bool ValidateToolArgs(const std::string& name,
                      const std::string& args,
                      std::string&       reasonOut)
{
    if ((name == "write" || name == "overwrite_file" || name == "read" ||
         name == "edit" || name == "python_run_script" ||
         name == "python_install_package") && args.empty()) {
        reasonOut = "missing required args for " + name;
        return false;
    }
    return true;
}

static int g_failures = 0;
#define CHECK(cond, label)                                            \
    do {                                                              \
        if (cond) { std::cout << "PASS  " << label << "\n"; }         \
        else      { std::cout << "FAIL  " << label << "\n"; ++g_failures; } \
    } while (0)

int main()
{
    // ── 1. The exact transcript failure: colon-less opener, valid XML
    //       body, stray </name> closer, EOS. Must now recover. ──
    {
        std::string filler;
        for (int i = 0; i < 130; ++i)
            filler += "*   some_source_file_" + std::to_string(i) + ".cpp\n";

        std::string text =
            "<|tool_call>call\n"
            "<name>write</name>\n"
            "<args>file_list.txt\n"
            "**C++ Files (.cpp):**\n" + filler +
            "</args>\n"
            "</name>\n";

        ParsedAssistantResponse p = ParseAssistantResponse(text);
        CHECK(p.hasInvocation,                       "transcript shape: recovered as invocation");
        CHECK(p.malformed.empty(),                   "transcript shape: no malformed entry");
        CHECK(p.invocation.valid,                    "transcript shape: invocation valid");
        CHECK(p.invocation.name == "write",          "transcript shape: name == write");
        CHECK(p.invocation.args.rfind("file_list.txt", 0) == 0,
                                                     "transcript shape: args start with filename");
        CHECK(p.invocation.args.find("</name>") == std::string::npos,
                                                     "transcript shape: stray closer not in args");
        CHECK(p.invocation.args.find("some_source_file_129.cpp") != std::string::npos,
                                                     "transcript shape: args content complete");
    }

    // ── 2. Plain XML opener, body, EOS with no closer at all. ──
    {
        std::string text =
            "<tool_call>\n<name>read</name>\n<args>foo.h</args>\n";
        ParsedAssistantResponse p = ParseAssistantResponse(text);
        CHECK(p.hasInvocation && p.invocation.valid &&
              p.invocation.name == "read" && p.invocation.args == "foo.h",
              "XML opener, EOS after </args>: recovered");
    }

    // ── 3. Stray closer with truncated final tag (cut mid-stream). ──
    {
        std::string text =
            "<|tool_call>call\n<name>pwd</name>\n<args></args>\n</tool_cal";
        ParsedAssistantResponse p = ParseAssistantResponse(text);
        CHECK(p.hasInvocation && p.invocation.valid && p.invocation.name == "pwd",
              "truncated closing tag at EOS: recovered");
    }

    // ── 4. Negative: trailing PROSE after body (not stray tags) must
    //       NOT recover — could be a literal protocol explanation. ──
    {
        std::string text =
            "<tool_call>\n<name>read</name>\n<args>foo.h</args>\n"
            "and then the closer goes here, like I was saying.";
        ParsedAssistantResponse p = ParseAssistantResponse(text);
        CHECK(!p.hasInvocation && !p.malformed.empty(),
              "trailing prose: NOT recovered, malformed");
        CHECK(p.malformed.front().reason.find("Format must be") != std::string::npos,
              "trailing prose: corrective reason includes format reminder");
    }

    // ── 5. Negative: unclosed <args> must NOT recover (fail closed). ──
    {
        std::string text =
            "<|tool_call>call\n<name>read</name>\n<args>some/path\n</name>";
        ParsedAssistantResponse p = ParseAssistantResponse(text);
        CHECK(!p.hasInvocation && !p.malformed.empty(),
              "unclosed <args>: NOT recovered");
    }

    // ── 6. Negative: opening tag in tail (not a closer) must NOT recover. ──
    {
        std::string text =
            "<tool_call>\n<name>read</name>\n<args>foo.h</args>\n<name>";
        ParsedAssistantResponse p = ParseAssistantResponse(text);
        CHECK(!p.hasInvocation, "opening tag in tail: NOT recovered");
    }

    // ── 7. Recovered-but-unknown tool surfaces the specific reason. ──
    {
        std::string text =
            "<|tool_call>call\n<name>frobnicate</name>\n<args>x</args>\n</name>";
        ParsedAssistantResponse p = ParseAssistantResponse(text);
        CHECK(p.hasInvocation && !p.invocation.valid &&
              p.invocation.invalidReason.find("unknown tool") != std::string::npos,
              "unknown tool via recovery: specific reason surfaced");
    }

    // ── 8. Regressions: previously working shapes still work. ──
    {
        std::string text =
            "Sure.\n<tool_call>\n<name>ls</name>\n<args>D:\\Music</args>\n</tool_call>\nDone.";
        ParsedAssistantResponse p = ParseAssistantResponse(text);
        CHECK(p.hasInvocation && p.invocation.valid && p.invocation.name == "ls" &&
              p.prose.find("Sure.") != std::string::npos &&
              p.prose.find("Done.") != std::string::npos,
              "regression: closed XML block with surrounding prose");
    }
    {
        std::string text = "<|tool_call>call:read{hello.txt}<tool_call|>";
        ParsedAssistantResponse p = ParseAssistantResponse(text);
        CHECK(p.hasInvocation && p.invocation.valid &&
              p.invocation.name == "read" && p.invocation.args == "hello.txt",
              "regression: gemma-native brace form");
    }
    {
        std::string text =
            "<|tool_call>call\n<name>read</name>\n<args>a.h</args>\n</tool_call>";
        ParsedAssistantResponse p = ParseAssistantResponse(text);
        CHECK(p.hasInvocation && p.invocation.valid && p.invocation.name == "read",
              "regression: colon-less hybrid WITH proper closer");
    }
    {
        std::string text =
            "<|tool_call>call:read{\n<args>hello.txt</args>\n}";
        ParsedAssistantResponse p = ParseAssistantResponse(text);
        CHECK(p.hasInvocation && p.invocation.valid &&
              p.invocation.name == "read" && p.invocation.args == "hello.txt",
              "regression: native hybrid without closer (final brace)");
    }
    {
        std::string text = "Just a normal prose answer, no tools.";
        ParsedAssistantResponse p = ParseAssistantResponse(text);
        CHECK(!p.hasInvocation && p.malformed.empty() && p.prose == text,
              "regression: pure prose untouched");
    }

    // ── 9. Preview: head+tail with omission marker; tail evidence kept. ──
    {
        std::string big(5000, 'A');
        std::string text =
            "<tool_call>\n<name>read</name>\n<args>" + big + "\nthen prose follows";
        // unclosed args + prose -> malformed; rawText must be previewed
        ParsedAssistantResponse p = ParseAssistantResponse(text);
        CHECK(!p.hasInvocation && !p.malformed.empty(),
              "preview case: malformed as expected");
        const std::string& rt = p.malformed.front().rawText;
        CHECK(rt.size() < 1500,
              "preview: rawText capped (got " + std::to_string(rt.size()) + " bytes)");
        CHECK(rt.find("bytes omitted") != std::string::npos,
              "preview: omission marker present");
        CHECK(rt.find("<tool_call>") != std::string::npos,
              "preview: head retained");
        CHECK(rt.find("then prose follows") != std::string::npos,
              "preview: TAIL retained (where the evidence lives)");
        CHECK(MakeToolCallDiagnosticPreview("short") == "short",
              "preview: small blocks pass through");
    }

    // ── 10. Streaming detector regression: normal closed block. ──
    {
        ToolCallStreamDetector det;
        bool fired = false;
        fired |= det.Feed("Let me check.\n<tool_ca");
        fired |= det.Feed("ll>\n<name>pwd</name>\n<ar");
        fired |= det.Feed("gs></args>\n</tool_call>");
        CHECK(fired && det.Complete() && det.GetInvocation().valid &&
              det.GetInvocation().name == "pwd",
              "regression: streaming detector across split deltas");
    }

    // ── 11. Sixth gemma drift shape (2026-06-11 transcript): colon
    //        opener carrying the name, XML <args> body, NO <name> tag,
    //        proper </tool_call> closer.  Previously the brace parser
    //        found no '{' and silently cleared args, so the call
    //        dispatched empty and validation produced a misleading
    //        "requires a filename" error. ──
    {
        std::string text =
            "<|tool_call>call:python_run_script\n"
            "<args>cli_downloader.py\n"
            "https://www.youtube.com/watch?v=GzLfCMu2G8o</args>\n"
            "</tool_call>";

        ParsedAssistantResponse p = ParseAssistantResponse(text);
        CHECK(p.hasInvocation,                       "hybrid args: invocation found");
        CHECK(p.invocation.valid,                    "hybrid args: invocation valid");
        CHECK(p.invocation.name == "python_run_script",
                                                     "hybrid args: name parsed from colon opener");
        CHECK(p.invocation.args.rfind("cli_downloader.py", 0) == 0,
                                                     "hybrid args: args line 1 is the script");
        CHECK(p.invocation.args.find("watch?v=GzLfCMu2G8o") != std::string::npos,
                                                     "hybrid args: argv URL preserved");
        CHECK(p.invocation.args.find("<args>") == std::string::npos,
                                                     "hybrid args: tags not leaked into args");
    }

    // Same shape with an unterminated <args> must fail closed, not
    // dispatch with empty/truncated args.
    {
        std::string text =
            "<|tool_call>call:python_run_script\n"
            "<args>cli_downloader.py\n"
            "</tool_call>";

        ParsedAssistantResponse p = ParseAssistantResponse(text);
        CHECK(p.hasInvocation || !p.malformed.empty(),
              "hybrid args unterminated: surfaced (invocation or malformed)");
        if (p.hasInvocation) {
            CHECK(!p.invocation.valid,
                  "hybrid args unterminated: invocation marked invalid");
        }
    }

    // Brace shape through the same opener must keep working untouched.
    {
        std::string text = "<|tool_call>call:read{hello.txt}<tool_call|>";
        ParsedAssistantResponse p = ParseAssistantResponse(text);
        CHECK(p.hasInvocation && p.invocation.valid &&
              p.invocation.name == "read" &&
              p.invocation.args == "hello.txt",
              "hybrid args: brace form regression intact");
    }


    // ── 12. Seventh gemma drift shape (2026-06-12 transcript): colon
    //        opener carrying the name, then a mistaken XML-ish argument tag
    //        (<name> or <path>) and only </args> at EOS.  This is terminal
    //        and unambiguous, so recover instead of burning malformed strikes. ──
    {
        std::string text =
            "<|tool_call>call:python_install_package<name>yt-dlp</args>";

        ParsedAssistantResponse p = ParseAssistantResponse(text);
        CHECK(p.hasInvocation,                       "tagged args: package install recovered");
        CHECK(p.invocation.valid,                    "tagged args: package install valid");
        CHECK(p.invocation.name == "python_install_package",
                                                     "tagged args: package tool name");
        CHECK(p.invocation.args == "yt-dlp",        "tagged args: package arg preserved");
    }
    {
        std::string text =
            "<|tool_call>call:overwrite_file<path>main.py\n"
            "print('hello')\n"
            "</args>";

        ParsedAssistantResponse p = ParseAssistantResponse(text);
        CHECK(p.hasInvocation,                       "tagged args: path/content recovered");
        CHECK(p.invocation.valid,                    "tagged args: path/content valid");
        CHECK(p.invocation.name == "overwrite_file", "tagged args: overwrite tool name");
        CHECK(p.invocation.args.rfind("main.py\n", 0) == 0,
                                                     "tagged args: path kept as first line");
        CHECK(p.invocation.args.find("print('hello')") != std::string::npos,
                                                     "tagged args: content preserved");
        CHECK(p.invocation.args.find("<path>") == std::string::npos,
                                                     "tagged args: fake tag not leaked");
    }
    {
        std::string text =
            "<|tool_call>call:python_install_package<name>yt-dlp</args>\n"
            "and then some prose";

        ParsedAssistantResponse p = ParseAssistantResponse(text);
        CHECK(!p.hasInvocation && !p.malformed.empty(),
              "tagged args with trailing prose: NOT recovered");
    }

    // ── Eighth drift shape: name on call line, newline-separated args,
    //    bare final </args> (2026-06-12 yt-dlp transcripts) ──────────
    {
        std::string text =
            "<|tool_call>call:write\n"
            "main.py\n"
            "import sys\n"
            "print('hello')\n"
            "</args>";

        ParsedAssistantResponse p = ParseAssistantResponse(text);
        CHECK(p.hasInvocation,                  "newline args: write recovered");
        CHECK(p.invocation.valid,               "newline args: write valid");
        CHECK(p.invocation.name == "write",     "newline args: write tool name");
        CHECK(p.invocation.args.rfind("main.py\n", 0) == 0,
                                                "newline args: path first line");
        CHECK(p.invocation.args.find("print('hello')") != std::string::npos,
                                                "newline args: content preserved");
        CHECK(p.invocation.args.find("</args>") == std::string::npos,
                                                "newline args: closer not leaked");
    }
    {
        // The exact python_run_script shape that burned the malformed cap.
        std::string text =
            "<|tool_call>call:python_run_script\n"
            "main.py\n"
            "https://www.youtube.com/shorts/pYvR0YGvyik\n"
            "</args>";

        ParsedAssistantResponse p = ParseAssistantResponse(text);
        CHECK(p.hasInvocation,                  "newline args: run_script recovered");
        CHECK(p.invocation.valid,               "newline args: run_script valid");
        CHECK(p.invocation.name == "python_run_script",
                                                "newline args: run_script name");
        CHECK(p.invocation.args ==
              "main.py\nhttps://www.youtube.com/shorts/pYvR0YGvyik",
                                                "newline args: script+argv preserved");
    }
    {
        // Trailing whitespace and newlines after the closer are fine.
        std::string text =
            "Sure, running it now.\n"
            "<|tool_call>call:python_run_script\n"
            "main.py\n"
            "</args>\n   \n";

        ParsedAssistantResponse p = ParseAssistantResponse(text);
        CHECK(p.hasInvocation,                  "newline args: ws tail recovered");
        CHECK(p.invocation.valid,               "newline args: ws tail valid");
        CHECK(p.prose.rfind("Sure, running it now.", 0) == 0,
                                                "newline args: prose preserved");
    }
    {
        // A literal </args> inside written file content must stay in the
        // payload — the LAST closer terminates the block.
        std::string text =
            "<|tool_call>call:write\n"
            "notes.txt\n"
            "the protocol closer is </args> by mistake sometimes\n"
            "</args>";

        ParsedAssistantResponse p = ParseAssistantResponse(text);
        CHECK(p.hasInvocation,                  "newline args: embedded closer recovered");
        CHECK(p.invocation.valid,               "newline args: embedded closer valid");
        CHECK(p.invocation.args.find("is </args> by mistake") != std::string::npos,
                                                "newline args: embedded closer kept in payload");
    }
    {
        // Prose AFTER the final </args> must NOT dispatch.
        std::string text =
            "<|tool_call>call:write\n"
            "main.py\n"
            "print('x')\n"
            "</args>\n"
            "Let me know if you need anything else.";

        ParsedAssistantResponse p = ParseAssistantResponse(text);
        CHECK(!p.hasInvocation && !p.malformed.empty(),
              "newline args with trailing prose: NOT recovered");
    }
    {
        // Non-identifier call line (protocol discussion) must NOT dispatch.
        std::string text =
            "<|tool_call>call:write the file like this\n"
            "main.py\n"
            "</args>";

        ParsedAssistantResponse p = ParseAssistantResponse(text);
        CHECK(!p.hasInvocation && !p.malformed.empty(),
              "newline args with prose call line: NOT recovered");
    }
    {
        // XML opener is NOT eligible for this gemma-only recovery.
        std::string text =
            "<tool_call>write\n"
            "main.py\n"
            "print('x')\n"
            "</args>";

        ParsedAssistantResponse p = ParseAssistantResponse(text);
        CHECK(!p.hasInvocation,
              "newline args: xml opener not eligible");
    }
    {
        // Unknown tool name recovers as an INVALID invocation (arg-level
        // coaching) rather than a protocol-level malformed block.
        std::string text =
            "<|tool_call>call:wrte\n"
            "main.py\n"
            "</args>";

        ParsedAssistantResponse p = ParseAssistantResponse(text);
        CHECK(p.hasInvocation && !p.invocation.valid,
              "newline args: unknown tool surfaces as invalid invocation");
        CHECK(p.invocation.invalidReason.find("unknown tool") != std::string::npos,
              "newline args: unknown-tool reason");
    }

    std::cout << (g_failures == 0 ? "\nALL TESTS PASSED\n"
                                  : "\nFAILURES: " + std::to_string(g_failures) + "\n");
    return g_failures == 0 ? 0 : 1;
}
