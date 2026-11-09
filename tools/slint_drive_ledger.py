#!/usr/bin/env python3
"""The drive suite's coverage ledger (docs/engine-stage4.md 6.47): every row of docs/slint-port-tick.md classified.

    python -I tools/slint_drive_ledger.py            writes docs/slint-drive-coverage.md
    python -I tools/slint_drive_ledger.py --check    exit 1 when the committed ledger is stale or a row is unclassified (the ctest)

THE CLASSES, per row of the inventory tick list:
    ASSERTED      a drive script operates the control and reads the effect back (the engine's state, the host's settings half, or the control's own value)
    DRIVEN        a drive script operates the control but nothing can be read back - the detail says why
    NOT DRIVABLE  the row cannot be driven by the scripted input - the detail says why (a readout, not a control; needs hardware; needs a
                  file dialog; withheld in a bounded run; deferred feature; no label in the element tree; ...)

THE SOURCES. src/slint/tests/drive/coverage-<author>.tsv (one file per author, all of them read) is the authors' word, one line per row:  <row>\t<class>\t<script>\t<detail>
(class ASSERTED | DRIVEN | NOT_DRIVABLE; script = the drive script's file name, or - ; several lines for one row are merged, the strongest
class wins). A row NOT in the tsv is classified by rule: a readout-like type (readout, lamp, tooltip, caption, overlay, column, status row,
message, panel, list, track, building block) is NOT DRIVABLE "a readout, not a control"; a DEFERRED row is NOT DRIVABLE "deferred: <note>";
a RELOCATED row is NOT DRIVABLE "relocated: <note>". Any other row left out is UNCLASSIFIED, and --check fails on it: no silent gaps.
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TICK = ROOT / "docs" / "slint-port-tick.md"
TSV_DIR = ROOT / "src" / "slint" / "tests" / "drive"   # every coverage-<author>.tsv in it is read
OUT = ROOT / "docs" / "slint-drive-coverage.md"

# The inventory's control types that are not controls (exact tokens after the spaces are removed, lower-case).
READOUT_TYPES = {
    "readout", "lamp/indicator", "tooltip", "caption", "overlay/marker", "column", "statusrow", "statusrows", "message", "messagelist",
    "panel", "panelrow", "panelrows", "list", "maptrack", "buildingblock", "layout", "label", "heading", "strip", "banner", "engraving",
    "lamp", "indicator", "chip", "picture", "image", "plate", "text", "note", "hint", "legend", "graticule", "trace", "axis", "scale",
    "readout(mono)", "readout(dim)", "status", "row", "rows", "table", "tablerow", "tablecolumn", "dialog", "window", "page", "sheet",
    "group", "groupcaption", "section", "well", "face", "frame", "well/plot", "plot", "canvas", "map", "listrow", "cardrow",
    "meter", "indicator", "maptrackflag", "maptrack(path)", "mappath", "panelheading", "heading+rows", "envvar(notui)", "rowfeed", "mark",
    "readout+lamp", "readout/lamp", "panelrow(heading)", "panelrow(warn)", "instrumentflag(alert)", "-", "behaviour",
}
# The plugin pages' own rows (the "Plugins: ..." sections) need a fitted plugin: the drive run fits none, the per-plugin smoke (docs 6.49)
# drives them and lists them in coverage.tsv; until then they are NOT DRIVABLE for this reason, never silently.
PLUGIN_SECTION_PREFIX = "Plugins: "
CLASS_RANK = {"ASSERTED": 3, "DRIVEN": 2, "NOT_DRIVABLE": 1}
CLASS_WORD = {"ASSERTED": "DRIVEN+ASSERTED", "DRIVEN": "DRIVEN", "NOT_DRIVABLE": "NOT DRIVABLE", "UNCLASSIFIED": "UNCLASSIFIED"}


def read_rows():
    """[(section, row, control, type, status, note)] in file order; section = the '## ' heading above the row."""
    rows = []
    section = ""
    for line in TICK.read_text(encoding="utf-8").splitlines():
        if line.startswith("## "):
            section = line[3:].strip()
            continue
        m = re.match(r"^\|\s*(\d+)\s*\|(.*)\|\s*$", line)
        if not m:
            continue
        cells = [c.strip() for c in m.group(2).split("|")]
        if len(cells) < 4:
            continue
        rows.append((section, int(m.group(1)), cells[0], cells[1], cells[2], "|".join(cells[3:])))
    return rows


def read_tsv():
    """{row: (class, script, detail)} - the strongest class per row, the details joined."""
    out = {}
    for tsv in sorted(TSV_DIR.glob("coverage-*.tsv")):
        for n, line in enumerate(tsv.read_text(encoding="utf-8").splitlines(), 1):
            if not line.strip() or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 4:
                sys.exit(f"{tsv}:{n}: four tab-separated fields wanted (row, class, script, detail): {line!r}")
            try:
                row = int(parts[0])
            except ValueError:
                sys.exit(f"{tsv}:{n}: the row is a number: {parts[0]!r}")
            cls = parts[1].strip()
            if cls not in CLASS_RANK:
                sys.exit(f"{tsv}:{n}: the class is ASSERTED, DRIVEN or NOT_DRIVABLE, not {cls!r}")
            script, detail = parts[2].strip(), "\t".join(parts[3:]).strip()
            if row in out:
                pcls, pscript, pdetail = out[row]
                if CLASS_RANK[cls] > CLASS_RANK[pcls]:
                    out[row] = (cls, script, detail + ("; " + pdetail if pdetail else ""))
                else:
                    out[row] = (pcls, pscript, pdetail + ("; " + detail if detail else ""))
            else:
                out[row] = (cls, script, detail)
    return out


def classify(rows, tsv):
    """[(section, row, control, type, class, script, detail)]"""
    out = []
    for section, row, control, typ, status, note in rows:
        if row in tsv:
            cls, script, detail = tsv[row]
        else:
            t = typ.replace(" ", "").lower()
            if t in READOUT_TYPES or t.startswith("readout") or t.startswith("lamp") or t.startswith("tooltip") or t.startswith("overlay") or t.startswith("instrumentface"):
                cls, script, detail = "NOT_DRIVABLE", "-", "a readout, not a control"
            elif section.startswith(PLUGIN_SECTION_PREFIX):
                cls, script, detail = "NOT_DRIVABLE", "-", "no script step for this row (the plugin is fitted in the 6.49 runs; a decode, a track or a picture needs 6.50's fixture, which this plugin has not)"
            elif status == "DEFERRED":
                cls, script, detail = "NOT_DRIVABLE", "-", "deferred: " + note
            elif status == "RELOCATED":
                cls, script, detail = "NOT_DRIVABLE", "-", "relocated: " + note
            else:
                cls, script, detail = "UNCLASSIFIED", "-", f"{typ} ({status}) - not in any coverage-*.tsv"
        out.append((section, row, control, typ, cls, script, detail))
    return out


def reason_class(detail):
    d = detail.lower()
    if "readout, not a control" in d:
        return "readout"
    if d.startswith("deferred"):
        return "deferred"
    if d.startswith("relocated"):
        return "relocated"
    if "hardware" in d or "radio" in d and "needs" in d:
        return "needs hardware"
    if "file dialog" in d or "file chooser" in d or "reads a file" in d or "a file" in d and "needs" in d:
        return "needs a file"
    if "bounded run" in d or "withheld" in d or "shell" in d or "clipboard" in d or "network" in d:
        return "withheld in a bounded run"
    if "not installed" in d:
        return "not installed"
    if "no script step for this row" in d:
        return "plugin row without a step"
    if "needs a signal" in d:
        return "needs a signal (no fixture)"
    if "no plugin" in d or "plugin fitted" in d:
        return "no plugin fitted"
    if "label" in d and ("dump" in d or "tree" in d):
        return "no label in the element tree"
    return "other"


def render(classified):
    lines = []
    lines.append("# Slint drive coverage - the ledger of every inventory row (generated by tools/slint_drive_ledger.py, docs/engine-stage4.md 6.47)")
    lines.append("")
    lines.append("Every row of `docs/slint-port-tick.md` against the drive suite (`src/slint/tests/drive/*.txt`, one ctest each, run on the hidden desktop by")
    lines.append("`test_slint_drive_run`). **DRIVEN+ASSERTED** = a script operates the control and reads the effect back (the engine's state, the host's")
    lines.append("settings half or the control's own value); **DRIVEN** = operated, nothing to read back (the detail says why); **NOT DRIVABLE** = the")
    lines.append("scripted input cannot reach it (the detail says why). The authors' word is `src/slint/tests/drive/coverage-<author>.tsv`; the rows not in it are")
    lines.append("classified by rule (a readout-like type, a DEFERRED or RELOCATED row). `ctest -R slint_drive_ledger` fails when this file is stale or a row")
    lines.append("is unclassified.")
    lines.append("")
    total = {"ASSERTED": 0, "DRIVEN": 0, "NOT_DRIVABLE": 0, "UNCLASSIFIED": 0}
    reasons_total = {}
    per_section = []
    cur = None
    for section, row, control, typ, cls, script, detail in classified:
        if cur is None or cur[0] != section:
            cur = [section, {"ASSERTED": 0, "DRIVEN": 0, "NOT_DRIVABLE": 0, "UNCLASSIFIED": 0}, {}, []]
            per_section.append(cur)
        cur[1][cls] += 1
        total[cls] += 1
        if cls == "NOT_DRIVABLE":
            r = reason_class(detail)
            cur[2][r] = cur[2].get(r, 0) + 1
            reasons_total[r] = reasons_total.get(r, 0) + 1
        cur[3].append((row, control, typ, cls, script, detail))
    n = sum(total.values())
    controls = n - reasons_total.get("readout", 0)
    lines.append("## Totals")
    lines.append("")
    lines.append(f"| rows | DRIVEN+ASSERTED | DRIVEN | NOT DRIVABLE | UNCLASSIFIED |")
    lines.append(f"|---|---|---|---|---|")
    lines.append(f"| {n} | {total['ASSERTED']} | {total['DRIVEN']} | {total['NOT_DRIVABLE']} | {total['UNCLASSIFIED']} |")
    lines.append("")
    lines.append(f"Of the {controls} rows that are controls (the {reasons_total.get('readout', 0)} readout-like rows left out): "
                 f"{total['ASSERTED']} asserted, {total['DRIVEN']} driven without a read-back, {total['NOT_DRIVABLE'] - reasons_total.get('readout', 0)} not drivable.")
    lines.append("")
    lines.append("NOT DRIVABLE by reason: " + ", ".join(f"{k} {v}" for k, v in sorted(reasons_total.items(), key=lambda kv: -kv[1])) + ".")
    lines.append("")
    lines.append("## Per section")
    lines.append("")
    lines.append("| section | rows | DRIVEN+ASSERTED | DRIVEN | NOT DRIVABLE | of which readouts | UNCLASSIFIED |")
    lines.append("|---|---|---|---|---|---|---|")
    for section, counts, reasons, _ in per_section:
        lines.append(f"| {section} | {sum(counts.values())} | {counts['ASSERTED']} | {counts['DRIVEN']} | {counts['NOT_DRIVABLE']} | {reasons.get('readout', 0)} | {counts['UNCLASSIFIED']} |")
    lines.append("")
    lines.append("## Every row")
    lines.append("")
    for section, counts, reasons, items in per_section:
        lines.append(f"### {section}")
        lines.append("")
        lines.append("| # | control | type | class | script | detail |")
        lines.append("|---|---|---|---|---|---|")
        for row, control, typ, cls, script, detail in items:
            d = detail.replace("|", "/")
            lines.append(f"| {row} | {control.replace('|', '/')} | {typ.replace('|', '/')} | {CLASS_WORD[cls]} | {script} | {d} |")
        lines.append("")
    return "\n".join(lines) + "\n", total


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="exit 1 when the committed ledger is stale or a row is unclassified")
    args = ap.parse_args()
    rows = read_rows()
    if not rows:
        sys.exit(f"no rows read from {TICK}")
    tsv = read_tsv()
    unknown = sorted(r for r in tsv if r not in {row for _, row, *_ in rows})
    if unknown:
        sys.exit(f"a coverage-*.tsv names rows that are not in the tick list: {unknown[:20]}")
    classified = classify(rows, tsv)
    text, total = render(classified)
    unclassified = [(row, control) for _, row, control, _, cls, _, _ in classified if cls == "UNCLASSIFIED"]
    if args.check:
        bad = False
        if unclassified:
            print(f"{len(unclassified)} rows unclassified (not in any coverage-*.tsv, not a readout, not deferred or relocated):")
            for row, control in unclassified[:50]:
                print(f"  {row}: {control}")
            bad = True
        current = OUT.read_text(encoding="utf-8") if OUT.exists() else ""
        if current != text:
            print(f"{OUT} is stale: run python -I tools/slint_drive_ledger.py")
            bad = True
        print(f"ledger: {len(rows)} rows, {total['ASSERTED']} asserted, {total['DRIVEN']} driven, {total['NOT_DRIVABLE']} not drivable, {total['UNCLASSIFIED']} unclassified")
        sys.exit(1 if bad else 0)
    OUT.write_text(text, encoding="utf-8")
    print(f"wrote {OUT}: {len(rows)} rows, {total['ASSERTED']} asserted, {total['DRIVEN']} driven, {total['NOT_DRIVABLE']} not drivable, {total['UNCLASSIFIED']} unclassified")
    if unclassified:
        print(f"  {len(unclassified)} UNCLASSIFIED rows - the first: " + ", ".join(f"{r} {c}" for r, c in unclassified[:15]))


if __name__ == "__main__":
    main()
