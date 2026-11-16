#!/usr/bin/env python3
"""errpath_seed.py - build (or re-build) the error-path allow list from the scanner's own output.

The gate (tests/test_error_paths.cpp, scanner tests/errpath_scan.hpp) finds every error path in the
source and requires each to be reported, allowed with a reason, or PENDING. This tool writes the
allow list (tests/errpath_allow.tsv) so the gate starts green and can only get better:

  1. it runs the scanner (`<scanner> <repo> --dump <file>`): every site, its key and whether a
     reporting call already covers it (those need no entry);
  2. it joins the remaining sites to the survey's verdicts (error-paths.tsv, 12 tab-separated
     columns: repository, file, line, first100, verdict, event_or_reason, rank, site_type, function,
     group, basis, key) WITHOUT line numbers: by file, kind, the last component of the function
     name and the text of the site, then looser, then by order inside one function;
       ALLOW                  -> an ALLOW entry carrying the survey's reason
       MUST REPORT / UNSURE   -> PENDING:<the survey's group>
       REPORTS (not covered   -> PENDING:reports-not-in-function (the survey believed a reporter is
        by the reporter rule)    reached; the scanner finds none in a block that encloses the site)
       no survey row          -> PENDING:unmatched (new code since the survey; classify it)
  3. when an allow file already exists (the normal case after a merge) its entries are KEPT for
     every site that still exists, re-keyed when the same function and text moved to another file,
     and dropped when the site is gone; only new unresolved sites are classified.  --fresh starts
     from the survey alone.

It prints what it could not match (sites with no survey row; survey rows with no site; entries
dropped) and the PENDING count per group.  --write-pin rewrites the `kPendingPin` line of the test
(or the --pin-file) to the new PENDING count; it REFUSES to raise the pin unless --allow-raise is
given (a re-seed after a merge brings in other lanes' new sites; raising is a decision).

Nothing here depends on line numbers.  Re-run in one command after a merge:

  python tools/errpath_seed.py --scanner <build>/tests/Release/test_error_paths.exe --write-pin

SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""
import argparse
import collections
import csv
import os
import re
import subprocess
import sys
import tempfile

KIND_MAP = {"ignored2": "ignored"}      # the survey split one kind in two
HEADER = (
    "# tests/errpath_allow.tsv - the error-path gate's allow list (tools/errpath_seed.py writes it).\n"
    "# key<TAB>verdict<TAB>reason<TAB>added.  key = file::function::kind::stem (NO line numbers).\n"
    "# verdict: ALLOW (reason: a code or >= 12 characters) | DETAIL-OF:<key> | PENDING:<group> (pinned count, may only go down).\n"
)


def die(msg):
    sys.stderr.write("errpath_seed: " + msg + "\n")
    sys.exit(2)


def norm_snippet(s):
    s = re.sub(r"\s+", "", s)
    while True:
        m = re.match(r"^[A-Za-z_][A-Za-z0-9_]*(::|->|\.)", s)
        if not m:
            break
        s = s[m.end():]
    return s


def compat(a, b):
    """two normalised snippets are the same site's text, one possibly cut shorter (the survey cut at a line end)"""
    n = min(len(a), len(b), 40)
    return n >= 6 and a[:n] == b[:n]


def last_fn(fn):
    return fn.split("::")[-1] if fn else fn


def clean(s):
    return re.sub(r"[\t\r\n]+", " ", s).strip()


def run_scanner(scanner, repo, extra):
    fd, path = tempfile.mkstemp(suffix=".tsv", prefix="errpath_sites_")
    os.close(fd)
    try:
        cmd = [scanner, repo, "--dump", path] + extra
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
        if p.returncode != 0:
            die("the scanner failed (%d): %s%s" % (p.returncode, p.stdout, p.stderr))
        sites = []
        with open(path, encoding="utf-8", newline="") as f:
            for line in f:
                c = line.rstrip("\n").split("\t")
                if len(c) < 8:
                    continue
                sites.append(dict(key=c[0], file=c[1], function=c[2], kind=c[3], reported=c[4] == "1",
                                  reporter=c[5], snippet=c[6]))
        return sites
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass


def load_survey(path, repo_name):
    rows = []
    with open(path, encoding="utf-8", newline="") as f:
        for i, c in enumerate(csv.reader(f, delimiter="\t", quoting=csv.QUOTE_NONE)):
            if i == 0 or len(c) < 12 or c[0] != repo_name:
                continue
            rows.append(dict(file=c[1], line=int(c[2]) if c[2].isdigit() else 0, first=c[3], verdict=c[4], reason=c[5],
                             kind=KIND_MAP.get(c[7], c[7]), fn=c[8], group=c[9], key=c[11], used=False))
    return rows


