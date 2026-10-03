// python_session.h
//
// RLM step 2, phase S1 — the persistent Python session.
//
// Unlike PythonRunner (one process per call, fixed helpers only), this
// subsystem keeps ONE long-lived Python "kernel" process alive per
// conversation and executes model/user-provided code snippets against a
// persistent namespace, Jupyter-style: a variable assigned in one call
// is visible in the next.  This is the "context as a variable" idea
// extended from files (var_store) to live Python objects.
//
// ── Architecture ─────────────────────────────────────────────────
//
//   PythonSessionManager (owned by MyFrame, like PythonRunner)
//     └── map<normalized conversation cwd, shared_ptr<PySession>>
//           └── PySession: kernel process + Job Object + frame pipes
//                          + stderr crash-channel drain thread
//                          + per-session temp dir (kernel.py, out.log,
//                            err.log)
//
//   The kernel is a small embedded Python script (assets/python/
//   lb_kernel.py, embedded as RCDATA; see python_resources.h).  It speaks a length-prefixed frame protocol on
//   its ORIGINAL stdin/stdout, which it dup()s to private fds at
//   startup before repointing:
//     fd 0 -> NUL              (user code calling input() gets EOFError
//                               instead of eating protocol bytes)
//     fd 1 -> out.log (append) (captures Python-level prints, C
//     fd 2 -> err.log (append)  extensions, and grandchild processes)
//
//   Frames and user output therefore NEVER share a pipe — the design
//   is deadlock-proof rather than deadlock-mitigated.  Because capture
//   goes to files at paths the C++ side knows, a timeout-kill can still
//   SALVAGE partial output by reading the files afterward.
//
// ── Frame protocol (both directions) ─────────────────────────────
//
//     LBPY1 <decimal-byte-length>\n
//     <exactly that many bytes of UTF-8 JSON>
//
//   Requests:  {"id":N,"op":"exec","code":"..."} | {"id":N,"op":"ping"}
//   Responses: {"id":N,"ok":bool,"stdout":...,"stderr":...,
//               "exc_type":...,"exc_message":...,"traceback":...,
//               "duration_ms":N,"stdout_truncated":bool,
//               "stderr_truncated":bool}
//   The kernel emits ensure_ascii JSON, so response payloads are pure
//   ASCII on the wire and immune to codepage mangling.
//
//   REPL semantics: if the last statement of the code is a bare
//   expression, its non-None value is repr()-printed to stdout and
//   bound to `_` — models can end with `total` instead of
//   remembering print(total).
//
// ── Failure semantics (the contract, not an implementation detail) ─
//
//   Timeout / cancel / kernel crash / protocol desync  =>  the session
//   is KILLED (Job Object, whole tree) and the result says so
//   explicitly: stateLost=true plus a state-lost banner in stderr.
//   There is no in-band interrupt and no resync — kill+respawn is the
//   ONLY recovery path, so session state is either fully alive or
//   explicitly gone, never silently corrupted.
//
//   A session found dead BETWEEN calls (kernel exited on its own) is
//   replaced by a fresh one and the requested code still runs there;
//   the result carries sessionDiedEarlier + the prior exit code so the
//   model knows why its variables are gone.
//
// ── Lifetime and event model (mirrors PythonRunner) ──────────────
//   - Owned by MyFrame.  One in-flight exec at a time per frame.
//   - Exec runs on a detached worker thread; completion is posted back
//     through wxEVT_PY_SESSION_COMPLETE guarded by the alive token.
//   - Cancellation is cooperative at the manager level (tick loop) and
//     enforced by killing the kernel's Job Object.
//   - Shutdown() (frame close / app exit) drops every session; the Job
//     Objects' KILL_ON_JOB_CLOSE reaps kernels and any grandchildren.
//
#pragma once

#include <wx/wx.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

wxDECLARE_EVENT(wxEVT_PY_SESSION_COMPLETE, wxCommandEvent);

struct PySessionResult {
    std::string code;            // the snippet that ran (echo for the card)
    std::string commandEcho;     // display echo, e.g. "/py total = 1+1"
    std::string stdoutText;      // captured stdout (UTF-8; includes repr of
                                 //   a trailing bare expression)
    std::string stderrText;      // captured stderr + any banners appended
                                 //   by the manager (state-lost, salvage)
    std::string excType;         // "" when ok
    std::string excMessage;
    std::string traceback;       // user-frame traceback, kernel frame hidden
    std::string pythonCommand;   // launcher label: "py -3" / "python" / ...
    std::string pythonVersion;   // e.g. "3.12.4" (from the spawn ping)

    bool ok        = false;      // exec completed without an exception
    bool timedOut  = false;
    bool cancelled = false;
    bool truncated = false;      // either capture stream hit its cap

    // Session lifecycle flags — the explicit state semantics.
    bool stateLost         = false;  // THIS call killed the session
                                     //   (timeout/cancel/crash/desync)
    bool freshSession      = false;  // a new kernel was spawned for this call
    bool sessionDiedEarlier = false; // the previous kernel was found dead
                                     //   between calls and replaced
    int  priorExitCode     = 0;      // exit code of that dead kernel

