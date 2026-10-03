#pragma once

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace lbtest {

class Failure : public std::runtime_error {
public:
    explicit Failure(const std::string& message) : std::runtime_error(message) {}
};

struct Environment {
    std::filesystem::path runRoot;
    bool keepTemp = false;

    std::filesystem::path CaseDir(const std::string& testName) const
    {
        std::string safe = testName;
        for (char& c : safe) {
            const unsigned char uc = static_cast<unsigned char>(c);
            if (!std::isalnum(uc) && c != '-' && c != '_') c = '_';
        }

        const std::filesystem::path dir = runRoot / safe;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            throw Failure("Could not create test directory: " + dir.string() +
                          " (" + ec.message() + ")");
        }
        return dir;
    }
};

using TestFunction = std::function<void(const Environment&)>;

struct TestCase {
    std::string name;
    TestFunction function;
};

inline std::vector<TestCase>& Registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

class Registrar {
public:
    Registrar(const char* name, TestFunction function)
    {
        Registry().push_back({ name, std::move(function) });
    }
};

template <typename T>
std::string ToText(const T& value)
{
    std::ostringstream out;
    out << value;
    return out.str();
}

inline std::string ToText(const std::string& value)
{
    return value;
}

inline std::string ToText(const char* value)
{
    return value ? std::string(value) : std::string("<null>");
}

inline void Expect(bool condition, const char* expression,
                   const char* file, int line)
{
    if (condition) return;
    std::ostringstream message;
    message << file << ':' << line << ": expectation failed: " << expression;
    throw Failure(message.str());
}

template <typename A, typename B>
void ExpectEqual(const A& actual, const B& expected,
                 const char* actualExpression,
                 const char* expectedExpression,
                 const char* file, int line)
{
    if (actual == expected) return;
    std::ostringstream message;
    message << file << ':' << line << ": expected " << actualExpression
            << " == " << expectedExpression << "\n"
            << "  actual:   " << ToText(actual) << "\n"
            << "  expected: " << ToText(expected);
    throw Failure(message.str());
}

inline void ExpectContains(const std::string& haystack,
                           const std::string& needle,
                           const char* haystackExpression,
                           const char* needleExpression,
                           const char* file, int line)
{
    if (haystack.find(needle) != std::string::npos) return;
    std::ostringstream message;
    message << file << ':' << line << ": expected " << haystackExpression
            << " to contain " << needleExpression << "\n"
            << "  missing: " << needle;
    throw Failure(message.str());
}

inline void ExpectNotContains(const std::string& haystack,
                              const std::string& needle,
                              const char* haystackExpression,
                              const char* needleExpression,
                              const char* file, int line)
{
    if (haystack.find(needle) == std::string::npos) return;
    std::ostringstream message;
    message << file << ':' << line << ": expected " << haystackExpression
            << " not to contain " << needleExpression << "\n"
            << "  unexpected: " << needle;
    throw Failure(message.str());
}

struct TestResult {
    std::string name;
    bool passed = false;
    std::string detail;
    long long elapsedMs = 0;
};

inline std::string JsonEscape(const std::string& value)
{
    std::ostringstream out;
    for (unsigned char c : value) {
        switch (c) {
        case '\\': out << "\\\\"; break;
        case '"':  out << "\\\""; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (c < 0x20) {
                out << "\\u" << std::hex << std::setw(4)
                    << std::setfill('0') << static_cast<int>(c)
                    << std::dec << std::setfill(' ');
            } else {
                out << static_cast<char>(c);
            }
        }
    }
    return out.str();
}

inline std::string LocalTimestamp()
{
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::ostringstream out;
    out << std::put_time(&local, "%Y-%m-%d %H:%M:%S");
    return out.str();
}

inline std::string RunId()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
#ifdef _WIN32
    const unsigned long pid = ::GetCurrentProcessId();
#else
    const unsigned long pid = 0;
#endif
    return std::to_string(millis) + "_" + std::to_string(pid);
}