def join(sites, survey):
    """Pair each site with at most one survey row. No line number of the CURRENT tree is used (the survey's own
    lines only order the rows of one file)."""
    pairs = {}
    groups = collections.defaultdict(list)
    for r in survey:
        r["n"] = norm_snippet(r["first"])
        groups[(r["file"], r["kind"])].append(r)
    for lst in groups.values():
        lst.sort(key=lambda r: r["line"])
    by_file_kind = collections.defaultdict(list)
    for s in sites:
        s["n"] = norm_snippet(s["snippet"])
        by_file_kind[(s["file"], s["kind"])].append(s)
    for gk, ss in by_file_kind.items():
        rows = groups.get(gk, [])
        if not rows:
            continue
        # pass 1: same function (last component) and same text; pass 2: same text; pass 3: same function, in order
        for mode in (1, 2):
            for s in ss:
                if s["key"] in pairs:
                    continue
                for r in rows:
                    if r["used"] or not compat(s["n"], r["n"]):
                        continue
                    if mode == 1 and last_fn(r["fn"]) != last_fn(s["function"]):
                        continue
                    r["used"] = True
                    pairs[s["key"]] = r
                    break
        left_s = collections.defaultdict(list)
        for s in ss:
            if s["key"] not in pairs:
                left_s[last_fn(s["function"])].append(s)
        left_r = collections.defaultdict(list)
        for r in rows:
            if not r["used"]:
                left_r[last_fn(r["fn"])].append(r)
        for fn, sl in left_s.items():
            rl = left_r.get(fn, [])
            if rl and len(rl) == len(sl):
                for s, r in zip(sl, rl):
                    r["used"] = True
                    pairs[s["key"]] = r
    return pairs


def parse_allow(path):
    out = []
    if not path or not os.path.exists(path):
        return out
    with open(path, encoding="utf-8", newline="") as f:
        for line in f:
            line = line.rstrip("\r\n")
            if not line.strip() or line.startswith("#"):
                continue
            c = line.split("\t")
            if len(c) < 3:
                continue
            out.append(dict(key=c[0], verdict=c[1], reason=c[2], added=c[3] if len(c) > 3 else ""))
    return out


