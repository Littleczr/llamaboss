#define _CRT_SECURE_NO_WARNINGS

// python_runner.cpp
//
// Controlled Python backend foundation.  See python_runner.h for the
// user-facing safety contract.  This intentionally mirrors CmdExecutor's
// Windows process model but narrows the command surface to fixed helpers.

#include "python_runner.h"

#include "path_safety.h"
#include "project_manager.h"
#include "server_manager.h"

#include <wx/filename.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#define NOMINMAX
#include <windows.h>

wxDEFINE_EVENT(wxEVT_PYTHON_COMPLETE, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_PYTHON_ERROR,    wxCommandEvent);

namespace {

std::wstring Utf8ToWide(const std::string& s)
{
    return path_safety::Utf8ToWide(s);
}

std::string WideToUtf8(const std::wstring& s)
{
    return path_safety::WideToUtf8(s);
}

double NowSec()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string JoinPath(const std::string& a, const std::string& b)
{
    if (a.empty()) return b;
    const char sep = wxFILE_SEP_PATH;
    if (a.back() == '/' || a.back() == '\\') return a + b;
    return a + std::string(1, sep) + b;
}

std::string ParentDirOf(const std::string& path)
{
    std::string s = path;
    while (!s.empty() && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    size_t pos = s.find_last_of("/\\");
    if (pos == std::string::npos) return std::string();
    return s.substr(0, pos);
}

std::string LowerLocal(std::string s);

std::string ScriptsDir()
{
    // Default layout:
    //   %USERPROFILE%\LlamaBoss\Workspace
    //   %USERPROFILE%\LlamaBoss\Scripts
    // Even if the user overrides the active workspace, built-in helper
    // scripts stay in the app's Scripts lane, not inside arbitrary user
    // folders.
    std::string root = ParentDirOf(ServerManager::GetDefaultWorkspaceDir());
    if (root.empty()) root = ParentDirOf(ServerManager::GetWorkspaceDir());
    return JoinPath(root, "Scripts");
}

std::string LlamaBossRootDir()
{
    std::string root = ParentDirOf(ServerManager::GetDefaultWorkspaceDir());
    if (root.empty()) root = ParentDirOf(ServerManager::GetWorkspaceDir());
    return root;
}

std::string TrimTrailingSeparators(std::string s)
{
    while (!s.empty() && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    return s;
}

std::string PathBaseNameLocal(const std::string& path)
{
    std::string s = TrimTrailingSeparators(path);
    size_t pos = s.find_last_of("/\\");
    return (pos == std::string::npos) ? s : s.substr(pos + 1);
}

bool StartsWithLocal(const std::string& s, const std::string& prefix)
{
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

std::string WorkflowRootFromCwd(const std::string& cwd)
{
    // Recognize the per-conversation folder layout created by
    // ChatHistory::EnsureWorkflowDir():
    //   %USERPROFILE%\LlamaBoss\Workflows\chat_xxxxxxxx\Workspace
    std::string clean = TrimTrailingSeparators(cwd);
    if (clean.empty()) return std::string();

    std::string base = PathBaseNameLocal(clean);
    if (LowerLocal(base) != "workspace") return std::string();

    std::string parent = ParentDirOf(clean);        // ...\chat_xxxxxxxx
    std::string workflows = ParentDirOf(parent);    // ...\Workflows
    if (parent.empty() || workflows.empty()) return std::string();

    if (!StartsWithLocal(PathBaseNameLocal(parent), "chat_")) return std::string();
    if (LowerLocal(PathBaseNameLocal(workflows)) != "workflows") return std::string();

    return parent;
}

std::string LaneDirForCwd(const std::string& cwd, const std::string& lane)
{
    std::string workflowRoot = WorkflowRootFromCwd(cwd);
    std::string root = workflowRoot.empty() ? LlamaBossRootDir() : workflowRoot;
    return JoinPath(root, lane);
}

std::string DocumentsDirForCwd(const std::string& cwd)
{
    return LaneDirForCwd(cwd, "Documents");
}

std::string SpreadsheetsDirForCwd(const std::string& cwd)
{
    return LaneDirForCwd(cwd, "Spreadsheets");
}

std::string PdfsDirForCwd(const std::string& cwd)
{
    return LaneDirForCwd(cwd, "PDFs");
}

std::string WordDirForCwd(const std::string& cwd)
{
    return LaneDirForCwd(cwd, "Word");
}

std::string FilledFormsDirForCwd(const std::string& cwd)
{
    return LaneDirForCwd(cwd, "Filled Forms");
}

std::string UserScriptsDirForCwd(const std::string& cwd)
{
    return LaneDirForCwd(cwd, "Scripts");
}

std::string ScanRootForCwd(const std::string& cwd)
{
    std::string workflowRoot = WorkflowRootFromCwd(cwd);
    return workflowRoot.empty() ? LlamaBossRootDir() : workflowRoot;
}

bool FileExistsRegular(const std::string& path)
{
    DWORD attrs = GetFileAttributesW(Utf8ToWide(path).c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::string LowerLocal(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string ExtensionLower(const std::string& path)
{
    size_t slash = path.find_last_of("/\\");
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    if (dot == std::string::npos) return std::string();
    return LowerLocal(base.substr(dot));
}

std::string LanguageForFile(const std::string& path)
{
    std::string ext = ExtensionLower(path);
    if (ext == ".py") return "python";
    if (ext == ".md" || ext == ".markdown") return "markdown";
    if (ext == ".txt" || ext == ".log") return "text";
    if (ext == ".csv") return "csv";
    if (ext == ".tsv") return "tsv";
    if (ext == ".json") return "json";
    if (ext == ".html" || ext == ".htm") return "html";
    if (ext == ".css") return "css";
    if (ext == ".cpp" || ext == ".cc" || ext == ".cxx") return "cpp";
    if (ext == ".h" || ext == ".hpp") return "cpp";
    return std::string();
}

std::string HelperScriptPath(const std::string& helperName)
{
    return JoinPath(ScriptsDir(), helperName + ".py");
}

bool WriteHelperScript(const std::string& helperName,
                       const char*        script,
                       std::string&       scriptPathOut,
                       std::string&       errorOut)
{
    scriptPathOut = HelperScriptPath(helperName);

    // Helper scripts are constant string literals — they never change
    // within a process.  Pre-fix this function truncated and rewrote
    // the .py file on every helper invocation, which churned the disk
    // for no benefit (and opened a small "partial script on disk
    // during write" window every call).  Track which helpers have
    // already been written this process and short-circuit subsequent
    // calls.  A different app launch will re-write — that's fine, the
    // scripts are tiny and one write per launch is acceptable.
    {
        static std::mutex            s_writtenMutex;
        static std::set<std::string> s_writtenHelpers;
        std::lock_guard<std::mutex> lock(s_writtenMutex);
        if (s_writtenHelpers.count(helperName) > 0) {
            return true;   // already on disk for this process
        }

        // Hold the lock across the actual write.  Helper invocations
        // are infrequent and the writes are tiny (kilobytes), so the
        // serialization cost is negligible.  Holding the lock prevents
        // two threads from racing to write the same script in the
        // unlikely case of concurrent helper kicks.

        wxFileName::Mkdir(wxString::FromUTF8(ScriptsDir().c_str()),
                          wxS_DIR_DEFAULT,
                          wxPATH_MKDIR_FULL);

        try {
            std::ofstream f(path_safety::Utf8ToWide(scriptPathOut),
                            std::ios::binary | std::ios::trunc);
            if (!f) {
                errorOut = "Could not write helper script: " + scriptPathOut;
                return false;
            }
            f.write(script, static_cast<std::streamsize>(std::strlen(script)));
            if (!f.good()) {
                errorOut = "Failed while writing helper script: " + scriptPathOut;
                return false;
            }
        } catch (...) {
            errorOut = "Exception while writing helper script: " + scriptPathOut;
            return false;
        }

        s_writtenHelpers.insert(helperName);
    }

    return true;
}

bool EnsurePythonHealthScript(std::string& scriptPathOut,
                              std::string& errorOut)
{
    const char* script =
        "import json\n"
        "import os\n"
        "import platform\n"
        "import sys\n"
        "\n"
        "data = {\n"
        "    'ok': True,\n"
        "    'helper': 'python_health',\n"
        "    'python_version': sys.version.split()[0],\n"
        "    'python_executable': sys.executable,\n"
        "    'cwd': os.getcwd(),\n"
        "    'platform': platform.platform(),\n"
        "}\n"
        "print(json.dumps(data, indent=2))\n";

    return WriteHelperScript("python_health", script, scriptPathOut, errorOut);
}

bool EnsureCsvInspectScript(std::string& scriptPathOut,
                            std::string& errorOut)
{
    const char* script =
        "import csv\n"
        "import json\n"
        "import sys\n"
        "from pathlib import Path\n"
        "\n"
        "def emit(obj, code=0):\n"
        "    print(json.dumps(obj, indent=2, ensure_ascii=False))\n"
        "    raise SystemExit(code)\n"
        "\n"
        "def is_relative_to(child, parent):\n"
        "    try:\n"
        "        child.relative_to(parent)\n"
        "        return True\n"
        "    except Exception:\n"
        "        return False\n"
        "\n"
        "def decode_text(path):\n"
        "    raw = path.read_bytes()\n"
        "    for enc in ('utf-8-sig', 'utf-8', 'cp1252'):\n"
        "        try:\n"
        "            return raw.decode(enc), enc, len(raw)\n"
        "        except UnicodeDecodeError:\n"
        "            pass\n"
        "    return raw.decode('utf-8', errors='replace'), 'utf-8-replace', len(raw)\n"
        "\n"
        "def main():\n"
        "    if len(sys.argv) != 2:\n"
        "        emit({'ok': False, 'helper': 'csv_inspect', 'error': 'csv_inspect expects exactly one file path argument'}, 2)\n"
        "\n"
        "    cwd = Path.cwd().resolve()\n"
        "    raw_arg = sys.argv[1]\n"
        "    target = Path(raw_arg)\n"
        "    if not target.is_absolute():\n"
        "        target = cwd / target\n"
        "    target = target.resolve()\n"
        "\n"
        "\n"
        "    if target.suffix.lower() not in ('.csv', '.tsv'):\n"
        "        emit({'ok': False, 'helper': 'csv_inspect', 'error': 'Only .csv and .tsv files are supported in this first data helper', 'path': str(target)}, 4)\n"
        "\n"
        "    if not target.exists() or not target.is_file():\n"
        "        emit({'ok': False, 'helper': 'csv_inspect', 'error': 'File not found', 'path': str(target)}, 5)\n"
        "\n"
        "    max_file_bytes = 10 * 1024 * 1024\n"
        "    if target.stat().st_size > max_file_bytes:\n"
        "        emit({'ok': False, 'helper': 'csv_inspect', 'error': 'File is larger than the first-phase 10 MB safety cap', 'path': str(target), 'file_size_bytes': target.stat().st_size}, 7)\n"
        "\n"
        "    text, encoding, size_bytes = decode_text(target)\n"
        "    sample_text = text[:65536]\n"
        "    delimiter = '\\t' if target.suffix.lower() == '.tsv' else ','\n"
        "    dialect_name = 'default-tsv' if delimiter == '\\t' else 'default-csv'\n"
        "    warnings = []\n"
        "\n"
        "    try:\n"
        "        sniffed = csv.Sniffer().sniff(sample_text, delimiters=',\\t;|')\n"
        "        delimiter = sniffed.delimiter\n"
        "        dialect_name = 'sniffed'\n"
        "    except Exception:\n"
        "        warnings.append('Could not confidently sniff delimiter; used extension/default delimiter.')\n"
        "\n"
        "    rows = []\n"
        "    row_count = 0\n"
        "    max_rows_to_scan = 100000\n"
        "    try:\n"
        "        reader = csv.reader(text.splitlines(), delimiter=delimiter)\n"
        "        for row in reader:\n"
        "            row_count += 1\n"
        "            if len(rows) < 6:\n"
        "                rows.append(row)\n"
        "            if row_count >= max_rows_to_scan:\n"
        "                warnings.append('Stopped counting at 100000 rows.')\n"
        "                break\n"
        "    except Exception as ex:\n"
        "        emit({'ok': False, 'helper': 'csv_inspect', 'error': 'CSV parse failed: ' + str(ex), 'path': str(target)}, 6)\n"
        "\n"
        "    header = rows[0] if rows else []\n"
        "    sample_rows = rows[1:6] if len(rows) > 1 else []\n"
        "    width = max((len(r) for r in rows), default=0)\n"
        "    ragged = any(len(r) != width for r in rows) if rows else False\n"
        "    if ragged:\n"
        "        warnings.append('Sample rows have inconsistent column counts.')\n"
        "\n"
        "    emit({\n"
        "        'ok': True,\n"
        "        'helper': 'csv_inspect',\n"
        "        'path': str(target),\n"
        "        'cwd': str(cwd),\n"
        "        'file_size_bytes': size_bytes,\n"
        "        'encoding': encoding,\n"
        "        'delimiter': delimiter,\n"
        "        'dialect': dialect_name,\n"
        "        'rows_scanned': row_count,\n"
        "        'row_count_exact': row_count < max_rows_to_scan,\n"
        "        'column_count_sample': width,\n"
        "        'columns': header,\n"
        "        'sample_rows': sample_rows,\n"
        "        'warnings': warnings,\n"
        "    })\n"
        "\n"
        "if __name__ == '__main__':\n"
        "    main()\n";

    return WriteHelperScript("csv_inspect", script, scriptPathOut, errorOut);
}

bool EnsureCsvReportScript(std::string& scriptPathOut,
                           std::string& errorOut)
{
    const char* script = R"PY(
import csv
import json
import re
import sys
from pathlib import Path


def emit(obj, code=0):
    print(json.dumps(obj, indent=2, ensure_ascii=False))
    raise SystemExit(code)


def is_relative_to(child, parent):
    try:
        child.relative_to(parent)
        return True
    except Exception:
        return False


def decode_text(path):
    raw = path.read_bytes()
    for enc in ('utf-8-sig', 'utf-8', 'cp1252'):
        try:
            return raw.decode(enc), enc, len(raw)
        except UnicodeDecodeError:
            pass
    return raw.decode('utf-8', errors='replace'), 'utf-8-replace', len(raw)


def safe_name(stem):
    cleaned = re.sub(r'[^A-Za-z0-9._-]+', '_', stem).strip('._-')
    return cleaned or 'csv_report'


def unique_path(folder, base_name):
    candidate = folder / base_name
    if not candidate.exists():
        return candidate
    stem = candidate.stem
    suffix = candidate.suffix
    for i in range(2, 1000):
        p = folder / f"{stem}_{i}{suffix}"
        if not p.exists():
            return p
    return folder / f"{stem}_latest{suffix}"


def md_cell(value):
    s = '' if value is None else str(value)
    s = s.replace('\r', ' ').replace('\n', ' ')
    s = s.replace('|', '\\|')
    if len(s) > 80:
        s = s[:77] + '...'
    return s


def md_table(headers, rows):
    out = []
    out.append('| ' + ' | '.join(md_cell(h) for h in headers) + ' |')
    out.append('| ' + ' | '.join('---' for _ in headers) + ' |')
    for row in rows:
        out.append('| ' + ' | '.join(md_cell(v) for v in row) + ' |')
    return '\n'.join(out)


def as_float(value):
    if value is None:
        return None
    s = str(value).strip()
    if not s:
        return None
    try:
        return float(s.replace(',', ''))
    except Exception:
        return None


def main():
    if len(sys.argv) != 3:
        emit({'ok': False, 'helper': 'csv_report', 'error': 'csv_report expects exactly two internal arguments: input path and fixed output directory'}, 2)

    cwd = Path.cwd().resolve()
    raw_arg = sys.argv[1]
    docs_dir = Path(sys.argv[2]).resolve()

    target = Path(raw_arg)
    if not target.is_absolute():
        target = cwd / target
    target = target.resolve()


    if target.suffix.lower() not in ('.csv', '.tsv'):
        emit({'ok': False, 'helper': 'csv_report', 'error': 'Only .csv and .tsv files are supported by csv_report', 'path': str(target)}, 4)

    if not target.exists() or not target.is_file():
        emit({'ok': False, 'helper': 'csv_report', 'error': 'File not found', 'path': str(target)}, 5)

    max_file_bytes = 10 * 1024 * 1024
    if target.stat().st_size > max_file_bytes:
        emit({'ok': False, 'helper': 'csv_report', 'error': 'File is larger than the first-phase 10 MB safety cap', 'path': str(target), 'file_size_bytes': target.stat().st_size}, 7)

    text, encoding, size_bytes = decode_text(target)
    sample_text = text[:65536]
    delimiter = '\t' if target.suffix.lower() == '.tsv' else ','
    dialect_name = 'default-tsv' if delimiter == '\t' else 'default-csv'
    warnings = []

    try:
        sniffed = csv.Sniffer().sniff(sample_text, delimiters=',\t;|')
        delimiter = sniffed.delimiter
        dialect_name = 'sniffed'
    except Exception:
        warnings.append('Could not confidently sniff delimiter; used extension/default delimiter.')

    rows = []
    max_rows_to_scan = 100000
    try:
        reader = csv.reader(text.splitlines(), delimiter=delimiter)
        for row in reader:
            rows.append(row)
            if len(rows) >= max_rows_to_scan:
                warnings.append('Stopped scanning at 100000 rows.')
                break
    except Exception as ex:
        emit({'ok': False, 'helper': 'csv_report', 'error': 'CSV parse failed: ' + str(ex), 'path': str(target)}, 6)

    header = rows[0] if rows else []
    data_rows = rows[1:] if len(rows) > 1 else []
    width = max((len(r) for r in rows), default=0)
    if rows and any(len(r) != width for r in rows[:200]):
        warnings.append('Some sampled rows have inconsistent column counts.')

    columns = []
    for i in range(width):
        name = header[i].strip() if i < len(header) and str(header[i]).strip() else f'column_{i + 1}'
        columns.append(name)

    missing_counts = [0 for _ in range(width)]
    nonempty_counts = [0 for _ in range(width)]
    numeric_values = [[] for _ in range(width)]

    for row in data_rows:
        for i in range(width):
            value = row[i] if i < len(row) else ''
            if str(value).strip() == '':
                missing_counts[i] += 1
                continue
            nonempty_counts[i] += 1
            f = as_float(value)
            if f is not None:
                numeric_values[i].append(f)

    numeric_summary = []
    for i, vals in enumerate(numeric_values):
        if not vals:
            continue
        denom = max(1, nonempty_counts[i])
        if len(vals) / denom < 0.80:
            continue
        total = sum(vals)
        numeric_summary.append({
            'column': columns[i],
            'count': len(vals),
            'sum': round(total, 6),
            'min': round(min(vals), 6),
            'max': round(max(vals), 6),
            'average': round(total / len(vals), 6),
        })

    docs_dir.mkdir(parents=True, exist_ok=True)
    report_name = safe_name(target.stem) + '_report.md'
    output_path = unique_path(docs_dir, report_name)

    report = []
    report.append(f'# CSV Report: {target.name}')
    report.append('')
    report.append('Generated by LlamaBoss controlled Python backend (`csv_report`).')
    report.append('')
    report.append('## Summary')
    report.append('')
    report.append(md_table(['Metric', 'Value'], [
        ['Input file', str(target)],
        ['Output report', str(output_path)],
        ['Encoding', encoding],
        ['Delimiter', '\\t' if delimiter == '\t' else delimiter],
        ['Dialect', dialect_name],
        ['File size', f'{size_bytes} bytes'],
        ['Rows scanned', len(rows)],
        ['Data rows', len(data_rows)],
        ['Columns', width],
    ]))
    report.append('')
    report.append('## Columns')
    report.append('')
    column_rows = []
    for i, name in enumerate(columns):
        column_rows.append([i + 1, name, nonempty_counts[i], missing_counts[i]])
    report.append(md_table(['#', 'Column', 'Non-empty', 'Missing/blank'], column_rows))
    report.append('')

    if numeric_summary:
        report.append('## Numeric Summary')
        report.append('')
        report.append(md_table(['Column', 'Count', 'Sum', 'Min', 'Max', 'Average'], [
            [n['column'], n['count'], n['sum'], n['min'], n['max'], n['average']]
            for n in numeric_summary
        ]))
        report.append('')

    report.append('## Sample Rows')
    report.append('')
    sample = data_rows[:10]
    if sample and columns:
        padded = []
        for row in sample:
            padded.append([(row[i] if i < len(row) else '') for i in range(width)])
        report.append(md_table(columns, padded))
    else:
        report.append('_No data rows found._')
    report.append('')

    report.append('## Warnings')
    report.append('')
    if warnings:
        for w in warnings:
            report.append(f'- {w}')
    else:
        report.append('- None')
    report.append('')

    output_text = '\n'.join(report)
    output_path.write_text(output_text, encoding='utf-8')

    emit({
        'ok': True,
        'helper': 'csv_report',
        'input_path': str(target),
        'output_path': str(output_path),
        'output_filename': output_path.name,
        'cwd': str(cwd),
        'documents_dir': str(docs_dir),
        'file_size_bytes': size_bytes,
        'encoding': encoding,
        'delimiter': delimiter,
        'dialect': dialect_name,
        'rows_scanned': len(rows),
        'row_count_exact': len(rows) < max_rows_to_scan,
        'data_row_count': len(data_rows),
        'column_count': width,
        'columns': columns,
        'numeric_columns': numeric_summary,
        'warnings': warnings,
    })


if __name__ == '__main__':
    main()
)PY";

    return WriteHelperScript("csv_report", script, scriptPathOut, errorOut);
}


bool EnsureCsvToXlsxScript(std::string& scriptPathOut,
                           std::string& errorOut)
{
    const char* script = R"PY(
import csv
import json
import re
import site
import sys
import zipfile
import xml.etree.ElementTree as ET
from pathlib import Path


def emit(obj, code=0):
    print(json.dumps(obj, indent=2, ensure_ascii=False))
    raise SystemExit(code)


def load_openpyxl():
    errors = []

    def try_import():
        try:
            from openpyxl import Workbook
            from openpyxl.styles import Font
            from openpyxl.utils import get_column_letter
            return Workbook, Font, get_column_letter
        except Exception as ex:
            errors.append('openpyxl: ' + str(ex))
            return None, None, None

    Workbook, Font, get_column_letter = try_import()
    if Workbook is not None:
        return Workbook, Font, get_column_letter, errors

    # PythonRunner launches fixed helpers with -I. Re-add the user site
    # so `py -3 -m pip install --user openpyxl` works during local testing.
    try:
        user_site = getattr(site, 'USER_SITE', None)
        if user_site:
            site.addsitedir(user_site)
    except Exception as ex:
        errors.append('user-site enable failed: ' + str(ex))

    Workbook, Font, get_column_letter = try_import()
    return Workbook, Font, get_column_letter, errors


def is_relative_to(child, parent):
    try:
        child.relative_to(parent)
        return True
    except Exception:
        return False


def decode_text(path):
    raw = path.read_bytes()
    for enc in ('utf-8-sig', 'utf-8', 'cp1252'):
        try:
            return raw.decode(enc), enc, len(raw)
        except UnicodeDecodeError:
            pass
    return raw.decode('utf-8', errors='replace'), 'utf-8-replace', len(raw)


def safe_name(stem):
    cleaned = re.sub(r'[^A-Za-z0-9._-]+', '_', stem).strip('._-')
    return cleaned or 'workbook'


def unique_path(folder, base_name):
    candidate = folder / base_name
    if not candidate.exists():
        return candidate
    stem = candidate.stem
    suffix = candidate.suffix
    for i in range(2, 1000):
        p = folder / f"{stem}_{i}{suffix}"
        if not p.exists():
            return p
    return folder / f"{stem}_latest{suffix}"


def maybe_number(value):
    s = str(value).strip()
    if not s:
        return ''

    # Keep leading-zero identifiers as text.
    plain = s.replace(',', '')
    if re.fullmatch(r'[+-]?\d+', plain):
        digits = plain[1:] if plain[:1] in '+-' else plain
        if len(digits) > 1 and digits.startswith('0'):
            return s
        try:
            return int(plain)
        except Exception:
            return s

    if re.fullmatch(r'[+-]?(\d+\.\d*|\.\d+)([eE][+-]?\d+)?', plain) or re.fullmatch(r'[+-]?\d+[eE][+-]?\d+', plain):
        try:
            return float(plain)
        except Exception:
            return s

    return s


def main():
    if len(sys.argv) != 3:
        emit({'ok': False, 'helper': 'csv_to_xlsx', 'error': 'csv_to_xlsx expects exactly two internal arguments: input path and fixed output directory'}, 2)

    cwd = Path.cwd().resolve()
    raw_arg = sys.argv[1]
    out_dir = Path(sys.argv[2]).resolve()

    target = Path(raw_arg)
    if not target.is_absolute():
        target = cwd / target
    target = target.resolve()


    if target.suffix.lower() not in ('.csv', '.tsv'):
        emit({'ok': False, 'helper': 'csv_to_xlsx', 'error': 'Only .csv and .tsv files are supported by csv_to_xlsx', 'path': str(target)}, 4)

    if not target.exists() or not target.is_file():
        emit({'ok': False, 'helper': 'csv_to_xlsx', 'error': 'File not found', 'path': str(target)}, 5)

    max_file_bytes = 10 * 1024 * 1024
    size_bytes = target.stat().st_size
    if size_bytes > max_file_bytes:
        emit({'ok': False, 'helper': 'csv_to_xlsx', 'error': 'File is larger than the first-phase 10 MB safety cap', 'path': str(target), 'file_size_bytes': size_bytes}, 7)

    Workbook, Font, get_column_letter, import_errors = load_openpyxl()
    if Workbook is None:
        emit({
            'ok': False,
            'helper': 'csv_to_xlsx',
            'error': 'The openpyxl Python package is required for csv_to_xlsx. Install it with: py -3 -m pip install --user openpyxl',
            'details': import_errors[-6:],
        }, 8)

    text, encoding, _ = decode_text(target)
    sample_text = text[:65536]
    delimiter = '\t' if target.suffix.lower() == '.tsv' else ','
    dialect_name = 'default-tsv' if delimiter == '\t' else 'default-csv'
    warnings = []

    try:
        sniffed = csv.Sniffer().sniff(sample_text, delimiters=',\t;|')
        delimiter = sniffed.delimiter
        dialect_name = 'sniffed'
    except Exception:
        warnings.append('Could not confidently sniff delimiter; used extension/default delimiter.')

    rows = []
    max_rows = 100000
    max_excel_cols = 16384
    try:
        reader = csv.reader(text.splitlines(), delimiter=delimiter)
        for row in reader:
            if len(row) > max_excel_cols:
                emit({'ok': False, 'helper': 'csv_to_xlsx', 'error': 'CSV has more columns than Excel supports', 'columns': len(row), 'excel_max_columns': max_excel_cols}, 9)
            rows.append(row)
            if len(rows) >= max_rows:
                warnings.append('Stopped writing after 100000 rows.')
                break
    except Exception as ex:
        emit({'ok': False, 'helper': 'csv_to_xlsx', 'error': 'CSV parse failed: ' + str(ex), 'path': str(target)}, 6)

    out_dir.mkdir(parents=True, exist_ok=True)
    output_path = unique_path(out_dir, safe_name(target.stem) + '.xlsx')

    wb = Workbook()
    ws = wb.active
    ws.title = 'Data'

    max_widths = []
    for r_idx, row in enumerate(rows, start=1):
        for c_idx, value in enumerate(row, start=1):
            converted = maybe_number(value)
            ws.cell(row=r_idx, column=c_idx, value=converted)
            text_len = len(str(value)) if value is not None else 0
            if c_idx > len(max_widths):
                max_widths.append(text_len)
            else:
                max_widths[c_idx - 1] = max(max_widths[c_idx - 1], text_len)

    if rows:
        for cell in ws[1]:
            cell.font = Font(bold=True)
        ws.freeze_panes = 'A2'
        if len(rows) > 1 and max_widths:
            ws.auto_filter.ref = ws.dimensions

    for idx, width in enumerate(max_widths, start=1):
        ws.column_dimensions[get_column_letter(idx)].width = min(max(width + 2, 10), 40)

    try:
        wb.save(output_path)
    except Exception as ex:
        emit({'ok': False, 'helper': 'csv_to_xlsx', 'error': 'Could not save workbook: ' + str(ex), 'output_path': str(output_path)}, 10)

    emit({
        'ok': True,
        'helper': 'csv_to_xlsx',
        'input_path': str(target),
        'output_path': str(output_path),
        'output_filename': output_path.name,
        'cwd': str(cwd),
        'spreadsheets_dir': str(out_dir),
        'file_size_bytes': size_bytes,
        'encoding': encoding,
        'delimiter': delimiter,
        'dialect': dialect_name,
        'rows_written': len(rows),
        'row_count_exact': len(rows) < max_rows,
        'column_count_max': max((len(r) for r in rows), default=0),
        'warnings': warnings,
    })


if __name__ == '__main__':
    main()
)PY";

    return WriteHelperScript("csv_to_xlsx", script, scriptPathOut, errorOut);
}



bool EnsureXlsxInspectScript(std::string& scriptPathOut,
                             std::string& errorOut)
{
    const char* script = R"PY(
import json
import site
import sys
from pathlib import Path


def emit(obj, code=0):
    print(json.dumps(obj, indent=2, ensure_ascii=False))
    raise SystemExit(code)


def load_openpyxl():
    errors = []

    def try_import():
        try:
            from openpyxl import load_workbook
            return load_workbook
        except Exception as ex:
            errors.append('openpyxl: ' + str(ex))
            return None

    loader = try_import()
    if loader is not None:
        return loader, errors

    # PythonRunner launches helpers with -I for a safer default. That
    # hides per-user site packages on many Windows installs. Re-add the
    # user site only for this fixed helper so `py -3 -m pip install --user
    # openpyxl` works during local testing.
    try:
        user_site = getattr(site, 'USER_SITE', None)
        if user_site:
            site.addsitedir(user_site)
    except Exception as ex:
        errors.append('user-site enable failed: ' + str(ex))

    loader = try_import()
    return loader, errors


def is_relative_to(child, parent):
    try:
        child.relative_to(parent)
        return True
    except Exception:
        return False


def cell_to_jsonable(value):
    # openpyxl gives Python primitives for most cell values, but
    # date/datetime/time and Decimal need string projection so the
    # result round-trips through json.dumps without surprises.
    if value is None:
        return ''
    if isinstance(value, (str, int, float, bool)):
        return value
    return str(value)


def main():
    if len(sys.argv) != 2:
        emit({'ok': False, 'helper': 'xlsx_inspect', 'error': 'xlsx_inspect expects exactly one file path argument'}, 2)

    cwd = Path.cwd().resolve()
    raw_arg = sys.argv[1]
    target = Path(raw_arg)
    if not target.is_absolute():
        target = cwd / target
    target = target.resolve()


    if target.suffix.lower() != '.xlsx':
        emit({'ok': False, 'helper': 'xlsx_inspect', 'error': 'Only .xlsx files are supported by xlsx_inspect (no .xls, .xlsm with macros, or .xlsb)', 'path': str(target)}, 4)

    if not target.exists() or not target.is_file():
        emit({'ok': False, 'helper': 'xlsx_inspect', 'error': 'File not found', 'path': str(target)}, 5)

    max_file_bytes = 50 * 1024 * 1024
    size_bytes = target.stat().st_size
    if size_bytes > max_file_bytes:
        emit({'ok': False, 'helper': 'xlsx_inspect', 'error': 'Workbook is larger than the first-phase 50 MB safety cap', 'path': str(target), 'file_size_bytes': size_bytes}, 7)

    load_workbook, import_errors = load_openpyxl()
    if load_workbook is None:
        emit({
            'ok': False,
            'helper': 'xlsx_inspect',
            'error': 'The openpyxl Python package is required for xlsx_inspect. Install it with: py -3 -m pip install --user openpyxl',
            'details': import_errors[-6:],
        }, 8)

    warnings = []
    try:
        # read_only=True streams sheets without loading the whole
        # workbook into memory; data_only=True returns cached values
        # rather than formula text.  Both matter for inspection-of-
        # last-saved-state on real timesheet files.
        wb = load_workbook(filename=str(target), read_only=True, data_only=True)
    except Exception as ex:
        emit({'ok': False, 'helper': 'xlsx_inspect', 'error': 'Could not open workbook: ' + str(ex), 'path': str(target)}, 6)

    max_rows_to_scan_per_sheet = 100000
    max_sample_data_rows = 5
    max_columns_to_report = 200

    sheets_out = []
    for sheet_name in wb.sheetnames:
        try:
            ws = wb[sheet_name]
        except Exception as ex:
            warnings.append('Could not open sheet "' + sheet_name + '": ' + str(ex))
            continue

        rows_iter = ws.iter_rows(values_only=True)
        rows = []
        row_count = 0
        try:
            for row in rows_iter:
                row_count += 1
                if len(rows) < (max_sample_data_rows + 1):
                    rows.append(list(row))
                if row_count >= max_rows_to_scan_per_sheet:
                    warnings.append('Sheet "' + sheet_name + '": stopped counting at 100000 rows.')
                    break
        except Exception as ex:
            warnings.append('Sheet "' + sheet_name + '": row scan failed: ' + str(ex))

        header = list(rows[0]) if rows else []
        if len(header) > max_columns_to_report:
            header = header[:max_columns_to_report]
            warnings.append('Sheet "' + sheet_name + '": column count exceeds 200; truncated.')

        sample_rows = []
        for raw in rows[1:max_sample_data_rows + 1]:
            sample_rows.append([cell_to_jsonable(v) for v in raw[:max_columns_to_report]])

        column_count = max((len(r) for r in rows), default=0)

        sheets_out.append({
            'name': sheet_name,
            'rows_scanned': row_count,
            'row_count_exact': row_count < max_rows_to_scan_per_sheet,
            'column_count_sample': column_count,
            'columns': [cell_to_jsonable(h) for h in header],
            'sample_rows': sample_rows,
        })

    try:
        wb.close()
    except Exception:
        pass

    emit({
        'ok': True,
        'helper': 'xlsx_inspect',
        'path': str(target),
        'cwd': str(cwd),
        'file_size_bytes': size_bytes,
        'sheet_count': len(sheets_out),
        'sheets': sheets_out,
        'warnings': warnings,
    })


if __name__ == '__main__':
    main()
)PY";

    return WriteHelperScript("xlsx_inspect", script, scriptPathOut, errorOut);
}


bool EnsureXlsxReportScript(std::string& scriptPathOut,
                            std::string& errorOut)
{
    const char* script = R"PY(
import json
import re
import site
import sys
import zipfile
import xml.etree.ElementTree as ET
from pathlib import Path


def emit(obj, code=0):
    print(json.dumps(obj, indent=2, ensure_ascii=False))
    raise SystemExit(code)


def load_openpyxl():
    errors = []

    def try_import():
        try:
            from openpyxl import load_workbook
            return load_workbook
        except Exception as ex:
            errors.append('openpyxl: ' + str(ex))
            return None

    loader = try_import()
    if loader is not None:
        return loader, errors

    # PythonRunner launches helpers with -I for a safer default. That
    # hides per-user site packages on many Windows installs. Re-add the
    # user site only for this fixed helper so `py -3 -m pip install --user
    # openpyxl` works during local testing.
    try:
        user_site = getattr(site, 'USER_SITE', None)
        if user_site:
            site.addsitedir(user_site)
    except Exception as ex:
        errors.append('user-site enable failed: ' + str(ex))

    loader = try_import()
    return loader, errors


def is_relative_to(child, parent):
    try:
        child.relative_to(parent)
        return True
    except Exception:
        return False


def safe_name(stem):
    cleaned = re.sub(r'[^A-Za-z0-9._-]+', '_', stem).strip('._-')
    return cleaned or 'xlsx_report'


def unique_path(folder, base_name):
    candidate = folder / base_name
    if not candidate.exists():
        return candidate
    stem = candidate.stem
    suffix = candidate.suffix
    for i in range(2, 1000):
        p = folder / f"{stem}_{i}{suffix}"
        if not p.exists():
            return p
    return folder / f"{stem}_latest{suffix}"


def md_cell(value):
    s = '' if value is None else str(value)
    s = s.replace('\r', ' ').replace('\n', ' ')
    s = s.replace('|', '\\|')
    if len(s) > 80:
        s = s[:77] + '...'
    return s


def md_table(headers, rows):
    out = []
    out.append('| ' + ' | '.join(md_cell(h) for h in headers) + ' |')
    out.append('| ' + ' | '.join('---' for _ in headers) + ' |')
    for row in rows:
        out.append('| ' + ' | '.join(md_cell(v) for v in row) + ' |')
    return '\n'.join(out)


def as_float(value):
    if value is None:
        return None
    if isinstance(value, bool):
        return None  # don't treat True/False as numeric
    if isinstance(value, (int, float)):
        return float(value)
    s = str(value).strip()
    if not s:
        return None
    try:
        return float(s.replace(',', ''))
    except Exception:
        return None


def cell_to_value(value):
    if value is None:
        return ''
    if isinstance(value, (str, int, float, bool)):
        return value
    return str(value)


def main():
    if len(sys.argv) != 3:
        emit({'ok': False, 'helper': 'xlsx_report', 'error': 'xlsx_report expects exactly two internal arguments: input path and fixed output directory'}, 2)

    cwd = Path.cwd().resolve()
    raw_arg = sys.argv[1]
    docs_dir = Path(sys.argv[2]).resolve()

    target = Path(raw_arg)
    if not target.is_absolute():
        target = cwd / target
    target = target.resolve()


    if target.suffix.lower() != '.xlsx':
        emit({'ok': False, 'helper': 'xlsx_report', 'error': 'Only .xlsx files are supported by xlsx_report (no .xls, .xlsm with macros, or .xlsb)', 'path': str(target)}, 4)

    if not target.exists() or not target.is_file():
        emit({'ok': False, 'helper': 'xlsx_report', 'error': 'File not found', 'path': str(target)}, 5)

    max_file_bytes = 50 * 1024 * 1024
    size_bytes = target.stat().st_size
    if size_bytes > max_file_bytes:
        emit({'ok': False, 'helper': 'xlsx_report', 'error': 'Workbook is larger than the first-phase 50 MB safety cap', 'path': str(target), 'file_size_bytes': size_bytes}, 7)

    load_workbook, import_errors = load_openpyxl()
    if load_workbook is None:
        emit({
            'ok': False,
            'helper': 'xlsx_report',
            'error': 'The openpyxl Python package is required for xlsx_report. Install it with: py -3 -m pip install --user openpyxl',
            'details': import_errors[-6:],
        }, 8)

    warnings = []
    try:
        wb = load_workbook(filename=str(target), read_only=True, data_only=True)
    except Exception as ex:
        emit({'ok': False, 'helper': 'xlsx_report', 'error': 'Could not open workbook: ' + str(ex), 'path': str(target)}, 6)

    max_rows_per_sheet = 100000
    max_sample_data_rows = 10
    max_columns_to_report = 200

    summary_sheets = []  # JSON-side sheet records returned alongside the report
    report_blocks = []   # markdown sections per sheet

    for sheet_name in wb.sheetnames:
        try:
            ws = wb[sheet_name]
        except Exception as ex:
            warnings.append('Could not open sheet "' + sheet_name + '": ' + str(ex))
            continue

        rows = []
        try:
            for row in ws.iter_rows(values_only=True):
                rows.append(list(row))
                if len(rows) >= max_rows_per_sheet:
                    warnings.append('Sheet "' + sheet_name + '": stopped scanning at 100000 rows.')
                    break
        except Exception as ex:
            warnings.append('Sheet "' + sheet_name + '": row scan failed: ' + str(ex))
            continue

        header = list(rows[0]) if rows else []
        data_rows = rows[1:] if len(rows) > 1 else []
        width = max((len(r) for r in rows), default=0)
        if width > max_columns_to_report:
            warnings.append('Sheet "' + sheet_name + '": column count exceeds 200; truncated for the report.')
            width = max_columns_to_report
        if rows and any(len(r) != max((len(r) for r in rows), default=0) for r in rows[:200]):
            warnings.append('Sheet "' + sheet_name + '": some sampled rows have inconsistent column counts.')

        columns = []
        for i in range(width):
            name = ''
            if i < len(header) and header[i] is not None and str(header[i]).strip():
                name = str(header[i]).strip()
            else:
                name = f'column_{i + 1}'
            columns.append(name)

        missing_counts = [0 for _ in range(width)]
        nonempty_counts = [0 for _ in range(width)]
        numeric_values = [[] for _ in range(width)]

        for row in data_rows:
            for i in range(width):
                value = row[i] if i < len(row) else None
                if value is None or (isinstance(value, str) and value.strip() == ''):
                    missing_counts[i] += 1
                    continue
                nonempty_counts[i] += 1
                f = as_float(value)
                if f is not None:
                    numeric_values[i].append(f)

        numeric_summary = []
        for i, vals in enumerate(numeric_values):
            if not vals:
                continue
            denom = max(1, nonempty_counts[i])
            if len(vals) / denom < 0.80:
                continue
            total = sum(vals)
            numeric_summary.append({
                'column': columns[i],
                'count': len(vals),
                'sum': round(total, 6),
                'min': round(min(vals), 6),
                'max': round(max(vals), 6),
                'average': round(total / len(vals), 6),
            })

        # Markdown section for this sheet
        block = []
        block.append(f'## Sheet: {sheet_name}')
        block.append('')
        block.append(md_table(['Metric', 'Value'], [
            ['Rows scanned', len(rows)],
            ['Data rows', len(data_rows)],
            ['Columns', width],
        ]))
        block.append('')
        block.append('### Columns')
        block.append('')
        column_rows = []
        for i, name in enumerate(columns):
            column_rows.append([i + 1, name, nonempty_counts[i], missing_counts[i]])
        block.append(md_table(['#', 'Column', 'Non-empty', 'Missing/blank'], column_rows))
        block.append('')

        if numeric_summary:
            block.append('### Numeric Summary')
            block.append('')
            block.append(md_table(['Column', 'Count', 'Sum', 'Min', 'Max', 'Average'], [
                [n['column'], n['count'], n['sum'], n['min'], n['max'], n['average']]
                for n in numeric_summary
            ]))
            block.append('')

        block.append('### Sample Rows')
        block.append('')
        sample = data_rows[:max_sample_data_rows]
        if sample and columns:
            padded = []
            for row in sample:
                padded.append([(cell_to_value(row[i]) if i < len(row) else '') for i in range(width)])
            block.append(md_table(columns, padded))
        else:
            block.append('_No data rows found._')
        block.append('')

        report_blocks.append('\n'.join(block))

        summary_sheets.append({
            'name': sheet_name,
            'rows_scanned': len(rows),
            'data_row_count': len(data_rows),
            'column_count': width,
            'columns': columns,
            'numeric_columns': numeric_summary,
        })

    try:
        wb.close()
    except Exception:
        pass

    docs_dir.mkdir(parents=True, exist_ok=True)
    report_name = safe_name(target.stem) + '_report.md'
    output_path = unique_path(docs_dir, report_name)

    header_lines = []
    header_lines.append(f'# XLSX Report: {target.name}')
    header_lines.append('')
    header_lines.append('Generated by LlamaBoss controlled Python backend (`xlsx_report`).')
    header_lines.append('')
    header_lines.append('## Summary')
    header_lines.append('')
    header_lines.append(md_table(['Metric', 'Value'], [
        ['Input file', str(target)],
        ['Output report', str(output_path)],
        ['File size', f'{size_bytes} bytes'],
        ['Sheets', len(summary_sheets)],
    ]))
    header_lines.append('')

    warnings_block = []
    warnings_block.append('## Warnings')
    warnings_block.append('')
    if warnings:
        for w in warnings:
            warnings_block.append(f'- {w}')
    else:
        warnings_block.append('- None')
    warnings_block.append('')

    output_text = '\n'.join(header_lines) + '\n'.join(report_blocks) + '\n' + '\n'.join(warnings_block)
    output_path.write_text(output_text, encoding='utf-8')

    emit({
        'ok': True,
        'helper': 'xlsx_report',
        'input_path': str(target),
        'output_path': str(output_path),
        'output_filename': output_path.name,
        'cwd': str(cwd),
        'documents_dir': str(docs_dir),
        'file_size_bytes': size_bytes,
        'sheet_count': len(summary_sheets),
        'sheets': summary_sheets,
        'warnings': warnings,
    })


if __name__ == '__main__':
    main()
)PY";

    return WriteHelperScript("xlsx_report", script, scriptPathOut, errorOut);
}


bool EnsurePdfExtractTextScript(std::string& scriptPathOut,
                                std::string& errorOut)
{
    const char* script = R"PY(
import json
import re
import site
import sys
from pathlib import Path


def emit(obj, code=0):
    print(json.dumps(obj, indent=2, ensure_ascii=False))
    raise SystemExit(code)


def is_relative_to(child, parent):
    try:
        child.relative_to(parent)
        return True
    except Exception:
        return False


def safe_name(name):
    cleaned = re.sub(r'[^A-Za-z0-9._-]+', '_', name).strip('._-')
    return cleaned or 'pdf_extract_text'


def unique_path(folder, filename):
    path = folder / filename
    if not path.exists():
        return path
    stem = path.stem
    suffix = path.suffix
    for i in range(2, 1000):
        candidate = folder / f"{stem}_{i}{suffix}"
        if not candidate.exists():
            return candidate
    return folder / f"{stem}_latest{suffix}"


def load_pdf_reader():
    errors = []

    def try_import():
        try:
            from pypdf import PdfReader
            return PdfReader, 'pypdf'
        except Exception as ex:
            errors.append('pypdf: ' + str(ex))
        try:
            from PyPDF2 import PdfReader
            return PdfReader, 'PyPDF2'
        except Exception as ex:
            errors.append('PyPDF2: ' + str(ex))
        return None, None

    reader_cls, module_name = try_import()
    if reader_cls is not None:
        return reader_cls, module_name, errors

    # PythonRunner launches helpers with -I for a safer default.  That
    # hides per-user site packages on many Windows installs.  Re-add the
    # user site only for this fixed helper so `py -3 -m pip install --user
    # pypdf` works during weekend-project testing.
    try:
        user_site = getattr(site, 'USER_SITE', None)
        if user_site:
            site.addsitedir(user_site)
    except Exception as ex:
        errors.append('user-site enable failed: ' + str(ex))

    reader_cls, module_name = try_import()
    return reader_cls, module_name, errors


def md_escape_line(s):
    return s.replace('\x00', '')


def main():
    if len(sys.argv) != 3:
        emit({'ok': False, 'helper': 'pdf_extract_text', 'error': 'pdf_extract_text expects exactly two internal arguments: input path and fixed output directory'}, 2)

    cwd = Path.cwd().resolve()
    raw_arg = sys.argv[1]
    pdfs_dir = Path(sys.argv[2]).resolve()

    target = Path(raw_arg)
    if not target.is_absolute():
        target = cwd / target
    target = target.resolve()


    if target.suffix.lower() != '.pdf':
        emit({'ok': False, 'helper': 'pdf_extract_text', 'error': 'Only .pdf files are supported by pdf_extract_text', 'path': str(target)}, 4)

    if not target.exists() or not target.is_file():
        emit({'ok': False, 'helper': 'pdf_extract_text', 'error': 'File not found', 'path': str(target)}, 5)

    max_file_bytes = 50 * 1024 * 1024
    size_bytes = target.stat().st_size
    if size_bytes > max_file_bytes:
        emit({'ok': False, 'helper': 'pdf_extract_text', 'error': 'PDF is larger than the first-phase 50 MB safety cap', 'path': str(target), 'file_size_bytes': size_bytes}, 7)

    PdfReader, extractor_module, import_errors = load_pdf_reader()
    if PdfReader is None:
        emit({
            'ok': False,
            'helper': 'pdf_extract_text',
            'error': 'Missing PDF text extraction dependency. Install pypdf with: py -3 -m pip install --user pypdf',
            'details': import_errors[-6:],
        }, 8)

    warnings = []
    pages_text = []

    try:
        reader = PdfReader(str(target))
        pages = list(reader.pages)
    except Exception as ex:
        emit({'ok': False, 'helper': 'pdf_extract_text', 'error': 'Could not open PDF: ' + str(ex), 'path': str(target)}, 6)

    max_pages = 500
    total_pages = len(pages)
    if total_pages > max_pages:
        warnings.append(f'Only extracted the first {max_pages} pages out of {total_pages}.')
        pages = pages[:max_pages]

    for index, page in enumerate(pages, start=1):
        try:
            text = page.extract_text() or ''
        except Exception as ex:
            text = ''
            warnings.append(f'Page {index}: text extraction failed: {ex}')
        text = md_escape_line(text.strip())
        pages_text.append((index, text))

    nonempty_pages = sum(1 for _, text in pages_text if text.strip())
    extracted_chars = sum(len(text) for _, text in pages_text)

    if extracted_chars == 0:
        emit({
            'ok': False,
            'helper': 'pdf_extract_text',
            'error': 'No extractable text found. This may be a scanned/image-only PDF. OCR is not supported yet.',
            'input_path': str(target),
            'page_count': total_pages,
            'extractor_module': extractor_module,
            'warnings': warnings,
        }, 9)

    pdfs_dir.mkdir(parents=True, exist_ok=True)
    output_name = safe_name(target.stem) + '_extracted_text.md'
    output_path = unique_path(pdfs_dir, output_name)

    out = []
    out.append(f'# Extracted PDF Text: {target.name}')
    out.append('')
    out.append('Generated by LlamaBoss controlled Python backend (`pdf_extract_text`).')
    out.append('')
    out.append('## Summary')
    out.append('')
    out.append(f'- Input file: `{target}`')
    out.append(f'- Output file: `{output_path}`')
    out.append(f'- File size: {size_bytes} bytes')
    out.append(f'- Pages in PDF: {total_pages}')
    out.append(f'- Pages extracted: {len(pages_text)}')
    out.append(f'- Pages with text: {nonempty_pages}')
    out.append(f'- Extracted characters: {extracted_chars}')
    out.append(f'- Extractor: {extractor_module}')
    out.append('')
    out.append('## Text')
    out.append('')

    for page_num, text in pages_text:
        out.append(f'--- Page {page_num} ---')
        out.append('')
        if text.strip():
            out.append(text)
        else:
            out.append('[No extractable text on this page]')
        out.append('')

    if warnings:
        out.append('## Warnings')
        out.append('')
        for w in warnings:
            out.append(f'- {w}')
        out.append('')

    output_text = '\n'.join(out)
    output_path.write_text(output_text, encoding='utf-8')

    emit({
        'ok': True,
        'helper': 'pdf_extract_text',
        'input_path': str(target),
        'output_path': str(output_path),
        'output_filename': output_path.name,
        'cwd': str(cwd),
        'pdfs_dir': str(pdfs_dir),
        'file_size_bytes': size_bytes,
        'page_count': total_pages,
        'pages_extracted': len(pages_text),
        'pages_with_text': nonempty_pages,
        'extracted_char_count': extracted_chars,
        'extractor_module': extractor_module,
        'warnings': warnings,
    })


if __name__ == '__main__':
    main()
)PY";

    return WriteHelperScript("pdf_extract_text", script, scriptPathOut, errorOut);
}


bool EnsurePdfInspectFormScript(std::string& scriptPathOut,
                                std::string& errorOut)
{
    const char* script = R"PY(
import json
import site
import sys
from pathlib import Path


def emit(obj, code=0):
    print(json.dumps(obj, indent=2, ensure_ascii=False))
    raise SystemExit(code)


def is_relative_to(child, parent):
    try:
        child.relative_to(parent)
        return True
    except Exception:
        return False


def load_fitz():
    # PyMuPDF imports as `pymupdf` on recent versions and `fitz` on
    # older ones.  Try both before re-adding the user site (PythonRunner
    # launches helpers with -I, which suppresses per-user site packages).
    errors = []

    def try_import():
        try:
            import pymupdf
            return pymupdf, 'pymupdf'
        except Exception as ex:
            errors.append('pymupdf: ' + str(ex))
        try:
            import fitz
            return fitz, 'fitz'
        except Exception as ex:
            errors.append('fitz: ' + str(ex))
        return None, None

    mod, name = try_import()
    if mod is not None:
        return mod, name, errors

    try:
        user_site = getattr(site, 'USER_SITE', None)
        if user_site:
            site.addsitedir(user_site)
    except Exception as ex:
        errors.append('user-site enable failed: ' + str(ex))

    mod, name = try_import()
    return mod, name, errors


def normalize_field_type(fitz_mod, widget):
    # Map PyMuPDF's int constants to the canonical strings the LlamaBoss
    # tool surface uses.  Exact constant names vary slightly between
    # PyMuPDF major versions, so look up the int once and compare.
    try:
        t = int(widget.field_type)
    except Exception:
        return 'unknown'

    name_by_int = {
        getattr(fitz_mod, 'PDF_WIDGET_TYPE_BUTTON', -1):       'button',
        getattr(fitz_mod, 'PDF_WIDGET_TYPE_CHECKBOX', -1):     'checkbox',
        getattr(fitz_mod, 'PDF_WIDGET_TYPE_RADIOBUTTON', -1):  'radio',
        getattr(fitz_mod, 'PDF_WIDGET_TYPE_TEXT', -1):         'text',
        getattr(fitz_mod, 'PDF_WIDGET_TYPE_LISTBOX', -1):      'listbox',
        getattr(fitz_mod, 'PDF_WIDGET_TYPE_COMBOBOX', -1):     'dropdown',
        getattr(fitz_mod, 'PDF_WIDGET_TYPE_SIGNATURE', -1):    'signature',
    }
    if t in name_by_int and name_by_int[t] != 'unknown':
        return name_by_int[t]

    # Fall back to the human string PyMuPDF exposes; lower-case it so
    # the wire shape stays predictable.
    label = getattr(widget, 'field_type_string', '') or ''
    label = str(label).strip().lower()
    if 'check' in label:    return 'checkbox'
    if 'radio' in label:    return 'radio'
    if 'combo' in label:    return 'dropdown'
    if 'list' in label:     return 'listbox'
    if 'sign' in label:     return 'signature'
    if 'text' in label:     return 'text'
    if 'button' in label:   return 'button'
    return 'unknown'


def safe_str(v):
    if v is None:
        return ''
    try:
        return str(v)
    except Exception:
        return ''


def collect_fields(doc, fitz_mod, max_fields):
    fields = []
    truncated = False
    total_seen = 0

    for page_index, page in enumerate(doc, start=1):
        try:
            widgets = list(page.widgets() or [])
        except Exception:
            widgets = []

        for widget in widgets:
            total_seen += 1
            if len(fields) >= max_fields:
                truncated = True
                continue

            ftype = normalize_field_type(fitz_mod, widget)

            # Required = bit 1 (value 2) of /Ff
            try:
                flags = int(getattr(widget, 'field_flags', 0) or 0)
            except Exception:
                flags = 0
            required = bool(flags & 2)

            entry = {
                'name':          safe_str(getattr(widget, 'field_name', '')),
                'type':          ftype,
                'page':          page_index,
                'current_value': safe_str(getattr(widget, 'field_value', '')),
                'required':      required,
            }

            tooltip = safe_str(getattr(widget, 'field_label', '') or
                               getattr(widget, 'field_display', ''))
            if tooltip:
                entry['tooltip'] = tooltip

            if ftype in ('dropdown', 'listbox'):
                try:
                    options = list(getattr(widget, 'choice_values', None) or [])
                    entry['options'] = [safe_str(o) for o in options]
                except Exception:
                    entry['options'] = []

            if ftype in ('checkbox', 'radio'):
                # button_states() returns {'normal': [...], 'down': [...]}.
                # The 'normal' on-state names are what /pdf_fill_form
                # needs to know to set a checkbox/radio correctly.
                try:
                    states = widget.button_states() or {}
                    on_states = list(states.get('normal', []) or [])
                    # Filter the off marker so the model only sees
                    # actual on-values to choose from.
                    on_states = [safe_str(s) for s in on_states
                                 if safe_str(s).lower() not in ('off', '')]
                    if on_states:
                        entry['on_states'] = on_states
                except Exception:
                    pass

            fields.append(entry)

    return fields, truncated, total_seen


def detect_form_kind(doc):
    # PyMuPDF's `is_form_pdf` returns True for AcroForm PDFs.  XFA-only
    # PDFs typically have a /XFA entry on the AcroForm dict but no widget
    # annotations -- detect them so we can refuse with a useful message
    # rather than reporting "0 fields".
    try:
        is_acro = bool(getattr(doc, 'is_form_pdf', False))
    except Exception:
        is_acro = False

    has_xfa = False
    try:
        # In PyMuPDF >= 1.18, doc.xfa is a list/None.  Treat any non-empty
        # XFA payload as evidence.
        xfa = getattr(doc, 'xfa', None)
        if xfa:
            has_xfa = True
    except Exception:
        pass

    if is_acro and not has_xfa:
        return 'AcroForm'
    if is_acro and has_xfa:
        return 'AcroForm+XFA'
    if has_xfa:
        return 'XFA'
    return 'None'


def main():
    if len(sys.argv) != 2:
        emit({'ok': False, 'helper': 'pdf_inspect_form', 'error': 'pdf_inspect_form expects exactly one internal argument: input path'}, 2)

    cwd = Path.cwd().resolve()
    raw_arg = sys.argv[1]

    target = Path(raw_arg)
    if not target.is_absolute():
        target = cwd / target
    target = target.resolve()


    if target.suffix.lower() != '.pdf':
        emit({'ok': False, 'helper': 'pdf_inspect_form', 'error': 'Only .pdf files are supported by pdf_inspect_form', 'path': str(target)}, 4)

    if not target.exists() or not target.is_file():
        emit({'ok': False, 'helper': 'pdf_inspect_form', 'error': 'File not found', 'path': str(target)}, 5)

    max_file_bytes = 50 * 1024 * 1024
    size_bytes = target.stat().st_size
    if size_bytes > max_file_bytes:
        emit({'ok': False, 'helper': 'pdf_inspect_form', 'error': 'PDF is larger than the first-phase 50 MB safety cap', 'path': str(target), 'file_size_bytes': size_bytes}, 7)

    fitz_mod, mod_name, import_errors = load_fitz()
    if fitz_mod is None:
        emit({
            'ok': False,
            'helper': 'pdf_inspect_form',
            'error': 'Missing PDF form dependency. Install PyMuPDF with: py -3 -m pip install --user pymupdf',
            'details': import_errors[-6:],
        }, 8)

    try:
        doc = fitz_mod.open(str(target))
    except Exception as ex:
        emit({'ok': False, 'helper': 'pdf_inspect_form', 'error': 'Could not open PDF: ' + str(ex), 'path': str(target)}, 6)

    form_kind = detect_form_kind(doc)
    page_count = doc.page_count

    if form_kind == 'XFA':
        emit({
            'ok': False,
            'helper': 'pdf_inspect_form',
            'error': 'This PDF uses XFA forms (older Adobe LiveCycle format), not AcroForm. pdf_inspect_form and pdf_fill_form do not support XFA. Common XFA examples: older IRS forms, some state DMV forms. Re-saving the PDF in a recent Acrobat or printing-to-PDF often produces an AcroForm-compatible copy.',
            'input_path': str(target),
            'page_count': page_count,
            'form_type': form_kind,
            'extractor_module': mod_name,
        }, 10)

    if form_kind == 'None':
        emit({
            'ok': False,
            'helper': 'pdf_inspect_form',
            'error': 'This PDF does not contain any fillable form fields. It may be a flat document or a scanned image.',
            'input_path': str(target),
            'page_count': page_count,
            'form_type': form_kind,
            'extractor_module': mod_name,
        }, 11)

    max_fields = 500
    fields, truncated, total_seen = collect_fields(doc, fitz_mod, max_fields)

    warnings = []
    if truncated:
        warnings.append(f'Only the first {max_fields} of {total_seen} fields are listed.')
    if form_kind == 'AcroForm+XFA':
        warnings.append('PDF carries both AcroForm and XFA dictionaries. Reading AcroForm fields; XFA-only fields will not appear.')

    emit({
        'ok': True,
        'helper': 'pdf_inspect_form',
        'input_path': str(target),
        'cwd': str(cwd),
        'file_size_bytes': size_bytes,
        'page_count': page_count,
        'form_type': form_kind,
        'field_count': len(fields),
        'total_fields_seen': total_seen,
        'fields': fields,
        'extractor_module': mod_name,
        'warnings': warnings,
    })


if __name__ == '__main__':
    main()
)PY";

    return WriteHelperScript("pdf_inspect_form", script, scriptPathOut, errorOut);
}


bool EnsurePdfFillFormScript(std::string& scriptPathOut,
                             std::string& errorOut)
{
    const char* script = R"PY(
import json
import re
import site
import sys
from pathlib import Path


def emit(obj, code=0):
    print(json.dumps(obj, indent=2, ensure_ascii=False))
    raise SystemExit(code)


def is_relative_to(child, parent):
    try:
        child.relative_to(parent)
        return True
    except Exception:
        return False


def safe_name(name):
    cleaned = re.sub(r'[^A-Za-z0-9._-]+', '_', name).strip('._-')
    return cleaned or 'pdf_fill_form'


def unique_path(folder, filename):
    path = folder / filename
    if not path.exists():
        return path
    stem = path.stem
    suffix = path.suffix
    for i in range(2, 1000):
        candidate = folder / f"{stem}_{i}{suffix}"
        if not candidate.exists():
            return candidate
    return folder / f"{stem}_latest{suffix}"


def load_fitz():
    errors = []

    def try_import():
        try:
            import pymupdf
            return pymupdf, 'pymupdf'
        except Exception as ex:
            errors.append('pymupdf: ' + str(ex))
        try:
            import fitz
            return fitz, 'fitz'
        except Exception as ex:
            errors.append('fitz: ' + str(ex))
        return None, None

    mod, name = try_import()
    if mod is not None:
        return mod, name, errors

    try:
        user_site = getattr(site, 'USER_SITE', None)
        if user_site:
            site.addsitedir(user_site)
    except Exception as ex:
        errors.append('user-site enable failed: ' + str(ex))

    mod, name = try_import()
    return mod, name, errors


def normalize_field_type(fitz_mod, widget):
    try:
        t = int(widget.field_type)
    except Exception:
        return 'unknown'

    name_by_int = {
        getattr(fitz_mod, 'PDF_WIDGET_TYPE_BUTTON', -1):       'button',
        getattr(fitz_mod, 'PDF_WIDGET_TYPE_CHECKBOX', -1):     'checkbox',
        getattr(fitz_mod, 'PDF_WIDGET_TYPE_RADIOBUTTON', -1):  'radio',
        getattr(fitz_mod, 'PDF_WIDGET_TYPE_TEXT', -1):         'text',
        getattr(fitz_mod, 'PDF_WIDGET_TYPE_LISTBOX', -1):      'listbox',
        getattr(fitz_mod, 'PDF_WIDGET_TYPE_COMBOBOX', -1):     'dropdown',
        getattr(fitz_mod, 'PDF_WIDGET_TYPE_SIGNATURE', -1):    'signature',
    }
    if t in name_by_int and name_by_int[t] != 'unknown':
        return name_by_int[t]
    label = getattr(widget, 'field_type_string', '') or ''
    label = str(label).strip().lower()
    if 'check' in label:    return 'checkbox'
    if 'radio' in label:    return 'radio'
    if 'combo' in label:    return 'dropdown'
    if 'list' in label:     return 'listbox'
    if 'sign' in label:     return 'signature'
    if 'text' in label:     return 'text'
    if 'button' in label:   return 'button'
    return 'unknown'


def widget_on_states(widget):
    try:
        states = widget.button_states() or {}
        on_states = list(states.get('normal', []) or [])
        return [s for s in on_states if str(s).lower() not in ('off', '')]
    except Exception:
        return []


def widget_options(widget):
    try:
        return [str(o) for o in (getattr(widget, 'choice_values', None) or [])]
    except Exception:
        return []


def detect_form_kind(doc):
    try:
        is_acro = bool(getattr(doc, 'is_form_pdf', False))
    except Exception:
        is_acro = False
    has_xfa = False
    try:
        xfa = getattr(doc, 'xfa', None)
        if xfa:
            has_xfa = True
    except Exception:
        pass
    if is_acro and not has_xfa:
        return 'AcroForm'
    if is_acro and has_xfa:
        return 'AcroForm+XFA'
    if has_xfa:
        return 'XFA'
    return 'None'


def collect_field_metadata(doc, fitz_mod):
    """Walk the document once and capture every per-field detail
    needed for validation, WITHOUT keeping widget references.

    PyMuPDF widgets become "unbound from their page" once the parent
    page Python object falls out of scope, which makes any later
    .update() call fail with "Annot is not bound to a page".  We
    sidestep that entirely by reading button_states / choice_values
    while the page is still in scope, caching only plain Python data,
    and re-walking the doc again in the mutation pass.
    """
    meta = {}
    for page_index, page in enumerate(doc, start=1):
        try:
            widgets = list(page.widgets() or [])
        except Exception:
            widgets = []
        for w in widgets:
            name = str(getattr(w, 'field_name', '') or '')
            if not name:
                continue
            ftype = normalize_field_type(fitz_mod, w)
            entry = meta.get(name)
            if entry is None:
                entry = {
                    'type':            ftype,
                    'first_page':      page_index,
                    'on_states':       set(),
                    'options':         [],
                    'instance_count':  0,
                }
                meta[name] = entry
            if ftype in ('checkbox', 'radio'):
                for st in widget_on_states(w):
                    entry['on_states'].add(st)
            if ftype in ('dropdown', 'listbox') and not entry['options']:
                entry['options'] = widget_options(w)
            entry['instance_count'] += 1

    # Stabilize set-typed fields for downstream comparisons.
    for entry in meta.values():
        entry['on_states'] = sorted(entry['on_states'])
    return meta


def coerce_text_value(v):
    if v is None:
        return ''
    if isinstance(v, bool):
        return 'true' if v else 'false'
    return str(v)


def normalize_checkbox_value(v, on_states):
    """Return either an on-state string (to check) or 'Off' (to uncheck),
    or None if the value cannot be resolved.
    """
    if isinstance(v, bool):
        if v:
            return on_states[0] if on_states else 'Yes'
        return 'Off'
    if v is None:
        return 'Off'
    s = str(v).strip()
    if s == '':
        return 'Off'
    sl = s.lower()
    if sl in ('off', 'false', 'no', '0', 'unchecked', 'no_'):
        return 'Off'
    if sl in ('true', 'yes', '1', 'checked', 'on'):
        return on_states[0] if on_states else 'Yes'
    # Strict path: caller passed the literal on-state string
    if s in on_states:
        return s
    # Case-insensitive fallback against on-states
    for st in on_states:
        if st.lower() == sl:
            return st
    return None


def suggest_similar_name(target, candidates, max_suggestions=3):
    """Cheap fuzzy match -- exact-substring, prefix, then char-overlap."""
    target_l = target.lower()
    suggestions = []

    for c in candidates:
        if c == target:
            continue
        if c.lower() == target_l:
            suggestions.append(c)
    for c in candidates:
        if c in suggestions:
            continue
        if target_l in c.lower() or c.lower() in target_l:
            suggestions.append(c)
        if len(suggestions) >= max_suggestions:
            break

    if len(suggestions) < max_suggestions:
        scored = []
        ts = set(target_l)
        for c in candidates:
            if c in suggestions:
                continue
            cs = set(c.lower())
            inter = len(ts & cs)
            if inter > 0:
                scored.append((inter, -abs(len(c) - len(target)), c))
        scored.sort(reverse=True)
        for _, _, c in scored:
            suggestions.append(c)
            if len(suggestions) >= max_suggestions:
                break
    return suggestions[:max_suggestions]


)PY"
R"PY(def main():
    if len(sys.argv) != 4:
        emit({'ok': False, 'helper': 'pdf_fill_form', 'error': 'pdf_fill_form expects exactly three internal arguments: input path, JSON field map, fixed output directory'}, 2)

