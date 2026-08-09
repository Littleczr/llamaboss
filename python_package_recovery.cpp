// ─── python_package_recovery.cpp ───────────────────────────────────
// Implementation of the missing-python-package recovery helpers
// extracted from LlamaBoss.cpp.  See python_package_recovery.h for
// the public API contract.

// Required because tool_dispatcher.h transitively pulls in wxWidgets
// headers (wxcrt.h), whose strcpy/wcscpy calls trip MSVC's SDLCheck
// C4996 deprecation-as-error.  Mirrors the define at the top of
// LlamaBoss.cpp and other TUs that include wx headers.  Must come
// before any other #include.
#define _CRT_SECURE_NO_WARNINGS

#include "python_package_recovery.h"
#include "lb_string_utils.h"
#include "tool_dispatcher.h"   // ToolInvocationResult
#include "python_runner.h"     // PythonRunResult

#include <algorithm>
#include <sstream>
#include <string>

namespace {

// Trim characters that commonly wrap an inferred package token in
// model output or pip diagnostics: whitespace, quotes, punctuation
// likely to flank the name in error messages.
std::string LbTrimPackageToken(std::string s)
{
    size_t a = s.find_first_not_of(" \t\r\n\"'`.,:;()[]{}");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t\r\n\"'`.,:;()[]{}");
    return s.substr(a, b - a + 1);
}

