# Regression cases

Place the maintained JSON regression case files in this directory, or use
`LlamaBossTests.exe --cases <directory>` to point to another Cases folder.

The 2026-10-03 source archive included the runner and JSON loader but omitted
the baseline case files. No replacement baseline is supplied here. The runner
rejects an empty directory, so restore the real cases before claiming a complete
regression run.

See `../README_TESTS.md` for build and execution instructions and
`../json_case_loader.cpp` for the supported case schema.
