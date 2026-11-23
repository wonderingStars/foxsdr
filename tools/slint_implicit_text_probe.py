#!/usr/bin/env python3
"""slint_implicit_text_probe.py - proves that no text reaches a slint::SharedString except through src/slint/safe_text*.hpp.

WHY: SharedString aborts the process on a byte that is not UTF-8 (0.99.112 crash groups 6CCFD0C0E58FC30C and 104F425230C2A742).
Every route in its header ends in from_utf8().unwrap(): the converting constructors, operator=, operator+= and operator+.

HOW: copies the Slint C++ include tree and rewrites slint_string.h so that, for every source in src/slint,
  * the constructors from std::string_view, const char*, const char8_t* and std::u8string_view are PRIVATE, with one friend,
    foxsdr::slintsafe::Make (src/slint/safe_text_make.hpp): so `SharedString s(text)`, `vec.emplace_back(text)`,
    `SharedString{text}` and every implicit conversion fail; a const char* VARIABLE is a deleted constructor;
  * a string LITERAL (a const char array) is accepted by the constructor, operator=, operator+= and operator+: authored text;
  * a NON-const char ARRAY (a local buffer, a member of a non-const object) is a deleted overload everywhere, so runtime text in a
    char buffer cannot be handed over. A CONST char array is indistinguishable from a literal by the language and is accepted
    (tools/slint_text_sites.py scans for the by-name cases);
  * operator=(std::string_view / const char*), operator+=(std::string_view) and both operator+(..., std::string_view) are deleted.
Slint's own headers use some of these routes (two logging helpers in slint_models.h, one line in slint.h); those are patched
in the copy so that the headers compile cleanly: an error inside a header would stop the compiler before it instantiates the
templates (an emplace_back into a vector of SharedString) that the probe exists to look at.
Then it compiles every translation unit of the foxsdr-slint target (cl /Zs, the flags of the configuration under test, taken from
MSBuild's CL.command.1.tlog of THAT configuration). Every error whose message block names a src/slint line is a finding.

IT CANNOT PASS WITHOUT CHECKING:
  * a POSITIVE CONTROL, tools/slint_probe_control.cpp, an unguarded file the probe compiles with a real unit's flags in every run: each
    line marked MUST-FLAG has to be refused and each MUST-PASS line has to compile, or the probe fails (it has stopped looking);
  * for every unit the include-path swap is checked twice: the command line holds the patched copy and no longer the real SDK, and
    /showIncludes shows the compiler read the patched slint_string.h (and never the real one);
  * the compiler's exit code is checked: an error that is not the deleted / private SharedString overload (a missing include, a
    syntax error), or a failure with no error line at all (cl did not start, command line too long), FAILS the probe;
  * zero translation units is a failure.
It still cannot see code the Release compile of foxsdr-slint does not compile (other targets, other platforms' #if blocks, a file not
yet built: the list is MSBuild's own record of what was built).

USAGE (needs an MSVC environment: vcvars64 done, or the compiler path given so vcvars64.bat is found above it):
    slint_implicit_text_probe.py <slint include dir (.../include/slint)> <build dir> <scratch dir> [<cl.exe>] [--config Release]
Exit codes: 0 proved, 1 findings or the probe's own failure, 77 NOT CHECKED (no MSVC environment, no Slint include dir, or foxsdr-slint
has not been built in this configuration): ctest's SKIP_RETURN_CODE; a loud "NOT CHECKED" line says so.
"""
import glob, os, re, shutil, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

