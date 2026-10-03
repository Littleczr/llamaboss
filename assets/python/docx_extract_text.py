
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
