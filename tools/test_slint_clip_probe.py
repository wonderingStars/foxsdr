"""Tests for tools/slint-clip-probe.py (the Text-room instrumentation of the Slint window's ui/) and the log parser of tools/slint_clip_sweep.py.

Every case builds a small ui directory in a temp folder whose name carries the process id (concurrent runs from parallel agents must not collide), runs the transform and reads the copy back.
A last transform case runs over the REAL ui/ and checks that every Text element of the original was instrumented and no line number moved.

Run from the repository root:  py -3.14 -I tools/test_slint_clip_probe.py
"""
import importlib.util
import os
import re
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, file))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


probe = load('slint_clip_probe', 'slint-clip-probe.py')
sweep = load('slint_clip_sweep', 'slint_clip_sweep.py')

TEXT = re.compile(r'(?<![\w-])Text\s*\{')


class ProbeTransform(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix='clipprobe-%d-' % os.getpid())
        self.ui = os.path.join(self.tmp, 'ui')
        self.out = os.path.join(self.tmp, 'out')
        os.makedirs(self.ui)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def write(self, name, text):
        with open(os.path.join(self.ui, name), 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)

    def read(self, name):
        with open(os.path.join(self.out, name), encoding='utf-8') as f:
            return f.read()

    def run_probe(self):
        return probe.main(['slint-clip-probe.py', self.ui, self.out])

    def test_every_text_gets_a_description_with_the_line_number_of_the_original(self):
        self.write('a.slint', 'import { B } from "b.slint";\n\nexport component A {\n    Text { text: "x"; }\n    Rectangle {\n        Text {\n            text: "y";\n        }\n    }\n}\n')
        self.write('b.slint', 'export component B { }\n')
        self.assertEqual(self.run_probe(), 0)
        out = self.read('a.slint')
        self.assertEqual(out.count('accessible-description: "CLIPPROBE |~| '), 2)
        self.assertIn('|~| a.slint:4 |~|', out)       # the first Text is on line 4 of the original
        self.assertIn('|~| a.slint:6 |~|', out)       # the second on line 6
        self.assertEqual(out.count('\n'), 10)         # no line was added: the original has 10 newlines
        self.assertIn('self.preferred-width / 1px', out)
        self.assertIn('self.preferred-height / 1px', out)
        self.assertNotIn('init =>', out)              # no handlers: the description is read when the layout has settled

    def test_a_decorative_text_becomes_visible_to_the_testing_api(self):
        self.write('a.slint', 'export component A {\n    Text { text: "x"; accessible-role: none; }\n    Text { text: "y"; accessible-role: button; }\n}\n')
        self.run_probe()
        out = self.read('a.slint')
        self.assertEqual(out.count('accessible-role: AccessibleRole.text;'), 1)
        self.assertEqual(out.count('accessible-role: none'), 0)
        self.assertEqual(out.count('accessible-role: button;'), 1)    # a role that is not none is left alone

    def test_a_role_none_beyond_the_texts_own_block_is_left_alone(self):
        self.write('a.slint', 'export component A {\n    Text { text: "}{ not a brace"; /* } */ }\n    Rectangle { accessible-role: none; }\n}\n')
        self.run_probe()
        out = self.read('a.slint')
        self.assertEqual(out.count('accessible-role: none;'), 1)      # the Rectangle's
        self.assertEqual(out.count('accessible-description:'), 1)

    def test_a_text_that_already_has_a_description_keeps_it(self):
        self.write('a.slint', 'export component A { Text { accessible-description: "mine"; } Text { } }\n')
        self.run_probe()
        out = self.read('a.slint')
        self.assertEqual(out.count('accessible-description:'), 2)
        self.assertIn('accessible-description: "mine"', out)

    def test_fonts_and_images_become_absolute_and_component_imports_stay_relative(self):
        os.makedirs(os.path.join(self.tmp, 'third_party'))
        os.makedirs(os.path.join(self.ui, 'icons'))
        open(os.path.join(self.tmp, 'third_party', 'F.ttf'), 'wb').close()
        open(os.path.join(self.ui, 'icons', 'i.svg'), 'wb').close()
        self.write('a.slint', 'import "../third_party/F.ttf";\nimport { B } from "b.slint";\nexport component A { Image { source: @image-url("icons/i.svg"); } }\n')
        self.write('b.slint', 'export component B { }\n')
        self.run_probe()
        out = self.read('a.slint')
        self.assertIn('import "%s";' % os.path.join(self.tmp, 'third_party', 'F.ttf').replace('\\', '/'), out)
        self.assertIn('from "b.slint"', out)
        self.assertIn('@image-url("%s")' % os.path.join(self.ui, 'icons', 'i.svg').replace('\\', '/'), out)

    def test_names_that_merely_end_in_text_are_not_text_elements(self):
        self.write('a.slint', 'export component A { NoteText { } HaloText { } TextInput { } Text { } }\n')
        self.run_probe()
        self.assertEqual(self.read('a.slint').count('accessible-description:'), 1)

    def test_a_component_that_inherits_text_is_instrumented_without_a_parent(self):
        # `export component Caption inherits Text { }` IS a Text and has no parent to ask about; a child Text does
        self.write('a.slint', 'export component Caption inherits Text { }\nexport component B { Text { } }\n')
        self.run_probe()
        out = self.read('a.slint')
        self.assertEqual(out.count('accessible-description:'), 2)
        self.assertEqual(out.count('parent.width'), 1)

    def test_the_originals_are_untouched(self):
        body = 'export component A { Text { } }\n'
        self.write('a.slint', body)
        self.run_probe()
        with open(os.path.join(self.ui, 'a.slint'), encoding='utf-8') as f:
            self.assertEqual(f.read(), body)

    def test_the_real_ui_is_fully_instrumented(self):
        ui = os.path.join(ROOT, 'src', 'slint', 'ui')
        out = os.path.join(self.tmp, 'real')
        self.assertEqual(probe.main(['slint-clip-probe.py', ui, out]), 0)
        total = 0
        for name in sorted(os.listdir(ui)):
            if not name.endswith('.slint'):
                continue
            with open(os.path.join(ui, name), encoding='utf-8') as f:
                original = f.read()
            with open(os.path.join(out, name), encoding='utf-8') as f:
                copy = f.read()
            n = len(TEXT.findall(original))
            total += n
            self.assertEqual(copy.count('accessible-description: "CLIPPROBE'), n, name)
            # no line moved: the copy has the original's line count
            self.assertEqual(copy.count('\n'), original.count('\n'), name)
        self.assertGreater(total, 250)


