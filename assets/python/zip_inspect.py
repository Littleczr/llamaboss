
import json
import sys
import zipfile
import posixpath
from pathlib import Path


def emit(obj, code=0):
    print(json.dumps(obj, indent=2, ensure_ascii=False))
    raise SystemExit(code)


METHOD_NAMES = {
    0: 'store', 8: 'deflate', 9: 'deflate64', 12: 'bzip2',
    14: 'lzma', 99: 'aes',
}

EXEC_EXTS = {
    '.exe', '.dll', '.bat', '.cmd', '.com', '.scr', '.msi',
    '.ps1', '.psm1', '.vbs', '.vbe', '.js', '.jse', '.jar',
    '.sh', '.app', '.lnk',
}


def looks_like_traversal(name):
    # The ZIP spec mandates forward slashes, but hostile archives embed
    # backslashes to dodge naive checks; normalize both. Flag absolute
    # paths, Windows drive roots, and any '..' path component (Zip Slip).
    n = name.replace('\\', '/')
    if n.startswith('/'):
        return True
    if len(n) >= 2 and n[1] == ':' and n[0].isalpha():
        return True
    parts = n.split('/')
    return any(p == '..' for p in parts)


def is_symlink(info):
    # Unix mode lives in the high 16 bits of external_attr.
    mode = (info.external_attr >> 16) & 0o170000
    return mode == 0o120000