    cwd = Path.cwd().resolve()
    raw_path_arg = sys.argv[1]
    raw_json_arg = sys.argv[2]
    out_dir = Path(sys.argv[3]).resolve()

    target = Path(raw_path_arg)
    if not target.is_absolute():
        target = cwd / target
    target = target.resolve()


    if target.suffix.lower() != '.pdf':
        emit({'ok': False, 'helper': 'pdf_fill_form', 'error': 'Only .pdf files are supported by pdf_fill_form', 'path': str(target)}, 4)

    if not target.exists() or not target.is_file():
        emit({'ok': False, 'helper': 'pdf_fill_form', 'error': 'File not found', 'path': str(target)}, 5)

    max_file_bytes = 50 * 1024 * 1024
    size_bytes = target.stat().st_size
    if size_bytes > max_file_bytes:
        emit({'ok': False, 'helper': 'pdf_fill_form', 'error': 'PDF is larger than the first-phase 50 MB safety cap', 'path': str(target), 'file_size_bytes': size_bytes}, 7)

    # Parse the JSON field map.  Empty / whitespace-only is rejected
    # so we don't silently produce a "filled" copy that's identical to
    # the input -- that would be confusing for the user.
    try:
        fill_map = json.loads(raw_json_arg)
    except Exception as ex:
        emit({'ok': False, 'helper': 'pdf_fill_form', 'error': 'Could not parse field map as JSON: ' + str(ex), 'received': raw_json_arg[:400]}, 12)

