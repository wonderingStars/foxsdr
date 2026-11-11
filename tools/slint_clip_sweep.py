#!/usr/bin/env python3
"""The clip sweep (docs/engine-stage4.md 6.62): every page of the Slint window in every language, every text item that does not fit its box.

    python -I tools/slint_clip_sweep.py [--build-dir build] [--languages en,de,...] [--views signal,tools,...] [--jobs 4] [--out sweep.tsv] [--explained tools/slint-clip-explained.tsv]

HOW. foxsdr-slint-probe (cmake -DFOXSDR_SLINT_CLIP_PROBE=ON, target foxsdr-slint-probe) is the window built from a copy of ui/ whose Text elements print a CLIPPROBE line when the text does not
fit (tools/slint-clip-probe.py). Each (language, view) is one bounded run of that window through the drive harness (test_slint_drive_run, FOXSDR_DRIVE_EXE naming the probe) on its scratch profile
- the real settings file is never touched - with FOXSDR_LANGUAGE set and the bank, the open sections or the page of the view; the run's output is read for CLIPPROBE lines. Run it on the hidden
desktop (~/.claude/scripts/run_on_hidden_desktop.ps1): it opens windows.

WHAT IS REPORTED, per (site = file:line of the Text in ui/, language, view, text): its KIND -
    CLIP    does not wrap or elide, its natural width is over its box: the text is cut at the box's edge (or runs on past it)
    ELIDE   overflow: elide and the natural width is over its box: the text ends in an ellipsis
    WRAP    wraps and its natural height at the box's width is over the box: the last lines are cut
    EDGE    the text's box runs past the main window's right edge (its box may be as wide as its text; the row it sits in is wider than the window)
    OUT     the text's box runs past the right edge of the element it sits in (a parent that clips cuts the text there)
A site listed in the explained file (site <TAB> kind <TAB> reason) is EXPLAINED - the reason says why it is meant (an elided text whose full words are in a tooltip, a clip that is a design
choice) - and counted apart. The exit code is 1 while any item is unexplained, so the sweep can gate a change.
"""
import argparse
import concurrent.futures
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# The 33 languages: English (the source) and the catalogues of resources/lang.
def all_languages():
    codes = ['en'] + sorted(p.stem for p in (ROOT / 'resources' / 'lang').glob('*.json'))
    return codes

# One view = the header words of a drive script (bank, open sections, page).
VIEWS = {
    'signal':   'bank=0 sections=0:0,1,2,3,4',
    'decode':   'bank=1',
    'tools':    'bank=2 sections=2:0,1,2,3,4',
    'extend':   'bank=3 sections=3:0,1',
    'system':   'bank=4 sections=4:0,1,2,3,4,5,6,7',
    'transmit': 'bank=0 open=transmit',
    'patch':    'bank=0 view=patch',
    'presets':  'bank=0 view=patch open=patch-presets',
}

# THE MULTI-RADIO VIEWS (screen stage S6): the screen with SEVERAL LIVE RADIOS (generator radios opened through the engine's real RADIO_ADD), at the two small sizes, in both skins. Each name is
# r4-<kind>-<skin>-<size>: kind panes = four radios in 2 x 2 (the bar, the pane headers, the strip's additions, the Audio mix, the status chips); decode = the DECODE bank's plugin drawer with two radios (the Runs-on
# picker); card = the Add-a-radio sheet with the engine's sentence for the sound card on it and a pane that carries a device notice (an rtl_tcp server nobody runs). The skin is the settings seed (fixtures/skin-*.json:
# uiTheme today = Workbench, night = Night Watch); the size is the window's (header size=).
SEEDS = {'workbench': 'skin-workbench.json', 'night': 'skin-night.json'}
SIZES = {'1280': '1280x720', '800': '800x560'}
RADIO_BODIES = {
    'one': ('bank=0', []),   # the BASELINE: the same window with one radio, so what the several radios add is the difference
    'panes': ('bank=0', ['+1 radio add 2 60000', '+1 radio add 3 90000', '+1 radio add 4 120000', '+30 state workspace.radios == 4', '+1 click "2 x 2"', '+20 state workspace.layout == 2']),
    # (a key's label is in the interface language, so the drawer and the sheet are opened by the window's own capture seams, not by clicking their English labels)
    'decode': ('bank=1 plugins=pocsag-decoder,adsb-decoder FOXSDR_SLINT_DECODE_DRAWER=1', ['+1 radio add 2 60000', '+1 radio add 3 90000', '+60 state workspace.radios == 3']),
    'card': ('bank=0 FOXSDR_SLINT_ADD_SHEET=1', ['+1 radio add 2 60000', '+1 radio add 3 90000', '+60 state workspace.radios == 3']),
}
for _kind in RADIO_BODIES:
    for _skin in SEEDS:
        for _size in SIZES:
            VIEWS['r4-%s-%s-%s' % (_kind, _skin, _size)] = None   # filled by view_header / view_body below

