// python_arg_policy.h
//
// Shared Python argument policy used by both tool_router.cpp and
// python_runner.cpp.
//
// The router performs shape validation before an approval card is shown;
// the runner repeats the same validation immediately before invoking Python
// for defense-in-depth.  Keeping these rules in one small module prevents
// the two validation layers from drifting.

#pragma once

#include <string>

namespace python_arg_policy {

// Normalize and validate the single package name accepted by
// python_install_package.
//
// This intentionally accepts only a simple PyPI distribution name after
// friendly import-name aliases are applied.  It rejects paths, URLs,
// version specifiers, extras, pip flags, requirements files, and shell-shaped
// payloads.  The per-package approval card remains the user-visible safety
// boundary.
bool NormalizeAllowedPythonPackage(const std::string& requested,
                                   std::string&       packageOut,
                                   std::string&       errorOut);

// Built-in helper script filenames are reserved so user-created scripts in the
// Scripts / Workflows / Skills lanes cannot collide with implementation
// helpers such as csv_inspect.py or xlsx_create_workbook.py.
bool IsReservedBuiltInPythonHelperName(const std::string& filename);

} // namespace python_arg_policy