BS = chr(92)
NL = chr(10)
SKIP = 77
HERE = os.path.dirname(os.path.abspath(__file__))
CONTROL = os.path.join(HERE, 'slint_probe_control.cpp')

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
    template <std::size_t N> SharedString(char (&)[N]) = delete;   // a NON-const char array is a runtime buffer, never a literal
    template <class T>
        requires((std::is_same_v<std::decay_t<T>, const char *> || std::is_same_v<std::decay_t<T>, char *>) && !std::is_array_v<std::remove_reference_t<T>>)
    SharedString(T &&) = delete;
    template <std::size_t N> SharedString &operator=(const char (&lit)[N]) { return *this = SharedString(std::string_view(lit, N - 1)); }
    template <std::size_t N> SharedString &operator=(char (&)[N]) = delete;
    SharedString &operator=(std::string_view) = delete;
    template <class T>
        requires((std::is_same_v<std::decay_t<T>, const char *> || std::is_same_v<std::decay_t<T>, char *>) && !std::is_array_v<std::remove_reference_t<T>>)
    SharedString &operator=(T &&) = delete;
    SharedString &operator+=(const SharedString &other) { cbindgen_private::slint_shared_string_append(this, other.data(), other.size()); return *this; }
    template <std::size_t N> SharedString &operator+=(const char (&lit)[N]) { return *this += SharedString(std::string_view(lit, N - 1)); }
    template <std::size_t N> SharedString &operator+=(char (&)[N]) = delete;
    SharedString &operator+=(std::string_view) = delete;
    friend SharedString operator+(const SharedString &a, const SharedString &b) { SharedString r = a; r += b; return r; }
    template <std::size_t N> friend SharedString operator+(const SharedString &a, const char (&b)[N]) { SharedString r = a; r += SharedString(std::string_view(b, N - 1)); return r; }
    template <std::size_t N> friend SharedString operator+(const SharedString &, char (&)[N]) = delete;
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


def norm(path):
    """a path or command text with one slash style and one case, for comparing"""
    return path.replace(BS, '/').lower()


def not_checked(why):
    print('slint_implicit_text_probe: *** NOT CHECKED *** ' + why + ' (exit 77: the text boundary was NOT verified by the compiler in this run)')
    return SKIP


def swap_include(cmd, inc, new):
    """cmd with the real SDK include path replaced by `new` (either slash style, any case); the count of replacements"""
    n = 0
    for spelled in {inc.replace(BS, '/'), inc.replace('/', BS)}:
        cmd, k = re.subn(re.escape(spelled), lambda m: new.replace(BS, '/'), cmd, flags=re.I)
        n += k
    return cmd, n


def is_boundary_error(block):
    """True when the error block is the compiler refusing the patched SharedString overload (deleted / private / no usable overload of it)"""
    text = '\n'.join(block)
    return ('SharedString' in text) or ('C2280' in text) or ('C2248' in text) or ('deleted function' in text) or ('private member' in text)


def parse_blocks(out):
    blocks, cur = [], None
    for l in out.splitlines():
        if 'error C' in l or 'fatal error' in l:
            cur = [l]
            blocks.append(cur)
        elif cur is not None and (l.startswith(' ') or 'note:' in l or 'see reference' in l):
            cur.append(l)
        else:
            cur = None
    return blocks


