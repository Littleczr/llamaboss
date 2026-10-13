#!/usr/bin/env python3
"""Build a PC package for a shared-code change prepared on a branch from main.

    macos/make_pc_package.py <item> <repo> <base> <head> <out_dir> [--notes FILE] [--requires ITEM]

<repo> is a checkout (or worktree) containing both commits.  The zip holds the
files changed between <base> and <head> (added or modified; deletions are not
supported) at repo-relative paths, a README.txt, and install_<item>.py, which:

  * --dry-run: checks every repo file and reports what it would do
  * refuses to touch anything if a repo file is not the expected original
    (compared with CRLF/LF normalised, since the PC checkout may use CRLF)
  * backs the originals up outside the repo (%USERPROFILE%\\LlamaBoss_Backups)
  * writes each file in the line-ending style the repo copy already uses
  * verifies every written file by sha256
  * --restore [backup dir]: puts the originals back (latest backup by default)

The user applies packages on the PC; nothing here touches main or GitHub.
"""
import hashlib
import io
import json
import os
import subprocess
import sys
import zipfile

DEFAULT_PC_REPO = r"C:\Users\Cesar\source\repos\LlamaBoss"


def git(repo, *args, binary=False):
    out = subprocess.run(["git", "-C", repo, *args], check=True, capture_output=True).stdout
    return out if binary else out.decode()


def lf_sha(data):
    return hashlib.sha256(data.replace(b"\r\n", b"\n")).hexdigest()


INSTALLER = r'''#!/usr/bin/env python3
r"""install___ITEM__.py - apply or undo the "__ITEM__" package on the PC repo.

    py -3 install___ITEM__.py [--repo PATH] --dry-run
    py -3 install___ITEM__.py [--repo PATH]
    py -3 install___ITEM__.py [--repo PATH] --restore [BACKUP_DIR]

Run it from the folder the zip was extracted to.  Default repo:
__DEFAULT_REPO__
"""
import argparse, datetime, hashlib, json, os, shutil, sys

ITEM = "__ITEM__"
BASE = "__BASE__"
HEAD = "__HEAD__"
REQUIRES_HINT = "__REQUIRES_HINT__"
# path -> {"orig": LF-normalised sha256 of the expected original, or None for a new file,
#          "new":  LF-normalised sha256 of the packaged file}
MANIFEST = json.loads(r"""__MANIFEST__""")
HERE = os.path.dirname(os.path.abspath(__file__))
BACKUP_ROOT = os.path.join(os.path.expanduser("~"), "LlamaBoss_Backups")


def lf_sha(data):
    return hashlib.sha256(data.replace(b"\r\n", b"\n")).hexdigest()


def raw_sha(data):
    return hashlib.sha256(data).hexdigest()


def read(p):
    with open(p, "rb") as f:
        return f.read()


def uses_crlf(data):
    return data.count(b"\r\n") * 2 > data.count(b"\n")


def fail(msg):
    print("STOPPED: " + msg)
    print("Nothing was changed.")
    sys.exit(1)


def install(repo, dry):
    plan = []
    for rel, h in MANIFEST.items():
        pkg = os.path.join(HERE, *rel.split("/"))
        dst = os.path.join(repo, *rel.split("/"))
        if not os.path.isfile(pkg):
            fail("package file missing: " + rel)
        new = read(pkg)
        if lf_sha(new) != h["new"]:
            fail("package file is damaged (sha256 mismatch): " + rel)
        if os.path.exists(dst):
            cur = read(dst)
            s = lf_sha(cur)
            if s == h["new"]:
                plan.append((rel, "already installed", None, None)); continue
            if h["orig"] is None:
                fail(rel + " already exists in the repo and is not this package's file")
            if s != h["orig"]:
                fail(rel + " is not the version this package was built on (" + BASE[:9] + ")" + REQUIRES_HINT)
            crlf = uses_crlf(cur)
        else:
            if h["orig"] is not None:
                fail(rel + " is missing from the repo")
            cur, crlf = None, False
        out = new.replace(b"\r\n", b"\n")
        if crlf:
            out = out.replace(b"\n", b"\r\n")
        plan.append((rel, "replace" if cur is not None else "add", cur, out))

    for rel, action, _, out in plan:
        eol = "" if out is None else (" (CRLF)" if b"\r\n" in out else " (LF)")
        print("  %-17s %s%s" % (action, rel, eol))
    todo = [p for p in plan if p[2] is not None or p[1] == "add"]
    if dry:
        print("Dry run: %d file(s) would change, %d already installed. Nothing was changed."
              % (len(todo), len(plan) - len(todo)))
        return
    if not todo:
        print("Already installed; nothing to do."); return

    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    bdir = os.path.join(BACKUP_ROOT, ITEM + "_" + stamp)
    os.makedirs(bdir)
    record = {"item": ITEM, "repo": repo, "base": BASE, "head": HEAD, "files": {}}
    for rel, action, cur, out in todo:
        entry = {"action": action, "installed_sha256": raw_sha(out)}
        if cur is not None:
            b = os.path.join(bdir, *rel.split("/"))
            os.makedirs(os.path.dirname(b), exist_ok=True)
            with open(b, "wb") as f:
                f.write(cur)
            if raw_sha(read(b)) != raw_sha(cur):
                fail("backup of " + rel + " did not verify")
            entry["original_sha256"] = raw_sha(cur)
        record["files"][rel] = entry
    with open(os.path.join(bdir, "backup.json"), "w") as f:
        json.dump(record, f, indent=2)
    print("Backup: " + bdir)

    for rel, action, cur, out in todo:
        dst = os.path.join(repo, *rel.split("/"))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as f:
            f.write(out)
    bad = [rel for rel, _, _, out in todo
           if raw_sha(read(os.path.join(repo, *rel.split("/")))) != raw_sha(out)]
    if bad:
        print("VERIFY FAILED for: " + ", ".join(bad))
        print("Undo with: py -3 install_" + ITEM + ".py --restore \"" + bdir + "\"")
        sys.exit(2)
    print("Installed and verified %d file(s)." % len(todo))
    print("Undo with: py -3 install_" + ITEM + ".py --restore")


def restore(repo, bdir, dry):
    if not bdir:
        if not os.path.isdir(BACKUP_ROOT):
            fail("no backups in " + BACKUP_ROOT)
        cands = sorted(d for d in os.listdir(BACKUP_ROOT) if d.startswith(ITEM + "_"))
        if not cands:
            fail("no backups for " + ITEM + " in " + BACKUP_ROOT)
        bdir = os.path.join(BACKUP_ROOT, cands[-1])
    with open(os.path.join(bdir, "backup.json")) as f:
        record = json.load(f)
    print("Restoring from " + bdir)
    for rel, e in record["files"].items():
        dst = os.path.join(repo, *rel.split("/"))
        if os.path.exists(dst) and raw_sha(read(dst)) != e["installed_sha256"]:
            fail(rel + " was changed after the install; restore it by hand from " + bdir)
    for rel, e in record["files"].items():
        dst = os.path.join(repo, *rel.split("/"))
        if e["action"] == "add":
            print("  remove   " + rel)
            if not dry and os.path.exists(dst):
                os.remove(dst)
        else:
            print("  restore  " + rel)
            if not dry:
                data = read(os.path.join(bdir, *rel.split("/")))
                with open(dst, "wb") as f:
                    f.write(data)
                if raw_sha(read(dst)) != e["original_sha256"]:
                    fail("restored " + rel + " did not verify")
    print("Dry run: nothing was changed." if dry else "Restored and verified.")


def main():
    ap = argparse.ArgumentParser(description="Install or undo the " + ITEM + " package.")
    ap.add_argument("--repo", default=r"__DEFAULT_REPO__")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--restore", nargs="?", const="", metavar="BACKUP_DIR")
    a = ap.parse_args()
    repo = os.path.abspath(a.repo)
    if not os.path.isdir(os.path.join(repo, ".git")):
        fail(repo + " is not a git checkout")
    print(ITEM + " package (" + BASE[:9] + " -> " + HEAD[:9] + "), repo " + repo)
    if a.restore is not None:
        restore(repo, a.restore, a.dry_run)
    else:
        install(repo, a.dry_run)


if __name__ == "__main__":
    main()
'''


