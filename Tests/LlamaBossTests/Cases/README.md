# Regression cases

This directory contains the maintained JSON regression case files discovered
by the native runner. Use `LlamaBossTests.exe --cases <directory>` to point to
another Cases folder when needed. The complete suite must include every JSON
case file in this directory; the runner rejects an empty directory.

See `../README_TESTS.md` for build and execution instructions and
`../json_case_loader.cpp` for the supported case schema.
