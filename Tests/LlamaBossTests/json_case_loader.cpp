#include "json_case_loader.h"

#include "test_framework.h"
#include "path_safety.h"
#include "tool_grep_args.h"
#include "tool_path_safety.h"
#include "tool_read.h"
#include "var_store.h"

#include <Poco/Dynamic/Var.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace lbtest {
namespace {

struct TextOccurrenceExpectation {
    std::string text;
    size_t count = 0;
};

struct JsonCaseSpec {
    std::string name;
    std::string action;
    std::string caseSource;
    std::string setupFile;
    size_t numberedLines = 0;
    size_t numberedLineWidth = 0;
    bool trailingNewline = true;
    bool crlf = false;
    bool hasRawSetup = false;
    std::string rawSetupBytes;
    std::string inputPath;
    std::vector<ReadLineRange> ranges;
    int ctxTokens = 131072;
    bool expectSuccess = true;
    std::string bodyMode;
    std::string errorContains;
    bool hasExactBody = false;
    std::string exactBody;
    std::vector<std::string> bodyContains;
    std::vector<std::string> bodyNotContains;
    std::vector<std::string> chipsContain;
    std::vector<std::string> chipsNotContain;
    bool hasHistoryBudget = false;
    size_t historyBudget = 0;
    std::string grepArgs;
    size_t maxContextLines = 50;
    std::string expectedPattern;
    std::string expectedPath;
    size_t expectedContextLines = 0;
    std::string expectedError;

    std::string operation;
    std::string rawValue;
    std::string fallbackValue = "file";
    std::string absPath;
    std::string cwd;
    std::string projectRoot;
    std::string skillsRoot;
    std::vector<std::string> additionalRoots;
    bool expectedBool = false;
    std::string expectedValue;

    std::string toolTag;
    std::string commandEcho;
    std::string demoteBody;
    size_t thresholdBytes = 12 * 1024;
    bool expectedDemoted = false;
    bool expectedAbsPathEmpty = false;
    bool hasExpectedAbsPathEmpty = false;
    std::string relPathEquals;
    std::string relPathContains;
    std::vector<std::string> cardContains;
    size_t expectedVarsFiles = 0;
    bool hasExpectedVarsFiles = false;
    bool expectSpooledBodyEqualsInput = false;

    std::string repositoryFile;
    std::vector<TextOccurrenceExpectation> textOccurrences;
};

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) throw Failure("Could not open text file: " + path.string());
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

std::string RequiredString(const Poco::JSON::Object::Ptr& object,
                           const std::string& key,
                           const std::string& location)
{
    if (!object || !object->has(key))
        throw Failure(location + " requires string field '" + key + "'.");
    try { return object->getValue<std::string>(key); }
    catch (...) { throw Failure(location + " field '" + key + "' must be a string."); }
}

long long RequiredInteger(const Poco::JSON::Object::Ptr& object,
                          const std::string& key,
                          const std::string& location)
{
    if (!object || !object->has(key))
        throw Failure(location + " requires integer field '" + key + "'.");
    try { return object->getValue<long long>(key); }
    catch (...) { throw Failure(location + " field '" + key + "' must be an integer."); }
}

size_t PositiveSize(long long value,
                    const std::string& field,
                    const std::string& location)
{
    if (value <= 0)
        throw Failure(location + " field '" + field + "' must be positive.");
    return static_cast<size_t>(value);
}

bool OptionalBool(const Poco::JSON::Object::Ptr& object,
                  const std::string& key,
                  bool fallback,
                  const std::string& location)
{
    if (!object || !object->has(key)) return fallback;
    try { return object->getValue<bool>(key); }
    catch (...) { throw Failure(location + " field '" + key + "' must be true or false."); }
}

bool RequiredBool(const Poco::JSON::Object::Ptr& object,
                  const std::string& key,
                  const std::string& location)
{
    if (!object || !object->has(key))
        throw Failure(location + " requires boolean field '" + key + "'.");
    try { return object->getValue<bool>(key); }
    catch (...) { throw Failure(location + " field '" + key + "' must be true or false."); }
}

std::string OptionalString(const Poco::JSON::Object::Ptr& object,
                           const std::string& key,
                           const std::string& fallback,
                           const std::string& location)
{
    if (!object || !object->has(key)) return fallback;
    try { return object->getValue<std::string>(key); }
    catch (...) { throw Failure(location + " field '" + key + "' must be a string."); }
}

