#!/bin/sh
# Builds Narval.app around a narval-gui binary, for /Applications (where
# Spotlight, Launchpad and the Dock find it) or anywhere else.
#
# Usage: packaging/macos/make-app.sh BUILD/narval-gui DEST/Narval.app [VERSION]
#
# The app holds the binary, the icon and Info.plist. It uses the SDL3 and
# SDL3_ttf libraries the binary was linked with (Homebrew's), and ~/.narval
# for its config, packages and fonts, like the terminal narval.
set -eu

bin="$1"
app="$2"
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
version="${3:-$("$bin" --version 2>/dev/null | sed 's/^Narval //' || echo 0)}"

[ -x "$bin" ] || { echo "make-app.sh: $bin isn't an executable" >&2; exit 1; }

# Built next to the destination, then swapped in: a running Narval keeps
# working until it quits.
tmp="$app.new"
rm -rf "$tmp"
mkdir -p "$tmp/Contents/MacOS" "$tmp/Contents/Resources"
cp "$bin" "$tmp/Contents/MacOS/narval-gui"
cp "$root/assets/icons/narval.icns" "$tmp/Contents/Resources/narval.icns"
sed "s/@VERSION@/$version/g" "$here/Info.plist" > "$tmp/Contents/Info.plist"
printf 'APPL????' > "$tmp/Contents/PkgInfo"
# Signed for this machine only (ad hoc): Apple Silicon runs nothing
# unsigned, and copying the binary dropped the linker's signature.
if command -v codesign >/dev/null 2>&1; then
    codesign --force --deep --sign - "$tmp" >/dev/null 2>&1 || true
fi
rm -rf "$app"
mv "$tmp" "$app"
# Tell Launch Services (Spotlight, Open With) about it now rather than on
# its next scan.
lsregister=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
[ -x "$lsregister" ] && "$lsregister" -f "$app" >/dev/null 2>&1 || true
echo "$app"
