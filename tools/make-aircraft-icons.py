"""Render resources/aircraft/aircraft_icons.svg into the RGBA pixels the map
and the radar scope draw aircraft with (src/gui/aircraft_icon_pixels.hpp).

WHY PIXELS AND NOT THE SVG AT RUN TIME. The icons are shaded - gradients, glass,
highlights - and Dear ImGui draws flat polygons. Rendering them once, here, with
a real SVG renderer and shipping the result means the application needs no SVG
library and no image decoder: the bytes go straight to glTexImage2D.

WHY 128 PIXELS. The user may choose an icon size up to 96 px (48 is standard),
and a texture drawn no larger than itself (with mipmaps below that) stays
sharp; drawn LARGER than itself it would blur. 128 covers the largest choice.

WHY THE COLOUR IS BLED INTO THE TRANSPARENT PIXELS. A renderer leaves fully
transparent pixels black. Bilinear filtering mixes an edge pixel with its
transparent neighbour, so that black would show as a dark fringe round every
icon at small sizes. Each transparent pixel takes the colour of its nearest
opaque neighbour instead; its alpha stays zero, so nothing new becomes visible.

RENDERER: headless Chrome (the same one the site's favicons are made with - see
the favicon lesson in CLAUDE.md), then Pillow to slice and bleed. Both exist on
the development machine only; the generated header is committed, so nobody
building FoxSDR needs either.

Run from the repository root:  py -3.14 tools/make-aircraft-icons.py
"""
import io
import os
import re
import subprocess
import sys
import tempfile

from PIL import Image

SRC = os.path.join('resources', 'aircraft', 'aircraft_icons.svg')
OUT = os.path.join('src', 'gui', 'aircraft_icon_pixels.hpp')
CHROME = r'C:\Program Files\Google\Chrome\Application\chrome.exe'
SIDE = 128

# The ORDER is the application's AircraftIcon enum (gui/aircraft_icons.hpp);
# test_aircraft_icons checks the names match, so a reordering here cannot
# silently hand a glider the helicopter's pixels.
NAMES = ['airliner', 'heavy', 'light', 'glider', 'microlight', 'uav',
         'balloon', 'surface', 'helicopter', 'unknown', 'helicopterlight']


def render_sheet(svg_text, tmp):
    """One row of every icon at SIDE x SIDE, rendered by Chrome."""
    defs = svg_text[svg_text.index('<defs>'):svg_text.index('</defs>') + len('</defs>')]
    width = SIDE * len(NAMES)
    uses = ''.join('<use xlink:href="#%s" x="%d" y="0" width="%d" height="%d"/>'
                   % (n, i * SIDE, SIDE, SIDE) for i, n in enumerate(NAMES))
    page = ('<!doctype html><html><head><style>html,body{margin:0;background:transparent}'
            'svg{display:block}</style></head><body>'
            '<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" '
            'width="%d" height="%d" viewBox="0 0 %d %d">%s%s</svg></body></html>'
            % (width, SIDE, width, SIDE, defs, uses))
    html = os.path.join(tmp, 'sheet.html')
    png = os.path.join(tmp, 'sheet.png')
    with io.open(html, 'w', encoding='utf-8') as f:
        f.write(page)
    # The window is taller than the row: headless Chrome will not honour a
    # window only 128 pixels high, and the extra is cropped off below.
    subprocess.run([CHROME, '--headless=new', '--disable-gpu', '--hide-scrollbars',
                    '--force-device-scale-factor=1', '--default-background-color=00000000',
                    '--window-size=%d,%d' % (width, SIDE + 200),
                    '--screenshot=' + png, 'file:///' + html.replace('\\', '/')],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    sheet = Image.open(png).convert('RGBA')
    if sheet.size[0] < width or sheet.size[1] < SIDE:
        raise SystemExit('Chrome rendered %dx%d, need at least %dx%d' % (sheet.size + (width, SIDE)))
    return sheet


def bleed(img):
    """Give every fully transparent pixel the colour of an opaque neighbour."""
    w, h = img.size
    px = img.load()
    known = [[px[x, y][3] > 0 for x in range(w)] for y in range(h)]
    frontier = [(x, y) for y in range(h) for x in range(w) if known[y][x]]
    while frontier:
        nxt = []
        for x, y in frontier:
            r, g, b, _ = px[x, y]
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                nx, ny = x + dx, y + dy
                if 0 <= nx < w and 0 <= ny < h and not known[ny][nx]:
                    known[ny][nx] = True
                    px[nx, ny] = (r, g, b, 0)
                    nxt.append((nx, ny))
        frontier = nxt
    return img


def main():
    if not os.path.isfile(SRC):
        sys.stderr.write('run me from the repository root: %s not found\n' % SRC)
        return 1
    svg = io.open(SRC, encoding='utf-8').read()
    found = re.findall(r'<symbol id="([a-z]+)"', svg)
    if sorted(found) != sorted(NAMES):
        sys.stderr.write('symbols in %s are %s, expected %s\n' % (SRC, found, NAMES))
        return 1
    with tempfile.TemporaryDirectory() as tmp:
        sheet = render_sheet(svg, tmp)
    parts = [HEADER % (SIDE, len(NAMES))]
    parts.append('inline constexpr const char* kNames[kCount] = {%s};\n\n'
                 % ', '.join('"%s"' % n for n in NAMES))
    parts.append('inline constexpr unsigned char kRgba[kCount][kSide * kSide * 4] = {\n')
    for i, name in enumerate(NAMES):
        cell = bleed(sheet.crop((i * SIDE, 0, (i + 1) * SIDE, SIDE)))
        data = cell.tobytes()
        opaque = sum(1 for k in range(3, len(data), 4) if data[k] > 0)
        if opaque < 200:
            sys.stderr.write('%s rendered almost empty (%d pixels) - is the symbol id right?\n'
                             % (name, opaque))
            return 1
        parts.append('// %s: %d visible pixels\n{\n' % (name, opaque))
        for r in range(0, len(data), 64):
            parts.append(','.join(str(b) for b in data[r:r + 64]))
            parts.append(',\n')
        parts.append('},\n')
    parts.append('};\n\n')
    parts.append(FOOTER)
    with io.open(OUT, 'w', encoding='utf-8', newline='\n') as f:
        f.write(''.join(parts))
    print('%s: %d icons at %dx%d, %d bytes of header' % (OUT, len(NAMES), SIDE, SIDE,
                                                         os.path.getsize(OUT)))
    return 0


HEADER = '''// aircraft_icon_pixels.hpp - GENERATED by tools/make-aircraft-icons.py from
// resources/aircraft/aircraft_icons.svg. Do not edit by hand; edit the SVG and
// re-run:   py -3.14 tools/make-aircraft-icons.py
//
// Straight (not premultiplied) RGBA, rows top to bottom, each icon a top view
// with the nose up. Transparent pixels carry their neighbour's colour so that
// filtering never pulls black into an edge.
//
// INCLUDE THIS FROM EXACTLY ONE TRANSLATION UNIT (gui/aircraft_icons.cpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_AIRCRAFT_ICON_PIXELS_HPP
#define CASCADE_GUI_AIRCRAFT_ICON_PIXELS_HPP

namespace cascade::gui::aircraft_pixels {

inline constexpr int kSide = %d;
inline constexpr int kCount = %d;
'''

FOOTER = '''}  // namespace cascade::gui::aircraft_pixels

#endif  // CASCADE_GUI_AIRCRAFT_ICON_PIXELS_HPP
'''

if __name__ == '__main__':
    sys.exit(main())
