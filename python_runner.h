// python_runner.h
//
// Controlled Python backend foundation for LlamaBoss.
//
// This runner intentionally does NOT execute arbitrary model/user code.
// It starts only named, built-in helper flows.  The foundation started
// with python_health, a first read-only data helper (csv_inspect),
// a first write-capable report helper (csv_report), and a text-based
// PDF extractor (pdf_extract_text).  It now also supports
// approval-gated execution of reviewable scripts created under the
// fixed LlamaBoss Scripts lane.  Helpers
// write/refresh small bundled scripts under the
// LlamaBoss Scripts lane and run them in isolated Python mode.
//
// Lifetime and event model mirrors CmdExecutor:
//   - PythonRunner is owned by MyFrame.
//   - Worker threads are detached wxThreads.
//   - Completion is posted back through wxEVT_PYTHON_COMPLETE.
//   - Cancellation is cooperative at the runner level and enforced by
//     killing the child process tree through a Windows Job Object.
//
#pragma once

#include <wx/wx.h>
#include <wx/thread.h>

#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "presented_file.h"

wxDECLARE_EVENT(wxEVT_PYTHON_COMPLETE, wxCommandEvent);
wxDECLARE_EVENT(wxEVT_PYTHON_ERROR,    wxCommandEvent);

struct PythonRunResult {
    std::string toolName;          // protocol tool name, e.g. python_health
    std::string helperName;        // built-in helper id, e.g. python_health
    std::string commandEcho;       // display echo for the tool card
    std::string stdoutText;        // captured stdout, post-cap (UTF-8)
    std::string stderrText;        // captured stderr, post-cap (UTF-8)
    std::string pythonCommand;     // launcher command used: py -3 / python / python3
    int         exitCode   = -1;
    double      elapsedSec = 0.0;
    bool        timedOut   = false;
    bool        truncated  = false;
    bool        cancelled  = false;

    // Future artifact-producing Python helpers surface created files
    // through the same artifact-card path used by /write.
    std::vector<PresentedFile> presentedFiles;
};

class PythonRunResultClientData : public wxClientData {
public:
    explicit PythonRunResultClientData(PythonRunResult r)
        : m_result(std::move(r)) {}
    const PythonRunResult& GetResult() const { return m_result; }
private:
    PythonRunResult m_result;
};

class PythonRunner {
public:
    static constexpr unsigned long kDefaultTimeoutMs = 15000;  // 15s
    static constexpr size_t        kMaxOutputBytes   = 4 * 1024 * 1024;  // 4 MiB per stream; display is previewed if large

    PythonRunner(wxEvtHandler* eventHandler,
                 std::weak_ptr<std::atomic<bool>> aliveToken);
    ~PythonRunner();

    // Starts the fixed python_health helper.  `cwd` is the working
    // directory for the helper process; empty falls back to the
    // LlamaBoss workspace.  This helper accepts no model-provided
    // script path or code string.
    bool StartHealth(const std::string& cwd,
                     unsigned long      timeoutMs = kDefaultTimeoutMs);

    // Starts the fixed csv_inspect helper.  `pathArg` is a data file
    // path supplied as data, not code.  The helper enforces that the
    // resolved file stays inside `cwd` and accepts only .csv/.tsv
    // files.  It is read-only and returns a JSON summary.
    bool StartCsvInspect(const std::string& pathArg,
                         const std::string& cwd,
                         unsigned long      timeoutMs = kDefaultTimeoutMs);

    // Starts the fixed csv_report helper.  It reads one .csv/.tsv file
    // inside `cwd` and writes a Markdown report to the LlamaBoss
    // Documents lane.  No arbitrary output path, script path, or code
    // string is accepted.
    bool StartCsvReport(const std::string& pathArg,
                        const std::string& cwd,
                        unsigned long      timeoutMs = kDefaultTimeoutMs);


    // Starts the fixed csv_to_xlsx helper.  It reads one .csv/.tsv file
    // inside `cwd` and writes an .xlsx workbook to the conversation
    // Spreadsheets lane.  No arbitrary output path, script path, or
    // code string is accepted.  Requires the openpyxl Python package
    // on the system Python.
    bool StartCsvToXlsx(const std::string& pathArg,
                        const std::string& cwd,
                        unsigned long      timeoutMs = kDefaultTimeoutMs);

    // Starts the fixed xlsx_inspect helper.  It reads one .xlsx file
    // inside `cwd` and returns a JSON summary covering every sheet
    // (sheet names, dimensions, column headers, sample rows).  Read-
    // only.  Requires the openpyxl Python package on the system Python;
    // the helper reports a clear error if openpyxl is not available.
    bool StartXlsxInspect(const std::string& pathArg,
                          const std::string& cwd,
                          unsigned long      timeoutMs = kDefaultTimeoutMs);

