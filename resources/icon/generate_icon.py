# FoxSDR application icon generator.
#
# Emits, next to this script:
#   foxsdr.ico             multi-resolution 32-bit BGRA icon (16/24/32/48/64/128/256)
#   foxsdr-256.png         256 px PNG preview so a human can see the mark
#   foxsdr_icon_rgba.hpp   raw RGBA pixel arrays (16/32/48) for glfwSetWindowIcon
#
# Run:  py -3.14 resources\icon\generate_icon.py
# Requires Pillow (pip install pillow). Nothing in the build depends on this
# script or on Python -- the three generated artefacts are checked in, and the
# script exists so the icon is reproducible rather than a mystery binary.
#
# The mark: an angular fox head (two triangular ears over a wide-cheeked head
# tapering to a pointed muzzle) in warm fox orange on a dark slate tile, with a
# four-bar spectrum trace cut out of the brow in the tile colour so the bars
# read as negative space. At 16 and 24 px a four-bar trace is physically
# unresolvable -- four bars plus gaps across ~7 usable pixels is under a pixel
# each -- so those two sizes drop to two equal bars, which read as eyes and keep
# the fox legible. That per-size simplification is deliberate; see README.md.
#
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

import io
import os
import struct

from PIL import Image, ImageDraw

# --- Palette -----------------------------------------------------------------
# Warm fox orange on the app's dark slate. Flat fills only: a gradient at 16 px
# averages to mud, and two flat colours give the highest possible contrast
# ratio (4.5:1 here) at every size.
ORANGE = (0xE8, 0x72, 0x2C, 0xFF)
SLATE = (0x1B, 0x1F, 0x26, 0xFF)

ICO_SIZES = [16, 24, 32, 48, 64, 128, 256]
# Sizes whose RGBA pixels are embedded for the runtime GLFW window icon. GLFW
# picks the closest match for the title-bar (small) and Alt-Tab/taskbar (big)
# slots, so 16/32/48 covers every slot Windows asks for without interpolation.
RGBA_SIZES = [16, 32, 48]

# --- Geometry, in normalised 0..1 coordinates (y grows downward) --------------

TILE_RADIUS = 0.20

# One closed polygon: left ear tip, ear notch, right ear tip, right cheek,
# muzzle point, left cheek. The outer ear edges continue straight into the
# cheeks, which is what gives the head its angular, non-cartoon read.
HEAD = [
    (0.115, 0.060),  # left ear tip
    (0.335, 0.300),  # ear notch, left side
    (0.665, 0.300),  # ear notch, right side
    (0.885, 0.060),  # right ear tip
    (0.905, 0.480),  # right cheek (widest point)
    (0.500, 0.945),  # muzzle
    (0.095, 0.480),  # left cheek
]

# Four-bar spectrum, cut into the brow. Bars share a baseline and vary in
# height; the tallest sits second from the left so the profile reads as a
# signal peak rather than a staircase.
BARS_X0, BARS_X1 = 0.255, 0.745
BARS_BASELINE = 0.615
BARS_MAX_HEIGHT = 0.290
BAR_HEIGHTS = [0.55, 1.00, 0.72, 0.88]
BAR_GAP_RATIO = 0.70  # gap width as a fraction of bar width

# Two-bar fallback for 16/24 px: equal heights, wider apart.
SMALL_X0, SMALL_X1 = 0.300, 0.700
SMALL_TOP, SMALL_BOTTOM = 0.360, 0.545
SMALL_GAP_RATIO = 0.80

# Below this pixel size the four-bar trace cannot hold a 2 px minimum bar
# width, so the two-bar variant is used instead.
SMALL_VARIANT_MAX_PX = 24


def _bar_rects(n, x0, x1, gap_ratio):
    """Evenly spaced bar spans across [x0, x1] with gap = bar * gap_ratio."""
    span = x1 - x0
    bar = span / (n + (n - 1) * gap_ratio)
    gap = bar * gap_ratio
    return [(x0 + i * (bar + gap), x0 + i * (bar + gap) + bar) for i in range(n)]


