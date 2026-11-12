
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


UNCALCULATED = '<uncalculated formula: {}>'


def is_formula(v):
    if v is None:
        return False
    if isinstance(v, str):
        return v.startswith('=')
    # ArrayFormula / DataTableFormula objects from openpyxl
    return type(v).__name__.endswith('Formula')


def formula_text(v):
    if isinstance(v, str):
        return v
    return str(getattr(v, 'text', None) or v)


def open_formula_view(load_workbook, target, warnings):
    """Second read-only view with data_only=False so formula cells can be told
    apart from genuinely empty cells.  data_only=True alone reports a formula
    with no cached result (a file never recalculated by Excel) as blank."""
    try:
        return load_workbook(filename=str(target), read_only=True, data_only=False)
    except Exception as ex:
        warnings.append('Could not read formula metadata; formulas without cached values '
                        'may appear blank: ' + str(ex))
        return None


def workbook_has_formulas(target):
    """Cheap pre-scan of the worksheet XML for formula elements (<f> or a
    namespaced <x:f>), so formula-free files skip the second read pass.
    Any doubt -> True (do the full check)."""
    import re as _re
    import zipfile as _zf
    pat = _re.compile(rb'<(?:[A-Za-z_][\w.-]*:)?f[\s>/]')
    try:
        with _zf.ZipFile(str(target)) as z:
            for name in z.namelist():
                if not (name.startswith('xl/worksheets/') and name.endswith('.xml')):
                    continue
                tail = b''
                with z.open(name) as fh:
                    while True:
                        chunk = fh.read(1 << 20)
                        if not chunk:
                            break
                        if pat.search(tail + chunk):
                            return True
                        tail = chunk[-32:]
        return False
    except Exception:
        return True


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
    wbf = open_formula_view(load_workbook, target, warnings) if workbook_has_formulas(target) else None

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
        f_iter = None
        if wbf is not None:
            try:
                f_iter = wbf[sheet_name].iter_rows(values_only=True)
            except Exception:
                f_iter = None
        rows = []
        row_count = 0
        formula_cells = 0
        uncalculated_cells = 0
        try:
            for row in rows_iter:
                row_count += 1
                frow = next(f_iter, None) if f_iter is not None else None
                row = list(row)
                if frow is not None:
                    for i, fv in enumerate(frow):
                        if is_formula(fv):
                            formula_cells += 1
                            if i < len(row) and row[i] is None:
                                uncalculated_cells += 1
                                row[i] = UNCALCULATED.format(formula_text(fv))
                            elif i >= len(row):
                                uncalculated_cells += 1
                if len(rows) < (max_sample_data_rows + 1):
                    rows.append(row)
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

        if uncalculated_cells:
            warnings.append(
                f'Sheet "{sheet_name}": {uncalculated_cells} formula cell(s) have no cached value '
                '(the file was not recalculated/saved by Excel). Their values are UNKNOWN, not blank; '
                'do not total or report them as empty.')

        sheets_out.append({
            'name': sheet_name,
            'rows_scanned': row_count,
            'row_count_exact': row_count < max_rows_to_scan_per_sheet,
            'column_count_sample': column_count,
            'columns': [cell_to_jsonable(h) for h in header],
            'sample_rows': sample_rows,
            'formula_cells': formula_cells,
            'uncalculated_formula_cells': uncalculated_cells,
        })

    for book in (wb, wbf):
        try:
            if book is not None:
                book.close()
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
