// python_arg_policy.cpp

#include "python_arg_policy.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace {

std::string LowerAscii(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string TrimAscii(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n\"'");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n\"'");
    return s.substr(a, b - a + 1);
}

bool IsAlnumAscii(char c)
{
    return (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9');
}

bool LooksLikeRequirementsFileName(const std::string& p)
{
    // Allow real package names such as "requirements-parser"; reject only
    // obvious requirements-file arguments that pip would treat as a file/list.
    if (p == "requirements.txt") return true;

    const std::string suffix = ".txt";
    if (p.size() <= suffix.size() ||
        p.compare(p.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return false;
    }

    return p.rfind("requirements-", 0) == 0 ||
           p.find("-requirements") != std::string::npos ||
           p.find(".requirements") != std::string::npos ||
           p.rfind("requirements.", 0) == 0;
}

} // namespace

namespace python_arg_policy {

bool NormalizeAllowedPythonPackage(const std::string& requested,
                                   std::string&       packageOut,
                                   std::string&       errorOut)
{
    std::string p = LowerAscii(TrimAscii(requested));
    std::replace(p.begin(), p.end(), '_', '-');

    // Friendly aliases for import/module names the model or user may mention.
    if (p == "docx") p = "python-docx";
    if (p == "fitz") p = "pymupdf";
    if (p == "pil")  p = "pillow";
    if (p == "pptx") p = "python-pptx";
    if (p == "bs4")  p = "beautifulsoup4";

    if (p.empty()) {
        errorOut = "python_install_package requires one package name.";
        return false;
    }

    // PEP 503 normalized name characters: lowercase letters, digits, '-', '.'.
    // Reject anything that could smuggle a path, URL, version specifier, extras,
    // pip flag, requirements file, or shell payload through the argument.
    for (char c : p) {
        const bool ok = (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') ||
                        c == '-' || c == '.';
        if (!ok) {
            errorOut = "python_install_package accepts one simple package "
                       "name only; no versions, paths, URLs, extras, pip "
                       "flags, or requirements files.";
            return false;
        }
    }

    if (p.rfind("--", 0) == 0 ||
        p.rfind("-", 0)  == 0 ||
        LooksLikeRequirementsFileName(p)) {
        errorOut = "python_install_package does not accept pip flags or "
                   "requirements files.";
        return false;
    }

    if (!IsAlnumAscii(p.front())) {
        errorOut = "python_install_package package names must start with a "
                   "letter or digit.";
        return false;
    }
    if (!IsAlnumAscii(p.back())) {
        errorOut = "python_install_package package names must end with a "
                   "letter or digit.";
        return false;
    }

    packageOut = p;
    errorOut.clear();
    return true;
}

bool IsReservedBuiltInPythonHelperName(const std::string& filename)
{
    const std::string key = LowerAscii(filename);
    static const char* reserved[] = {
        "python_health.py",
        "csv_inspect.py",
        "csv_report.py",
        "csv_to_xlsx.py",
        "xlsx_create_workbook.py",
        "xlsx_inspect.py",
        "xlsx_report.py",
        "pdf_extract_text.py",
        "pdf_inspect_form.py",
        "pdf_fill_form.py",
        "docx_extract_text.py",
        "docx_inspect.py"
    };

    for (const char* name : reserved) {
        if (key == name) return true;
    }
    return false;
}

} // namespace python_arg_policy
