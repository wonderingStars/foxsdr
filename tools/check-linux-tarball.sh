#!/usr/bin/env bash
# Asserts what a Linux release tarball must hold, and runs what it holds.
#
#   tools/check-linux-tarball.sh TARBALL TOPDIR VERSION [--no-elf] [--no-run]
#   tools/check-linux-tarball.sh --self-test
#
# WHY THIS EXISTS (engine extraction stage 7g). The tarball used to be one binary and three text
# files, staged by a `cp` in .github/workflows/build.yml, and nothing looked inside it afterwards:
# a tarball without the application is a release asset that downloads fine. With a second binary
# (foxsdr-engine, the receiver with no window) and the resources both binaries read from beside
# themselves (BandPlan::defaultDir, aorFirmwarePath: executable-relative), a `cp` line that drops
# one of them is an easy edit and an invisible one. So the tarball is READ BACK, after it is made:
#
#   * every member that must be there, by name: both binaries, the three text files, a band plan
#     and AOR's firmware notice;
#   * both binaries are ELF files with the execute bit (a `cp` from a build dir keeps it; a stage
#     through some other route may not);
#   * both binaries, extracted and RUN, answer `--version` with exactly `FoxSDR <VERSION>` - the
#     line tools/release.ps1 reads off cascade.exe - so the engine in the tarball is the engine of
#     THIS version, not a stale file.
#
# TOPDIR is the directory name inside the archive (foxsdr-<version>-linux-x64). --no-elf and
# --no-run exist for --self-test, which builds a small archive of stand-in scripts on whatever
# machine this is and shows the check passing on a good one and failing, one reason each, on a
# tarball that lacks the engine, a band plan, the firmware notice, or carries the wrong version.
# That is the proof that the assertion can fail; the CI run is the first proof that it passes on
# the real tarball (docs/engine-stage4.md section 6.20: CI-only evidence until a run has happened).
#
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
set -u

fail_count=0
fail() { echo "FAIL: $*" >&2; fail_count=$((fail_count + 1)); }
ok() { echo "ok:   $*"; }

# check_tarball TARBALL TOPDIR VERSION ELF(1|0) RUN(1|0)   - prints each failure, returns 1 on any
check_tarball() {
    local tarball="$1" top="$2" version="$3" want_elf="$4" want_run="$5"
    local before=$fail_count
    if [ ! -f "$tarball" ]; then fail "no tarball at $tarball"; return 1; fi

    local listing
    listing="$(tar -tzf "$tarball")" || { fail "$tarball is not a readable gzip tar"; return 1; }

    has() { printf '%s\n' "$listing" | grep -qxF -- "$1"; }
    for member in "$top/cascade" "$top/foxsdr-engine" "$top/README.md" "$top/LICENSE" "$top/PRIVACY.md" \
                  "$top/resources/firmware/aor/NOTICE"; do
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
    local bin
    for bin in cascade foxsdr-engine; do
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
    rm -rf "$work"
    [ "$fail_count" -eq "$before" ]
}

# --- the self-test ------------------------------------------------------------------------------------------

make_mock() {   # make_mock DIR TOP VERSION [omit...]   - a tree of stand-ins, minus what is named in omit
    local dir="$1" top="$2" version="$3"
    shift 3
    mkdir -p "$dir/$top/resources/bandplans" "$dir/$top/resources/firmware/aor"
    local bin
    for bin in cascade foxsdr-engine; do
        printf '#!/bin/sh\necho "FoxSDR %s"\n' "$version" > "$dir/$top/$bin"
        chmod +x "$dir/$top/$bin"
    done
    for f in README.md LICENSE PRIVACY.md resources/bandplans/uk.json resources/firmware/aor/NOTICE; do
        echo x > "$dir/$top/$f"
    done
    local omit
    for omit in "$@"; do rm -f "$dir/$top/$omit"; done
}

expect() {   # expect pass|fail DESCRIPTION TARBALL TOP VERSION
    local want="$1" what="$2" tarball="$3" top="$4" version="$5"
    local before=$fail_count
    # a failure is the point of a "fail" case: count what it reports, then take it back out of the total
    local out rc
    out="$(check_tarball "$tarball" "$top" "$version" 0 1 2>&1)"; rc=$?
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
    expect pass "a tarball with both binaries and the resources" "$scratch/good.tar.gz" "$top" 9.9.9

    make_mock "$scratch/noengine" "$top" 9.9.9 foxsdr-engine
    tar -C "$scratch/noengine" -czf "$scratch/noengine.tar.gz" "$top"
    expect fail "a tarball WITHOUT foxsdr-engine" "$scratch/noengine.tar.gz" "$top" 9.9.9

    make_mock "$scratch/nocascade" "$top" 9.9.9 cascade
    tar -C "$scratch/nocascade" -czf "$scratch/nocascade.tar.gz" "$top"
    expect fail "a tarball WITHOUT cascade" "$scratch/nocascade.tar.gz" "$top" 9.9.9

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
    echo "usage: $0 TARBALL TOPDIR VERSION [--no-elf] [--no-run]   |   $0 --self-test" >&2
    exit 2
fi
shift 3
want_elf=1; want_run=1
for a in "$@"; do
    case "$a" in
        --no-elf) want_elf=0 ;;
        --no-run) want_run=0 ;;
        *) echo "unknown argument: $a" >&2; exit 2 ;;
    esac
done
if check_tarball "$tarball" "$top" "$version" "$want_elf" "$want_run"; then
    echo "$tarball: complete"
    exit 0
fi
echo "$tarball: INCOMPLETE ($fail_count failure(s))" >&2
exit 1
