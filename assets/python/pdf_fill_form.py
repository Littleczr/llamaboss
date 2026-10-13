
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


def collect_field_metadata(reader):
    """Walk the form once and capture every per-field detail needed for
    validation as plain Python data: type, first page, on-states and
    options.  Radio groups and mirrored fields appear once."""
    meta = {}
    for page_index, widget in iter_widgets(reader.pages):
        field = field_dict(widget)
        name = qualified_name(field)
        if not name:
            continue
        ftype = field_type(widget)
        entry = meta.get(name)
        if entry is None:
            entry = {
                'type':            ftype,
                'first_page':      page_index,
                'on_states':       [],
                'options':         [],
                'instance_count':  0,
            }
            meta[name] = entry
        if ftype in ('checkbox', 'radio'):
            for st in widget_on_states(widget):
                if st not in entry['on_states']:
                    entry['on_states'].append(st)
        if ftype in ('dropdown', 'listbox') and not entry['options']:
            entry['options'] = field_options(field)
        entry['instance_count'] += 1
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

    pypdf, import_errors = load_pypdf()
    if pypdf is None:
        emit({
            'ok': False,
            'helper': 'pdf_fill_form',
            'error': 'Missing PDF form dependency: the Python package pypdf is not installed.',
            'details': import_errors[-6:],
        }, 8)
    mod_name = 'pypdf'

    try:
        reader = pypdf.PdfReader(str(target))
        if reader.is_encrypted:
            reader.decrypt('')
        len(reader.pages)   # parse the page tree now, so a damaged file fails here
    except Exception as ex:
        emit({'ok': False, 'helper': 'pdf_fill_form', 'error': 'Could not open PDF: ' + str(ex), 'path': str(target)}, 6)

    form_kind_name = form_kind(reader)
    if form_kind_name == 'XFA':
        emit({
            'ok': False,
            'helper': 'pdf_fill_form',
            'error': 'This PDF uses XFA forms (older Adobe LiveCycle format), not AcroForm. pdf_fill_form does not support XFA. Re-saving the PDF in a recent Acrobat or printing-to-PDF often produces an AcroForm-compatible copy.',
            'input_path': str(target),
            'form_type': form_kind_name,
        }, 10)
    if form_kind_name == 'None':
        emit({
            'ok': False,
            'helper': 'pdf_fill_form',
            'error': 'This PDF does not contain any fillable form fields. There is nothing for pdf_fill_form to fill.',
            'input_path': str(target),
            'form_type': form_kind_name,
        }, 11)

    field_meta = collect_field_metadata(reader)

    # ── Validation pass: hard-fail policy.  Any unknown field name or
    # invalid value rejects the WHOLE call.  We collect every error
    # before reporting so the model gets one complete picture instead
    # of having to retry repeatedly to surface each problem.  Operates
    # purely on field_meta (plain Python data).
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
            'form_type': form_kind_name,
            'validation_errors': errors,
            'extractor_module': mod_name,
        }, 15)

    # ── Write pass, on a copy of the document.  Text and choice values
    # go through pypdf, which also writes each widget's appearance stream:
    # viewers such as Preview draw that stream, not the raw value.
    # Checkboxes and radios are set here: /V on the field (a PDF name)
    # and /AS on every widget, the on-state on the matching widget and
    # /Off on the others, so the existing check/dot appearances show.
    from pypdf.generic import NameObject

    fill_results = []
    applied = set()  # names that have already produced a fill_results entry
    try:
        writer = pypdf.PdfWriter(clone_from=reader)
        text_values = {}  # page index -> {qualified name: value}
        for page_index, widget in iter_widgets(writer.pages):
            field = field_dict(widget)
            name = qualified_name(field)
            if not name or name not in plan:
                continue
            ftype, resolved, original = plan[name]
            if ftype in ('checkbox', 'radio'):
                on = NameObject('/' + resolved)
                states = []
                try:
                    states = list(widget['/AP'].get_object()['/N'].get_object().keys())
                except Exception:
                    pass
                widget[NameObject('/AS')] = on if on in states else NameObject('/Off')
                field[NameObject('/V')] = on if resolved != 'Off' else NameObject('/Off')
            else:
                text_values.setdefault(page_index, {})[name] = resolved
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
        warnings = []
        if form_kind_name == 'AcroForm+XFA':
            # Acrobat prefers the XFA copy of the form and would show it
            # empty; without /XFA every viewer uses the AcroForm values.
            acro = writer._root_object['/AcroForm'].get_object()
            if '/XFA' in acro:
                del acro['/XFA']
            warnings.append('This PDF also carried an XFA form. The filled copy keeps only the AcroForm fields (what Preview, browsers and Acrobat then show).')
        for page_index, values in text_values.items():
            # auto_regenerate=False: the appearance streams are written
            # now, so viewers need not rebuild them.
            writer.update_page_form_field_values(
                writer.pages[page_index - 1], values, auto_regenerate=False)
    except Exception as ex:
        emit({
            'ok': False,
            'helper': 'pdf_fill_form',
            'error': 'Failed during field update: ' + str(ex),
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
        with open(output_path, 'wb') as f:
            writer.write(f)
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
        'form_type':        form_kind_name,
        'fields_filled':    len(fill_results),
        'fields':           fill_results,
        'output_size_bytes': out_size,
        'extractor_module': mod_name,
        'warnings':         warnings,
    })


if __name__ == '__main__':
    main()
