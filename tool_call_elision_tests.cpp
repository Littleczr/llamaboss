// tool_call_elision_tests.cpp
//
// Regression harness for tool_call_elision.h (2026-10-01, open item A).
// Poco::JSON only, no wx:
//   g++ -std=c++17 -I . tool_call_elision_tests.cpp -lPocoJSON -lPocoFoundation && ./a.out
// (Windows: link PocoJSON/PocoFoundation the same way the app does.)
//
// Pins: valid JSON out, key order kept, UTF-8-safe cuts, idempotence,
// small arguments untouched, the stored tool_calls array never mutated,
// and the Responses adapter contract behind "drop the sidecar":
//   * shortened arguments + the ORIGINAL sidecar -> the adapter throws
//     "does not match the executed tool call" (why the sidecar must go);
//   * shortened arguments + no sidecar -> legacy replay succeeds and
//     carries the shortened arguments.
// And the copy guard (2026-10-01 live run: the model re-issued shortened
// calls verbatim): cut values are spooled and the marker names the file;
// ContainsArgElisionMarker matches real markers (new and first-shipped
// shapes, raw JSON or decoded) but not a bare grep for the phrase.
#include "tool_call_elision.h"
#include "openai_responses.h"

#include <iostream>
#include <string>

using namespace lb_toolcall_elision;
using Poco::JSON::Array;
using Poco::JSON::Object;

static int fails = 0, passes = 0;
static void check(bool ok, const std::string& what)
{
    if (ok) ++passes; else ++fails;
    std::cout << (ok ? "PASS " : "FAIL ") << what << "\n";
}

static Object::Ptr ParseObj(const std::string& s)
{
    Poco::JSON::Parser p(new Poco::JSON::ParseHandler(true));
    return p.parse(s).extract<Object::Ptr>();
}

static std::string JStr(const std::string& s)   // JSON string literal
{
    std::ostringstream o;
    Poco::JSON::Stringifier::stringify(Poco::Dynamic::Var(s), o);
    return o.str();
}

static std::string BigScript(std::size_t n)
{
    std::string s = "$ErrorActionPreference = 'Continue'\n";
    while (s.size() < n) s += "Get-ChildItem -LiteralPath \"C:\\Users\\cesar\\src\" -Recurse | Out-Null\n";
    return s;
}

static Array::Ptr MakeToolCalls(const std::string& id, const std::string& args)
{
    Array::Ptr calls = new Array;
    Object::Ptr fn = new Object;
    fn->set("name", std::string("run_command"));
    fn->set("arguments", args);
    Object::Ptr call = new Object;
    call->set("id", id);
    call->set("type", std::string("function"));
    call->set("function", fn);
    calls->add(call);
    return calls;
}

