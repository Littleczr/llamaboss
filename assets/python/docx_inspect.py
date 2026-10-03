
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

    try:
        user_site = getattr(site, 'USER_SITE', None)
        if user_site:
            site.addsitedir(user_site)
    except Exception as ex:
        errors.append('user-site enable failed: ' + str(ex))

    mod = try_import()
    return mod, errors


def heading_level(style_name):
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
                return n
        except Exception:
            return 0
    return 0



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


def inspect_docx_fallback(target, cwd, size_bytes):
    try:
        with zipfile.ZipFile(target, 'r') as zf:
            names = set(zf.namelist())
            xml_bytes = zf.read('word/document.xml')
    except KeyError:
        emit({'ok': False, 'helper': 'docx_inspect', 'error': 'DOCX fallback could not find word/document.xml', 'path': str(target)}, 8)
    except Exception as ex:
        emit({'ok': False, 'helper': 'docx_inspect', 'error': 'DOCX fallback could not open ZIP/XML content: ' + str(ex), 'path': str(target)}, 8)

    try:
        root = ET.fromstring(xml_bytes)
    except Exception as ex:
        emit({'ok': False, 'helper': 'docx_inspect', 'error': 'DOCX fallback could not parse document XML: ' + str(ex), 'path': str(target)}, 8)

    body = root.find(W_TAG + 'body')
    if body is None:
        emit({'ok': False, 'helper': 'docx_inspect', 'error': 'DOCX fallback could not locate document body', 'path': str(target)}, 8)

    paragraph_count = 0
    table_count = 0
    headings = []
    tables = []
    styles_in_use = set()
    HEADING_CAP = 100
    TABLE_CAP = 100

    for child in list(body):
        name = local_name(child.tag)
        if name == 'p':
            paragraph_count += 1
            style_id = paragraph_style_id(child)
            if style_id:
                styles_in_use.add(style_id)
            level = heading_level_from_style_id(style_id)
            text = paragraph_text_from_xml(child)
            if level > 0 and text and len(headings) < HEADING_CAP:
                snippet = text if len(text) <= 200 else (text[:200] + '...')
                headings.append({'level': level, 'text': snippet})
        elif name == 'tbl':
            table_count += 1
            if len(tables) < TABLE_CAP:
                rows = child.findall('.//' + W_TAG + 'tr')
                rcount = len(rows)
                ccount = 0
                for r in rows:
                    ccount = max(ccount, len(r.findall(W_TAG + 'tc')))
                tables.append({'rows': rcount, 'cols': ccount})

    section_count = len(body.findall(W_TAG + 'sectPr'))
    has_images = any(n.startswith('word/media/') for n in names)

    emit({
        'ok': True,
        'helper': 'docx_inspect',
        'input_path':      str(target),
        'cwd':             str(cwd),
        'file_size_bytes': size_bytes,
        'paragraph_count': paragraph_count,
        'heading_count':   len(headings),
        'headings':        headings,
        'headings_truncated': len(headings) >= HEADING_CAP,
        'table_count':     table_count,
        'tables':          tables,
        'tables_truncated': len(tables) >= TABLE_CAP,
        'section_count':   section_count,
        'has_images':      has_images,
        'styles_in_use':   sorted(styles_in_use),
        'extractor':       'builtin_zip_xml_fallback',
        'fallback_used':   True,
    })


def main():
    if len(sys.argv) != 2:
        emit({'ok': False, 'helper': 'docx_inspect', 'error': 'docx_inspect expects exactly one internal argument: input path'}, 2)

    cwd = Path.cwd().resolve()
    raw_arg = sys.argv[1]

    target = Path(raw_arg)
    if not target.is_absolute():
        target = cwd / target
    target = target.resolve()


    if target.suffix.lower() not in ('.docx', '.docm'):
        emit({'ok': False, 'helper': 'docx_inspect', 'error': 'Only .docx or .docm files are supported by docx_inspect', 'path': str(target)}, 4)

    if not target.exists() or not target.is_file():
        emit({'ok': False, 'helper': 'docx_inspect', 'error': 'File not found', 'path': str(target)}, 5)

    max_file_bytes = 50 * 1024 * 1024
    size_bytes = target.stat().st_size
    if size_bytes > max_file_bytes:
        emit({'ok': False, 'helper': 'docx_inspect', 'error': 'DOCX is larger than the first-phase 50 MB safety cap', 'path': str(target), 'file_size_bytes': size_bytes}, 7)

    docx_mod, import_errors = load_docx_module()
    if docx_mod is None:
        inspect_docx_fallback(target, cwd, size_bytes)

    try:
        doc = docx_mod.Document(str(target))
    except Exception as ex:
        emit({'ok': False, 'helper': 'docx_inspect', 'error': 'Could not open DOCX: ' + str(ex), 'path': str(target)}, 6)

    try:
        from docx.oxml.ns import qn
        from docx.text.paragraph import Paragraph
        from docx.table import Table
    except Exception as ex:
        emit({'ok': False, 'helper': 'docx_inspect', 'error': 'python-docx internals not available: ' + str(ex)}, 8)

    body = doc.element.body
    paragraph_count = 0
    table_count = 0
    headings = []          # list of {level, text}
    tables = []            # list of {rows, cols}
    styles_in_use = set()

    # Cap heading list and table list so a giant document doesn't
    # produce a multi-megabyte JSON summary.  100 of each is plenty
    # for navigation.
    HEADING_CAP = 100
    TABLE_CAP   = 100

    for child in body.iterchildren():
        tag = child.tag
        if tag == qn('w:p'):
            paragraph_count += 1
            p = Paragraph(child, doc.part)
            style_name = (p.style.name if p.style is not None else '') or ''
            if style_name:
                styles_in_use.add(style_name)
            level = heading_level(style_name)
            text = (p.text or '').strip()
            if level > 0 and text and len(headings) < HEADING_CAP:
                # Truncate very long headings so the JSON stays compact.
                snippet = text if len(text) <= 200 else (text[:200] + '...')
                headings.append({'level': level, 'text': snippet})
        elif tag == qn('w:tbl'):
            table_count += 1
            if len(tables) < TABLE_CAP:
                t = Table(child, doc.part)
                rows = list(t.rows)
                rcount = len(rows)
                ccount = max((len(r.cells) for r in rows), default=0)
                tables.append({'rows': rcount, 'cols': ccount})

    section_count = 0
    try:
        section_count = len(doc.sections)
    except Exception:
        section_count = 0

    has_images = False
    try:
        # Inline shapes are images embedded in paragraphs.  We don't
        # extract them; we just report their presence so the model
        # knows the doc has visuals.
        has_images = len(doc.inline_shapes) > 0
    except Exception:
        has_images = False

    emit({
        'ok': True,
        'helper': 'docx_inspect',
        'input_path':      str(target),
        'cwd':             str(cwd),
        'file_size_bytes': size_bytes,
        'paragraph_count': paragraph_count,
        'heading_count':   len(headings),
        'headings':        headings,
        'headings_truncated': len(headings) >= HEADING_CAP,
        'table_count':     table_count,
        'tables':          tables,
        'tables_truncated': len(tables) >= TABLE_CAP,
        'section_count':   section_count,
        'has_images':      has_images,
        'styles_in_use':   sorted(styles_in_use),
        'extractor':       'python-docx',
        'fallback_used':   False,
    })


if __name__ == '__main__':
    main()
