#!/usr/bin/env bash
# Asserts what a Linux release tarball must hold, and runs what it holds.
#
#   tools/check-linux-tarball.sh TARBALL TOPDIR VERSION [--no-elf] [--no-run] [--no-slint] [--glibc-max X.Y]
#   tools/check-linux-tarball.sh --self-test
#
# WHY THIS EXISTS (engine extraction stage 7g). The tarball used to be one binary and three text
# files, staged by a `cp` in .github/workflows/build.yml, and nothing looked inside it afterwards:
# a tarball without the application is a release asset that downloads fine. With a second binary
# (foxsdr-engine, the receiver with no window) and the resources both binaries read from beside
# themselves (BandPlan::defaultDir, aorFirmwarePath: executable-relative), a `cp` line that drops
# one of them is an easy edit and an invisible one. So the tarball is READ BACK, after it is made:
#
#   * every member that must be there, by name: the programs, the three text files, a band plan
#     and AOR's firmware notice;
#   * the programs are ELF files with the execute bit (a `cp` from a build dir keeps it; a stage
#     through some other route may not);
#   * the programs, extracted and RUN, answer `--version` with exactly `FoxSDR <VERSION>` - the
#     line tools/release.ps1 reads off cascade.exe - so what is in the tarball is THIS version, not
#     a stale file.
#
# THE NEW WINDOW (the Linux lane of docs/multi-radio-records/linux-local.md section 11). Unless
# --no-slint, the tarball must also hold foxsdr-slint (the new window), libslint_cpp.so (its toolkit),
# the toolkit's licence texts under licenses/slint/ and the launcher `foxsdr`, and:
#   * foxsdr-slint's RUNPATH names $ORIGIN, and every library of its dynamic section resolves on this
#     machine, libslint_cpp.so from the unpacked folder itself (a copied tarball has no SDK folder);
#   * the launcher answers --version through the new window, and `foxsdr --legacy --version` through
#     Legacy (cascade);
#   * --glibc-max X.Y: no ELF file of the tarball imports a glibc symbol newer than X.Y
#     (tools/check-glibc-ceiling.sh).
#
# TOPDIR is the directory name inside the archive (foxsdr-<version>-linux-x64). --no-elf and
# --no-run exist for --self-test, which builds a small archive of stand-in scripts on whatever
# machine this is and shows the check passing on a good one and failing, one reason each, on a
# tarball that lacks the engine, a band plan, the firmware notice, the new window, its library or the
# launcher, or carries the wrong version.
# That is the proof that the assertion can fail; the CI run is the first proof that it passes on
# the real tarball (docs/engine-stage4.md section 6.20: CI-only evidence until a run has happened).
#
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
set -u

fail_count=0
fail() { echo "FAIL: $*" >&2; fail_count=$((fail_count + 1)); }
ok() { echo "ok:   $*"; }

here="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"

