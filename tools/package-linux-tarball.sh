#!/usr/bin/env bash
# Makes the Linux release tarball from a finished build: the body of the "Package" step of .github/workflows/build.yml, kept in one script so the workflow and a local run make the same archive.
#
#   tools/package-linux-tarball.sh BUILD_DIR VERSION ARCH OUT_DIR [--slint-sdk DIR]
#
#   BUILD_DIR   the CMake build directory (it holds cascade and foxsdr-engine, and src/slint/foxsdr-slint when the new window was built)
#   VERSION     the version string (the project() line of CMakeLists.txt)
#   ARCH        x64 or arm64: the name of the tarball, foxsdr-<VERSION>-linux-<ARCH>.tar.gz, and of the folder inside it
#   OUT_DIR     where the folder (OUT_DIR/foxsdr-<VERSION>-linux-<ARCH>/) and the tarball beside it go
#   --slint-sdk DIR   the Slint C++ toolkit's folder (it holds lib/libslint_cpp.so and licenses/): with it the tarball carries the new window
#
# What is in it (tools/check-linux-tarball.sh reads every one back):
#   foxsdr             the launcher (installer/linux/foxsdr): the new window by default, Legacy on --legacy or FOXSDR_WINDOW=legacy, and when the new window cannot run
#   foxsdr-slint       the new window; its RUNPATH names $ORIGIN, so libslint_cpp.so is found beside it wherever the folder is unpacked
#   libslint_cpp.so    the Slint toolkit's library, stripped (the debug information of the SDK's copy is not shipped)
#   cascade            Legacy, the old window; foxsdr-engine, the receiver with no window
#   licenses/slint/    the toolkit's own LICENSE.md and THIRDPARTY.md (the Royalty-free licence's disclosure is also shown inside the window, in About)
#   resources/         the band plans and the firmware notice both programs read from beside themselves
#
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
set -eu

build="${1:-}"; version="${2:-}"; arch="${3:-}"; out="${4:-}"
if [ -z "$build" ] || [ -z "$version" ] || [ -z "$arch" ] || [ -z "$out" ]; then
    echo "usage: $0 BUILD_DIR VERSION ARCH OUT_DIR [--slint-sdk DIR]" >&2
    exit 2
fi
shift 4
sdk=""
while [ $# -gt 0 ]; do
    case "$1" in
        --slint-sdk) sdk="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

here="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
top="foxsdr-$version-linux-$arch"
dir="$out/$top"
rm -rf "$dir"
mkdir -p "$dir/resources/bandplans"

cp "$build/cascade" "$build/foxsdr-engine" "$here/README.md" "$here/LICENSE" "$here/PRIVACY.md" "$dir/"
cp "$here"/resources/bandplans/*.json "$dir/resources/bandplans/"
cp -r "$here/resources/firmware" "$dir/resources/"

if [ -n "$sdk" ]; then
    slint="$build/src/slint/foxsdr-slint"
    [ -f "$slint" ] || { echo "no $slint: the new window was not built" >&2; exit 1; }
    [ -f "$sdk/lib/libslint_cpp.so" ] || { echo "no $sdk/lib/libslint_cpp.so" >&2; exit 1; }
    cp "$slint" "$dir/foxsdr-slint"
    cp "$sdk/lib/libslint_cpp.so" "$dir/libslint_cpp.so"
    strip --strip-unneeded "$dir/libslint_cpp.so"
    mkdir -p "$dir/licenses/slint"
    cp "$sdk/licenses/LICENSE.md" "$sdk/licenses/THIRDPARTY.md" "$dir/licenses/slint/"
    install -m 0755 "$here/installer/linux/foxsdr" "$dir/foxsdr"
fi
chmod 0755 "$dir/cascade" "$dir/foxsdr-engine"
[ -f "$dir/foxsdr-slint" ] && chmod 0755 "$dir/foxsdr-slint"

tar --owner=0 --group=0 --numeric-owner -C "$out" -czf "$out/$top.tar.gz" "$top"
ls -l "$out/$top.tar.gz"
echo "version=$version"