class SweepLog(unittest.TestCase):
    def test_a_report_parses_into_its_columns_and_a_text_may_hold_a_newline(self):
        log = ('CLIPPROBE-LOG begin\n'
               'CLIPPROBE |~| keys.slint:37 |~| FREQUENCE ACCORDEE |~| 80 |~| 118.5 |~| 14 |~| 14 |~| 0 |~| 0 |~| 30 |~| 5 |~| 1 |~| 0 |~| 0 |~|END\n'
               'CLIPPROBE |~| cards.slint:5 |~| two\nlines |~| 40 |~| 40 |~| 14 |~| 28 |~| 0 |~| 1 |~| 1590 |~| 7 |~| 1 |~| 0 |~| 0 |~|END\n'
               'CLIPPROBE-WINDOW 1600 900\n'
               'CLIPPROBE-LOG end\n')
        reports = [m.group(1).split(' |~| ') for m in sweep.LINE.finditer(log)]
        self.assertEqual(len(reports), 2)
        self.assertEqual(reports[0][0], 'keys.slint:37')
        self.assertEqual(reports[0][1], 'FREQUENCE ACCORDEE')
        self.assertEqual(len(reports[0]), 13)
        self.assertEqual(reports[1][1], 'two\nlines')
        self.assertEqual(reports[1][7], '1')

    def test_a_run_without_a_log_is_not_finished(self):
        # no CLIPPROBE-LOG markers (a crash, a timeout, the normal build): not finished, no items
        lang, view, items, finished, _ = sweep.parse_log('foxsdr-slint: bounded run: 60 of 60 ticks, exit 0\n', 'de', 'signal')
        self.assertFalse(finished)
        self.assertEqual(items, [])

    def test_a_decimal_comma_parses(self):
        log = ('CLIPPROBE-LOG begin\n'
               'CLIPPROBE |~| a.slint:1 |~| LONG |~| 79,33333 |~| 118,5 |~| 14 |~| 14 |~| 0 |~| 0 |~| 10 |~| 5 |~| 1 |~| 0 |~| 0 |~|END\n'
               'CLIPPROBE-LOG end\n')
        lang, view, items, finished, _ = sweep.parse_log(log, 'fr', 'signal')
        self.assertTrue(finished)
        self.assertEqual(items[0][4], 'CLIP')
        self.assertEqual(items[0][5], 79.33333)

    def test_the_kinds_the_widest_overshoot_and_what_is_not_reported(self):
        log = ('CLIPPROBE-LOG begin\n'
               # CLIP: no wrap, no elide, natural 118 in a box of 80
               'CLIPPROBE |~| a.slint:1 |~| LONG |~| 80 |~| 118 |~| 14 |~| 14 |~| 0 |~| 0 |~| 10 |~| 5 |~| 1 |~| 0 |~| 0 |~|END\n'
               # the same item seen again (another instance), wider: the larger overshoot wins
               'CLIPPROBE |~| a.slint:1 |~| LONG |~| 80 |~| 130 |~| 14 |~| 14 |~| 0 |~| 0 |~| 10 |~| 5 |~| 1 |~| 0 |~| 0 |~|END\n'
               # ELIDE: overflow elide, natural over the box
               'CLIPPROBE |~| b.slint:2 |~| ELIDED |~| 50 |~| 70 |~| 14 |~| 14 |~| 1 |~| 0 |~| 10 |~| 5 |~| 1 |~| 0 |~| 0 |~|END\n'
               # WRAP: wraps, natural height over the box height
               'CLIPPROBE |~| c.slint:3 |~| WRAPS |~| 100 |~| 100 |~| 14 |~| 42 |~| 0 |~| 1 |~| 10 |~| 5 |~| 1 |~| 0 |~| 0 |~|END\n'
               # EDGE: fits its own box, but the box runs past the window
               'CLIPPROBE |~| d.slint:4 |~| EDGE |~| 60 |~| 60 |~| 14 |~| 14 |~| 0 |~| 0 |~| 1580 |~| 5 |~| 1 |~| 0 |~| 0 |~|END\n'
               # fits: not reported; an empty text: not reported
               'CLIPPROBE |~| e.slint:5 |~| FITS |~| 60 |~| 59 |~| 14 |~| 14 |~| 0 |~| 0 |~| 10 |~| 5 |~| 1 |~| 0 |~| 0 |~|END\n'
               'CLIPPROBE |~| f.slint:6 |~|  |~| 0 |~| 0 |~| 14 |~| 14 |~| 0 |~| 0 |~| 10 |~| 5 |~| 1 |~| 0 |~| 0 |~|END\n'
               'CLIPPROBE-WINDOW 1600 900\n'
               'CLIPPROBE-LOG end\n')
        lang, view, items, finished, _ = sweep.parse_log(log, 'de', 'signal')
        self.assertTrue(finished)
        by_site = {i[0]: i for i in items}
        self.assertEqual(sorted(by_site), ['a.slint:1', 'b.slint:2', 'c.slint:3', 'd.slint:4'])
        self.assertEqual(by_site['a.slint:1'][4], 'CLIP')
        self.assertEqual(by_site['a.slint:1'][6], 130.0)
        self.assertEqual(by_site['b.slint:2'][4], 'ELIDE')
        self.assertEqual(by_site['c.slint:3'][4], 'WRAP')
        self.assertEqual(by_site['d.slint:4'][4], 'EDGE')
        self.assertEqual(by_site['d.slint:4'][7], 40.0)

    def test_a_text_that_hides_itself_is_not_a_clip(self):
        log = ('CLIPPROBE-LOG begin\n'
               'CLIPPROBE |~| a.slint:1 |~| HIDDEN CAPTION |~| 1.5 |~| 31 |~| 14 |~| 14 |~| 1 |~| 0 |~| 10 |~| 5 |~| 0 |~| 0 |~| 0 |~|END\n'
               'CLIPPROBE |~| a.slint:2 |~| SHOWN CAPTION |~| 1.5 |~| 31 |~| 14 |~| 14 |~| 1 |~| 0 |~| 10 |~| 5 |~| 1 |~| 0 |~| 0 |~|END\n'
               'CLIPPROBE-WINDOW 1600 900\n'
               'CLIPPROBE-LOG end\n')
        lang, view, items, finished, _ = sweep.parse_log(log, 'vi', 'signal')
        self.assertEqual([i[0] for i in items], ['a.slint:2'])

    def test_a_text_past_the_edge_of_its_parent_is_out(self):
        log = ('CLIPPROBE-LOG begin\n'
               # fits its own box (natural 40 in 40) but the box starts at 10 and runs to 50, the parent spans 0..44: out by 6
               'CLIPPROBE |~| a.slint:1 |~| AGE |~| 40 |~| 40 |~| 14 |~| 14 |~| 0 |~| 0 |~| 10 |~| 5 |~| 1 |~| 0 |~| 44 |~|END\n'
               # inside its parent: nothing; a parent with no width (a zero-sized container) is skipped
               'CLIPPROBE |~| a.slint:2 |~| INSIDE |~| 40 |~| 40 |~| 14 |~| 14 |~| 0 |~| 0 |~| 10 |~| 5 |~| 1 |~| 0 |~| 60 |~|END\n'
               'CLIPPROBE |~| a.slint:3 |~| ORPHAN |~| 40 |~| 40 |~| 14 |~| 14 |~| 0 |~| 0 |~| 10 |~| 5 |~| 1 |~| 0 |~| 0 |~|END\n'
               'CLIPPROBE-WINDOW 1600 900\n'
               'CLIPPROBE-LOG end\n')
        lang, view, items, finished, _ = sweep.parse_log(log, 'ru', 'signal')
        self.assertEqual([(i[0], i[4], i[7]) for i in items], [('a.slint:1', 'OUT', 6.0)])


