#!/usr/bin/env python3
"""slint_implicit_text_probe.py - proves that no text reaches a slint::SharedString except through src/slint/safe_text*.hpp.

WHY: SharedString aborts the process on a byte that is not UTF-8 (0.99.112 crash groups 6CCFD0C0E58FC30C and 104F425230C2A742).
Every route in its header ends in from_utf8().unwrap(): the converting constructors, operator=, operator+= and operator+.

HOW: copies the Slint C++ include tree and rewrites slint_string.h so that, for every source in src/slint,
  * the constructors from std::string_view, const char*, const char8_t* and std::u8string_view are PRIVATE, with one friend,
    foxsdr::slintsafe::Make (src/slint/safe_text_make.hpp): so `SharedString s(text)`, `vec.emplace_back(text)`,
    `SharedString{text}` and every implicit conversion fail; a const char* VARIABLE is a deleted constructor;
  * a string LITERAL (a const char array) is accepted by the constructor, operator=, operator+= and operator+: authored text;
  * operator=(std::string_view / const char*), operator+=(std::string_view) and both operator+(..., std::string_view) are deleted.
Slint's own headers use some of these routes (two logging helpers in slint_models.h, one line in slint.h); those are patched
in the copy so that the headers compile cleanly: an error inside a header would stop the compiler before it instantiates the
templates (an emplace_back into a vector of SharedString) that the probe exists to look at.
Then it compiles every translation unit of the foxsdr-slint target syntax-only (cl /Zs). Every error whose message block names a
src/slint line (the error itself or a "see reference to..." note) is a finding. 0 findings = proved for what the compiler sees.
A char ARRAY holding runtime text would match the literal overload; src/slint has no such site (grep).

USAGE (needs an MSVC environment: vcvars64 done, or the compiler path given so vcvars64.bat is found above it):
    slint_implicit_text_probe.py <slint include dir (.../include/slint)> <build dir> <scratch dir> [<cl.exe>]
Exit codes: 0 proved, 1 findings, 77 SKIPPED (no MSVC environment, or foxsdr-slint has not been built): ctest's
SKIP_RETURN_CODE, so a machine without the compiler skips and does not fail.
"""
import glob, os, shutil, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

BS = chr(92)
NL = chr(10)
SKIP = 77

CTORS = [
    "    SharedString(std::string_view s)\n    {\n        cbindgen_private::slint_shared_string_from_bytes(this, s.data(), s.size());\n    }\n",
    "    SharedString(const char *s) : SharedString(std::string_view(s)) { }\n",
    "    SharedString(const char8_t *s) : SharedString(reinterpret_cast<const char *>(s)) { }\n",
    "    SharedString(std::u8string_view s)\n    {\n        cbindgen_private::slint_shared_string_from_bytes(\n                this, reinterpret_cast<const char *>(s.data()), s.size());\n    }\n",
]
ASSIGNS = [
    "    SharedString &operator=(std::string_view s)\n    {\n        cbindgen_private::slint_shared_string_drop(this);\n        cbindgen_private::slint_shared_string_from_bytes(this, s.data(), s.size());\n        return *this;\n    }\n",
    "    SharedString &operator=(const char *s) { return *this = std::string_view(s); }\n",
]
PLUSES = [
    "    friend SharedString operator+(const SharedString &a, std::string_view b)\n    {\n        SharedString a2 = a;\n        return a2 += b;\n    }\n",
    "    friend SharedString operator+(SharedString &&a, std::string_view b)\n    {\n        a += b;\n        return a;\n    }\n",
]
PLUSEQ = "    SharedString &operator+=(std::string_view other)\n    {\n        cbindgen_private::slint_shared_string_append(this, other.data(), other.size());\n        return *this;\n    }\n"

