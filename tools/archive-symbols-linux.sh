#!/usr/bin/env bash
# Archives the symbols for a Linux build we have just produced, keyed by the
# GNU build id a crash report will quote - the ELF counterpart of
# tools/archive-symbols.ps1, which does the same job for a PE/PDB. See that
# script's header for the argument this one does not repeat: why this has to
# be a POST_BUILD step and not a release-time afterthought, why the archive is
# keyed by a build id rather than a version or file name, and why symbols\ is
# not committed to git.
#
# WHAT DIFFERS FOR ELF, in full:
#
#   - there is no PDB. A normal (non `-s`-linked) binary already carries its
#     DWARF debug info inline, so the archive keeps TWO things instead of a
#     matched exe+pdb pair: a split-debug file (the debug info alone, via
#     `objcopy --only-keep-debug`) under
#         <root>/<module>.debug/<build-id>/<module>.debug
#     and the stripped binary itself under
#         <root>/<module>/<build-id>/<module>
#     - the exact two paths src/core/report_reader.hpp's
#     SymbolArchive::elfSymbolPath() already looks for, in that priority
#     order (split debug first, because it is the one with line tables).
#   - the build id comes from the NT_GNU_BUILD_ID note rather than a CodeView
#     record; tools/elf_symmap.py reads the identical note, so the archive key
#     and the map's own `buildId` field agree by construction, the same way
#     diag_report.cpp's gnuBuildIdFromNote() (the RUNNING image) agrees with
#     both.
#   - the symbol map (see tools/elf_symmap.py) is `nm`-based and needs no
#     debug info at all, so it is produced even for a binary this script
#     could not split debug info out of.
#
# Never fails the build: every error path here prints and exits 0, exactly as
# archive-symbols.ps1's Warn-and-continue does - a build must not fail because
# symbol tooling hiccuped, and the one place that promise is actually checked
# is tests/test_crash_capture.cpp's real-fault reports resolving through
# tools/elf_symmap.py's own output (see the test file for how).
#
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
set -u

warn() { echo "archive-symbols-linux: $*" >&2; }

BINARY=""
ARCHIVE_ROOT=""
VERSION=""
COMMIT=""
COMMIT_HEADER=""
MODULE="cascade"

while [ $# -gt 0 ]; do
    case "$1" in
        --binary) BINARY="$2"; shift 2 ;;
        --archive-root) ARCHIVE_ROOT="$2"; shift 2 ;;
        --version) VERSION="$2"; shift 2 ;;
        --commit) COMMIT="$2"; shift 2 ;;
        # Read at RUN time, not configure time - see cmake/git-commit.cmake and
        # archive-symbols.ps1's own -CommitHeader for why: the header is
        # regenerated on every build, so the commit in the index has to be the
        # commit that header names right now, not whatever HEAD was when CMake
        # last configured. An explicit --commit still wins (a caller with its
        # own idea, e.g. a plugin repository archiving into this tree).
        --commit-header) COMMIT_HEADER="$2"; shift 2 ;;
        --module) MODULE="$2"; shift 2 ;;
        *) warn "unknown argument: $1"; shift ;;
    esac
done

if [ -z "$COMMIT" ] && [ -n "$COMMIT_HEADER" ] && [ -f "$COMMIT_HEADER" ]; then
    COMMIT="$(sed -n 's/.*CASCADE_GIT_COMMIT[^"]*"\([^"]*\)".*/\1/p' "$COMMIT_HEADER" | head -1)"
fi

if [ -z "$BINARY" ] || [ -z "$ARCHIVE_ROOT" ]; then
    warn "usage: --binary PATH --archive-root DIR [--version V] [--commit C] [--module NAME]"
    exit 0
fi
if [ ! -f "$BINARY" ]; then
    warn "no binary at $BINARY"
    exit 0
fi

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PY="${FOXSDR_PYTHON:-python3}"

