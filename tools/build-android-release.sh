#!/usr/bin/env bash
# tools/build-android-release.sh
#
# Builds the signed FoxSDR Android alpha release APK: stages the bundled
# decoder plugins, runs `assembleRelease`, copies the APK to dist/, and
# verifies the symbol-archiving POST_BUILD step (src/platform/android/
# CMakeLists.txt) actually captured symbols for the binary that shipped -
# not a stripped copy of it - for BOTH ABIs. Every check here failed silently
# once before elsewhere in this codebase (see the Lessons-learned entries on
# stale .obj files and pinned-at-ingest crash groups); this script exists so
# a release is proven, not assumed.
#
# Usage:
#   tools/build-android-release.sh [--plugins <dir>]
#
#   --plugins <dir>   Passed through to Gradle as -PfoxsdrPluginsDir. Defaults
#                      to /root/fox-plugins-and (this machine's sibling
#                      plugins checkout). Without a real plugins tree the
#                      release APK ships zero decoders - this script treats
#                      that as an error rather than silently shipping an
#                      empty plugin store.
#
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

PLUGINS_DIR="/root/fox-plugins-and"
while [ $# -gt 0 ]; do
    case "$1" in
        --plugins)
            PLUGINS_DIR="$2"
            shift 2
            ;;
        *)
            echo "build-android-release: unknown argument '$1'" >&2
            exit 1
            ;;
    esac
done

echo "== foxsdr android release build =="
echo "repo:    $REPO_ROOT"
echo "plugins: $PLUGINS_DIR"

if [ ! -d "$PLUGINS_DIR" ]; then
    echo "build-android-release: plugins dir '$PLUGINS_DIR' does not exist - the release apk would ship zero decoders" >&2
    exit 2
fi

if [ -f "$HOME/android-env.sh" ]; then
    # shellcheck disable=SC1091
    source "$HOME/android-env.sh"
fi

cd "$REPO_ROOT/android"

echo
echo "-- assembleRelease --"
./gradlew --no-daemon "-PfoxsdrPluginsDir=${PLUGINS_DIR}" assembleRelease

VERSION_NAME="$(./gradlew --no-daemon -q printVersionName | tail -1 | tr -d '\r')"
if [ -z "$VERSION_NAME" ]; then
    echo "build-android-release: could not read versionName from Gradle" >&2
    exit 3
fi
echo "versionName: $VERSION_NAME"

APK_DIR="$REPO_ROOT/android/app/build/outputs/apk/release"
APK="$(find "$APK_DIR" -maxdepth 1 -name '*.apk' | head -1)"
if [ -z "$APK" ] || [ ! -f "$APK" ]; then
    echo "build-android-release: no release apk found under $APK_DIR" >&2
    exit 4
fi

DIST_DIR="$REPO_ROOT/dist"
mkdir -p "$DIST_DIR"
DIST_APK="$DIST_DIR/FoxSDR-${VERSION_NAME}.apk"
cp -f "$APK" "$DIST_APK"

echo
echo "-- release apk --"
echo "path: $DIST_APK"
APK_SIZE="$(stat -c '%s' "$DIST_APK")"
APK_SHA256="$(sha256sum "$DIST_APK" | awk '{print $1}')"
echo "size:   $APK_SIZE bytes"
echo "sha256: $APK_SHA256"

# ---------------------------------------------------------------------------
# Per-ABI: the packaged .so's GNU build id, and whether the symbol archive
# has a matching entry for it.
# ---------------------------------------------------------------------------
#
# The POST_BUILD step in src/platform/android/CMakeLists.txt archives
# libfoxsdr.so THE MOMENT IT IS LINKED, before AGP copies and (for a release
# build) strips a copy of it into the apk's lib/<abi>/ directory. If AGP's
# strip ever changes the build id (some strip flags rewrite ELF sections and
# can drop or regenerate the build-id note), the archived symbols would
# silently stop matching what actually shipped - this is the check that
# would catch it, run against the REAL packaged binary rather than assumed
# from the build log.
WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

SYMBOL_ROOT="$REPO_ROOT/symbols"
ALL_MATCH=1

for ABI in arm64-v8a x86_64; do
    echo
    echo "-- ABI: $ABI --"
    SO_PATH="$WORKDIR/libfoxsdr-$ABI.so"
    if ! unzip -p "$DIST_APK" "lib/$ABI/libfoxsdr.so" > "$SO_PATH" 2>/dev/null; then
        echo "build-android-release: lib/$ABI/libfoxsdr.so not found in apk" >&2
        ALL_MATCH=0
        continue
    fi
    PACKAGED_SIZE="$(stat -c '%s' "$SO_PATH")"
    echo "packaged .so size: $PACKAGED_SIZE bytes"

    BUILD_ID="$(readelf -n "$SO_PATH" 2>/dev/null | grep -A1 'Build ID' | grep -oE '[0-9a-f]{20,64}' | head -1)"
    if [ -z "$BUILD_ID" ]; then
        echo "build-android-release: could not read a GNU build-id note from lib/$ABI/libfoxsdr.so" >&2
        ALL_MATCH=0
        continue
    fi
    echo "packaged build-id: $BUILD_ID"

    ARCHIVE_BIN="$SYMBOL_ROOT/foxsdr/$BUILD_ID/foxsdr"
    ARCHIVE_DEBUG="$SYMBOL_ROOT/foxsdr.debug/$BUILD_ID/foxsdr.debug"

    if [ -f "$ARCHIVE_BIN" ]; then
        ARCHIVE_BUILD_ID="$(readelf -n "$ARCHIVE_BIN" 2>/dev/null | grep -A1 'Build ID' | grep -oE '[0-9a-f]{20,64}' | head -1)"
        echo "archived binary:   $ARCHIVE_BIN"
        echo "archived build-id: $ARCHIVE_BUILD_ID"
        if [ "$ARCHIVE_BUILD_ID" != "$BUILD_ID" ]; then
            echo "build-android-release: ABI $ABI build-id MISMATCH: packaged=$BUILD_ID archived=$ARCHIVE_BUILD_ID" >&2
            ALL_MATCH=0
        fi
    else
        echo "build-android-release: no archived binary at $ARCHIVE_BIN - the symbol-archive POST_BUILD step did not run (or archived a different build) for ABI $ABI" >&2
        ALL_MATCH=0
    fi

    if [ -f "$ARCHIVE_DEBUG" ]; then
        echo "archived debug:    $ARCHIVE_DEBUG"
    else
        echo "build-android-release: no split-debug archive at $ARCHIVE_DEBUG for ABI $ABI" >&2
        ALL_MATCH=0
    fi
done

echo
if [ "$ALL_MATCH" -eq 1 ]; then
    echo "== symbol archive verified for both ABIs =="
else
    echo "== symbol archive verification FAILED - see above =="
fi

echo
echo "== done =="
echo "apk:    $DIST_APK"
echo "sha256: $APK_SHA256"
echo "size:   $APK_SIZE bytes"

# Exit explicitly rather than letting the script's exit status fall out of
# the last command: the EXIT trap above (rm -rf "$WORKDIR") runs after this
# and, on at least one shell tested here, left the process exit code at 0
# even when this test was false - a script whose failure is silently
# swallowed is worse than one that never checked.
if [ "$ALL_MATCH" -eq 1 ]; then
    exit 0
else
    exit 1
fi