def tail(key):
    p = key.find("::")
    return key[p + 2:] if p >= 0 else key


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scanner", required=True, help="the built test_error_paths (or errpath_check) executable")
    ap.add_argument("--repo", default=".", help="repository root to scan (default .)")
    ap.add_argument("--survey", default="D:/foxsdr-build/prep/error-paths.tsv", help="the survey's error-paths.tsv")
    ap.add_argument("--survey-repo", default="engine", help="value of the survey's repository column (engine | plugins)")
    ap.add_argument("--out", default=None, help="allow list to write (default <repo>/tests/errpath_allow.tsv)")
    ap.add_argument("--fresh", action="store_true", help="ignore the existing allow list; start from the survey")
    ap.add_argument("--scanner-arg", action="append", default=[], help="extra argument for the scanner (repeatable)")
    ap.add_argument("--write-pin", action="store_true", help="rewrite the pinned PENDING count to the new count")
    ap.add_argument("--pin-file", default=None, help="a file holding the pin (default: the kPendingPin line of <repo>/tests/test_error_paths.cpp)")
    ap.add_argument("--allow-raise", action="store_true", help="let --write-pin RAISE the pin (a decision, not a habit)")
    ap.add_argument("--added", default="seed", help="the 'added' column for new lines")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()

    repo = os.path.abspath(a.repo)
    out_path = a.out or os.path.join(repo, "tests", "errpath_allow.tsv")
    sites = run_scanner(a.scanner, repo, a.scanner_arg)
    if not sites:
        die("the scanner found no site: wrong --repo or --scanner")
    survey = load_survey(a.survey, a.survey_repo) if os.path.exists(a.survey) else []
    if not survey:
        sys.stderr.write("errpath_seed: no survey rows for '%s' in %s: every unresolved site becomes PENDING:unmatched\n" % (a.survey_repo, a.survey))
    existing = [] if a.fresh else parse_allow(out_path)

    by_key = {s["key"]: s for s in sites}
    entries = {}        # key -> (verdict, reason, added)
    kept = moved = dropped = 0
    dropped_list = []
    # 1. keep what exists
    stale = []
    for e in existing:
        if e["key"] in by_key:
            entries[e["key"]] = (e["verdict"], e["reason"], e["added"])
            kept += 1
        else:
            stale.append(e)
    new_tail = collections.defaultdict(list)
    for s in sites:
        if s["key"] not in entries and not s["reported"]:
            new_tail[tail(s["key"])].append(s)
    for e in stale:
        c = new_tail.get(tail(e["key"]))
        if c:
            s = c.pop(0)
            entries[s["key"]] = (e["verdict"], e["reason"], e["added"])
            moved += 1
        else:
            dropped += 1
            dropped_list.append(e["key"])
    # 2. classify what is still unresolved
    pairs = join(sites, survey) if survey else {}
    unmatched = []
    counts = collections.Counter()
    disagree = collections.Counter()
    for s in sites:
        r = pairs.get(s["key"])
        if r is not None:
            counts["site matched to a survey row"] += 1
            if s["reported"] and r["verdict"] != "REPORTS":
                disagree["scanner: passes by reporter; survey: " + r["verdict"]] += 1
            if not s["reported"] and r["verdict"] == "REPORTS":
                disagree["scanner: no reporter encloses the site; survey: REPORTS"] += 1
        else:
            counts["site with no survey row"] += 1
        if s["reported"] or s["key"] in entries:
            continue
        if r is None:
            unmatched.append(s)
            entries[s["key"]] = ("PENDING:unmatched", "not in the survey (new code or a text the join could not pair): classify it", a.added)
            continue
        v = r["verdict"]
        if v == "ALLOW":
            reason = clean(r["reason"])
            if len(reason) < 12:
                reason = "survey ALLOW: " + reason
            entries[s["key"]] = ("ALLOW", reason, a.added)
        elif v in ("MUST REPORT", "UNSURE"):
            grp = clean(r["group"]) or ("unsure" if v == "UNSURE" else "ungrouped")
            entries[s["key"]] = ("PENDING:" + grp, clean(("UNSURE: " if v == "UNSURE" else "") + r["reason"]), a.added)
        elif v == "REPORTS":
            entries[s["key"]] = ("PENDING:reports-not-in-function", "survey: REPORTS (" + clean(r["reason"]) + ") but no reporting call encloses the site", a.added)
        else:
            entries[s["key"]] = ("PENDING:unmatched", "survey verdict '" + v + "' not understood", a.added)

    # 3. write
    lines = sorted(entries.items())
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(HEADER)
        for k, (v, reason, added) in lines:
            f.write("%s\t%s\t%s\t%s\n" % (k, v, clean(reason), added))
    pend = collections.Counter(v[len("PENDING:"):] for (v, _, _) in entries.values() if v.startswith("PENDING:"))
    npend = sum(pend.values())
    nallow = sum(1 for (v, _, _) in entries.values() if not v.startswith("PENDING:"))
    nrep = sum(1 for s in sites if s["reported"])
    kinds = collections.Counter(s["kind"] for s in sites)
    print("sites %d (%s)" % (len(sites), ", ".join("%s %d" % kv for kv in sorted(kinds.items()))))
    print("  pass by a reporter: %d   ALLOW/DETAIL-OF lines: %d   PENDING lines: %d   allow file lines: %d" % (nrep, nallow, npend, len(entries)))
    print("  kept %d existing lines, re-keyed %d moved, dropped %d stale" % (kept, moved, dropped))
    for g, n in sorted(pend.items(), key=lambda kv: -kv[1]):
        print("  PENDING %-34s %d" % (g, n))
    print("could not match:")
    print("  sites with no survey row: %d" % len(unmatched))
    for s in unmatched[:15]:
        print("    %s  [%s] %s" % (s["file"], s["kind"], s["snippet"][:70]))
    if len(unmatched) > 15:
        print("    ... %d more (all are PENDING:unmatched in the file)" % (len(unmatched) - 15))
    left = [r for r in survey if not r["used"] and r["kind"] != "chokepoint"]
    print("  survey rows with no scanner site: %d (the survey was partly mechanical: the CODE decides)" % len(left))
    kc = collections.Counter(r["kind"] for r in left)
    print("    by kind: " + ", ".join("%s %d" % kv for kv in sorted(kc.items())))
    if dropped_list:
        print("  existing lines dropped (site gone): %d" % len(dropped_list))
        for k in dropped_list[:10]:
            print("    " + k)
    for k, n in sorted(disagree.items()):
        print("  disagreement: %s: %d" % (k, n))
    print("wrote %s" % out_path)

    if a.write_pin:
        if a.pin_file:
            cur = None
            if os.path.exists(a.pin_file):
                m = re.search(r"\d+", open(a.pin_file, encoding="utf-8").read())
                cur = int(m.group(0)) if m else None
            if cur is not None and npend > cur and not a.allow_raise:
                die("the pin would RISE from %d to %d: re-run with --allow-raise if that is intended" % (cur, npend))
            with open(a.pin_file, "w", encoding="utf-8", newline="\n") as f:
                f.write("%d\n" % npend)
            print("pin file %s = %d" % (a.pin_file, npend))
        else:
            tp = os.path.join(repo, "tests", "test_error_paths.cpp")
            txt = open(tp, encoding="utf-8", newline="").read()
            m = re.search(r"(constexpr int kPendingPin = )(\d+)(;[^\n]*errpath-pin)", txt)
            if not m:
                die("no 'constexpr int kPendingPin = N;   // errpath-pin' line in " + tp)
            cur = int(m.group(2))
            if npend > cur and cur != 0 and not a.allow_raise:
                die("the pin would RISE from %d to %d: re-run with --allow-raise if that is intended" % (cur, npend))
            txt = txt[:m.start(2)] + str(npend) + txt[m.end(2):]
            with open(tp, "w", encoding="utf-8", newline="") as f:
                f.write(txt)
            print("kPendingPin %d -> %d in %s" % (cur, npend, tp))
    return 0


if __name__ == "__main__":
    sys.exit(main())