    if not isinstance(fill_map, dict):
        emit({'ok': False, 'helper': 'pdf_fill_form', 'error': 'Field map must be a JSON object {field_name: value, ...}', 'received_type': type(fill_map).__name__}, 13)

    if not fill_map:
        emit({'ok': False, 'helper': 'pdf_fill_form', 'error': 'Field map is empty -- nothing to fill. Provide at least one {field_name: value} entry.'}, 14)

    fitz_mod, mod_name, import_errors = load_fitz()
    if fitz_mod is None:
        emit({
            'ok': False,
            'helper': 'pdf_fill_form',
            'error': 'Missing PDF form dependency. Install PyMuPDF with: py -3 -m pip install --user pymupdf',
            'details': import_errors[-6:],
        }, 8)

    try:
        doc = fitz_mod.open(str(target))
    except Exception as ex:
        emit({'ok': False, 'helper': 'pdf_fill_form', 'error': 'Could not open PDF: ' + str(ex), 'path': str(target)}, 6)

    form_kind = detect_form_kind(doc)
    if form_kind == 'XFA':
        emit({
            'ok': False,
            'helper': 'pdf_fill_form',
            'error': 'This PDF uses XFA forms (older Adobe LiveCycle format), not AcroForm. pdf_fill_form does not support XFA. Re-saving the PDF in a recent Acrobat or printing-to-PDF often produces an AcroForm-compatible copy.',
            'input_path': str(target),
            'form_type': form_kind,
        }, 10)
    if form_kind == 'None':
        emit({
            'ok': False,
            'helper': 'pdf_fill_form',
            'error': 'This PDF does not contain any fillable form fields. There is nothing for pdf_fill_form to fill.',
            'input_path': str(target),
            'form_type': form_kind,
        }, 11)

