
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
