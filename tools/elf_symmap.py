#!/usr/bin/env python3
"""Exports a compact RVA -> function-name map from an ELF binary, for the
crash dashboard's SYMBOLIC GROUPING (foxsdrWebsite, crash.go) - the Linux
counterpart of tools/export-symbol-map.py, which does the same job from a PDB.

See that script's header for the full rationale (a report's grouping
signature is built from module+offset, and offsets into code we do not ship
identify the victim's patch level rather than the bug; the server therefore
groups by the nearest frame in OUR OWN code, by name, which needs a name
table keyed by the same build id every frame quotes). Nothing here repeats
that argument; this file only covers what is different for an ELF binary:

  - the build id is the GNU build-id note (NT_GNU_BUILD_ID), not a CodeView
    RSDS record - see src/core/diag_report.cpp's gnuBuildIdFromNote(), which
    reads the identical note out of the RUNNING image so the two agree by
    construction;
  - the function table comes from `nm`, not dbghelp's SymEnumSymbols, and
    needs no debug info at all - a normal (non-`-s`-linked) binary keeps its
    .symtab, which is exactly the table this reads. A binary built with
    frame-pointer-friendly optimisation still exports every non-inlined,
    non-static-and-discarded C++ function by its mangled name; `nm -C`
    demangles the same way SymFromAddr's SYMOPT_UNDNAME does on Windows;
  - the addresses `nm` reports for a PIE executable (the default on a modern
    distribution) are ALREADY relative to the module's own load bias - the
    same quantity a Windows RVA is relative to the image base - so no
    "subtract the base" step is needed the way it would be for a non-PIE
    binary inspected as a live process. This tool refuses a non-PIE input
    (see below) rather than silently producing a table an ASLR'd load can
    never match.

The output schema is BYTE-FOR-BYTE what export-symbol-map.py writes -
{"schema":1,"module":...,"buildId":...,"functions":[[rva,size,name],...]},
gzipped - so nothing on the server that reads a symmap.json.gz needs to know
which platform produced it.

SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""

import argparse
import gzip
import json
import os
import re
import shutil
import struct
import subprocess
import sys

NT_GNU_BUILD_ID = 3


def read_gnu_build_id(binary_path: str) -> str:
    """The build id exactly as diag_report.cpp's gnuBuildIdFromNote() reads
    it: the NT_GNU_BUILD_ID note's descriptor, hex-encoded lowercase. Parsed
    from the ELF section headers directly (no readelf dependency) so this
    tool has exactly one external-tool dependency (nm) rather than two."""
    with open(binary_path, "rb") as fh:
        data = fh.read()
    if len(data) < 64 or data[:4] != b"\x7fELF":
        raise ValueError(f"{binary_path} is not an ELF file")
    ei_class = data[4]
    if ei_class != 2:
        raise ValueError(f"{binary_path} is not a 64-bit ELF file")
    little = data[5] == 1
    end = "<" if little else ">"
    # e_shoff(Q)@0x28, e_shentsize(H)@0x3A, e_shnum(H)@0x3C, e_shstrndx(H)@0x3E
    e_shoff, = struct.unpack_from(end + "Q", data, 0x28)
    e_shentsize, = struct.unpack_from(end + "H", data, 0x3A)
    e_shnum, = struct.unpack_from(end + "H", data, 0x3C)
    e_shstrndx, = struct.unpack_from(end + "H", data, 0x3E)
    if e_shoff == 0 or e_shnum == 0:
        raise ValueError(f"{binary_path} carries no section headers")

    def section(i):
        off = e_shoff + i * e_shentsize
        # Elf64_Shdr: name(I) type(I) flags(Q) addr(Q) offset(Q) size(Q) ...
        name_off, sh_type, _flags, _addr, sh_offset, sh_size = struct.unpack_from(
            end + "IIQQQQ", data, off)
        return name_off, sh_type, sh_offset, sh_size

    strtab_name_off, _t, strtab_off, strtab_size = section(e_shstrndx)

    def shname(name_off):
        end_ix = data.index(b"\x00", strtab_off + name_off)
        return data[strtab_off + name_off:end_ix].decode("ascii", "replace")

    for i in range(e_shnum):
        name_off, sh_type, sh_offset, sh_size = section(i)
        if sh_type != 7:  # SHT_NOTE
            continue
        if shname(name_off) not in (".note.gnu.build-id", ".notes", ".note"):
            continue
        buf = data[sh_offset:sh_offset + sh_size]
        pos = 0
        while pos + 12 <= len(buf):
            namesz, descsz, ntype = struct.unpack_from(end + "III", buf, pos)
            pos += 12
            name_end = pos + namesz
            pos_aligned_name = (namesz + 3) & ~3
            pos_aligned_desc = (descsz + 3) & ~3
            if ntype == NT_GNU_BUILD_ID and buf[pos:name_end] == b"GNU\x00":
                desc_off = pos + pos_aligned_name
                desc = buf[desc_off:desc_off + descsz]
                return desc.hex()
            pos += pos_aligned_name + pos_aligned_desc
    raise ValueError(f"{binary_path} carries no NT_GNU_BUILD_ID note - was it "
                      f"linked with a linker that emits one? (GNU ld and lld "
                      f"both do by default)")


def is_pie(binary_path: str) -> bool:
    with open(binary_path, "rb") as fh:
        data = fh.read(20)
    # e_type is a 2-byte field at offset 0x10; ET_DYN (3) is what a PIE
    # executable AND a plain shared library both report - the distinction
    # this tool cares about (an address in the symbol table already being
    # bias-relative) holds for both, so no further check is needed.
    e_type, = struct.unpack_from("<H" if data[5] == 1 else ">H", data, 0x10)
    return e_type == 3


def export(binary_path: str, module: str, build_id: str, out_path: str) -> int:
    nm = shutil.which("nm") or "nm"
    try:
        proc = subprocess.run(
            [nm, "-C", "--defined-only", "-S", "--size-sort", binary_path],
            capture_output=True, text=True, check=True)
    except (subprocess.CalledProcessError, FileNotFoundError) as exc:
        print(f"elf_symmap: nm failed on {binary_path}: {exc}", file=sys.stderr)
        return 1

    # "<addr> <size> <type> <name>", type upper/lower-case T is a text
    # (function) symbol in either the global or local binding - the same
    # "functions only" filter export-symbol-map.py applies via SYM_TAG_FUNCTION.
    line_re = re.compile(r"^([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+([TtWw])\s+(.+)$")
    functions = []
    for line in proc.stdout.splitlines():
        m = line_re.match(line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        size = int(m.group(2), 16)
        name = m.group(4).strip()
        if not name:
            continue
        functions.append((addr, size, name))

    if not functions:
        print(f"elf_symmap: {binary_path} yielded no function symbols - is it "
              f"stripped? (a normal, non `-s`-linked build keeps .symtab)",
              file=sys.stderr)
        return 1

    functions.sort(key=lambda f: f[0])
    deduped = []
    last_addr = -1
    for f in functions:
        if f[0] == last_addr:
            continue
        deduped.append(f)
        last_addr = f[0]

    doc = {
        "schema": 1,
        "module": module,
        "buildId": build_id,
        "functions": [[addr, size, name] for (addr, size, name) in deduped],
    }
    tmp = out_path + ".tmp"
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with gzip.open(tmp, "wt", encoding="utf-8") as fh:
        json.dump(doc, fh, separators=(",", ":"), ensure_ascii=False)
    os.replace(tmp, out_path)
    print(f"elf_symmap: {module} {build_id}: {len(deduped)} functions -> {out_path}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", required=True, help="path to the ELF binary to export")
    ap.add_argument("--module", default="cascade",
                    help="module name reports will quote (default cascade)")
    ap.add_argument("--build-id", default=None,
                    help="the GNU build id; read from the binary's own "
                         "NT_GNU_BUILD_ID note when omitted")
    ap.add_argument("--out", required=True, help="output .json.gz path")
    args = ap.parse_args()

    if not os.path.isfile(args.binary):
        print(f"elf_symmap: no such file: {args.binary}", file=sys.stderr)
        return 1
    if not is_pie(args.binary):
        # Not a hard requirement of the format, but every binary this build
        # produces is PIE by default (modern GCC/Ubuntu), and a non-PIE input
        # would mean the addresses below are ABSOLUTE, not bias-relative -
        # silently producing a table that resolves nothing once ASLR is
        # applied to a real crash's addresses. Loud rather than wrong.
        print(f"elf_symmap: {args.binary} is not position-independent (ET_EXEC, "
              f"not ET_DYN) - its symbol addresses are absolute, not RVAs, and "
              f"this tool does not know the load bias to subtract", file=sys.stderr)
        return 1

    build_id = args.build_id
    if not build_id:
        try:
            build_id = read_gnu_build_id(args.binary)
        except ValueError as exc:
            print(f"elf_symmap: {exc}", file=sys.stderr)
            return 1

    return export(args.binary, args.module, build_id, args.out)


if __name__ == "__main__":
    sys.exit(main())