# check_tarball TARBALL TOPDIR VERSION ELF(1|0) RUN(1|0) SLINT(1|0) GLIBC_MAX|""   - prints each failure, returns 1 on any
check_tarball() {
    local tarball="$1" top="$2" version="$3" want_elf="$4" want_run="$5" want_slint="$6" glibc_max="$7"
    local before=$fail_count
    if [ ! -f "$tarball" ]; then fail "no tarball at $tarball"; return 1; fi

    local listing
    listing="$(tar -tzf "$tarball")" || { fail "$tarball is not a readable gzip tar"; return 1; }

    has() { printf '%s\n' "$listing" | grep -qxF -- "$1"; }
    local members=("$top/cascade" "$top/foxsdr-engine" "$top/README.md" "$top/LICENSE" "$top/PRIVACY.md" "$top/resources/firmware/aor/NOTICE")
    if [ "$want_slint" = "1" ]; then
        members+=("$top/foxsdr" "$top/foxsdr-slint" "$top/libslint_cpp.so" "$top/licenses/slint/LICENSE.md" "$top/licenses/slint/THIRDPARTY.md")
    fi
    for member in "${members[@]}"; do
        if has "$member"; then ok "member $member"; else fail "$tarball has no $member"; fi
    done
    if printf '%s\n' "$listing" | grep -qE -- "^$top/resources/bandplans/[^/]+[.]json$"; then
        ok "member $top/resources/bandplans/*.json"
    else
        fail "$tarball holds no band plan under $top/resources/bandplans/ (the binaries read them from beside themselves)"
    fi

    local work
    work="$(mktemp -d)" || { fail "no scratch directory"; return 1; }
    if ! tar -xzf "$tarball" -C "$work"; then
        fail "could not extract $tarball"
        rm -rf "$work"
        return 1
    fi
    local bins="cascade foxsdr-engine"
    [ "$want_slint" = "1" ] && bins="$bins foxsdr-slint"
    local bin
    for bin in $bins; do
        local path="$work/$top/$bin"
        [ -f "$path" ] || continue   # already reported above
        if [ "$want_elf" = "1" ]; then
            if [ "$(head -c 4 "$path" | od -An -tx1 | tr -d ' \n')" = "7f454c46" ]; then ok "$bin is an ELF file"; else fail "$bin is not an ELF file"; fi
        fi
        if [ -x "$path" ]; then ok "$bin is executable"; else fail "$bin has no execute bit in the tarball"; fi
        if [ "$want_run" = "1" ]; then
            local line
            line="$("$path" --version 2>/dev/null | head -n 1)"
            if [ "$line" = "FoxSDR $version" ]; then ok "$bin --version: $line"; else fail "$bin --version printed '$line', expected 'FoxSDR $version'"; fi
        fi
    done
    if [ "$want_slint" = "1" ]; then
        local launcher="$work/$top/foxsdr"
        if [ -f "$launcher" ]; then
            if [ -x "$launcher" ]; then ok "foxsdr (the launcher) is executable"; else fail "foxsdr (the launcher) has no execute bit in the tarball"; fi
            if [ "$want_run" = "1" ]; then
                local line
                line="$("$launcher" --version 2>/dev/null | head -n 1)"
                if [ "$line" = "FoxSDR $version" ]; then ok "foxsdr --version: $line"; else fail "foxsdr --version printed '$line', expected 'FoxSDR $version'"; fi
                line="$("$launcher" --legacy --version 2>/dev/null | head -n 1)"
                if [ "$line" = "FoxSDR $version" ]; then ok "foxsdr --legacy --version: $line"; else fail "foxsdr --legacy --version printed '$line', expected 'FoxSDR $version'"; fi
            fi
        fi
        if [ "$want_elf" = "1" ] && [ -f "$work/$top/foxsdr-slint" ]; then
            local dyn
            dyn="$(readelf -d "$work/$top/foxsdr-slint" 2>/dev/null)"
            case "$dyn" in
                *'RUNPATH'*'$ORIGIN'* | *'RPATH'*'$ORIGIN'*) ok "foxsdr-slint's RUNPATH names \$ORIGIN" ;;
                *) fail "foxsdr-slint's RUNPATH does not name \$ORIGIN (the toolkit library would not be found in an unpacked copy)" ;;
            esac
            local resolved
            resolved="$(env -u LD_LIBRARY_PATH ldd "$work/$top/foxsdr-slint" 2>&1)"
            case "$resolved" in
                *"libslint_cpp.so => $work/$top/libslint_cpp.so"*) ok "libslint_cpp.so resolves from the unpacked folder" ;;
                *) fail "libslint_cpp.so does not resolve from beside foxsdr-slint: $(printf '%s\n' "$resolved" | grep libslint_cpp | head -n 1)" ;;
            esac
            if printf '%s\n' "$resolved" | grep -q 'not found'; then
                fail "foxsdr-slint has libraries this system does not provide: $(printf '%s\n' "$resolved" | grep 'not found' | tr -s ' \t' ' ' | tr '\n' ';')"
            else
                ok "every library of foxsdr-slint resolves on this system"
            fi
        fi
    fi
    if [ -n "$glibc_max" ] && [ "$want_elf" = "1" ]; then
        local elfs=() f
        for f in cascade foxsdr-engine foxsdr-slint libslint_cpp.so; do
            [ -f "$work/$top/$f" ] && elfs+=("$work/$top/$f")
        done
        if bash "$here/check-glibc-ceiling.sh" "$glibc_max" "${elfs[@]}"; then ok "glibc ceiling $glibc_max holds"; else fail "a program of the tarball needs a newer glibc than $glibc_max"; fi
    fi
    rm -rf "$work"
    [ "$fail_count" -eq "$before" ]
}

