// Regression harness for python_resources.h + python_helpers.rc2 +
// assets/python/*.py (the embedded built-in Python scripts).
//
// Portable checks (any OS, no wx/Poco):
//   * kHelperNames has no duplicates and does not contain the kernel name
//   * python_helpers.rc2 lists exactly kHelperNames + kKernelName, each once,
//     as  <UPPERCASE_STEM> RCDATA "assets\\python\\<stem>.py"
//   * LlamaBoss.rc includes python_helpers.rc2
//   * LlamaBoss.vcxproj lists python_helpers.rc2 and every .py
//   * every .py exists, is non-empty, LF-only, BOM-free, valid UTF-8
//     (rc.exe embeds bytes as-is)
// Windows only, when the test exe is linked with python_helpers.rc2:
//   * lb_pyres::Load returns exactly the bytes of the matching .py file
//     (otherwise those checks are reported as SKIP)
//
// Build / run from the source root:
//   g++ -std=c++17 -O1 python_resources_tests.cpp -o python_resources_tests && ./python_resources_tests .
//   cl /std:c++17 /EHsc python_resources_tests.cpp && python_resources_tests.exe .
// Optional embedded check on Windows:
//   rc /fo pyres_test.res python_helpers.rc2 && cl /std:c++17 /EHsc python_resources_tests.cpp pyres_test.res

#include "python_resources.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int g_failures = 0;
static int g_checks   = 0;
#define CHECK(cond, label)                                                  \
    do {                                                                    \
        ++g_checks;                                                         \
        if (cond) { std::cout << "PASS  " << label << "\n"; }               \
        else      { std::cout << "FAIL  " << label << "\n"; ++g_failures; } \
    } while (0)

static bool ReadAll(const fs::path& p, std::string& out)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

static bool ValidUtf8(const std::string& s)
{
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t n = 0;
        if (c < 0x80) n = 0;
        else if ((c & 0xE0) == 0xC0 && c >= 0xC2) n = 1;
        else if ((c & 0xF0) == 0xE0) n = 2;
        else if ((c & 0xF8) == 0xF0 && c <= 0xF4) n = 3;
        else return false;
        if (i + n >= s.size() && n > 0) return false;
        for (size_t k = 1; k <= n; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        i += n + 1;
    }
    return true;
}

static std::vector<std::string> AllNames()
{
    std::vector<std::string> v(std::begin(lb_pyres::kHelperNames),
                               std::end(lb_pyres::kHelperNames));
    v.push_back(lb_pyres::kKernelName);
    return v;
}

