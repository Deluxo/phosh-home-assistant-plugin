#!/bin/sh
# Build an arm64 .deb of the phosh Home Assistant quick-setting plugin.
#
# Runs natively on any Debian trixie (arm64) system:
#   - in CI: a qemu-emulated container on the GitHub x86 runner (see
#     .github/workflows/build-deb.yml)
#   - locally: any ARM machine with trixie, e.g. for self-hosted runners
#
# The result is a real ARM64 .deb (in ./dist/) installable via apt on
# Phosphan/postmarketOS phones. Debian trixie ships phosh 0.46 /
# libphosh-0.45, the same SONAME the phone runs, so no postmarketOS repo
# is needed.
#
# POSIX sh (the debian:trixie image has no bash).
set -eux

SRC="$(cd "$(dirname "$0")/.." && pwd)"
DIST="$SRC/dist"
PKG="phosh-plugin-home-assistant-quick-setting"
SHA="${PKG_GHA_SHA:-}"

export DEBIAN_FRONTEND=noninteractive

# --- build deps (Debian trixie) -------------------------------------------
# Under qemu-user emulation on CI runners, syscalls are slow and apt can hit
# transient mirror timeouts; retry a few times before giving up.
apt_deps() {
  apt-get update && \
  apt-get install -y --no-install-recommends \
    build-essential dpkg-dev file \
    meson ninja-build pkg-config \
    phosh-dev libphosh-0.45-dev \
    libsoup-3.0-dev libadwaita-1-dev libgtk-4-dev libgtk-3-dev \
    libjson-glib-dev \
    libglib2.0-bin   # glib-compile-schemas, used by the meson schema test
}
attempt=1
until apt_deps; do
  if [ "$attempt" -ge 3 ]; then
    echo "ERROR: apt-get failed after $attempt attempts" >&2
    exit 1
  fi
  echo "apt attempt $attempt failed, retrying in 30s..." >&2
  attempt=$((attempt + 1))
  sleep 30
done

cd "$SRC"

# --- version: exact git tag if on one, else meson version + commit --------
VERSION_BASE="$(sed -n "s/^  version: '\([^']*\)'.*/\1/p" meson.build)"
if [ -z "${VERSION_BASE:-}" ]; then
  echo "ERROR: no version in meson.build" >&2
  exit 1
fi

UPSTREAM="$VERSION_BASE"
TAG="$(git describe --tags --exact-match 2>/dev/null || true)"
if [ -n "$TAG" ]; then
  UPSTREAM="${TAG#v}"
elif [ -n "$SHA" ]; then
  # ${SHA:0:7} is a bashism; this script runs under dash (debian /bin/sh).
  SHORT_SHA="$(printf '%s' "$SHA" | cut -c1-7)"
  UPSTREAM="${VERSION_BASE}~g$SHORT_SHA"
fi
VERSION="${UPSTREAM}-1"

# --- build + test ----------------------------------------------------------
meson setup build -Dprefix=/usr

# Under qemu-user emulation the emulated gcc can sporadically segfault on a
# translation unit (seen on GitHub runners; native ARM is unaffected).
# Ninja resumes from the last good .o, so retrying makes progress.
attempt=1
until meson compile -C build; do
  if [ "$attempt" -ge 5 ]; then
    echo "ERROR: meson compile failed after $attempt attempts" >&2
    exit 1
  fi
  echo "compile attempt $attempt failed, retrying..." >&2
  attempt=$((attempt + 1))
done

meson test -C build   # validates the gsettings schema

# --- stage install (Debian multiarch layout, libdir picked by meson) -------
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
DESTDIR="$STAGE" meson install -C build

# --- assemble the .deb -----------------------------------------------------
PKGDIR="$STAGE/$PKG"
mkdir -p "$PKGDIR/DEBIAN"
mv "$STAGE/usr" "$PKGDIR/usr"

# Note on Depends: intentionally minimal. The runtime phone (Phosphan /
# postmarketOS) already provides everything else this plugin links against
# (gtk3/4, libadwaita, libsoup, json-glib, glib) as phosh's own dependencies,
# and their package names differ from Debian trixie's t64-renamed ones
# (e.g. libgtk-3-0t64), which would break `apt install` on the device.
cat > "$PKGDIR/DEBIAN/control" <<EOF
Package: $PKG
Version: $VERSION
Architecture: arm64
Maintainer: Deluxo <48fk01e49@mozmail.com>
Section: utils
Priority: optional
Depends: libphosh-0.45-0
Description: Home Assistant quick-setting plugin for Phosh
 Quick-setting tile, status page and prefs dialog to control Home
 Assistant entities (lights, switches, covers, locks, fans) from a
 Phosphan phone. Installs the plugin .so files, icon and gsettings
 schema; compiles the schema on install/remove.
EOF

install -m 0755 packaging/postinst "$PKGDIR/DEBIAN/postinst"
install -m 0755 packaging/postrm "$PKGDIR/DEBIAN/postrm"

mkdir -p "$DIST"
DEB="$DIST/${PKG}_${VERSION}_arm64.deb"
dpkg-deb --build --root-owner-group "$PKGDIR" "$DEB"

# --- sanity: the payload must be ARM aarch64 -------------------------------
VERIFY="$STAGE/verify"
mkdir -p "$VERIFY"
dpkg-deb -x "$DEB" "$VERIFY"
NON_ARM="$(find "$VERIFY" -name '*.so' -exec file {} \; | grep -v 'ARM aarch64' || true)"
if [ -n "$NON_ARM" ]; then
  echo "ERROR: non-arm64 ELF in payload:" >&2
  echo "$NON_ARM" >&2
  exit 1
fi

dpkg-deb --info "$DEB"
echo "built: $DEB"
