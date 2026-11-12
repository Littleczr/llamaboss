"""Regression tests for LlamaBoss's Excel/PDF Python helpers.

Covers the 2026-11-10 prelaunch review findings (F1-F9) plus the
negative-number / visible-apostrophe bug, with controls that must keep
working.  Runs the real helper scripts as subprocesses, the same way
PythonRunner does (python -I <helper> <args>), then reopens the outputs.

Usage:
    py test_xlsx_pdf_helpers.py <path-to-assets\\python> [--keep]

Needs openpyxl, pypdf and reportlab.  Exit code 0 = all passed.
"""
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import openpyxl
from pypdf import PdfReader, PdfWriter
from pypdf.generic import DictionaryObject, NameObject

HELPERS = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[2] / 'assets' / 'python'
KEEP = '--keep' in sys.argv
WORK = Path(tempfile.mkdtemp(prefix='lb_helper_tests_'))
results = []


def check(name, cond, detail=''):
    results.append((name, bool(cond), detail))


def run(helper, *args):
    p = subprocess.run([sys.executable, '-I', str(HELPERS / helper), *map(str, args)],
                       capture_output=True, text=True, cwd=WORK, encoding='utf-8')
    try:
        return json.loads(p.stdout)
    except Exception:
        return {'ok': False, 'raw_stdout': p.stdout[-600:], 'raw_stderr': p.stderr[-600:]}


_n = [0]


def outdir(tag):
    _n[0] += 1
    d = WORK / f'{_n[0]:02d}_{tag}'
    d.mkdir()
    return d


def create(spec, tag):
    d = outdir(tag)
    r = run('xlsx_create_workbook.py', json.dumps(spec), d)
    files = list(d.glob('*.xlsx'))
    return r, (files[0] if files else None)


def col(path, sheet, column, first_row):
    ws = openpyxl.load_workbook(path)[sheet]
    out = []
    r = first_row
    while ws.cell(row=r, column=column).value is not None:
        c = ws.cell(row=r, column=column)
        out.append((c.value, c.data_type))
        r += 1
    return out


def safe(name, fn):
    try:
        fn()
    except Exception as ex:
        check(name + ' (crashed)', False, repr(ex))


# ---------------------------------------------------------------- F1
def t_f1():
    hdr = ['Name', 'Scheduled Hours', 'Actual Hours', 'Overtime Hours', 'Overtime Cost', 'Status']
    sites = [{'name': 'Site A', 'headers': hdr, 'rows': [['a', 40, 45, 5, 100, 'Complete']]},
             {'name': 'Site B', 'headers': hdr, 'rows': [['b', 40, 50, 10, 200, 'Complete']]}]
    for order, tag in ((sites, 'f1_ab'), (list(reversed(sites)), 'f1_ba')):
        spec = {'filename': 'f1', 'allow_formulas': True, 'sheets': order + [
            {'name': 'Summary', 'headers': ['Metric', 'Value'], 'rows': [
                ['Total Actual Hours', 95], ['Total Overtime Cost', 300],
                ['Total Scheduled Hours', {'formula': "='Site A'!B2+'Site B'!B2"}]]}]}
        r, p = create(spec, tag)
        v = col(p, 'Summary', 2, 2)
        check(f'F1 multi-sheet totals preserved ({tag})', [x[0] for x in v[:2]] == [95, 300], v)
        check(f'F1 explicit summary formula stays a formula ({tag})', v[2][1] == 'f', v[2])
    # single data sheet with a wrong total: flagged, not rewritten
    spec = {'filename': 'f1w', 'sheets': [
        {'name': 'Data', 'headers': hdr, 'rows': [['a', 40, 45, 5, 100, 'Complete'], ['b', 40, 50, 10, 200, 'Complete']]},
        {'name': 'Summary', 'headers': ['Metric', 'Value'], 'rows': [['Total Actual Hours', 90]]}]}
    r, p = create(spec, 'f1_wrong')
    check('F1 wrong summary kept as written', col(p, 'Summary', 2, 2)[0][0] == 90)
    check('F1 wrong summary reported as discrepancy',
          any('Total Actual Hours' in w for w in r.get('warnings', [])), r.get('warnings'))


