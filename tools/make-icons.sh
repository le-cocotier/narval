#!/bin/sh
# Regenerates narval-gui's icons from assets/icons/narval.svg: narval.png
# (Linux launchers), narval.icns (Narval.app), narval.ico (narval-gui.exe)
# and narval.bmp (the window's own icon, SDL_LoadBMP). Needs rsvg-convert
# (librsvg), ImageMagick's convert and python3.
#
# Usage: tools/make-icons.sh [source-root]
set -eu
root="${1:-$(dirname "$0")/..}"
icons="$root/assets/icons"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

for s in 16 32 48 64 128 256 512 1024; do
    rsvg-convert -w $s -h $s "$icons/narval.svg" -o "$tmp/$s.png"
done
cp "$tmp/256.png" "$icons/narval.png"
convert "$tmp/16.png" "$tmp/32.png" "$tmp/48.png" "$tmp/64.png" "$tmp/128.png" "$tmp/256.png" "$icons/narval.ico"
# 256 px, so a Dock tile on a Retina screen stays sharp. 32-bit, with alpha.
convert "$tmp/256.png" "BMP:$icons/narval.bmp"
# .icns: PNG images under Apple's type codes for each size.
python3 - "$tmp" "$icons/narval.icns" <<'PY'
import struct, sys
src, out = sys.argv[1], sys.argv[2]
types = [('icp4', 16), ('icp5', 32), ('icp6', 64), ('ic07', 128), ('ic08', 256),
         ('ic09', 512), ('ic10', 1024), ('ic11', 32), ('ic12', 64), ('ic13', 256), ('ic14', 512)]
body = b''
for code, size in types:
    data = open(f'{src}/{size}.png', 'rb').read()
    body += code.encode() + struct.pack('>I', len(data) + 8) + data
open(out, 'wb').write(b'icns' + struct.pack('>I', len(body) + 8) + body)
PY
echo "icons written to $icons"
