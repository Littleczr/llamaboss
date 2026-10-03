# LlamaBossTests

This native Windows regression runner exercises file reading/search, path
safety, variables, and JSON-defined cases against production implementations.
It does not need a model server or API key.

## Build and run

Open `LlamaBoss.slnx` with the v145 C++ toolset and vcpkg manifest integration.
Build `LlamaBossTests` in Release | x64. From the repository root:

```powershell
.\x64\Release\LlamaBossTests.exe --cases .\Tests\LlamaBossTests\Cases
```

You can instead pass an absolute path to your maintained Cases directory.
Use `--help` to inspect runner options. The runner writes `RESULTS.md` and
`results.json` into its configured result directory.

**The 2026-10-03 source snapshot did not include the baseline JSON case files.**
Restore the maintained cases before running the command above. An empty Cases
directory is an error, not a passing regression run. Do not substitute invented
cases or report a successful full suite from a reduced case set.

## Portable harnesses

Several root-level `*_tests.cpp` files have independent `main` functions and
build instructions in their comments. These harnesses are separate from this
project; some require Poco while others use only the C++ standard library.

For example, from the repository root with a C++17 compiler:

```sh
g++ -std=c++17 -O1 python_resources_tests.cpp -o python_resources_tests
./python_resources_tests .
g++ -std=c++17 command_policy_tests.cpp command_policy.cpp -o command_policy_tests
./command_policy_tests
```

The Python resource harness checks the helper names, resource manifest, project
entries, and UTF-8/LF source bytes. Checking the embedded Windows resource bytes
also requires a Windows build with `python_helpers.rc2` linked into the harness.
