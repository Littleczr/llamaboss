#!/bin/zsh
# macos/sign_bundle.sh — sign LlamaBoss.app inside-out.
#
#   macos/sign_bundle.sh <identity> <path/to/LlamaBoss.app> [--release]
#
# Nested code (llama-server and its dylibs in Contents/Helpers) must be signed
# before the app itself, or the app's signature does not validate.  --release
# adds what notarization requires: the hardened runtime and a secure
# timestamp on every signature.  Dev builds (CMake post-build) omit it.
set -euo pipefail

ID=$1
APP=$2
MODE=${3:-}

opts=(--force --sign "$ID")
if [[ $MODE == --release ]]; then
    opts+=(--options runtime --timestamp)
else
    opts+=(--timestamp=none)
fi

for dir in "$APP/Contents/Helpers" "$APP/Contents/Frameworks"; do
    [[ -d $dir ]] || continue
    find "$dir" -type f -print0 | while IFS= read -r -d '' f; do
        if file -b "$f" | grep -q 'Mach-O'; then
            codesign "${opts[@]}" "$f" 2>&1 | grep -v 'replacing existing signature' || true
        fi
    done
done

codesign "${opts[@]}" "$APP" 2>&1 | grep -v 'replacing existing signature' || true
codesign --verify --strict --deep "$APP"
