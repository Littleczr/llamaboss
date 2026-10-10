// tool_python_syntax.h
//
// Shared Python syntax pre-check, used by both tool_write.cpp (the
// write / overwrite_file tools) and tool_router.cpp (the
// python_create_script tool).  One implementation so deadlock and
// verdict fixes can't land in one copy but not another.
//
// ─── Behavior ─────────────────────────────────────────────────────
// CheckFile compiles the file at `filePath` in memory (compile(), via
// `python -I -B -c`, writing no __pycache__) using whichever
// launcher is found first (py.exe -3, then python.exe, then
// python3.exe).  It never throws and always returns a populated result.
//
// The verdict is deliberately narrow: ok == false ONLY when the
// compiler output names a recognized source-level error
// (SyntaxError / IndentationError / TabError).  Anything else -- no
// interpreter on PATH, a py launcher with no usable 3.x runtime, a
// check that times out, or the checker failing to start -- leaves
// ok == true so the caller does NOT block the write/creation; a real
// runtime problem is surfaced later by python_health / python_run_script.
//
// ─── Threading / deadlock note ────────────────────────────────────
// The child's stdout/stderr pipe is drained on a helper thread while
// the main thread does a bounded (10s) WaitForSingleObject, so a child
// that emits more output than the pipe buffer can hold can never
// deadlock against the wait.  Runs synchronously on the caller's
// thread otherwise; callers should invoke it off the UI thread.
//
#pragma once

#include <string>

namespace tool_python_syntax {

struct SyntaxCheckResult {
    // false ONLY for a recognized SyntaxError / IndentationError /
    // TabError.  true in every other case, including "couldn't verify".
    bool ok = true;

    // true if the checker actually produced a verdict (clean or error);
    // false if no usable interpreter ran, or the check timed out.
    // Carried for diagnostics / future use; callers may ignore it.
    bool checked = false;

    // On a syntax failure: the compiler output.  Otherwise a short
    // diagnostic (not surfaced to the user when ok == true).
    std::string message;
};

// Syntax-checks `filePath` without writing bytecode.  Never throws.  See header notes for
// the exact verdict rules.
SyntaxCheckResult CheckFile(const std::string& filePath);

} // namespace tool_python_syntax
