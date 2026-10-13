#!/bin/zsh
# macos/fetch_python.sh — the Python runtime bundled in LlamaBoss.app.
#
#   macos/fetch_python.sh [dest]        default: third_party/python
#
# Downloads python-build-standalone's CPython for Apple silicon (pinned and
# SHA-256 checked), drops what LlamaBoss never uses (Tcl/Tk, IDLE, the
# embedding libpython, pydoc data), installs the hash-pinned packages in
# macos/python-requirements.txt, removes the runtime's own pip (LlamaBoss's
# environment gets pip from ensurepip), and precompiles every module as
# unchecked-hash .pyc so Python never writes __pycache__ into the signed app.
# CMake copies the result to Contents/Resources/python-runtime.
set -euo pipefail

ROOT=${0:A:h:h}
DEST=${1:-$ROOT/third_party/python}

PBS_TAG=20261009
PY_VER=3.13.16
PY_MM=3.13
ASSET=cpython-$PY_VER+$PBS_TAG-aarch64-apple-darwin-install_only_stripped.tar.gz
SHA256=11785155d0f70e634178219c20925d8f20be00a3da4684fc4f5019dd15602ef7
URL=https://github.com/astral-sh/python-build-standalone/releases/download/$PBS_TAG/${ASSET//+/%2B}

DL=$ROOT/third_party/dl
mkdir -p $DL
if [[ ! -f $DL/$ASSET ]]; then
    curl -fL --retry 3 -o $DL/$ASSET.part $URL
    mv $DL/$ASSET.part $DL/$ASSET
fi
echo "$SHA256  $DL/$ASSET" | shasum -a 256 -c -

WORK=$(mktemp -d)
trap 'rm -rf -- "$WORK"' EXIT
tar -xzf $DL/$ASSET -C $WORK
P=$WORK/python
LIB=$P/lib/python$PY_MM

# ── Trim ────────────────────────────────────────────────────────────
# bin/python3.13 is statically linked; libpython is only for embedding.
rm -rf -- $P/share $P/lib/pkgconfig $P/lib/libpython$PY_MM.dylib \
          $P/lib/libtcl* $P/lib/tcl* $P/lib/tk* $P/lib/itcl* $P/lib/thread* \
          $LIB/idlelib $LIB/tkinter $LIB/turtledemo $LIB/turtle.py $LIB/pydoc_data \
          $LIB/lib-dynload/_tkinter.*.so \
          $P/bin/idle3* $P/bin/pydoc3* $P/bin/pip*

# ── Bundled packages (hash-pinned, wheels only) ─────────────────────
$P/bin/python3 -I -m pip install --disable-pip-version-check --no-cache-dir \
    --no-deps --require-hashes --only-binary :all: --no-compile \
    -r $ROOT/macos/python-requirements.txt
rm -rf -- $LIB/site-packages/pip $LIB/site-packages/pip-*.dist-info

# ── Precompile (never rewritten, so the bundle stays sealed) ────────
find $P -name __pycache__ -type d -prune -exec rm -rf {} +
$P/bin/python3 -I -m compileall -q -j 0 --invalidation-mode unchecked-hash $LIB

# Smoke test before replacing the previous runtime.
$P/bin/python3 -I -c 'import ssl, sqlite3, ctypes, venv, ensurepip, pypdf, openpyxl; print("python", __import__("sys").version.split()[0], "ok")'

if [[ -d $DEST ]]; then
    [[ -x $DEST/bin/python3 ]] || { echo "$DEST exists and is not a runtime from this script" >&2; exit 1; }
    rm -rf -- "$DEST"
fi
mkdir -p ${DEST:h}
mv $P $DEST
du -sh $DEST