inline void WriteReports(const std::filesystem::path& outputDir,
                         const std::vector<TestResult>& results,
                         const std::filesystem::path& tempRoot)
{
    std::error_code ec;
    std::filesystem::create_directories(outputDir, ec);
    if (ec) throw Failure("Could not create TestResults: " + ec.message());

    const std::size_t passed = static_cast<std::size_t>(std::count_if(
        results.begin(), results.end(), [](const TestResult& r) { return r.passed; }));
    const std::size_t failed = results.size() - passed;

    {
        std::ofstream report(outputDir / "RESULTS.md", std::ios::binary | std::ios::trunc);
        if (!report) throw Failure("Could not write RESULTS.md");
        report << "# LlamaBoss Native Test Results\n\n"
               << "Generated: " << LocalTimestamp() << "\n\n"
               << "| Test | Result | Time |\n"
               << "| --- | --- | ---: |\n";
        for (const TestResult& result : results) {
            report << "| `" << result.name << "` | "
                   << (result.passed ? "PASS" : "FAIL") << " | "
                   << result.elapsedMs << " ms |\n";
        }
        report << "\nPassed: " << passed << "  \n"
               << "Failed: " << failed << "  \n"
               << "Total: " << results.size() << "\n";
        if (failed > 0) {
            report << "\n## Failures\n";
            for (const TestResult& result : results) {
                if (result.passed) continue;
                report << "\n### `" << result.name << "`\n\n```text\n"
                       << result.detail << "\n```\n";
            }
            report << "\nTemporary evidence retained at:\n\n`"
                   << tempRoot.string() << "`\n";
        }
    }

    {
        std::ofstream report(outputDir / "results.json", std::ios::binary | std::ios::trunc);
        if (!report) throw Failure("Could not write results.json");
        report << "{\n  \"generated\": \"" << JsonEscape(LocalTimestamp()) << "\",\n"
               << "  \"passed\": " << passed << ",\n"
               << "  \"failed\": " << failed << ",\n"
               << "  \"total\": " << results.size() << ",\n"
               << "  \"tests\": [\n";
        for (std::size_t i = 0; i < results.size(); ++i) {
            const TestResult& result = results[i];
            report << "    {\"name\": \"" << JsonEscape(result.name)
                   << "\", \"passed\": " << (result.passed ? "true" : "false")
                   << ", \"elapsed_ms\": " << result.elapsedMs
                   << ", \"detail\": \"" << JsonEscape(result.detail) << "\"}"
                   << (i + 1 == results.size() ? "\n" : ",\n");
        }
        report << "  ]\n}\n";
    }
}

inline void PrintUsage()
{
    std::cout << "LlamaBossTests [--list] [--filter <text>] [--keep-temp] "
                 "[--cases <directory>] [--output <directory>] "
                 "[--temp-root <directory>]\n"
                 "  --output     Directory for RESULTS.md and results.json\n"
                 "  --temp-root  Parent directory for unique per-run evidence\n";
}

