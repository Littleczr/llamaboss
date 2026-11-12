
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


FF_RADIO = 1 << 15
MAX_FORM_FIELDS = 500


def _plain(v):
    if v is None:
        return None
    s = str(v)
    return s[1:] if s.startswith('/') else s


def describe_field_value(field):
    """Human/model-readable value for one AcroForm field."""
    ft = str(field.get('/FT') or '')
    v = field.get('/V')
    if ft == '/Btn':
        try:
            ff = int(field.get('/Ff') or 0)
        except Exception:
            ff = 0
        state = _plain(v)
        if ff & FF_RADIO:
            return ('radio', 'nothing selected' if state in (None, '', 'Off') else f'selected: {state}',
                    state not in (None, '', 'Off'))
        if state in (None, '', 'Off'):
            return ('checkbox', 'unchecked', False)
        return ('checkbox', f'checked ({state})', True)
    if ft == '/Sig':
        return ('signature', 'signed' if v is not None else 'not signed', v is not None)
    if ft == '/Ch':
        if isinstance(v, (list, tuple)):
            vals = [_plain(x) or '' for x in v]
            return ('choice', ', '.join(vals) if vals else '(empty)', bool(vals))
        txt = _plain(v)
        return ('choice', txt if txt else '(empty)', bool(txt))
    txt = '' if v is None else str(v)
    return ('text', txt if txt.strip() else '(empty)', bool(txt.strip()))


def collect_form_fields(reader, warnings):
    """Interactive form values live in the AcroForm, not in page text, so
    page.extract_text() never sees what was typed into a filled form."""
    try:
        getter = getattr(reader, 'get_fields', None) or getattr(reader, 'getFields', None)
        fields = getter() if getter else None
    except Exception as ex:
        warnings.append('Could not read form fields: ' + str(ex))
        return []
    if not fields:
        return []
    out = []
    for name, field in fields.items():
        try:
            if not hasattr(field, 'get'):
                continue
            ft = field.get('/FT')
            if ft is None and field.get('/V') is None:
                continue  # non-terminal parent node
            kind, text, filled = describe_field_value(field)
        except Exception:
            continue
        text = text.replace('\r\n', '\n').replace('\r', '\n').replace('\x00', '')
        out.append({'name': str(name), 'type': kind, 'value': text, 'filled': filled})
        if len(out) >= MAX_FORM_FIELDS:
            warnings.append(f'Only the first {MAX_FORM_FIELDS} form fields are listed.')
            break
    return out


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

    form_fields = collect_form_fields(reader, warnings)
    filled_fields = sum(1 for f in form_fields if f['filled'])
    if form_fields:
        warnings.append(
            f'This PDF has {len(form_fields)} interactive form field(s) ({filled_fields} filled). '
            'Their values are NOT part of the page text; they are listed in the "Form fields" section '
            'and form_fields. Use those values when answering about what was entered in the form.')

    if extracted_chars == 0 and not filled_fields:
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

    if form_fields:
        out.append('## Form fields')
        out.append('')
        out.append('Values entered in the interactive form (not part of the page text above).')
        out.append('')
        out.append('| Field | Type | Value |')
        out.append('| --- | --- | --- |')
        for f in form_fields:
            cell = lambda s: s.replace('|', '\\|').replace('\n', '<br>')
            out.append(f"| {cell(f['name'])} | {f['type']} | {cell(f['value'])} |")
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
        'form_field_count': len(form_fields),
        'form_fields_filled': filled_fields,
        'form_fields': form_fields[:100],
        'warnings': warnings,
    })


if __name__ == '__main__':
    main()