    field_meta = collect_field_metadata(doc, fitz_mod)

    # ── Validation pass: hard-fail policy.  Any unknown field name or
    # invalid value rejects the WHOLE call.  We collect every error
    # before reporting so the model gets one complete picture instead
    # of having to retry repeatedly to surface each problem.  Operates
    # purely on field_meta (plain Python data) -- no widget references
    # are touched here, since PyMuPDF widget objects can lose their
    # page binding the moment the originating page falls out of scope.
    errors = []
    plan = {}  # {field_name: (ftype, resolved_value, original_input)}

    for key, value in fill_map.items():
        if key not in field_meta:
            suggestions = suggest_similar_name(key, list(field_meta.keys()))
            entry = {'field': key, 'reason': 'Field name not found in this PDF.'}
            if suggestions:
                entry['did_you_mean'] = suggestions
            errors.append(entry)
            continue

        meta = field_meta[key]
        ftype = meta['type']

        if ftype == 'text':
            plan[key] = (ftype, coerce_text_value(value), value)

        elif ftype == 'checkbox':
            on_states = list(meta['on_states'])
            resolved = normalize_checkbox_value(value, on_states)
            if resolved is None:
                errors.append({
                    'field': key,
                    'reason': f'Invalid checkbox value {value!r}. Use true/false, or one of: {on_states + ["Off"]}.',
                })
                continue
            plan[key] = (ftype, resolved, value)

        elif ftype == 'radio':
            if isinstance(value, bool):
                errors.append({
                    'field': key,
                    'reason': 'Radio fields cannot be set with a boolean. Pass the on-state string of the option you want to select.',
                })
                continue
            s = '' if value is None else str(value).strip()
            group_states = list(meta['on_states'])
            if s.lower() == 'off' or s == '':
                resolved = 'Off'
            elif s in group_states:
                resolved = s
            else:
                match = next((st for st in group_states if st.lower() == s.lower()), None)
                if match:
                    resolved = match
                else:
                    errors.append({
                        'field': key,
                        'reason': f'Invalid radio value {value!r}. Valid on-states for this group: {group_states}, or "Off" to deselect.',
                    })
                    continue
            plan[key] = (ftype, resolved, value)

        elif ftype in ('dropdown', 'listbox'):
            options = list(meta['options'])
            s = '' if value is None else str(value)
            if s == '':
                resolved = ''
            elif s in options:
                resolved = s
            else:
                match = next((o for o in options if o.lower() == s.lower()), None)
                if match:
                    resolved = match
                else:
                    errors.append({
                        'field': key,
                        'reason': f'Invalid {ftype} value {value!r}. Valid options: {options}.',
                    })
                    continue
            plan[key] = (ftype, resolved, value)

        elif ftype == 'signature':
            errors.append({
                'field': key,
                'reason': 'Signature fields cannot be filled by pdf_fill_form. They require an actual signing workflow.',
            })

        else:
            errors.append({
                'field': key,
                'reason': f'Field type {ftype!r} is not supported for filling.',
            })

    if errors:
        emit({
            'ok': False,
            'helper': 'pdf_fill_form',
            'error': 'Field map validation failed. No changes were written. Fix every entry below and try again.',
            'input_path': str(target),
            'form_type': form_kind,
            'validation_errors': errors,
            'extractor_module': mod_name,
        }, 15)

    # ── Mutation pass: re-walk the doc fresh.  Crucially, the widgets
    # mutated here are obtained INSIDE the same enumerate() that holds
    # their parent page in scope; we never carry widget references
    # across iterations.  This is what avoids "Annot is not bound to
    # a page" failures from PyMuPDF.
    fill_results = []
    applied = set()  # names that have already produced a fill_results entry
    try:
        for page_index, page in enumerate(doc, start=1):
            try:
                widgets_iter = list(page.widgets() or [])
            except Exception:
                widgets_iter = []
            for widget in widgets_iter:
                name = str(getattr(widget, 'field_name', '') or '')
                if not name or name not in plan:
                    continue
                ftype, resolved, original = plan[name]
                # Radios: a single field name corresponds to multiple
                # widgets (one per option).  Setting field_value on every
                # widget in the group is idempotent and defends against
                # PyMuPDF version quirks where setting on the wrong widget
                # is a no-op.  Other field types: set on first encounter
                # only; later encounters with the same name are unusual
                # mirrored fields and the value is already in field_value.
                if ftype != 'radio' and name in applied:
                    continue
                widget.field_value = resolved
                widget.update()
                if name not in applied:
                    applied.add(name)
                    preview = resolved
                    if ftype == 'text' and isinstance(resolved, str) and len(resolved) > 200:
                        preview = resolved[:200] + '...'
                    fill_results.append({
                        'field':          name,
                        'type':           ftype,
                        'page':           page_index,
                        'value_written':  preview,
                        'original_input': original,
                    })
    except Exception as ex:
        emit({
            'ok': False,
            'helper': 'pdf_fill_form',
            'error': 'Failed during widget update: ' + str(ex),
            'input_path': str(target),
            'fields_processed_before_failure': len(fill_results),
        }, 16)

