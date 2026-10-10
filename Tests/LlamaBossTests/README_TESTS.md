# LlamaBoss Native Regression Tests

`LlamaBossTests.exe` is an independent console runner in the LlamaBoss solution.
It compiles the real production `var_store.cpp`, `path_safety.cpp`,
`tool_path.cpp`, and `tool_read.cpp` files, so a failure indicates that
production behavior changed.

The project defines both `NOMINMAX` and `_CRT_SECURE_NO_WARNINGS` because these
production files normally inherit those protections through LlamaBoss's
precompiled wxWidgets header. The standalone test target does not use the
application's precompiled header.

## First run in Visual Studio

1. Select `Debug` and `x64` in the toolbar.
2. Right-click **LlamaBossTests** and choose **Set as Startup Project**.
3. Right-click **LlamaBossTests** and choose **Build**.
4. Press **Ctrl+F5**.

The first build can take longer while vcpkg restores the dependencies from the
solution's existing `vcpkg.json` manifest. A successful run ends with:

```text
12 passed, 0 failed
```

Reports are written to the process working directory:

- `TestResults/RESULTS.md`
- `TestResults/results.json`

Temporary test workspaces are deleted after a completely successful run. They
are retained automatically after a failure so the files can be inspected.

## Runner options

Run these from a Developer PowerShell opened in the folder containing the built
executable:

```powershell
.\LlamaBossTests.exe --list
.\LlamaBossTests.exe --filter Shape
.\LlamaBossTests.exe --keep-temp
.\LlamaBossTests.exe --cases C:\path\to\Cases
```

Exit codes are `0` for success, `1` for a test failure, and `2` for a runner or
configuration error. This makes the executable suitable for PowerShell scripts
and later CI automation.

## Initial coverage

- The demotion threshold is strictly greater-than, not greater-than-or-equal.
- Large tool output is spooled byte-exactly and produces useful query guidance.
- Whole-file workspace reads reuse the live source file.
- Re-reading a `Vars` spool creates no duplicate.
- Derived inspection output never points its card at a binary input file.
- An existing handle card is never demoted again.
- Markdown shape detection ignores headings inside fenced code blocks.
- Code shape detection reports navigation declarations.
- A single ranged read returns exactly the requested inclusive lines.
- Multi-range reads preserve range order, labels, and exact content.
- Multi-range reads reject a combined request above 1,000 lines atomically.
- Quoted grep patterns preserve spaces and colons while separating the path.

The quoted-pattern case uses the same UI-free parser included by production
`tool_router.cpp`. The parser was extracted into `tool_grep_args.h` so the test
cannot drift into testing a private copy.

## Adding a JSON test without compiling

The runner discovers every `.json` file in `Cases` each time it starts. The
four read-range/grep tests above are loaded from `Cases/rlm_retrieval.json`.

For a new case that uses a supported action:

1. Add or update a JSON file under `Cases`.
2. Save it.
3. Run the existing `LlamaBossTests.exe` again.

No C++ compilation is required merely because a JSON case changed. See
`Cases/README.md` for the schema and supported `read_ranges` and `parse_grep`
actions. Case names must be unique.

The test executable must still be rebuilt when production C++ changes, because
the runner compiles the real production sources into the executable. It must
also be rebuilt when a completely new action type is added to the JSON engine.

## Adding a native C++ test

Use native C++ for invariants that cannot be expressed by an existing JSON
action. Add another source file to the project, include `test_framework.h`, and
write:

```cpp
LB_TEST(MyRegressionCase)
{
    const std::filesystem::path workspace = env.CaseDir("my_case");
    LB_EXPECT(/* condition */);
}
```

Keep tests deterministic and offline. Put all disposable files under the
per-test directory returned by `env.CaseDir(...)`.
