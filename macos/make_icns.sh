#!/bin/sh
# Rebuild macos/app_icon.icns from the Windows app_icon.ico (PNG frames up to
# 256x256). The 512 and 1024 slots are upscaled from 256 until a larger
# source image exists. Run from the repo root: sh macos/make_icns.sh
set -e
tmp=$(mktemp -d)
python3 - "$tmp" <<'PY'
import struct, sys
out = sys.argv[1]
d = open('app_icon.ico', 'rb').read()
n = struct.unpack('<HHH', d[:6])[2]
for i in range(n):
    w, h, c, r, pl, bpp, size, off = struct.unpack('<BBBBHHII', d[6+16*i:22+16*i])
    data = d[off:off+size]
    if data[:4] == b'\x89PNG':
        open('%s/%d.png' % (out, w or 256), 'wb').write(data)
PY
set_dir="$tmp/app_icon.iconset"
mkdir "$set_dir"
put() { sips -z "$2" "$2" "$tmp/$1.png" --out "$set_dir/$3" >/dev/null; }
put 16 16 icon_16x16.png
put 32 32 icon_16x16@2x.png
put 32 32 icon_32x32.png
put 64 64 icon_32x32@2x.png
put 128 128 icon_128x128.png
put 256 256 icon_128x128@2x.png
put 256 256 icon_256x256.png
put 256 512 icon_256x256@2x.png
put 256 512 icon_512x512.png
put 256 1024 icon_512x512@2x.png
iconutil -c icns "$set_dir" -o macos/app_icon.icns
rm -rf "$tmp"
echo "wrote macos/app_icon.icns"
