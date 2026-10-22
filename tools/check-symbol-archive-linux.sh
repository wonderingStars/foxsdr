#!/usr/bin/env bash
# What a working Linux symbol archive looks like, asserted for ONE binary of a build.
#
#   tools/check-symbol-archive-linux.sh MODULE SOURCE-FILE [BUILD-DIR] [SYMBOLS-DIR]
#
#   tools/check-symbol-archive-linux.sh cascade main.cpp
#   tools/check-symbol-archive-linux.sh foxsdr-engine engine_main.cpp
#
# This is the body of the "Check the symbol archive" step of .github/workflows/build.yml, which was
# written inline for `cascade` and is now called once per binary (engine extraction stage 7g added
# foxsdr-engine: its build id is its own, so a crash report from a headless run resolves only if THAT
# binary was archived). Moved out of the workflow so it can be run by hand on any Linux build; the
# assertions are the workflow's, unchanged:
#
#   * the archived split-debug file (BUILD-DIR/MODULE's build id, under SYMBOLS-DIR) has line tables;
#   * the binary that ships has none (a full-DWARF binary would be tens of megabytes heavier for every
#     user);
#   * a real address in the shipped binary - main() - resolves, in the archived file, with the same
#     addr2line a developer will use, to a function and a line in SOURCE-FILE.
#
# Matches are done with case rather than a pipe into grep -q, because a pipe plus pipefail turns
# grep's early exit into a spurious failure.
#
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
set -eu

mod="${1:-}"; srcfile="${2:-}"; builddir="${3:-build}"; symdir="${4:-symbols}"
if [ -z "$mod" ] || [ -z "$srcfile" ]; then
    echo "usage: $0 MODULE SOURCE-FILE [BUILD-DIR] [SYMBOLS-DIR]" >&2
    exit 2
fi
bin="$builddir/$mod"

id=$(readelf -n "$bin" | sed -n 's/.*Build ID: \([0-9a-f]*\).*/\1/p')
test -n "$id"
dbg="$symdir/$mod.debug/$id/$mod.debug"
echo "$mod: build id $id"
ls -l "$bin" "$dbg"
dbg_sections=$(readelf -S -W "$dbg")
bin_sections=$(readelf -S -W "$bin")
printf '%s\n' "$dbg_sections" | grep -E '\.debug_(info|line)' || true
case "$dbg_sections" in
  *.debug_line*) ;;
  *) echo "FAIL: the archived split-debug file for $mod has no line tables"; exit 1 ;;
esac
case "$bin_sections" in
  *.debug_info*|*.debug_line*) echo "FAIL: the $mod binary that ships still carries DWARF"; exit 1 ;;
esac
addr=$(nm "$bin" | awk '$2 == "T" && $3 == "main" { print $1 }')
test -n "$addr"
where=$(addr2line -f -C -e "$dbg" "0x$addr")
echo "$where"
case "$where" in
  *"$srcfile":[0-9]*) ;;
  *) echo "FAIL: main() of $mod did not resolve to a line of $srcfile in the archived file"; exit 1 ;;
esac
echo "$mod: the symbol archive is good"