    // Starts the fixed xlsx_report helper.  It reads one .xlsx file
    // inside `cwd` and writes a Markdown report covering every sheet
    // to the LlamaBoss Documents lane.  No arbitrary output path,
    // script path, or code string is accepted.  Requires the openpyxl
    // Python package on the system Python.
    bool StartXlsxReport(const std::string& pathArg,
                         const std::string& cwd,
                         unsigned long      timeoutMs = kDefaultTimeoutMs);

    // Starts the fixed pdf_extract_text helper.  It reads one text-based
    // .pdf file inside `cwd` and writes extracted text as Markdown to
    // the LlamaBoss PDFs lane.  No OCR, PDF editing, output path, script
    // path, or code string is accepted.
    bool StartPdfExtractText(const std::string& pathArg,
                             const std::string& cwd,
                             unsigned long      timeoutMs = kDefaultTimeoutMs);

    // Starts the fixed pdf_inspect_form helper.  It reads AcroForm
    // metadata from one .pdf file inside `cwd` and emits a JSON
    // summary of every form field (name, type, current value,
    // options where applicable, page, required flag, tooltip).
    // Read-only, no file output, no approval gate.  Refuses XFA-only
    // PDFs with a model-readable explanation.  Requires the PyMuPDF
    // (`pymupdf` / `fitz`) Python package on the system Python.
    bool StartPdfInspectForm(const std::string& pathArg,
                             const std::string& cwd,
                             unsigned long      timeoutMs = kDefaultTimeoutMs);

    // Starts the fixed pdf_fill_form helper.  It fills AcroForm fields
    // in one .pdf file inside `cwd` from a JSON {field: value} map,
    // writes the filled PDF to the LlamaBoss Filled Forms folder, and
    // returns a JSON summary plus an artifact card.  argsBlob is the
    // raw multi-line tool argument: first line is the input PDF path,
    // remaining lines are a JSON object mapping field names to values.
    // Mutating, approval-gated.  Refuses XFA-only PDFs and rejects the
    // entire call (no partial fills) if any field name does not exist
    // or any value is invalid for its field type.  Requires the
    // PyMuPDF (`pymupdf`) Python package on the system Python.
    bool StartPdfFillForm(const std::string& argsBlob,
                          const std::string& cwd,
                          unsigned long      timeoutMs = kDefaultTimeoutMs);

    // Starts the fixed docx_extract_text helper.  It reads one .docx
    // (or .docm) file inside `cwd` and writes extracted text as
    // Markdown to the LlamaBoss Word lane.  Heading styles map to
    // Markdown heading levels; bullet/numbered lists render as -/1.;
    // tables render as Markdown tables.  No output path, script path,
    // or code string is accepted.  Requires the python-docx package
    // (`pip install python-docx`) on the system Python.
    bool StartDocxExtractText(const std::string& pathArg,
                              const std::string& cwd,
                              unsigned long      timeoutMs = kDefaultTimeoutMs);

    // Starts the fixed docx_inspect helper.  It reads one .docx (or
    // .docm) file inside `cwd` and returns a JSON summary: paragraph
    // count, heading list (with levels), table list (with row/column
    // counts), section count, styles in use.  Read-only, no file
    // output, no approval gate.  Requires the python-docx package on
    // the system Python.
    bool StartDocxInspect(const std::string& pathArg,
                          const std::string& cwd,
                          unsigned long      timeoutMs = kDefaultTimeoutMs);

    // Runs one existing .py script from the fixed conversation Scripts lane,
    // or (when a project is active) from that project's Workflows lane.
    // The script reference is data, not a shell command: filename only, no
    // paths, no command-line arguments in this first phase. The process
    // runs with the current LlamaBoss workspace as its cwd, captures
    // stdout/stderr/exit code/runtime, supports timeout/cancel through
    // the same job-object path, and reports newly-created files under
    // the LlamaBoss root as artifact cards when practical.
    bool StartPythonRunScript(const std::string& scriptArg,
                              const std::string& cwd,
                              unsigned long      timeoutMs = 30000,
                              const std::string& activeProjectRoot = std::string());

    // Installs one allowlisted Python package into the user's Python
    // user-site with `py -3 -m pip install --user <package>` (falling
    // back to python/python3 launchers if needed).  This is a deliberate
    // approval-gated environment mutation, not a general shell escape:
    // the package name is normalized against a fixed allowlist and no
    // version specifiers, paths, requirements files, extra pip flags, or
    // arbitrary command-line arguments are accepted.
    bool StartPythonInstallPackage(const std::string& packageArg,
                                   const std::string& cwd,
                                   unsigned long      timeoutMs = 300000);

    void Cancel();

    bool IsRunning() const {
        return m_isRunning && m_isRunning->load();
    }

private:
    wxEvtHandler*                      m_eventHandler;
    std::weak_ptr<std::atomic<bool>>   m_aliveToken;
    std::shared_ptr<std::atomic<bool>> m_cancelFlag;
    std::shared_ptr<std::atomic<bool>> m_isRunning;
};
