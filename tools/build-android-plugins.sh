#!/bin/bash
# Cross-compiles the FoxSDR decoder plugins for Android and stages them where
# the apk packages them from.
#
# ---------------------------------------------------------------------------
# WHY THE PLUGINS ARE INSIDE THE APK
# ---------------------------------------------------------------------------
#
# GOOGLE PLAY FORBIDS AN APPLICATION DOWNLOADING EXECUTABLE CODE. On the
# desktop FoxSDR fetches decoder modules from a catalogue, verifies a sha256
# and drops the .so or .dll into a plugins directory; none of that is permitted
# on this platform at any price. So the modules ship INSIDE the signed apk,
# under lib/<abi>/, and the platform installs them into the application's own
# native library directory beside libfoxsdr.so - which is the directory
# PluginHost scans (core/plugin_host.hpp's chooseAndroidPluginDir).
#
# Nothing this script produces is fetched at runtime, and the plugin store on
# Android says so rather than offering a key that cannot work.
#
# ---------------------------------------------------------------------------
# WHY A SCRIPT AND NOT A GRADLE/CMAKE DEPENDENCY
# ---------------------------------------------------------------------------
#
# The plugins are a SEPARATE REPOSITORY with its own history, its own
# versioning and its own licence per module (see its README.md). An apk build
# that reached into a sibling checkout and built it would make the application
# unbuildable for anyone who does not have that checkout - which is everyone
# outside this project, and also this project's own CI.
#
# So the coupling is one explicit step, and android/app/build.gradle wires it
# in ONLY when -PfoxsdrPluginsDir=<path> names the tree. Without that property
# the apk builds exactly as it did before, with whatever is already staged in
# jniLibs (which is gitignored: binaries are not committed).
#
# ---------------------------------------------------------------------------
# USAGE
# ---------------------------------------------------------------------------
#
#   tools/build-android-plugins.sh [--plugins <dir>] [--abi <abi>]...
#                                  [--ndk <dir>] [--out <dir>] [--clean]
#
#   --plugins  the plugin repository checkout. Default $FOXSDR_PLUGINS_DIR, or
#              ../foxsdr-plugins-dev beside this one.
#   --abi      an Android ABI to build. Repeatable. Default: the two the apk
#              declares, arm64-v8a and x86_64 (android/app/build.gradle's
#              abiFilters) - a third would be bytes no shipped build can load.
#   --ndk      the NDK. Default $ANDROID_NDK_HOME, else
#              $ANDROID_HOME/ndk/27.2.12479018 - the version build.gradle
#              pins, named so that a machine with two NDKs cannot produce two
#              different binaries from one commit.
#   --out      where to stage. Default android/app/src/main/jniLibs, which is
#              Gradle's own default jniLibs source directory.
#   --clean    delete each ABI's build tree first.
#
# WHICH MODULES GET BUILT IS NOT DECIDED HERE. The plugin repository's own
# CMakeLists.txt carries the list (FOXSDR_ANDROID_BUNDLED) and the reason each
# excluded module is excluded - TLS the NDK cannot provide, a legal notice with
# no install step to show it at, a patent-encumbered codec, or a module that is
# not published yet. This script builds whatever that configure produces, so
# the publishing decision lives with the plugins and not with the apk.
#
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "${SCRIPT_DIR}/.." && pwd)

PLUGINS_DIR="${FOXSDR_PLUGINS_DIR:-${REPO_ROOT}/../foxsdr-plugins-dev}"
NDK_DIR="${ANDROID_NDK_HOME:-${ANDROID_HOME:-$HOME/android-sdk}/ndk/27.2.12479018}"
OUT_DIR="${REPO_ROOT}/android/app/src/main/jniLibs"
CLEAN=0
ABIS=()

while [ $# -gt 0 ]; do
    case "$1" in
        --plugins) PLUGINS_DIR="$2"; shift 2;;
        --abi)     ABIS+=("$2");     shift 2;;
        --ndk)     NDK_DIR="$2";     shift 2;;
        --out)     OUT_DIR="$2";     shift 2;;
        --clean)   CLEAN=1;          shift;;
        -h|--help) sed -n '1,64p' "$0"; exit 0;;
        *) echo "unknown argument: $1" >&2; exit 2;;
    esac
done

