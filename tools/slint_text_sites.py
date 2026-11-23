#!/usr/bin/env python3
"""slint_text_sites.py - the source scan that keeps text from reaching Slint unchecked (src/slint/safe_text.hpp is the boundary).

WHY: slint::SharedString aborts the process on a byte that is not UTF-8 (0.99.112 crash groups 6CCFD0C0E58FC30C and
104F425230C2A742). Text the program did not write must go through ss() / foxsdr::slintsafe::shared().

THIS SCAN (textual, wired into ctest as slint_text_sites; exit 1 on a finding) walks EVERY source file under src/slint (sub-directories
included; tests, ui and probe are left out) and fails when
  1. a SharedString( / SharedString{ construction is not in ALLOW below, wrapped in ss() / slintsafe::shared(), or a string literal;
  2. a FORBIDDEN call appears outside the files allowed to make it:
       slint::select_bundled_translation   takes its std::string_view straight to Rust, past every SharedString guard: only
                                           safe_text.hpp's selectBundledTranslation (which checks the language code) may call it;
       slintsafe::Make::from               builds a SharedString with NO validity check: only safe_text.hpp, and the files in the
                                           allow-list, whose text is valid by construction (a path already converted by core::utf8OrAnsiToUtf8);
       cbindgen_private                    Slint's own FFI, never from the host;
  3. a char ARRAY with no string-literal initialiser is passed directly to a Slint text entry point (set_*, invoke_*, ss, SharedString,
     set_row_data).
It prints the counts. (A textual scan of setter arguments was tried and dropped: with no type information it flags 140 of 180
std::string-looking arguments that are authored text built by tr()/formatText(), which drowns the real finding.)

WHAT IT CANNOT SEE: a std::string held in a variable and passed to a setter. That gap is closed by the COMPILER, not by text:
tools/slint_implicit_text_probe.py (ctest slint_implicit_text_probe) compiles every src/slint translation unit against a copy of the
Slint headers in which every route from a std::string_view / const char* into a SharedString is private or deleted, and proves by a
deliberately unguarded control file that it does flag. It needs an MSVC environment and skips (77, loudly "NOT CHECKED") without one.
The char-array rule (3) is by NAME only: it cannot see an array reached through a member (`rec.name`), a function that returns one, a
const array that is a parameter or member of a const object (the compiler's literal overload accepts it, and so does this scan), an
array declared in another file or header, an array put into a model by push_back / emplace_back (the compiler sees that one), or a
name that is reused for something else inside one block. The compiler closes the common case itself: a
NON-const char array does not compile into a SharedString in the probe.

Usage: slint_text_sites.py <repo>/src/slint [--list]
"""
import os, re, sys

# (file, text of the construction's argument) -> reason
ALLOW = {
    ('safe_text_make.hpp', 's'): 'Make::from: the one unchecked route, used by safe_text.hpp after validation and by text valid by construction',
}
# pattern -> (files allowed to contain it, why). Comments are stripped first.
FORBIDDEN = {
    r'\bselect_bundled_translation\s*\(': ({'safe_text.hpp'}, 'call slintsafe::selectBundledTranslation (it checks the language code is UTF-8 and plausible)'),
    r'\bMake::from\s*\(': ({'safe_text.hpp', 'cjk_register.hpp'}, 'Make::from builds a SharedString without a validity check: use ss() / slintsafe::shared(), or add the file to FORBIDDEN here with its reason'),
    r'\bcbindgen_private\b': (set(), "Slint's own FFI is not for the host"),
}
# a wrapper that makes an argument safe
WRAPPED = re.compile(r'^(std::move\()?(ss|slintsafe::shared|foxsdr::slintsafe::shared)\(')
BS = chr(92)
QUOTES = (chr(34), chr(39))
SKIP_DIRS = {'tests', 'ui', 'probe'}

def strip_comments(s):
    s = re.sub(r'/\*.*?\*/', lambda m: re.sub(r'[^\n]', ' ', m.group(0)), s, flags=re.S)
    return re.sub(r'//[^\n]*', lambda m: ' ' * len(m.group(0)), s)

def balanced(s, i):
    depth, j, instr = 0, i, None
    while j < len(s):
        c = s[j]
        if instr:
            if c == BS: j += 1
            elif c == instr: instr = None
        elif c in QUOTES: instr = c
        elif c in '([{': depth += 1
        elif c in ')]}':
            depth -= 1
            if depth == 0: return s[i + 1:j]
        j += 1
    return s[i + 1:]

