import csv
import io
import json
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

def cell_looks_like_data(cell):
    c = cell.strip()
    if not c:
        return False
    parts = c.split('/')
    if len(parts) == 3 and all(p.strip().isdigit() for p in parts):
        return True
    c2 = c.replace('$', '').replace(',', '').replace('%', '').strip()
    if c2.count('.') <= 1 and c2.replace('.', '').isdigit():
        return True
    return False

def header_score(row):
    nonempty = [c for c in row if c.strip()]
    if not nonempty:
        return -1000
    data_cells = sum(1 for c in row if cell_looks_like_data(c))
    uniq = len(set(' '.join(c.split()).lower() for c in nonempty))
    score = len(nonempty) - 3 * data_cells
    if uniq == len(nonempty):
        score += 1
    return score

def guess_col_type(values):
    vals = [v.strip() for v in values if v.strip()]
    if not vals:
        return 'empty'
    kinds = set()
    for v in vals:
        if cell_looks_like_data(v):
            kinds.add('date' if v.count('/') == 2 else 'number')
        else:
            kinds.add('text')
    if len(kinds) == 1:
        return kinds.pop()
    return 'mixed'

def main():
    if len(sys.argv) != 2:
        emit({'ok': False, 'helper': 'csv_inspect', 'error': 'csv_inspect expects exactly one file path argument'}, 2)

    cwd = Path.cwd().resolve()
    raw_arg = sys.argv[1]
    target = Path(raw_arg)
    if not target.is_absolute():
        target = cwd / target
    target = target.resolve()

    if target.suffix.lower() not in ('.csv', '.tsv'):
        emit({'ok': False, 'helper': 'csv_inspect', 'error': 'Only .csv and .tsv files are supported in this first data helper', 'path': str(target)}, 4)

    if not target.exists() or not target.is_file():
        emit({'ok': False, 'helper': 'csv_inspect', 'error': 'File not found', 'path': str(target)}, 5)

    max_file_bytes = 10 * 1024 * 1024
    if target.stat().st_size > max_file_bytes:
        emit({'ok': False, 'helper': 'csv_inspect', 'error': 'File is larger than the first-phase 10 MB safety cap', 'path': str(target), 'file_size_bytes': target.stat().st_size}, 7)

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
    row_count = 0
    max_rows_to_scan = 100000
    try:
        reader = csv.reader(io.StringIO(text), delimiter=delimiter)
        for row in reader:
            row_count += 1
            if len(rows) < 12:
                rows.append(row)
            if row_count >= max_rows_to_scan:
                warnings.append('Stopped counting at 100000 rows.')
                break
    except Exception as ex:
        emit({'ok': False, 'helper': 'csv_inspect', 'error': 'CSV parse failed: ' + str(ex), 'path': str(target)}, 6)

    # Header detection: Excel-exported reports often carry metadata rows
    # (titles, totals, font notes) ABOVE the real header.  Blindly calling
    # rows[0] the header poisons every downstream consumer.  Score the
    # first rows and pick the most header-like one: many non-empty,
    # unique, non-data-looking cells.
    scan = rows[:10]
    header_idx = 0
    if scan:
        best = max(range(len(scan)), key=lambda i: header_score(scan[i]))
        if header_score(scan[best]) > header_score(scan[0]):
            header_idx = best

    header = rows[header_idx] if rows else []
    preamble_rows = rows[:header_idx]
    sample_rows = rows[header_idx + 1: header_idx + 6]
    data_row_count = max(0, row_count - header_idx - 1)
    width = max((len(r) for r in rows), default=0)
    ragged = any(len(r) != width for r in rows) if rows else False
    if ragged:
        warnings.append('Sample rows have inconsistent column counts.')

    multiline_headers = any('\n' in c or '\r' in c for c in header)
    parse_hints = []
    if header_idx > 0:
        warnings.append('File has ' + str(header_idx) + ' metadata row(s) ABOVE the real header. Row index ' + str(header_idx) + ' (0-based) is the header. Do not treat row 0 as the header.')
        parse_hints.append("pandas: pd.read_csv(path, skiprows=" + str(header_idx) + ")")
    if multiline_headers:
        warnings.append('Some header names contain embedded newlines (shown as \\n in the columns list). Match on normalized names.')
        parse_hints.append("normalize headers: df.columns = [' '.join(str(c).split()) for c in df.columns]")

    columns_sample_types = [
        guess_col_type([r[ci] for r in sample_rows if ci < len(r)])
        for ci in range(len(header))
    ]

    emit({
        'ok': True,
        'helper': 'csv_inspect',
        'path': str(target),
        'cwd': str(cwd),
        'file_size_bytes': size_bytes,
        'encoding': encoding,
        'delimiter': delimiter,
        'dialect': dialect_name,
        'rows_scanned': row_count,
        'row_count_exact': row_count < max_rows_to_scan,
        'column_count_sample': width,
        'header_row_index': header_idx,
        'preamble_rows': preamble_rows,
        'columns': header,
        'column_types_from_sample': columns_sample_types,
        'data_row_count': data_row_count,
        'sample_rows': sample_rows,
        'sample_note': 'sample_rows shows only the first ' + str(len(sample_rows)) + ' of ' + str(data_row_count) + ' data rows. Do NOT analyze, count, filter, or summarize the dataset from this sample; any dataset-level conclusion (totals, who matches a condition, expirations, etc.) requires processing the FULL file, e.g. with a Python script.',
        'parse_hints': parse_hints,
        'warnings': warnings,
    })

if __name__ == '__main__':
    main()