def render(size):
    """Render the mark at `size` px as an RGBA image."""
    # Supersample then Lanczos down: PIL's polygon fill is hard-edged, and the
    # ears and muzzle are diagonals that alias badly without it.
    ss = 8 if size <= 64 else 4
    n = size * ss
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    def px(p):
        return (p[0] * n, p[1] * n)

    d.rounded_rectangle([0, 0, n - 1, n - 1], radius=TILE_RADIUS * n, fill=SLATE)
    d.polygon([px(p) for p in HEAD], fill=ORANGE)

    if size <= SMALL_VARIANT_MAX_PX:
        rects = _bar_rects(2, SMALL_X0, SMALL_X1, SMALL_GAP_RATIO)
        for bx0, bx1 in rects:
            d.rectangle([bx0 * n, SMALL_TOP * n, bx1 * n - 1, SMALL_BOTTOM * n - 1],
                        fill=SLATE)
    else:
        rects = _bar_rects(4, BARS_X0, BARS_X1, BAR_GAP_RATIO)
        for (bx0, bx1), h in zip(rects, BAR_HEIGHTS):
            top = BARS_BASELINE - h * BARS_MAX_HEIGHT
            d.rectangle([bx0 * n, top * n, bx1 * n - 1, BARS_BASELINE * n - 1],
                        fill=SLATE)

    return img.resize((size, size), Image.LANCZOS)


# --- ICO container ------------------------------------------------------------
#
# Written by hand rather than via Image.save(format="ICO"): PIL's ICO writer
# resizes ONE source image to every requested size, which would throw away the
# per-size simplification above. Sizes up to 128 are stored as 32-bit BI_RGB
# DIBs (the universally supported form); 256 is stored as PNG, the convention
# every Windows shell since Vista expects and what keeps the file small.


def dib_bytes(img):
    w, h = img.size
    hdr = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, w * h * 4, 0, 0, 0, 0)
    px = img.load()
    xor = bytearray()
    for y in range(h - 1, -1, -1):  # DIB rows are bottom-up
        for x in range(w):
            r, g, b, a = px[x, y]
            xor += bytes((b, g, r, a))
    # 1bpp AND mask, rows padded to 4 bytes. All zero: alpha in the XOR data is
    # what actually masks a 32-bit icon, but the mask must still be present and
    # correctly sized or the shell rejects the entry.
    stride = ((w + 31) // 32) * 4
    return bytes(hdr) + bytes(xor) + bytes(stride * h)


def write_ico(path, images):
    blobs = []
    for img in images:
        if img.size[0] >= 256:
            buf = io.BytesIO()
            img.save(buf, format="PNG")
            blobs.append(buf.getvalue())
        else:
            blobs.append(dib_bytes(img))

    offset = 6 + 16 * len(images)
    out = bytearray(struct.pack("<HHH", 0, 1, len(images)))
    for img, blob in zip(images, blobs):
        w, h = img.size
        out += struct.pack("<BBBBHHII", w & 0xFF, h & 0xFF, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
    for blob in blobs:
        out += blob
    with open(path, "wb") as f:
        f.write(bytes(out))


# --- Runtime RGBA header ------------------------------------------------------


def write_rgba_header(path, images):
    lines = [
        "// FoxSDR window-icon pixels -- GENERATED by resources/icon/generate_icon.py.",
        "// Do not edit by hand; re-run the generator instead.",
        "//",
        "// Raw RGBA8 (GLFW's glfwSetWindowIcon pixel layout: one byte each of R, G,",
        "// B, A, rows top-to-bottom). Embedded as plain arrays so the runtime window",
        "// icon costs the build ZERO image-decoding dependencies -- adding a PNG",
        "// decoder to a tree whose whole premise is a small, licence-audited",
        "// dependency set would be a poor trade for 14 KB of pixels.",
        "//",
        "// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0",
        "#pragma once",
        "",
        "namespace cascade::gui::icon {",
        "",
    ]
    for img in images:
        w, _ = img.size
        data = img.convert("RGBA").tobytes()
        lines.append(f"// {w}x{w}, RGBA8, {len(data)} bytes")
        lines.append(f"inline constexpr int kSize{w} = {w};")
        lines.append(f"inline constexpr unsigned char kPixels{w}[{len(data)}] = {{")
        for i in range(0, len(data), 16):
            chunk = ",".join(f"0x{b:02X}" for b in data[i:i + 16])
            lines.append("    " + chunk + ",")
        lines.append("};")
        lines.append("")
    lines.append("}  // namespace cascade::gui::icon")
    lines.append("")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    images = {s: render(s) for s in sorted(set(ICO_SIZES) | set(RGBA_SIZES))}

    write_ico(os.path.join(here, "foxsdr.ico"), [images[s] for s in ICO_SIZES])
    images[256].save(os.path.join(here, "foxsdr-256.png"), format="PNG")
    write_rgba_header(os.path.join(here, "foxsdr_icon_rgba.hpp"),
                      [images[s] for s in RGBA_SIZES])

    for name in ("foxsdr.ico", "foxsdr-256.png", "foxsdr_icon_rgba.hpp"):
        p = os.path.join(here, name)
        print(f"wrote {p} ({os.path.getsize(p)} bytes)")


if __name__ == "__main__":
    main()
