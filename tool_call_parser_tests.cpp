// Regression harness for tool_call_parser.
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
    // ── 1. Colon-less opener, valid XML body, stray </name> closer,
    //       EOS. Must recover. ──
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

    // ── 8. Baseline shapes still work. ──
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

    // ── 10. Streaming detector: normal closed block. ──
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

    // ── 11. Gemma drift: colon opener carrying the name, XML <args>
    //        body, NO <name> tag, proper </tool_call> closer.  The brace
    //        parser would find no '{' and silently clear args, so the
    //        call would dispatch empty with a misleading "requires a
    //        filename" error. ──
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


    // ── 12. Gemma drift: colon opener carrying the name, then a
    //        mistaken XML-ish argument tag (<name> or <path>) and only
    //        </args> at EOS.  This is terminal and unambiguous, so
    //        recover instead of burning malformed strikes. ──
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

    // ── Gemma drift: name on call line, newline-separated args, bare
    //    final </args> ─────────────────────────────────────────────
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

    // ── Reasoning spans are never tool calls. ───────────────────────
    // A model quoting the protocol inside its reasoning must not have
    // the quote dispatched (PowerShell would execute "...").
    const std::string kThinkExample =
        "<think>The tool call format keeps failing. I need to use the exact "
        "XML format: <tool_call><name>pwd</name><args>...</args></tool_call> "
        "Let me try again with the correct format.\n</think>\n\n";
    {
        // Exact screenshot shape: example in reasoning, broken real call.
        std::string text = kThinkExample +
            "<tool_call>\n<function>pwd</function>\n</tool_call>";
        ParsedAssistantResponse p = ParseAssistantResponse(text);
        // The real call uses Qwen's <function>pwd</function> drift, which
        // the function-tag shim recovers; the quoted "..." must not win.
        CHECK(!(p.hasInvocation && p.invocation.args == "..."),
              "think: quoted example inside <think> does not dispatch");
        CHECK(p.hasInvocation && p.invocation.valid &&
              p.invocation.name == "pwd" && p.invocation.args.empty(),
              "think: the real call after </think> is what gets parsed");
        CHECK(p.prose.find("<name>pwd</name><args>...</args>") != std::string::npos,
              "think: quoted example stays in prose untouched");
    }
    {
        // Valid call after reasoning still dispatches.
        std::string text = kThinkExample +
            "<tool_call><name>read</name><args>a.txt</args></tool_call>";
        ParsedAssistantResponse p = ParseAssistantResponse(text);
        CHECK(p.hasInvocation && p.invocation.valid &&
              p.invocation.name == "read" && p.invocation.args == "a.txt",
              "think: real call after </think> dispatches");
        CHECK(p.prose == kThinkExample,
              "think: prose keeps the full reasoning block");
    }
    {
        // Unterminated reasoning: nothing inside it may dispatch.
        std::string text =
            "<think>maybe <tool_call><name>pwd</name></tool_call> then";
        ParsedAssistantResponse p = ParseAssistantResponse(text);
        CHECK(!p.hasInvocation && p.malformed.empty(),
              "think: unterminated <think> hides openers (fail closed)");
    }
    {
        // Orphan </think> (template prefilled <think> in the prompt).
        std::string text =
            "reasoning: <tool_call><name>pwd</name></tool_call>\n</think>\n"
            "<tool_call><name>read</name><args>b.txt</args></tool_call>";
        ParsedAssistantResponse p = ParseAssistantResponse(text);
        CHECK(p.hasInvocation && p.invocation.name == "read" &&
              p.invocation.args == "b.txt",
              "think: orphan </think> prefix is reasoning; call after it dispatches");
    }
    {
        // Two reasoning blocks; call sits between them.
        std::string text =
            "<think>a <tool_call><name>pwd</name></tool_call></think>"
            "<tool_call><name>read</name><args>c.txt</args></tool_call>"
            "<think>b</think>";
        ParsedAssistantResponse p = ParseAssistantResponse(text);
        CHECK(p.hasInvocation && p.invocation.name == "read",
              "think: call between two reasoning blocks dispatches");
    }
    {
        // Streaming, one byte per delta: the quoted example must not fire,
        // must reach the display as prose, and the real call must fire.
        std::string text = kThinkExample +
            "<tool_call><name>read</name><args>d.txt</args></tool_call>";
        ToolCallStreamDetector det;
        bool firedEarly = false;
        bool fired = false;
        for (size_t i = 0; i < text.size(); ++i) {
            bool f = det.Feed(std::string(1, text[i]));
            if (f && i + 1 < kThinkExample.size()) firedEarly = true;
            fired |= f;
        }
        CHECK(!firedEarly,
              "think stream: quoted example inside reasoning does not fire");
        CHECK(fired && det.GetInvocation().valid &&
              det.GetInvocation().name == "read" &&
              det.GetInvocation().args == "d.txt",
              "think stream: real call after </think> fires");
        CHECK(det.GetProsePrefix() == kThinkExample,
              "think stream: reasoning (with example) published as prose");
    }
    {
        // Streaming with markers split at awkward boundaries.
        ToolCallStreamDetector det;
        bool fired = false;
        fired |= det.Feed("<thi");
        fired |= det.Feed("nk>x <tool_call><name>pwd</name></tool_ca");
        fired |= det.Feed("ll> y</thi");
        fired |= det.Feed("nk>\nok <tool_ca");
        fired |= det.Feed("ll><name>pwd</name></tool_call>");
        CHECK(fired && det.GetInvocation().valid &&
              det.GetProsePrefix() ==
                  "<think>x <tool_call><name>pwd</name></tool_call> y</think>\nok ",
              "think stream: split <think>/</think> tags handled");
    }
    {
        // Stream that ends mid-reasoning: never fires, held bytes flushable.
        ToolCallStreamDetector det;
        bool fired = det.Feed("<think>thinking about <tool_call><name>pwd</name>");
        fired |= det.Feed("</tool_call> still going");
        CHECK(!fired && !det.Complete() &&
              !ContainsToolCallOpenMarker(det.GetHeldBuffer()),
              "think stream: open reasoning never fires; held tail is safe to flush");
    }

    // ── Leaked reasoning. ───────────────────────────────────────────
    // llama-server ends reasoning_content at the first "<tool_call>" the
    // model writes, even inside its thinking.  ChatClient closes the
    // wrapped block there; the rest of the thought arrives as content,
    // closed by the model's own stray </think>.
    const std::string kBrokenReal =
        "<tool_call>\n<function>read</name>\n<args>@timeout=300\n"
        "a.txt\n</args>\n</tool_call>";
    {
        // A MENTION of <tool_call> in reasoning.
        std::string text =
            "<think>I need to fix the format of the tool call - I used "
            "`<function>` instead of `</think>\n"
            "<tool_call>`. Let me retry with the correct format.\n</think>\n\n" +
            kBrokenReal;
        ParsedAssistantResponse p = ParseAssistantResponse(text);
        CHECK(p.hasInvocation && p.invocation.valid &&
              p.invocation.name == "read" &&
              p.invocation.rawBlock.rfind("<tool_call>\n<function>", 0) == 0,
              "leak: mention before stray </think> skipped; real block parsed");
    }
    {
        // A complete quoted example, more reasoning, stray </think>,
        // then the real (broken) attempt.
        std::string text =
            "<think>I need to fix the tool call format. The correct format "
            "is as follows:\n</think>\n"
            "<tool_call>\n<name>read</name>\n<args>...</args>\n</tool_call>\n"
            "That should work.\n</think>\n\n" + kBrokenReal;
        ParsedAssistantResponse p = ParseAssistantResponse(text);
        CHECK(p.hasInvocation && p.invocation.valid &&
              p.invocation.args != "..." &&
              p.invocation.args.find("a.txt") != std::string::npos,
              "leak: quoted example before stray </think> never dispatches");
    }
    {
        // Same, but the real attempt is well-formed: it must dispatch.
        std::string text =
            "<think>format is:\n</think>\n"
            "<tool_call><name>read</name><args>...</args></tool_call>\n"
            "ok.\n</think>\n"
            "<tool_call><name>read</name><args>real.txt</args></tool_call>";
        ParsedAssistantResponse p = ParseAssistantResponse(text);
        CHECK(p.hasInvocation && p.invocation.valid &&
              p.invocation.args == "real.txt",
              "leak: real call after the stray </think> dispatches");
    }
    {
        // A real write whose content contains "</think>" must dispatch,
        // with or without reasoning before it.
        std::string call =
            "<tool_call><name>write</name><args>chat_display.cpp\n"
            "const std::string kEnd = \"</think>\";\n</args></tool_call>";
        ParsedAssistantResponse p1 = ParseAssistantResponse(call);
        CHECK(p1.hasInvocation && p1.invocation.valid &&
              p1.invocation.name == "write" &&
              p1.invocation.args.find("\"</think>\"") != std::string::npos,
              "args: </think> inside write args still dispatches");
        ParsedAssistantResponse p2 =
            ParseAssistantResponse("<think>plan</think>\n" + call);
        CHECK(p2.hasInvocation && p2.invocation.valid &&
              p2.invocation.name == "write",
              "args: </think> in args after wrapped reasoning still dispatches");
    }
    {
        // Prose mention of <name> in leaked reasoning is not a started call.
        std::string text =
            "<tool_call> needs <name> and <args> tags.\n</think>\n"
            "<tool_call><name>read</name><args>b.txt</args></tool_call>";
        ParsedAssistantResponse p = ParseAssistantResponse(text);
        CHECK(p.hasInvocation && p.invocation.valid &&
              p.invocation.args == "b.txt",
              "leak: <name> mentioned without </name> is still reasoning");
    }
    {
        // Placeholder guard: nothing structural distinguishes this from
        // a real call, so the args themselves are rejected.
        ParsedAssistantResponse p = ParseAssistantResponse(
            "<tool_call><name>read</name><args>...</args></tool_call>");
        CHECK(p.hasInvocation && !p.invocation.valid &&
              p.invocation.invalidReason.find("placeholder") != std::string::npos,
              "placeholder: args \"...\" rejected, not executed");
        ParsedAssistantResponse p2 = ParseAssistantResponse(
            "<tool_call><name>read</name><args>notes...txt</args></tool_call>");
        CHECK(p2.hasInvocation && p2.invocation.valid,
              "placeholder: args merely containing ... still valid");
    }
    {
        // Streaming, byte by byte: the mention must not swallow the
        // display; the real block fires.
        std::string text =
            "<think>I used `<function>` instead of `</think>\n"
            "<tool_call>`. Let me retry.\n</think>\n\n"
            "<tool_call><name>read</name><args>c.txt</args></tool_call>";
        ToolCallStreamDetector det;
        bool fired = false;
        for (char ch : text) fired |= det.Feed(std::string(1, ch));
        CHECK(fired && det.GetInvocation().valid &&
              det.GetInvocation().args == "c.txt" &&
              det.GetProsePrefix().find("Let me retry.") != std::string::npos,
              "leak stream: mention returned to prose; real call fires");
    }
    {
        // Streaming: a real write with </think> in args still fires valid.
        ToolCallStreamDetector det;
        bool fired = det.Feed(
            "<tool_call><name>write</name><args>x.cpp\n\"</think>\"\n"
            "</args></tool_call>");
        CHECK(fired && det.GetInvocation().valid &&
              det.GetInvocation().name == "write",
              "args stream: </think> inside write args still fires");
    }

    // ── Qwen function-tag drift. ────────────────────────────────────
    auto parse = [](const std::string& t) { return ParseAssistantResponse(t); };
    {
        // Real-world shape (tool swapped to a stubbed one).
        ParsedAssistantResponse p = parse(
            "I'll search your D: drive first.\n\n"
            "<tool_call>\n<function>read</name>\n"
            "<args>Get-ChildItem -Path 'D:\\' -Recurse | Out-String -Width 300</args>\n"
            "</tool_call>");
        CHECK(p.hasInvocation && p.invocation.valid &&
              p.invocation.name == "read" &&
              p.invocation.args.rfind("Get-ChildItem", 0) == 0,
              "qwen: <function>NAME</name> + <args> recovers");
        CHECK(p.prose == "I'll search your D: drive first.\n\n",
              "qwen: prose before the block preserved");
    }
    {
        ParsedAssistantResponse p = parse(
            "<tool_call>\n<function=read</name>\n<args>x.txt</args>\n</tool_call>");
        CHECK(p.hasInvocation && p.invocation.valid && p.invocation.args == "x.txt",
              "qwen: <function=NAME</name> + <args> recovers");
    }
    {
        // Timeout directive rides along untouched in args.
        ParsedAssistantResponse p = parse(
            "<tool_call>\n<function>read</name>\n<args>@timeout=300\nx.txt\n</args>\n</tool_call>");
        CHECK(p.hasInvocation && p.invocation.valid &&
              p.invocation.args == "@timeout=300\nx.txt",
              "qwen: @timeout directive preserved in args");
    }
    {
        ParsedAssistantResponse p = parse(
            "<tool_call>\n<function=read>\n<args>x.txt</args>\n</function>\n</tool_call>");
        CHECK(p.hasInvocation && p.invocation.valid && p.invocation.args == "x.txt",
              "qwen: <function=NAME> + <args> + </function> recovers");
    }
    {
        // Qwen3-Coder native, single parameter.
        ParsedAssistantResponse p = parse(
            "<tool_call>\n<function=read>\n<parameter=path>\nx.txt\n</parameter>\n"
            "</function>\n</tool_call>");
        CHECK(p.hasInvocation && p.invocation.valid && p.invocation.args == "x.txt",
              "qwen: native single <parameter=…> recovers");
    }
    {
        // Native with two parameters: not mapped, error names the mistake.
        ParsedAssistantResponse p = parse(
            "<tool_call>\n<function=write>\n<parameter=path>a</parameter>\n"
            "<parameter=content>b</parameter>\n</function>\n</tool_call>");
        CHECK(!p.hasInvocation && p.malformed.size() == 1 &&
              p.malformed[0].reason.find("you wrote \"<function=write>\"") !=
                  std::string::npos,
              "qwen: multi-parameter body refused with a specific reason");
    }
    {
        // No-args tool via <function>NAME</function>.
        ParsedAssistantResponse p = parse(
            "<tool_call>\n<function>pwd</function>\n</tool_call>");
        CHECK(p.hasInvocation && p.invocation.valid && p.invocation.name == "pwd",
              "qwen: <function>NAME</function> for a no-args tool recovers");
    }
    {
        // Required args missing: recognized, but invalid with the tool's reason.
        ParsedAssistantResponse p = parse(
            "<tool_call>\n<function>read</function>\n</tool_call>");
        CHECK(p.hasInvocation && !p.invocation.valid &&
              p.invocation.invalidReason.find("missing required args") !=
                  std::string::npos,
              "qwen: recovered call still goes through ValidateToolArgs");
    }
    {
        // Unknown tool name stays an unknown-tool error.
        ParsedAssistantResponse p = parse(
            "<tool_call><function>rm_rf</name><args>C:\\</args></tool_call>");
        CHECK(p.hasInvocation && !p.invocation.valid &&
              p.invocation.invalidReason.find("unknown tool") != std::string::npos,
              "qwen: unknown tool through the shim is rejected");
    }
    {
        // Fail closed: unclosed <args>, prose before the tag, trailing junk.
        ParsedAssistantResponse a = parse(
            "<tool_call><function>read</name><args>x.txt</tool_call>");
        ParsedAssistantResponse b = parse(
            "<tool_call>I will call <function>read</name><args>x.txt</args></tool_call>");
        ParsedAssistantResponse c = parse(
            "<tool_call><function>read</name><args>x.txt</args> and more</tool_call>");
        CHECK(!a.hasInvocation && !b.hasInvocation && !c.hasInvocation,
              "qwen: partial args / leading prose / trailing text not recovered");
        CHECK(a.malformed.size() == 1 &&
              a.malformed[0].reason.find("you wrote") != std::string::npos,
              "qwen: unrecovered function tag gets the specific reason");
    }
    {
        // Placeholder guard still applies through the shim.
        ParsedAssistantResponse p = parse(
            "<tool_call><function>read</name><args>...</args></tool_call>");
        CHECK(p.hasInvocation && !p.invocation.valid &&
              p.invocation.invalidReason.find("placeholder") != std::string::npos,
              "qwen: placeholder args rejected through the shim");
    }
    {
        // Leak rule: a function-tag write whose content has </think>
        // is a started call, so it still dispatches.
        ParsedAssistantResponse p = parse(
            "<think>plan</think>\n<tool_call><function>write</name>"
            "<args>x.cpp\n\"</think>\"\n</args></tool_call>");
        CHECK(p.hasInvocation && p.invocation.valid && p.invocation.name == "write",
              "qwen: function-tag write with </think> in args dispatches");
    }
    {
        // Streaming detector recovers the same shape across split deltas.
        ToolCallStreamDetector det;
        bool fired = false;
        fired |= det.Feed("Searching.\n<tool_ca");
        fired |= det.Feed("ll>\n<function>re");
        fired |= det.Feed("ad</name>\n<args>x.txt</ar");
        fired |= det.Feed("gs>\n</tool_call>");
        CHECK(fired && det.GetInvocation().valid &&
              det.GetInvocation().name == "read" &&
              det.GetInvocation().args == "x.txt",
              "qwen stream: function-tag call recovered across deltas");
    }

    {
        // <function>NAME> (no slash).
        ParsedAssistantResponse p = ParseAssistantResponse(
            "<tool_call>\n<function>read>\n"
            "<args>Get-ChildItem -Path 'D:\\' -Recurse -File -Filter '*child*'</args>\n"
            "</tool_call>");
        CHECK(p.hasInvocation && p.invocation.valid &&
              p.invocation.name == "read" &&
              p.invocation.args.rfind("Get-ChildItem -Path 'D:\\'", 0) == 0,
              "qwen: <function>NAME> + <args> recovers");
        // Still identifier-only: prose inside the tag does not recover.
        ParsedAssistantResponse q = ParseAssistantResponse(
            "<tool_call><function>read the file><args>x.txt</args></tool_call>");
        CHECK(!q.hasInvocation,
              "qwen: <function>words with spaces> is not recovered");
    }

    std::cout << (g_failures == 0 ? "\nALL TESTS PASSED\n"
                                  : "\nFAILURES: " + std::to_string(g_failures) + "\n");
    return g_failures == 0 ? 0 : 1;
}
