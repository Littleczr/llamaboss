#include "test_framework.h"

#include "var_store.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

std::string ReadBytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) throw lbtest::Failure("Could not read test file: " + path.string());
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

void WriteBytes(const std::filesystem::path& path, const std::string& bytes)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        throw lbtest::Failure("Could not create test directory: " + ec.message());
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw lbtest::Failure("Could not write test file: " + path.string());
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) throw lbtest::Failure("Incomplete test-file write: " + path.string());
}

std::size_t CountRegularFiles(const std::filesystem::path& directory)
{
    std::error_code ec;
    if (!std::filesystem::exists(directory, ec)) return 0;

    std::size_t count = 0;
    for (std::filesystem::directory_iterator it(directory, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)) ++count;
    }
    if (ec) throw lbtest::Failure("Could not enumerate " + directory.string() +
                                  ": " + ec.message());
    return count;
}

std::string LargeBody()
{
    std::string body;
    for (int line = 1; line <= 300; ++line) {
        body += "record " + std::to_string(line) +
                " | alpha beta gamma | byte-exact payload\r\n";
    }
    return body;
}

varstore::DemotionConfig SmallThreshold()
{
    varstore::DemotionConfig config;
    config.thresholdBytes = 32;
    return config;
}

} // namespace

LB_TEST(DemotionThresholdIsStrictlyGreaterThan)
{
    const std::filesystem::path workspace = env.CaseDir("threshold");
    const varstore::DemotionConfig config = SmallThreshold();

    const std::string exactly(config.thresholdBytes, 'x');
    const varstore::DemoteOutcome atLimit = varstore::MaybeDemoteToolBody(
        "cmd", "threshold", exactly, workspace.u8string(), config);
    LB_EXPECT(!atLimit.demoted);
    LB_EXPECT_EQ(CountRegularFiles(workspace / "Vars"), std::size_t{ 0 });

    const std::string above(config.thresholdBytes + 1, 'y');
    const varstore::DemoteOutcome overLimit = varstore::MaybeDemoteToolBody(
        "cmd", "threshold", above, workspace.u8string(), config);
    LB_EXPECT(overLimit.demoted);
    LB_EXPECT_EQ(ReadBytes(std::filesystem::u8path(overLimit.absPath)), above);
}

LB_TEST(LargeOutputSpoolsByteExactlyAndReturnsGuidance)
{
    const std::filesystem::path workspace = env.CaseDir("large_output");
    const std::string body = LargeBody();

    const varstore::DemoteOutcome outcome = varstore::MaybeDemoteToolBody(
        "cmd", "build --verbose", body, workspace.u8string(), SmallThreshold());

    LB_EXPECT(outcome.demoted);
    LB_EXPECT(!outcome.absPath.empty());
    LB_EXPECT_EQ(ReadBytes(std::filesystem::u8path(outcome.absPath)), body);
    LB_EXPECT_CONTAINS(outcome.relPath, "Vars\\cmd_0001");
    LB_EXPECT_CONTAINS(outcome.cardBody, varstore::kCardSentinel);
    LB_EXPECT_CONTAINS(outcome.cardBody, outcome.relPath);
    LB_EXPECT_CONTAINS(outcome.cardBody, "grep \"<pattern>\"");
    LB_EXPECT_CONTAINS(outcome.cardBody, "read_range <start>:<end>");
    LB_EXPECT_EQ(CountRegularFiles(workspace / "Vars"), std::size_t{ 1 });
}

LB_TEST(WholeFileWorkspaceReadReusesTheLiveFile)
{
    const std::filesystem::path workspace = env.CaseDir("workspace_read");
    const std::filesystem::path source = workspace / "notes.md";
    const std::string body = LargeBody();
    WriteBytes(source, body);

    const varstore::DemoteOutcome outcome = varstore::MaybeDemoteToolBody(
        "read", "notes.md", body, workspace.u8string(), SmallThreshold());

    LB_EXPECT(outcome.demoted);
    LB_EXPECT(outcome.absPath.empty());
    LB_EXPECT_EQ(outcome.relPath, std::string("notes.md"));
    LB_EXPECT_CONTAINS(outcome.cardBody, "SAME variable file");
    LB_EXPECT_EQ(CountRegularFiles(workspace / "Vars"), std::size_t{ 0 });
}

