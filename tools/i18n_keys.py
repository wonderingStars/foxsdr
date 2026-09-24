"""The translation keys: what the source asks for, and what each catalogue has.

    py -3.14 tools/i18n_keys.py                    counts, missing and stale keys
                                                   for every resources/lang/*.json
    py -3.14 tools/i18n_keys.py --list             every key the source uses
    py -3.14 tools/i18n_keys.py --skeleton pt-BR   write resources/lang/pt-BR.json
        [--name "Português (Brasil)" --english-name "Portuguese (Brazil)"]
        [--machine] [--out PATH] [--force]
                                                   a catalogue with every key and
                                                   an empty translation, for a
                                                   translator (or a machine) to fill
    py -3.14 tools/i18n_keys.py --merge pt-BR      add the source's new keys to an
                                                   existing catalogue (empty) and
                                                   drop the ones it no longer uses

Run from the repository root. Exits 1 when any catalogue has missing or stale
keys, so it can gate a script the way test_i18n gates the build.

THE EXTRACTION RULES ARE tests/test_i18n.cpp's, line for line - a change to
one is a change to both. A KEY is a call of tr, trId or FOX_TR_NOOP whose whole
argument is one string literal or several adjacent ones, joined as the
compiler joins them, with C escapes decoded. Comments are skipped; "tr(" inside
a string is not a call. For trId and FOX_TR_NOOP the key is the text before
"##" (the part a widget shows), and an id-only literal is no key.

An empty translation is how a skeleton leaves an entry for someone to fill;
the application shows English for it, but test_i18n refuses a catalogue that
still has one - a shipped catalogue is finished or it is not shipped.
"""
import argparse
import io
import json
import os
import sys

SRC = 'src'
LANG = os.path.join('resources', 'lang')


# --- the lexer (tests/test_i18n.cpp: lex, readQuoted) ------------------------

def is_ident_start(c):
    return c.isascii() and (c.isalpha() or c == '_')


def is_ident_char(c):
    return c.isascii() and (c.isalnum() or c == '_')


def hex_val(c):
    return int(c, 16) if c in '0123456789abcdefABCDEF' else -1


def utf8(cp):
    try:
        return chr(cp).encode('utf-8')
    except (ValueError, OverflowError):
        return b'?'


SIMPLE = {'n': b'\n', 't': b'\t', 'r': b'\r', 'a': b'\a', 'b': b'\b', 'f': b'\f',
          'v': b'\v', '\\': b'\\', "'": b"'", '"': b'"', '?': b'?'}


def read_quoted(src, i, quote):
    """Body of an ordinary literal from src[i] (past the opening quote).
    Returns (bytes, index past the closing quote)."""
    out = bytearray()
    n = len(src)
    while i < n and src[i] != quote:
        c = src[i]
        i += 1
        if c == '\n':
            break
        if c != '\\' or i >= n:
            out += c.encode('utf-8', 'surrogateescape')
            continue
        c = src[i]
        i += 1
        if c in SIMPLE:
            out += SIMPLE[c]
        elif c == '\n':
            pass  # line splice
        elif c == '\r':
            if i < n and src[i] == '\n':
                i += 1
        elif c == 'x':
            v = 0
            while i < n and hex_val(src[i]) >= 0:
                v = v * 16 + hex_val(src[i])
                i += 1
            out.append(v & 0xFF)
        elif c in 'uU':
            count = 4 if c == 'u' else 8
            v = 0
            k = 0
            while k < count and i < n and hex_val(src[i]) >= 0:
                v = v * 16 + hex_val(src[i])
                i += 1
                k += 1
            out += utf8(v)
        elif '0' <= c <= '7':
            v = ord(c) - ord('0')
            k = 0
            while k < 2 and i < n and '0' <= src[i] <= '7':
                v = v * 8 + ord(src[i]) - ord('0')
                i += 1
                k += 1
            out.append(v & 0xFF)
        else:
            out += c.encode('utf-8', 'surrogateescape')
    if i < n and src[i] == quote:
        i += 1
    return bytes(out), i


def lex(src):
    toks = []
    i = 0
    n = len(src)
    while i < n:
        c = src[i]
        if c == '/' and i + 1 < n and src[i + 1] == '/':
            while i < n and src[i] != '\n':
                if src[i] == '\\' and i + 1 < n and src[i + 1] == '\n':
                    i += 1
                i += 1
            continue
        if c == '/' and i + 1 < n and src[i + 1] == '*':
            end = src.find('*/', i + 2)
            i = n if end < 0 else end + 2
            continue
        if c == '"':
            body, i = read_quoted(src, i + 1, '"')
            toks.append(('S', body))
            continue
        if c == "'":
            _, i = read_quoted(src, i + 1, "'")
            toks.append(('P', "'"))
            continue
        if c.isdigit() and c.isascii() or (c == '.' and i + 1 < n and src[i + 1].isdigit()):
            i += 1
            while i < n:
                d = src[i]
                if d in '+-' and src[i - 1] in 'eEpP':
                    i += 1
                elif is_ident_char(d) or d == '.' or (d == "'" and i + 1 < n and is_ident_char(src[i + 1])):
                    i += 1
                else:
                    break
            continue
        if is_ident_start(c):
            start = i
            while i < n and is_ident_char(src[i]):
                i += 1
            ident = src[start:i]
            if i < n and src[i] == '"':
                if ident in ('R', 'LR', 'uR', 'UR', 'u8R'):
                    op = src.find('(', i + 1)
                    if op < 0:
                        break
                    delim = src[i + 1:op]
                    close = ')' + delim + '"'
                    end = src.find(close, op + 1)
                    stop = n if end < 0 else end
                    toks.append(('S', src[op + 1:stop].encode('utf-8', 'surrogateescape')))
                    i = n if end < 0 else end + len(close)
                    continue
                if ident in ('L', 'u', 'U', 'u8'):
                    body, i = read_quoted(src, i + 1, '"')
                    toks.append(('S', body))
                    continue
            toks.append(('I', ident))
            continue
        if c.isspace():
            i += 1
            continue
        toks.append(('P', c))
        i += 1
    return toks