    // Set when this call spawned a fresh session because an EARLIER
    // call killed the previous one (timeout/cancel/crash/desync), or
    // it was reset or idle-reaped.  One human-readable line naming the
    // cause ("timed out after 60s at exec #3"), surfaced as a banner
    // so the model learns WHY its variables are gone, not merely that
    // a new session exists.
    std::string restartNote;

    unsigned execCount  = 0;     // 1-based exec counter within the session
    double   elapsedSec = 0.0;   // wall time of the whole exchange
};

class PySessionResultClientData : public wxClientData {
public:
    explicit PySessionResultClientData(PySessionResult r)
        : m_result(std::move(r)) {}
    const PySessionResult& GetResult() const { return m_result; }
private:
    PySessionResult m_result;
};

// Builds the tool-card display parts (body, error body, chips) from a
// session result.  ONE implementation shared by the two card sites —
// AgentController::HandlePySessionComplete and
// ToolResultController::OnPySessionComplete — so the chip vocabulary
// ("ok"/exc-type, "#N", "state lost", "restarted", "new session") can
// never drift between the agent and slash renderings the way the old
// per-helper presentation ladders did.  Deliberately takes out-params
// instead of returning a ToolInvocationResult so this header does not
// depend on tool_dispatcher.h (the session layer sits below the tool
// layer).
void BuildPySessionCardParts(const PySessionResult&    r,
                             std::string&              bodyOut,
                             std::string&              errorBodyOut,
                             std::vector<std::string>& chipsOut);

class PythonSessionManager {
public:
    static constexpr unsigned long kDefaultExecTimeoutMs = 30000;   // API-level
        // fallback used only when a caller passes timeoutMs == 0.  The
        // real caller (the py ToolSpec dispatch) passes the RESOLVED
        // conversation tool timeout — the per-conversation override or
        // the 60s kDefaultToolTimeoutMs.
    static constexpr unsigned long kSpawnPingTimeoutMs   = 10000;   // 10 s
        // for spawn + protocol handshake before trying the next
        // launcher candidate.
    static constexpr size_t kMaxCodeBytes    = 512 * 1024;          // request
        // cap; keeps a single frame write below the pipe buffer so
        // WriteFile can never block indefinitely on a wedged kernel.
    static constexpr size_t kMaxSalvageBytes = 256 * 1024;          // per
        // stream when reading capture files after a kill.
    static constexpr unsigned long long kSessionMemoryCapBytes =
        2ull * 1024 * 1024 * 1024;   // 2 GiB per-process Job Object
        // limit (JOB_OBJECT_LIMIT_PROCESS_MEMORY).  Exceeding it makes
        // allocations FAIL inside the kernel — Python raises a normal
        // MemoryError and the session SURVIVES with state intact —
        // rather than killing the process.  Applies per process in the
        // job, so a runaway grandchild is capped independently.
    static constexpr unsigned long kIdleReapAfterMs = 30 * 60 * 1000;  // 30 min
        // without an exec before ReapIdle kills a session.  Also the
        // fix for the cross-window zombie edge: a session stranded in
        // a frame whose conversation moved to another window ages out
        // here instead of living until app close.

    PythonSessionManager(wxEvtHandler* eventHandler,
                         std::weak_ptr<std::atomic<bool>> aliveToken);
    ~PythonSessionManager();

    // Executes `code` in the persistent session keyed on `cwd` (the
    // conversation workspace — also the kernel process's working
    // directory, so Vars\... relative paths resolve naturally).
    // Spawns the session lazily on first use.  Returns false without
    // side effects when an exec is already in flight or `code`
    // exceeds kMaxCodeBytes (a system message should tell the user).
    // Completion is ALWAYS posted as wxEVT_PY_SESSION_COMPLETE with a
    // PySessionResultClientData payload — including every failure
    // path — guarded by the alive token.
    bool StartExec(const std::string& code,
                   const std::string& cwd,
                   unsigned long      timeoutMs = kDefaultExecTimeoutMs);

    // Cooperative cancel of the in-flight exec.  The worker's tick
    // loop sees the flag, kills the session's Job Object, salvages
    // partial capture output, and posts a cancelled result with
    // stateLost=true.  No-op when nothing is running.
    void Cancel();

    bool IsRunning() const;

    // Kills every session whose last exec finished more than
    // maxIdleMs ago (skipping one currently mid-exec).  Called from a
    // frame timer; silent — the death note surfaces on the next exec
    // for that conversation.  Returns the number reaped.
    size_t ReapIdle(unsigned long maxIdleMs = kIdleReapAfterMs);

    // Kills and forgets the session for one conversation workspace.
    // Safe to call when none exists.  Wired to conversation close
    // (the typed /py reset verb was removed 2026-09-28).  Returns true if
    // a live session was actually torn down.
    bool CloseSessionFor(const std::string& cwd);

    // Kills and forgets every session (frame close / app exit).  Also
    // raises the cancel flag so an in-flight worker finishes promptly;
    // its completion post is absorbed by the alive-token guard when
    // the frame is already gone.
    void Shutdown();

private:
    struct Impl;
    // shared_ptr, not unique_ptr: detached exec workers capture the
    // Impl by shared_ptr, so a worker that outlives the manager (frame
    // closed mid-exec) keeps the mutex/session-map/flags alive until
    // it finishes.  Its final completion post is absorbed by the
    // alive-token guard.
    std::shared_ptr<Impl> m_impl;
};