class Explained(unittest.TestCase):
    def test_an_explanation_covers_its_site_and_kind_and_the_box_it_names(self):
        explained = {('a.slint:1', 'ELIDE'): ('why', None), ('b.slint:2', 'ELIDE'): ('why', 120.0)}
        item = lambda site, kind, box: (site, 'de', 'signal', 'text', kind, box, 200.0, 80.0)
        self.assertTrue(sweep.is_explained(item('a.slint:1', 'ELIDE', 33.0), explained))
        self.assertFalse(sweep.is_explained(item('a.slint:1', 'CLIP', 33.0), explained))      # another kind at the same site is still reported
        self.assertTrue(sweep.is_explained(item('b.slint:2', 'ELIDE', 120.0), explained))
        self.assertFalse(sweep.is_explained(item('b.slint:2', 'ELIDE', 20.0), explained))     # the same Text squeezed to some other width is not covered
        self.assertFalse(sweep.is_explained(item('c.slint:3', 'ELIDE', 120.0), explained))

    def test_the_explained_file_loads_and_every_site_still_names_a_text(self):
        # a site is file:line of the Text in src/slint/ui; an edit that moves it must not leave the explanation covering some other line
        explained = sweep.load_explained(os.path.join(ROOT, 'tools', 'slint-clip-explained.tsv'))
        self.assertGreaterEqual(len(explained), 5)
        for (site, kind), (reason, box) in explained.items():
            name, line = site.rsplit(':', 1)
            with open(os.path.join(ROOT, 'src', 'slint', 'ui', name), encoding='utf-8') as f:
                lines = f.read().split('\n')
            self.assertTrue(TEXT.search(lines[int(line) - 1]) or re.search(r'inherits\s+Text\s*\{', lines[int(line) - 1]), site)
            self.assertIn(kind, ('ELIDE', 'CLIP', 'WRAP', 'EDGE', 'OUT'), site)
            self.assertGreater(len(reason), 30, site)


if __name__ == '__main__':
    unittest.main()
