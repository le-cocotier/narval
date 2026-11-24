#!/bin/bash
#
# Cross-compiles narval.exe and narval-gui.exe on Linux and assembles the
# Windows release zip,
#   <outdir>/narval-<tag>-windows-x64.zip
# with narval.exe, narval-gui.exe and the SDL3 DLLs it needs, install.ps1,
# LICENSE, README.txt, the default config, the bundled packages and the
# assets. Windows users unzip it and run install.ps1, which installs
# narval-gui.exe and its DLLs next to narval.exe and adds it to the Start
# menu.
#
#   packaging/windows/build-zip.sh v3.5 [outdir]
#
# narval-gui.exe is built against SDL's own MinGW releases (SDL3 and
# SDL3_ttf, whose DLL carries FreeType), downloaded from GitHub once into
# build-windows-sdl3/. Set SDL3_MINGW_DIR to a folder that already holds
# the extracted SDL3-<v>/ and SDL3_ttf-<v>/ trees to skip the download, or
# NO_GUI=1 to leave narval-gui.exe out.
#
# Needs meson, ninja, mingw-w64, pkg-config, curl and zip. Used by the
# release workflows in .gitea/workflows and .github/workflows.

set -euo pipefail

TAG="${1:?usage: $0 TAG [OUTDIR]}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$(mkdir -p "${2:-$ROOT/dist}" && cd "${2:-$ROOT/dist}" && pwd)"
PKG="narval-$TAG-windows-x64"
BUILD="$ROOT/build-windows-release"
NO_GUI="${NO_GUI:-0}"
SDL3_VERSION="${SDL3_VERSION:-3.4.2}"
SDL3_TTF_VERSION="${SDL3_TTF_VERSION:-3.2.2}"
SDL3_MINGW_DIR="${SDL3_MINGW_DIR:-$ROOT/build-windows-sdl3}"
TRIPLET=x86_64-w64-mingw32

cd "$ROOT"
MESON_ARGS=(--cross-file cross/mingw-w64.ini --buildtype=release)
TARGETS=(narval)
if [ "$NO_GUI" = 1 ]; then
    MESON_ARGS+=(-Dgui=disabled)
else
    # SDL's MinGW development releases: headers, import libraries, DLLs.
    fetch_sdl() { # repo name version
        local tree="$SDL3_MINGW_DIR/$2-$3"
        [ -d "$tree/$TRIPLET" ] && return 0
        mkdir -p "$SDL3_MINGW_DIR"
        curl -fsSL "https://github.com/libsdl-org/$1/releases/download/release-$3/$2-devel-$3-mingw.tar.gz" |
            tar -xz -C "$SDL3_MINGW_DIR"
        [ -d "$tree/$TRIPLET" ] || { echo "No $tree/$TRIPLET in $2's MinGW release" >&2; exit 1; }
    }
    fetch_sdl SDL SDL3 "$SDL3_VERSION"
    fetch_sdl SDL_ttf SDL3_ttf "$SDL3_TTF_VERSION"
    SDL3_TREE="$SDL3_MINGW_DIR/SDL3-$SDL3_VERSION/$TRIPLET"
    TTF_TREE="$SDL3_MINGW_DIR/SDL3_ttf-$SDL3_TTF_VERSION/$TRIPLET"
    # pkg-config for the Windows side: only SDL's .pc files, with their
    # prefix taken from where they are (--define-prefix), whatever path
    # they were built with.
    mkdir -p "$BUILD-sdl3"
    printf '#!/bin/sh\nexec pkg-config --define-prefix "$@"\n' > "$BUILD-sdl3/pkg-config"
    chmod +x "$BUILD-sdl3/pkg-config"
    cat > "$BUILD-sdl3/sdl3.ini" <<INI
[binaries]
pkg-config = '$BUILD-sdl3/pkg-config'

[properties]
pkg_config_libdir = ['$SDL3_TREE/lib/pkgconfig', '$TTF_TREE/lib/pkgconfig']
INI
    MESON_ARGS+=(--cross-file "$BUILD-sdl3/sdl3.ini" -Dgui=enabled)
    TARGETS+=(narval-gui:executable)
fi
meson setup "${MESON_ARGS[@]}" --reconfigure "$BUILD" >/dev/null 2>&1 ||
  { rm -rf "$BUILD"; meson setup "${MESON_ARGS[@]}" "$BUILD" >/dev/null; }
meson compile -C "$BUILD" "${TARGETS[@]}"
# Debug info is useless to users and doubles the size.
x86_64-w64-mingw32-strip "$BUILD/narval.exe"

rm -rf "${OUT:?}/$PKG" "$OUT/$PKG.zip"
mkdir -p "$OUT/$PKG"
cp "$BUILD/narval.exe" install.ps1 LICENSE "$OUT/$PKG/"
if [ "$NO_GUI" != 1 ]; then
    x86_64-w64-mingw32-strip "$BUILD/narval-gui.exe"
    cp "$BUILD/narval-gui.exe" "$SDL3_TREE/bin/"*.dll "$TTF_TREE/bin/"*.dll "$OUT/$PKG/"
    # zlib licenses: the notices travel with the DLLs.
    [ -f "$SDL3_TREE/../LICENSE.txt" ] && cp "$SDL3_TREE/../LICENSE.txt" "$OUT/$PKG/LICENSE-SDL3.txt"
    [ -f "$TTF_TREE/../LICENSE.txt" ] && cp "$TTF_TREE/../LICENSE.txt" "$OUT/$PKG/LICENSE-SDL3_ttf.txt"
    # Every DLL narval-gui.exe loads is either Windows' own or in the zip.
    for dll in $(x86_64-w64-mingw32-objdump -p "$OUT/$PKG/narval-gui.exe" "$OUT/$PKG/"*.dll |
                 sed -n 's/.*DLL Name: //p' | sort -u); do
        if [ ! -f "$OUT/$PKG/$dll" ] && [ ! -f "/usr/$TRIPLET/lib/$(echo "lib${dll%.dll}.a" | tr 'A-Z' 'a-z')" ]; then
            echo "narval-gui.exe needs $dll, which isn't in the zip" >&2
            exit 1
        fi
    done
fi
cp windows/README.txt "$OUT/$PKG/README.txt"
cp -r assets config packages "$OUT/$PKG/"
# The editor writes this next to wherever it runs; never ship one.
find "$OUT/$PKG" -name lisp_log.err -delete
(cd "$OUT" && zip -qr9 "$PKG.zip" "$PKG")
rm -rf "${OUT:?}/$PKG"
ls -l "$OUT/$PKG.zip"