long long OptionalInteger(const Poco::JSON::Object::Ptr& object,
                          const std::string& key,
                          long long fallback,
                          const std::string& location)
{
    if (!object || !object->has(key)) return fallback;
    try { return object->getValue<long long>(key); }
    catch (...) { throw Failure(location + " field '" + key + "' must be an integer."); }
}

std::vector<std::string> OptionalStringArray(
    const Poco::JSON::Object::Ptr& object,
    const std::string& key,
    const std::string& location)
{
    std::vector<std::string> values;
    if (!object || !object->has(key)) return values;
    Poco::JSON::Array::Ptr array;
    try { array = object->getArray(key); } catch (...) { array = nullptr; }
    if (!array) throw Failure(location + " field '" + key + "' must be an array.");
    for (size_t i = 0; i < array->size(); ++i) {
        try { values.push_back(array->getElement<std::string>(i)); }
        catch (...) {
            throw Failure(location + " field '" + key + "' must contain only strings.");
        }
    }
    return values;
}

int HexNibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string DecodeHexBytes(const std::string& encoded,
                           const std::string& location)
{
    std::string compact;
    compact.reserve(encoded.size());
    for (char c : encoded) {
        if (!std::isspace(static_cast<unsigned char>(c))) compact.push_back(c);
    }
    if ((compact.size() % 2) != 0)
        throw Failure(location + " hex_bytes must contain complete byte pairs.");

    std::string bytes;
    bytes.reserve(compact.size() / 2);
    for (size_t i = 0; i < compact.size(); i += 2) {
        const int hi = HexNibble(compact[i]);
        const int lo = HexNibble(compact[i + 1]);
        if (hi < 0 || lo < 0)
            throw Failure(location + " hex_bytes contains a non-hex character.");
        bytes.push_back(static_cast<char>((hi << 4) | lo));
    }
    return bytes;
}

Poco::JSON::Object::Ptr RequiredObject(const Poco::JSON::Object::Ptr& object,
                                       const std::string& key,
                                       const std::string& location)
{
    if (!object || !object->has(key))
        throw Failure(location + " requires object field '" + key + "'.");
    try {
        Poco::JSON::Object::Ptr child = object->getObject(key);
        if (!child) throw Failure(location + " field '" + key + "' must be an object.");
        return child;
    } catch (const Failure&) {
        throw;
    } catch (...) {
        throw Failure(location + " field '" + key + "' must be an object.");
    }
}

std::string NumberedLine(size_t line)
{
    std::ostringstream out;
    out << "line " << std::setw(4) << std::setfill('0') << line;
    return out.str();
}

std::string NumberedLine(const JsonCaseSpec& spec, size_t line)
{
    std::string value = NumberedLine(line);
    if (spec.numberedLineWidth > value.size())
        value.append(spec.numberedLineWidth - value.size(), 'x');
    return value;
}

void WriteSetupFile(const std::filesystem::path& path, const JsonCaseSpec& spec)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw Failure("Could not create generated case file: " + path.string());
    if (spec.hasRawSetup) {
        output.write(spec.rawSetupBytes.data(),
                     static_cast<std::streamsize>(spec.rawSetupBytes.size()));
    } else {
        const char* newline = spec.crlf ? "\r\n" : "\n";
        for (size_t line = 1; line <= spec.numberedLines; ++line) {
            output << NumberedLine(spec, line);
            if (line < spec.numberedLines || spec.trailingNewline) output << newline;
        }
    }
    if (!output) throw Failure("Incomplete generated case file: " + path.string());
}

std::string ExpectedNumberedLines(const JsonCaseSpec& spec,
                                  const ReadLineRange& range)
{
    std::ostringstream out;
    for (size_t line = range.startLine; line <= range.endLine; ++line)
        out << NumberedLine(spec, line) << (spec.crlf ? "\r\n" : "\n");
    return out.str();
}

std::string ExpectedNumberedRanges(const JsonCaseSpec& spec)
{
    std::ostringstream out;
    for (size_t i = 0; i < spec.ranges.size(); ++i) {
        const ReadLineRange& range = spec.ranges[i];
        const size_t last = std::min(range.endLine, spec.numberedLines);
        if (i > 0) out << '\n';
        out << "[range " << (i + 1) << ": lines "
            << range.startLine << '-' << last << " of "
            << spec.numberedLines << "]\n";
        for (size_t line = range.startLine; line <= last; ++line)
            out << NumberedLine(spec, line) << (spec.crlf ? "\r\n" : "\n");
    }
    return out.str();
}

