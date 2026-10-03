
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
