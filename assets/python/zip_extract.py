
import json
import os
import sys
import zipfile
import posixpath
from pathlib import Path


def emit(obj, code=0):
    print(json.dumps(obj, indent=2, ensure_ascii=False))
    raise SystemExit(code)


EXEC_EXTS = {
    '.exe', '.dll', '.bat', '.cmd', '.com', '.scr', '.msi',
    '.ps1', '.psm1', '.vbs', '.vbe', '.js', '.jse', '.jar',
    '.sh', '.app', '.lnk',
}

# Enforcement caps (the real teeth zip_inspect could only flag).
MAX_ARCHIVE_BYTES   = 1024 * 1024 * 1024          # 1 GB archive file
MAX_TOTAL_UNCOMP    = 2 * 1024 * 1024 * 1024       # 2 GB written total
MAX_ENTRY_COUNT     = 20000
MAX_ENTRY_RATIO     = 1000.0                        # per-entry declared ratio
CHUNK               = 1024 * 1024                   # 1 MB streaming chunks


def fail(msg, path, code=8, **extra):
    obj = {'ok': False, 'helper': 'zip_extract', 'error': msg, 'path': path}
    obj.update(extra)
    emit(obj, code)


def is_symlink(info):
    mode = (info.external_attr >> 16) & 0o170000
    return mode == 0o120000


def is_encrypted(info):
    return bool(info.flag_bits & 0x1) or info.compress_type == 99


def unique_dir(base):
    if not base.exists():
        return base
    i = 1
    while True:
        cand = base.with_name(base.name + ' (' + str(i) + ')')
        if not cand.exists():
            return cand
        i += 1


def safe_target(dest_root, entry_name):
    # Compute our OWN target path; never trust zipfile.extract() with the
    # raw name. Normalize separators, strip leading slashes/drive, then
    # confirm the resolved path stays under dest_root (defense in depth).
    norm = entry_name.replace('\\', '/')
    norm = norm.lstrip('/')
    if len(norm) >= 2 and norm[1] == ':' and norm[0].isalpha():
        return None
    parts = [p for p in norm.split('/') if p not in ('', '.')]
    if any(p == '..' for p in parts):
        return None
    target = (dest_root / Path(*parts)) if parts else dest_root
    try:
        target_res = target.resolve()
        root_res = dest_root.resolve()
    except Exception:
        return None
    if target_res != root_res and root_res not in target_res.parents:
        return None
    return target


