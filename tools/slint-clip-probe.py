#!/usr/bin/env python3
"""The clip probe's source transform (docs/engine-stage4.md 6.62).

    python -I tools/slint-clip-probe.py <ui_dir> <out_dir>

Copies every .slint file of the Slint window's ui/ directory (not specimens/) to <out_dir> with ONE change: every `Text { ... }` element gets an `accessible-description` that says what the
text is and how much room it has,

    CLIPPROBE |~| <file>:<line> |~| <text> |~| <box width> |~| <natural width> |~| <box height> |~| <natural height> |~| <elide 0/1> |~| <wrap 0/1> |~| <x> |~| <y> |~| <visible 0/1> |~| <parent x> |~| <parent width> |~|END

evaluated when it is READ, so it is the settled layout's answer and not a transient of the window's first frames (a first design printed from `init` and `changed` handlers and reported a Key's
label at 11 px wide that is 53 px wide once laid out; and Slint's debug() had no stderr in the Windows-subsystem window at all). The natural width is Text.preferred-width - the width the
words ask for on one line - and the natural height is Text.preferred-height at the box's width. foxsdr-slint-probe (main.cpp, built with FOXSDR_SLINT_PROBE) reads every element's description
through the testing API at the end of a bounded run and prints the CLIPPROBE ones; tools/slint_clip_sweep.py judges them. A Text whose accessible-role was `none` becomes `text` in the copy so the
testing API visits it. Nothing else is changed: the line numbers of the original are kept (the description is inserted on the line of the `Text {`), relative paths of fonts and images are made
absolute so the copy compiles from any directory, and the original files are never touched.
"""
import re
import sys
from pathlib import Path

TEXT_OPEN = re.compile(r'(?<![\w-])Text\s*\{')
FILE_IMPORT = re.compile(r'(^import\s+")([^"]+)(";)', re.MULTILINE)       # import "font.ttf";
IMAGE_URL = re.compile(r'(@image-url\(")([^"]+)("\))')
ROLE_NONE = re.compile(r'accessible-role\s*:\s*none\s*;')


def description(where: str, root: bool = False) -> str:
    # a component that INHERITS Text is itself the Text: it has no parent to ask about
    parent_x, parent_w = ('0', '0') if root else ('(parent.absolute-position.x / 1px)', '(parent.width / 1px)')
    return (
        'accessible-description: "CLIPPROBE |~| %s |~| " + self.text + " |~| " + (self.width / 1px) + " |~| " + (self.preferred-width / 1px) + " |~| " + (self.height / 1px) + '
        '" |~| " + (self.preferred-height / 1px) + " |~| " + (self.overflow == TextOverflow.elide ? "1" : "0") + " |~| " + (self.wrap != TextWrap.no-wrap ? "1" : "0") + '
        '" |~| " + (self.absolute-position.x / 1px) + " |~| " + (self.absolute-position.y / 1px) + " |~| " + (self.visible ? "1" : "0") + " |~| " + %s + " |~| " + %s + " |~|END"; ' % (where, parent_x, parent_w)
    )


def block_end(text: str, start: int) -> int:
    """The index of the `}` that closes the block whose `{` is just before `start` (strings and comments skipped); len(text) when the file ends first."""
    depth, i, n = 1, start, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            i += 1
            while i < n and text[i] != '"':
                i += 2 if text[i] == '\\' else 1
        elif text.startswith('//', i):
            j = text.find('\n', i)
            i = n if j < 0 else j
        elif text.startswith('/*', i):
            j = text.find('*/', i)
            i = n if j < 0 else j + 1
        elif c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return n


def absolutise(text: str, src_dir: Path) -> str:
    def resolve(m):
        p = (src_dir / m.group(2)).resolve()
        return m.group(1) + (p.as_posix() if p.exists() else m.group(2)) + m.group(3)

    # a .slint import is a component import (kept relative: the copy sits beside its siblings); any other file is a font
    def file_import(m):
        return m.group(0) if m.group(2).endswith('.slint') else resolve(m)

    return IMAGE_URL.sub(resolve, FILE_IMPORT.sub(file_import, text))


def transform(path: Path, out_dir: Path):
    """Writes the copy of `path`; returns (Text elements found, Text elements instrumented)."""
    original = path.read_text(encoding='utf-8')
    edits = []   # (offset, length to remove, text to put there)
    found = done = 0
    for m in TEXT_OPEN.finditer(original):
        found += 1
        start = m.end()
        end = block_end(original, start)
        block = original[start:end]
        if 'accessible-description' in block:
            continue   # a Text that already says something there keeps it (none does today; the test counts)
        line = original.count('\n', 0, m.start()) + 1
        is_root = original[:m.start()].rstrip().endswith('inherits')
        edits.append((start, 0, ' ' + description('%s:%d' % (path.name, line), is_root)))
        for r in ROLE_NONE.finditer(block):
            edits.append((start + r.start(), r.end() - r.start(), 'accessible-role: AccessibleRole.text;'))
        done += 1
    text = original
    for offset, length, add in sorted(edits, key=lambda e: -e[0]):
        text = text[:offset] + add + text[offset + length:]
    text = absolutise(text, path.parent)
    (out_dir / path.name).write_text(text, encoding='utf-8', newline='\n')
    return found, done


def main(argv) -> int:
    if len(argv) != 3:
        print(__doc__)
        return 2
    ui_dir, out_dir = Path(argv[1]), Path(argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)
    found = done = 0
    for path in sorted(ui_dir.glob('*.slint')):
        f, d = transform(path, out_dir)
        found += f
        done += d
    print('slint-clip-probe: %d of %d Text elements instrumented in %s' % (done, found, out_dir))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
