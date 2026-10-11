#include "test_framework.h"

#include "skill_connections.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// Which Connections a python_run_script script may receive: the declaration
// parser (frontmatter + "## Connections" section) and the owning-file walk
// that keeps a script from borrowing another folder's declaration.

namespace {

namespace sc = lb_skill_connections;

std::string Join(const std::vector<std::string>& v)
{
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) out += (i ? "," : "") + v[i];
    return out;
}

std::string Declared(const std::string& doc)
{
    return Join(sc::ParseDeclaredConnections(doc));
}

// Paths compare as paths, not text: on Windows the walk returns backslash
// separators while test paths built from "a/b" literals keep '/'.
bool SamePath(const std::string& actual, const std::filesystem::path& expected)
{
    if (actual.empty()) return false;
    return std::filesystem::u8path(actual).lexically_normal() ==
           expected.lexically_normal();
}

void Touch(const std::filesystem::path& p)
{
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << "x";
    if (!out) throw lbtest::Failure("Could not write test file: " + p.string());
}

} // namespace

LB_TEST(Connections_FrontmatterForms)
{
    (void)env;
    LB_EXPECT_EQ(Declared("---\nname: x\nconnections: runpod, Smartsheet\n---\nbody"),
                 std::string("runpod,smartsheet"));
    LB_EXPECT_EQ(Declared("---\r\nconnections: [\"runpod\", 'gmail']\r\n---\r\n"),
                 std::string("runpod,gmail"));
    LB_EXPECT_EQ(Declared("---\nconnections:\n  - runpod\n  - gmail\ndescription: d\n---\n"),
                 std::string("runpod,gmail"));
    LB_EXPECT_EQ(Declared("\xEF\xBB\xBF---\nconnections: runpod\n---\n"),
                 std::string("runpod"));
    // Only top-level keys count; an unterminated frontmatter block is body.
    LB_EXPECT_EQ(Declared("---\nmeta:\n  connections: runpod\n---\n"), std::string());
    LB_EXPECT_EQ(Declared("---\nconnections: runpod\n"), std::string());
}

LB_TEST(Connections_BodySection)
{
    (void)env;
    const std::string doc =
        "# My skill\n\nUses connections for stuff.\n\n## Connections\n"
        "This skill needs:\n- runpod - the RunPod API key\n* `SMARTSHEET_TOKEN`\n"
        "1. **gmail**: mail\n## Steps\n- notakey\n";
    LB_EXPECT_EQ(Declared(doc), std::string("runpod,smartsheet_token,gmail"));
    // Any heading level; fenced code inside the section is ignored.
    LB_EXPECT_EQ(Declared("### connections\n```\n- fake\n```\n- real\n"),
                 std::string("real"));
    LB_EXPECT_EQ(Declared("---\nname: x\n---\n# Title\n- item\n"), std::string());
    // Frontmatter and section together are de-duplicated.
    LB_EXPECT_EQ(Declared("---\nconnections: runpod\n---\n## Connections\n- RunPod\n"),
                 std::string("runpod"));
}

LB_TEST(Connections_MatchProviderOrEnvName)
{
    (void)env;
    const std::vector<std::string> d = { "runpod", "smartsheet_token" };
    LB_EXPECT(sc::IsDeclared(d, "runpod", "RUNPOD_API_KEY"));
    LB_EXPECT(sc::IsDeclared(d, "RunPod", "X"));
    LB_EXPECT(sc::IsDeclared(d, "smartsheet", "SMARTSHEET_TOKEN"));
    LB_EXPECT(!sc::IsDeclared(d, "smartsheet", "SMARTSHEET_OTHER"));
    LB_EXPECT(!sc::IsDeclared(d, "openrouter", "OPENROUTER_API_KEY"));
}

LB_TEST(Connections_OwningInstructionFile)
{
    namespace fs = std::filesystem;
    const fs::path base = env.CaseDir("owning_doc");
    Touch(base / "Skills/runPod/SKILL.md");
    Touch(base / "Skills/runPod/scripts/ops.py");
    Touch(base / "Skills/SKILL.md");                 // stray doc AT the root never counts
    Touch(base / "Skills/loose/a.py");
    Touch(base / "Proj/Workflows/WORKFLOW.md");      // same for the Workflows root
    Touch(base / "Proj/Workflows/flow1/WORKFLOW.md");
    Touch(base / "Skills2/evil/SKILL.md");           // name-prefix lookalike of Skills
    const std::vector<std::string> roots = {
        (base / "Skills").u8string(), (base / "Proj/Workflows").u8string() };

    auto owner = [&](const fs::path& p) {
        return sc::FindOwningInstructionDoc(p.u8string(), roots);
    };
    const fs::path runPodDoc = base / "Skills/runPod/SKILL.md";
    LB_EXPECT(SamePath(owner(base / "Skills/runPod/scripts/ops.py"), runPodDoc));
    LB_EXPECT(SamePath(owner(base / "Skills/runPod/ops.py"), runPodDoc));
    LB_EXPECT_EQ(owner(base / "Skills/loose/a.py"), std::string());
    LB_EXPECT_EQ(owner(base / "Skills/flat.py"), std::string());
    LB_EXPECT(SamePath(owner(base / "Proj/Workflows/flow1/sub/x.py"),
                       base / "Proj/Workflows/flow1/WORKFLOW.md"));
    LB_EXPECT_EQ(owner(base / "Proj/Workflows/flat.py"), std::string());
    LB_EXPECT_EQ(owner(base / "Chat/Scripts/x.py"), std::string());
    LB_EXPECT_EQ(owner(base / "Skills/../Chat/Scripts/x.py"), std::string());
    LB_EXPECT_EQ(owner(base / "Skills2/evil/x.py"), std::string());
}