[[noreturn]] void Mismatch(const std::string& field,
                           const std::string& actual,
                           const std::string& expected)
{
    throw Failure("JSON expectation mismatch for " + field +
                  "\n  actual:   " + actual +
                  "\n  expected: " + expected);
}

void RequireEqual(const std::string& field,
                  const std::string& actual,
                  const std::string& expected)
{
    if (actual != expected) Mismatch(field, actual, expected);
}

void RequireEqual(const std::string& field, size_t actual, size_t expected)
{
    if (actual != expected)
        Mismatch(field, std::to_string(actual), std::to_string(expected));
}

void RequireContainsAll(const std::string& field,
                        const std::string& value,
                        const std::vector<std::string>& needles)
{
    for (const std::string& needle : needles) {
        if (value.find(needle) == std::string::npos)
            throw Failure(field + " does not contain expected text: " + needle);
    }
}

void RequireContainsNone(const std::string& field,
                         const std::string& value,
                         const std::vector<std::string>& needles)
{
    for (const std::string& needle : needles) {
        if (value.find(needle) != std::string::npos)
            throw Failure(field + " unexpectedly contains text: " + needle);
    }
}

size_t CountOccurrences(const std::string& value, const std::string& needle)
{
    if (needle.empty()) throw Failure("Occurrence text cannot be empty.");
    size_t count = 0;
    size_t offset = 0;
    while ((offset = value.find(needle, offset)) != std::string::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

std::filesystem::path FindRepositoryFile(const JsonCaseSpec& spec)
{
    const std::filesystem::path relative =
        std::filesystem::u8path(spec.repositoryFile).lexically_normal();
    if (relative.empty() || relative.is_absolute() || relative.has_root_name())
        throw Failure("source_contract path must be repository-relative.");
    for (const std::filesystem::path& part : relative) {
        if (part == "..")
            throw Failure("source_contract path cannot contain '..'.");
    }

    std::vector<std::filesystem::path> starts;
    starts.push_back(std::filesystem::u8path(spec.caseSource).parent_path());
    starts.push_back(std::filesystem::current_path());
    for (std::filesystem::path start : starts) {
        std::error_code ec;
        start = std::filesystem::absolute(start, ec);
        if (ec) continue;
        for (;;) {
            const std::filesystem::path candidate = start / relative;
            if (std::filesystem::is_regular_file(candidate, ec) && !ec)
                return candidate;
            ec.clear();
            const std::filesystem::path parent = start.parent_path();
            if (parent == start || parent.empty()) break;
            start = parent;
        }
    }

    throw Failure("Could not locate repository file: " + spec.repositoryFile);
}

std::string JoinChips(const std::vector<std::string>& chips)
{
    std::ostringstream out;
    for (const std::string& chip : chips) out << chip << '\n';
    return out.str();
}

size_t CountRegularFiles(const std::filesystem::path& directory)
{
    std::error_code ec;
    if (!std::filesystem::exists(directory, ec)) return 0;
    size_t count = 0;
    for (std::filesystem::directory_iterator it(directory, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)) ++count;
    }
    if (ec) throw Failure("Could not enumerate " + directory.string() +
                          ": " + ec.message());
    return count;
}

void WriteBytes(const std::filesystem::path& path, const std::string& bytes)
{
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) throw Failure("Could not create setup directory: " + ec.message());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw Failure("Could not write setup file: " + path.string());
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) throw Failure("Incomplete setup-file write: " + path.string());
}

