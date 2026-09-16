#!/bin/bash
# Pushes the curated CASCADE_ANDROID test binaries (see the
# CASCADE_ANDROID_TEST_NAMES list in tests/CMakeLists.txt) from a
# cross-compiled Android build tree to whatever device or emulator `adb` is
# currently pointed at, runs each one, and prints a PASS/FAIL/SKIP summary
# line. A device is required — this does not build anything.
#
# Usage: tools/run-android-tests.sh <build-dir>
#   e.g. tools/run-android-tests.sh build-android-x86_64
#
# Known environment differences from a desktop Linux run, both confirmed on
# the x86_64 emulator (see the CASCADE_ANDROID commit message for the raw
# output): test_band_plan needs resources/bandplans staged next to the
# binary (BandPlan::defaultDir() is executable-relative; this script does
# not stage it - push resources/bandplans yourself under the same directory
# first if you need that test to find real data), and test_usb_usbfs's
# "no sysfs USB tree on this machine" and "/tmp exists" assumptions do not
# hold on Android (the emulator DOES expose /sys/bus/usb/devices via its
# virtual USB controllers, and there is no /tmp at all, only
# /data/local/tmp) - both are environment facts about the test's own
# assumptions, not defects in cascade_core.
#
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
set -u

BUILD_DIR="${1:?usage: $0 <build-dir>}"
DEVICE_DIR=/data/local/tmp/foxsdr-test

if ! adb get-state >/dev/null 2>&1; then
    echo "no adb device/emulator reachable (adb get-state failed)" >&2
    exit 1
fi

adb shell "mkdir -p ${DEVICE_DIR}" >/dev/null

for f in "${BUILD_DIR}"/tests/test_*; do
    [ -x "$f" ] || continue
    adb push "$f" "${DEVICE_DIR}/" >/dev/null
done
adb shell "chmod 755 ${DEVICE_DIR}/*"

NAMES=$(adb shell "ls ${DEVICE_DIR}" | tr -d '\r' | grep '^test_')
for name in $NAMES; do
    if [ "$name" = "test_json_dump_policy" ]; then
        # This test scans the HOST src/ tree by absolute path (see
        # tests/CMakeLists.txt's normal, non-Android registration of it) —
        # nothing meaningful to scan on the device, so it is built (proving
        # it cross-compiles) but not run here.
        echo "SKIP  $name (needs the host src/ tree, device run not meaningful)"
        continue
    fi
    out=$(adb shell "cd ${DEVICE_DIR} && TMPDIR=${DEVICE_DIR} HOME=${DEVICE_DIR} ./$name" 2>&1; echo "EXIT:$?")
    exitcode=$(echo "$out" | tail -1 | sed 's/EXIT://')
    body=$(echo "$out" | sed '$d')
    lastline=$(echo "$body" | tail -1)
    if [ "$exitcode" = "0" ]; then
        echo "PASS  $name :: $lastline"
    else
        echo "FAIL  $name (exit $exitcode) :: $lastline"
        echo "----- full output for $name -----"
        echo "$body"
        echo "----- end $name -----"
    fi
done