# ---------------------------------------------------------------- F2
def t_f2():
    spec = {'filename': 'f2', 'sheets': [{'name': 'D', 'headers': ['Region', 'Growth %'],
            'rows': [['a', 0.5], ['b', 1.5], ['c', -1.5], ['d', 0.25]]}]}
    r, p = create(spec, 'f2_decimals')
    check('F2 decimals incl. >100% unchanged', [x[0] for x in col(p, 'D', 2, 2)] == [0.5, 1.5, -1.5, 0.25],
          col(p, 'D', 2, 2))
    spec['sheets'][0]['rows'] = [['a', '50%'], ['b', '150%'], ['c', '-150%']]
    r, p = create(spec, 'f2_strings')
    check('F2 explicit % strings convert exactly once', [x[0] for x in col(p, 'D', 2, 2)] == [0.5, 1.5, -1.5],
          col(p, 'D', 2, 2))
    spec['sheets'][0]['rows'] = [['a', 92], ['b', 85], ['c', 0]]
    r, p = create(spec, 'f2_whole_auto')
    check('F2 control: all-whole column still auto-divided', [x[0] for x in col(p, 'D', 2, 2)] == [0.92, 0.85, 0],
          col(p, 'D', 2, 2))
    check('F2 auto-divide is reported', any('divided by 100' in w for w in r.get('warnings', [])), r.get('warnings'))
    spec['sheets'][0]['rows'] = [['a', 1.5], ['b', 2]]
    spec['percent_scale'] = 'decimal'
    r, p = create(spec, 'f2_scale_decimal')
    check('F2 percent_scale decimal respected', [x[0] for x in col(p, 'D', 2, 2)] == [1.5, 2], col(p, 'D', 2, 2))
    spec['percent_scale'] = 'whole'
    spec['sheets'][0]['rows'] = [['a', 0.5], ['b', 150]]
    r, p = create(spec, 'f2_scale_whole')
    check('F2 percent_scale whole respected', [x[0] for x in col(p, 'D', 2, 2)] == [0.005, 1.5], col(p, 'D', 2, 2))
    del spec['percent_scale']
    spec['sheets'][0]['rows'] = [['a', 0.5], ['b', 150]]
    r, p = create(spec, 'f2_mixed')
    check('F2 mixed column left unchanged', [x[0] for x in col(p, 'D', 2, 2)] == [0.5, 150], col(p, 'D', 2, 2))
    check('F2 mixed column flagged', any('mixes' in w for w in r.get('warnings', [])), r.get('warnings'))


# ---------------------------------------------------------------- F3
def t_f3():
    for allow in (False, True):
        spec = {'filename': 'f3', 'allow_formulas': allow, 'sheets': [
            {'name': 'D', 'title': '=1+1', 'headers': ['=2+2', 'B'], 'rows': [['=3+3', 'x']]}]}
        r, p = create(spec, f'f3_allow_{allow}')
        ws = openpyxl.load_workbook(p)['D']
        check(f'F3 title stays text (allow={allow})', ws['A1'].data_type == 's' and ws['A1'].value == '=1+1',
              (ws['A1'].value, ws['A1'].data_type))
        check(f'F3 header stays text (allow={allow})', ws['A3'].data_type == 's' and ws['A3'].value == '=2+2',
              (ws['A3'].value, ws['A3'].data_type))
        if allow:
            check('F3 control: authorized body formula works', ws['A4'].data_type == 'f', (ws['A4'].value, ws['A4'].data_type))
        else:
            check('F3 protected body cell is text with no visible apostrophe',
                  ws['A4'].data_type == 's' and ws['A4'].value == '=3+3', (ws['A4'].value, ws['A4'].data_type))
    csvp = WORK / 'f3.csv'
    csvp.write_text('a,b\n=1+1,x\n', encoding='utf-8')
    d = outdir('f3_csv')
    run('csv_to_xlsx.py', csvp, d)
    ws = openpyxl.load_workbook(next(d.glob('*.xlsx'))).active
    check('F3 CSV "=1+1" stays literal text', ws['A2'].data_type == 's' and ws['A2'].value == '=1+1',
          (ws['A2'].value, ws['A2'].data_type))