void RunReadRangesCase(const JsonCaseSpec& spec, const Environment& env)
{
    const std::filesystem::path workspace = env.CaseDir(spec.name);
    WriteSetupFile(workspace / spec.setupFile, spec);

    ToolContext context;
    context.cwd = workspace.u8string();
    context.ctxTokens = spec.ctxTokens;
    const ReadResult result = ReadFileRanges(spec.inputPath, context, spec.ranges);

    if (spec.expectSuccess && !result.errorBody.empty())
        Mismatch("errorBody", result.errorBody, "<empty>");
    if (!spec.expectSuccess && result.errorBody.empty())
        Mismatch("errorBody", "<empty>", "<non-empty>");

    if (spec.bodyMode == "numbered_lines") {
        if (spec.ranges.size() != 1)
            throw Failure("body_mode numbered_lines requires exactly one range.");
        RequireEqual("body", result.body,
                     ExpectedNumberedLines(spec, spec.ranges.front()));
    } else if (spec.bodyMode == "numbered_ranges") {
        RequireEqual("body", result.body, ExpectedNumberedRanges(spec));
    } else if (spec.bodyMode == "empty") {
        RequireEqual("body", result.body, std::string());
    } else if (spec.bodyMode == "exact") {
        RequireEqual("body", result.body, spec.exactBody);
    } else if (spec.bodyMode == "unchecked") {
        // Assertions below inspect only the properties relevant to the case.
    } else {
        throw Failure("Unsupported body_mode: " + spec.bodyMode);
    }

    if (!spec.errorContains.empty() &&
        result.errorBody.find(spec.errorContains) == std::string::npos) {
        throw Failure("errorBody does not contain expected text: " +
                      spec.errorContains + "\n  actual: " + result.errorBody);
    }
    if (spec.hasHistoryBudget)
        RequireEqual("historyInlineBudgetBytes",
                     result.historyInlineBudgetBytes, spec.historyBudget);
    RequireContainsAll("body", result.body, spec.bodyContains);
    RequireContainsNone("body", result.body, spec.bodyNotContains);
    const std::string chips = JoinChips(result.chips);
    RequireContainsAll("chips", chips, spec.chipsContain);
    RequireContainsNone("chips", chips, spec.chipsNotContain);
}

void RunParseGrepCase(const JsonCaseSpec& spec, const Environment&)
{
    const tool_grep_args::ParsedGrepArgs parsed =
        tool_grep_args::Parse(spec.grepArgs, spec.maxContextLines);
    RequireEqual("pattern", parsed.pattern, spec.expectedPattern);
    RequireEqual("path", parsed.path, spec.expectedPath);
    RequireEqual("contextLines", parsed.contextLines, spec.expectedContextLines);
    RequireEqual("error", parsed.error, spec.expectedError);
}

void RunPathPolicyCase(const JsonCaseSpec& spec, const Environment&)
{
    bool actual = false;
    if (spec.operation == "known_project_relative") {
        actual = tool_path_safety::IsKnownProjectRelativePath(spec.rawValue);
    } else if (spec.operation == "under_cwd") {
        actual = tool_path_safety::IsUnderCwd(spec.absPath, spec.cwd);
    } else if (spec.operation == "same_path") {
        actual = tool_path_safety::SamePath(spec.absPath, spec.cwd);
    } else if (spec.operation == "under_allowed_write_root") {
        actual = tool_path_safety::IsUnderAllowedWriteRoot(
            spec.absPath, spec.cwd, spec.projectRoot, spec.skillsRoot,
            spec.additionalRoots);
    } else {
        throw Failure("Unsupported path_policy operation: " + spec.operation);
    }
    if (actual != spec.expectedBool)
        Mismatch("result", actual ? "true" : "false",
                 spec.expectedBool ? "true" : "false");
}

void RunSanitizeFilenameCase(const JsonCaseSpec& spec, const Environment&)
{
    RequireEqual("result",
                 path_safety::SanitizeFilename(spec.rawValue, spec.fallbackValue),
                 spec.expectedValue);
}

