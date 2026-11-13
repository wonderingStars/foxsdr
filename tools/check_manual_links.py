#!/usr/bin/env python3
"""Link check for the user manual and the documents that link into it.

Usage:  python tools/check_manual_links.py [file.md ...]
Default files: docs/MANUAL.md, README.md, docs/release-notes-0.99.109.md,
docs/silent-install-switches.md.

Checks, for every markdown link [text](target) outside fenced code blocks:
  * a relative file target exists (resolved against the linking file);
  * a #fragment names a heading that exists in the target file, using GitHub's
    slug rules (lower case, punctuation dropped, spaces to hyphens, repeated
    headings get -1, -2 ... suffixes);
  * http(s)/mailto links are NOT fetched (no network); they are only counted.
It also fails when two headings of docs/MANUAL.md would share a slug in a way
that makes a link ambiguous, and when the table of contents at the top of
docs/MANUAL.md lists a heading that does not exist or misses a ## heading.

Exit code 0 = clean, 1 = at least one problem. Python 3 standard library only.
"""
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
DEFAULT_FILES = [
    "docs/MANUAL.md",
    "README.md",
    "docs/release-notes-0.99.109.md",
    "docs/silent-install-switches.md",
]

LINK_RE = re.compile(r"(?<!\!)\[(?:[^\[\]]|\[[^\]]*\])*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")
HEADING_RE = re.compile(r"^(#{1,6})\s+(.*?)\s*#*\s*$")
FENCE_RE = re.compile(r"^\s*(```|~~~)")


def slugify(text):
    """GitHub's heading slug: strip markup, lower-case, drop punctuation, spaces to '-'."""
    text = re.sub(r"`([^`]*)`", r"\1", text)
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)
    text = re.sub(r"<[^>]+>", "", text)
    text = text.replace("*", "")
    text = text.strip().lower()
    out = []
    for ch in text:
        if ch.isalnum() or ch in ("-", "_"):
            out.append(ch)
        elif ch == " ":
            out.append("-")
        # every other punctuation character is dropped
    return "".join(out)


def headings_of(path):
    """Ordered list of (level, text, slug) with GitHub duplicate suffixes."""
    seen = {}
    result = []
    in_fence = False
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            if FENCE_RE.match(line):
                in_fence = not in_fence
                continue
            if in_fence:
                continue
            m = HEADING_RE.match(line)
            if not m:
                continue
            base = slugify(m.group(2))
            n = seen.get(base, 0)
            seen[base] = n + 1
            slug = base if n == 0 else "%s-%d" % (base, n)
            result.append((len(m.group(1)), m.group(2), slug))
    return result


_cache = {}


def slugs_of(path):
    if path not in _cache:
        _cache[path] = {s for (_l, _t, s) in headings_of(path)}
    return _cache[path]


def links_of(path):
    in_fence = False
    with open(path, encoding="utf-8") as fh:
        for lineno, line in enumerate(fh, 1):
            if FENCE_RE.match(line):
                in_fence = not in_fence
                continue
            if in_fence:
                continue
            for m in LINK_RE.finditer(line):
                yield lineno, m.group(1)


def check_file(rel):
    path = os.path.join(ROOT, rel)
    problems = []
    external = 0
    internal = 0
    if not os.path.isfile(path):
        return ["%s: file does not exist" % rel], 0, 0
    for lineno, target in links_of(path):
        if re.match(r"^(https?:|mailto:)", target):
            external += 1
            continue
        internal += 1
        frag = None
        if "#" in target:
            target, frag = target.split("#", 1)
        if target == "":
            dest = path
        else:
            dest = os.path.normpath(os.path.join(os.path.dirname(path), target))
        if not os.path.exists(dest):
            problems.append("%s:%d: target does not exist: %s" % (rel, lineno, target))
            continue
        if frag:
            if os.path.isdir(dest) or not dest.lower().endswith(".md"):
                continue  # a fragment into a non-markdown file is not checked
            if frag not in slugs_of(dest):
                problems.append("%s:%d: no heading #%s in %s" % (rel, lineno, frag, os.path.relpath(dest, ROOT)))
    return problems, internal, external


def check_toc(rel="docs/MANUAL.md"):
    """The contents list must name every '## ' heading, and nothing else."""
    path = os.path.join(ROOT, rel)
    problems = []
    if not os.path.isfile(path):
        return problems
    heads = headings_of(path)
    h2 = [(t, s) for (lvl, t, s) in heads if lvl == 2]
    # the contents list sits between the '## Contents' heading and the next '## '.
    lines = open(path, encoding="utf-8").read().splitlines()
    start = None
    for i, line in enumerate(lines):
        if re.match(r"^##\s+Contents\s*$", line):
            start = i + 1
            break
    if start is None:
        return ["%s: no '## Contents' heading" % rel]
    toc = []
    for line in lines[start:]:
        if line.startswith("## "):
            break
        m = re.match(r"^\s*[-*]\s+\[[^\]]*\]\(#([^)]+)\)", line)
        if m:
            toc.append(m.group(1))
    slugs = {s for (_t, s) in h2}
    for s in toc:
        if s not in slugs:
            problems.append("%s: contents entry #%s is not a '## ' heading" % (rel, s))
    for (t, s) in h2:
        if s == "contents":
            continue
        if s not in toc:
            problems.append("%s: '## %s' (#%s) is missing from the contents list" % (rel, t, s))
    return problems


def main(argv):
    files = argv[1:] or [f for f in DEFAULT_FILES if os.path.isfile(os.path.join(ROOT, f))]
    allp = []
    tot_i = tot_e = 0
    for rel in files:
        p, i, e = check_file(rel)
        allp += p
        tot_i += i
        tot_e += e
        print("%-40s internal links %4d  external (not fetched) %4d  problems %d" % (rel, i, e, len(p)))
    if "docs/MANUAL.md" in files:
        allp += check_toc()
    for p in allp:
        print("PROBLEM " + p)
    print("TOTAL: %d internal links, %d external links not fetched, %d problem(s)" % (tot_i, tot_e, len(allp)))
    return 1 if allp else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