# ---------------------------------------------------------------- F4
def t_f4():
    spec = {'filename': 'f4', 'allow_formulas': True, 'sheets': [
        {'name': 'D', 'headers': ['Amount', 'Plain'], 'rows': [[10, 1], [{'formula': '=A2*2'}, 2]]}]}
    r, p = create(spec, 'f4')
    ins = run('xlsx_inspect.py', p)
    sheet = (ins.get('sheets') or [{}])[0]
    check('F4 inspect counts uncalculated formula', sheet.get('uncalculated_formula_cells') == 1, sheet)
    check('F4 inspect sample shows formula, not blank',
          any('uncalculated formula' in str(c) for row in sheet.get('sample_rows', []) for c in row), sheet.get('sample_rows'))
    check('F4 inspect warns', any('UNKNOWN' in w for w in ins.get('warnings', [])), ins.get('warnings'))
    d = outdir('f4_report')
    rep = run('xlsx_report.py', p, d)
    nums = {n['column']: n for n in (rep.get('sheets') or [{}])[0].get('numeric_columns', [])}
    amt = nums.get('Amount', {})
    check('F4 report does not present 10 as a complete sum', amt.get('complete') is False, amt)
    check('F4 report warns about incomplete totals', any('INCOMPLETE' in w for w in rep.get('warnings', [])),
          rep.get('warnings'))
    md = next(d.glob('*.md')).read_text(encoding='utf-8') if list(d.glob('*.md')) else ''
    check('F4 markdown marks sum INCOMPLETE', 'INCOMPLETE' in md)
    check('F4 control: plain column still complete with sum 3',
          nums.get('Plain', {}).get('complete') is True and nums.get('Plain', {}).get('sum') == 3, nums.get('Plain'))


# ---------------------------------------------------------------- F5 / F9
def make_form(path, yes_state='/Yes'):
    from reportlab.pdfgen import canvas
    c = canvas.Canvas(str(path))
    c.drawString(50, 750, 'Employee name:')
    c.acroForm.textfield(name='Employee', x=150, y=740, width=200, height=20)
    c.drawString(50, 700, 'Agree:')
    c.acroForm.checkbox(name='Agree', x=150, y=695, size=15)
    c.save()
    if yes_state == '/Yes':
        return path
    w = PdfWriter(clone_from=str(path))
    for annot in w.pages[0]['/Annots']:
        a = annot.get_object()
        if a.get('/T') == 'Agree':
            for k in ('/N', '/D'):
                ap = a['/AP'].get(k)
                if ap is None:
                    continue
                on = [x for x in ap if x != '/Off'][0]
                nd = DictionaryObject()
                nd[NameObject(yes_state)] = ap[on]
                nd[NameObject('/Off')] = ap['/Off']
                a['/AP'][NameObject(k)] = nd
    out = path.with_name(path.stem + '_custom.pdf')
    w.write(out)
    return out


