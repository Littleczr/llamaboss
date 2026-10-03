
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


def main():
    if len(sys.argv) != 4:
        emit({'ok': False, 'helper': 'pdf_fill_form', 'error': 'pdf_fill_form expects exactly three internal arguments: input path, JSON field map, fixed output directory'}, 2)

    cwd = Path.cwd().resolve()
    raw_path_arg = sys.argv[1]
    raw_json_arg_in = sys.argv[2]
    if raw_json_arg_in.startswith('@'):
        try:
            raw_json_arg = Path(raw_json_arg_in[1:]).read_text(encoding='utf-8')
        except Exception as ex:
            emit({'ok': False, 'helper': 'pdf_fill_form', 'error': 'Could not read field-map JSON file: ' + str(ex)}, 12)
    else:
        raw_json_arg = raw_json_arg_in
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
