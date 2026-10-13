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


def load_pypdf():
    # pypdf is pure Python and permissively licensed (BSD), so it can ship
    # with LlamaBoss; the Mac app bundles it.  Re-add the user site before
    # giving up (PythonRunner launches helpers with -I, which suppresses
    # per-user site packages).
    errors = []

    def try_import():
        try:
            import pypdf
            return pypdf
        except Exception as ex:
            errors.append('pypdf: ' + str(ex))
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

    return try_import(), errors


# ── AcroForm walking (shared shape with pdf_fill_form.py) ───────────
# Field attributes /FT, /Ff, /V and /DA are inheritable: a widget may
# carry them itself (merged field+widget) or inherit them from any
# ancestor in the /Parent chain.  Names are fully qualified
# ("parent.child"), matching what Acrobat and Preview show.

FF_REQUIRED = 1 << 1
FF_RADIO = 1 << 15
FF_PUSHBUTTON = 1 << 16
FF_COMBO = 1 << 17


def inherited(node, key):
    seen = 0
    while node is not None and seen < 64:
        if key in node:
            return node[key]
        parent = node.get('/Parent')
        node = parent.get_object() if parent is not None else None
        seen += 1
    return None


def field_dict(widget):
    # The terminal field is the widget itself when it has a /T, else its parent.
    if '/T' in widget:
        return widget
    parent = widget.get('/Parent')
    return parent.get_object() if parent is not None else widget


def qualified_name(field):
    parts = []
    node, seen = field, 0
    while node is not None and seen < 64:
        t = node.get('/T')
        if t is not None:
            parts.append(str(t))
        parent = node.get('/Parent')
        node = parent.get_object() if parent is not None else None
        seen += 1
    return '.'.join(reversed(parts))


def name_str(v):
    # PDF names come back as '/Yes'; text strings as-is.
    s = '' if v is None else str(v)
    return s[1:] if s.startswith('/') else s


def field_type(widget):
    ft = inherited(widget, '/FT')
    try:
        ff = int(inherited(widget, '/Ff') or 0)
    except Exception:
        ff = 0
    if ft == '/Tx':
        return 'text'
    if ft == '/Btn':
        if ff & FF_PUSHBUTTON:
            return 'button'
        return 'radio' if ff & FF_RADIO else 'checkbox'
    if ft == '/Ch':
        return 'dropdown' if ff & FF_COMBO else 'listbox'
    if ft == '/Sig':
        return 'signature'
    return 'unknown'


def widget_on_states(widget):
    try:
        normal = widget['/AP'].get_object()['/N'].get_object()
        return [name_str(k) for k in normal.keys() if name_str(k).lower() != 'off']
    except Exception:
        return []


def field_options(field):
    out = []
    try:
        for o in (inherited(field, '/Opt') or []):
            o = o.get_object() if hasattr(o, 'get_object') else o
            # [export_value, display_text] pairs: the export value is what /V holds.
            if isinstance(o, list) and o:
                o = o[0]
            out.append(str(o))
    except Exception:
        pass
    return out


def iter_widgets(pages):
    for page_index, page in enumerate(pages, start=1):
        try:
            annots = page.get('/Annots')
            annots = annots.get_object() if annots is not None else []
        except Exception:
            annots = []
        for a in annots or []:
            try:
                w = a.get_object()
            except Exception:
                continue
            if w.get('/Subtype') == '/Widget':
                yield page_index, w


def form_kind(reader):
    try:
        acro = reader.trailer['/Root'].get('/AcroForm')
        acro = acro.get_object() if acro is not None else None
    except Exception:
        acro = None
    if acro is None:
        return 'None'
    try:
        has_fields = len(acro.get('/Fields') or []) > 0
    except Exception:
        has_fields = False
    has_xfa = '/XFA' in acro
    if has_fields and not has_xfa:
        return 'AcroForm'
    if has_fields and has_xfa:
        return 'AcroForm+XFA'
    if has_xfa:
        return 'XFA'
    return 'None'


def safe_str(v):
    if v is None:
        return ''
    try:
        return str(v)
    except Exception:
        return ''