def extract_keys(src):
    """Keys used by one source text, in order of appearance (as str)."""
    keys = []
    t = lex(src)
    for k in range(len(t)):
        kind, text = t[k]
        if kind != 'I' or text not in ('tr', 'trId', 'FOX_TR_NOOP'):
            continue
        if k + 1 >= len(t) or t[k + 1] != ('P', '('):
            continue
        j = k + 2
        joined = b''
        any_lit = False
        while j < len(t) and t[j][0] == 'S':
            joined += t[j][1]
            any_lit = True
            j += 1
        if not any_lit or j >= len(t) or t[j] != ('P', ')'):
            continue
        if text != 'tr':
            h = joined.find(b'##')
            if h >= 0:
                joined = joined[:h]
        if joined:
            keys.append(joined.decode('utf-8', 'replace'))
    return keys


def source_keys(root=SRC):
    keys = set()
    for dirpath, _, files in os.walk(root):
        for f in files:
            if os.path.splitext(f)[1] not in ('.cpp', '.hpp', '.h'):
                continue
            with open(os.path.join(dirpath, f), 'rb') as fh:
                # Read as UTF-8 with surrogateescape so a stray byte survives
                # into the key exactly as the C++ reader sees it.
                text = fh.read().decode('utf-8', 'surrogateescape')
            keys.update(extract_keys(text))
    return keys


# --- catalogues --------------------------------------------------------------

def catalogue_paths():
    if not os.path.isdir(LANG):
        return []
    return [os.path.join(LANG, f) for f in sorted(os.listdir(LANG)) if f.endswith('.json')]


def load(path):
    with io.open(path, 'r', encoding='utf-8') as f:
        return json.load(f)


def save(path, doc):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    # UTF-8 without a BOM, LF, keys in a stable order so a diff shows what
    # changed and nothing else.
    text = json.dumps(doc, ensure_ascii=False, indent=2) + '\n'
    with io.open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


def report(used):
    bad = False
    paths = catalogue_paths()
    print('source: %d keys' % len(used))
    if not paths:
        print('no catalogues in %s' % LANG)
    for path in paths:
        doc = load(path)
        have = doc.get('strings', {})
        missing = sorted(k for k in used if k not in have)
        stale = sorted(k for k in have if k not in used)
        empty = sorted(k for k, v in have.items() if k in used and not v)
        print('%s: %d translated, %d empty, %d missing, %d stale'
              % (os.path.basename(path), len(have) - len(empty) - len(stale), len(empty),
                 len(missing), len(stale)))
        for k in missing:
            print('  missing: %s' % json.dumps(k, ensure_ascii=False))
        for k in stale:
            print('  stale:   %s' % json.dumps(k, ensure_ascii=False))
        for k in empty:
            print('  empty:   %s' % json.dumps(k, ensure_ascii=False))
        bad = bad or bool(missing or stale or empty)
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--list', action='store_true', help='print every key the source uses')
    ap.add_argument('--skeleton', metavar='CODE', help='write a catalogue of empty entries')
    ap.add_argument('--merge', metavar='CODE', help='bring an existing catalogue up to the source')
    ap.add_argument('--name', help='the language in its own language (skeleton)')
    ap.add_argument('--english-name', help='the language in English (skeleton)')
    ap.add_argument('--machine', action='store_true', help='mark as machine-translated (skeleton)')
    ap.add_argument('--out', help='where to write (default resources/lang/<code>.json)')
    ap.add_argument('--force', action='store_true', help='overwrite an existing file (skeleton)')
    args = ap.parse_args()
    # Keys carry accented letters (country names already do); a Windows
    # console's code page would print them as '?'.
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8')

    if not os.path.isdir(SRC) or not os.path.isdir('resources'):
        sys.stderr.write('run me from the repository root\n')
        return 2
    used = source_keys()

    if args.list:
        for k in sorted(used):
            print(json.dumps(k, ensure_ascii=False))
        return 0

    if args.skeleton:
        out = args.out or os.path.join(LANG, args.skeleton + '.json')
        if os.path.exists(out) and not args.force:
            sys.stderr.write('%s exists; use --merge to update it, or --force\n' % out)
            return 2
        doc = {
            'code': args.skeleton,
            'name': args.name or '',
            'englishName': args.english_name or '',
            'machine': bool(args.machine),
            'strings': {k: '' for k in sorted(used)},
        }
        save(out, doc)
        print('%s: %d keys, all empty' % (out, len(used)))
        if not doc['name'] or not doc['englishName']:
            print('  fill in "name" and "englishName" too')
        return 0

    if args.merge:
        path = args.out or os.path.join(LANG, args.merge + '.json')
        doc = load(path)
        have = doc.get('strings', {})
        merged = {k: have.get(k, '') for k in sorted(used)}
        added = sum(1 for k in used if k not in have)
        dropped = sum(1 for k in have if k not in used)
        doc['strings'] = merged
        save(path, doc)
        print('%s: %d keys added (empty), %d stale keys dropped' % (path, added, dropped))
        print('  re-run: py -3.14 tools/embed-lang.py')
        return 0

    return report(used)


if __name__ == '__main__':
    sys.exit(main())
