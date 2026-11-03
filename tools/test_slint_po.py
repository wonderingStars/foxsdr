"""Tests for tools/slint-po.py (the resources/lang -> gettext .po bridge of the Slint window).

Every case builds a small ui directory and a small catalogue directory in a temp folder whose name carries
the process id (concurrent runs from parallel agents must not collide), runs the generator's main() on
them and reads the .po files back.

Run from the repository root:  py -3.14 -I tools/test_slint_po.py
"""
import importlib.util
import io
import json
import os
import shutil
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location('slint_po', os.path.join(HERE, 'slint-po.py'))
slint_po = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(slint_po)


def run(ui, lang, out, *extra):
    err = io.StringIO()
    outp = io.StringIO()
    with redirect_stderr(err), redirect_stdout(outp):
        rc = slint_po.main([ui, lang, out] + list(extra))
    return rc, outp.getvalue(), err.getvalue()


class Fixture(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix='slint-po-test-%d-' % os.getpid())
        self.ui = os.path.join(self.root, 'ui')
        self.lang = os.path.join(self.root, 'lang')
        self.out = os.path.join(self.root, 'out')
        os.makedirs(os.path.join(self.ui, 'specimens'))
        os.makedirs(self.lang)

    def tearDown(self):
        shutil.rmtree(self.root, ignore_errors=True)

    def slint(self, name, text):
        path = os.path.join(self.ui, name)
        with open(path, 'w', encoding='utf-8', newline='') as f:
            f.write(text)

    def catalogue(self, code, strings, english='X'):
        with open(os.path.join(self.lang, code + '.json'), 'w', encoding='utf-8') as f:
            json.dump({'code': code, 'englishName': english, 'machine': True, 'name': code, 'strings': strings},
                      f, ensure_ascii=False, indent=1)

    def po(self, code):
        path = os.path.join(self.out, code, 'LC_MESSAGES', 'foxsdr-slint.po')
        with open(path, 'rb') as f:
            return f.read().decode('utf-8')


