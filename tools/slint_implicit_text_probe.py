#!/usr/bin/env python3
"""slint_implicit_text_probe.py - finds every IMPLICIT std::string / const char* -> slint::SharedString conversion in src/slint.

How: copies the Slint C++ include tree, makes SharedString's constructors from std::string_view, const char*, const char8_t* and
std::u8string_view `explicit`, and compiles every src/slint translation unit of the foxsdr-slint target syntax-only (cl /Zs)
against that copy. Each implicit conversion becomes an error C2664/C2440 whose location names the line; explicit constructions
(slint::SharedString(x), ss(x)) still compile. String LITERALS also error ("from 'const char [N]'") and are authored text.

Run from an MSVC prompt (cl on PATH) after building foxsdr-slint once (it reads that target's CL.command tlog):
    python tools/slint_implicit_text_probe.py <slint include dir (…/include/slint)> <build dir> <scratch dir>
Prints every error line that is in a src/slint file and is not a literal. 0 lines = no implicit conversion of non-literal text.
Last run on fix/slint-utf8: 110 translation units, 0 such errors (the only errors were inside slint-testing.h itself).
"""
import json, os, shutil, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

BS = chr(92)

def main():
    inc, build, scratch = sys.argv[1:4]
    copy = os.path.join(scratch, 'slint-explicit')
    shutil.rmtree(copy, ignore_errors=True)
    shutil.copytree(inc, os.path.join(copy, 'slint'))
    p = os.path.join(copy, 'slint', 'private', 'slint_string.h')
    b = open(p, encoding='utf-8').read()
    for o in ('SharedString(std::string_view s)', 'SharedString(const char *s)', 'SharedString(const char8_t *s)', 'SharedString(std::u8string_view s)'):
        assert b.count('    ' + o) == 1, o
        b = b.replace('    ' + o, '    explicit ' + o)
    open(p, 'w', encoding='utf-8').write(b)
    h = os.path.join(copy, 'slint', 'slint.h')       # the header's own copy-initialisation
    t = open(h, encoding='utf-8').read().replace('SharedString s = xdg_app_id;', 'SharedString s(xdg_app_id);')
    open(h, 'w', encoding='utf-8').write(t)
    tlog = os.path.join(build, 'src', 'slint', 'foxsdr-slint.dir', 'Release', 'foxsdr-slint.tlog', 'CL.command.1.tlog')
    lines = open(tlog, 'rb').read().decode('utf-16').splitlines()
    cmds = []
    for i in range(0, len(lines) - 1, 2):
        f, c = lines[i][1:], lines[i + 1]
        if '/SRC/SLINT/' not in f.replace(BS, '/').upper() or 'GENERATED' in f.upper(): continue
        c = c.replace(inc.replace(BS, '/'), (copy + '/slint').replace(BS, '/')).replace('/c ', '/c /Zs ', 1).replace('/Zi ', '').replace('/O2 /Ob2', '')
        cmds.append((f, c.rsplit(' ', 1)[0]))
    def go(fc):
        f, c = fc
        r = subprocess.run('cl ' + c + ' "' + f + '"', shell=True, capture_output=True, text=True, cwd=os.path.join(build, 'src', 'slint'))
        return r.stdout + r.stderr
    bad = []
    with ThreadPoolExecutor(3) as ex:
        for out in ex.map(go, cmds):
            for l in out.splitlines():
                if 'error C' in l and "from 'const char [" not in l and 'src' + BS + 'slint' in l.lower().replace('/', BS):
                    bad.append(l)
    print('\n'.join(bad))
    print('slint_implicit_text_probe: %d translation units, %d implicit non-literal conversions' % (len(cmds), len(bad)))
    return 1 if bad else 0

if __name__ == '__main__':
    sys.exit(main())