if [ ${#ABIS[@]} -eq 0 ]; then
    ABIS=(arm64-v8a x86_64)
fi

# FAIL LOUDLY AND EARLY, naming the path that was wrong. A missing plugin tree
# used to be the most likely way to run this by accident, and an empty jniLibs
# directory afterwards looks exactly like a build that worked and shipped no
# decoders.
if [ ! -f "${PLUGINS_DIR}/CMakeLists.txt" ]; then
    echo "no plugin repository at ${PLUGINS_DIR}" >&2
    echo "pass --plugins <dir> or set FOXSDR_PLUGINS_DIR" >&2
    exit 1
fi
PLUGINS_DIR=$(cd "${PLUGINS_DIR}" && pwd)
TOOLCHAIN="${NDK_DIR}/build/cmake/android.toolchain.cmake"
if [ ! -f "${TOOLCHAIN}" ]; then
    echo "no NDK toolchain file at ${TOOLCHAIN}" >&2
    echo "pass --ndk <dir> or set ANDROID_NDK_HOME" >&2
    exit 1
fi

echo "plugins:   ${PLUGINS_DIR}"
echo "ndk:       ${NDK_DIR}"
echo "staging:   ${OUT_DIR}"

rc=0
for abi in "${ABIS[@]}"; do
    build="${PLUGINS_DIR}/build-android-${abi}"
    if [ "${CLEAN}" = "1" ]; then rm -rf "${build}"; fi
    echo
    echo "=== ${abi} ==============================================="
    # ANDROID_STL=c++_shared, AND IT MATCHES THE APPLICATION BY NECESSITY.
    # android/app/build.gradle passes the same value for libfoxsdr.so: two
    # copies of a STATIC libc++ in one process means two sets of type_info,
    # two operator new/delete pairs, and exceptions that cannot cross the
    # plugin boundary. A plugin built the other way loads and then misbehaves
    # in ways no log line explains.
    #
    # ANDROID_PLATFORM=android-26 is the apk's own minSdk.
    #
    # FOXSDR_CASCADE_DIR points the plugin repository's symbol archiver at
    # THIS tree's symbols/ directory, so a crash report naming an Android
    # plugin frame can be read - the same archive, in the same layout, as the
    # application's own POST_BUILD step writes.
    cmake -S "${PLUGINS_DIR}" -B "${build}" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN}" \
        -DANDROID_ABI="${abi}" \
        -DANDROID_PLATFORM=android-26 \
        -DANDROID_STL=c++_shared \
        -DCMAKE_BUILD_TYPE=Release \
        -DFOXSDR_CASCADE_DIR="${REPO_ROOT}" || { rc=1; continue; }
    cmake --build "${build}" || { rc=1; continue; }

    dest="${OUT_DIR}/${abi}"
    mkdir -p "${dest}"
    # THE STAGED SET IS NAMED, NOT GLOBBED WHOLESALE. The build tree's out/
    # also holds a <module>.so.debug beside every plugin - the split DWARF the
    # archiver keeps - and shipping those would double the apk for bytes no
    # user can use. "*.so" does not match "*.so.debug", and the loop below
    # skips one explicitly anyway, because that is the kind of thing a later
    # edit gets wrong silently.
    staged=0
    for f in "${build}"/out/libfoxsdr_plugin_*.so; do
        [ -f "$f" ] || continue
        case "$f" in *.debug) continue;; esac
        cp -f "$f" "${dest}/"
        staged=$((staged + 1))
    done
    if [ "${staged}" = "0" ]; then
        echo "no plugin modules were produced for ${abi}" >&2
        rc=1
        continue
    fi
    echo "--- staged ${staged} modules in ${dest}"
    ls -l "${dest}" | awk 'NR>1 { printf "    %8d  %s\n", $5, $9 }'
done

echo
if [ "${rc}" = "0" ]; then
    total=$(find "${OUT_DIR}" -name 'libfoxsdr_plugin_*.so' | wc -l)
    bytes=$(find "${OUT_DIR}" -name 'libfoxsdr_plugin_*.so' -printf '%s\n' | \
            awk '{ s += $1 } END { print s + 0 }')
    echo "staged ${total} module files, ${bytes} bytes total"
    echo "the apk packages these from jniLibs; nothing is fetched at runtime"
else
    echo "FAILED - see above" >&2
fi
exit "${rc}"
