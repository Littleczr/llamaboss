// python_resources.h — LlamaBoss's built-in Python scripts, embedded in the
// executable as RCDATA resources.
//
// Source of truth: assets/python/<name>.py.  python_helpers.rc2 embeds each
// file under the resource name <NAME> (the file stem, upper-cased; resource
// names are case-insensitive).  At runtime python_runner.cpp writes helpers
// to %USERPROFILE%\LlamaBoss\System\PythonHelpers\<name>.py and
// python_session.cpp writes the kernel to the session temp dir.
//
// To add a script: drop the .py into assets/python, add one line to
// python_helpers.rc2, add the name below, and add the dispatch branch in
// python_runner.cpp.  python_resources_tests.cpp checks that the three lists
// agree and that every .py is LF-only UTF-8 without a BOM.
//
// The name table is portable (the test builds without Windows); only the
// loader needs <windows.h>.

#pragma once

#include <cstddef>
#include <string>

namespace lb_pyres {

// The 14 fixed helper scripts run through PythonHelperThread.
inline constexpr const char* kHelperNames[] = {
    "python_health",
    "csv_inspect",
    "csv_report",
    "csv_to_xlsx",
    "xlsx_create_workbook",
    "xlsx_inspect",
    "xlsx_report",
    "pdf_extract_text",
    "pdf_inspect_form",
    "pdf_fill_form",
    "docx_extract_text",
    "docx_inspect",
    "zip_inspect",
    "zip_extract",
};

// The persistent `py` session kernel (python_session.cpp).
inline constexpr const char* kKernelName = "lb_kernel";

inline bool IsHelperName(const std::string& name)
{
    for (const char* n : kHelperNames)
        if (name == n) return true;
    return false;
}

// Resource name for a script: the file stem upper-cased.
inline std::string ResourceName(const std::string& name)
{
    std::string out = name;
    for (char& c : out)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return out;
}

} // namespace lb_pyres

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace lb_pyres {

// Points `data`/`size` at the embedded bytes of script `name` (e.g.
// "csv_inspect", "lb_kernel").  The bytes live in the mapped executable
// image for the life of the process and are NOT NUL-terminated.  Returns
// false with a readable error when the resource is missing or empty —
// callers must never write a script file in that case.
inline bool Load(const std::string& name,
                 const char*&       data,
                 std::size_t&       size,
                 std::string&       errorOut)
{
    data = nullptr;
    size = 0;
    const std::string resName = ResourceName(name);
    HRSRC res = ::FindResourceA(nullptr, resName.c_str(), MAKEINTRESOURCEA(10) /* RT_RCDATA */);
    if (!res) {
        errorOut = "Built-in Python script '" + name +
                   "' is missing from LlamaBoss.exe (RCDATA " + resName +
                   " not found). Rebuild LlamaBoss with python_helpers.rc2.";
        return false;
    }
    HGLOBAL mem = ::LoadResource(nullptr, res);
    const DWORD bytes = ::SizeofResource(nullptr, res);
    const void* ptr = mem ? ::LockResource(mem) : nullptr;
    if (!ptr || bytes == 0) {
        errorOut = "Built-in Python script '" + name +
                   "' could not be loaded from LlamaBoss.exe (RCDATA " +
                   resName + " is empty or unreadable).";
        return false;
    }
    data = static_cast<const char*>(ptr);
    size = static_cast<std::size_t>(bytes);
    return true;
}

} // namespace lb_pyres
#endif // _WIN32
