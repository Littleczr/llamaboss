#!/bin/zsh
# macos/package.sh — build, sign, package and (optionally) notarize the
# public LlamaBoss for Mac download.
#
#   macos/package.sh [--identity "<codesign identity>"] [--notarize <profile>]
#
#   --identity   default: the first "Developer ID Application" identity in the
#                keychain.  Any other identity (e.g. "Apple Development") gives
#                a DMG for local testing only: Gatekeeper blocks it elsewhere.
#   --notarize   notarytool keychain profile, created once with
#                  xcrun notarytool store-credentials <profile> \
#                      --apple-id <apple id> --team-id <team id>
#                (asks for an app-specific password from account.apple.com).
#                Omit to skip notarization.
#
# Needs macos/build_deps.sh to have run (default ~/LlamaBossDeps/prefix), the
# llama.cpp macOS release unpacked in third_party/llama.cpp, and
# macos/fetch_python.sh (third_party/python).
# Output: dist/LlamaBoss-<version>-macOS.dmg
set -euo pipefail

ROOT=${0:A:h:h}
DEPS_PREFIX=${LLAMABOSS_DEPS_PREFIX:-$HOME/LlamaBossDeps/prefix}
BUILD=$ROOT/build_release
DIST=$ROOT/dist
IDENTITY=""
PROFILE=""

while (( $# )); do
    case $1 in
        --identity) IDENTITY=$2; shift 2 ;;
        --notarize) PROFILE=$2; shift 2 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done

if [[ -z $IDENTITY ]]; then
    IDENTITY=$(security find-identity -v -p codesigning |
               sed -n 's/.*"\(Developer ID Application: [^"]*\)".*/\1/p' | head -1)
    [[ -n $IDENTITY ]] || { echo "No Developer ID Application identity; pass --identity" >&2; exit 1; }
fi
echo "Signing identity: $IDENTITY"

# ── Build (static release dependencies, macOS 13.3+) ────────────────
cmake -S $ROOT -B $BUILD -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DLLAMABOSS_DEPS_PREFIX=$DEPS_PREFIX -DLLAMABOSS_CODESIGN_IDENTITY="$IDENTITY"
cmake --build $BUILD
BUILT=$BUILD/LlamaBoss.app
VERSION=$(/usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' $BUILT/Contents/Info.plist)

# ── Stage the app, keeping only the llama.cpp files llama-server loads ──
# The llama.cpp release ships every tool's libraries (bench, batched, ...).
# Copy the app without Helpers, then copy into Helpers/llama just the
# closure of llama-server's @rpath references (its rpath is @loader_path),
# plus the version symlinks among them.  The build output is not modified.
STAGE=$(mktemp -d)
APP=$STAGE/LlamaBoss.app
rsync -a --exclude 'Contents/Helpers/' $BUILT/ $APP/
SRCH=$BUILT/Contents/Helpers/llama
H=$APP/Contents/Helpers/llama
mkdir -p $H
typeset -A need
queue=(llama-server)
while (( ${#queue} )); do
    cur=${queue[1]}; queue=(${queue[2,-1]})
    [[ -n ${need[$cur]:-} ]] && continue
    need[$cur]=1
    if [[ -L $SRCH/$cur ]]; then
        queue+=($(readlink $SRCH/$cur))
        continue
    fi
    for dep in $(otool -L $SRCH/$cur | tail -n +2 | awk '{print $1}' | sed -n 's|^@rpath/||p'); do
        queue+=($dep)
    done
done
for name in ${(k)need}; do cp -a $SRCH/$name $H/; done
echo "llama.cpp files kept: ${(ok)need}"

# ── Python runtime (macos/fetch_python.sh; staged with Resources) ────
[[ -x $APP/Contents/Resources/python-runtime/bin/python3 ]] ||
    { echo "Python runtime missing from the build: run macos/fetch_python.sh" >&2; exit 1; }

# ── Self-containment check: only system libraries may be referenced ──
bad=0
for f in $APP/Contents/MacOS/LlamaBoss $APP/Contents/Helpers/llama/*(.) $APP/Contents/Resources/python-runtime/**/*(.); do
    file -b $f | grep -q Mach-O || continue
    while read -r lib; do
        case $lib in
            /System/*|/usr/lib/*|@rpath/*|@loader_path/*|@executable_path/*) ;;
            *) echo "NOT SELF-CONTAINED: ${f:t} -> $lib" >&2; bad=1 ;;
        esac
    done < <(otool -L $f | tail -n +2 | awk '{print $1}')
done
(( bad == 0 )) || exit 1
echo "Minimum macOS: $(otool -l $APP/Contents/MacOS/LlamaBoss | awk '/LC_BUILD_VERSION/{f=1} f&&/minos/{print $2; exit}')"

# ── Release signature (hardened runtime + timestamp) ────────────────
$ROOT/macos/sign_bundle.sh "$IDENTITY" $APP --release

# ── DMG: the app plus an Applications shortcut ──────────────────────
mkdir -p $DIST
ln -s /Applications $STAGE/Applications
DMG=$DIST/LlamaBoss-$VERSION-macOS.dmg
rm -f $DMG
hdiutil create -volname "LlamaBoss $VERSION" -srcfolder $STAGE -ov -format UDZO $DMG
[[ $STAGE == ${TMPDIR:-/tmp}* && -d $STAGE/LlamaBoss.app ]] && rm -rf -- "$STAGE"   # our mktemp dir only
codesign --force --sign "$IDENTITY" --timestamp $DMG

# ── Notarize + staple ───────────────────────────────────────────────
if [[ -n $PROFILE ]]; then
    xcrun notarytool submit $DMG --keychain-profile "$PROFILE" --wait
    xcrun stapler staple $DMG
    spctl --assess --type open --context context:primary-signature -v $DMG
fi

shasum -a 256 $DMG
echo "Done: $DMG"