int main(int argc, char** argv)
{
    const fs::path root = (argc > 1) ? fs::path(argv[1]) : fs::path(".");
    const std::vector<std::string> names = AllNames();

    // ── name table ─────────────────────────────────────────────────
    {
        std::set<std::string> uniq(std::begin(lb_pyres::kHelperNames),
                                   std::end(lb_pyres::kHelperNames));
        CHECK(uniq.size() == std::size(lb_pyres::kHelperNames), "helper names are unique");
        CHECK(std::size(lb_pyres::kHelperNames) == 14, "14 helper names");
        CHECK(!lb_pyres::IsHelperName(lb_pyres::kKernelName), "kernel is not a helper");
        CHECK(lb_pyres::IsHelperName("csv_inspect"), "IsHelperName(csv_inspect)");
        CHECK(!lb_pyres::IsHelperName("python_install_package"), "python_install_package is not a script helper");
        CHECK(!lb_pyres::IsHelperName("python_run_script"), "python_run_script is not a script helper");
        CHECK(!lb_pyres::IsHelperName("CSV_INSPECT"), "IsHelperName is exact-case");
        CHECK(lb_pyres::ResourceName("xlsx_create_workbook") == "XLSX_CREATE_WORKBOOK", "ResourceName upper-cases");
    }

    // ── python_helpers.rc2 ─────────────────────────────────────────
    std::string rc2;
    CHECK(ReadAll(root / "python_helpers.rc2", rc2), "python_helpers.rc2 readable");
    {
        std::map<std::string, std::string> entries;   // RESNAME -> path
        int lines = 0;
        bool dup = false;
        const std::regex re(R"rx(^\s*([A-Za-z_][A-Za-z0-9_]*)\s+RCDATA\s+"([^"]+)"\s*$)rx");
        std::istringstream in(rc2);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::smatch m;
            if (std::regex_match(line, m, re)) {
                ++lines;
                if (entries.count(m[1].str())) dup = true;
                entries[m[1].str()] = m[2].str();
            }
        }
        CHECK(!dup, "rc2 has no duplicate resource names");
        CHECK(lines == static_cast<int>(names.size()), "rc2 lists exactly 15 scripts");
        for (const std::string& n : names) {
            const std::string res = lb_pyres::ResourceName(n);
            auto it = entries.find(res);
            CHECK(it != entries.end(), "rc2 has " + res);
            if (it != entries.end())
                CHECK(it->second == "assets\\\\python\\\\" + n + ".py",
                      "rc2 " + res + " -> assets\\python\\" + n + ".py");
        }
    }

    // ── LlamaBoss.rc / vcxproj wiring ──────────────────────────────
    {
        std::string rc, proj;
        CHECK(ReadAll(root / "LlamaBoss.rc", rc), "LlamaBoss.rc readable");
        CHECK(rc.find("#include \"python_helpers.rc2\"") != std::string::npos,
              "LlamaBoss.rc includes python_helpers.rc2");
        CHECK(ReadAll(root / "LlamaBoss.vcxproj", proj), "LlamaBoss.vcxproj readable");
        CHECK(proj.find("Include=\"python_helpers.rc2\"") != std::string::npos,
              "vcxproj lists python_helpers.rc2");
        CHECK(proj.find("Include=\"python_resources.h\"") != std::string::npos,
              "vcxproj lists python_resources.h");
        for (const std::string& n : names)
            CHECK(proj.find("Include=\"assets\\python\\" + n + ".py\"") != std::string::npos,
                  "vcxproj lists " + n + ".py");
    }

    // ── the .py files themselves ───────────────────────────────────
    std::map<std::string, std::string> fileBytes;
    for (const std::string& n : names) {
        const fs::path p = root / "assets" / "python" / (n + ".py");
        std::string b;
        const bool ok = ReadAll(p, b);
        CHECK(ok && !b.empty(), n + ".py exists and is non-empty");
        if (!ok) continue;
        fileBytes[n] = b;
        CHECK(b.find('\r') == std::string::npos, n + ".py is LF-only");
        CHECK(!(b.size() >= 3 && b.compare(0, 3, "\xEF\xBB\xBF") == 0), n + ".py has no BOM");
        CHECK(ValidUtf8(b), n + ".py is valid UTF-8");
    }
    CHECK(fileBytes.count("lb_kernel") && !fileBytes["lb_kernel"].empty() &&
          fileBytes["lb_kernel"][0] != '\n',
          "lb_kernel.py does not start with a blank line");

    // ── embedded bytes (Windows, test exe linked with the .rc2) ────
#ifdef _WIN32
    {
        const char* d = nullptr; size_t sz = 0; std::string err;
        if (!lb_pyres::Load("python_health", d, sz, err)) {
            std::cout << "SKIP  embedded-bytes checks (test exe built without python_helpers.rc2)\n";
        } else {
            for (const std::string& n : names) {
                const char* data = nullptr; size_t size = 0; std::string e;
                const bool ok = lb_pyres::Load(n, data, size, e);
                CHECK(ok, "Load(" + n + ")");
                if (ok && fileBytes.count(n))
                    CHECK(std::string(data, size) == fileBytes[n],
                          "embedded " + n + " == assets\\python\\" + n + ".py");
            }
            const char* data = nullptr; size_t size = 0; std::string e;
            CHECK(!lb_pyres::Load("no_such_helper", data, size, e) && !e.empty() && !data,
                  "Load(missing) fails with an error and no data");
        }
    }
#else
    std::cout << "SKIP  embedded-bytes checks (not Windows)\n";
#endif

    std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " passed\n";
    return g_failures ? 1 : 0;
}