    # Surface any plan entries that were never matched to a widget on
    # the second walk.  In practice this only happens if the document
    # changes between walks, which shouldn't, but the guard is cheap.
    unmatched = [n for n in plan.keys() if n not in applied]
    if unmatched:
        emit({
            'ok': False,
            'helper': 'pdf_fill_form',
            'error': 'Some validated fields could not be located on the second walk: ' + ', '.join(unmatched),
            'input_path': str(target),
        }, 18)

    out_dir.mkdir(parents=True, exist_ok=True)
    output_name = safe_name(target.stem) + '_filled.pdf'
    output_path = unique_path(out_dir, output_name)

    try:
        # garbage=4 cleans unused objects; deflate=True compresses
        # streams.  Both are cheap on a single-form save and produce
        # smaller output, which matters for large filled HR packets.
        doc.save(str(output_path), garbage=4, deflate=True)
    except Exception as ex:
        emit({
            'ok': False,
            'helper': 'pdf_fill_form',
            'error': 'Failed to save filled PDF: ' + str(ex),
            'input_path': str(target),
            'output_path': str(output_path),
        }, 17)

    out_size = output_path.stat().st_size if output_path.exists() else 0

    emit({
        'ok': True,
        'helper': 'pdf_fill_form',
        'input_path':       str(target),
        'output_path':      str(output_path),
        'output_filename':  output_path.name,
        'cwd':              str(cwd),
        'filled_forms_dir': str(out_dir),
        'form_type':        form_kind,
        'fields_filled':    len(fill_results),
        'fields':           fill_results,
        'output_size_bytes': out_size,
        'extractor_module': mod_name,
    })


if __name__ == '__main__':
    main()
)PY";

    return WriteHelperScript("pdf_fill_form", script, scriptPathOut, errorOut);
}

bool EnsureDocxExtractTextScript(std::string& scriptPathOut,
                                 std::string& errorOut)
{
    const char* script = R"PY(
import json
import re
import site
import sys
import zipfile
import xml.etree.ElementTree as ET
from pathlib import Path


def emit(obj, code=0):
    print(json.dumps(obj, indent=2, ensure_ascii=False))
    raise SystemExit(code)


def is_relative_to(child, parent):
    try:
        child.relative_to(parent)
        return True
    except Exception:
        return False


def safe_name(name):
    cleaned = re.sub(r'[^A-Za-z0-9._-]+', '_', name).strip('._-')
    return cleaned or 'docx_extract_text'


def unique_path(folder, filename):
    path = folder / filename
    if not path.exists():
        return path
    stem = path.stem
    suffix = path.suffix
    for i in range(2, 1000):
        candidate = folder / f"{stem}_{i}{suffix}"
        if not candidate.exists():
            return candidate
    return folder / f"{stem}_latest{suffix}"


def load_docx_module():
    errors = []

    def try_import():
        try:
            import docx
            return docx
        except Exception as ex:
            errors.append('python-docx: ' + str(ex))
            return None

    mod = try_import()
    if mod is not None:
        return mod, errors

    # PythonRunner launches helpers with -I for a safer default.  That
    # hides per-user site packages on many Windows installs.  Re-enable
    # the user site for this fixed helper so `py -3 -m pip install --user
    # python-docx` works.
    try:
        user_site = getattr(site, 'USER_SITE', None)
        if user_site:
            site.addsitedir(user_site)
    except Exception as ex:
        errors.append('user-site enable failed: ' + str(ex))

    mod = try_import()
    return mod, errors



W_NS = 'http://schemas.openxmlformats.org/wordprocessingml/2006/main'
W_TAG = '{' + W_NS + '}'


def local_name(tag):
    if not isinstance(tag, str):
        return ''
    if tag.startswith('{'):
        return tag.rsplit('}', 1)[-1]
    return tag


def word_attr(el, name):
    return el.attrib.get(W_TAG + name) or el.attrib.get(name) or ''


def paragraph_style_id(p_el):
    ppr = p_el.find(W_TAG + 'pPr')
    if ppr is None:
        return ''
    pstyle = ppr.find(W_TAG + 'pStyle')
    if pstyle is None:
        return ''
    return word_attr(pstyle, 'val')


def heading_level_from_style_id(style_id):
    if not style_id:
        return 0
    compact = re.sub(r'[^a-z0-9]+', '', style_id.lower())
    if compact == 'title':
        return 1
    if compact == 'subtitle':
        return 2
    m = re.match(r'heading([1-9])$', compact)
    if m:
        return min(int(m.group(1)), 6)
    return 0


def paragraph_text_from_xml(p_el):
    parts = []
    for el in p_el.iter():
        name = local_name(el.tag)
        if name == 't':
            parts.append(el.text or '')
        elif name == 'tab':
            parts.append('\t')
        elif name in ('br', 'cr'):
            parts.append('\n')
    return ''.join(parts).strip()


def table_rows_from_xml(tbl_el):
    rows = []
    for tr in tbl_el.findall('.//' + W_TAG + 'tr'):
        cells = []
        for tc in tr.findall(W_TAG + 'tc'):
            paras = []
            for p in tc.findall('.//' + W_TAG + 'p'):
                t = paragraph_text_from_xml(p)
                if t:
                    paras.append(t.replace('|', '\\|'))
            cells.append(' <br> '.join(paras) if paras else ' ')
        if cells:
            rows.append(cells)
    return rows


def render_xml_table(tbl_el, out):
    rows = table_rows_from_xml(tbl_el)
    if not rows:
        return
    width = max(len(r) for r in rows)
    header = [(rows[0][i] if i < len(rows[0]) else ' ') for i in range(width)]
    out.append('| ' + ' | '.join(header) + ' |')
    out.append('|' + '|'.join([' --- '] * width) + '|')
    for r in rows[1:]:
        row_cells = [(r[i] if i < len(r) else ' ') for i in range(width)]
        out.append('| ' + ' | '.join(row_cells) + ' |')
    out.append('')


def extract_docx_fallback(target, word_dir, cwd, size_bytes):
    try:
        with zipfile.ZipFile(target, 'r') as zf:
            xml_bytes = zf.read('word/document.xml')
    except KeyError:
        emit({'ok': False, 'helper': 'docx_extract_text', 'error': 'DOCX fallback could not find word/document.xml', 'path': str(target)}, 8)
    except Exception as ex:
        emit({'ok': False, 'helper': 'docx_extract_text', 'error': 'DOCX fallback could not open ZIP/XML content: ' + str(ex), 'path': str(target)}, 8)

    try:
        root = ET.fromstring(xml_bytes)
    except Exception as ex:
        emit({'ok': False, 'helper': 'docx_extract_text', 'error': 'DOCX fallback could not parse document XML: ' + str(ex), 'path': str(target)}, 8)

    body = root.find(W_TAG + 'body')
    if body is None:
        emit({'ok': False, 'helper': 'docx_extract_text', 'error': 'DOCX fallback could not locate document body', 'path': str(target)}, 8)

    paragraph_count = 0
    table_count = 0
    heading_count = 0
    out = []
    out.append(f'# Extracted DOCX Text: {target.name}')
    out.append('')
    out.append('Generated by LlamaBoss controlled Python backend (`docx_extract_text`).')
    out.append('')
    out.append('> Note: Used the built-in DOCX fallback extractor because `python-docx` is not installed. Text and basic tables are extracted, but advanced Word formatting may be simplified.')
    out.append('')
    out.append('## Summary')
    out.append('')
    out.append(f'- Input file: `{target}`')
    out.append(f'- File size: {size_bytes} bytes')
    out.append('- Extractor: built-in ZIP/XML fallback')
    out.append('')
    out.append('## Content')
    out.append('')

    prev_was_blank = True
    for child in list(body):
        name = local_name(child.tag)
        if name == 'p':
            paragraph_count += 1
            text = paragraph_text_from_xml(child)
            level = heading_level_from_style_id(paragraph_style_id(child))
            if level > 0 and text:
                heading_count += 1
                if not prev_was_blank:
                    out.append('')
                out.append('#' * level + ' ' + text)
                out.append('')
                prev_was_blank = True
            elif text:
                out.append(text)
                out.append('')
                prev_was_blank = True
            elif not prev_was_blank:
                out.append('')
                prev_was_blank = True
        elif name == 'tbl':
            table_count += 1
            if not prev_was_blank:
                out.append('')
            render_xml_table(child, out)
            prev_was_blank = True

    word_dir.mkdir(parents=True, exist_ok=True)
    output_name = safe_name(target.stem) + '_extracted_text.md'
    output_path = unique_path(word_dir, output_name)
    output_text = '\n'.join(out).rstrip() + '\n'
    output_path.write_text(output_text, encoding='utf-8')

    emit({
        'ok': True,
        'helper': 'docx_extract_text',
        'input_path':       str(target),
        'output_path':      str(output_path),
        'output_filename':  output_path.name,
        'cwd':              str(cwd),
        'word_dir':         str(word_dir),
        'file_size_bytes':  size_bytes,
        'paragraph_count':  paragraph_count,
        'heading_count':    heading_count,
        'table_count':      table_count,
        'extracted_char_count': len(output_text),
        'extractor':        'builtin_zip_xml_fallback',
        'fallback_used':    True,
    })


def heading_level(style_name):
    # python-docx exposes built-in heading styles as "Heading 1" through
    # "Heading 9".  Title and Subtitle map to # and ## by convention so
    # the extracted Markdown reads naturally for typical Word templates.
    if not style_name:
        return 0
    name = style_name.strip()
    lower = name.lower()
    if lower == 'title':
        return 1
    if lower == 'subtitle':
        return 2
    m = re.match(r'^heading\s+(\d+)$', lower)
    if m:
        try:
            n = int(m.group(1))
            if 1 <= n <= 9:
                return min(n, 6)  # Markdown caps at h6
        except Exception:
            return 0
    return 0


def list_marker(style_name):
    if not style_name:
        return None
    lower = style_name.lower()
    if 'list bullet' in lower or 'bulleted list' in lower:
        return '- '
    if 'list number' in lower or 'numbered list' in lower:
        return '1. '
    if lower.startswith('list paragraph'):
        # Generic list paragraph — bullet is the safer default
        return '- '
    return None


def md_escape(s):
    if s is None:
        return ''
    return s.replace('\x00', '')


def render_paragraph(p, out, prev_was_blank):
    text = md_escape((p.text or '').strip())
    style = (p.style.name if p.style is not None else '') or ''
    level = heading_level(style)
    marker = list_marker(style)

    if level > 0 and text:
        if not prev_was_blank:
            out.append('')
        out.append('#' * level + ' ' + text)
        out.append('')
        return True   # this counts as ending with a blank line
    if marker is not None and text:
        out.append(marker + text)
        return False
    if text:
        out.append(text)
        out.append('')
        return True
    # Empty paragraph -- emit a blank line only if we don't already
    # have one trailing the buffer.
    if not prev_was_blank:
        out.append('')
        return True
    return prev_was_blank


def render_table(table, out):
    rows = list(table.rows)
    if not rows:
        return
    width = max(len(r.cells) for r in rows)
    if width == 0:
        return

    def cell_text(cell):
        # A cell may contain multiple paragraphs; join with <br> so the
        # Markdown table cell renders coherently.  Strip surrounding
        # whitespace and pipes that would break the table.
        parts = []
        for p in cell.paragraphs:
            t = (p.text or '').strip()
            if t:
                parts.append(t.replace('|', '\\|'))
        return ' <br> '.join(parts) if parts else ' '

    # Header row uses the first row of the table.  Markdown tables
    # require a header even if the source doesn't have one; this mirrors
    # how most rendered Word tables read.
    header = [cell_text(rows[0].cells[i]) if i < len(rows[0].cells) else ' '
              for i in range(width)]
    out.append('| ' + ' | '.join(header) + ' |')
    out.append('|' + '|'.join([' --- '] * width) + '|')

    for r in rows[1:]:
        row_cells = [cell_text(r.cells[i]) if i < len(r.cells) else ' '
                     for i in range(width)]
        out.append('| ' + ' | '.join(row_cells) + ' |')

    out.append('')


def main():
    if len(sys.argv) != 3:
        emit({'ok': False, 'helper': 'docx_extract_text', 'error': 'docx_extract_text expects exactly two internal arguments: input path and fixed output directory'}, 2)

    cwd = Path.cwd().resolve()
    raw_arg = sys.argv[1]
    word_dir = Path(sys.argv[2]).resolve()

    target = Path(raw_arg)
    if not target.is_absolute():
        target = cwd / target
    target = target.resolve()


    if target.suffix.lower() not in ('.docx', '.docm'):
        emit({'ok': False, 'helper': 'docx_extract_text', 'error': 'Only .docx or .docm files are supported by docx_extract_text', 'path': str(target)}, 4)

    if not target.exists() or not target.is_file():
        emit({'ok': False, 'helper': 'docx_extract_text', 'error': 'File not found', 'path': str(target)}, 5)

    max_file_bytes = 50 * 1024 * 1024
    size_bytes = target.stat().st_size
    if size_bytes > max_file_bytes:
        emit({'ok': False, 'helper': 'docx_extract_text', 'error': 'DOCX is larger than the first-phase 50 MB safety cap', 'path': str(target), 'file_size_bytes': size_bytes}, 7)

    docx_mod, import_errors = load_docx_module()
    if docx_mod is None:
        extract_docx_fallback(target, word_dir, cwd, size_bytes)

    try:
        doc = docx_mod.Document(str(target))
    except Exception as ex:
        emit({'ok': False, 'helper': 'docx_extract_text', 'error': 'Could not open DOCX: ' + str(ex), 'path': str(target)}, 6)

    # Walk body children in document order so paragraphs and tables
    # interleave the way they do in the source document.  python-docx
    # exposes the body XML element directly; we identify each child by
    # its tag and find the matching wrapper object via the element id.
    try:
        from docx.oxml.ns import qn
        from docx.text.paragraph import Paragraph
        from docx.table import Table
    except Exception as ex:
        emit({'ok': False, 'helper': 'docx_extract_text', 'error': 'python-docx internals not available: ' + str(ex)}, 8)

    body = doc.element.body
    paragraph_count = 0
    table_count = 0
    heading_count = 0
    out = []
    out.append(f'# Extracted DOCX Text: {target.name}')
    out.append('')
    out.append('Generated by LlamaBoss controlled Python backend (`docx_extract_text`).')
    out.append('')
    out.append('## Summary')
    out.append('')
    out.append(f'- Input file: `{target}`')
    out.append(f'- File size: {size_bytes} bytes')
    out.append('')
    out.append('## Content')
    out.append('')

    prev_was_blank = True
    for child in body.iterchildren():
        tag = child.tag
        if tag == qn('w:p'):
            paragraph_count += 1
            p = Paragraph(child, doc.part)
            style_name = (p.style.name if p.style is not None else '') or ''
            if heading_level(style_name) > 0 and (p.text or '').strip():
                heading_count += 1
            prev_was_blank = render_paragraph(p, out, prev_was_blank)
        elif tag == qn('w:tbl'):
            table_count += 1
            t = Table(child, doc.part)
            if not prev_was_blank:
                out.append('')
            render_table(t, out)
            prev_was_blank = True
        # Sections (w:sectPr) and other non-content elements are skipped.

    word_dir.mkdir(parents=True, exist_ok=True)
    output_name = safe_name(target.stem) + '_extracted_text.md'
    output_path = unique_path(word_dir, output_name)

    output_text = '\n'.join(out).rstrip() + '\n'
    output_path.write_text(output_text, encoding='utf-8')

    extracted_chars = len(output_text)

    emit({
        'ok': True,
        'helper': 'docx_extract_text',
        'input_path':       str(target),
        'output_path':      str(output_path),
        'output_filename':  output_path.name,
        'cwd':              str(cwd),
        'word_dir':         str(word_dir),
        'file_size_bytes':  size_bytes,
        'paragraph_count':  paragraph_count,
        'heading_count':    heading_count,
        'table_count':      table_count,
        'extracted_char_count': extracted_chars,
        'extractor':        'python-docx',
        'fallback_used':    False,
    })


if __name__ == '__main__':
    main()
)PY";

    return WriteHelperScript("docx_extract_text", script, scriptPathOut, errorOut);
}


