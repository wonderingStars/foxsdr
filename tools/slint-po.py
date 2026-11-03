"""Turn resources/lang/*.json into gettext .po files for the Slint window's bundled translations.

WHY THIS EXISTS. The ImGui window answers tr("English text") from the catalogues in resources/lang
(core/i18n.hpp: the key IS the English text, 33 languages, no en.json). The Slint window marks its words
with @tr("English text") and the Slint compiler can bundle translations into the executable from
gettext files laid out as <dir>/<lang>/LC_MESSAGES/<domain>.po (slint-compiler --bundle-translations).
This script is the bridge, so ONE set of catalogues serves both windows and a translator edits one file:
for every <code>.json it writes <out>/<code>/LC_MESSAGES/foxsdr-slint.po.

WHAT IS EXTRACTED. Every @tr(...) under the ui directory, except specimens/ (the test sheets), read with
a small lexer that skips comments and string literals so an "@tr" inside either is not found:
  * the plain form        @tr("Fitted modules")                     -> msgid
  * the argument form     @tr("{} unread", n)                       -> msgid with {} kept as written
  * the plural form       @tr("{} file" | "{} files" % n)           -> msgid + msgid_plural
  * the context form      @tr("ctx" => "text") - MUST NOT EXIST (the build passes
    --no-default-translation-context and nothing here gives a word a context); one is warned about on
    stderr and written with its msgctxt all the same, so it still works.

WHAT IS WRITTEN. Only a msgid that HAS a value in the catalogue. A msgid with none is left out and Slint
shows the source string, which is exactly what the ImGui window does for a missing key (English is the
source; there is no English .po). A value is found two ways, counted separately in the summary:
  1. exact: the catalogue key is the msgid, character for character;
  2. by placeholder: Slint writes "{} unread", the ImGui window's key is "%d unread". Each {} of the Slint
     string is matched against a %s / %d (also %i %u %ld %lld %zu) at the same place of a catalogue key,
     and the translation's conversions are turned back into {} in order. The catalogues hold the same
     conversions in the same order as their keys (test_i18n enforces it), so the order is safe. A key with
     any other conversion (%.1f, %5d) never matches: {} cannot carry a format. Numbered placeholders ({0})
     are not matched by placeholder, only exactly.
A literal brace in a translation is doubled ({{ }}) because Slint's formatter reads one as a placeholder.
A PLURAL needs both forms translated and a language with one or two plural forms: the catalogues hold a
singular and a plural sentence, which cannot fill the three or four forms of Russian, Polish, Czech and
the rest - a guessed form is worse grammar than English, so those entries are left out (counted).

THE PLURAL-FORMS HEADER is a small table for the 33 codes below (the gettext project's rules for each
language); a code not in it gets the gettext default, "nplurals=2; plural=(n != 1);", and a warning.

DETERMINISM. No dates, no source line references (they would move with every edit of a .slint file and
rebuild the translations for nothing), entries sorted by msgid, UTF-8 and \\n line ends. A .po is
rewritten only when its bytes change. With --stamp FILE the stamp is touched only when something was
written (or it does not exist yet): CMake makes the Slint compiler depend on it, so a catalogue edit that
changes no foxsdr-slint string does not recompile the window.

Run from the repository root:
    py -3.14 tools/slint-po.py src/slint/ui resources/lang build/src/slint/translations
Options: --stamp FILE, --missing CODE (list the msgids that code has no value for), --quiet.
The tests: py -3.14 -I tools/test_slint_po.py
"""
import argparse
import json
import os
import re
import sys

DOMAIN = 'foxsdr-slint'

