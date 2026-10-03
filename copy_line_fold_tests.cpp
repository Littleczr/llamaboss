// copy_line_fold_tests.cpp
//
// Regression harness for copy_line_fold.h (2026-10-01).  No dependencies:
//   g++ -std=c++17 -I . copy_line_fold_tests.cpp && ./a.out
//
// The first fixture is a verbatim MSBuild block from the r16c1 Luna
// transcript (test-project rebuild: compile echo, LTCG banner, the
// .vcxproj -> .exe output line, the vcpkg app-local DLL copies, the
// script's exit line), CRLF line endings as captured.
#include "copy_line_fold.h"
#include <iostream>
#include <string>

using lb_copyfold::FoldCopyProgressLines;
static int fails = 0, passes = 0;
static void check(bool ok, const std::string& w)
{ (ok ? passes : fails)++; std::cout << (ok ? "PASS " : "FAIL ") << w << "\n"; }
static size_t Count(const std::string& s, const std::string& n)
{ size_t c = 0, p = 0; while ((p = s.find(n, p)) != std::string::npos) { ++c; p += n.size(); } return c; }

static const char* kTranscriptBlock =
    "  macminicli_async.cpp\r\n"
    "  macminicli_executor_tests.cpp\r\n"
    "  macminicli_integration_tests.cpp\r\n"
    "  macminicli_async_tests.cpp\r\n"
    "  Generating code\r\n"
    "  Previous IPDB not found, fall back to full compilation.\r\n"
    "  All 4171 functions were compiled because no usable IPDB/IOBJ from previous compilation was found.\r\n"
    "  Finished generating code\r\n"
    "  LlamaBossTests.vcxproj -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-test-output\\LlamaBossTests.exe\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\PocoFoundation.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-test-output\\PocoFoundation.dll done\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\pcre2-8.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-test-output\\pcre2-8.dll done\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\zlib1.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-test-output\\zlib1.dll done\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\utf8proc.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-test-output\\utf8proc.dll done\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\PocoJSON.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-test-output\\PocoJSON.dll done\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\wxbase331u_vc_x64_custom.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-test-output\\wxbase331u_vc_x64_custom.dll done\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\pcre2-16.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-test-output\\pcre2-16.dll done\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\wxmsw331u_core_vc_x64_custom.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-test-output\\wxmsw331u_core_vc_x64_custom.dll done\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\jpeg62.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-test-output\\jpeg62.dll done\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\libpng16.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-test-output\\libpng16.dll done\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\tiff.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-test-output\\tiff.dll done\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\liblzma.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-test-output\\liblzma.dll done\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\libwebpdecoder.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-test-output\\libwebpdecoder.dll done\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\libwebp.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-test-output\\libwebp.dll done\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\libsharpyuv.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-test-output\\libsharpyuv.dll done\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\libwebpdemux.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-test-output\\libwebpdemux.dll done\r\n"
    "PASS configuration defaults disabled and unconfigured\r\n"
    "PASS disabled empty configuration validates\r\n"
    "PASS production profile path resolves per-user APPDATA\r\n"
    "PASS complete enabled profile accepts path-only identity and executable configuration\r\n"
    "PASS relative identity path rejected\r\n"
    "PASS relative executable path rejected\r\n"
    "PASS invalid port rejected\r\n"
    "PASS missing profile loads safe defaults\r\n"
    "PASS profile persists atomically\r\n"
    "PASS persisted profile round-trips target data and path settings\r\n"
    "PASS profile persistence contains no credential or private-key material\r\n"
    "PASS test writes forbidden credential field\r\n"
    "PASS credential-bearing profile rejected fail-closed\r\n"
    "PASS oversized schema_version fails safely with useful error/defaults\r\n"
    "PASS schema_version beyond int64 fails safely with useful error/defaults\r\n"
    "PASS port beyond int64 fails safely with useful error/defaults\r\n"
    "PASS port beyond uint64 fails safely with useful error/defaults\r\n"
    "PASS duplicate configuration keys rejected\r\n"
    "PASS configuration trailing garbage rejected\r\n"
    "PASS multiple configuration documents rejected\r\n"
    "PASS configuration host validation rejects whitespace like CLI argument builder\r\n"
    "PASS configuration user validation rejects whitespace like CLI argument builder\r\n"
    "PASS native structured and XML JSON exec arguments normalize identically\r\n"
    "PASS native structured and XML JSON transfer arguments normalize identically\r\n"
    "PASS operation derives from download tool name\r\n"
    "PASS operation conflicts, configuration, credentials, and unsupported fields rejected consistently\r\n"
    "PASS unknown tool name rejected\r\n"
    "PASS wrong argument type rejected\r\n"
    "PASS timeout lower bound enforced\r\n"
    "PASS missing transfer destination rejected\r\n"
    "PASS timeout beyond int64 rejected safely with useful error\r\n"
    "PASS timeout beyond signed 64-bit rejected safely\r\n"
    "PASS duplicate tool argument keys rejected\r\n"
    "PASS tool argument trailing garbage rejected\r\n"
    "PASS multiple tool argument documents rejected\r\n"
    "PASS upload uses a stable synthetic snapshot, not the mutable caller path\r\n"
    "PASS identity metadata failure fails closed\r\n"
    "PASS configured identity rejected through case-only alias\r\n"
    "PASS configured identity rejected by file identity\r\n"
    "PASS directory rejected as non-regular upload source\r\n"
    "FAIL source replacement is denied while snapshot preparation is held and CLI bytes stay stable\r\n"
    "PASS download preflight accepts cwd-granted destination\r\n"
    "PASS download preflight rejects outside folder grants\r\n"
    "\r\n"
    "[... 100 lines omitted; open erroractionpreferencecontinue_repoc_users_cesar_4.txt for full captured output ...]\r\n"
    "\r\n"
    "  Finished generating code\r\n"
    "  LlamaBoss.vcxproj -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-app-output\\LlamaBoss.exe\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\PocoCrypto.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-app-output\\PocoCrypto.dll done\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\PocoFoundation.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-app-output\\PocoFoundation.dll done\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\pcre2-8.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-app-output\\pcre2-8.dll done\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\zlib1.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-app-output\\zlib1.dll done\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\utf8proc.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-app-output\\utf8proc.dll done\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\libcrypto-3-x64.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-app-output\\libcrypto-3-x64.dll done\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\PocoJSON.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-app-output\\PocoJSON.dll done\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\PocoNet.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-app-output\\PocoNet.dll done\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\PocoNetSSL.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-app-output\\PocoNetSSL.dll done\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\PocoUtil.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-app-output\\PocoUtil.dll done\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\PocoXML.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-app-output\\PocoXML.dll done\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\libexpat.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-app-output\\libexpat.dll done\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\libssl-3-x64.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-app-output\\libssl-3-x64.dll done\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\wxbase331u_vc_x64_custom.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-app-output\\wxbase331u_vc_x64_custom.dll done\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\pcre2-16.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-app-output\\pcre2-16.dll done\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\wxmsw331u_core_vc_x64_custom.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-app-output\\wxmsw331u_core_vc_x64_custom.dll done\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\jpeg62.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-app-output\\jpeg62.dll done\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\libpng16.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-app-output\\libpng16.dll done\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\tiff.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-app-output\\tiff.dll done\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\liblzma.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-app-output\\liblzma.dll done\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\libwebpdecoder.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-app-output\\libwebpdecoder.dll done\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\libwebp.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-app-output\\libwebp.dll done\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\libsharpyuv.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-app-output\\libsharpyuv.dll done\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\libwebpdemux.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-app-output\\libwebpdemux.dll done\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\wxmsw331u_richtext_vc_x64_custom.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-app-output\\wxmsw331u_richtext_vc_x64_custom.dll done\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\wxmsw331u_html_vc_x64_custom.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-app-output\\wxmsw331u_html_vc_x64_custom.dll done\r\n"
    "  C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\vcpkg_installed\\x64-windows\\x64-windows\\bin\\wxbase331u_xml_vc_x64_custom.dll -> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-app-output\\wxbase331u_xml_vc_x64_custom.dll done\r\n"
    "TEST_BUILD_EXIT=0 TEST_EXIT=1 APP_BUILD_EXIT=0\r\n";