bool EnsureDocxInspectScript(std::string& scriptPathOut,
                             std::string& errorOut)
{
    const char* script = R"PY(
import json
import re
import site
import sys
import zipfile
import xml.etree.ElementTree as ET
from pathlib import Path


def emit(obj, code=0):
    print(json.dumps(obj, indent=2, ensure_ascii=False))
    raise SystemExit(code)


def is_relative_to(child, parent):
    try:
        child.relative_to(parent)
        return True
    except Exception:
        return False


def load_docx_module():
    errors = []

    def try_import():
        try:
            import docx
            return docx
        except Exception as ex:
            errors.append('python-docx: ' + str(ex))
            return None

    mod = try_import()
    if mod is not None:
        return mod, errors

    try:
        user_site = getattr(site, 'USER_SITE', None)
        if user_site:
            site.addsitedir(user_site)
    except Exception as ex:
        errors.append('user-site enable failed: ' + str(ex))

    mod = try_import()
    return mod, errors


def heading_level(style_name):
    if not style_name:
        return 0
    name = style_name.strip()
    lower = name.lower()
    if lower == 'title':
        return 1
    if lower == 'subtitle':
        return 2
    m = re.match(r'^heading\s+(\d+)$', lower)
    if m:
        try:
            n = int(m.group(1))
            if 1 <= n <= 9:
                return n
        except Exception:
            return 0
    return 0



W_NS = 'http://schemas.openxmlformats.org/wordprocessingml/2006/main'
W_TAG = '{' + W_NS + '}'


def local_name(tag):
    if not isinstance(tag, str):
        return ''
    if tag.startswith('{'):
        return tag.rsplit('}', 1)[-1]
    return tag


def word_attr(el, name):
    return el.attrib.get(W_TAG + name) or el.attrib.get(name) or ''


def paragraph_style_id(p_el):
    ppr = p_el.find(W_TAG + 'pPr')
    if ppr is None:
        return ''
    pstyle = ppr.find(W_TAG + 'pStyle')
    if pstyle is None:
        return ''
    return word_attr(pstyle, 'val')


def heading_level_from_style_id(style_id):
    if not style_id:
        return 0
    compact = re.sub(r'[^a-z0-9]+', '', style_id.lower())
    if compact == 'title':
        return 1
    if compact == 'subtitle':
        return 2
    m = re.match(r'heading([1-9])$', compact)
    if m:
        return min(int(m.group(1)), 6)
    return 0


def paragraph_text_from_xml(p_el):
    parts = []
    for el in p_el.iter():
        name = local_name(el.tag)
        if name == 't':
            parts.append(el.text or '')
        elif name == 'tab':
            parts.append('\t')
        elif name in ('br', 'cr'):
            parts.append('\n')
    return ''.join(parts).strip()


def inspect_docx_fallback(target, cwd, size_bytes):
    try:
        with zipfile.ZipFile(target, 'r') as zf:
            names = set(zf.namelist())
            xml_bytes = zf.read('word/document.xml')
    except KeyError:
        emit({'ok': False, 'helper': 'docx_inspect', 'error': 'DOCX fallback could not find word/document.xml', 'path': str(target)}, 8)
    except Exception as ex:
        emit({'ok': False, 'helper': 'docx_inspect', 'error': 'DOCX fallback could not open ZIP/XML content: ' + str(ex), 'path': str(target)}, 8)

    try:
        root = ET.fromstring(xml_bytes)
    except Exception as ex:
        emit({'ok': False, 'helper': 'docx_inspect', 'error': 'DOCX fallback could not parse document XML: ' + str(ex), 'path': str(target)}, 8)

    body = root.find(W_TAG + 'body')
    if body is None:
        emit({'ok': False, 'helper': 'docx_inspect', 'error': 'DOCX fallback could not locate document body', 'path': str(target)}, 8)

    paragraph_count = 0
    table_count = 0
    headings = []
    tables = []
    styles_in_use = set()
    HEADING_CAP = 100
    TABLE_CAP = 100

    for child in list(body):
        name = local_name(child.tag)
        if name == 'p':
            paragraph_count += 1
            style_id = paragraph_style_id(child)
            if style_id:
                styles_in_use.add(style_id)
            level = heading_level_from_style_id(style_id)
            text = paragraph_text_from_xml(child)
            if level > 0 and text and len(headings) < HEADING_CAP:
                snippet = text if len(text) <= 200 else (text[:200] + '...')
                headings.append({'level': level, 'text': snippet})
        elif name == 'tbl':
            table_count += 1
            if len(tables) < TABLE_CAP:
                rows = child.findall('.//' + W_TAG + 'tr')
                rcount = len(rows)
                ccount = 0
                for r in rows:
                    ccount = max(ccount, len(r.findall(W_TAG + 'tc')))
                tables.append({'rows': rcount, 'cols': ccount})

    section_count = len(body.findall(W_TAG + 'sectPr'))
    has_images = any(n.startswith('word/media/') for n in names)

    emit({
        'ok': True,
        'helper': 'docx_inspect',
        'input_path':      str(target),
        'cwd':             str(cwd),
        'file_size_bytes': size_bytes,
        'paragraph_count': paragraph_count,
        'heading_count':   len(headings),
        'headings':        headings,
        'headings_truncated': len(headings) >= HEADING_CAP,
        'table_count':     table_count,
        'tables':          tables,
        'tables_truncated': len(tables) >= TABLE_CAP,
        'section_count':   section_count,
        'has_images':      has_images,
        'styles_in_use':   sorted(styles_in_use),
        'extractor':       'builtin_zip_xml_fallback',
        'fallback_used':   True,
    })


def main():
    if len(sys.argv) != 2:
        emit({'ok': False, 'helper': 'docx_inspect', 'error': 'docx_inspect expects exactly one internal argument: input path'}, 2)

    cwd = Path.cwd().resolve()
    raw_arg = sys.argv[1]

    target = Path(raw_arg)
    if not target.is_absolute():
        target = cwd / target
    target = target.resolve()


    if target.suffix.lower() not in ('.docx', '.docm'):
        emit({'ok': False, 'helper': 'docx_inspect', 'error': 'Only .docx or .docm files are supported by docx_inspect', 'path': str(target)}, 4)

    if not target.exists() or not target.is_file():
        emit({'ok': False, 'helper': 'docx_inspect', 'error': 'File not found', 'path': str(target)}, 5)

    max_file_bytes = 50 * 1024 * 1024
    size_bytes = target.stat().st_size
    if size_bytes > max_file_bytes:
        emit({'ok': False, 'helper': 'docx_inspect', 'error': 'DOCX is larger than the first-phase 50 MB safety cap', 'path': str(target), 'file_size_bytes': size_bytes}, 7)

    docx_mod, import_errors = load_docx_module()
    if docx_mod is None:
        inspect_docx_fallback(target, cwd, size_bytes)

    try:
        doc = docx_mod.Document(str(target))
    except Exception as ex:
        emit({'ok': False, 'helper': 'docx_inspect', 'error': 'Could not open DOCX: ' + str(ex), 'path': str(target)}, 6)

    try:
        from docx.oxml.ns import qn
        from docx.text.paragraph import Paragraph
        from docx.table import Table
    except Exception as ex:
        emit({'ok': False, 'helper': 'docx_inspect', 'error': 'python-docx internals not available: ' + str(ex)}, 8)

    body = doc.element.body
    paragraph_count = 0
    table_count = 0
    headings = []          # list of {level, text}
    tables = []            # list of {rows, cols}
    styles_in_use = set()

    # Cap heading list and table list so a giant document doesn't
    # produce a multi-megabyte JSON summary.  100 of each is plenty
    # for navigation.
    HEADING_CAP = 100
    TABLE_CAP   = 100

    for child in body.iterchildren():
        tag = child.tag
        if tag == qn('w:p'):
            paragraph_count += 1
            p = Paragraph(child, doc.part)
            style_name = (p.style.name if p.style is not None else '') or ''
            if style_name:
                styles_in_use.add(style_name)
            level = heading_level(style_name)
            text = (p.text or '').strip()
            if level > 0 and text and len(headings) < HEADING_CAP:
                # Truncate very long headings so the JSON stays compact.
                snippet = text if len(text) <= 200 else (text[:200] + '...')
                headings.append({'level': level, 'text': snippet})
        elif tag == qn('w:tbl'):
            table_count += 1
            if len(tables) < TABLE_CAP:
                t = Table(child, doc.part)
                rows = list(t.rows)
                rcount = len(rows)
                ccount = max((len(r.cells) for r in rows), default=0)
                tables.append({'rows': rcount, 'cols': ccount})

    section_count = 0
    try:
        section_count = len(doc.sections)
    except Exception:
        section_count = 0

    has_images = False
    try:
        # Inline shapes are images embedded in paragraphs.  We don't
        # extract them; we just report their presence so the model
        # knows the doc has visuals.
        has_images = len(doc.inline_shapes) > 0
    except Exception:
        has_images = False

    emit({
        'ok': True,
        'helper': 'docx_inspect',
        'input_path':      str(target),
        'cwd':             str(cwd),
        'file_size_bytes': size_bytes,
        'paragraph_count': paragraph_count,
        'heading_count':   len(headings),
        'headings':        headings,
        'headings_truncated': len(headings) >= HEADING_CAP,
        'table_count':     table_count,
        'tables':          tables,
        'tables_truncated': len(tables) >= TABLE_CAP,
        'section_count':   section_count,
        'has_images':      has_images,
        'styles_in_use':   sorted(styles_in_use),
        'extractor':       'python-docx',
        'fallback_used':   False,
    })


if __name__ == '__main__':
    main()
)PY";

    return WriteHelperScript("docx_inspect", script, scriptPathOut, errorOut);
}

bool JsonStringField(const std::string& json,
                     const std::string& field,
                     std::string& out)
{
    std::string key = "\"" + field + "\"";
    size_t k = json.find(key);
    if (k == std::string::npos) return false;
    size_t colon = json.find(':', k + key.size());
    if (colon == std::string::npos) return false;
    size_t q = json.find('"', colon + 1);
    if (q == std::string::npos) return false;

    std::string value;
    bool esc = false;
    for (size_t i = q + 1; i < json.size(); ++i) {
        char ch = json[i];
        if (esc) {
            switch (ch) {
            case '"': value.push_back('"'); break;
            case '\\': value.push_back('\\'); break;
            case '/': value.push_back('/'); break;
            case 'b': value.push_back('\b'); break;
            case 'f': value.push_back('\f'); break;
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            default: value.push_back(ch); break;
            }
            esc = false;
        } else if (ch == '\\') {
            esc = true;
        } else if (ch == '"') {
            out = value;
            return true;
        } else {
            value.push_back(ch);
        }
    }
    return false;
}

size_t FileSizeBytes(const std::string& path)
{
    try {
        std::ifstream f(Utf8ToWide(path), std::ios::binary | std::ios::ate);
        if (!f) return 0;
        return static_cast<size_t>(f.tellg());
    } catch (...) {
        return 0;
    }
}

int CountFileLines(const std::string& path)
{
    try {
        std::ifstream f(Utf8ToWide(path), std::ios::binary);
        if (!f) return 0;
        std::string data((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        if (data.empty()) return 0;
        int n = 0;
        for (char c : data) if (c == '\n') ++n;
        if (data.back() != '\n') ++n;
        return n;
    } catch (...) {
        return 0;
    }
}

std::string BaseNameOf(const std::string& path)
{
    size_t p = path.find_last_of("/\\");
    return (p == std::string::npos) ? path : path.substr(p + 1);
}


std::string SafeOutputStemForTool(const std::string& text, const std::string& fallback)
{
    std::string out;
    out.reserve(std::min<size_t>(text.size(), 64));
    for (char ch : text) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c)) out.push_back(static_cast<char>(std::tolower(c)));
        else if (c == '_' || c == '-' || c == '.') out.push_back(ch);
        else if (std::isspace(c) || c == ':' || c == '\\' || c == '/' || c == '|') {
            if (!out.empty() && out.back() != '_') out.push_back('_');
        }
        if (out.size() >= 48) break;
    }
    while (!out.empty() && (out.back() == '_' || out.back() == '.')) out.pop_back();
    if (out.empty()) out = fallback;
    return out;
}

std::string ToolOutputsDirForCwd(const std::string& cwd)
{
    return LaneDirForCwd(cwd, "ToolOutputs");
}

std::string UniqueToolOutputPath(const std::string& dir,
                                 const std::string& stem,
                                 const std::string& suffix,
                                 std::string& displayNameOut)
{
    std::string safeStem = SafeOutputStemForTool(stem, "tool_output");
    for (int i = 0; i < 1000; ++i) {
        displayNameOut = (i == 0)
            ? (safeStem + suffix)
            : (safeStem + "_" + std::to_string(i + 1) + suffix);
        std::string path = JoinPath(dir, displayNameOut);
        DWORD attrs = GetFileAttributesW(Utf8ToWide(path).c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) return path;
    }
    displayNameOut.clear();
    return std::string();
}

size_t CountTextLinesForOutput(const std::string& s)
{
    if (s.empty()) return 0;
    size_t n = 0;
    for (char c : s) if (c == '\n') ++n;
    if (s.back() != '\n') ++n;
    return n;
}

std::vector<std::string> SplitLinesForOutput(const std::string& s)
{
    std::vector<std::string> lines;
    std::istringstream in(s);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

std::string BuildHeadTailPreviewForOutput(const std::string& text,
                                          const std::string& displayName,
                                          bool streamWasCapped)
{
    constexpr size_t kHeadLines = 80;
    constexpr size_t kTailLines = 30;

    std::vector<std::string> lines = SplitLinesForOutput(text);
    const size_t totalLines = lines.empty() && !text.empty() ? 1 : lines.size();

    std::ostringstream out;
    out << "Large output saved to " << displayName << ".\n";
    out << "Showing preview";
    if (totalLines > 0) out << " of " << totalLines << " captured line" << (totalLines == 1 ? "" : "s");
    out << ".";
    if (streamWasCapped) out << " Output hit the capture cap; the saved file contains the captured portion.";
    out << "\n\n";

    if (lines.empty()) {
        out << text;
        return out.str();
    }

    if (lines.size() <= kHeadLines + kTailLines) {
        for (const auto& line : lines) out << line << "\n";
        return out.str();
    }

    for (size_t i = 0; i < kHeadLines; ++i) out << lines[i] << "\n";
    out << "\n[... " << (lines.size() - kHeadLines - kTailLines)
        << " lines omitted; open " << displayName << " for full captured output ...]\n\n";
    for (size_t i = lines.size() - kTailLines; i < lines.size(); ++i) out << lines[i] << "\n";
    return out.str();
}

bool WriteUtf8TextFileForOutput(const std::string& path, const std::string& content)
{
    try {
        std::ofstream f(Utf8ToWide(path), std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
        return f.good();
    } catch (...) {
        return false;
    }
}

bool ShouldExternalizeOutput(const std::string& text)
{
    constexpr size_t kMaxInlineBytes = 16 * 1024;
    constexpr size_t kMaxInlineLines = 120;
    return text.size() > kMaxInlineBytes || CountTextLinesForOutput(text) > kMaxInlineLines;
}

void ExternalizeOnePythonStream(PythonRunResult& result,
                                std::string& streamText,
                                const std::string& cwd,
                                const std::string& stem,
                                const std::string& streamName,
                                bool streamWasCapped)
{
    if (!ShouldExternalizeOutput(streamText)) return;

    std::string dir = ToolOutputsDirForCwd(cwd);
    wxFileName::Mkdir(wxString::FromUTF8(dir.c_str()), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);

    std::string displayName;
    std::string path = UniqueToolOutputPath(dir, stem + "_" + streamName, ".txt", displayName);
    if (path.empty()) return;

    std::string fileBody = streamText;
    if (streamWasCapped) {
        if (!fileBody.empty() && fileBody.back() != '\n') fileBody += "\n";
        fileBody += "\n[LlamaBoss output capture cap reached; additional output was discarded.]\n";
    }

    if (!WriteUtf8TextFileForOutput(path, fileBody)) return;

    PresentedFile f;
    f.displayName = displayName;
    f.language    = "text";
    f.diskPath    = path;
    f.sizeBytes   = FileSizeBytes(path);
    f.lineCount   = static_cast<int>(CountTextLinesForOutput(fileBody));
    result.presentedFiles.push_back(std::move(f));

    streamText = BuildHeadTailPreviewForOutput(streamText, displayName, streamWasCapped);
}

void ApplyLargePythonRunOutputHandling(PythonRunResult& result, const std::string& cwd)
{
    if (!ShouldExternalizeOutput(result.stdoutText) &&
        !ShouldExternalizeOutput(result.stderrText)) {
        return;
    }

    std::string stem = SafeOutputStemForTool(result.commandEcho.empty()
        ? std::string("python_run_output")
        : result.commandEcho,
        "python_run_output");
    ExternalizeOnePythonStream(result, result.stdoutText, cwd, stem, "stdout", result.truncated);
    ExternalizeOnePythonStream(result, result.stderrText, cwd, stem, "stderr", result.truncated);
}

void AttachCsvReportArtifact(PythonRunResult& result)
{
    if (result.exitCode != 0 || result.stdoutText.empty()) return;

    std::string outputPath;
    if (!JsonStringField(result.stdoutText, "output_path", outputPath)) return;
    if (outputPath.empty()) return;

    std::string outputName;
    if (!JsonStringField(result.stdoutText, "output_filename", outputName) || outputName.empty()) {
        outputName = BaseNameOf(outputPath);
    }

    size_t size = FileSizeBytes(outputPath);
    if (size == 0) return;

    PresentedFile f;
    f.displayName = outputName.empty() ? std::string("csv_report.md") : outputName;
    f.language    = "markdown";
    f.diskPath    = outputPath;
    f.sizeBytes   = size;
    f.lineCount   = CountFileLines(outputPath);
    result.presentedFiles.push_back(std::move(f));
}


void AttachXlsxReportArtifact(PythonRunResult& result)
{
    if (result.exitCode != 0 || result.stdoutText.empty()) return;

    std::string outputPath;
    if (!JsonStringField(result.stdoutText, "output_path", outputPath)) return;
    if (outputPath.empty()) return;

    std::string outputName;
    if (!JsonStringField(result.stdoutText, "output_filename", outputName) || outputName.empty()) {
        outputName = BaseNameOf(outputPath);
    }

    size_t size = FileSizeBytes(outputPath);
    if (size == 0) return;

    PresentedFile f;
    f.displayName = outputName.empty() ? std::string("xlsx_report.md") : outputName;
    f.language    = "markdown";
    f.diskPath    = outputPath;
    f.sizeBytes   = size;
    f.lineCount   = CountFileLines(outputPath);
    result.presentedFiles.push_back(std::move(f));
}


void AttachPdfExtractTextArtifact(PythonRunResult& result)
{
    if (result.exitCode != 0 || result.stdoutText.empty()) return;

    std::string outputPath;
    if (!JsonStringField(result.stdoutText, "output_path", outputPath)) return;
    if (outputPath.empty()) return;

    std::string outputName;
    if (!JsonStringField(result.stdoutText, "output_filename", outputName) || outputName.empty()) {
        outputName = BaseNameOf(outputPath);
    }

    size_t size = FileSizeBytes(outputPath);
    if (size == 0) return;

    PresentedFile f;
    f.displayName = outputName.empty() ? std::string("pdf_extracted_text.md") : outputName;
    f.language    = "markdown";
    f.diskPath    = outputPath;
    f.sizeBytes   = size;
    f.lineCount   = CountFileLines(outputPath);
    result.presentedFiles.push_back(std::move(f));
}

void AttachDocxExtractTextArtifact(PythonRunResult& result)
{
    if (result.exitCode != 0 || result.stdoutText.empty()) return;

    std::string outputPath;
    if (!JsonStringField(result.stdoutText, "output_path", outputPath)) return;
    if (outputPath.empty()) return;

    std::string outputName;
    if (!JsonStringField(result.stdoutText, "output_filename", outputName) || outputName.empty()) {
        outputName = BaseNameOf(outputPath);
    }

    size_t size = FileSizeBytes(outputPath);
    if (size == 0) return;

    PresentedFile f;
    f.displayName = outputName.empty() ? std::string("docx_extracted_text.md") : outputName;
    f.language    = "markdown";
    f.diskPath    = outputPath;
    f.sizeBytes   = size;
    f.lineCount   = CountFileLines(outputPath);
    result.presentedFiles.push_back(std::move(f));
}

void AttachPdfFillFormArtifact(PythonRunResult& result)
{
    if (result.exitCode != 0 || result.stdoutText.empty()) return;

    std::string outputPath;
    if (!JsonStringField(result.stdoutText, "output_path", outputPath)) return;
    if (outputPath.empty()) return;

    std::string outputName;
    if (!JsonStringField(result.stdoutText, "output_filename", outputName) || outputName.empty()) {
        outputName = BaseNameOf(outputPath);
    }

    size_t size = FileSizeBytes(outputPath);
    if (size == 0) return;

    PresentedFile f;
    f.displayName = outputName.empty() ? std::string("filled.pdf") : outputName;
    // Empty language hint -- this is a binary PDF, not source.  The
    // renderer treats this as "no syntax highlight" and the [Open]
    // button hands off to the OS default app (Acrobat, Edge, etc.)
    // because IsLikelyCodeFile() returns false for .pdf.
    f.language    = "";
    f.diskPath    = outputPath;
    f.sizeBytes   = size;
    f.lineCount   = 0;
    result.presentedFiles.push_back(std::move(f));
}

bool ResolveRunnableScriptPath(const std::string& requested,
                               const std::string& cwd,
                               const std::string& activeProjectRoot,
                               std::string&       pathOut,
                               std::string&       displayNameOut,
                               std::string&       errorOut)
{
    std::string name = requested;
    size_t a = name.find_first_not_of(" \t\r\n");
    size_t b = name.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) {
        errorOut = "python_run_script requires a script filename.";
        return false;
    }
    name = name.substr(a, b - a + 1);

    if (name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos ||
        name.find(':') != std::string::npos) {
        errorOut = "python_run_script accepts a filename only, not a path. Scripts must already be in the conversation Scripts folder, the active project Workflows folder, or the LlamaBoss Skills folder.";
        return false;
    }

    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) {
        name += ".py";
    } else if (LowerLocal(name.substr(dot)) != ".py") {
        errorOut = "python_run_script only runs .py files from the conversation Scripts folder, the active project Workflows folder, or the LlamaBoss Skills folder.";
        return false;
    }

    // Reuse the same filename sanitizer semantics as script creation.
    std::string safe = path_safety::SanitizeFilename(name, "");
    if (safe.empty() || safe != name) {
        errorOut = "Unsafe Python script filename: " + name;
        return false;
    }

    std::string fullPath = JoinPath(UserScriptsDirForCwd(cwd), name);
    if (FileExistsRegular(fullPath)) {
        pathOut = fullPath;
        displayNameOut = name;
        return true;
    }

    if (!activeProjectRoot.empty()) {
        ProjectWorkflowScriptInfo script;
        std::string projectError;
        if (ProjectManager::ResolveProjectWorkflowScript(activeProjectRoot, name, script, projectError)) {
            pathOut = script.path;
            displayNameOut = script.name;
            return true;
        }
    }

    // Skills fallback: project scope wins on filename collision because
    // we try project resolution first; this lane runs only when the
    // script wasn't found in the conversation Scripts folder or in any
    // attached project's Workflows folder.
    {
        ProjectWorkflowScriptInfo script;
        std::string globalError;
        if (ProjectManager::ResolveGlobalWorkflowScript(name, script, globalError)) {
            pathOut = script.path;
            displayNameOut = script.name;
            return true;
        }
    }

    errorOut = "Python script not found in this conversation's Scripts folder, the active project Workflows folder, or the LlamaBoss Skills folder. Conversation Scripts path checked: " + fullPath;
    return false;
}