# gettext's Plural-Forms for the 33 catalogues in resources/lang (the CLDR/gettext rules).
_CS = ('3', '(n==1) ? 0 : (n>=2 && n<=4) ? 1 : 2')
_RU = ('3', 'n%10==1 && n%100!=11 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2')
_ONE = ('1', '0')
_NE1 = ('2', 'n != 1')
_GT1 = ('2', 'n > 1')
PLURALS = {
    'bg': _NE1, 'ca': _NE1, 'cs': _CS, 'da': _NE1, 'de': _NE1, 'el': _NE1, 'es': _NE1, 'et': _NE1,
    'fi': _NE1, 'fr': _GT1, 'hr': _RU, 'hu': _NE1, 'id': _ONE, 'it': _NE1, 'ja': _ONE, 'ko': _ONE,
    'lt': ('3', 'n%10==1 && (n%100<11 || n%100>19) ? 0 : n%10>=2 && n%10<=9 && (n%100<11 || n%100>19) ? 1 : 2'),
    'lv': ('3', 'n%10==0 || (n%100>=11 && n%100<=19) ? 0 : n%10==1 && n%100!=11 ? 1 : 2'),
    'nb': _NE1, 'nl': _NE1,
    'pl': ('3', 'n==1 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2'),
    'pt-BR': _GT1, 'pt-PT': _NE1,
    'ro': ('3', 'n==1 ? 0 : (n==0 || (n%100 > 0 && n%100 < 20)) ? 1 : 2'),
    'ru': _RU, 'sk': _CS,
    'sl': ('4', 'n%100==1 ? 0 : n%100==2 ? 1 : n%100==3 || n%100==4 ? 2 : 3'),
    'sv': _NE1, 'tr': _GT1, 'uk': _RU, 'vi': _ONE, 'zh-CN': _ONE, 'zh-TW': _ONE,
}
DEFAULT_PLURAL = _NE1


# --- Extraction ------------------------------------------------------------------------------------

class Message(object):
    """One @tr(): msgid, optional plural and context, where it was found."""

    def __init__(self, msgid, plural, ctx, where):
        self.msgid = msgid
        self.plural = plural
        self.ctx = ctx
        self.where = where


class Lexer(object):
    def __init__(self, text, name, warn):
        self.t = text
        self.name = name
        self.warn = warn
        self.i = 0
        self.n = len(text)

    def line(self, i=None):
        return self.t.count('\n', 0, self.i if i is None else i) + 1

    def skip_ws(self):
        """Whitespace and comments."""
        t, n = self.t, self.n
        while self.i < n:
            c = t[self.i]
            if c in ' \t\r\n':
                self.i += 1
            elif t.startswith('//', self.i):
                j = t.find('\n', self.i)
                self.i = n if j < 0 else j
            elif t.startswith('/*', self.i):
                j = t.find('*/', self.i + 2)
                self.i = n if j < 0 else j + 2
            else:
                break

    def skip_string(self):
        """Step over a string literal that starts at self.i (a quote), interpolations included."""
        t, n = self.t, self.n
        self.i += 1
        while self.i < n:
            c = t[self.i]
            if c == '\\':
                if t.startswith('\\{', self.i):
                    self.i += 2
                    depth = 1
                    while self.i < n and depth:
                        d = t[self.i]
                        if d == '"':
                            self.skip_string()
                            continue
                        if d == '{':
                            depth += 1
                        elif d == '}':
                            depth -= 1
                        self.i += 1
                    continue
                self.i += 2
                continue
            if c == '"':
                self.i += 1
                return
            self.i += 1

    def read_string(self):
        """Parse the string literal at self.i into its value; None (position unmoved) if there is none
        or it interpolates."""
        t, n = self.t, self.n
        if self.i >= n or t[self.i] != '"':
            return None
        start = self.i
        j = self.i + 1
        out = []
        while j < n:
            c = t[j]
            if c == '"':
                self.i = j + 1
                return ''.join(out)
            if c != '\\':
                out.append(c)
                j += 1
                continue
            e = t[j + 1] if j + 1 < n else ''
            if e == 'n':
                out.append('\n')
            elif e == 't':
                out.append('\t')
            elif e == 'r':
                out.append('\r')
            elif e in ('"', '\\'):
                out.append(e)
            elif e == 'u' and t.startswith('{', j + 2):
                k = t.find('}', j + 3)
                if k < 0:
                    break
                out.append(chr(int(t[j + 3:k], 16)))
                j = k + 1
                continue
            else:
                # \{ (interpolation) or an escape this lexer does not know: not a literal msgid
                self.warn('%s:%d: @tr text with the escape \\%s is not extracted' % (self.name, self.line(start), e))
                self.i = start
                return None
            j += 2
        self.i = start
        return None

    def messages(self):
        t, n = self.t, self.n
        found = []
        while self.i < n:
            c = t[self.i]
            if c == '"':
                self.skip_string()
            elif t.startswith('//', self.i) or t.startswith('/*', self.i):
                self.skip_ws()
            elif t.startswith('@tr', self.i) and not (self.i + 3 < n and (t[self.i + 3].isalnum() or t[self.i + 3] in '_-')):
                at = self.i
                self.i += 3
                m = self.after_tr(at)
                if m is not None:
                    found.append(m)
            else:
                self.i += 1
        return found

    def after_tr(self, at):
        where = '%s:%d' % (self.name, self.line(at))
        self.skip_ws()
        if self.i >= self.n or self.t[self.i] != '(':
            self.warn('%s: @tr without ( - skipped' % where)
            return None
        self.i += 1
        self.skip_ws()
        first = self.read_string()
        if first is None:
            self.warn('%s: @tr without a plain string literal first - skipped' % where)
            return None
        self.skip_ws()
        ctx = None
        plural = None
        msgid = first
        if self.t.startswith('=>', self.i):
            self.i += 2
            self.skip_ws()
            ctx = first
            msgid = self.read_string()
            if msgid is None:
                self.warn('%s: @tr context without a text - skipped' % where)
                return None
            self.skip_ws()
            self.warn('%s: @tr with a context (%r): the house rule is none; written with its msgctxt' % (where, ctx))
        if self.t.startswith('|', self.i):
            self.i += 1
            self.skip_ws()
            plural = self.read_string()
            if plural is None:
                self.warn('%s: @tr plural form is not a plain string - skipped' % where)
                return None
        if re.search(r'\{\d+\}', msgid) or (plural is not None and re.search(r'\{\d+\}', plural)):
            self.warn('%s: @tr with numbered placeholders: matched exactly only, never by placeholder' % where)
        return Message(msgid, plural, ctx, where)


