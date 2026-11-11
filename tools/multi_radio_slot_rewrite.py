#!/usr/bin/env python3
"""multi_radio_slot_rewrite.py - the MECHANICAL half of multi-radio engine stage 4, reproducible.

Stage 1 left Engine with a reference of the same name bound to slot 0 for each of the 106 members that moved into Receiver
(`float& volume_ = receivers_[0]->volume_;`). Stage 4 makes every Engine function act on "the receiver the calling thread selected" (Engine::rx(), a
thread-local slot, slot 0 when none is selected): this script rewrites each use of such a name, in code (never in a comment or a string, never after
`.`, `->` or `::`), to `rx().<name>`. The reference declarations themselves are left alone (the friends and the tests that name them still read slot 0).

Usage (from the repository root):
    python3 tools/multi_radio_slot_rewrite.py src/engine/engine.cpp src/engine/engine_*.cpp src/engine/engine.hpp

It is IDEMPOTENT (a second run changes nothing) and it is how a branch that adds new uses of the aliases - the AGC and noise-blanker lane's handlers, for
one - is brought onto the slot model after a merge: merge, run this, build.
"""
import re
import sys

HEADER = 'src/engine/engine.hpp'
DECL = re.compile(r'&\s*(\w+)\s*=\s*receivers_\[0\]->\w+')
IDENT = re.compile(r'[A-Za-z_]\w*')
BACKSLASH = chr(92)


def aliases():
    names = set()
    for line in open(HEADER, encoding='utf-8'):
        m = DECL.search(line)
        if m:
            names.add(m.group(1))
    return names


def convert(text, names):
    res = []
    inblock = False
    count = 0
    for line in text.split('\n'):
        if '= receivers_[0]->' in line:
            res.append(line)
            continue
        out = []
        i = 0
        n = len(line)
        while i < n:
            ch = line[i]
            if inblock:
                j = line.find('*/', i)
                if j < 0:
                    out.append(line[i:])
                    i = n
                else:
                    out.append(line[i:j + 2])
                    i = j + 2
                    inblock = False
                continue
            if ch == '/' and i + 1 < n and line[i + 1] == '/':
                out.append(line[i:])
                i = n
                continue
            if ch == '/' and i + 1 < n and line[i + 1] == '*':
                inblock = True
                out.append('/*')
                i += 2
                continue
            if ch in ('"', "'"):
                q = ch
                j = i + 1
                while j < n:
                    if line[j] == BACKSLASH:
                        j += 2
                        continue
                    if line[j] == q:
                        break
                    j += 1
                out.append(line[i:j + 1])
                i = j + 1
                continue
            m = IDENT.match(line, i)
            if m:
                w = m.group(0)
                before = line[max(0, i - 2):i]
                p1 = line[i - 1] if i > 0 else ''
                if w in names and p1 != '.' and not before.endswith('->') and not before.endswith('::'):
                    out.append('rx().' + w)
                    count += 1
                else:
                    out.append(w)
                i = m.end()
                continue
            out.append(ch)
            i += 1
        res.append(''.join(out))
    return '\n'.join(res), count


def main(paths):
    names = aliases()
    total = 0
    for f in paths:
        t = open(f, encoding='utf-8', newline='').read()
        crlf = '\r\n' in t
        if crlf:
            t = t.replace('\r\n', '\n')
        new, c = convert(t, names)
        if c:
            open(f, 'w', encoding='utf-8', newline='').write(new.replace('\n', '\r\n') if crlf else new)
            print(c, f)
            total += c
    print('total', total, 'uses of', len(names), 'names')


if __name__ == '__main__':
    main(sys.argv[1:])