def main():
    args = [a for a in sys.argv[1:]]
    config = 'Release'
    only_control = '--only-control' in args
    if only_control: args.remove('--only-control')
    if '--config' in args:
        i = args.index('--config')
        config = args[i + 1]
        del args[i:i + 2]
    if len(args) < 3:
        print(__doc__)
        return 2
    inc, build, scratch = args[0:3]
    cl = args[3] if len(args) > 3 else shutil.which('cl')
    if os.name != 'nt':
        return not_checked('not Windows')
    if not os.path.isdir(inc):
        return not_checked('the Slint include directory ' + inc + ' does not exist')
    tlogs = glob.glob(os.path.join(build, 'src', 'slint', 'foxsdr-slint.dir', config, 'foxsdr-slint.tlog', 'CL.command.1.tlog'))
    if not tlogs:
        return not_checked('foxsdr-slint has not been built in the ' + config + ' configuration here (no CL.command.1.tlog for it)')
    have_env = shutil.which('cl') is not None and bool(os.environ.get('INCLUDE'))
    vcvars = None if have_env else find_vcvars(cl)
    if not have_env and not vcvars:
        return not_checked('no MSVC environment: cl is not on PATH and vcvars64.bat was not found')
    if not os.path.isfile(CONTROL):
        print('slint_implicit_text_probe: FAILED: the positive control %s is missing' % CONTROL)
        return 1
    copy = os.path.join(scratch, 'slint-explicit')
    shutil.rmtree(copy, ignore_errors=True)
    os.makedirs(scratch, exist_ok=True)
    shutil.copytree(inc, os.path.join(copy, 'slint'))
    patch_header(copy)
    copy_norm = norm(copy + '/slint')
    inc_norm = norm(inc)
    lines = open(tlogs[0], 'rb').read().decode('utf-16').splitlines()
    cmds, setup_failures = [], []
    for i in range(0, len(lines) - 1, 2):
        f, c = lines[i][1:], lines[i + 1]
        if '/SRC/SLINT/' not in f.replace(BS, '/').upper() or 'GENERATED' in f.upper():
            continue
        c, swapped = swap_include(c, inc, copy + '/slint')
        if swapped == 0 or inc_norm in norm(c) or copy_norm not in norm(c):
            setup_failures.append('%s: the include-path swap did not take effect (the command names the real Slint SDK, or not the patched copy)' % f)
            continue
        c = c.replace('/c ', '/c /Zs /showIncludes ', 1).replace('/Zi ', '').replace('/O2 /Ob2', '')
        cmds.append((f, c.rsplit(' ', 1)[0]))
    workdir = os.path.join(build, 'src', 'slint')
    if not cmds and not setup_failures:
        print('slint_implicit_text_probe: FAILED: 0 translation units found in %s (the probe has nothing to check)' % tlogs[0])
        return 1

    def run(f, c):
        line = 'cl ' + c + ' "' + f + '"'
        if vcvars:
            line = 'call "' + vcvars + '" >nul 2>&1 && ' + line
        r = subprocess.run(line, shell=True, capture_output=True, text=True, cwd=workdir, errors='replace')
        return r.returncode, r.stdout + r.stderr

    def ours(text):
        return [l.strip()[:220] for l in text.splitlines() if 'src' + BS + 'slint' in l.lower().replace('/', BS)]

    def include_evidence(out):
        """(saw the patched slint_string.h, saw the real one)"""
        patched = real = False
        for l in out.splitlines():
            if 'including file' in l and 'slint_string.h' in l:
                n = norm(l)
                if copy_norm in n: patched = True
                if inc_norm in n: real = True
        return patched, real

    findings, noise, other, silent = set(), set(), set(), []
    saw_patched = 0
    if only_control:
        cmds_all, cmds = cmds, cmds[:1]   # a debugging aid: the control with the first unit's flags, nothing else

    def go(fc):
        f, c = fc
        return f, run(f, c)

    with ThreadPoolExecutor(3) as ex:
        for f, (rc, out) in ex.map(go, cmds):
            patched, real = include_evidence(out)
            if real:
                setup_failures.append('%s: the compiler read the REAL slint_string.h, not the patched copy' % f)
            if patched:
                saw_patched += 1
            blocks = parse_blocks(out)
            if rc != 0 and not blocks:
                silent.append('%s: the compiler failed (exit %d) with no error line: %s' % (f, rc, ' | '.join(l.strip() for l in out.splitlines() if l.strip() and 'including file' not in l)[:200]))
                continue
            for blk in blocks:
                n = ours('\n'.join(blk))
                if n and is_boundary_error(blk):
                    findings.add(n[0])
                elif n:
                    other.add(n[0])            # an error in src/slint that is not the SharedString overload: the unit did not compile for another reason
                elif is_boundary_error(blk):
                    noise.add(blk[0][:200])    # an error in a Slint header that names no src/slint line: the probe's own header patch is incomplete
                else:
                    other.add(blk[0][:200])

    # ---- the positive control: the compiler must refuse every MUST-FLAG statement and accept every MUST-PASS one, each compiled ON ITS OWN ----
    control_failures = []
    must_flag, must_pass, control_flagged = set(), set(), set()
    if cmds:
        ctl_lines = open(CONTROL, encoding='utf-8').read().splitlines()
        begin = [i for i, l in enumerate(ctl_lines) if l.strip() == '// CASES-BEGIN']
        prelude = NL.join(ctl_lines[:begin[0]]) if begin else ''
        cases = []
        for no, text in enumerate(ctl_lines, 1):
            for tag, bucket in (('// MUST-FLAG', must_flag), ('// MUST-PASS', must_pass)):
                if begin and no > begin[0] + 1 and text.rstrip().endswith(tag):
                    bucket.add(no)
                    cases.append((no, tag, text.rstrip()[:-len(tag)].strip()))
        f0, c0 = cmds[0]
        case_dir = os.path.join(scratch, 'control-cases')
        shutil.rmtree(case_dir, ignore_errors=True)
        os.makedirs(case_dir, exist_ok=True)

        def one(case):
            no, tag, stmt = case
            path = os.path.join(case_dir, 'case_%d.cpp' % no)
            body = (prelude + NL + 'void probe_case(const std::string& s, const char* p, char* q) { char buf[16] = "x"; slint::SharedString b; std::vector<slint::SharedString> v;' + NL
                    + '#line %d "slint_probe_control.cpp"' % no + NL + stmt + NL + '(void)s; (void)p; (void)q; (void)buf; (void)b; (void)v; }' + NL)
            open(path, 'w', encoding='utf-8').write(body)
            rc, out = run(path, c0)
            return case, rc, out

        if not cases:
            control_failures.append('the positive control has no MUST-FLAG / MUST-PASS statements after // CASES-BEGIN')
        refused_ok = 0
        with ThreadPoolExecutor(3) as ex:
            for (no, tag, stmt), rc, out in ex.map(one, cases):
                patched, real = include_evidence(out)
                if not patched or real:
                    control_failures.append('control line %d: the compiler did not read the patched slint_string.h (patched seen: %s, real seen: %s)' % (no, patched, real))
                blocks = parse_blocks(out)
                named = any(re.search(r'slint_probe_control\.cpp\(%d[,)]' % no, l) for l in out.splitlines() if 'error' in l or 'note' in l)
                if tag == '// MUST-FLAG':
                    if rc == 0 or not named:
                        control_failures.append('control line %d (MUST-FLAG) was NOT refused by the compiler: %s' % (no, stmt))
                    elif not all(is_boundary_error(b) for b in blocks):
                        control_failures.append('control line %d was refused, but by an error that is not the SharedString overload: %s' % (no, blocks[0][0][:200]))
                    else:
                        control_flagged.add(no)
                elif rc != 0 or blocks:
                    control_failures.append('control line %d (MUST-PASS, authored text) was refused by the compiler: %s' % (no, (blocks[0][0] if blocks else 'exit %d' % rc)[:200]))
    else:
        control_failures.append('the positive control could not run (no translation units)')

    print('\n'.join(sorted(findings)))
    failed = False
    if noise:
        failed = True
        print('slint_implicit_text_probe: %d error(s) inside the patched Slint headers (the patch needs extending):' % len(noise))
        print('\n'.join(sorted(noise)))
    if other:
        failed = True
        print('slint_implicit_text_probe: %d compile error(s) that are NOT the SharedString overload (a unit that does not compile proves nothing):' % len(other))
        print('\n'.join(sorted(other)))
    if silent:
        failed = True
        print('slint_implicit_text_probe: %d unit(s) failed with no compiler error line:' % len(silent))
        print('\n'.join(silent))
    if setup_failures:
        failed = True
        print('slint_implicit_text_probe: %d unit(s) with a broken setup:' % len(setup_failures))
        print('\n'.join(setup_failures))
    if control_failures:
        failed = True
        print('slint_implicit_text_probe: POSITIVE CONTROL FAILED (the probe cannot be trusted):')
        print('\n'.join(control_failures))
    print('slint_implicit_text_probe: configuration %s, %d translation units (%d read the patched header), %d finding(s), %d header error(s), %d other compile error(s); '
          'positive control: %d of %d MUST-FLAG lines refused, %d of %d MUST-PASS lines accepted (%s)'
          % (config, len(cmds), saw_patched, len(findings), len(noise), len(other), len(control_flagged), len(must_flag), len(must_pass) - sum('MUST-PASS' in x for x in control_failures), len(must_pass),
             'OK' if not control_failures else 'FAILED'))
    return 1 if (findings or failed) else 0


if __name__ == '__main__':
    sys.exit(main())