def extract(ui_dir, warn):
    """Every Message under ui_dir/**.slint except specimens/, in a stable order."""
    files = []
    for root, dirs, names in os.walk(ui_dir):
        dirs[:] = sorted(d for d in dirs if d != 'specimens')
        for f in sorted(names):
            if f.endswith('.slint'):
                files.append(os.path.join(root, f))
    messages = []
    for path in files:
        with open(path, 'r', encoding='utf-8', newline='') as fh:
            text = fh.read()
        rel = os.path.relpath(path, ui_dir).replace(os.sep, '/')
        messages.extend(Lexer(text, rel, warn).messages())
    return files, messages


def unique(messages, warn):
    """One entry per (ctx, msgid); a msgid used with two different plurals is a conflict (first wins)."""
    seen = {}
    for m in messages:
        key = (m.ctx, m.msgid)
        old = seen.get(key)
        if old is None:
            seen[key] = m
        elif old.plural != m.plural:
            warn('%s: "%s" has the plural %r here and %r at %s - the first is kept' % (m.where, m.msgid, m.plural, old.plural, old.where))
    return [seen[k] for k in sorted(seen, key=lambda k: ((k[0] or ''), k[1]))]


# --- Catalogue lookup ------------------------------------------------------------------------------

_SPEC = re.compile(r'%%|%(?:zu|l{1,2}[diu]|[sdiu])')
_ANY_SPEC = re.compile(r'%(?!%)')
_BRACES = re.compile(r'\{\}|\{|\}')
PH = '\x00'


def key_shape(key):
    """(shape, specs) of a catalogue key made of %s/%d-style conversions only: the key with each one
    replaced by a NUL, and how many; None for a key with any other conversion."""
    out = []
    count = 0
    pos = 0
    for m in _SPEC.finditer(key):
        out.append(key[pos:m.start()])
        if m.group(0) == '%%':
            out.append('%')
        else:
            out.append(PH)
            count += 1
        pos = m.end()
    out.append(key[pos:])
    shape = ''.join(out)
    # a % left over (from "%.1f", "%5d") is a conversion this cannot carry
    leftover = re.sub(r'%%', '', key)
    stripped = _SPEC.sub('', leftover)
    if '%' in stripped:
        return None
    return shape, count


def to_braces(translation):
    """A translation of a catalogue key with conversions -> Slint's {} form (the conversions in order)."""
    out = []
    pos = 0
    for m in _SPEC.finditer(translation):
        out.append(_escape_braces(translation[pos:m.start()]))
        out.append('%' if m.group(0) == '%%' else '{}')
        pos = m.end()
    out.append(_escape_braces(translation[pos:]))
    return ''.join(out)


def _escape_braces(s):
    return s.replace('{', '{{').replace('}', '}}')


def msgid_shape(msgid):
    """(shape, count) of a Slint msgid: each {} a NUL. None when it has a numbered or lone brace."""
    if re.search(r'\{\d+\}', msgid):
        return None
    shape = msgid.replace('{}', PH)
    if '{' in shape or '}' in shape:
        return None
    return shape, msgid.count('{}')


