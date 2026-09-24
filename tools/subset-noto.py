"""Cut the embedded fallback faces out of upstream Noto Sans Condensed.

    py -3.14 tools/subset-noto.py <folder holding the two upstream files>

then  py -3.14 tools/embed-fonts.py  to compile them in.

WHAT THIS IS FOR. The bench's own faces (Saira Condensed, Nova Mono, Georgia
on Windows) have no Cyrillic, Georgia has no Vietnamese, and a letter a face
lacks is drawn as a box. Noto Sans Condensed Medium and SemiBold sit behind
them in every role's chain (src/gui/fonts.hpp) and supply those letters on any
machine, Linux included.

WHY A SUBSET IS ALLOWED HERE AND NOT FOR THE OTHER FACES. Noto Sans is under
the SIL Open Font License 1.1 with NO Reserved Font Name (the copyright line in
third_party/fonts/OFL-NotoSans.txt reserves none), so a Modified Version -
which a subset is - may keep its name. Saira and Nova Mono DO reserve theirs,
and stay byte-for-byte upstream (tools/embed-fonts.py says why). The full
static faces are about 425 KB each; this subset, which keeps the alphabets the
catalogues are written in and drops the rest (IPA, polytonic Greek, Cyrillic
Extended-B/C, the layout tables ImGui never reads), is about 120 KB each.

THE UPSTREAM FILES, pinned: github.com/notofonts/notofonts.github.io at commit
28b15b4b43b7bed62b5cf6e6b0b5ff5846270535, fonts/NotoSans/unhinted/ttf/
NotoSans-CondensedMedium.ttf (sha256 f77b0588...a71876) and
NotoSans-CondensedSemiBold.ttf (sha256 b41ecd08...bf9294) - Noto Sans v2.015,
the release notofonts/latin-greek-cyrillic tagged NotoSans-v2.015. Unhinted,
because ImGui's rasteriser ignores hinting and the hinted files are larger.

Needs fontTools (py -3.14 -m pip install fonttools); 4.64 was used.
"""
import os
import subprocess
import sys

OUT = os.path.join('third_party', 'fonts')

# Basic Latin, Latin-1, Latin Extended-A and -B, spacing modifiers and
# combining marks, Greek and Coptic, Cyrillic and its supplement, Latin
# Extended Additional (Vietnamese), general punctuation, super/subscripts,
# currency (the dong), letterlike symbols (the Russian numero sign), number
# forms, arrows, a few mathematical signs, and the dotted circle.
UNICODES = ','.join([
    'U+0020-007E', 'U+00A0-024F', 'U+02B0-036F', 'U+0370-03FF', 'U+0400-052F',
    'U+1E00-1EFF', 'U+2000-206F', 'U+2070-20CF', 'U+2100-218F', 'U+2190-21FF',
    'U+2212', 'U+2215', 'U+2219', 'U+221E', 'U+2248', 'U+2260', 'U+2264',
    'U+2265', 'U+25CC',
])

FACES = [
    ('NotoSans-CondensedMedium.ttf', 'NotoSansCondensed-Medium-subset.ttf'),
    ('NotoSans-CondensedSemiBold.ttf', 'NotoSansCondensed-SemiBold-subset.ttf'),
]


def main():
    if len(sys.argv) != 2:
        sys.stderr.write(__doc__)
        return 2
    src = sys.argv[1]
    if not os.path.isdir(OUT):
        sys.stderr.write('run me from the repository root: %s not found\n' % OUT)
        return 1
    for upstream, subset in FACES:
        path = os.path.join(src, upstream)
        if not os.path.isfile(path):
            sys.stderr.write('missing upstream face: %s\n' % path)
            return 1
        # No layout features: ImGui places glyphs by advance alone and never
        # reads GSUB/GPOS, so they would be bytes nobody uses.
        subprocess.check_call([
            sys.executable, '-m', 'fontTools.subset', path,
            '--unicodes=' + UNICODES,
            '--layout-features=',
            '--no-hinting',
            '--drop-tables+=DSIG',
            '--output-file=' + os.path.join(OUT, subset),
        ])
        print('%s: %d bytes' % (subset, os.path.getsize(os.path.join(OUT, subset))))
    return 0


if __name__ == '__main__':
    sys.exit(main())