PUBLIC_ADD = """
    // ---- probe: every route from a std::string_view / const char* is closed, except the one friend ----
    friend struct ::foxsdr::slintsafe::Make;
    template <std::size_t N> SharedString(const char (&lit)[N]) : SharedString(std::string_view(lit, N - 1)) { }   // a literal: authored text
    template <class T>
        requires((std::is_same_v<std::decay_t<T>, const char *> || std::is_same_v<std::decay_t<T>, char *>) && !std::is_array_v<std::remove_reference_t<T>>)
    SharedString(T &&) = delete;
    template <std::size_t N> SharedString &operator=(const char (&lit)[N]) { return *this = SharedString(std::string_view(lit, N - 1)); }
    SharedString &operator=(std::string_view) = delete;
    template <class T>
        requires((std::is_same_v<std::decay_t<T>, const char *> || std::is_same_v<std::decay_t<T>, char *>) && !std::is_array_v<std::remove_reference_t<T>>)
    SharedString &operator=(T &&) = delete;
    SharedString &operator+=(const SharedString &other) { cbindgen_private::slint_shared_string_append(this, other.data(), other.size()); return *this; }
    template <std::size_t N> SharedString &operator+=(const char (&lit)[N]) { return *this += SharedString(std::string_view(lit, N - 1)); }
    SharedString &operator+=(std::string_view) = delete;
    friend SharedString operator+(const SharedString &a, const SharedString &b) { SharedString r = a; r += b; return r; }
    template <std::size_t N> friend SharedString operator+(const SharedString &a, const char (&b)[N]) { SharedString r = a; r += SharedString(std::string_view(b, N - 1)); return r; }
    friend SharedString operator+(const SharedString &, std::string_view) = delete;
    friend SharedString operator+(SharedString &&, std::string_view) = delete;
"""

# Slint's own diagnostics that add a std::string_view to a SharedString
MODELS_NOISE = [
    "    auto message =\n            SharedString(\"array.\") + function + \"(): the model does not support this modification\";\n    cbindgen_private::slint_debug(&message);\n",
    "    auto message = SharedString(\"array.\") + function\n            + \"(): the row index is out of bounds (the model has \"\n            + SharedString::from_number(static_cast<double>(row_count)) + \" rows)\";\n    cbindgen_private::slint_debug(&message);\n",
]


def read_lf(path):
    return open(path, encoding='utf-8', newline='').read().replace(chr(13) + NL, NL)


def patch_header(copy):
    p = os.path.join(copy, 'slint', 'private', 'slint_string.h')
    b = read_lf(p)
    assert b.count('struct SharedString\n{\n') == 1 and b.count('namespace slint {\n') >= 1, 'slint_string.h layout changed'
    # the forward declaration goes at GLOBAL scope, before namespace slint: the friend must name this Make, not a slint::foxsdr one
    b = b.replace('namespace slint {\n', 'namespace foxsdr::slintsafe { struct Make; }\nnamespace slint {\n', 1)
    for blk in CTORS + ASSIGNS + PLUSES + [PLUSEQ]:
        assert b.count(blk) == 1, 'header block not found (Slint version changed?):\n' + blk
        b = b.replace(blk, '', 1)
    marker = '\nprivate:\n    /// Use SharedString::from_number'
    assert b.count(marker) == 1, 'private marker'
    # only the string_view constructors come back (privately): a private const char* one would beat the literal template for a literal
    keep = ''.join(c for c in CTORS if 'const char *s' not in c and 'const char8_t' not in c)
    b = b.replace(marker, PUBLIC_ADD + '\nprivate:\n' + keep + '    /// Use SharedString::from_number', 1)
    clr = '    void clear() { *this = std::string_view("", 0); }\n'
    assert b.count(clr) == 1, 'clear() not found (Slint version changed?)'
    b = b.replace(clr, '    void clear() { *this = SharedString(); }\n', 1)
    open(p, 'w', encoding='utf-8', newline='').write(b)
    m = os.path.join(copy, 'slint', 'private', 'slint_models.h')
    t = read_lf(m)
    for body in MODELS_NOISE:
        assert t.count(body) == 1, 'slint_models.h block not found (Slint version changed?)'
        t = t.replace(body, '    (void)function;\n', 1)
    open(m, 'w', encoding='utf-8', newline='').write(t)
    h = os.path.join(copy, 'slint', 'slint.h')
    t = read_lf(h)
    old = 'SharedString s = xdg_app_id;'
    assert t.count(old) == 1, 'slint.h line not found (Slint version changed?)'
    open(h, 'w', encoding='utf-8', newline='').write(t.replace(old, 'SharedString s; (void)xdg_app_id;', 1))