class Cases(Fixture):
    def test_plain_argument_and_placeholder(self):
        self.slint('a.slint', 'component A { Text { text: @tr("Band plan"); }\n'
                              'Text { text: @tr("EVENT {}", seq); }\n'
                              'Text { text: @tr("{} of {} used", a, b); } }\n')
        self.catalogue('fr', {'Band plan': 'Plan de bande', 'EVENT %d': 'ÉVÉNEMENT %d', '%s of %s used': '%s sur %s utilisés'}, 'French')
        rc, out, err = run(self.ui, self.lang, self.out)
        self.assertEqual(rc, 0, err)
        po = self.po('fr')
        self.assertIn('msgid "Band plan"\nmsgstr "Plan de bande"\n', po)
        self.assertIn('msgid "EVENT {}"\nmsgstr "ÉVÉNEMENT {}"\n', po)
        self.assertIn('msgid "{} of {} used"\nmsgstr "{} sur {} utilisés"\n', po)
        self.assertIn('fr     msgids 3  exact 1  placeholder 2  left out 0', out)

    def test_header(self):
        self.slint('a.slint', '@tr("x")')
        self.catalogue('ru', {'x': 'икс'}, 'Russian')
        self.catalogue('ja', {'x': 'エックス'}, 'Japanese')
        rc, _, err = run(self.ui, self.lang, self.out)
        self.assertEqual(rc, 0, err)
        ru = self.po('ru')
        self.assertIn('"Language: ru\\n"', ru)
        self.assertIn('"Content-Type: text/plain; charset=UTF-8\\n"', ru)
        self.assertIn('"Plural-Forms: nplurals=3; plural=(n%10==1', ru)
        self.assertIn('"Plural-Forms: nplurals=1; plural=(0);\\n"', self.po('ja'))

    def test_key_without_a_value_is_left_out(self):
        self.slint('a.slint', 'x: @tr("Has"); y: @tr("Hasnt"); z: @tr("Empty");')
        self.catalogue('de', {'Has': 'Hat', 'Empty': ''})
        rc, out, _ = run(self.ui, self.lang, self.out)
        self.assertEqual(rc, 0)
        po = self.po('de')
        self.assertIn('msgid "Has"', po)
        self.assertNotIn('Hasnt', po)
        self.assertNotIn('msgid "Empty"', po)
        self.assertIn('exact 1  placeholder 0  left out 2', out)

    def test_no_english_catalogue_is_needed_and_none_is_written(self):
        self.slint('a.slint', '@tr("x")')
        self.catalogue('de', {'x': 'y'})
        run(self.ui, self.lang, self.out)
        self.assertEqual(sorted(os.listdir(self.out)), ['de'])

    def test_plural(self):
        self.slint('a.slint', 'a: @tr("{} file" | "{} files" % n);')
        self.catalogue('de', {'%d file': '%d Datei', '%d files': '%d Dateien'})
        self.catalogue('ja', {'%d file': '%d ファイル', '%d files': '%d 個のファイル'})
        self.catalogue('ru', {'%d file': '%d файл', '%d files': '%d файлов'})
        self.catalogue('pl', {'%d file': '%d plik'})   # the plural form has no value
        rc, out, _ = run(self.ui, self.lang, self.out)
        self.assertEqual(rc, 0)
        self.assertIn('msgid "{} file"\nmsgid_plural "{} files"\nmsgstr[0] "{} Datei"\nmsgstr[1] "{} Dateien"\n', self.po('de'))
        ja = self.po('ja')
        self.assertIn('msgstr[0] "{} 個のファイル"', ja)
        self.assertNotIn('msgstr[1]', ja)
        # three plural forms cannot be filled from a singular and a plural sentence: English instead
        self.assertNotIn('msgid "{} file"', self.po('ru'))
        self.assertIn('1 plural left out: more than two forms', out)
        self.assertNotIn('msgid "{} file"', self.po('pl'))

    def test_escaping(self):
        self.slint('a.slint', 'a: @tr("say \\"hi\\" \\\\ ok");\nb: @tr("two\\nlines");\nc: @tr("tab\\there");')
        self.catalogue('de', {'say "hi" \\ ok': 'sag "hallo" \\ ok', 'two\nlines': 'zwei\nZeilen', 'tab\there': 'Tab\thier'})
        rc, _, err = run(self.ui, self.lang, self.out)
        self.assertEqual(rc, 0, err)
        po = self.po('de')
        self.assertIn('msgid "say \\"hi\\" \\\\ ok"\nmsgstr "sag \\"hallo\\" \\\\ ok"\n', po)
        self.assertIn('msgid ""\n"two\\n"\n"lines"\nmsgstr ""\n"zwei\\n"\n"Zeilen"\n', po)
        self.assertIn('msgstr "Tab\\thier"', po)

    def test_braces_in_a_translation_are_doubled(self):
        self.slint('a.slint', 'a: @tr("Braces");')
        self.catalogue('de', {'Braces': 'a {b} c'})
        run(self.ui, self.lang, self.out)
        self.assertIn('msgstr "a {{b}} c"', self.po('de'))

    def test_specimens_comments_and_strings_are_not_read(self):
        self.slint(os.path.join('specimens', 's.slint'), 'x: @tr("In a specimen");')
        self.slint('a.slint', '// @tr("In a line comment")\n/* @tr("In a block comment") */\n'
                              'text: "an @tr(\\"inside a string\\") here";\nreal: @tr("Real");\n'
                              'name: @trouble("not it");')
        self.catalogue('de', {'In a specimen': 'x', 'In a line comment': 'x', 'In a block comment': 'x',
                              'inside a string': 'x', 'Real': 'Echt', 'not it': 'x'})
        rc, out, _ = run(self.ui, self.lang, self.out)
        self.assertEqual(rc, 0)
        po = self.po('de')
        entries = [l for l in po.splitlines() if l.startswith('msgid ') and l != 'msgid ""']   # the header's own msgid is not an entry
        self.assertEqual(entries, ['msgid "Real"'], po)
        self.assertIn('1 msgids', out)

    def test_context_form_is_warned_about(self):
        self.slint('a.slint', 'a: @tr("menu" => "Open");')
        self.catalogue('de', {'Open': 'Öffnen'})
        rc, _, err = run(self.ui, self.lang, self.out)
        self.assertEqual(rc, 0)
        self.assertIn('with a context', err)
        self.assertIn('msgctxt "menu"\nmsgid "Open"\nmsgstr "Öffnen"', self.po('de'))

    def test_same_msgid_twice_is_one_entry(self):
        self.slint('a.slint', 'a: @tr("Twice"); b: @tr("Twice");')
        self.catalogue('de', {'Twice': 'Zweimal'})
        run(self.ui, self.lang, self.out)
        self.assertEqual(self.po('de').count('msgid "Twice"'), 1)

    def test_conversions_that_braces_cannot_carry_do_not_match(self):
        self.slint('a.slint', 'a: @tr("Gain {} dB", g);')
        self.catalogue('de', {'Gain %.1f dB': 'Verstärkung %.1f dB'})
        _, out, _ = run(self.ui, self.lang, self.out)
        self.assertNotIn('msgid "Gain {} dB"', self.po('de'))
        self.assertIn('exact 0  placeholder 0  left out 1', out)

    def test_two_runs_give_the_same_bytes(self):
        self.slint('a.slint', 'a: @tr("Zebra"); b: @tr("Apple {}", n); c: @tr("{} egg" | "{} eggs" % n);')
        self.slint('b.slint', 'c: @tr("Mango");')
        self.catalogue('fr', {'Zebra': 'Zèbre', 'Apple %d': 'Pomme %d', 'Mango': 'Mangue', '%d egg': '%d oeuf', '%d eggs': '%d oeufs'})
        self.catalogue('de', {'Zebra': 'Zebra'})
        out2 = os.path.join(self.root, 'out2')
        run(self.ui, self.lang, self.out)
        run(self.ui, self.lang, out2)
        for code in ('fr', 'de'):
            with open(os.path.join(self.out, code, 'LC_MESSAGES', 'foxsdr-slint.po'), 'rb') as a, \
                    open(os.path.join(out2, code, 'LC_MESSAGES', 'foxsdr-slint.po'), 'rb') as b:
                self.assertEqual(a.read(), b.read())
        po = self.po('fr')
        self.assertLess(po.index('msgid "Apple {}"'), po.index('msgid "Zebra"'))   # sorted by msgid

    def test_a_second_run_rewrites_nothing_and_leaves_the_stamp(self):
        self.slint('a.slint', 'a: @tr("Zebra");')
        self.catalogue('fr', {'Zebra': 'Zèbre'})
        stamp = os.path.join(self.root, 'stamp', 'translations.stamp')
        run(self.ui, self.lang, self.out, '--stamp', stamp)
        self.assertTrue(os.path.exists(stamp))
        po = os.path.join(self.out, 'fr', 'LC_MESSAGES', 'foxsdr-slint.po')
        os.utime(stamp, (1000, 1000))
        os.utime(po, (2000, 2000))
        _, out, _ = run(self.ui, self.lang, self.out, '--stamp', stamp)
        self.assertEqual(os.stat(stamp).st_mtime, 1000)    # nothing changed: the Slint compiler is not woken
        self.assertEqual(os.stat(po).st_mtime, 2000)
        self.assertIn('0 .po files written', out)
        self.catalogue('fr', {'Zebra': 'Zébre'})
        run(self.ui, self.lang, self.out, '--stamp', stamp)
        self.assertGreater(os.stat(stamp).st_mtime, 1000)  # a changed translation touches it

    def test_a_removed_language_does_not_stay_bundled(self):
        self.slint('a.slint', 'a: @tr("x");')
        self.catalogue('fr', {'x': 'y'})
        self.catalogue('de', {'x': 'z'})
        run(self.ui, self.lang, self.out)
        os.remove(os.path.join(self.lang, 'de.json'))
        run(self.ui, self.lang, self.out)
        self.assertFalse(os.path.exists(os.path.join(self.out, 'de', 'LC_MESSAGES', 'foxsdr-slint.po')))
        self.assertTrue(os.path.exists(os.path.join(self.out, 'fr', 'LC_MESSAGES', 'foxsdr-slint.po')))

    def test_a_missing_directory_is_an_error(self):
        rc, _, err = run(os.path.join(self.root, 'nope'), self.lang, self.out)
        self.assertEqual(rc, 1)
        self.assertIn('not a directory', err)

    def test_every_catalogue_code_has_a_plural_rule(self):
        # the real resources/lang: a language added without a Plural-Forms row would get the default silently
        lang = os.path.join(os.path.dirname(HERE), 'resources', 'lang')
        if not os.path.isdir(lang):
            self.skipTest('resources/lang not found beside tools/')
        codes = sorted(f[:-5] for f in os.listdir(lang) if f.endswith('.json'))
        self.assertEqual([c for c in codes if c not in slint_po.PLURALS], [])


if __name__ == '__main__':
    unittest.main(verbosity=2)