# --- the self-test ------------------------------------------------------------------------------------------

make_mock() {   # make_mock DIR TOP VERSION [omit...]   - a tree of stand-ins, minus what is named in omit
    local dir="$1" top="$2" version="$3"
    shift 3
    mkdir -p "$dir/$top/resources/bandplans" "$dir/$top/resources/firmware/aor" "$dir/$top/licenses/slint"
    local bin
    for bin in cascade foxsdr-engine foxsdr-slint foxsdr; do
        printf '#!/bin/sh\necho "FoxSDR %s"\n' "$version" > "$dir/$top/$bin"
        chmod +x "$dir/$top/$bin"
    done
    for f in README.md LICENSE PRIVACY.md resources/bandplans/uk.json resources/firmware/aor/NOTICE libslint_cpp.so licenses/slint/LICENSE.md licenses/slint/THIRDPARTY.md; do
        echo x > "$dir/$top/$f"
    done
    local omit
    for omit in "$@"; do rm -f "$dir/$top/$omit"; done
}

expect() {   # expect pass|fail DESCRIPTION TARBALL TOP VERSION [SLINT=1]
    local want="$1" what="$2" tarball="$3" top="$4" version="$5" slint="${6:-1}"
    local before=$fail_count
    # a failure is the point of a "fail" case: count what it reports, then take it back out of the total
    local out rc
    out="$(check_tarball "$tarball" "$top" "$version" 0 1 "$slint" "" 2>&1)"; rc=$?
    # the subshell above kept its own counter: rc carries the verdict
    fail_count=$before
    if [ "$want" = "pass" ] && [ "$rc" -eq 0 ]; then echo "self-test ok:   $what (accepted)"; return 0; fi
    if [ "$want" = "fail" ] && [ "$rc" -ne 0 ]; then echo "self-test ok:   $what (refused: $(printf '%s\n' "$out" | grep -m1 '^FAIL' | cut -c1-90))"; return 0; fi
    echo "SELF-TEST FAIL: $what - expected $want, the check returned $rc" >&2
    printf '%s\n' "$out" >&2
    fail_count=$((fail_count + 1))
    return 1
}