# one report: the probe window's Texts carry their room in an accessible-description, main.cpp (FOXSDR_SLINT_PROBE) reads them through the testing API at the end of the run and prints them
# between two marker lines, then CLIPPROBE-WINDOW <w> <h>; a report ends at its END marker because the text of a Text may itself hold a newline
LINE = re.compile(r'CLIPPROBE \|~\| (.*?) \|~\|END', re.DOTALL)


def view_script(view: str, language: str, frames: int, shot: str = '') -> str:
    """The drive script of one (view, language): a one-radio view is its header and the language check; a multi-radio view adds its radios, its skin and its size."""
    if view.startswith('r4-'):
        _, kind, skin, size = view.split('-')
        bank, body = RADIO_BODIES[kind]
        head = '#! frames=%d %s size=%s config=%s FOXSDR_LANGUAGE=%s' % (frames, bank, SIZES[size], SEEDS[skin], language)
        lines = ['10 state ui.language == %s' % language] + body
        if shot:
            lines.append('+5 shot "%s"' % shot)
        return head + '\n# clip sweep: %s in %s\n' % (view, language) + '\n'.join(lines) + '\n'
    return ('#! frames=%d %s FOXSDR_LANGUAGE=%s\n# clip sweep: %s in %s\n%d state ui.language == %s\n') % (frames, VIEWS[view], language, view, language, frames // 2, language)


def run_one(build_dir: Path, language: str, view: str, frames: int):
    script = view_script(view, language, frames, 'sweep-%s-%s.png' % (view, language) if view.startswith('r4-') else '')
    with tempfile.NamedTemporaryFile('w', suffix='.txt', prefix='clipsweep-%s-%s-' % (view, language), delete=False, encoding='utf-8', newline='\n') as f:
        f.write(script)
        path = f.name
    env = dict(os.environ)
    env['FOXSDR_DRIVE_VERBOSE'] = '1'
    env['FOXSDR_DRIVE_EXE'] = str(build_dir / 'src' / 'slint' / 'Release' / 'foxsdr-slint-probe.exe')
    runner = build_dir / 'src' / 'slint' / 'Release' / 'test_slint_drive_run.exe'
    try:
        proc = subprocess.run([str(runner), path], env=env, capture_output=True, timeout=240)
        out = proc.stdout.decode('utf-8', errors='replace') + proc.stderr.decode('utf-8', errors='replace')
    except subprocess.TimeoutExpired:
        out = 'TIMEOUT'
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass
    return parse_log(out, language, view)


def parse_log(out: str, language: str, view: str):
    """(language, view, items, finished, out): the reports of one run's output, one item per (site, text) with the widest overshoot; finished is False when the run printed no log
    (a crash, a timeout, a build that is not the probe)."""
    items = {}
    begin, end = out.find('CLIPPROBE-LOG begin'), out.find('CLIPPROBE-LOG end')
    if not (0 <= begin < end):
        return language, view, [], False, out
    log = out[begin:end]
    win = 0.0
    wm = re.search(r'CLIPPROBE-WINDOW (\d+) (\d+)', log)
    if wm:
        win = float(wm.group(1))
    num = lambda t: float(t.strip().replace(',', '.'))   # Slint formats a number in the language's own decimal separator: fr and de write 39,66667
    for m in LINE.finditer(log):
        parts = m.group(1).split(' |~| ')
        if len(parts) < 10:
            continue
        site, text = parts[0].strip(), parts[1]
        if not text.strip():
            continue
        if len(parts) > 10 and parts[10].strip() == '0':
            continue   # a Text that hides itself (its own visible binding) draws nothing: there is nothing to clip
        box, natural, hbox, hnat = (num(parts[2]), num(parts[3]), num(parts[4]), num(parts[5]))
        elide, wrap = parts[6].strip() == '1', parts[7].strip() == '1'
        x = num(parts[8])
        # the item's overshoot in px, and what kind it is
        edge = x + box - win if win > 0 else 0.0
        if wrap:
            kind, over = ('WRAP', hnat - hbox) if hnat > hbox + 0.5 else ('', 0.0)
        elif natural > box + 0.5:
            kind, over = ('ELIDE' if elide else 'CLIP'), natural - box
        else:
            kind, over = '', 0.0
        if not kind and edge > 0.5:
            kind, over = 'EDGE', edge   # fits its own box, but the box runs past the window's right edge
        if not kind and len(parts) > 12:
            # fits its own box, but the box runs past the right edge of the element it sits in (a parent that clips cuts the text there; a width-less parent is skipped)
            pw = num(parts[12])
            out_by = x + box - (num(parts[11]) + pw)
            if pw >= 1.0 and out_by > 0.5:
                kind, over = 'OUT', out_by
        if not kind:
            continue
        key = (site, text)
        prior = items.get(key)
        if prior is None or over > prior[-1]:
            items[key] = (site, language, view, text, kind, box, natural, over)
    return language, view, list(items.values()), True, out


def load_explained(path: Path):
    """{(site, kind): (reason, box or None)} from the explained file: site, kind, reason and optionally the box width (px) the explanation is for, tab separated; '#' lines are comments.
    The box column keeps an explanation from covering the same site when it is squeezed to some other width (a Caption in a 120 px lane is explained, the same Caption in a 20 px one is not)."""
    explained = {}
    path = Path(path)
    if path.exists():
        for line in path.read_text(encoding='utf-8').splitlines():
            if not line.strip() or line.startswith('#'):
                continue
            cols = line.split('\t')
            if len(cols) >= 3:
                box = float(cols[3]) if len(cols) > 3 and cols[3].strip() else None
                explained[(cols[0], cols[1])] = (cols[2], box)
    return explained


def is_explained(item, explained) -> bool:
    """item is a parse_log tuple (site, language, view, text, kind, box, natural, over)."""
    entry = explained.get((item[0], item[4]))
    if entry is None:
        return False
    return entry[1] is None or abs(item[5] - entry[1]) < 0.5


def main(argv) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--build-dir', default=str(ROOT / 'build'))
    ap.add_argument('--languages', default='all')
    ap.add_argument('--views', default=','.join(VIEWS))
    ap.add_argument('--frames', type=int, default=60)
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--out', default=str(ROOT / 'clip-sweep.tsv'))
    ap.add_argument('--explained', default=str(ROOT / 'tools' / 'slint-clip-explained.tsv'))
    args = ap.parse_args(argv)
    build_dir = Path(args.build_dir)
    languages = all_languages() if args.languages == 'all' else args.languages.split(',')
    views = args.views.split(',')
    explained = load_explained(Path(args.explained))

    jobs = [(l, v) for l in languages for v in views]
    results, failures = [], []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {pool.submit(run_one, build_dir, l, v, args.frames): (l, v) for l, v in jobs}
        for fut in concurrent.futures.as_completed(futures):
            l, v = futures[fut]
            try:
                lang, view, items, ok, out = fut.result()
            except Exception as e:   # a run that could not be made at all
                failures.append((l, v, repr(e)))
                continue
            if not ok:
                failures.append((l, v, out[-400:]))
            results.extend(items)
            print('%-6s %-9s %3d items' % (l, v, len(items)), flush=True)

    results.sort(key=lambda r: (r[0], r[1], r[2], r[3]))
    with open(args.out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('site\tlanguage\tview\tkind\tbox\tnatural\tover\ttext\n')
        for site, lang, view, text, kind, box, natural, over in results:
            f.write('%s\t%s\t%s\t%s\t%.1f\t%.1f\t%.1f\t%s\n' % (site, lang, view, kind, box, natural, over, text.replace('\t', ' ').replace('\n', ' / ')))
    unexplained = [r for r in results if not is_explained(r, explained)]
    sites = sorted({(r[0], r[4]) for r in unexplained})
    by_kind = {}
    for r in results:
        by_kind[r[4]] = by_kind.get(r[4], 0) + 1
    langs_hit = sorted({r[1] for r in unexplained})
    print('SWEEP: %d runs (%d languages x %d views), %d runs that did not finish' % (len(jobs), len(languages), len(views), len(failures)))
    for l, v, why in failures[:10]:
        print('  not finished: %s %s: %s' % (l, v, why.replace('\n', ' ')[:200]))
    print('SWEEP: %d items (%s); %d explained, %d UNEXPLAINED at %d sites in %d languages' %
          (len(results), ', '.join('%s %d' % kv for kv in sorted(by_kind.items())), len(results) - len(unexplained), len(unexplained), len(sites), len(langs_hit)))
    for site, kind in sites[:60]:
        n = sum(1 for r in unexplained if (r[0], r[4]) == (site, kind))
        print('  %-28s %-5s %3d items' % (site, kind, n))
    print('SWEEP: report in %s' % args.out)
    return 1 if (unexplained or failures) else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