// Syntactic-only vetting of a candidate package name.  The historical
// allowlist has been retired; per-package approval cards (rendered by
// tool_approval.h for python_install_package) are now the safety
// boundary, so this helper only needs to confirm the name is
// syntactically usable as a pip target.  This mirrors the character
// whitelist enforced authoritatively by NormalizeAllowedPythonPackage
// in python_runner.cpp.
bool LbPackageIsAllowed(const std::string& packageName)
{
    if (packageName.empty()) return false;
    if (!((packageName.front() >= 'a' && packageName.front() <= 'z') ||
          (packageName.front() >= '0' && packageName.front() <= '9'))) {
        return false;
    }
    for (char c : packageName) {
        const bool ok = (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') ||
                        c == '-' || c == '.';
        if (!ok) return false;
    }
    return true;
}

// Map common import names to their actual PyPI package names.
// Examples: `import docx` is `pip install python-docx`,
// `import fitz` is `pip install pymupdf`.
std::string LbNormalizeMissingPackageName(const std::string& raw)
{
    std::string p = LbLowerAscii(LbTrimPackageToken(raw));
    std::replace(p.begin(), p.end(), '_', '-');

    if (p == "docx") p = "python-docx";
    else if (p == "fitz") p = "pymupdf";
    else if (p == "pil") p = "pillow";
    else if (p == "pptx") p = "python-pptx";
    else if (p == "bs4") p = "beautifulsoup4";

    return p;
}

// Scan `text` for `token`, then read the package-name-like sequence
// that follows.  Skips intermediate whitespace and common quoting
// characters between the token and the name, then consumes
// [A-Za-z0-9_.-]+ as the name.  Returns true and fills `out` on
// success; false if either the token isn't found or no valid name
// follows it.
bool LbExtractAfterToken(const std::string& text,
                         const std::string& token,
                         std::string&       out)
{
    size_t pos = text.find(token);
    if (pos == std::string::npos) return false;
    pos += token.size();

    while (pos < text.size() &&
           (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\r' ||
            text[pos] == '\n' || text[pos] == '\'' || text[pos] == '"' ||
            text[pos] == '`')) {
        ++pos;
    }

    size_t end = pos;
    while (end < text.size()) {
        const char c = text[end];
        const bool ok = (c >= 'A' && c <= 'Z') ||
                        (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') ||
                        c == '_' || c == '-' || c == '.';
        if (!ok) break;
        ++end;
    }

    if (end <= pos) return false;
    out = text.substr(pos, end - pos);
    return !out.empty();
}

// Inspect a failed python run's stdout/stderr for a missing-module
// signal.  Tries (in order):
//   1. "No module named X" / "no module named X" — the canonical
//      ModuleNotFoundError shape.
//   2. "pip install --user ..." / "pip install --user
//      --disable-pip-version-check ..." — a hint our own scripts
//      sometimes emit before failing.
//   3. A small allow-list of "openpyxl python package is required",
//      "install pymupdf", "install pypdf" phrases that our own
//      helpers emit when they detect a missing dependency early.
// On success: importNameOut is the raw import-name candidate,
// packageNameOut is the normalized PyPI name, and installableOut is
// true iff the normalized name passes syntactic validation as a pip
// target.  Returns false if no missing-package signal was found.
bool LbFindMissingPythonPackage(const std::string& stdoutText,
                                const std::string& stderrText,
                                std::string&       importNameOut,
                                std::string&       packageNameOut,
                                bool&              installableOut)
{
    importNameOut.clear();
    packageNameOut.clear();
    installableOut = false;

    const std::string text = stderrText + "\n" + stdoutText;
    const std::string lower = LbLowerAscii(text);

    std::string candidate;
    if (!LbExtractAfterToken(text, "No module named", candidate) &&
        !LbExtractAfterToken(text, "no module named", candidate)) {
        if (!LbExtractAfterToken(lower, "pip install --user --disable-pip-version-check", candidate) &&
            !LbExtractAfterToken(lower, "pip install --user", candidate)) {
            if (lower.find("openpyxl python package is required") != std::string::npos) {
                candidate = "openpyxl";
            } else if (lower.find("missing pdf form dependency") != std::string::npos ||
                       lower.find("install pymupdf") != std::string::npos) {
                candidate = "pymupdf";
            } else if (lower.find("missing pdf text extraction dependency") != std::string::npos ||
                       lower.find("install pypdf") != std::string::npos) {
                candidate = "pypdf";
            }
        }
    }

    candidate = LbTrimPackageToken(candidate);
    if (candidate.empty()) return false;

    importNameOut = candidate;
    packageNameOut = LbNormalizeMissingPackageName(candidate);
    // installableOut is true when the normalized name is a syntactically
    // valid pip target (no path/URL/version/extras/flags).  The historical
    // allowlist that this used to consult has been retired; the per-package
    // approval card is now the safety boundary.
    installableOut = LbPackageIsAllowed(packageNameOut);
    return true;
}

// Detect whether a missing-module diagnostic came from a REMOTE
// machine whose output was merely relayed through a local wrapper
// script (the runPod skill's runpod_ssh.py is the canonical case:
// verify_env.py fails on the pod, its ModuleNotFoundError traceback
// rides home inside the wrapper's stdout, and the recovery card then
// wrongly suggests python_install_package — which installs LOCALLY
// and cannot fix the pod).  Observed in production 2026-08-03.
//
// Two independent signals:
//   1. Relay envelope markers: runpod_ssh.py prints a JSON envelope
//      containing "remote_exit_code" and a "Remote command finished
//      on <target>" line around relayed output.
//   2. Traceback frame origin: a traceback produced on THIS machine
//      (Windows) cites drive-letter frames (File "C:\...).  A
//      traceback whose File "..." frames are exclusively POSIX
//      absolute paths (File "/usr/..., File "/workspace/...) was
//      produced on another OS and therefore another machine.
// Frames like File "<frozen importlib._bootstrap>" match neither
// pattern and are ignored by both tests.
bool LbLooksLikeRemoteExecutionOutput(const std::string& text)
{
    const std::string lower = LbLowerAscii(text);
    if (lower.find("\"remote_exit_code\"") != std::string::npos) return true;
    if (lower.find("remote command finished on ") != std::string::npos) return true;

    const bool hasPosixFrame = text.find("File \"/") != std::string::npos;
    if (!hasPosixFrame) return false;

    size_t pos = 0;
    while ((pos = text.find("File \"", pos)) != std::string::npos) {
        pos += 6;
        if (pos + 2 < text.size()) {
            const char d = text[pos];
            const bool driveLetter =
                ((d >= 'A' && d <= 'Z') || (d >= 'a' && d <= 'z')) &&
                text[pos + 1] == ':' &&
                (text[pos + 2] == '\\' || text[pos + 2] == '/');
            if (driveLetter) return false;   // local frame present
        }
    }
    return true;   // POSIX frames only: the failure happened remotely
}

} // anonymous namespace

void ApplyMissingPythonPackageRecovery(ToolInvocationResult&  r,
                                       const PythonRunResult& py)
{
    if (py.exitCode == 0 || py.cancelled || py.timedOut) return;

    std::string importName;
    std::string packageName;
    bool installable = false;
    if (!LbFindMissingPythonPackage(py.stdoutText,
                                    py.stderrText,
                                    importName,
                                    packageName,
                                    installable)) {
        return;
    }

    r.iconUtf8 = "\xF0\x9F\x93\xA6"; // 📦

    // Remote relays get their own card BEFORE the local-install
    // suggestion is composed: coaching python_install_package here
    // would install the package on THIS machine while the failure
    // lives on the remote host.  A small model follows that bait,
    // installs locally, retries, hits the identical remote failure,
    // and loops.  Name the right move instead.
    if (LbLooksLikeRemoteExecutionOutput(py.stdoutText + "\n" + py.stderrText)) {
        const std::string& shown =
            installable ? packageName : importName;
        r.toolName = "Missing Remote Python Package";

        std::ostringstream remoteBody;
        remoteBody
            << "The failed run reports `" << shown
            << "` missing \xE2\x80\x94 but the traceback comes from a "
               "REMOTE machine whose output was relayed through this "
               "script, not from this computer's Python.\n\n"
            << "Do NOT use `python_install_package` \xE2\x80\x94 it installs "
               "locally and cannot fix the remote host. Install the package "
               "on the remote machine through the same channel that ran "
               "this command (for example `python -m pip install " << shown
            << "` via the remote helper), then retry the failed step once.";
        if (!r.body.empty()) {
            remoteBody << "\n\nOriginal stdout:\n" << r.body;
        }
        r.body = remoteBody.str();
        r.bodyLang = "markdown";
        return;
    }

    r.toolName = installable ? std::string("Missing Python Package")
                             : std::string("Unsupported Python Package");

    std::ostringstream body;
    if (installable) {
        // Standard recovery path: name is syntactically valid for pip,
        // suggest the install tool.  The user still has to approve the
        // install card before pip runs.
        body << "Python needs the package `" << packageName
             << "` before this step can continue.\n\n"
             << "Suggested next step for LlamaBoss: use `python_install_package "
             << packageName << "`, then retry the failed step once.\n\n"
             << "No package was installed yet. The user will see an approval "
                "card with the exact package name before pip runs.";
    } else {
        // The detected name didn't even pass syntactic validation
        // (unusual characters, looked like a path/URL/version, etc.).
        // Don't suggest python_install_package -- it would just fail.
        body << "Python tried to import `" << importName
             << "`, but the inferred package name `" << packageName
             << "` is not a simple PyPI name LlamaBoss can install through "
                "python_install_package.\n\n"
             << "The script may need to be rewritten using the standard "
                "library, or the user can install the dependency manually.";
    }

    if (!r.body.empty()) {
        body << "\n\nOriginal stdout:\n" << r.body;
    }

    r.body = body.str();
    r.bodyLang = "markdown";
}