self_test() {
    local scratch top="foxsdr-9.9.9-linux-x64"
    scratch="$(mktemp -d)" || exit 1
    make_mock "$scratch/good" "$top" 9.9.9
    tar -C "$scratch/good" -czf "$scratch/good.tar.gz" "$top"
    expect pass "a tarball with the programs, the new window and the resources" "$scratch/good.tar.gz" "$top" 9.9.9

    make_mock "$scratch/noengine" "$top" 9.9.9 foxsdr-engine
    tar -C "$scratch/noengine" -czf "$scratch/noengine.tar.gz" "$top"
    expect fail "a tarball WITHOUT foxsdr-engine" "$scratch/noengine.tar.gz" "$top" 9.9.9

    make_mock "$scratch/nocascade" "$top" 9.9.9 cascade
    tar -C "$scratch/nocascade" -czf "$scratch/nocascade.tar.gz" "$top"
    expect fail "a tarball WITHOUT cascade (Legacy)" "$scratch/nocascade.tar.gz" "$top" 9.9.9

    make_mock "$scratch/noslint" "$top" 9.9.9 foxsdr-slint
    tar -C "$scratch/noslint" -czf "$scratch/noslint.tar.gz" "$top"
    expect fail "a tarball WITHOUT the new window" "$scratch/noslint.tar.gz" "$top" 9.9.9

    make_mock "$scratch/nolib" "$top" 9.9.9 libslint_cpp.so
    tar -C "$scratch/nolib" -czf "$scratch/nolib.tar.gz" "$top"
    expect fail "a tarball WITHOUT the toolkit library" "$scratch/nolib.tar.gz" "$top" 9.9.9

    make_mock "$scratch/nolauncher" "$top" 9.9.9 foxsdr
    tar -C "$scratch/nolauncher" -czf "$scratch/nolauncher.tar.gz" "$top"
    expect fail "a tarball WITHOUT the launcher" "$scratch/nolauncher.tar.gz" "$top" 9.9.9

    make_mock "$scratch/nolic" "$top" 9.9.9 licenses/slint/THIRDPARTY.md
    tar -C "$scratch/nolic" -czf "$scratch/nolic.tar.gz" "$top"
    expect fail "a tarball WITHOUT the toolkit's third-party notices" "$scratch/nolic.tar.gz" "$top" 9.9.9

    # the same tarball is complete for a build without the new window (the arm64 job until its toolkit route is decided)
    expect pass "a tarball without the new window, checked with --no-slint" "$scratch/noslint.tar.gz" "$top" 9.9.9 0

    make_mock "$scratch/noplan" "$top" 9.9.9 resources/bandplans/uk.json
    tar -C "$scratch/noplan" -czf "$scratch/noplan.tar.gz" "$top"
    expect fail "a tarball with no band plan" "$scratch/noplan.tar.gz" "$top" 9.9.9

    make_mock "$scratch/nofw" "$top" 9.9.9 resources/firmware/aor/NOTICE
    tar -C "$scratch/nofw" -czf "$scratch/nofw.tar.gz" "$top"
    expect fail "a tarball with no firmware notice" "$scratch/nofw.tar.gz" "$top" 9.9.9

    expect fail "the right tarball, the wrong version" "$scratch/good.tar.gz" "$top" 9.9.8

    make_mock "$scratch/noexec" "$top" 9.9.9
    chmod -x "$scratch/noexec/$top/foxsdr-engine"
    if [ -x "$scratch/noexec/$top/foxsdr-engine" ]; then
        # an MSYS / Git Bash file system reports a script as executable whatever chmod says
        echo "self-test SKIP: an engine without its execute bit (this file system cannot clear the bit, so the case cannot be built here)"
    else
        tar -C "$scratch/noexec" -czf "$scratch/noexec.tar.gz" "$top"
        expect fail "an engine without its execute bit" "$scratch/noexec.tar.gz" "$top" 9.9.9
    fi

    expect fail "a tarball that does not exist" "$scratch/absent.tar.gz" "$top" 9.9.9

    rm -rf "$scratch"
    if [ "$fail_count" -eq 0 ]; then echo "self-test: all cases behaved"; return 0; fi
    echo "self-test: $fail_count case(s) misbehaved" >&2
    return 1
}

if [ "${1:-}" = "--self-test" ]; then
    self_test
    exit $?
fi

tarball="${1:-}"; top="${2:-}"; version="${3:-}"
if [ -z "$tarball" ] || [ -z "$top" ] || [ -z "$version" ]; then
    echo "usage: $0 TARBALL TOPDIR VERSION [--no-elf] [--no-run] [--no-slint] [--glibc-max X.Y]   |   $0 --self-test" >&2
    exit 2
fi
shift 3
want_elf=1; want_run=1; want_slint=1; glibc_max=""
while [ $# -gt 0 ]; do
    case "$1" in
        --no-elf) want_elf=0 ;;
        --no-run) want_run=0 ;;
        --no-slint) want_slint=0 ;;
        --glibc-max) glibc_max="${2:-}"; shift ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done
if check_tarball "$tarball" "$top" "$version" "$want_elf" "$want_run" "$want_slint" "$glibc_max"; then
    echo "$tarball: complete"
    exit 0
fi
echo "$tarball: INCOMPLETE ($fail_count failure(s))" >&2
exit 1
