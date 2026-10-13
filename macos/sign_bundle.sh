#!/bin/zsh
# macos/sign_bundle.sh — sign LlamaBoss.app inside-out.
#
#   macos/sign_bundle.sh <identity> <path/to/LlamaBoss.app> [--release]
#
# Nested code (llama-server and its dylibs in Contents/Helpers, the Python
# runtime in Contents/Resources/python-runtime) must be signed before the app itself, or the app's signature does
# not validate.  --release adds what notarization requires: the hardened
# runtime and a secure timestamp on every signature.  Dev builds (CMake
# post-build) omit it.
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

for dir in "$APP/Contents/Helpers" "$APP/Contents/Frameworks" \
           "$APP/Contents/Resources/python-runtime"; do
    [[ -d $dir ]] || continue
    find "$dir" -type f -print0 | while IFS= read -r -d '' f; do
        if file -b "$f" | grep -q 'Mach-O'; then
            extra=()
            # The bundled python3 loads pip-installed extensions signed by
            # other developers (macos/python.entitlements).
            [[ $f == "$APP/Contents/Resources/python-runtime/bin/"* ]] &&
                extra=(--entitlements "${0:A:h}/python.entitlements")
            codesign "${opts[@]}" "${extra[@]}" "$f" 2>&1 | grep -v 'replacing existing signature' || true
        fi
    done
done

codesign "${opts[@]}" "$APP" 2>&1 | grep -v 'replacing existing signature' || true
codesign --verify --strict --deep "$APP"