void RunVarDemoteCase(const JsonCaseSpec& spec, const Environment& env)
{
    const std::filesystem::path workspace = env.CaseDir(spec.name);
    if (!spec.setupFile.empty()) WriteBytes(workspace / spec.setupFile, spec.rawSetupBytes);

    varstore::DemotionConfig config;
    config.thresholdBytes = spec.thresholdBytes;
    const varstore::DemoteOutcome outcome = varstore::MaybeDemoteToolBody(
        spec.toolTag, spec.commandEcho, spec.demoteBody,
        workspace.u8string(), config);

    if (outcome.demoted != spec.expectedDemoted)
        Mismatch("demoted", outcome.demoted ? "true" : "false",
                 spec.expectedDemoted ? "true" : "false");
    if (!spec.relPathEquals.empty())
        RequireEqual("relPath", outcome.relPath, spec.relPathEquals);
    if (!spec.relPathContains.empty() &&
        outcome.relPath.find(spec.relPathContains) == std::string::npos) {
        throw Failure("relPath does not contain expected text: " + spec.relPathContains +
                      "\n  actual: " + outcome.relPath);
    }
    if (spec.hasExpectedAbsPathEmpty &&
        outcome.absPath.empty() != spec.expectedAbsPathEmpty) {
        Mismatch("absPath.empty", outcome.absPath.empty() ? "true" : "false",
                 spec.expectedAbsPathEmpty ? "true" : "false");
    }
    RequireContainsAll("cardBody", outcome.cardBody, spec.cardContains);
    if (spec.hasExpectedVarsFiles) {
        RequireEqual("Vars file count", CountRegularFiles(workspace / "Vars"),
                     spec.expectedVarsFiles);
    }
    if (spec.expectSpooledBodyEqualsInput) {
        if (outcome.absPath.empty())
            throw Failure("Expected a spool path for byte-exact comparison.");
        RequireEqual("spooled body", ReadText(std::filesystem::u8path(outcome.absPath)),
                     spec.demoteBody);
    }
}

void RunSourceContractCase(const JsonCaseSpec& spec, const Environment&)
{
    const std::filesystem::path path = FindRepositoryFile(spec);
    const std::string source = ReadText(path);
    RequireContainsAll(spec.repositoryFile, source, spec.bodyContains);
    RequireContainsNone(spec.repositoryFile, source, spec.bodyNotContains);
    for (const TextOccurrenceExpectation& expectation : spec.textOccurrences) {
        RequireEqual("occurrences of '" + expectation.text + "' in " +
                         spec.repositoryFile,
                     CountOccurrences(source, expectation.text),
                     expectation.count);
    }
}