void CollectFilesRecursive(const std::string& dir,
                           std::vector<std::string>& out,
                           size_t& seenCount,
                           size_t maxSeen)
{
    if (dir.empty() || seenCount >= maxSeen) return;

    std::wstring pattern = Utf8ToWide(JoinPath(dir, "*"));
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        if (seenCount >= maxSeen) break;

        std::wstring nameW = fd.cFileName;
        if (nameW == L"." || nameW == L"..") continue;

        std::string name = WideToUtf8(nameW);
        std::string path = JoinPath(dir, name);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            CollectFilesRecursive(path, out, seenCount, maxSeen);
        } else {
            ++seenCount;
            out.push_back(path);
        }
    } while (FindNextFileW(h, &fd));

    FindClose(h);
}

std::set<std::string> SnapshotLlamaBossFiles(const std::string& cwd)
{
    std::set<std::string> files;
    std::string root = ScanRootForCwd(cwd);
    if (root.empty()) return files;

    std::vector<std::string> paths;
    size_t seen = 0;
    CollectFilesRecursive(root, paths, seen, 5000);
    for (const auto& p : paths) files.insert(p);
    return files;
}

void AttachNewFilesUnderLlamaBoss(PythonRunResult& result,
                                  const std::set<std::string>& before,
                                  const std::string& cwd)
{
    std::set<std::string> after = SnapshotLlamaBossFiles(cwd);

    int attached = 0;
    int extra = 0;
    constexpr int kMaxAttach = 12;
    constexpr size_t kMaxFileBytes = 25 * 1024 * 1024;

    for (const auto& path : after) {
        if (before.find(path) != before.end()) continue;

        size_t size = FileSizeBytes(path);
        if (size > kMaxFileBytes) continue;

        if (attached >= kMaxAttach) {
            ++extra;
            continue;
        }

        PresentedFile f;
        f.displayName = BaseNameOf(path);
        f.language    = LanguageForFile(path);
        f.diskPath    = path;
        f.sizeBytes   = size;
        f.lineCount   = CountFileLines(path);
        result.presentedFiles.push_back(std::move(f));
        ++attached;
    }

    if (extra > 0) {
        if (!result.stderrText.empty() && result.stderrText.back() != '\n')
            result.stderrText += "\r\n";
        result.stderrText += "[LlamaBoss detected additional created files but only attached the first " +
                             std::to_string(kMaxAttach) + " artifact cards.]\r\n";
    }
}

