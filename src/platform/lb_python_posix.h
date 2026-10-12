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
//   base interpreter : the first existing of Homebrew (/opt/homebrew,
//                      /usr/local), python.org's framework build, then
//                      /usr/bin/python3 only when the Command Line Tools
//                      are actually installed.
//   environment      : <data dir>/python, created from the base with
//                      --system-site-packages on the first package install,
//                      so packages the user already has stay importable.
//   run interpreter  : the environment's python3 once it exists, else base.
//
// Header-only; not compiled on Windows.
#pragma once

#ifndef _WIN32

#include <string>
#include <vector>
#include <unistd.h>

#include "lb_process.h"
#include "server_manager.h"

namespace lb_python {

inline bool IsExecutableFile(const std::string& p)
{
    return !p.empty() && ::access(p.c_str(), X_OK) == 0;
}

inline std::string FindBasePython3()
{
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

// Interpreter for helpers, scripts and the py session.  Empty when no
// Python 3 is installed.
inline std::string RunInterpreter()
{
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
    if (IsExecutableFile(VenvPython())) return true;
    const std::string base = FindBasePython3();
    if (base.empty()) {
        errorOut = NoPythonMessage();
        return false;
    }
    lb_process::Options o;
    o.argv = { base, "-m", "venv", "--system-site-packages", VenvDir() };
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
