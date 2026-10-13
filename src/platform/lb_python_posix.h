// lb_python_posix.h
//
// macOS: which Python 3 the Python tools run, and LlamaBoss's own virtual
// environment for packages the user approves.
//
// The Windows build tries py -3, python and python3 and installs approved
// packages with `pip install --user`.  On macOS that does not work: the
// Homebrew Python refuses --user installs (PEP 668 "externally managed"),
// and /usr/bin/python3 is a Command Line Tools shim that opens an install
// dialog when the tools are missing.  So:
//
//   base interpreter : the Python bundled in LlamaBoss.app
//                      (Contents/Resources/python-runtime, from
//                      macos/fetch_python.sh: CPython with pypdf and
//                      openpyxl).  Only a build
//                      without it falls back to Homebrew (/opt/homebrew,
//                      /usr/local), python.org's framework build, then
//                      /usr/bin/python3 when the Command Line Tools are
//                      actually installed.
//   environment      : <data dir>/python, created from the base with
//                      --system-site-packages on the first package install,
//                      so the bundled packages stay importable.
//   run interpreter  : the environment's python3 once it exists, else base.
//
// With the bundled Python the environment must be built on it: one made
// from another Python (an older LlamaBoss, or Homebrew) is set aside, and one
// made from the same Python version at another app location (the app was
// moved) is re-pointed with `venv --upgrade`, keeping its packages.
//
// Header-only; not compiled on Windows.
#pragma once

#ifndef _WIN32

#include <cstdlib>
#include <dirent.h>
#include <ctime>
#include <fstream>
#include <limits.h>
#include <mutex>
#include <stdio.h>
#include <string>
#include <vector>
#include <unistd.h>

#include <wx/stdpaths.h>

#include "lb_process.h"
#include "server_manager.h"

namespace lb_python {

inline bool IsExecutableFile(const std::string& p)
{
    return !p.empty() && ::access(p.c_str(), X_OK) == 0;
}

inline std::string RealPath(const std::string& p)
{
    char buf[PATH_MAX];
    return (!p.empty() && ::realpath(p.c_str(), buf)) ? std::string(buf) : p;
}

// LlamaBoss.app/Contents/Resources/python-runtime, or empty when this build
// has none.
inline std::string BundledPythonDir()
{
    static const std::string dir = [] {
        // .../LlamaBoss.app/Contents/MacOS/LlamaBoss -> .../Contents
        std::string p = wxStandardPaths::Get().GetExecutablePath().ToStdString(wxConvUTF8);
        for (int i = 0; i < 2 && !p.empty(); ++i) {
            const size_t slash = p.rfind('/');
            p = (slash == std::string::npos) ? std::string() : p.substr(0, slash);
        }
        const std::string d = p + "/Resources/python-runtime";
        return (!p.empty() && IsExecutableFile(d + "/bin/python3")) ? d : std::string();
    }();
    return dir;
}

inline std::string BundledPython()
{
    const std::string d = BundledPythonDir();
    return d.empty() ? std::string() : d + "/bin/python3";
}

// "3.13" for the bundled runtime (its lib/python3.13 folder), or empty.
inline std::string BundledPythonMajorMinor()
{
    static const std::string mm = [] {
        std::string found;
        if (DIR* d = ::opendir((BundledPythonDir() + "/lib").c_str())) {
            while (const dirent* e = ::readdir(d)) {
                const std::string n = e->d_name;
                if (n.rfind("python3.", 0) == 0) { found = n.substr(6); break; }
            }
            ::closedir(d);
        }
        return found;
    }();
    return mm;
}

inline std::string FindBasePython3()
{
    const std::string bundled = BundledPython();
    if (!bundled.empty()) return bundled;

    static const char* const kCandidates[] = {
        "/opt/homebrew/bin/python3",
        "/usr/local/bin/python3",
        "/Library/Frameworks/Python.framework/Versions/Current/bin/python3",
    };
    for (const char* c : kCandidates)
        if (IsExecutableFile(c)) return c;
    // Apple's shim is only safe when the tools behind it exist.
    if (IsExecutableFile("/Library/Developer/CommandLineTools/usr/bin/python3") ||
        IsExecutableFile("/Applications/Xcode.app/Contents/Developer/usr/bin/python3"))
        return "/usr/bin/python3";
    return std::string();
}

inline std::string VenvDir()
{
    return ServerManager::GetDataDir() + "/python";
}

inline std::string VenvPython()
{
    return VenvDir() + "/bin/python3";
}

// A "key = value" line of the environment's pyvenv.cfg.
inline std::string VenvConfigValue(const std::string& key)
{
    std::ifstream in(VenvDir() + "/pyvenv.cfg");
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto trim = [](std::string s) {
            const size_t a = s.find_first_not_of(" \t\r");
            const size_t b = s.find_last_not_of(" \t\r");
            return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
        };
        if (trim(line.substr(0, eq)) == key) return trim(line.substr(eq + 1));
    }
    return std::string();
}

