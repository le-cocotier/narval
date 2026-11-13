#!/bin/bash
#
# Builds narval_<version>_<arch>.deb and narval-<version>-1.<arch>.rpm from
# this checkout, into dist/ (or the folder given as the second argument).
#
#   packaging/linux/build-packages.sh 4.1.0 [outdir] [amd64|arm64]
#
# The architecture defaults to this machine's. The other one is
# cross-compiled, which needs its gcc: gcc-x86-64-linux-gnu on an ARM
# machine, gcc-aarch64-linux-gnu on an x86_64 one.
#
# Both install /usr/bin/narval and /usr/share/narval (default config, the
# bundled packages, the splash screen); narval sets up ~/.narval from those
# on its first start. Needs meson, ninja, a C compiler, dpkg-deb and
# rpmbuild (apt-get install meson ninja-build gcc rpm). Run by
# .gitea/workflows/release-windows.yml on every v* tag, once per architecture.

set -euo pipefail

VERSION="${1:?usage: $0 VERSION [OUTDIR]}"
VERSION="${VERSION#v}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$(mkdir -p "${2:-$ROOT/dist}" && cd "${2:-$ROOT/dist}" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

HOST_DEB="$(dpkg --print-architecture 2>/dev/null || echo amd64)"
ARCH_DEB="${3:-$HOST_DEB}"
case "$ARCH_DEB" in
    amd64) ARCH_RPM=x86_64;  TRIPLET=x86_64-linux-gnu ;;
    arm64) ARCH_RPM=aarch64; TRIPLET=aarch64-linux-gnu ;;
    *) echo "Unsupported architecture: $ARCH_DEB (amd64 or arm64)" >&2; exit 2 ;;
esac
SUMMARY="Terminal text editor configured in Lisp, with LSP and a debugger"
HOMEPAGE="https://git.giorgio-nas.fr/arthur/narval"
MAINTAINER="Arthur Barraux <arthurbarraux@icloud.com>"
LICENSE="MIT"

# --- build and stage -------------------------------------------------------
CROSS=()
STRIP=strip
if [ "$ARCH_DEB" != "$HOST_DEB" ]; then
    STRIP="$TRIPLET-strip"
    cat > "$WORK/cross.ini" <<INI
[binaries]
c = '$TRIPLET-gcc'
strip = '$TRIPLET-strip'

[host_machine]
system = 'linux'
cpu_family = '$ARCH_RPM'
cpu = '$ARCH_RPM'
endian = 'little'
INI
    CROSS=(--cross-file "$WORK/cross.ini")
fi
meson setup "$WORK/build" "$ROOT" "${CROSS[@]}" --prefix=/usr --buildtype=release >/dev/null
meson compile -C "$WORK/build"
DESTDIR="$WORK/root" meson install -C "$WORK/build" --no-rebuild >/dev/null
"$STRIP" "$WORK/root/usr/bin/narval"
mkdir -p "$WORK/root/usr/share/doc/narval"
cp "$ROOT/README.md" "$WORK/root/usr/share/doc/narval/"
[ -f "$ROOT/LICENSE" ] && cp "$ROOT/LICENSE" "$WORK/root/usr/share/doc/narval/copyright"

# --- .deb ------------------------------------------------------------------
DEB="$WORK/deb"
cp -a "$WORK/root" "$DEB"
mkdir -p "$DEB/DEBIAN"
# The C library and libutil are all it links; dpkg-shlibdeps would say the
# same, but needs a debian/ tree to run.
cat > "$DEB/DEBIAN/control" <<CONTROL
Package: narval
Version: $VERSION
Architecture: $ARCH_DEB
Maintainer: $MAINTAINER
Installed-Size: $(du -sk "$WORK/root" | cut -f1)
Depends: libc6
Replaces: beluga
Conflicts: beluga
Recommends: git
Suggests: gdb, clangd
Section: editors
Priority: optional
Homepage: $HOMEPAGE
Description: $SUMMARY
 Narval is a terminal text editor for Linux, macOS and Windows, written in
 C and configured in Lisp. It has language servers (LSP), a debugger (DAP),
 a git client, a package manager, terminal panes and a daemon mode.
CONTROL
# xz, not the zstd newer dpkg defaults to: Debian 11 and older read only xz.
dpkg-deb -Zxz --root-owner-group --build "$DEB" "$OUT/narval_${VERSION}_${ARCH_DEB}.deb"

# --- .rpm ------------------------------------------------------------------
RPMTOP="$WORK/rpm"
mkdir -p "$RPMTOP"/{SPECS,BUILD,RPMS,SRPMS,SOURCES}
cat > "$RPMTOP/SPECS/narval.spec" <<SPEC
Name:           narval
Version:        $VERSION
Release:        1
Summary:        $SUMMARY
License:        $LICENSE
URL:            $HOMEPAGE
Obsoletes:      beluga
Recommends:     git
%global debug_package %{nil}
%global _build_id_links none
%global __os_install_post %{nil}

%description
Narval is a terminal text editor for Linux, macOS and Windows, written in
C and configured in Lisp. It has language servers (LSP), a debugger (DAP),
a git client, a package manager, terminal panes and a daemon mode.

%install
cp -a "$WORK/root/." "%{buildroot}/"

%files
/usr/bin/narval
/usr/share/narval
%doc /usr/share/doc/narval
SPEC
rpmbuild --quiet -bb --target "$ARCH_RPM" --define "_topdir $RPMTOP" "$RPMTOP/SPECS/narval.spec"
cp "$RPMTOP"/RPMS/"$ARCH_RPM"/narval-*.rpm "$OUT/"

ls -l "$OUT"