struct HandleGuard {
    HANDLE h = nullptr;
    HandleGuard() = default;
    explicit HandleGuard(HANDLE handle) : h(handle) {}
    ~HandleGuard() {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
    HANDLE release() { HANDLE r = h; h = nullptr; return r; }
};

void ReaderLoop(HANDLE readEnd,
                std::string& dest,
                std::mutex& destMutex,
                std::atomic<bool>& truncatedFlag)
{
    constexpr DWORD kChunk = 4096;
    char buf[kChunk];
    for (;;) {
        DWORD got = 0;
        BOOL ok = ReadFile(readEnd, buf, kChunk, &got, nullptr);
        if (!ok || got == 0) break;

        std::lock_guard<std::mutex> lk(destMutex);
        if (dest.size() < PythonRunner::kMaxOutputBytes) {
            size_t room = PythonRunner::kMaxOutputBytes - dest.size();
            size_t take = std::min<size_t>(got, room);
            dest.append(buf, take);
            if (take < got) truncatedFlag.store(true);
        } else {
            truncatedFlag.store(true);
        }
    }
}

std::wstring QuoteArg(const std::wstring& arg)
{
    // Minimal Windows argv quoting for normal file paths.  Helper
    // paths and helper args are generated/validated by C++ and passed
    // as data, never as code; still escape defensively.
    std::wstring out = L"\"";
    for (wchar_t ch : arg) {
        if (ch == L'\"') out += L"\\\"";
        else             out += ch;
    }
    out += L"\"";
    return out;
}

struct PythonCandidate {
    std::string label;
    std::wstring commandLine;
};

std::vector<PythonCandidate> BuildCandidates(const std::string& scriptPath,
                                             const std::vector<std::string>& helperArgs,
                                             bool isolatedMode = true)
{
    std::wstring script = QuoteArg(Utf8ToWide(scriptPath));
    std::wstring argText;
    for (const std::string& a : helperArgs) {
        argText += L" ";
        argText += QuoteArg(Utf8ToWide(a));
    }

    const std::wstring pyFlags = isolatedMode ? L" -3 -I -B " : L" -3 -B ";
    const std::wstring pythonFlags = isolatedMode ? L" -I -B " : L" -B ";

    return {
        { "py -3",   L"py.exe" + pyFlags + script + argText },
        { "python",  L"python.exe" + pythonFlags + script + argText },
        { "python3", L"python3.exe" + pythonFlags + script + argText },
    };
}

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

bool NormalizeAllowedPythonPackage(const std::string& requested,
                                   std::string&       packageOut,
                                   std::string&       errorOut)
{
    std::string p = LowerAscii(TrimAscii(requested));
    std::replace(p.begin(), p.end(), '_', '-');

    // Friendly aliases for import/module names the model or user may mention.
    if (p == "docx")      p = "python-docx";
    if (p == "fitz")      p = "pymupdf";
    if (p == "pil")       p = "pillow";
    if (p == "pptx")      p = "python-pptx";
    if (p == "bs4")       p = "beautifulsoup4";

    // No requirement specs, URLs, files, extras, or pip flags.  This tool is
    // intentionally one approved package name at a time.
    if (p.empty()) {
        errorOut = "python_install_package requires one package name.";
        return false;
    }
    for (char c : p) {
        const bool ok = (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') ||
                        c == '-';
        if (!ok) {
            errorOut = "python_install_package accepts one simple allowlisted package name only; no versions, paths, URLs, extras, flags, or requirements files.";
            return false;
        }
    }
    if (p.rfind("--", 0) == 0 ||
        p.find("requirements") != std::string::npos) {
        errorOut = "python_install_package does not accept pip flags or requirements files.";
        return false;
    }

    static const char* kAllowed[] = {
        "python-docx",
        "openpyxl",
        "pymupdf",
        "pypdf",
        "pypdfium2",
        "pandas",
        "pillow",
        "reportlab",
        "matplotlib",
        "python-pptx",
        "xlsxwriter",
        "beautifulsoup4",
        "lxml"
    };
    for (const char* allowed : kAllowed) {
        if (p == allowed) {
            packageOut = p;
            errorOut.clear();
            return true;
        }
    }

    errorOut = "Package '" + p + "' is not on the first-phase allowlist. Allowed packages: python-docx, openpyxl, pymupdf, pypdf, pypdfium2, pandas, pillow, reportlab, matplotlib, python-pptx, xlsxwriter, beautifulsoup4, lxml.";
    return false;
}

std::vector<PythonCandidate> BuildPipInstallCandidates(const std::string& packageName)
{
    const std::wstring pkg = QuoteArg(Utf8ToWide(packageName));
    const std::wstring common = L" -B -m pip install --user --disable-pip-version-check " + pkg;

    return {
        { "py -3",   L"py.exe -3" + common },
        { "python",  L"python.exe" + common },
        { "python3", L"python3.exe" + common },
    };
}

class PythonWorkerThread : public wxThread {
public:
    PythonWorkerThread(wxEvtHandler* evtHandler,
                       const std::string& helperName,
                       const std::string& helperArg,
                       const std::string& cwd,
                       unsigned long      timeoutMs,
                       std::string        activeProjectRoot,
                       std::shared_ptr<std::atomic<bool>> cancelFlag,
                       std::shared_ptr<std::atomic<bool>> runningFlag,
                       std::weak_ptr<std::atomic<bool>> aliveToken)
        : wxThread(wxTHREAD_DETACHED)
        , m_evtHandler(evtHandler)
        , m_helperName(helperName)
        , m_helperArg(helperArg)
        , m_cwd(cwd)
        , m_timeoutMs(timeoutMs ? timeoutMs : PythonRunner::kDefaultTimeoutMs)
        , m_activeProjectRoot(std::move(activeProjectRoot))
        , m_cancelFlag(std::move(cancelFlag))
        , m_runningFlag(std::move(runningFlag))
        , m_aliveToken(std::move(aliveToken))
    {}

protected:
    ExitCode Entry() override
    {
        PythonRunResult result;
        result.toolName    = m_helperName;
        result.helperName  = m_helperName;
        result.commandEcho = m_helperName;

        double t0 = NowSec();
        RunOne(result);
        if (m_helperName == "python_run_script") {
            ApplyLargePythonRunOutputHandling(result, m_cwd);
        }
        result.elapsedSec = NowSec() - t0;

        if (m_runningFlag) m_runningFlag->store(false);
        PostCompletion(std::move(result));
        return (ExitCode)0;
    }

private:
    void RunOne(PythonRunResult& result)
    {
        std::string scriptPath, scriptError;
        std::vector<std::string> helperArgs;
        const bool isUserScript = (m_helperName == "python_run_script");
        const bool isPackageInstall = (m_helperName == "python_install_package");
        std::set<std::string> beforeFiles;

        if (m_helperName == "python_install_package") {
            std::string packageName;
            if (!NormalizeAllowedPythonPackage(m_helperArg, packageName, scriptError)) {
                result.stderrText = scriptError;
                result.exitCode = -1;
                return;
            }
            helperArgs.push_back(packageName);
            result.commandEcho = "python_install_package " + packageName;
        } else if (m_helperName == "python_health") {
            if (!EnsurePythonHealthScript(scriptPath, scriptError)) {
                result.stderrText = scriptError;
                result.exitCode = -1;
                return;
            }
            result.commandEcho = "python_health";
        } else if (m_helperName == "csv_inspect") {
            if (!EnsureCsvInspectScript(scriptPath, scriptError)) {
                result.stderrText = scriptError;
                result.exitCode = -1;
                return;
            }
            helperArgs.push_back(m_helperArg);
            result.commandEcho = m_helperArg.empty()
                ? std::string("csv_inspect")
                : std::string("csv_inspect ") + m_helperArg;
        } else if (m_helperName == "csv_report") {
            if (!EnsureCsvReportScript(scriptPath, scriptError)) {
                result.stderrText = scriptError;
                result.exitCode = -1;
                return;
            }
            helperArgs.push_back(m_helperArg);
            helperArgs.push_back(DocumentsDirForCwd(m_cwd));
            result.commandEcho = m_helperArg.empty()
                ? std::string("csv_report")
                : std::string("csv_report ") + m_helperArg;
        } else if (m_helperName == "csv_to_xlsx") {
            if (!EnsureCsvToXlsxScript(scriptPath, scriptError)) {
                result.stderrText = scriptError;
                result.exitCode = -1;
                return;
            }
            helperArgs.push_back(m_helperArg);
            helperArgs.push_back(SpreadsheetsDirForCwd(m_cwd));
            result.commandEcho = m_helperArg.empty()
                ? std::string("csv_to_xlsx")
                : std::string("csv_to_xlsx ") + m_helperArg;
        } else if (m_helperName == "xlsx_inspect") {
            if (!EnsureXlsxInspectScript(scriptPath, scriptError)) {
                result.stderrText = scriptError;
                result.exitCode = -1;
                return;
            }
            helperArgs.push_back(m_helperArg);
            result.commandEcho = m_helperArg.empty()
                ? std::string("xlsx_inspect")
                : std::string("xlsx_inspect ") + m_helperArg;
        } else if (m_helperName == "xlsx_report") {
            if (!EnsureXlsxReportScript(scriptPath, scriptError)) {
                result.stderrText = scriptError;
                result.exitCode = -1;
                return;
            }
            helperArgs.push_back(m_helperArg);
            helperArgs.push_back(DocumentsDirForCwd(m_cwd));
            result.commandEcho = m_helperArg.empty()
                ? std::string("xlsx_report")
                : std::string("xlsx_report ") + m_helperArg;
        } else if (m_helperName == "pdf_extract_text") {
            if (!EnsurePdfExtractTextScript(scriptPath, scriptError)) {
                result.stderrText = scriptError;
                result.exitCode = -1;
                return;
            }
            helperArgs.push_back(m_helperArg);
            helperArgs.push_back(PdfsDirForCwd(m_cwd));
            result.commandEcho = m_helperArg.empty()
                ? std::string("pdf_extract_text")
                : std::string("pdf_extract_text ") + m_helperArg;
        } else if (m_helperName == "pdf_inspect_form") {
            if (!EnsurePdfInspectFormScript(scriptPath, scriptError)) {
                result.stderrText = scriptError;
                result.exitCode = -1;
                return;
            }
            helperArgs.push_back(m_helperArg);
            // No output directory argument: pdf_inspect_form is a
            // read-only inspector, sibling to csv_inspect/xlsx_inspect.
            // It emits JSON to stdout and produces no artifact file.
            result.commandEcho = m_helperArg.empty()
                ? std::string("pdf_inspect_form")
                : std::string("pdf_inspect_form ") + m_helperArg;
        } else if (m_helperName == "pdf_fill_form") {
            if (!EnsurePdfFillFormScript(scriptPath, scriptError)) {
                result.stderrText = scriptError;
                result.exitCode = -1;
                return;
            }
            // Multi-line args contract: first line is the input PDF
            // path, remaining lines are a JSON {field: value} object.
            // Splitting here keeps the helper's argv shape clean
            // (path, json, out_dir) and preserves the established
            // first-line-is-path convention used by /write and /edit.
            std::string blob = m_helperArg;
            size_t nl = blob.find('\n');
            std::string pathPart;
            std::string jsonPart;
            if (nl == std::string::npos) {
                pathPart = blob;
                jsonPart = "";
            } else {
                pathPart = blob.substr(0, nl);
                jsonPart = blob.substr(nl + 1);
            }
            // Trim CR from CRLF endings on Windows-pasted content.
            while (!pathPart.empty() &&
                   (pathPart.back() == '\r' || pathPart.back() == ' ' ||
                    pathPart.back() == '\t'))
                pathPart.pop_back();
            // Leading whitespace on path
            size_t ps = pathPart.find_first_not_of(" \t");
            if (ps != std::string::npos && ps > 0)
                pathPart = pathPart.substr(ps);

            helperArgs.push_back(pathPart);
            helperArgs.push_back(jsonPart);
            helperArgs.push_back(FilledFormsDirForCwd(m_cwd));

            result.commandEcho = pathPart.empty()
                ? std::string("pdf_fill_form")
                : std::string("pdf_fill_form ") + pathPart;
        } else if (m_helperName == "docx_extract_text") {
            if (!EnsureDocxExtractTextScript(scriptPath, scriptError)) {
                result.stderrText = scriptError;
                result.exitCode = -1;
                return;
            }
            helperArgs.push_back(m_helperArg);
            helperArgs.push_back(WordDirForCwd(m_cwd));
            result.commandEcho = m_helperArg.empty()
                ? std::string("docx_extract_text")
                : std::string("docx_extract_text ") + m_helperArg;
        } else if (m_helperName == "docx_inspect") {
            if (!EnsureDocxInspectScript(scriptPath, scriptError)) {
                result.stderrText = scriptError;
                result.exitCode = -1;
                return;
            }
            helperArgs.push_back(m_helperArg);
            // No output directory argument: docx_inspect is read-only,
            // sibling to pdf_inspect_form / xlsx_inspect / csv_inspect.
            result.commandEcho = m_helperArg.empty()
                ? std::string("docx_inspect")
                : std::string("docx_inspect ") + m_helperArg;
        } else if (m_helperName == "python_run_script") {
            std::string displayName;
            if (!ResolveRunnableScriptPath(m_helperArg, m_cwd, m_activeProjectRoot, scriptPath, displayName, scriptError)) {
                result.stderrText = scriptError;
                result.exitCode = -1;
                return;
            }
            result.commandEcho = displayName.empty()
                ? std::string("python_run_script")
                : std::string("python_run_script ") + displayName;
            beforeFiles = SnapshotLlamaBossFiles(m_cwd);
        } else {
            result.stderrText = "Unknown built-in Python helper/script runner: " + m_helperName;
            result.exitCode = -1;
            return;
        }

        std::string cwd = m_cwd.empty() ? ServerManager::GetWorkspaceDir() : m_cwd;
        std::wstring wCwd = Utf8ToWide(cwd);
        LPCWSTR cwdArg = wCwd.empty() ? nullptr : wCwd.c_str();

        std::vector<PythonCandidate> candidates = isPackageInstall
            ? BuildPipInstallCandidates(helperArgs.empty() ? std::string() : helperArgs.front())
            : BuildCandidates(scriptPath, helperArgs, !isUserScript);
        std::ostringstream startErrors;

        for (const PythonCandidate& c : candidates) {
            PythonRunResult attempt = result;
            attempt.pythonCommand = c.label;

            std::wstring mutableCmd = c.commandLine;
            std::vector<wchar_t> cmdBuf(mutableCmd.begin(), mutableCmd.end());
            cmdBuf.push_back(L'\0');

            if (RunProcess(cmdBuf.data(), cwdArg, attempt, startErrors)) {
                if (m_helperName == "csv_report") {
                    AttachCsvReportArtifact(attempt);
                } else if (m_helperName == "xlsx_report") {
                    AttachXlsxReportArtifact(attempt);
                } else if (m_helperName == "pdf_extract_text") {
                    AttachPdfExtractTextArtifact(attempt);
                } else if (m_helperName == "pdf_fill_form") {
                    AttachPdfFillFormArtifact(attempt);
                } else if (m_helperName == "docx_extract_text") {
                    AttachDocxExtractTextArtifact(attempt);
                } else if (m_helperName == "python_run_script") {
                    AttachNewFilesUnderLlamaBoss(attempt, beforeFiles, m_cwd);
                }
                result = std::move(attempt);
                return;
            }
        }

        result.stderrText =
            "Could not start Python. Tried py -3, python, and python3.\n" +
            startErrors.str();
        result.exitCode = -1;
    }

    bool RunProcess(wchar_t* cmdLine,
                    LPCWSTR cwdArg,
                    PythonRunResult& result,
                    std::ostringstream& startErrors)
    {
        SECURITY_ATTRIBUTES sa = {};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        sa.lpSecurityDescriptor = nullptr;

        HANDLE outR_raw = nullptr, outW_raw = nullptr;
        HANDLE errR_raw = nullptr, errW_raw = nullptr;
        if (!CreatePipe(&outR_raw, &outW_raw, &sa, 0)) {
            result.stderrText = "CreatePipe(stdout) failed, error=" +
                                std::to_string(GetLastError());
            result.exitCode = -1;
            return true; // fatal infrastructure error; do not try candidates
        }
        HandleGuard outR(outR_raw), outW(outW_raw);
        if (!CreatePipe(&errR_raw, &errW_raw, &sa, 0)) {
            result.stderrText = "CreatePipe(stderr) failed, error=" +
                                std::to_string(GetLastError());
            result.exitCode = -1;
            return true;
        }
        HandleGuard errR(errR_raw), errW(errW_raw);

        SetHandleInformation(outR.h, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(errR.h, HANDLE_FLAG_INHERIT, 0);

        HandleGuard job(CreateJobObjectW(nullptr, nullptr));
        if (!job.h) {
            result.stderrText = "CreateJobObject failed, error=" +
                                std::to_string(GetLastError());
            result.exitCode = -1;
            return true;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job.h,
                                     JobObjectExtendedLimitInformation,
                                     &jeli,
                                     sizeof(jeli))) {
            result.stderrText = "SetInformationJobObject failed, error=" +
                                std::to_string(GetLastError());
            result.exitCode = -1;
            return true;
        }

        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput  = nullptr;
        si.hStdOutput = outW.h;
        si.hStdError  = errW.h;

        PROCESS_INFORMATION pi = {};
        BOOL ok = CreateProcessW(
            nullptr,
            cmdLine,
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED,
            nullptr,
            cwdArg,
            &si,
            &pi);

        if (!ok) {
            DWORD err = GetLastError();
            startErrors << result.pythonCommand << ": CreateProcess failed, error="
                        << err << "\n";
            return false; // try next launcher candidate
        }

        HandleGuard proc(pi.hProcess);
        HandleGuard thr(pi.hThread);

        if (!AssignProcessToJobObject(job.h, proc.h)) {
            DWORD err = GetLastError();
            TerminateProcess(proc.h, 1);
            result.stderrText = "AssignProcessToJobObject failed, error=" +
                                std::to_string(err);
            result.exitCode = -1;
            return true;
        }

        ResumeThread(thr.h);
        CloseHandle(outW.release());
        CloseHandle(errW.release());

        std::mutex outMu, errMu;
        std::atomic<bool> truncated(false);
        std::thread outThread(ReaderLoop,
            outR.h,
            std::ref(result.stdoutText),
            std::ref(outMu),
            std::ref(truncated));
        std::thread errThread(ReaderLoop,
            errR.h,
            std::ref(result.stderrText),
            std::ref(errMu),
            std::ref(truncated));

        constexpr DWORD kTickMs = 100;
        double deadline = NowSec() + (m_timeoutMs / 1000.0);
        bool killIt = false;

        for (;;) {
            DWORD wr = WaitForSingleObject(proc.h, kTickMs);
            if (wr == WAIT_OBJECT_0) break;
            if (wr == WAIT_FAILED) { killIt = true; break; }

            if (m_cancelFlag && m_cancelFlag->load()) {
                result.cancelled = true;
                killIt = true;
                break;
            }
            if (NowSec() >= deadline) {
                result.timedOut = true;
                killIt = true;
                break;
            }
        }

        if (killIt) {
            CloseHandle(job.release());
            WaitForSingleObject(proc.h, 2000);
        }

        if (outThread.joinable()) outThread.join();
        if (errThread.joinable()) errThread.join();

        DWORD code = 0;
        if (GetExitCodeProcess(proc.h, &code))
            result.exitCode = static_cast<int>(code);
        else
            result.exitCode = -1;

        result.truncated = truncated.load();

        if (result.cancelled && result.stderrText.empty())
            result.stderrText = "[cancelled by user]\r\n";
        else if (result.timedOut && result.stderrText.empty())
            result.stderrText = "[timed out]\r\n";

        return true;
    }

    void PostCompletion(PythonRunResult result)
    {
        auto alive = m_aliveToken.lock();
        if (!alive || !alive->load()) return;

        auto* ev = new wxCommandEvent(wxEVT_PYTHON_COMPLETE);
        ev->SetClientObject(new PythonRunResultClientData(std::move(result)));
        wxQueueEvent(m_evtHandler, ev);
    }

    wxEvtHandler*                      m_evtHandler;
    std::string                        m_helperName;
    std::string                        m_helperArg;
    std::string                        m_cwd;
    unsigned long                      m_timeoutMs;
    std::string                        m_activeProjectRoot;
    std::shared_ptr<std::atomic<bool>> m_cancelFlag;
    std::shared_ptr<std::atomic<bool>> m_runningFlag;
    std::weak_ptr<std::atomic<bool>>   m_aliveToken;
};

} // namespace

PythonRunner::PythonRunner(wxEvtHandler* eventHandler,
                           std::weak_ptr<std::atomic<bool>> aliveToken)
    : m_eventHandler(eventHandler)
    , m_aliveToken(std::move(aliveToken))
    , m_cancelFlag(std::make_shared<std::atomic<bool>>(false))
    , m_isRunning(std::make_shared<std::atomic<bool>>(false))
{}

PythonRunner::~PythonRunner()
{
    if (m_cancelFlag) m_cancelFlag->store(true);
}

bool PythonRunner::StartHealth(const std::string& cwd,
                               unsigned long      timeoutMs)
{
    if (IsRunning()) return false;

    m_cancelFlag->store(false);
    m_isRunning->store(true);

    auto* worker = new PythonWorkerThread(
        m_eventHandler,
        "python_health",
        std::string(),
        cwd,
        timeoutMs,
        std::string(),
        m_cancelFlag,
        m_isRunning,
        m_aliveToken);

    if (worker->Run() != wxTHREAD_NO_ERROR) {
        delete worker;
        m_isRunning->store(false);

        auto alive = m_aliveToken.lock();
        if (alive && alive->load()) {
            auto* ev = new wxCommandEvent(wxEVT_PYTHON_ERROR);
            ev->SetString("Failed to start Python worker thread.");
            wxQueueEvent(m_eventHandler, ev);
        }
        return false;
    }

    return true;
}

bool PythonRunner::StartCsvInspect(const std::string& pathArg,
                                   const std::string& cwd,
                                   unsigned long      timeoutMs)
{
    if (IsRunning()) return false;

    m_cancelFlag->store(false);
    m_isRunning->store(true);

    auto* worker = new PythonWorkerThread(
        m_eventHandler,
        "csv_inspect",
        pathArg,
        cwd,
        timeoutMs,
        std::string(),
        m_cancelFlag,
        m_isRunning,
        m_aliveToken);

    if (worker->Run() != wxTHREAD_NO_ERROR) {
        delete worker;
        m_isRunning->store(false);

        auto alive = m_aliveToken.lock();
        if (alive && alive->load()) {
            auto* ev = new wxCommandEvent(wxEVT_PYTHON_ERROR);
            ev->SetString("Failed to start Python worker thread.");
            wxQueueEvent(m_eventHandler, ev);
        }
        return false;
    }

    return true;
}

bool PythonRunner::StartCsvReport(const std::string& pathArg,
                                  const std::string& cwd,
                                  unsigned long      timeoutMs)
{
    if (IsRunning()) return false;

    m_cancelFlag->store(false);
    m_isRunning->store(true);

    auto* worker = new PythonWorkerThread(
        m_eventHandler,
        "csv_report",
        pathArg,
        cwd,
        timeoutMs,
        std::string(),
        m_cancelFlag,
        m_isRunning,
        m_aliveToken);

    if (worker->Run() != wxTHREAD_NO_ERROR) {
        delete worker;
        m_isRunning->store(false);

        auto alive = m_aliveToken.lock();
        if (alive && alive->load()) {
            auto* ev = new wxCommandEvent(wxEVT_PYTHON_ERROR);
            ev->SetString("Failed to start Python worker thread.");
            wxQueueEvent(m_eventHandler, ev);
        }
        return false;
    }

    return true;
}

bool PythonRunner::StartCsvToXlsx(const std::string& pathArg,
                                  const std::string& cwd,
                                  unsigned long      timeoutMs)
{
    if (IsRunning()) return false;

    m_cancelFlag->store(false);
    m_isRunning->store(true);

    auto* worker = new PythonWorkerThread(
        m_eventHandler,
        "csv_to_xlsx",
        pathArg,
        cwd,
        timeoutMs,
        std::string(),
        m_cancelFlag,
        m_isRunning,
        m_aliveToken);

    if (worker->Run() != wxTHREAD_NO_ERROR) {
        delete worker;
        m_isRunning->store(false);

        auto alive = m_aliveToken.lock();
        if (alive && alive->load()) {
            auto* ev = new wxCommandEvent(wxEVT_PYTHON_ERROR);
            ev->SetString("Failed to start Python worker thread.");
            wxQueueEvent(m_eventHandler, ev);
        }
        return false;
    }

    return true;
}


bool PythonRunner::StartXlsxInspect(const std::string& pathArg,
                                    const std::string& cwd,
                                    unsigned long      timeoutMs)
{
    if (IsRunning()) return false;

    m_cancelFlag->store(false);
    m_isRunning->store(true);

    auto* worker = new PythonWorkerThread(
        m_eventHandler,
        "xlsx_inspect",
        pathArg,
        cwd,
        timeoutMs,
        std::string(),
        m_cancelFlag,
        m_isRunning,
        m_aliveToken);

    if (worker->Run() != wxTHREAD_NO_ERROR) {
        delete worker;
        m_isRunning->store(false);

        auto alive = m_aliveToken.lock();
        if (alive && alive->load()) {
            auto* ev = new wxCommandEvent(wxEVT_PYTHON_ERROR);
            ev->SetString("Failed to start Python worker thread.");
            wxQueueEvent(m_eventHandler, ev);
        }
        return false;
    }

    return true;
}

bool PythonRunner::StartXlsxReport(const std::string& pathArg,
                                   const std::string& cwd,
                                   unsigned long      timeoutMs)
{
    if (IsRunning()) return false;

    m_cancelFlag->store(false);
    m_isRunning->store(true);

    auto* worker = new PythonWorkerThread(
        m_eventHandler,
        "xlsx_report",
        pathArg,
        cwd,
        timeoutMs,
        std::string(),
        m_cancelFlag,
        m_isRunning,
        m_aliveToken);

    if (worker->Run() != wxTHREAD_NO_ERROR) {
        delete worker;
        m_isRunning->store(false);

        auto alive = m_aliveToken.lock();
        if (alive && alive->load()) {
            auto* ev = new wxCommandEvent(wxEVT_PYTHON_ERROR);
            ev->SetString("Failed to start Python worker thread.");
            wxQueueEvent(m_eventHandler, ev);
        }
        return false;
    }

    return true;
}


bool PythonRunner::StartPdfExtractText(const std::string& pathArg,
                                       const std::string& cwd,
                                       unsigned long      timeoutMs)
{
    if (IsRunning()) return false;

    m_cancelFlag->store(false);
    m_isRunning->store(true);

    auto* worker = new PythonWorkerThread(
        m_eventHandler,
        "pdf_extract_text",
        pathArg,
        cwd,
        timeoutMs,
        std::string(),
        m_cancelFlag,
        m_isRunning,
        m_aliveToken);

    if (worker->Run() != wxTHREAD_NO_ERROR) {
        delete worker;
        m_isRunning->store(false);

        auto alive = m_aliveToken.lock();
        if (alive && alive->load()) {
            auto* ev = new wxCommandEvent(wxEVT_PYTHON_ERROR);
            ev->SetString("Failed to start Python worker thread.");
            wxQueueEvent(m_eventHandler, ev);
        }
        return false;
    }

    return true;
}

bool PythonRunner::StartPdfInspectForm(const std::string& pathArg,
                                       const std::string& cwd,
                                       unsigned long      timeoutMs)
{
    if (IsRunning()) return false;

    m_cancelFlag->store(false);
    m_isRunning->store(true);

    auto* worker = new PythonWorkerThread(
        m_eventHandler,
        "pdf_inspect_form",
        pathArg,
        cwd,
        timeoutMs,
        std::string(),
        m_cancelFlag,
        m_isRunning,
        m_aliveToken);

    if (worker->Run() != wxTHREAD_NO_ERROR) {
        delete worker;
        m_isRunning->store(false);

        auto alive = m_aliveToken.lock();
        if (alive && alive->load()) {
            auto* ev = new wxCommandEvent(wxEVT_PYTHON_ERROR);
            ev->SetString("Failed to start Python worker thread.");
            wxQueueEvent(m_eventHandler, ev);
        }
        return false;
    }

    return true;
}

bool PythonRunner::StartPdfFillForm(const std::string& argsBlob,
                                    const std::string& cwd,
                                    unsigned long      timeoutMs)
{
    if (IsRunning()) return false;

    m_cancelFlag->store(false);
    m_isRunning->store(true);

    auto* worker = new PythonWorkerThread(
        m_eventHandler,
        "pdf_fill_form",
        argsBlob,
        cwd,
        timeoutMs,
        std::string(),
        m_cancelFlag,
        m_isRunning,
        m_aliveToken);

    if (worker->Run() != wxTHREAD_NO_ERROR) {
        delete worker;
        m_isRunning->store(false);

        auto alive = m_aliveToken.lock();
        if (alive && alive->load()) {
            auto* ev = new wxCommandEvent(wxEVT_PYTHON_ERROR);
            ev->SetString("Failed to start Python worker thread.");
            wxQueueEvent(m_eventHandler, ev);
        }
        return false;
    }

    return true;
}

bool PythonRunner::StartDocxExtractText(const std::string& pathArg,
                                        const std::string& cwd,
                                        unsigned long      timeoutMs)
{
    if (IsRunning()) return false;

    m_cancelFlag->store(false);
    m_isRunning->store(true);

    auto* worker = new PythonWorkerThread(
        m_eventHandler,
        "docx_extract_text",
        pathArg,
        cwd,
        timeoutMs,
        std::string(),
        m_cancelFlag,
        m_isRunning,
        m_aliveToken);

    if (worker->Run() != wxTHREAD_NO_ERROR) {
        delete worker;
        m_isRunning->store(false);

        auto alive = m_aliveToken.lock();
        if (alive && alive->load()) {
            auto* ev = new wxCommandEvent(wxEVT_PYTHON_ERROR);
            ev->SetString("Failed to start Python worker thread.");
            wxQueueEvent(m_eventHandler, ev);
        }
        return false;
    }

    return true;
}

bool PythonRunner::StartDocxInspect(const std::string& pathArg,
                                    const std::string& cwd,
                                    unsigned long      timeoutMs)
{
    if (IsRunning()) return false;

    m_cancelFlag->store(false);
    m_isRunning->store(true);

    auto* worker = new PythonWorkerThread(
        m_eventHandler,
        "docx_inspect",
        pathArg,
        cwd,
        timeoutMs,
        std::string(),
        m_cancelFlag,
        m_isRunning,
        m_aliveToken);

    if (worker->Run() != wxTHREAD_NO_ERROR) {
        delete worker;
        m_isRunning->store(false);

        auto alive = m_aliveToken.lock();
        if (alive && alive->load()) {
            auto* ev = new wxCommandEvent(wxEVT_PYTHON_ERROR);
            ev->SetString("Failed to start Python worker thread.");
            wxQueueEvent(m_eventHandler, ev);
        }
        return false;
    }

    return true;
}

bool PythonRunner::StartPythonRunScript(const std::string& scriptArg,
                                        const std::string& cwd,
                                        unsigned long      timeoutMs,
                                        const std::string& activeProjectRoot)
{
    if (IsRunning()) return false;

    m_cancelFlag->store(false);
    m_isRunning->store(true);

    auto* worker = new PythonWorkerThread(
        m_eventHandler,
        "python_run_script",
        scriptArg,
        cwd,
        timeoutMs ? timeoutMs : 30000,
        activeProjectRoot,
        m_cancelFlag,
        m_isRunning,
        m_aliveToken);

    if (worker->Run() != wxTHREAD_NO_ERROR) {
        delete worker;
        m_isRunning->store(false);

        auto alive = m_aliveToken.lock();
        if (alive && alive->load()) {
            auto* ev = new wxCommandEvent(wxEVT_PYTHON_ERROR);
            ev->SetString("Failed to start Python worker thread.");
            wxQueueEvent(m_eventHandler, ev);
        }
        return false;
    }

    return true;
}

bool PythonRunner::StartPythonInstallPackage(const std::string& packageArg,
                                             const std::string& cwd,
                                             unsigned long      timeoutMs)
{
    if (IsRunning()) return false;

    std::string packageName;
    std::string err;
    if (!NormalizeAllowedPythonPackage(packageArg, packageName, err)) {
        auto alive = m_aliveToken.lock();
        if (alive && alive->load()) {
            PythonRunResult r;
            r.toolName    = "python_install_package";
            r.helperName  = "python_install_package";
            r.commandEcho = "python_install_package";
            r.stderrText  = err;
            r.exitCode    = -1;
            auto* ev = new wxCommandEvent(wxEVT_PYTHON_COMPLETE);
            ev->SetClientObject(new PythonRunResultClientData(std::move(r)));
            wxQueueEvent(m_eventHandler, ev);
        }
        return true;
    }

    m_cancelFlag->store(false);
    m_isRunning->store(true);

    auto* worker = new PythonWorkerThread(
        m_eventHandler,
        "python_install_package",
        packageName,
        cwd,
        timeoutMs ? timeoutMs : 300000,
        std::string(),
        m_cancelFlag,
        m_isRunning,
        m_aliveToken);

    if (worker->Run() != wxTHREAD_NO_ERROR) {
        delete worker;
        m_isRunning->store(false);

        auto alive = m_aliveToken.lock();
        if (alive && alive->load()) {
            auto* ev = new wxCommandEvent(wxEVT_PYTHON_ERROR);
            ev->SetString("Failed to start Python worker thread.");
            wxQueueEvent(m_eventHandler, ev);
        }
        return false;
    }

    return true;
}

void PythonRunner::Cancel()
{
    if (m_cancelFlag) m_cancelFlag->store(true);
}