inline std::string ChildPath();

// With the bundled Python: makes an existing environment usable with it
// (see the top of this file).  Returns true when the environment can be
// used.  Blocking (venv --upgrade takes about a second); worker threads only.
inline bool ReconcileVenvWithBundled()
{
    const std::string bundledBin = BundledPythonDir() + "/bin";
    if (!IsExecutableFile(VenvPython())) return false;
    if (RealPath(VenvConfigValue("home")) == RealPath(bundledBin)) return true;

    static std::mutex s_mutex;
    std::lock_guard<std::mutex> lock(s_mutex);
    if (RealPath(VenvConfigValue("home")) == RealPath(bundledBin)) return true;

    const std::string version = VenvConfigValue("version");   // e.g. 3.13.16
    const std::string mm = BundledPythonMajorMinor();
    if (!mm.empty() && version.rfind(mm + ".", 0) == 0) {
        lb_process::Options o;
        o.argv = { BundledPython(), "-m", "venv", "--upgrade",
                   "--system-site-packages", VenvDir() };
        o.cwd = ServerManager::GetDataDir();   // see EnsureVenv
        o.envSet = { "PATH=" + ChildPath() };
        o.timeoutMs = 60000;
        o.mergeStderr = true;
        const lb_process::Result r = lb_process::Run(o);
        if (r.started && r.exitCode == 0 &&
            RealPath(VenvConfigValue("home")) == RealPath(bundledBin))
            return true;
    }

    // Another Python version, or the upgrade failed: keep it, but out of
    // the way.  Packages are reinstalled on demand into a new environment.
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", std::localtime(&now));
    const std::string aside = VenvDir() + ".previous-" + stamp;
    ::rename(VenvDir().c_str(), aside.c_str());
    return false;
}

// Interpreter for helpers, scripts and the py session.  Empty when no
// Python 3 is available.  Worker threads only (see ReconcileVenvWithBundled).
inline std::string RunInterpreter()
{
    const std::string bundled = BundledPython();
    if (!bundled.empty())
        return ReconcileVenvWithBundled() ? VenvPython() : bundled;

    const std::string venv = VenvPython();
    if (IsExecutableFile(venv)) return venv;
    return FindBasePython3();
}

// Homebrew prefixes in front of launchd's minimal PATH, as the shell tool does.
inline std::string ChildPath()
{
    const char* p = std::getenv("PATH");
    return std::string("/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:") +
           (p && *p ? p : "/usr/bin:/bin:/usr/sbin:/sbin");
}

inline std::string NoPythonMessage()
{
    return "Python 3 was not found. Install it with Homebrew (brew install python) "
           "or from python.org, then try again.";
}

// Creates the environment when it does not exist yet.  Blocking; call it
// from a worker thread.  Returns false with a readable error.
inline bool EnsureVenv(std::string& errorOut)
{
    if (!BundledPython().empty()) {
        if (ReconcileVenvWithBundled()) return true;
    } else if (IsExecutableFile(VenvPython())) {
        return true;
    }
    const std::string base = FindBasePython3();
    if (base.empty()) {
        errorOut = NoPythonMessage();
        return false;
    }
    lb_process::Options o;
    o.argv = { base, "-m", "venv", "--system-site-packages", VenvDir() };
    // Not the inherited cwd: wx sets it to the folder holding the app, and
    // Python reads its cwd at startup, so an app run from Downloads or the
    // Desktop would raise a folder-access prompt for an unrelated step.
    o.cwd = ServerManager::GetDataDir();
    o.envSet = { "PATH=" + ChildPath() };
    o.timeoutMs = 180000;
    o.mergeStderr = true;
    const lb_process::Result r = lb_process::Run(o);
    if (!r.started || r.exitCode != 0 || !IsExecutableFile(VenvPython())) {
        errorOut = "Could not create LlamaBoss's Python environment at " + VenvDir() +
                   " with " + base + ":\n" + r.out;
        return false;
    }
    return true;
}

} // namespace lb_python

#endif // !_WIN32