def find_vcvars(cl):
    if not cl:
        return None
    d = os.path.dirname(os.path.abspath(cl))
    for _ in range(8):
        cand = os.path.join(d, 'Auxiliary', 'Build', 'vcvars64.bat')
        if os.path.exists(cand):
            return cand
        d = os.path.dirname(d)
    return None


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    inc, build, scratch = sys.argv[1:4]
    cl = sys.argv[4] if len(sys.argv) > 4 else shutil.which('cl')
    tlogs = glob.glob(os.path.join(build, 'src', 'slint', 'foxsdr-slint.dir', '*', 'foxsdr-slint.tlog', 'CL.command.1.tlog'))
    if os.name != 'nt' or not tlogs or not os.path.isdir(inc):
        print('slint_implicit_text_probe: SKIPPED (not Windows, foxsdr-slint not built here, or no Slint include dir)')
        return SKIP
    have_env = shutil.which('cl') is not None and bool(os.environ.get('INCLUDE'))
    vcvars = None if have_env else find_vcvars(cl)
    if not have_env and not vcvars:
        print('slint_implicit_text_probe: SKIPPED (no MSVC environment: cl is not on PATH and vcvars64.bat was not found)')
        return SKIP
    copy = os.path.join(scratch, 'slint-explicit')
    shutil.rmtree(copy, ignore_errors=True)
    os.makedirs(scratch, exist_ok=True)
    shutil.copytree(inc, os.path.join(copy, 'slint'))
    patch_header(copy)
    lines = open(tlogs[0], 'rb').read().decode('utf-16').splitlines()
    cmds = []
    for i in range(0, len(lines) - 1, 2):
        f, c = lines[i][1:], lines[i + 1]
        if '/SRC/SLINT/' not in f.replace(BS, '/').upper() or 'GENERATED' in f.upper():
            continue
        c = c.replace(inc.replace(BS, '/'), (copy + '/slint').replace(BS, '/')).replace('/c ', '/c /Zs ', 1).replace('/Zi ', '').replace('/O2 /Ob2', '')
        cmds.append((f, c.rsplit(' ', 1)[0]))
    workdir = os.path.join(build, 'src', 'slint')

    def go(fc):
        f, c = fc
        line = 'cl ' + c + ' "' + f + '"'
        if vcvars:
            line = 'call "' + vcvars + '" >nul 2>&1 && ' + line
        r = subprocess.run(line, shell=True, capture_output=True, text=True, cwd=workdir)
        return r.stdout + r.stderr

    def ours(text):
        return [l.strip()[:220] for l in text.splitlines() if 'src' + BS + 'slint' in l.lower().replace('/', BS)]

    findings, noise = set(), set()
    with ThreadPoolExecutor(3) as ex:
        for out in ex.map(go, cmds):
            blocks, cur = [], None
            for l in out.splitlines():
                if 'error C' in l:
                    cur = [l]
                    blocks.append(cur)
                elif cur is not None and (l.startswith(' ') or 'note:' in l or 'see reference' in l):
                    cur.append(l)
                else:
                    cur = None
            for blk in blocks:
                n = ours('\n'.join(blk))
                if n:
                    findings.add(n[0])
                else:
                    noise.add(blk[0][:200])   # an error in a Slint header that names no src/slint line: the probe's own header patch is incomplete
    print('\n'.join(sorted(findings)))
    if noise:
        print('slint_implicit_text_probe: %d error(s) inside the patched Slint headers (the patch needs extending):' % len(noise))
        print('\n'.join(sorted(noise)))
    print('slint_implicit_text_probe: %d translation units, %d finding(s), %d header error(s)' % (len(cmds), len(findings), len(noise)))
    return 1 if (findings or noise) else 0


if __name__ == '__main__':
    sys.exit(main())