def t_f5():
    form = make_form(WORK / 'form.pdf')
    d = outdir('f5_fill')
    fr = run('pdf_fill_form.py', form, json.dumps({'Employee': 'Test Person', 'Agree': True}), d)
    filled = next(d.glob('*.pdf'))
    check('F5 control: fill succeeds and reads back', fr.get('ok') and
          PdfReader(filled).get_fields()['Employee'].get('/V') == 'Test Person', fr)
    d2 = outdir('f5_extract')
    er = run('pdf_extract_text.py', filled, d2)
    md = next(d2.glob('*.md')).read_text(encoding='utf-8') if list(d2.glob('*.md')) else ''
    check('F5 extracted markdown includes employee value', 'Test Person' in md)
    check('F5 extracted markdown includes checkbox state', 'checked' in md)
    check('F5 control: static page text still extracted', 'Employee name' in md)
    fields = {f['name']: f for f in er.get('form_fields', [])}
    check('F5 JSON form_fields carry values', fields.get('Employee', {}).get('value') == 'Test Person'
          and fields.get('Agree', {}).get('filled') is True, er.get('form_fields'))
    # plain PDF without a form: no form section
    from reportlab.pdfgen import canvas
    plain = WORK / 'plain.pdf'
    c = canvas.Canvas(str(plain)); c.drawString(50, 750, 'Just text'); c.save()
    d3 = outdir('f5_plain')
    pr = run('pdf_extract_text.py', plain, d3)
    check('F5 control: plain PDF extracts, no form fields', pr.get('ok') and not pr.get('form_fields'), pr)


def t_f9():
    form = make_form(WORK / 'form9.pdf', yes_state='/No')
    d = outdir('f9_literal')
    r = run('pdf_fill_form.py', form, json.dumps({'Agree': 'No'}), d)
    f = next(d.glob('*.pdf'), None)
    v = PdfReader(f).get_fields()['Agree'].get('/V') if f else None
    check('F9 literal "No" on-state selects /No', v == '/No', (r.get('ok'), v))
    d = outdir('f9_false')
    run('pdf_fill_form.py', form, json.dumps({'Agree': False}), d)
    f = next(d.glob('*.pdf'), None)
    v = PdfReader(f).get_fields()['Agree'].get('/V') if f else None
    check('F9 boolean false still unchecks', v in ('/Off', None), v)
    std = make_form(WORK / 'std.pdf')
    d = outdir('f9_std')
    run('pdf_fill_form.py', std, json.dumps({'Agree': 'Yes'}), d)
    f = next(d.glob('*.pdf'), None)
    v = PdfReader(f).get_fields()['Agree'].get('/V') if f else None
    check('F9 control: standard /Yes checkbox', v == '/Yes', v)


