# FoxSDR MSIX logo asset generator.
#
# Emits every PNG the package manifest and the Store ask for, at every size,
# INTO A DIRECTORY GIVEN ON THE COMMAND LINE:
#
#   py -3.14 installer\msix\make-logos.py <output dir>
#
# WHY IT RENDERS RATHER THAN RESAMPLES. resources/icon/generate_icon.py draws
# the mark from normalised geometry and supersamples before it downscales, so
# every size is drawn at that size. Resampling the checked-in 256 px PNG down
# to 44 px would instead blur a rendition that was tuned for 256 - and the
# generator deliberately SIMPLIFIES below 32 px (four spectrum bars become two,
# because four bars plus gaps across ~7 usable pixels is under a pixel each).
# A resample cannot know that, so it would ship an unreadable 16 px icon.
# Importing render() gets the per-size decisions for free.
#
# It is the SAME rendition as the .ico the installer ships, the window icon and
# the site's mark - padding, tile and all. Cropping the mark to its bounding
# box to make it read larger at 16 px was tried on the website favicon and
# rejected: the icon a user recognises has to be one icon everywhere.
#
# SIZES ARE NOT INVENTED HERE. Every number below comes from
# https://learn.microsoft.com/en-us/windows/apps/design/iconography/app-icon-construction
# (ms.date 2026-07-21, updated_at 2026-08-05), "How icon sizes relate to the
# MSIX manifest":
#
#   Manifest entry     | Base | 100% | 125% | 150% | 200% | 400%
#   Square44x44Logo    |  44  |  44  |  55  |  66  |  88  |  176
#   Square71x71Logo    |  71  |  71  |  89  | 107  | 142  |  284
#   Square150x150Logo  | 150  | 150  | 188  | 225  | 300  |  600
#   Square310x310Logo  | 310  | 310  | 388  | 465  | 620  | 1240
#   Wide310x150Logo    |310x150|310x150|388x188|465x225|620x300|1240x600
#   StoreLogo          |  50  |  50  |  63  |  75  | 100  |  200
#
# and the same page's required lists:
#   - "Package logo (Microsoft Store logo) ... These assets are required to
#      publish to the Microsoft Store" - StoreLogo.scale-100/125/150/200/400
#   - "Windows 11 does not use the tile assets, but currently at minimum the
#      Medium tile assets at 100% are required to publish to the Microsoft
#      Store"
#   - "App List Target Size (Required)" - targetsize 16/20/24/30/32/36/40/48/
#      60/64/72/80/96/256, plus the _altform-unplated set: "If you do not
#      include the targetsize-*-altform-unplated assets above your icon will
#      scale to a smaller size and will get an undesirable backplate behind
#      the icon on Taskbar and Start."
#
# The qualifier-suffixed filenames (Square44x44Logo.scale-200.png,
# Square44x44Logo.targetsize-16_altform-unplated.png) are resolved at runtime
# by resources.pri, which build-msix.ps1 builds with makepri.exe. Without that
# index only the unqualified names are used, which is why makepri is not
# optional here.
#
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

import importlib.util
import os
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
GENERATOR = os.path.join(REPO, "resources", "icon", "generate_icon.py")

# Scale factors Windows asks for, and the required minimum set. 100/200/400 is
# the documented floor ("At minimum, provide assets at 100%, 200%, and 400%
# scale"); all five are emitted because they cost nothing and remove every
# intermediate rescale.
SCALES = [100, 125, 150, 200, 400]

# The square manifest entries and their base sizes.
SQUARE_ENTRIES = {
    "Square44x44Logo": 44,
    "Square71x71Logo": 71,
    "Square150x150Logo": 150,
    "Square310x310Logo": 310,
    "StoreLogo": 50,
}

# Non-square entries: (base width, base height).
WIDE_ENTRIES = {
    "Wide310x150Logo": (310, 150),
    "SplashScreen": (620, 300),
}

# The app-list target sizes, from the "App List Target Size (Required)" list.
TARGET_SIZES = [16, 20, 24, 30, 32, 36, 40, 48, 60, 64, 72, 80, 96, 256]


def load_generator():
    """Import resources/icon/generate_icon.py by path.

    By PATH rather than by package name, because resources/icon is not a
    Python package and never should become one just to satisfy an import.
    """
    if not os.path.isfile(GENERATOR):
        raise SystemExit("cannot find the icon generator at " + GENERATOR)
    spec = importlib.util.spec_from_file_location("foxsdr_generate_icon", GENERATOR)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    if not hasattr(module, "render"):
        raise SystemExit(GENERATOR + " has no render(size) - the icon generator changed shape")
    return module


def scaled(base, scale):
    """The pixel size for `base` at `scale` percent.

    Rounded to nearest, matching the published table: 44 at 125% is 55, and
    150 at 125% is 188 (187.5 rounded up), so a floor would disagree with
    Microsoft's own numbers on half the rows.
    """
    return int(round(base * scale / 100.0))


