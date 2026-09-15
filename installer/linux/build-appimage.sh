#!/usr/bin/env bash
# build-appimage.sh - package a Release build of FoxSDR as a single-file
# AppImage.
#
# THE PAYLOAD MATCHES THE PLAIN TARBALL, deliberately: the same files
# .github/workflows/build.yml's "Package" step already stages (the cascade
# binary and the resources/bandplans it reads at runtime) plus the licensing
# text a distributable build must carry (LICENSE, THIRD-PARTY-LICENSES.txt,
# POSTINSTALL.txt). This is packaging, not a new build system - it does not
# link anything, does not bundle SoapySDR/OpenSSL/ALSA/OpenGL, and does not
# change what the binary needs on the host. See installer/linux/AppRun for why.
#
# WHY APPIMAGE_EXTRACT_AND_RUN=1. appimagetool is itself shipped as an
# AppImage, and mounting one needs FUSE - which WSL does not have. Extracting
# it to a temp directory and running the extracted binary needs no mount at
# all, and is exactly what that environment variable tells appimagetool's own
# runtime to do for itself before it does anything else. The AppImage this
# script PRODUCES is a normal type-2 AppImage; APPIMAGE_EXTRACT_AND_RUN is
# only needed to run appimagetool, and to run this script's OWN output later
# on a FUSE-less machine (see the "how to test" note at the bottom).
#
# USAGE
#   installer/linux/build-appimage.sh [BUILD_DIR]
# BUILD_DIR defaults to "build" (relative to the repo root, or an absolute
# path) and must already contain a built `cascade` (cmake --build build).
#
# OUTPUT
#   dist/FoxSDR-<version>-x86_64.AppImage
#
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${1:-$REPO_ROOT/build}"
case "$BUILD_DIR" in
    /*) : ;;
    *) BUILD_DIR="$REPO_ROOT/$BUILD_DIR" ;;
esac

CASCADE_BIN="$BUILD_DIR/cascade"
if [ ! -x "$CASCADE_BIN" ]; then
    echo "build-appimage.sh: no built 'cascade' at $CASCADE_BIN - run cmake --build first" >&2
    exit 1
fi

VERSION="$(sed -n 's/^project(cascade VERSION \([0-9.]*\).*/\1/p' "$REPO_ROOT/CMakeLists.txt")"
if [ -z "$VERSION" ]; then
    echo "build-appimage.sh: could not read the version out of CMakeLists.txt" >&2
    exit 1
fi

DIST_DIR="$REPO_ROOT/dist"
APPDIR="$DIST_DIR/AppDir"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin/resources/bandplans" \
         "$APPDIR/usr/share/licenses/foxsdr" \
         "$APPDIR/usr/share/doc/foxsdr"

# --- the payload: the same files the CI tarball stages, plus attribution ---
cp "$CASCADE_BIN" "$APPDIR/usr/bin/cascade"
cp "$REPO_ROOT"/resources/bandplans/*.json "$APPDIR/usr/bin/resources/bandplans/"
cp "$REPO_ROOT/LICENSE" "$APPDIR/usr/share/licenses/foxsdr/LICENSE"
cp "$REPO_ROOT/installer/THIRD-PARTY-LICENSES.txt" \
   "$APPDIR/usr/share/licenses/foxsdr/THIRD-PARTY-LICENSES.txt"
cp "$REPO_ROOT/installer/POSTINSTALL.txt" "$APPDIR/usr/share/doc/foxsdr/POSTINSTALL.txt"

# --- the AppImage shell: desktop entry, icon and AppRun, all at AppDir root ---
install -m 0644 "$SCRIPT_DIR/foxsdr.desktop" "$APPDIR/foxsdr.desktop"
install -m 0644 "$REPO_ROOT/resources/icon/foxsdr-256.png" "$APPDIR/foxsdr.png"
install -m 0755 "$SCRIPT_DIR/AppRun" "$APPDIR/AppRun"

# --- appimagetool, pinned by version and verified by sha256 before it is
# trusted to run at all - this is code that packages a release, so a
# tampered-with or mismatched download must be refused rather than executed.
APPIMAGETOOL_VERSION="1.9.1"
APPIMAGETOOL_URL="https://github.com/AppImage/appimagetool/releases/download/${APPIMAGETOOL_VERSION}/appimagetool-x86_64.AppImage"
APPIMAGETOOL_SHA256="ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0"
APPIMAGETOOL_CACHE="$DIST_DIR/appimagetool-x86_64.AppImage"

if [ ! -x "$APPIMAGETOOL_CACHE" ] || ! echo "$APPIMAGETOOL_SHA256  $APPIMAGETOOL_CACHE" | sha256sum -c - >/dev/null 2>&1; then
    echo "build-appimage.sh: fetching appimagetool $APPIMAGETOOL_VERSION"
    curl -fL -o "$APPIMAGETOOL_CACHE.tmp" "$APPIMAGETOOL_URL"
    if ! echo "$APPIMAGETOOL_SHA256  $APPIMAGETOOL_CACHE.tmp" | sha256sum -c -; then
        echo "build-appimage.sh: appimagetool download did not match the pinned sha256 - refusing to use it" >&2
        rm -f "$APPIMAGETOOL_CACHE.tmp"
        exit 1
    fi
    mv "$APPIMAGETOOL_CACHE.tmp" "$APPIMAGETOOL_CACHE"
    chmod +x "$APPIMAGETOOL_CACHE"
fi

OUTPUT="$DIST_DIR/FoxSDR-${VERSION}-x86_64.AppImage"
rm -f "$OUTPUT"

# FUSE is not available in WSL (or many CI containers), so appimagetool is
# told to extract-and-run itself rather than mount its own AppImage.
APPIMAGE_EXTRACT_AND_RUN=1 ARCH=x86_64 "$APPIMAGETOOL_CACHE" "$APPDIR" "$OUTPUT"

echo "build-appimage.sh: wrote $OUTPUT"
echo "build-appimage.sh: to run the result on a machine with no FUSE:"
echo "  APPIMAGE_EXTRACT_AND_RUN=1 $OUTPUT --version"