def collect_fields(reader, max_fields):
    # One entry per field; radio groups and mirrored fields have several
    # widgets but are listed once (page = first widget's page).
    fields, by_name = [], {}
    truncated = False

    for page_index, widget in iter_widgets(reader.pages):
        field = field_dict(widget)
        name = qualified_name(field)
        if not name:
            continue
        ftype = field_type(widget)
        entry = by_name.get(name)
        if entry is None:
            if len(fields) >= max_fields:
                truncated = True
                by_name[name] = {}
                continue
            try:
                flags = int(inherited(widget, '/Ff') or 0)
            except Exception:
                flags = 0
            value = inherited(field, '/V')
            if isinstance(value, list):
                value = ', '.join(safe_str(v) for v in value)
            entry = {
                'name':          name,
                'type':          ftype,
                'page':          page_index,
                'current_value': name_str(value) if ftype in ('checkbox', 'radio') else safe_str(value),
                'required':      bool(flags & FF_REQUIRED),
            }
            tooltip = safe_str(inherited(field, '/TU'))
            if tooltip:
                entry['tooltip'] = tooltip
            if ftype in ('dropdown', 'listbox'):
                entry['options'] = field_options(field)
            fields.append(entry)
            by_name[name] = entry
        if ftype in ('checkbox', 'radio') and entry:
            # The on-state names are what pdf_fill_form needs to set a
            # checkbox or pick a radio option.
            states = entry.setdefault('on_states', [])
            for st in widget_on_states(widget):
                if st not in states:
                    states.append(st)

    for e in fields:
        if 'on_states' in e and not e['on_states']:
            del e['on_states']
    return fields, truncated, len(by_name)


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

    pypdf, import_errors = load_pypdf()
    if pypdf is None:
        emit({
            'ok': False,
            'helper': 'pdf_inspect_form',
            'error': 'Missing PDF form dependency: the Python package pypdf is not installed.',
            'details': import_errors[-6:],
        }, 8)

    try:
        reader = pypdf.PdfReader(str(target))
        if reader.is_encrypted:
            reader.decrypt('')
        page_count = len(reader.pages)
    except Exception as ex:
        emit({'ok': False, 'helper': 'pdf_inspect_form', 'error': 'Could not open PDF: ' + str(ex), 'path': str(target)}, 6)

    kind = form_kind(reader)

    if kind == 'XFA':
        emit({
            'ok': False,
            'helper': 'pdf_inspect_form',
            'error': 'This PDF uses XFA forms (older Adobe LiveCycle format), not AcroForm. pdf_inspect_form and pdf_fill_form do not support XFA. Common XFA examples: older IRS forms, some state DMV forms. Re-saving the PDF in a recent Acrobat or printing-to-PDF often produces an AcroForm-compatible copy.',
            'input_path': str(target),
            'page_count': page_count,
            'form_type': kind,
            'extractor_module': 'pypdf',
        }, 10)

    if kind == 'None':
        emit({
            'ok': False,
            'helper': 'pdf_inspect_form',
            'error': 'This PDF does not contain any fillable form fields. It may be a flat document or a scanned image.',
            'input_path': str(target),
            'page_count': page_count,
            'form_type': kind,
            'extractor_module': 'pypdf',
        }, 11)

    max_fields = 500
    try:
        fields, truncated, total_seen = collect_fields(reader, max_fields)
    except Exception as ex:
        emit({'ok': False, 'helper': 'pdf_inspect_form', 'error': 'Could not read the form fields: ' + str(ex), 'path': str(target)}, 6)

    warnings = []
    if truncated:
        warnings.append(f'Only the first {max_fields} of {total_seen} fields are listed.')
    if kind == 'AcroForm+XFA':
        warnings.append('PDF carries both AcroForm and XFA dictionaries. Reading AcroForm fields; XFA-only fields will not appear.')

    emit({
        'ok': True,
        'helper': 'pdf_inspect_form',
        'input_path': str(target),
        'cwd': str(cwd),
        'file_size_bytes': size_bytes,
        'page_count': page_count,
        'form_type': kind,
        'field_count': len(fields),
        'total_fields_seen': total_seen,
        'fields': fields,
        'extractor_module': 'pypdf',
        'warnings': warnings,
    })


if __name__ == '__main__':
    main()