LB_TEST(ReadingAnExistingVarsSpoolDoesNotDuplicateIt)
{
    const std::filesystem::path workspace = env.CaseDir("vars_reread");
    const std::filesystem::path spool = workspace / "Vars" / "cmd_0001.txt";
    const std::string body = LargeBody();
    WriteBytes(spool, body);

    const varstore::DemoteOutcome outcome = varstore::MaybeDemoteToolBody(
        "read", "Vars\\cmd_0001.txt", body, workspace.u8string(), SmallThreshold());

    LB_EXPECT(outcome.demoted);
    LB_EXPECT(outcome.absPath.empty());
    LB_EXPECT_EQ(outcome.relPath, std::string("Vars\\cmd_0001.txt"));
    LB_EXPECT_EQ(CountRegularFiles(workspace / "Vars"), std::size_t{ 1 });
}

LB_TEST(DerivedInspectResultNeverReusesItsBinaryInput)
{
    const std::filesystem::path workspace = env.CaseDir("derived_inspect");
    const std::filesystem::path source = workspace / "report.xlsx";
    const std::string fakeBinary("PK\x03\x04", 4);
    WriteBytes(source, fakeBinary);

    const std::string inspection =
        "Workbook: report.xlsx\nSheet: Revenue\nRows: 40000\n" + LargeBody();
    const varstore::DemoteOutcome outcome = varstore::MaybeDemoteToolBody(
        "xlsx_inspect", "report.xlsx", inspection,
        workspace.u8string(), SmallThreshold());

    LB_EXPECT(outcome.demoted);
    LB_EXPECT(!outcome.absPath.empty());
    LB_EXPECT_CONTAINS(outcome.relPath, "Vars\\xlsx_inspect_0001_report-xlsx.txt");
    LB_EXPECT_EQ(ReadBytes(std::filesystem::u8path(outcome.absPath)), inspection);
    LB_EXPECT_EQ(ReadBytes(source), fakeBinary);
    LB_EXPECT_EQ(CountRegularFiles(workspace / "Vars"), std::size_t{ 1 });
}

LB_TEST(ExistingHandleCardIsNeverRedemoted)
{
    const std::filesystem::path workspace = env.CaseDir("card_guard");
    const std::string card = std::string(varstore::kCardSentinel) +
        "\nfile: Vars\\cmd_0001.txt\n" + LargeBody();

    const varstore::DemoteOutcome outcome = varstore::MaybeDemoteToolBody(
        "cmd", "echo card", card, workspace.u8string(), SmallThreshold());

    LB_EXPECT(!outcome.demoted);
    LB_EXPECT_EQ(CountRegularFiles(workspace / "Vars"), std::size_t{ 0 });
}

LB_TEST(MarkdownShapeIgnoresHeadingsInsideFences)
{
    (void)env;
    const std::string markdown =
        "# Project\n"
        "Intro\n"
        "```text\n"
        "# This is data, not a heading\n"
        "```\n"
        "## Approved\n";

    const std::string shape = varstore::BuildShapeReport(markdown, "plan.md");
    LB_EXPECT_CONTAINS(shape, "L1: # Project");
    LB_EXPECT_CONTAINS(shape, "L6: ## Approved");
    LB_EXPECT_NOT_CONTAINS(shape, "This is data");
}

LB_TEST(CodeShapeReportsNavigationDeclarations)
{
    (void)env;
    const std::string source =
        "namespace demo {\n"
        "class Widget {\n"
        "public:\n"
        "    bool Start();\n"
        "};\n"
        "}\n";

    const std::string shape = varstore::BuildShapeReport(source, "widget.h");
    LB_EXPECT_CONTAINS(shape, "L1: namespace demo {");
    LB_EXPECT_CONTAINS(shape, "L2: class Widget {");
    LB_EXPECT_CONTAINS(shape, "L3: public:");
    LB_EXPECT_CONTAINS(shape, "L4: bool Start();");
}