JsonCaseSpec ParseCase(const Poco::JSON::Object::Ptr& object,
                       const std::string& source,
                       size_t index)
{
    const std::string location = source + " case " + std::to_string(index + 1);
    JsonCaseSpec spec;
    spec.name = RequiredString(object, "name", location);
    spec.action = RequiredString(object, "action", location);
    spec.caseSource = source;
    if (spec.name.empty()) throw Failure(location + " has an empty name.");

    const Poco::JSON::Object::Ptr input = RequiredObject(object, "input", location);
    const Poco::JSON::Object::Ptr expect = RequiredObject(object, "expect", location);

    if (spec.action == "read_ranges") {
        const Poco::JSON::Object::Ptr setup = RequiredObject(object, "setup", location);
        spec.setupFile = RequiredString(setup, "file", location + " setup");
        const std::filesystem::path setupPath = std::filesystem::u8path(spec.setupFile);
        if (setupPath.is_absolute() || setupPath.has_parent_path() ||
            spec.setupFile == "." || spec.setupFile == "..") {
            throw Failure(location + " setup file must be one filename component.");
        }
        const bool hasNumbered = setup->has("numbered_lines");
        const bool hasContent = setup->has("content");
        const bool hasHex = setup->has("hex_bytes");
        if (static_cast<int>(hasNumbered) + static_cast<int>(hasContent) +
            static_cast<int>(hasHex) != 1) {
            throw Failure(location + " setup requires exactly one of "
                          "numbered_lines, content, or hex_bytes.");
        }
        if (hasNumbered) {
            spec.numberedLines = PositiveSize(
                RequiredInteger(setup, "numbered_lines", location + " setup"),
                "numbered_lines", location + " setup");
            const long long width = OptionalInteger(
                setup, "numbered_line_width", 0, location + " setup");
            if (width < 0 || width > 1024 * 1024)
                throw Failure(location + " numbered_line_width is outside the supported range.");
            spec.numberedLineWidth = static_cast<size_t>(width);
            spec.trailingNewline = OptionalBool(
                setup, "trailing_newline", true, location + " setup");
            const std::string newline = OptionalString(
                setup, "newline", "lf", location + " setup");
            if (newline != "lf" && newline != "crlf")
                throw Failure(location + " setup newline must be 'lf' or 'crlf'.");
            spec.crlf = (newline == "crlf");
        } else {
            spec.hasRawSetup = true;
            if (hasContent) {
                spec.rawSetupBytes = RequiredString(setup, "content", location + " setup");
            } else {
                spec.rawSetupBytes = DecodeHexBytes(
                    RequiredString(setup, "hex_bytes", location + " setup"),
                    location + " setup");
            }
        }

        spec.inputPath = RequiredString(input, "path", location + " input");
        if (input->has("ctx_tokens")) {
            const long long value = RequiredInteger(input, "ctx_tokens", location + " input");
            if (value <= 0 || value > 10000000)
                throw Failure(location + " input ctx_tokens is outside the supported range.");
            spec.ctxTokens = static_cast<int>(value);
        }

        Poco::JSON::Array::Ptr ranges;
        try { ranges = input->getArray("ranges"); } catch (...) { ranges = nullptr; }
        if (!ranges || ranges->size() == 0)
            throw Failure(location + " input requires a non-empty ranges array.");
        for (size_t i = 0; i < ranges->size(); ++i) {
            Poco::JSON::Object::Ptr range;
            try { range = ranges->getObject(i); } catch (...) { range = nullptr; }
            if (!range) throw Failure(location + " range entries must be objects.");
            const long long startValue =
                RequiredInteger(range, "start", location + " range");
            const long long endValue =
                RequiredInteger(range, "end", location + " range");
            if (startValue < 0 || endValue < 0)
                throw Failure(location + " range line numbers cannot be negative.");
            const size_t start = static_cast<size_t>(startValue);
            const size_t end = static_cast<size_t>(endValue);
            spec.ranges.push_back({ start, end });
        }

        spec.expectSuccess = OptionalBool(expect, "success", true, location + " expect");
        spec.bodyMode = RequiredString(expect, "body_mode", location + " expect");
        if (expect->has("body")) {
            spec.hasExactBody = true;
            spec.exactBody = RequiredString(expect, "body", location + " expect");
        }
        if (spec.bodyMode == "exact" && !spec.hasExactBody)
            throw Failure(location + " exact body_mode requires expect.body.");
        spec.errorContains = OptionalString(
            expect, "error_contains", std::string(), location + " expect");
        spec.bodyContains = OptionalStringArray(
            expect, "body_contains", location + " expect");
        spec.bodyNotContains = OptionalStringArray(
            expect, "body_not_contains", location + " expect");
        spec.chipsContain = OptionalStringArray(
            expect, "chips_contain", location + " expect");
        spec.chipsNotContain = OptionalStringArray(
            expect, "chips_not_contain", location + " expect");
        if (expect->has("history_inline_budget_bytes")) {
            spec.hasHistoryBudget = true;
            const long long budget = RequiredInteger(
                expect, "history_inline_budget_bytes", location + " expect");
            if (budget < 0)
                throw Failure(location + " history_inline_budget_bytes cannot be negative.");
            spec.historyBudget = static_cast<size_t>(budget);
        }
    } else if (spec.action == "parse_grep") {
        spec.grepArgs = RequiredString(input, "args", location + " input");
        if (input->has("max_context_lines")) {
            spec.maxContextLines = PositiveSize(
                RequiredInteger(input, "max_context_lines", location + " input"),
                "max_context_lines", location + " input");
        }
        spec.expectedPattern = RequiredString(expect, "pattern", location + " expect");
        spec.expectedPath = RequiredString(expect, "path", location + " expect");
        const long long expectedContext =
            RequiredInteger(expect, "context_lines", location + " expect");
        if (expectedContext < 0)
            throw Failure(location + " expected context_lines cannot be negative.");
        spec.expectedContextLines = static_cast<size_t>(expectedContext);
        spec.expectedError = RequiredString(expect, "error", location + " expect");
    } else if (spec.action == "source_contract") {
        spec.repositoryFile = RequiredString(input, "path", location + " input");
        spec.bodyContains = OptionalStringArray(
            expect, "contains", location + " expect");
        spec.bodyNotContains = OptionalStringArray(
            expect, "not_contains", location + " expect");

        if (expect->has("occurrences")) {
            Poco::JSON::Array::Ptr occurrences;
            try { occurrences = expect->getArray("occurrences"); }
            catch (...) { occurrences = nullptr; }
            if (!occurrences)
                throw Failure(location + " expect.occurrences must be an array.");
            for (size_t i = 0; i < occurrences->size(); ++i) {
                Poco::JSON::Object::Ptr item;
                try { item = occurrences->getObject(i); }
                catch (...) { item = nullptr; }
                if (!item)
                    throw Failure(location +
                                  " expect.occurrences entries must be objects.");
                TextOccurrenceExpectation expectation;
                expectation.text = RequiredString(
                    item, "text", location + " expect.occurrences");
                const long long count = RequiredInteger(
                    item, "count", location + " expect.occurrences");
                if (expectation.text.empty() || count < 0)
                    throw Failure(location +
                                  " occurrence text must be non-empty and count non-negative.");
                expectation.count = static_cast<size_t>(count);
                spec.textOccurrences.push_back(std::move(expectation));
            }
        }
        if (spec.bodyContains.empty() && spec.bodyNotContains.empty() &&
            spec.textOccurrences.empty()) {
            throw Failure(location + " source_contract has no expectations.");
        }
    } else if (spec.action == "path_policy") {
        spec.operation = RequiredString(input, "operation", location + " input");
        spec.expectedBool = RequiredBool(expect, "result", location + " expect");
        if (spec.operation == "known_project_relative") {
            spec.rawValue = RequiredString(input, "path", location + " input");
        } else if (spec.operation == "under_cwd" ||
                   spec.operation == "same_path" ||
                   spec.operation == "under_allowed_write_root") {
            spec.absPath = RequiredString(input, "path", location + " input");
            spec.cwd = RequiredString(input, "cwd", location + " input");
            if (spec.operation == "under_allowed_write_root") {
                spec.projectRoot = OptionalString(
                    input, "project_root", std::string(), location + " input");
                spec.skillsRoot = OptionalString(
                    input, "skills_root", std::string(), location + " input");
                spec.additionalRoots = OptionalStringArray(
                    input, "additional_roots", location + " input");
            }
        } else {
            throw Failure(location + " uses unsupported path_policy operation '" +
                          spec.operation + "'.");
        }
    } else if (spec.action == "sanitize_filename") {
        spec.rawValue = RequiredString(input, "raw", location + " input");
        spec.fallbackValue = OptionalString(
            input, "fallback", "file", location + " input");
        spec.expectedValue = RequiredString(expect, "result", location + " expect");
    } else if (spec.action == "var_demote") {
        spec.toolTag = RequiredString(input, "tool_tag", location + " input");
        spec.commandEcho = RequiredString(input, "command_echo", location + " input");
        spec.demoteBody = RequiredString(input, "body", location + " input");
        const long long threshold = OptionalInteger(
            input, "threshold_bytes", 12 * 1024, location + " input");
        if (threshold < 0)
            throw Failure(location + " threshold_bytes cannot be negative.");
        spec.thresholdBytes = static_cast<size_t>(threshold);
        spec.expectedDemoted = RequiredBool(
            expect, "demoted", location + " expect");
        spec.relPathEquals = OptionalString(
            expect, "rel_path", std::string(), location + " expect");
        spec.relPathContains = OptionalString(
            expect, "rel_path_contains", std::string(), location + " expect");
        spec.cardContains = OptionalStringArray(
            expect, "card_contains", location + " expect");
        if (expect->has("abs_path_empty")) {
            spec.hasExpectedAbsPathEmpty = true;
            spec.expectedAbsPathEmpty = OptionalBool(
                expect, "abs_path_empty", false, location + " expect");
        }
        if (expect->has("vars_file_count")) {
            const long long count = RequiredInteger(
                expect, "vars_file_count", location + " expect");
            if (count < 0)
                throw Failure(location + " vars_file_count cannot be negative.");
            spec.hasExpectedVarsFiles = true;
            spec.expectedVarsFiles = static_cast<size_t>(count);
        }
        spec.expectSpooledBodyEqualsInput = OptionalBool(
            expect, "spooled_body_equals_input", false, location + " expect");
        if (object->has("setup")) {
            const Poco::JSON::Object::Ptr setup =
                RequiredObject(object, "setup", location);
            spec.setupFile = RequiredString(setup, "file", location + " setup");
            const std::filesystem::path setupPath = std::filesystem::u8path(spec.setupFile);
            if (setupPath.is_absolute() || spec.setupFile.find("..") != std::string::npos)
                throw Failure(location + " setup file must stay under the case workspace.");
            spec.rawSetupBytes = RequiredString(
                setup, "content", location + " setup");
        }
    } else {
        throw Failure(location + " uses unsupported action '" + spec.action + "'.");
    }
    return spec;
}