def main():
    if len(sys.argv) != 2:
        emit({'ok': False, 'helper': 'zip_inspect',
              'error': 'zip_inspect expects exactly one internal argument: input path'}, 2)

    cwd = Path.cwd().resolve()
    raw_arg = sys.argv[1]
    target = Path(raw_arg)
    if not target.is_absolute():
        target = cwd / target
    target = target.resolve()

    if target.suffix.lower() != '.zip':
        emit({'ok': False, 'helper': 'zip_inspect',
              'error': 'Only .zip files are supported by zip_inspect',
              'path': str(target)}, 4)

    if not target.exists() or not target.is_file():
        emit({'ok': False, 'helper': 'zip_inspect',
              'error': 'File not found', 'path': str(target)}, 5)

    size_bytes = target.stat().st_size
    max_file_bytes = 1024 * 1024 * 1024  # 1 GB cap on the archive file itself
    if size_bytes > max_file_bytes:
        emit({'ok': False, 'helper': 'zip_inspect',
              'error': 'Archive is larger than the 1 GB inspection cap',
              'path': str(target), 'file_size_bytes': size_bytes}, 7)

    if not zipfile.is_zipfile(target):
        emit({'ok': False, 'helper': 'zip_inspect',
              'error': 'Not a valid ZIP archive (bad signature or unsupported variant)',
              'path': str(target), 'file_size_bytes': size_bytes}, 6)

    # This helper only reads central-directory metadata via infolist();
    # it never calls .read()/.extract*(), so no entry is decompressed and
    # a zip bomb cannot detonate at inspection time.
    ENTRY_SCAN_CAP = 50000   # stop walking pathological entry counts
    ENTRY_DETAIL_CAP = 500   # entries returned verbatim in the manifest

    entries = []
    file_count = 0
    dir_count = 0
    total_uncompressed = 0
    total_compressed = 0
    encrypted_count = 0
    traversal_suspects = []
    symlink_suspects = []
    executable_entries = []
    backslash_entries = []
    backslash_count = 0
    top_level = set()
    max_depth = 0
    max_entry_ratio = 0.0
    ext_hist = {}
    warnings = []

    try:
        with zipfile.ZipFile(target, 'r') as zf:
            comment = zf.comment.decode('utf-8', 'replace') if zf.comment else ''
            infos = zf.infolist()
            scanned = 0
            for info in infos:
                scanned += 1
                if scanned > ENTRY_SCAN_CAP:
                    warnings.append('Stopped scanning at ' + str(ENTRY_SCAN_CAP) + ' entries.')
                    break

                name = info.filename
                norm = name.replace('\\', '/')
                if '\\' in name:
                    backslash_count += 1
                    if len(backslash_entries) < 20:
                        backslash_entries.append(name)
                is_dir = info.is_dir()
                comp = info.compress_size
                uncomp = info.file_size
                encrypted = bool(info.flag_bits & 0x1) or info.compress_type == 99
                sym = is_symlink(info)

                if is_dir:
                    dir_count += 1
                else:
                    file_count += 1
                    total_uncompressed += uncomp
                    total_compressed += comp
                    ext = posixpath.splitext(norm)[1].lower()
                    if ext:
                        ext_hist[ext] = ext_hist.get(ext, 0) + 1
                        if ext in EXEC_EXTS and len(executable_entries) < 100:
                            executable_entries.append(name)
                    ratio = (uncomp / comp) if comp > 0 else 0.0
                    if ratio > max_entry_ratio:
                        max_entry_ratio = ratio

                if encrypted:
                    encrypted_count += 1
                if sym and len(symlink_suspects) < 100:
                    symlink_suspects.append(name)
                if looks_like_traversal(name) and len(traversal_suspects) < 100:
                    traversal_suspects.append(name)

                stripped = norm.strip('/')
                if stripped:
                    top_level.add(stripped.split('/')[0])
                    depth = stripped.count('/') + 1
                    if depth > max_depth:
                        max_depth = depth

                if len(entries) < ENTRY_DETAIL_CAP:
                    dt = info.date_time
                    modified = '%04d-%02d-%02d %02d:%02d:%02d' % dt if dt and dt[0] >= 1980 else ''
                    entries.append({
                        'name': name,
                        'is_dir': is_dir,
                        'size_bytes': uncomp,
                        'compressed_bytes': comp,
                        'method': METHOD_NAMES.get(info.compress_type, str(info.compress_type)),
                        'encrypted': encrypted,
                        'modified': modified,
                    })
    except zipfile.BadZipFile as ex:
        emit({'ok': False, 'helper': 'zip_inspect',
              'error': 'Corrupt or unreadable ZIP central directory: ' + str(ex),
              'path': str(target)}, 6)
    except Exception as ex:
        emit({'ok': False, 'helper': 'zip_inspect',
              'error': 'Could not read ZIP: ' + str(ex),
              'path': str(target)}, 6)

    entry_count = file_count + dir_count
    overall_ratio = (total_uncompressed / total_compressed) if total_compressed > 0 else 0.0

    # Compression-ratio heuristic: a normal archive sits well under ~100x.
    # A wildly higher ratio is the classic zip-bomb signature. We only
    # FLAG it here (inspection never extracts); a future zip_extract must
    # enforce real caps before writing anything.
    suspicious_ratio = overall_ratio >= 200.0 or max_entry_ratio >= 1000.0

    top_sorted = sorted(top_level)
    single_root = len(top_sorted) == 1
    ext_top = sorted(ext_hist.items(), key=lambda kv: (-kv[1], kv[0]))[:20]

    safety_flags = []
    if encrypted_count:
        safety_flags.append('encrypted_entries')
    if traversal_suspects:
        safety_flags.append('path_traversal_suspects')
    if symlink_suspects:
        safety_flags.append('symlink_entries')
    if executable_entries:
        safety_flags.append('executable_entries')
    if suspicious_ratio:
        safety_flags.append('high_compression_ratio')
    # Portability (2026-10-01): Windows PowerShell 5.1 Compress-Archive
    # writes entry names with '\\' separators.  Non-Windows unzip warns and
    # Python zipfile / macOS tools create flat files with literal
    # backslashes in their names, so a reviewer on Linux or macOS does not
    # get the folder tree.  Not a security risk, but listed in
    # safety_flags because that is where callers look before sharing.
    if backslash_count:
        safety_flags.append('backslash_path_separators')
        warnings.append(
            '%d entr%s use \\ as the path separator (typical of Windows '
            'PowerShell 5.1 Compress-Archive). Non-Windows tools treat it as '
            'part of the file name, so the folder structure is lost there. '
            'For a portable archive, build it with Python zipfile or with '
            '[System.IO.Compression.ZipFile] / ZipArchive.CreateEntry using '
            'names that contain / separators.'
            % (backslash_count, 'y' if backslash_count == 1 else 'ies'))

    emit({
        'ok': True,
        'helper': 'zip_inspect',
        'input_path': str(target),
        'cwd': str(cwd),
        'file_size_bytes': size_bytes,
        'comment': comment,
        'entry_count': entry_count,
        'file_count': file_count,
        'dir_count': dir_count,
        'total_uncompressed_bytes': total_uncompressed,
        'total_compressed_bytes': total_compressed,
        'overall_compression_ratio': round(overall_ratio, 2),
        'max_entry_compression_ratio': round(max_entry_ratio, 2),
        'top_level_entries': top_sorted[:100],
        'single_root_folder': single_root,
        'max_path_depth': max_depth,
        'extension_histogram': [{'ext': e, 'count': c} for e, c in ext_top],
        'encrypted_entry_count': encrypted_count,
        'has_encrypted_entries': encrypted_count > 0,
        'path_traversal_suspects': traversal_suspects,
        'symlink_entries': symlink_suspects,
        'executable_entries': executable_entries,
        'backslash_separator_entry_count': backslash_count,
        'backslash_separator_entries': backslash_entries,
        'high_compression_ratio': suspicious_ratio,
        'safety_flags': safety_flags,
        'entries': entries,
        'entries_truncated': entry_count > len(entries),
        'entry_detail_cap': ENTRY_DETAIL_CAP,
        'note': 'Read-only manifest. No entry was decompressed or written to disk. '
                'Before extracting, heed safety_flags: encrypted/symlink/traversal/executable '
                'entries and high compression ratios all warrant caution.',
        'warnings': warnings,
    })


if __name__ == '__main__':
    main()
