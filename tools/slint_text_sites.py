#!/usr/bin/env python3
"""slint_text_sites.py - the source scan that keeps text from reaching Slint unchecked (src/slint/safe_text.hpp is the boundary).

WHY: slint::SharedString aborts the process on a byte that is not UTF-8 (0.99.112 crash groups 6CCFD0C0E58FC30C and
104F425230C2A742). Text the program did not write must go through ss() / foxsdr::slintsafe::shared().

THIS SCAN (textual, wired into ctest as slint_text_sites; exit 1 on a finding):
  1. every SharedString( / SharedString{ construction in src/slint (tests excluded) must be in ALLOW below, with its reason;
It prints the counts. (A textual scan of setter arguments was tried and dropped: with no type information it flags 140 of 180
std::string-looking arguments that are authored text built by tr()/formatText(), which drowns the real finding.)

WHAT IT CANNOT SEE: a std::string held in a variable and passed to a setter. That gap is closed by the COMPILER, not by text:
tools/slint_implicit_text_probe.py compiles every src/slint translation unit against a copy of the Slint headers whose
SharedString constructors from std::string_view / const char* are `explicit`, so every implicit conversion is an error that
names its line. It needs an MSVC prompt and a copy of the Slint include tree, so it is run by hand when src/slint changes
(the last run: 0 implicit conversions in 110 files); it is not part of ctest.

Usage: slint_text_sites.py <repo>/src/slint [--list]
"""
import os, re, sys

# (file, text of the construction's argument) -> reason
ALLOW = {
    ('safe_text.hpp', 's'): 'the boundary itself, after isValidUtf8()',
    ('safe_text.hpp', 'std::string_view(valid(s, site))'): 'the boundary itself, after repair',
    ('cjk_register.hpp', 'cascade::core::utf8OrAnsiToUtf8(face.path)'): 'a path from path::string() (ANSI on Windows) converted to UTF-8 first',
    ('input_target.hpp', 't'): 'key text: the slint::platform::key_codes constants and the host key table (authored)',
}
# a wrapper that makes an argument safe
WRAPPED = re.compile(r'^(std::move\()?(ss|slintsafe::shared|foxsdr::slintsafe::shared)\(')
STRING_EXPR = re.compile(r'(\.c_str\(\)|\.data\(\)|std::string\(|\.string\(\)|\.substr\(|\bfmt\(|\bformatText\(|std::to_string\(|\+\s*")')
BS = chr(92)
QUOTES = (chr(34), chr(39))

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

def split_args(text):
    parts, depth, cur, instr, k = [], 0, '', None, 0
    while k < len(text):
        c = text[k]
        if instr:
            cur += c
            if c == BS: cur += text[k + 1]; k += 1
            elif c == instr: instr = None
        elif c in QUOTES: instr = c; cur += c
        elif c in '([{': depth += 1; cur += c
        elif c in ')]}': depth -= 1; cur += c
        elif c == ',' and depth == 0: parts.append(cur.strip()); cur = ''
        else: cur += c
        k += 1
    parts.append(cur.strip())
    return parts

CTOR = re.compile(r'\bSharedString\s*[({]')
CALL = re.compile(r'\b(set_[a-z0-9_]+|push_back|set_row_data|insert|emplace_back)\s*\(')

def scan(root, list_all=False):
    ctor_found = ctor_allowed = 0
    expr_found = expr_wrapped = expr_allowed = 0
    findings = []
    for fn in sorted(os.listdir(root)):
        if not (fn.endswith('.cpp') or fn.endswith('.hpp')): continue
        src = strip_comments(open(os.path.join(root, fn), encoding='utf-8', errors='replace').read())
        line = lambda p: src.count('\n', 0, p) + 1
        for m in CTOR.finditer(src):
            arg = ' '.join(balanced(src, m.end() - 1).split())
            if re.match(r'^(const )?(slint::)?SharedString( &|&)?\s*\w*$', src[max(0, m.start() - 0):m.start()]): continue
            if arg == '' or arg.startswith(('std::string_view s', 'const ', 'std::string&')): continue   # declarations, the empty string
            ctor_found += 1
            if (fn, arg) in ALLOW: ctor_allowed += 1
            elif WRAPPED.match(arg) or re.match(r'^"', arg): ctor_allowed += 1
            else: findings.append((fn, line(m.start()), 'SharedString(' + arg + ') is not in ALLOW'))
    return findings, (ctor_found, ctor_allowed, expr_found, expr_wrapped, expr_allowed)

def main():
    root = sys.argv[1] if len(sys.argv) > 1 else 'src/slint'
    findings, c = scan(root)
    for f in findings: print('%s:%d: %s' % f)
    print('slint_text_sites: constructions %d (allow-listed or wrapped %d); %d finding(s)' % (c[0], c[1], len(findings)))
    return 1 if findings else 0

if __name__ == '__main__':
    sys.exit(main())