def wide_canvas(mark, width, height):
    """The square mark centred on a `width` x `height` transparent canvas.

    The mark is drawn at the canvas HEIGHT, not at some fraction of it: the
    mark already carries its own padding inside a rounded tile, so insetting it
    again would leave the wide tile looking like a small icon adrift in a
    field. The tile's own BackgroundColor in the manifest is the same slate the
    mark is drawn on, so the seam does not show.
    """
    canvas = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    m = mark.resize((height, height), Image.LANCZOS) if mark.size[0] != height else mark
    canvas.paste(m, ((width - height) // 2, 0), m)
    return canvas


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: make-logos.py <output dir>")
    out = os.path.abspath(sys.argv[1])
    os.makedirs(out, exist_ok=True)

    gen = load_generator()

    # Every distinct square pixel size this run needs, rendered ONCE. A 44 px
    # mark is a 44 px mark whether it is being written as scale-100 of
    # Square44x44Logo or as targetsize-44.
    wanted = set()
    for base in SQUARE_ENTRIES.values():
        for s in SCALES:
            wanted.add(scaled(base, s))
    for s in TARGET_SIZES:
        wanted.add(s)
    for (w, h) in WIDE_ENTRIES.values():
        for s in SCALES:
            wanted.add(scaled(h, s))

    marks = {}
    for size in sorted(wanted):
        marks[size] = gen.render(size)

    written = 0

    def save(img, name):
        nonlocal written
        img.save(os.path.join(out, name), format="PNG")
        written += 1

    # Square entries, every scale. scale-100 is ALSO written under the bare
    # name, because that is the filename the manifest references and the one
    # used when no resources.pri is present (the Add-AppxPackage -Register
    # path, which is how the layout is tested before it is packed).
    for name, base in SQUARE_ENTRIES.items():
        for s in SCALES:
            px = scaled(base, s)
            save(marks[px], "%s.scale-%d.png" % (name, s))
            if s == 100:
                save(marks[px], "%s.png" % name)

    # The app-list icon's target sizes, plated and unplated.
    #
    # The unplated copies are BYTE-IDENTICAL to the plated ones, and that is
    # deliberate rather than lazy: FoxSDR's mark is a filled slate tile with
    # the fox cut into it, so it is already its own plate. Supplying them is
    # what stops Windows adding a SECOND plate behind it on the taskbar.
    for px in TARGET_SIZES:
        save(marks[px], "Square44x44Logo.targetsize-%d.png" % px)
        save(marks[px], "Square44x44Logo.targetsize-%d_altform-unplated.png" % px)
        save(marks[px], "Square44x44Logo.targetsize-%d_altform-lightunplated.png" % px)

    # Wide tile and splash screen.
    for name, (bw, bh) in WIDE_ENTRIES.items():
        for s in SCALES:
            w = scaled(bw, s)
            h = scaled(bh, s)
            img = wide_canvas(marks[h], w, h)
            save(img, "%s.scale-%d.png" % (name, s))
            if s == 100:
                save(img, "%s.png" % name)

    print("make-logos: wrote %d PNGs into %s" % (written, out))

    # A size assertion, because a silently mis-sized asset is exactly the kind
    # of thing certification rejects days later. Every file is reopened and
    # its dimensions checked against the name it was given.
    bad = 0
    for fn in sorted(os.listdir(out)):
        if not fn.lower().endswith(".png"):
            continue
        with Image.open(os.path.join(out, fn)) as im:
            w, h = im.size
        stem = fn[:-4]
        expect = None
        for name, base in SQUARE_ENTRIES.items():
            if stem == name:
                expect = (base, base)
            elif stem.startswith(name + ".scale-"):
                s = int(stem.split(".scale-")[1])
                expect = (scaled(base, s), scaled(base, s))
            elif stem.startswith(name + ".targetsize-"):
                n = int(stem.split(".targetsize-")[1].split("_")[0])
                expect = (n, n)
        for name, (bw, bh) in WIDE_ENTRIES.items():
            if stem == name:
                expect = (bw, bh)
            elif stem.startswith(name + ".scale-"):
                s = int(stem.split(".scale-")[1])
                expect = (scaled(bw, s), scaled(bh, s))
        if expect is None:
            print("  UNCHECKED %s (%dx%d)" % (fn, w, h))
            bad += 1
        elif (w, h) != expect:
            print("  WRONG SIZE %s: %dx%d, expected %dx%d" % (fn, w, h, expect[0], expect[1]))
            bad += 1
    if bad:
        raise SystemExit("make-logos: %d asset(s) failed the size check" % bad)
    print("make-logos: all %d assets are the size their name claims" % written)


if __name__ == "__main__":
    main()
