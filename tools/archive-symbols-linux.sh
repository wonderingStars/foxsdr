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
#   - there is no PDB. The DWARF is IN the binary the linker produced, provided
#     the compiler was given -g: CMakeLists.txt adds it to the Release flags on
#     Linux, and until it did NOTHING here had a line table to keep - every
#     archived cascade.debug before that change held a symbol table and no
#     .debug_* section at all (measured with readelf), so a Linux frame could
#     be named but never placed on a line. So the archive keeps TWO things
#     instead of a matched exe+pdb pair: a split-debug file (the debug info
#     alone, via `objcopy --only-keep-debug`, sections compressed) under
#         <root>/<module>.debug/<build-id>/<module>.debug
#     and the SHIPPED binary - the same one with its DWARF removed - under
#         <root>/<module>/<build-id>/<module>
#     - the exact two paths src/core/report_reader.hpp's
#     SymbolArchive::elfSymbolPath() already looks for, in that priority
#     order (split debug first, because it is the one with line tables).
#   - the DWARF is removed from the binary in the build tree, in place, once
#     the split file is safely written, because that binary is what the CI
#     tarball and the AppImage copy: left in, a full-DWARF cascade would be
#     tens of megabytes heavier for every user. Only debug information goes
#     (`--strip-debug`); the function symbol table stays, so
#     tools/elf_symmap.py and the crash handler see the binary they always
#     saw. The build id lives in a note section neither step touches, so it is
#     the same before and after, and so is the archive key. A Debug or
#     RelWithDebInfo build is left alone (see --config): that binary is a
#     developer's, and is for a debugger.
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
CONFIG=""

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
        # The CMake configuration ($<CONFIG>). Absent or empty means a shipping
        # build: the DWARF is taken out of the binary after it is archived.
        --config) CONFIG="$2"; shift 2 ;;
        *) warn "unknown argument: $1"; shift ;;
    esac
done

if [ -z "$COMMIT" ] && [ -n "$COMMIT_HEADER" ] && [ -f "$COMMIT_HEADER" ]; then
    COMMIT="$(sed -n 's/.*CASCADE_GIT_COMMIT[^"]*"\([^"]*\)".*/\1/p' "$COMMIT_HEADER" | head -1)"
fi

if [ -z "$BINARY" ] || [ -z "$ARCHIVE_ROOT" ]; then
    warn "usage: --binary PATH --archive-root DIR [--version V] [--commit C] [--module NAME] [--config CONFIG]"
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
        # --only-keep-debug, then --strip-debug, then --add-gnu-debuglink is the
        # order binutils' own documentation gives for a split-debug file, and
        # the order matters: the strip must come AFTER the debug file has been
        # written and checked, or a failed split would leave nothing anywhere.
        #
        # The split file is written with its sections compressed
        # (--compress-debug-sections=zlib, the value every binutils with the
        # option accepts): the DWARF for this program is tens of megabytes
        # even so, and several times that left uncompressed. binutils' addr2line
        # reads compressed sections, which is what the report reader drives. If
        # this objcopy refuses the option the file is written plain rather
        # than not at all.
        ERR_FILE="/tmp/.objcopy_err.$$"
        SPLIT_OK=0
        if objcopy --only-keep-debug --compress-debug-sections=zlib "$BINARY" "$DEBUG_DEST" 2>"$ERR_FILE"; then
            SPLIT_OK=1
        else
            FIRST_ERR="$(cat "$ERR_FILE" 2>/dev/null)"
            if objcopy --only-keep-debug "$BINARY" "$DEBUG_DEST" 2>"$ERR_FILE"; then
                warn "objcopy refused --compress-debug-sections=zlib ($FIRST_ERR); the split-debug file is archived uncompressed"
                SPLIT_OK=1
            else
                warn "objcopy --only-keep-debug failed: $(cat "$ERR_FILE" 2>/dev/null)"
                # A half-written file here would be picked up by the reader as
                # though it were the archive for this build id.
                rm -f "$DEBUG_DEST"
            fi
        fi
        rm -f "$ERR_FILE"

        if [ "$SPLIT_OK" = "1" ] && [ -s "$DEBUG_DEST" ]; then
            chmod 644 "$DEBUG_DEST" 2>/dev/null || true
            # Take the DWARF out of the binary that ships, now that it is saved.
            # The symbol table stays (--strip-debug, not --strip-all), and the
            # debuglink names the split file so gdb can find it from the binary.
            case "$CONFIG" in
                Debug|RelWithDebInfo) : ;;
                *)
                    if ! objcopy --strip-debug --add-gnu-debuglink="$DEBUG_DEST" "$BINARY" 2>"$ERR_FILE"; then
                        warn "could not remove the DWARF from $BINARY ($(cat "$ERR_FILE" 2>/dev/null)); the shipped binary still carries it"
                    fi
                    rm -f "$ERR_FILE"
                    ;;
            esac
        fi
    else
        warn "objcopy not found; no split-debug file archived for build id $BUILD_ID (the module copy below still carries its symbol table)"
    fi
    cp -f "$BINARY" "$MODULE_DEST"
fi

# DID THE ARCHIVED FILE ACTUALLY GET LINE TABLES? Looked at every run, not only
# when this run split the file: an archive that names functions but places none
# on a line is the failure that went unnoticed until a crash needed it, and the
# only way to notice it earlier is for the build to say so. Never fatal, like
# everything here.
LINE_TABLES="unchecked"
if [ -s "$DEBUG_DEST" ] && command -v readelf >/dev/null 2>&1; then
    if readelf -S -W "$DEBUG_DEST" 2>/dev/null | grep -q '\.debug_line'; then
        LINE_TABLES="yes"
    else
        LINE_TABLES="NO"
        warn "NO LINE TABLES in $DEBUG_DEST - was $BINARY compiled without -g? A report against build id $BUILD_ID will name functions only."
    fi
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

echo "archive-symbols-linux: $MODULE $VERSION build $BUILD_ID -> $MODULE_DEST (line tables: $LINE_TABLES)"
exit 0