std::string LowerExtension(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

} // namespace

std::filesystem::path ResolveJsonCaseDirectory(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--cases") {
            if (i + 1 >= argc) throw Failure("--cases requires a directory.");
            std::filesystem::path explicitPath = std::filesystem::u8path(argv[i + 1]);
            if (explicitPath.is_relative())
                explicitPath = std::filesystem::current_path() / explicitPath;
            if (!std::filesystem::is_directory(explicitPath))
                throw Failure("Cases directory not found: " + explicitPath.string());
            return explicitPath;
        }
    }

    std::vector<std::filesystem::path> candidates;
    const std::filesystem::path cwd = std::filesystem::current_path();
    candidates.push_back(cwd / "Cases");
    candidates.push_back(cwd / "Tests" / "LlamaBossTests" / "Cases");

    if (argc > 0 && argv[0] && *argv[0]) {
        std::error_code ec;
        const std::filesystem::path executable =
            std::filesystem::absolute(std::filesystem::u8path(argv[0]), ec);
        if (!ec) {
            const std::filesystem::path bin = executable.parent_path();
            candidates.push_back(bin / "Cases");
            const std::filesystem::path repository =
                bin.parent_path().parent_path();
            candidates.push_back(repository / "Tests" / "LlamaBossTests" / "Cases");
        }
    }

    for (const std::filesystem::path& candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::is_directory(candidate, ec) && !ec) return candidate;
    }

    std::ostringstream message;
    message << "No Cases directory was found. Use --cases <directory>. Searched:";
    for (const std::filesystem::path& candidate : candidates)
        message << "\n  " << candidate.string();
    throw Failure(message.str());
}