class Lookup(object):
    def __init__(self, strings):
        self.strings = strings
        self.shapes = {}
        for key in sorted(strings):
            if '%' not in key:
                continue
            s = key_shape(key)
            if s is not None and s[1] > 0:
                self.shapes.setdefault(s[0], key)

    def find(self, text):
        """(translation, how) for one Slint string, how in {'exact', 'placeholder'}; (None, None) if none."""
        value = self.strings.get(text)
        if isinstance(value, str) and value != '':
            if '{}' in text:
                return _keep_braces(value), 'exact'
            return _escape_braces(value), 'exact'
        shape = msgid_shape(text)
        if shape is None or shape[1] == 0:
            return None, None
        key = self.shapes.get(shape[0])
        if key is None:
            return None, None
        value = self.strings[key]
        if not isinstance(value, str) or value == '':
            return None, None
        # the translation must carry the same number of conversions as the key (test_i18n holds this)
        if len(_SPEC.findall(value)) - value.count('%%') != shape[1]:
            return None, None
        return to_braces(value), 'placeholder'


def _keep_braces(value):
    out = []
    pos = 0
    for m in _BRACES.finditer(value):
        out.append(value[pos:m.start()])
        out.append('{}' if m.group(0) == '{}' else _escape_braces(m.group(0)))
        pos = m.end()
    out.append(value[pos:])
    return ''.join(out)


# --- .po output ------------------------------------------------------------------------------------

def po_quote(s):
    """A .po string: escaped, and a string with newlines split after each one, gettext style."""
    def esc(part):
        return (part.replace('\\', '\\\\').replace('"', '\\"').replace('\t', '\\t').replace('\r', '\\r'))
    pieces = s.split('\n')
    lines = []
    for idx, piece in enumerate(pieces):
        last = idx == len(pieces) - 1
        text = esc(piece) + ('' if last else '\\n')
        # the empty piece after a trailing newline is nothing to write; an empty string is "" itself
        if text or len(pieces) == 1:
            lines.append('"%s"' % text)
    if len(lines) == 1:
        return lines[0]
    return '""\n' + '\n'.join(lines)


def po_field(name, s):
    return '%s %s\n' % (name, po_quote(s))


def build_po(code, name_en, messages, lookup, warn):
    """(bytes, stats) for one language."""
    nplurals, expr = PLURALS.get(code, DEFAULT_PLURAL)
    if code not in PLURALS:
        warn('%s: no Plural-Forms in the table - the gettext default (nplurals=2; plural=(n != 1);) is written' % code)
    stats = {'msgids': len(messages), 'exact': 0, 'placeholder': 0, 'left': 0, 'plural_skipped': 0}
    head = [
        '# FoxSDR Slint window - %s. GENERATED by tools/slint-po.py from resources/lang/%s.json.\n' % (name_en, code),
        '# Do not edit: correct the catalogue and rebuild. Entries with no value are absent on purpose\n',
        '# (Slint then shows the English source string).\n',
        'msgid ""\n',
        'msgstr ""\n',
        '"Project-Id-Version: %s\\n"\n' % DOMAIN,
        '"Language: %s\\n"\n' % code,
        '"MIME-Version: 1.0\\n"\n',
        '"Content-Type: text/plain; charset=UTF-8\\n"\n',
        '"Content-Transfer-Encoding: 8bit\\n"\n',
        '"Plural-Forms: nplurals=%s; plural=(%s);\\n"\n' % (nplurals, expr),
    ]
    body = []
    for m in messages:
        one, how = lookup.find(m.msgid)
        entry = []
        if m.plural is None:
            if one is None:
                stats['left'] += 1
                continue
            if m.ctx is not None:
                entry.append(po_field('msgctxt', m.ctx))
            entry.append(po_field('msgid', m.msgid))
            entry.append(po_field('msgstr', one))
            stats[how] += 1
        else:
            many, how2 = lookup.find(m.plural)
            if one is None or many is None:
                stats['left'] += 1
                continue
            if int(nplurals) > 2:
                stats['plural_skipped'] += 1
                stats['left'] += 1
                continue
            if m.ctx is not None:
                entry.append(po_field('msgctxt', m.ctx))
            entry.append(po_field('msgid', m.msgid))
            entry.append(po_field('msgid_plural', m.plural))
            if int(nplurals) == 1:
                entry.append(po_field('msgstr[0]', many))
            else:
                entry.append(po_field('msgstr[0]', one))
                entry.append(po_field('msgstr[1]', many))
            stats['exact' if how == 'exact' and how2 == 'exact' else 'placeholder'] += 1
        body.append(''.join(entry))
    text = ''.join(head) + ('\n' + '\n'.join(body) if body else '')
    return text.encode('utf-8'), stats