inline int RunAll(int argc, char** argv)
{
    bool listOnly = false;
    bool keepTemp = false;
    std::string filter;
    std::filesystem::path outputArgument;
    std::filesystem::path tempRootArgument;
    bool hasOutputArgument = false;
    bool hasTempRootArgument = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--list") {
            listOnly = true;
        } else if (arg == "--keep-temp") {
            keepTemp = true;
        } else if (arg == "--filter" && i + 1 < argc) {
            filter = argv[++i];
        } else if (arg == "--cases" && i + 1 < argc) {
            ++i; // JSON registration consumed the directory before RunAll.
        } else if (arg == "--output" && i + 1 < argc) {
            outputArgument = std::filesystem::u8path(argv[++i]);
            hasOutputArgument = true;
        } else if (arg == "--temp-root" && i + 1 < argc) {
            tempRootArgument = std::filesystem::u8path(argv[++i]);
            hasTempRootArgument = true;
        } else if (arg == "--help" || arg == "-h") {
            PrintUsage();
            return 0;
        } else {
            std::cerr << "Unknown or incomplete option: " << arg << "\n";
            PrintUsage();
            return 2;
        }
    }

    std::vector<TestCase> selected;
    for (const TestCase& test : Registry()) {
        if (filter.empty() || test.name.find(filter) != std::string::npos)
            selected.push_back(test);
    }
    std::sort(selected.begin(), selected.end(),
              [](const TestCase& a, const TestCase& b) { return a.name < b.name; });

    if (listOnly) {
        for (const TestCase& test : selected) std::cout << test.name << '\n';
        return 0;
    }
    if (selected.empty()) {
        std::cerr << "No tests matched the requested filter.\n";
        return 2;
    }

    std::error_code ec;
    const std::filesystem::path launchDirectory =
        std::filesystem::current_path(ec);
    if (ec) {
        std::cerr << "Could not determine launch directory: " << ec.message() << '\n';
        return 2;
    }

    auto resolveFromLaunch = [&](const std::filesystem::path& argument) {
        return (argument.is_relative() ? launchDirectory / argument : argument)
            .lexically_normal();
    };

    if (hasOutputArgument && outputArgument.empty()) {
        std::cerr << "--output requires a non-empty directory.\n";
        return 2;
    }
    if (hasTempRootArgument && tempRootArgument.empty()) {
        std::cerr << "--temp-root requires a non-empty directory.\n";
        return 2;
    }

    const std::filesystem::path outputDirectory = hasOutputArgument
        ? resolveFromLaunch(outputArgument)
        : launchDirectory / "TestResults";

    std::filesystem::path tempParent;
    if (hasTempRootArgument) {
        tempParent = resolveFromLaunch(tempRootArgument);
    } else {
        tempParent = std::filesystem::temp_directory_path(ec) / "LlamaBossTests";
        if (ec) {
            std::cerr << "Could not determine temporary directory: "
                      << ec.message() << '\n';
            return 2;
        }
    }

    // Always isolate a run beneath the selected parent.  This prevents two
    // chats or concurrent test processes from sharing generated fixtures.
    const std::filesystem::path tempRoot = tempParent / RunId();
    ec.clear();
    std::filesystem::create_directories(tempRoot, ec);
    if (ec) {
        std::cerr << "Could not create temporary test root: " << ec.message() << '\n';
        return 2;
    }

    Environment environment{ tempRoot, keepTemp };
    std::vector<TestResult> results;
    results.reserve(selected.size());

    std::cout << "LlamaBoss native regression tests\n"
              << "Temporary root: " << tempRoot.string() << "\n\n";

    for (const TestCase& test : selected) {
        TestResult result;
        result.name = test.name;
        const auto started = std::chrono::steady_clock::now();
        try {
            test.function(environment);
            result.passed = true;
        } catch (const std::exception& ex) {
            result.detail = ex.what();
        } catch (...) {
            result.detail = "Unknown non-standard exception";
        }
        result.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        std::cout << (result.passed ? "[PASS] " : "[FAIL] ")
                  << result.name << " (" << result.elapsedMs << " ms)\n";
        if (!result.passed) std::cout << "       " << result.detail << "\n";
        results.push_back(std::move(result));
    }

    const std::size_t failures = static_cast<std::size_t>(std::count_if(
        results.begin(), results.end(), [](const TestResult& r) { return !r.passed; }));

    try {
        WriteReports(outputDirectory, results, tempRoot);
    } catch (const std::exception& ex) {
        std::cerr << "Could not write test reports: " << ex.what() << '\n';
        return 2;
    }

    std::cout << "\n" << (results.size() - failures) << " passed, "
              << failures << " failed\n"
              << "Reports: "
              << outputDirectory.string() << "\n";

    if (failures == 0 && !keepTemp) {
        std::filesystem::remove_all(tempRoot, ec);
        if (ec) {
            std::cerr << "Warning: could not remove temporary test root: "
                      << ec.message() << '\n';
        }
    } else {
        std::cout << "Temporary evidence retained: " << tempRoot.string() << "\n";
    }

    return failures == 0 ? 0 : 1;
}

} // namespace lbtest

#define LB_TEST(name)                                                        \
    static void name(const lbtest::Environment&);                            \
    static lbtest::Registrar name##_registrar(#name, name);                  \
    static void name(const lbtest::Environment& env)

#define LB_EXPECT(expression)                                                \
    lbtest::Expect(static_cast<bool>(expression), #expression, __FILE__, __LINE__)

#define LB_EXPECT_EQ(actual, expected)                                       \
    lbtest::ExpectEqual((actual), (expected), #actual, #expected, __FILE__, __LINE__)

#define LB_EXPECT_CONTAINS(haystack, needle)                                 \
    lbtest::ExpectContains((haystack), (needle), #haystack, #needle, __FILE__, __LINE__)

#define LB_EXPECT_NOT_CONTAINS(haystack, needle)                             \
    lbtest::ExpectNotContains((haystack), (needle), #haystack, #needle, __FILE__, __LINE__)