int main()
{
    // 1. Small arguments are returned byte-identical.
    {
        const std::string small = "{\"command\":\"Get-Location\"}";
        check(ShortenToolCallArguments(small) == small, "small args unchanged");
    }

    // 2. Large script: valid JSON, keys and order kept, value cut + marker.
    {
        const std::string script = BigScript(3000);
        const std::string args = "{\"command\":" + JStr(script) +
                                 ",\"cwd\":\"C:\\\\work\",\"timeout_ms\":120000}";
        const std::string out = ShortenToolCallArguments(args);
        check(out.size() < 600, "3 KB script shrinks below 600 bytes (" + std::to_string(out.size()) + ")");
        Object::Ptr o;
        try { o = ParseObj(out); } catch (...) {}
        check(o != nullptr, "shortened args are valid JSON");
        if (o) {
            std::vector<std::string> names; o->getNames(names);
            check(names.size() == 3 && names[0] == "command" && names[1] == "cwd" &&
                  names[2] == "timeout_ms", "key order preserved");
            const std::string cmd = o->getValue<std::string>("command");
            check(cmd.compare(0, 40, script.substr(0, 40)) == 0, "command keeps its head");
            check(cmd.find("already-executed tool call elided") != std::string::npos, "marker present");
            check(o->getValue<std::string>("cwd") == "C:\\work", "short value untouched");
            check(o->getValue<int>("timeout_ms") == 120000, "number untouched");
        }
        // 3. Idempotent: shortening again changes nothing.
        check(ShortenToolCallArguments(out) == out, "idempotent");
    }

    // 4. UTF-8: never cut inside a multibyte sequence.
    {
        std::string s;
        while (s.size() < 2000) s += "\xC3\xA9\xE2\x82\xAC";   // é €
        for (std::size_t keep = 195; keep <= 205; ++keep) {
            const std::string out = ShortenToolCallArguments("{\"content\":" + JStr(s) + "}", keep);
            Object::Ptr o;
            try { o = ParseObj(out); } catch (...) {}
            bool ok = o != nullptr;
            if (ok) {
                const std::string v = o->getValue<std::string>("content");
                const std::size_t m = v.find("... [");
                ok = m != std::string::npos && m <= keep;
                // Head must be whole é€ units (2 or 3 bytes) - check by validity:
                for (std::size_t i = 0; ok && i < m; ) {
                    unsigned char c = (unsigned char)v[i];
                    std::size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 0;
                    ok = len && i + len <= m; i += len ? len : 1;
                }
            }
            if (!ok) { check(false, "utf8 cut at keep=" + std::to_string(keep)); goto utf8done; }
        }
        check(true, "utf8 cuts land on sequence boundaries (keep 195..205)");
    }
utf8done:

    // 5. Nested arrays/objects are walked.
    {
        const std::string big = BigScript(1500);
        const std::string args = "{\"edits\":[{\"path\":\"a.cpp\",\"new_text\":" + JStr(big) +
                                 "}],\"dry_run\":false}";
        const std::string out = ShortenToolCallArguments(args);
        Object::Ptr o; try { o = ParseObj(out); } catch (...) {}
        check(o && o->getArray("edits") && o->getArray("edits")->getObject(0)->getValue<std::string>("path") == "a.cpp" &&
              o->getArray("edits")->getObject(0)->getValue<std::string>("new_text").size() < 400 &&
              o->getValue<bool>("dry_run") == false, "nested string shortened, structure kept");
    }

    // 6. Many small values: falls back to single-key form, still valid.
    {
        std::string args = "{\"paths\":[";
        for (int i = 0; i < 200; ++i) args += (i ? "," : "") + JStr("C:\\dir\\file_" + std::to_string(i) + ".txt");
        args += "]}";
        const std::string out = ShortenToolCallArguments(args);
        Object::Ptr o; try { o = ParseObj(out); } catch (...) {}
        check(o && o->has("_elided_arguments") && out.size() < 600, "many-small-values fallback");
    }

    // 7. Non-JSON arguments: fallback, valid JSON.
    {
        const std::string raw = "not json " + BigScript(1000);
        const std::string out = ShortenToolCallArguments(raw);
        Object::Ptr o; try { o = ParseObj(out); } catch (...) {}
        check(o && o->getValue<std::string>("_elided_arguments").compare(0, 9, "not json ") == 0,
              "non-JSON args fallback");
    }

    // 8. Array copy: stored array untouched; nullptr when nothing changes.
    {
        const std::string args = "{\"command\":" + JStr(BigScript(3000)) + "}";
        Array::Ptr stored = MakeToolCalls("call_1", args);
        Array::Ptr shorter = ShortenToolCallsArray(stored);
        check(shorter != nullptr, "array copy produced");
        check(stored->getObject(0)->getObject("function")->getValue<std::string>("arguments") == args,
              "stored tool_calls not mutated");
        check(shorter && shorter->getObject(0)->getValue<std::string>("id") == "call_1" &&
              shorter->getObject(0)->getObject("function")->getValue<std::string>("name") == "run_command",
              "copy keeps id and name");
        check(ShortenToolCallsArray(MakeToolCalls("c", "{\"command\":\"pwd\"}")) == nullptr,
              "nullptr when nothing to shorten");
    }

    // 9. Responses adapter contract.
    {
        const std::string args = "{\"command\":" + JStr(BigScript(3000)) + "}";
        Array::Ptr stored = MakeToolCalls("call_9", args);
        Array::Ptr shorter = ShortenToolCallsArray(stored);

        // Saved Responses output exactly as the model emitted it.
        Array::Ptr sidecar = new Array;
        Object::Ptr reasoning = new Object;
        reasoning->set("type", std::string("reasoning"));
        reasoning->set("id", std::string("rs_1"));
        reasoning->set("encrypted_content", std::string("opaque"));
        sidecar->add(reasoning);
        Object::Ptr fc = new Object;
        fc->set("type", std::string("function_call"));
        fc->set("id", std::string("fc_1"));
        fc->set("call_id", std::string("call_9"));
        fc->set("name", std::string("run_command"));
        fc->set("arguments", args);
        sidecar->add(fc);

        auto buildWith = [&](bool keepSidecar) -> std::string {
            Object::Ptr root = new Object;
            root->set("model", std::string("gpt-6-luna"));
            root->set("stream", true);
            Array::Ptr msgs = new Array;
            Object::Ptr u = new Object; u->set("role", std::string("user")); u->set("content", std::string("go"));
            msgs->add(u);
            Object::Ptr a = new Object;
            a->set("role", std::string("assistant"));
            a->set("tool_calls", shorter);
            if (keepSidecar) a->set(lb_responses::kOutputSidecarKey(), sidecar);
            msgs->add(a);
            Object::Ptr t = new Object;
            t->set("role", std::string("tool"));
            t->set("tool_call_id", std::string("call_9"));
            t->set("content", std::string("[tool: run_command]\n> ...\n\n[output]\n(no output)\n"));
            msgs->add(t);
            root->set("messages", msgs);
            return lb_responses::Stringify(root);
        };

        bool threw = false;
        try { lb_responses::BuildChatRequest(buildWith(true)); }
        catch (const std::exception& e) {
            threw = std::string(e.what()).find("does not match") != std::string::npos;
        }
        check(threw, "shortened args + original sidecar is rejected (sidecar must be dropped)");

        std::string req; bool ok = true;
        try { req = lb_responses::BuildChatRequest(buildWith(false)); }
        catch (const std::exception& e) { ok = false; std::cout << "  threw: " << e.what() << "\n"; }
        check(ok && req.find("\"function_call\"") != std::string::npos &&
              req.find("\"call_9\"") != std::string::npos &&
              req.find("already-executed tool call elided") != std::string::npos &&
              req.find("\"reasoning\"") == std::string::npos,
              "shortened args + no sidecar replays via legacy path");
    }


    // 10. Spool callback: marker names the file; full text handed over intact.
    {
        const std::string script = BigScript(3000);
        std::string spooled;
        SpoolFn spool = [&](const std::string& full) { spooled = full; return std::string("Vars\\elided_00ff.txt"); };
        const std::string out = ShortenToolCallArguments("{\"command\":" + JStr(script) + "}",
                                                         kKeepChars, kMinElideArgBytes, spool);
        Object::Ptr o; try { o = ParseObj(out); } catch (...) {}
        const std::string cmd = o ? o->getValue<std::string>("command") : "";
        check(spooled == script, "spool receives the exact full value");
        check(cmd.find("full text: Vars\\elided_00ff.txt]") != std::string::npos, "marker names the spool file");
        check(cmd.find("Not runnable as shown") != std::string::npos, "marker says not runnable");
        check(ContainsArgElisionMarker(cmd), "guard matches decoded marker");
        check(ContainsArgElisionMarker(out), "guard matches raw JSON marker");
        check(SpoolPathFromMarker(cmd) == "Vars\\elided_00ff.txt", "spool path from decoded marker");
        check(SpoolPathFromMarker(out) == "Vars\\elided_00ff.txt", "spool path from raw JSON (backslash collapsed)");
        const std::string why = CopiedElisionRejection(out);
        check(why.find("No tool was executed") == 0 && why.find("Vars\\elided_00ff.txt") != std::string::npos,
              "rejection names the spool file");
        check(ShortenToolCallArguments(out, kKeepChars, kMinElideArgBytes, spool) == out, "spooled form idempotent");
        // A throwing spool degrades to marker-only.
        SpoolFn bad = [](const std::string&) -> std::string { throw std::runtime_error("disk"); };
        const std::string out2 = ShortenToolCallArguments("{\"command\":" + JStr(script) + "}",
                                                          kKeepChars, kMinElideArgBytes, bad);
        check(out2.find("full text") == std::string::npos && ContainsArgElisionMarker(out2), "throwing spool -> marker only");
    }

    // 11. Guard: exactly the transcript's copied shape, first-shipped marker.
    {
        const std::string copied =
            "$ErrorActionPreference='Continue'; $ms='C:\\Program Files\\Microsoft Visual Studio\\18\\Communit"
            "... [1306 more bytes of this old, already-executed tool call elided to fit context]";
        check(ContainsArgElisionMarker(copied), "guard matches first-shipped marker (live transcript)");
        check(CopiedElisionRejection(copied).find("Rewrite the complete arguments") != std::string::npos,
              "rejection without spool path asks for full rewrite");
    }

    // 12. No false positives on legitimate work about the marker.
    {
        check(!ContainsArgElisionMarker("{\"pattern\":\"already-executed tool call elided to fit context\"}"),
              "grep for the phrase is allowed");
        check(!ContainsArgElisionMarker("of this old, already-executed tool call elided to fit context"),
              "bare sentinel is allowed");
        check(!ContainsArgElisionMarker("[more bytes of this old, already-executed tool call elided to fit context]"),
              "no digits -> allowed");
        check(!ContainsArgElisionMarker("Get-ChildItem C:\\src -Recurse"), "ordinary command allowed");
    }

    std::cout << "\n" << passes << "/" << (passes + fails) << " passed\n";
    return fails ? 1 : 0;
}