void RegisterJsonCases(const std::filesystem::path& directory)
{
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(directory, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (LowerExtension(it->path().extension().string()) == ".json")
            files.push_back(it->path());
    }
    if (ec) throw Failure("Could not enumerate Cases directory: " + ec.message());
    std::sort(files.begin(), files.end());
    if (files.empty())
        throw Failure("Cases directory contains no JSON files: " + directory.string());

    std::set<std::string> names;
    for (const TestCase& test : Registry()) names.insert(test.name);
    size_t added = 0;

    for (const std::filesystem::path& file : files) {
        Poco::JSON::Object::Ptr root;
        try {
            Poco::JSON::Parser parser;
            Poco::Dynamic::Var parsed = parser.parse(ReadText(file));
            root = parsed.extract<Poco::JSON::Object::Ptr>();
        } catch (const std::exception& ex) {
            throw Failure("Invalid JSON in " + file.string() + ": " + ex.what());
        }
        if (!root) throw Failure("JSON root must be an object: " + file.string());

        const long long schema = RequiredInteger(root, "schema_version", file.string());
        if (schema != 1)
            throw Failure(file.string() + " uses unsupported schema_version " +
                          std::to_string(schema) + "; expected 1.");

        Poco::JSON::Array::Ptr cases;
        try { cases = root->getArray("cases"); } catch (...) { cases = nullptr; }
        if (!cases) throw Failure(file.string() + " requires a cases array.");

        for (size_t i = 0; i < cases->size(); ++i) {
            Poco::JSON::Object::Ptr object;
            try { object = cases->getObject(i); } catch (...) { object = nullptr; }
            if (!object) throw Failure(file.string() + " case entries must be objects.");
            JsonCaseSpec spec = ParseCase(object, file.string(), i);
            if (!names.insert(spec.name).second)
                throw Failure("Duplicate test name: " + spec.name);

            Registry().push_back({ spec.name,
                [spec](const Environment& env) {
                    if (spec.action == "read_ranges") RunReadRangesCase(spec, env);
                    else if (spec.action == "parse_grep") RunParseGrepCase(spec, env);
                    else if (spec.action == "source_contract") RunSourceContractCase(spec, env);
                    else if (spec.action == "path_policy") RunPathPolicyCase(spec, env);
                    else if (spec.action == "sanitize_filename") RunSanitizeFilenameCase(spec, env);
                    else if (spec.action == "var_demote") RunVarDemoteCase(spec, env);
                    else throw Failure("Unsupported JSON action: " + spec.action);
                } });
            ++added;
        }
    }

    if (added == 0)
        throw Failure("JSON case files registered no tests: " + directory.string());
}

} // namespace lbtest