# ---------------------------------------------------------------- F6 / F7 / negatives
def t_values():
    spec = {'filename': 'f6', 'sheets': [{'name': 'D',
            'headers': ['Quantity', 'ID', 'Delta', 'Amount', 'Code'],
            'rows': [[1.75, '1234567890123456789', '-5', '-$1,200.50', '00123'],
                     [2.25, 'A-100', '-12.50', '(300)', '007'],
                     [3, '123456', '+7', '250', 'X']]}]}
    r, p = create(spec, 'values')
    ws = openpyxl.load_workbook(p)['D']
    q = [ws.cell(row=i, column=1).value for i in (2, 3, 4)]
    check('F6 fractional quantities preserved', q == [1.75, 2.25, 3], q)
    check('F6 fraction preservation reported', any('fractional' in w for w in r.get('warnings', [])), r.get('warnings'))
    check('F7 long ID kept exact as text', ws['B2'].value == '1234567890123456789' and ws['B2'].data_type == 's',
          (ws['B2'].value, ws['B2'].data_type))
    check('F7 control: short numeric ID still a number', ws['B4'].value == 123456, ws['B4'].value)
    check('NEG "-5" and "-12.50" are numbers', ws['C2'].value == -5 and ws['C3'].value == -12.5,
          (ws['C2'].value, ws['C3'].value))
    check('NEG currency "-$1,200.50" and "(300)" are numbers', ws['D2'].value == -1200.5 and ws['D3'].value == -300,
          (ws['D2'].value, ws['D3'].value))
    check('NEG no visible apostrophes anywhere', not any(isinstance(c.value, str) and c.value.startswith("'")
                                                         for row in ws.iter_rows() for c in row))
    check('Control: leading-zero codes stay text', ws['E2'].value == '00123' and ws['E3'].value == '007',
          (ws['E2'].value, ws['E3'].value))

    csvp = WORK / 'f7.csv'
    csvp.write_bytes(b'id,qty,note\r\n1234567890123456789,-5,"Line one\r\nLine two"\r\n'
                     b'00042,1.5,"has, comma and ""quotes"""\r\n42,3,"lf\nonly"\r\n')
    d = outdir('csv')
    cr = run('csv_to_xlsx.py', csvp, d)
    ws = openpyxl.load_workbook(next(d.glob('*.xlsx'))).active
    check('F7 CSV long ID kept exact', ws['A2'].value == '1234567890123456789', ws['A2'].value)
    check('F7 CSV control: leading zeros and small ints', ws['A3'].value == '00042' and ws['A4'].value == 42
          and ws['B2'].value == -5, (ws['A3'].value, ws['A4'].value, ws['B2'].value))
    check('F8 CRLF multiline cell preserved', ws['C2'].value == 'Line one\nLine two', repr(ws['C2'].value))
    check('F8 LF multiline cell preserved', ws['C4'].value == 'lf\nonly', repr(ws['C4'].value))
    check('F8 comma + escaped quotes preserved', ws['C3'].value == 'has, comma and "quotes"', repr(ws['C3'].value))
    check('F8 row count correct', cr.get('rows_written') == 4, cr.get('rows_written'))
    d = outdir('csv_report')
    rr = run('csv_report.py', csvp, d)
    md = next(d.glob('*.md')).read_text(encoding='utf-8') if list(d.glob('*.md')) else ''
    check('F8 csv_report keeps the line break (words not glued together)',
          rr.get('ok') and rr.get('data_row_count') == 3 and 'Line one' in md and 'Line oneLine two' not in md,
          rr.get('data_row_count'))


def t_controls():
    spec = {'filename': 'ctl', 'sheets': [{'name': 'D', 'headers': ['Item', 'Cost', 'Hours', 'Review Date'],
            'rows': [['a', 10.5, 8, '2026-11-10'], ['b', '20', '7.5', '11/09/2026']]}]}
    r, p = create(spec, 'controls')
    ws = openpyxl.load_workbook(p)['D']
    check('Control: ordinary values', r.get('ok') and ws['B2'].value == 10.5 and ws['B3'].value == 20
          and ws['C3'].value == 7.5, r)
    check('Control: dates parsed', ws['D2'].is_date and ws['D3'].is_date)
    d = outdir('controls_report')
    rep = run('xlsx_report.py', p, d)
    nums = {n['column']: n for n in (rep.get('sheets') or [{}])[0].get('numeric_columns', [])}
    check('Control: report sum on plain values', nums.get('Cost', {}).get('sum') == 30.5
          and nums.get('Cost', {}).get('complete', True) is True, nums.get('Cost'))
    ins = run('xlsx_inspect.py', p)
    check('Control: inspect has no false warnings', ins.get('ok') and not ins.get('warnings'), ins.get('warnings'))


for name, fn in [('F1', t_f1), ('F2', t_f2), ('F3', t_f3), ('F4', t_f4), ('F5', t_f5),
                 ('F9', t_f9), ('values', t_values), ('controls', t_controls)]:
    safe(name, fn)

passed = sum(1 for _, ok, _ in results if ok)
for name, ok, detail in results:
    print(('PASS ' if ok else 'FAIL ') + name + ('' if ok else f'   -> {detail}'))
print(f'\n{passed}/{len(results)} passed')
if not KEEP:
    shutil.rmtree(WORK, ignore_errors=True)
else:
    print('fixtures kept in', WORK)
sys.exit(0 if passed == len(results) else 1)