CTOR = re.compile(r'\bSharedString(?:\s+\w+)?\s*[({]')   # SharedString(x), SharedString{x} and the named declaration SharedString s(x)
# char arrays: `char name[N]`, `char name[] = ...`, `char (&name)[N]`; the initialiser tells a literal-backed table from a buffer
ARRAY = re.compile(r'\bchar\s+(\w+)\s*\[[^\]]*\]\s*(=\s*")?|\bchar\s*\(\s*&\s*(\w+)\s*\)\s*\[')
ENTRY = r'(?:set_[a-z0-9_]+|invoke_[a-z0-9_]+|ss|SharedString(?:\s+\w+)?|set_row_data)'   # not push_back / emplace_back: with no types the scan cannot tell a model of SharedString from a std::vector<std::string>

def split_args(text):
    parts, depth, cur, instr, k = [], 0, '', None, 0
    while k < len(text):
        c = text[k]
        if instr:
            cur += c
            if c == BS: cur += text[k + 1]; k += 1
            elif c == instr: instr = None
        elif c in QUOTES: instr = c; cur += c
        elif c in '([{<': depth += 1; cur += c
        elif c in ')]}>': depth -= 1; cur += c
        elif c == ',' and depth == 0: parts.append(cur.strip()); cur = ''
        else: cur += c
        k += 1
    parts.append(cur.strip())
    return parts

PARAM = re.compile(r'^(const\s+)?[A-Za-z_][\w:]*(<[^>]*>)?\s*[&*]*\s+\w+(\s*=.*)?$')

def is_parameter_list(arg):
    return all(PARAM.match(p) for p in split_args(arg))

def block_end(src, p):
    """index of the } that closes the block that holds position p"""
    depth, j, instr = 0, p, None
    while j < len(src):
        c = src[j]
        if instr:
            if c == BS: j += 1
            elif c == instr: instr = None
        elif c in QUOTES: instr = c
        elif c == '{': depth += 1
        elif c == '}':
            if depth == 0: return j
            depth -= 1
        j += 1
    return len(src)

def source_files(root):
    for d, dirs, files in os.walk(root):
        dirs[:] = sorted(x for x in dirs if x not in SKIP_DIRS)
        for fn in sorted(files):
            if fn.endswith('.cpp') or fn.endswith('.hpp'):
                yield os.path.relpath(os.path.join(d, fn), root), fn

def scan(root):
    ctor_found = ctor_allowed = forbidden_calls = 0
    arrays = 0
    findings = []
    nfiles = 0
    for rel, fn in source_files(root):
        nfiles += 1
        src = strip_comments(open(os.path.join(root, rel), encoding='utf-8', errors='replace').read())
        line = lambda p: src.count('\n', 0, p) + 1
        for m in CTOR.finditer(src):
            arg = ' '.join(balanced(src, m.end() - 1).split())
            if re.match(r'^(const )?(slint::)?SharedString( &|&)?\s*\w*$', src[max(0, m.start() - 0):m.start()]): continue
            if arg == '' or arg.startswith(('std::string_view s', 'const ', 'std::string&')) or is_parameter_list(arg): continue   # declarations (a parameter list), the empty string
            ctor_found += 1
            if (fn, arg) in ALLOW: ctor_allowed += 1
            elif WRAPPED.match(arg) or re.match(r'^"', arg): ctor_allowed += 1
            else: findings.append((rel, line(m.start()), 'SharedString(' + arg + ') is not in ALLOW'))
        for pat, (allowed, why) in FORBIDDEN.items():
            for m in re.finditer(pat, src):
                forbidden_calls += 1
                if fn not in allowed:
                    findings.append((rel, line(m.start()), m.group(0).strip() + ' outside ' + ', '.join(sorted(allowed)) + ': ' + why if allowed else m.group(0).strip() + ': ' + why))
        for dm in ARRAY.finditer(src):
            arrays += 1
            if dm.group(2) is not None: continue   # a string literal initialiser: a table of authored text, not a buffer
            name = dm.group(1) or dm.group(3)
            scope_end = block_end(src, dm.end())   # the name means this array only inside the block that declares it
            for m in re.finditer(r'\b' + ENTRY + r'\s*\(\s*' + re.escape(name) + r'\s*[,)]', src[dm.end():scope_end]):
                findings.append((rel, line(dm.end() + m.start()), 'char array "' + name + '" passed straight to a Slint text entry point (' + m.group(0).strip() + '): copy it into a std::string and use ss()'))
    return findings, (nfiles, ctor_found, ctor_allowed, forbidden_calls, arrays)

def main():
    root = sys.argv[1] if len(sys.argv) > 1 else 'src/slint'
    findings, c = scan(root)
    for f in findings: print('%s:%d: %s' % f)
    print('slint_text_sites: %d files; constructions %d (allow-listed or wrapped %d); %d guarded-call site(s) seen; %d char array declaration(s); %d finding(s)' % (c[0], c[1], c[2], c[3], c[4], len(findings)))
    if c[0] < 50:
        print('slint_text_sites: FAILED: only %d source files found under %s (expected 100+): the scan did not look at the tree' % (c[0], root))
        return 1
    return 1 if findings else 0

if __name__ == '__main__':
    sys.exit(main())