# Read via elf_symmap.py's own note parser (imported, not re-implemented), so
# there is exactly one piece of code in this repository that knows the
# NT_GNU_BUILD_ID note layout on the archiving side.
BUILD_ID="$("$PY" - "$HERE" "$BINARY" <<'PYEOF'
import sys
here, binary = sys.argv[1], sys.argv[2]
sys.path.insert(0, here)
from elf_symmap import read_gnu_build_id
try:
    print(read_gnu_build_id(binary))
except Exception as exc:
    print("ERROR:" + str(exc), file=sys.stderr)
    sys.exit(1)
PYEOF
)"
if [ $? -ne 0 ] || [ -z "$BUILD_ID" ]; then
    warn "could not read a GNU build id from $BINARY - reports against this build cannot be symbolised"
    exit 0
fi

DEBUG_DIR="$ARCHIVE_ROOT/$MODULE.debug/$BUILD_ID"
MODULE_DIR="$ARCHIVE_ROOT/$MODULE/$BUILD_ID"
mkdir -p "$DEBUG_DIR" "$MODULE_DIR"

DEBUG_DEST="$DEBUG_DIR/$MODULE.debug"
MODULE_DEST="$MODULE_DIR/$MODULE"

# Only when it differs: a rebuild that did not relink keeps the same build
# id, and re-splitting/re-copying on every incremental build would be noticed
# on a binary this size.
NEED=1
if [ -f "$MODULE_DEST" ]; then
    if [ "$(stat -c%s "$BINARY" 2>/dev/null)" = "$(stat -c%s "$MODULE_DEST" 2>/dev/null)" ]; then
        NEED=0
    fi
fi

if [ "$NEED" = "1" ]; then
    if command -v objcopy >/dev/null 2>&1; then
        # --only-keep-debug then --add-gnu-debuglink is the standard split-debug
        # recipe; the copy left in the archive as "the module" also keeps its
        # full symbol table (no --strip-debug applied to it) so tools/elf_symmap.py
        # can read it even on a machine that has only this half of the archive.
        if objcopy --only-keep-debug "$BINARY" "$DEBUG_DEST" 2>/tmp/.objcopy_err.$$; then
            chmod 644 "$DEBUG_DEST" 2>/dev/null || true
        else
            warn "objcopy --only-keep-debug failed: $(cat /tmp/.objcopy_err.$$ 2>/dev/null)"
        fi
        rm -f /tmp/.objcopy_err.$$
    else
        warn "objcopy not found; no split-debug file archived for build id $BUILD_ID (the module copy below still carries its symbol table)"
    fi
    cp -f "$BINARY" "$MODULE_DEST"
fi

# THE SYMBOL MAP, beside the split-debug file - the same "beside the archived
# PDB" convention archive-symbols.ps1 uses, so a reader who has found one half
# of a build id's archive finds the other beside it on either platform.
MAP_DEST="$DEBUG_DIR/symmap.json.gz"
if [ ! -f "$MAP_DEST" ]; then
    if ! "$PY" "$HERE/elf_symmap.py" --binary "$BINARY" --module "$MODULE" \
            --build-id "$BUILD_ID" --out "$MAP_DEST"; then
        warn "symbol-map export failed for $BUILD_ID - symbolic grouping will fall back to raw offsets for this build"
    fi
fi

# THE HUMAN INDEX - same six tab-separated fields as archive-symbols.ps1's
# index.txt, in the same order, so one line format serves both platforms and
# a build id column search does not care which produced the row. Written
# without a BOM for the same reason as the Windows script: this file is read
# by tools that are not PowerShell.
INDEX_PATH="$ARCHIVE_ROOT/index.txt"
mkdir -p "$ARCHIVE_ROOT"
LINE="$(date '+%Y-%m-%d %H:%M:%S')	$MODULE	$VERSION	$COMMIT	$BUILD_ID	$MODULE.debug"
if [ ! -f "$INDEX_PATH" ] || ! grep -qF "	$BUILD_ID	" "$INDEX_PATH"; then
    printf '%s\r\n' "$LINE" >> "$INDEX_PATH"
fi

echo "archive-symbols-linux: $MODULE $VERSION build $BUILD_ID -> $MODULE_DEST"
exit 0
