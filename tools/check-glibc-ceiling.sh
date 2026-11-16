#!/usr/bin/env bash
# The glibc ceiling of a Linux release: the highest GLIBC_ symbol version any listed ELF file imports must not exceed MAX.
#
#   tools/check-glibc-ceiling.sh MAX FILE...        MAX is 2.35 for the x64 build (Ubuntu 22.04's glibc) and 2.39 for arm64
#   tools/check-glibc-ceiling.sh --self-test
#
# A binary built on a newer system silently needs that system's glibc: the loader refuses it with "version `GLIBC_2.38' not found" on an older one, and nothing in a build log says so. This reads each
# file's dynamic symbol table (objdump -T) and fails, naming the symbol's version, when it is above the ceiling. A file that is not an ELF file, or imports no versioned glibc symbol, passes (shell
# scripts, text). The libstdc++ side (GLIBCXX_) is printed for the record, not judged: the compiler's runtime comes with the system's libstdc++ and is checked by running the program.
#
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
set -u

# highest GLIBC_ version imported by one file, "" when none
highest() {
    objdump -T "$1" 2>/dev/null | grep -o 'GLIBC_[0-9][0-9.]*' | sed 's/^GLIBC_//' | sort -uV | tail -n 1
}

# newer A B: true when version A is above version B
newer() {
    [ "$1" != "$2" ] && [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -n 1)" = "$1" ]
}

check() {
    local max="$1"; shift
    local bad=0 f h x
    for f in "$@"; do
        h="$(highest "$f")"
        x="$(objdump -T "$f" 2>/dev/null | grep -o 'GLIBCXX_[0-9][0-9.]*' | sed 's/^GLIBCXX_//' | sort -uV | tail -n 1)"
        if [ -z "$h" ]; then
            echo "ok:   $f imports no versioned glibc symbol"
        elif newer "$h" "$max"; then
            echo "FAIL: $f imports GLIBC_$h, above the ceiling GLIBC_$max" >&2
            bad=$((bad + 1))
        else
            echo "ok:   $f highest GLIBC_$h (ceiling $max)${x:+, GLIBCXX_$x}"
        fi
    done
    [ "$bad" -eq 0 ]
}

self_test() {
    local fails=0
    # the comparison itself: versions compare part by part, so 2.9 is BELOW 2.35 (9 < 35) and 2.38 above it
    newer 2.38 2.35 || { echo "SELF-TEST FAIL: 2.38 is above 2.35" >&2; fails=$((fails + 1)); }
    newer 2.35 2.35 && { echo "SELF-TEST FAIL: 2.35 is not above 2.35" >&2; fails=$((fails + 1)); }
    newer 2.4 2.35 && { echo "SELF-TEST FAIL: 2.4 is below 2.35 as a version (4 < 35)" >&2; fails=$((fails + 1)); }
    newer 2.9 2.35 && { echo "SELF-TEST FAIL: 2.9 is below 2.35 as a version (9 < 35)" >&2; fails=$((fails + 1)); }
    # a real file: this shell itself imports some version
    local sh_file; sh_file="$(command -v bash)"
    local h; h="$(highest "$sh_file")"
    if [ -n "$h" ]; then
        check 99.0 "$sh_file" >/dev/null || { echo "SELF-TEST FAIL: a ceiling of 99.0 refused bash" >&2; fails=$((fails + 1)); }
        check 1.0 "$sh_file" >/dev/null 2>&1 && { echo "SELF-TEST FAIL: a ceiling of 1.0 accepted bash (it imports GLIBC_$h)" >&2; fails=$((fails + 1)); }
    else
        echo "self-test SKIP: bash imports no versioned glibc symbol here"
    fi
    if [ "$fails" -eq 0 ]; then echo "self-test: all cases behaved"; return 0; fi
    echo "self-test: $fails case(s) misbehaved" >&2
    return 1
}

if [ "${1:-}" = "--self-test" ]; then
    self_test
    exit $?
fi
max="${1:-}"
if [ -z "$max" ] || [ $# -lt 2 ]; then
    echo "usage: $0 MAX FILE...   |   $0 --self-test" >&2
    exit 2
fi
shift
check "$max" "$@"