def main():
    args = sys.argv[1:]
    notes = None
    requires = None
    if "--requires" in args:
        i = args.index("--requires"); requires = args[i + 1]; del args[i:i + 2]
    if "--notes" in args:
        i = args.index("--notes"); notes = open(args[i + 1]).read(); del args[i:i + 2]
    item, repo, base, head, out_dir = args
    base = git(repo, "rev-parse", base).strip()
    head = git(repo, "rev-parse", head).strip()
    manifest, files = {}, {}
    for line in git(repo, "diff", "--name-status", "--no-renames", base, head).splitlines():
        status, path = line.split("\t", 1)
        if status not in ("A", "M"):
            sys.exit("unsupported change %s %s" % (status, path))
        new = git(repo, "show", "%s:%s" % (head, path), binary=True)
        orig = None if status == "A" else lf_sha(git(repo, "show", "%s:%s" % (base, path), binary=True))
        manifest[path] = {"orig": orig, "new": lf_sha(new)}
        files[path] = new
    installer = (INSTALLER.replace("__ITEM__", item).replace("__BASE__", base)
                 .replace("__HEAD__", head)
                 .replace("__REQUIRES_HINT__", ("; install the " + requires + " package first") if requires else "").replace("__DEFAULT_REPO__", DEFAULT_PC_REPO)
                 .replace("__MANIFEST__", json.dumps(manifest, indent=4, sort_keys=True)))
    readme = (f"{item} package\n\nShared-code change prepared on the Mac against {base[:9]}"
              + (f" (main + the {requires} package; install that first)" if requires else " (main)") + ".\n"
              f"Files ({len(files)}):\n" + "".join(f"  {p}\n" for p in sorted(files)) +
              ("\n" + notes if notes else ""))
    os.makedirs(out_dir, exist_ok=True)
    zpath = os.path.join(out_dir, f"LlamaBoss_Patch_{item}.zip")
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for p in sorted(files):
            z.writestr(p, files[p])
        z.writestr(f"install_{item}.py", installer)
        z.writestr("README.txt", readme)
    print(zpath, f"{len(files)} files")


if __name__ == "__main__":
    main()