int main()
{
    {
        std::string s = kTranscriptBlock;
        const size_t before = s.size();
        const size_t removed = FoldCopyProgressLines(s);
        check(removed == 41 && Count(s, "file-copy lines folded") == 2, "43 DLL copy lines in 2 runs (test + app project) -> 2 summaries (" + std::to_string(removed) + " removed)");
        check(Count(s, " done\r\n") == 0, "no copy lines remain");
        check(s.find("LlamaBossTests.vcxproj -> ") != std::string::npos, "build output (.exe) line kept");
        check(s.find("TEST_BUILD_EXIT=") != std::string::npos, "script exit line kept");
        check(s.find("Generating code") != std::string::npos, "other build output kept");
        check(s.find("file-copy lines folded: PocoFoundation.dll, pcre2-8.dll") != std::string::npos, "summary lists first names");
        check(s.find("-> C:\\Users\\Cesar\\source\\repos\\LlamaBoss\\r16c1-test-output\\]") != std::string::npos, "summary names the destination folder");
        check(Count(s, "\r\n") == Count(s, "\n"), "CRLF endings preserved");
        std::cout << "  bytes " << before << " -> " << s.size() << "\n";
        check(s.size() * 3 < before, "block shrinks by more than 2/3");
        std::string again = s;
        check(FoldCopyProgressLines(again) == 0 && again == s, "idempotent");
    }
    {   // A copy failure splits the run and stays verbatim.
        std::string s =
            "  a.dll -> C:\\o\\a.dll done\n  b.dll -> C:\\o\\b.dll done\n  c.dll -> C:\\o\\c.dll done\n"
            "C:\\x\\Microsoft.Common.targets(5034,5): error MSB3021: Unable to copy file \"d.dll\" to \"C:\\o\\d.dll\". done\n"
            "  e.dll -> C:\\o\\e.dll done\n  f.dll -> C:\\o\\f.dll done\n";
        FoldCopyProgressLines(s);
        check(s.find("error MSB3021") != std::string::npos, "MSB3021 failure kept");
        check(s.find("[3 file-copy lines folded: a.dll, b.dll, c.dll -> C:\\o\\]") != std::string::npos, "run before failure folded");
        check(s.find("  e.dll -> C:\\o\\e.dll done\n  f.dll -> C:\\o\\f.dll done\n") != std::string::npos, "2-line run after failure kept verbatim");
    }
    {   // Warnings never fold; no-op text untouched; mixed destinations.
        std::string w = "a -> b done\nwarning: x -> y done\nc -> d done\n";
        const std::string w0 = w;
        check(FoldCopyProgressLines(w) == 0 && w == w0, "warning line breaks run; short runs untouched");
        std::string n = "Get-ChildItem output\nno arrows here\n";
        const std::string n0 = n;
        check(FoldCopyProgressLines(n) == 0 && n == n0, "unrelated output untouched");
        std::string m = "x.dll -> C:\\a\\x.dll done\ny.dll -> C:\\b\\y.dll done\nz.dll -> C:\\a\\z.dll done";
        FoldCopyProgressLines(m);
        check(m == "[3 file-copy lines folded: x.dll, y.dll, z.dll -> various destinations]", "mixed destinations; no trailing newline invented");
        std::string many;
        for (int k = 0; k < 10; ++k) many += "f" + std::to_string(k) + ".dll -> C:\\o\\f" + std::to_string(k) + ".dll done\n";
        FoldCopyProgressLines(many);
        check(many.find("... (+4 more) -> C:\\o\\]\n") != std::string::npos, "name list capped with remainder count");
    }
    std::cout << "\n" << passes << "/" << (passes + fails) << " passed\n";
    return fails ? 1 : 0;
}