def main():
    if len(sys.argv) != 3:
        emit({'ok': False, 'helper': 'zip_extract',
              'error': 'zip_extract expects exactly two internal arguments: <input.zip> <extracted_lane_dir>'}, 2)

    cwd = Path.cwd().resolve()
    raw_arg = sys.argv[1]
    lane_dir = Path(sys.argv[2])

    target = Path(raw_arg)
    if not target.is_absolute():
        target = cwd / target
    target = target.resolve()

    if target.suffix.lower() != '.zip':
        fail('Only .zip files are supported by zip_extract', str(target), 4)
    if not target.exists() or not target.is_file():
        fail('File not found', str(target), 5)

    size_bytes = target.stat().st_size
    if size_bytes > MAX_ARCHIVE_BYTES:
        fail('Archive is larger than the 1 GB extraction cap', str(target), 7,
             file_size_bytes=size_bytes)
    if not zipfile.is_zipfile(target):
        fail('Not a valid ZIP archive (bad signature or unsupported variant)',
             str(target), 6, file_size_bytes=size_bytes)

    dest_root = unique_dir(lane_dir / target.stem)

    # PASS 1: central-directory audit. NO decompression. Refuse the whole
    # archive on any security violation before writing a byte.
    refusals = []
    encrypted_entries = []
    executable_entries = []
    declared_total = 0
    file_entries = []
    try:
        with zipfile.ZipFile(target, 'r') as zf:
            infos = zf.infolist()
            if len(infos) > MAX_ENTRY_COUNT:
                fail('Archive exceeds the entry-count cap (' + str(MAX_ENTRY_COUNT) + ')',
                     str(target), 9, entry_count=len(infos))
            for info in infos:
                name = info.filename
                if is_symlink(info):
                    refusals.append({'name': name, 'reason': 'symlink entry'})
                    continue
                if is_encrypted(info):
                    encrypted_entries.append(name)
                    continue
                if info.is_dir():
                    continue
                if safe_target(dest_root, name) is None:
                    refusals.append({'name': name, 'reason': 'path escapes extraction root'})
                    continue
                declared_total += info.file_size
                if info.compress_size > 0:
                    ratio = info.file_size / info.compress_size
                    if ratio > MAX_ENTRY_RATIO:
                        refusals.append({'name': name,
                                         'reason': 'compression ratio %.0fx exceeds cap' % ratio})
                        continue
                ext = posixpath.splitext(name.replace('\\', '/'))[1].lower()
                if ext in EXEC_EXTS:
                    executable_entries.append(name)
                file_entries.append(info)

            if refusals:
                fail('Refused: ' + str(len(refusals)) + ' entry(ies) failed security checks. '
                     'No files were extracted.', str(target), 10, refused=refusals)
            if encrypted_entries:
                fail('Archive contains encrypted entries; zip_extract will not extract '
                     'encrypted archives. No files were extracted.', str(target), 11,
                     encrypted_entries=encrypted_entries[:50])
            if declared_total > MAX_TOTAL_UNCOMP:
                fail('Declared uncompressed size exceeds the 2 GB cap. No files were extracted.',
                     str(target), 12, declared_total_bytes=declared_total)

            # PASS 2: extract, streaming, with a hard written-bytes cap that
            # catches archives lying about their declared sizes.
            dest_root.mkdir(parents=True, exist_ok=True)
            written_total = 0
            extracted = []
            for info in file_entries:
                tgt = safe_target(dest_root, info.filename)
                if tgt is None:   # paranoia; pass 1 already checked
                    fail('Internal: path check failed during extraction. Partial output may exist.',
                         str(target), 13)
                tgt.parent.mkdir(parents=True, exist_ok=True)
                entry_cap = max(info.file_size * 2, 4096)   # tolerance over declared
                entry_written = 0
                with zf.open(info, 'r') as src, open(tgt, 'wb') as dst:
                    while True:
                        chunk = src.read(CHUNK)
                        if not chunk:
                            break
                        entry_written += len(chunk)
                        written_total += len(chunk)
                        if written_total > MAX_TOTAL_UNCOMP:
                            dst.close()
                            fail('Aborted: total extracted bytes exceeded the 2 GB cap '
                                 '(possible zip bomb). Partial output remains in the '
                                 'destination folder.', str(target), 14,
                                 destination=str(dest_root), bytes_written=written_total)
                        if entry_written > entry_cap:
                            dst.close()
                            fail('Aborted: entry "' + info.filename + '" produced far more '
                                 'data than its declared size (possible zip bomb). Partial '
                                 'output remains in the destination folder.', str(target), 15,
                                 destination=str(dest_root), entry=info.filename)
                        dst.write(chunk)
                extracted.append({'name': info.filename, 'bytes': entry_written})
    except zipfile.BadZipFile as ex:
        fail('Corrupt or unreadable ZIP: ' + str(ex), str(target), 6)
    except Exception as ex:
        fail('Could not extract ZIP: ' + str(ex), str(target), 6)

    image_exts = ('.png', '.jpg', '.jpeg', '.gif', '.bmp', '.webp')
    image_files = [str(dest_root / e['name']) for e in extracted
                   if e['name'].lower().endswith(image_exts)]

    result = {
        'ok': True,
        'helper': 'zip_extract',
        'input_path': str(target),
        'destination': str(dest_root),
        'extracted_count': len(extracted),
        'total_bytes_written': written_total,
        'executable_entries': executable_entries,
        'note': 'Extraction confined to the destination folder. All entry paths were '
                'validated against Zip Slip before writing; symlink, encrypted, and '
                'over-ratio entries are rejected. Executable entries (if any) were '
                'extracted but are listed above for your awareness.',
        'extracted': extracted[:200],
        'extracted_truncated': len(extracted) > 200,
    }
    if image_files:
        result['image_count'] = len(image_files)
        result['image_files'] = image_files[:50]
        result['next_step_for_images'] = (
            'To look at these pictures, call view_image with the image paths (or '
            'their folder). view_image attaches them to your vision input; read '
            'cannot show images. Max 8 images per view_image call.')
    emit(result)


if __name__ == '__main__':
    main()