def write_if_changed(path, data):
    try:
        with open(path, 'rb') as fh:
            if fh.read() == data:
                return False
    except OSError:
        pass
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as fh:
        fh.write(data)
    return True


def load_catalogues(lang_dir, warn):
    out = []
    for f in sorted(os.listdir(lang_dir)):
        if not f.endswith('.json'):
            continue
        code = f[:-len('.json')]
        try:
            with open(os.path.join(lang_dir, f), 'r', encoding='utf-8-sig') as fh:
                doc = json.load(fh)
        except (OSError, ValueError) as e:
            warn('%s: not read (%s)' % (f, e))
            continue
        strings = doc.get('strings') if isinstance(doc, dict) else None
        if not isinstance(strings, dict):
            warn('%s: no "strings" object - skipped' % f)
            continue
        if doc.get('code') != code:
            warn('%s: "code" is %r, not the file name - the file name is used' % (f, doc.get('code')))
        out.append((code, str(doc.get('englishName') or code), strings))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description='resources/lang/*.json -> <out>/<code>/LC_MESSAGES/%s.po' % DOMAIN)
    ap.add_argument('ui_dir')
    ap.add_argument('lang_dir')
    ap.add_argument('out_dir')
    ap.add_argument('--stamp', help='touched only when a .po was written (or does not exist)')
    ap.add_argument('--missing', metavar='CODE', help='list the msgids that CODE has no value for')
    ap.add_argument('--quiet', action='store_true')
    args = ap.parse_args(argv)
    # A build log is a pipe in the console's code page: a msgid with an ellipsis must not kill the run.
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, 'reconfigure'):
            stream.reconfigure(errors='backslashreplace')

    def warn(msg):
        sys.stderr.write('slint-po: warning: %s\n' % msg)

    if not os.path.isdir(args.ui_dir) or not os.path.isdir(args.lang_dir):
        sys.stderr.write('slint-po: %s or %s is not a directory (run from the repository root)\n' % (args.ui_dir, args.lang_dir))
        return 1
    files, raw = extract(args.ui_dir, warn)
    messages = unique(raw, warn)
    catalogues = load_catalogues(args.lang_dir, warn)
    if not args.quiet:
        print('slint-po: %d .slint files, %d @tr uses, %d msgids' % (len(files), len(raw), len(messages)))
    written = 0
    codes = set()
    for code, name_en, strings in catalogues:
        codes.add(code)
        lookup = Lookup(strings)
        data, st = build_po(code, name_en, messages, lookup, warn)
        path = os.path.join(args.out_dir, code, 'LC_MESSAGES', DOMAIN + '.po')
        if write_if_changed(path, data):
            written += 1
        if not args.quiet:
            extra = ' (%d plural left out: more than two forms)' % st['plural_skipped'] if st['plural_skipped'] else ''
            print('slint-po: %-6s msgids %d  exact %d  placeholder %d  left out %d%s'
                  % (code, st['msgids'], st['exact'], st['placeholder'], st['left'], extra))
        if args.missing == code:
            for m in messages:
                if lookup.find(m.msgid)[0] is None:
                    print('slint-po: missing in %s: %r' % (code, m.msgid))
    # A language whose catalogue was removed must not stay bundled.
    if os.path.isdir(args.out_dir):
        for d in sorted(os.listdir(args.out_dir)):
            stale = os.path.join(args.out_dir, d, 'LC_MESSAGES', DOMAIN + '.po')
            if d not in codes and os.path.isfile(stale):
                os.remove(stale)
                written += 1
    if args.stamp and (written or not os.path.exists(args.stamp)):
        os.makedirs(os.path.dirname(os.path.abspath(args.stamp)), exist_ok=True)
        with open(args.stamp, 'w') as fh:
            fh.write('%d\n' % written)
    if not args.quiet:
        print('slint-po: %d .po files written or removed, %d languages' % (written, len(codes)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
