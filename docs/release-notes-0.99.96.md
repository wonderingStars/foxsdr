# FoxSDR 0.99.96

**FoxSDR 0.99.96 puts "Listen to the tower" in the patch page's Blueprints list - pick an airport and its tower frequencies land on the canvas, each with its own AM demodulator and speaker - and makes every patch part plainly resizable, with a brass corner you can see and Smaller, Larger and Default size keys.**

This is a feature release; nothing else changes. The Microsoft Store copy stays on its current version until the next submission is certified.

---

**SHA-256 of `foxsdr-setup-0.99.96.exe`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.96-x86_64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.96-linux-x64.tar.gz`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.96-aarch64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.96-linux-arm64.tar.gz`:**
`TBD`

## Features and fixes, 2026-10-27

**Listen to the tower, as a blueprint.** The Blueprints list on the patch page now offers a "Listen to the tower" entry for every airport you have looked up in the Airband section, and - when your receiver position is set - for the six airports nearest it. Picking one places the airport's tower frequencies (or, at a field with no tower, its common traffic frequency) as rows of Channel, AM demodulator with its squelch on, and a speaker that plays through your sound output, with a Spectrum on the radio. The radio is centred to hold as many of them as it can, with none sitting on its centre; any that do not fit in one radio's band are left out, and the note under the list says how many. Press START to listen. Each frequency has a speaker of its own, and a patch channel is not narrowed to an airband channel's width the way the Airband section's LISTEN narrows it.

**Make any patch part larger or smaller.** Every part on the patch canvas - the Map, the Spectrum, the radio, the Text out and the rest - could always be resized from its bottom-right corner, but that corner was three faint hairlines that were easy to miss. It is now a brass corner that lights up under the pointer, with the resize cursor, and the part's own controls no longer reach into it. Selecting a part also shows Smaller, Larger and Default size keys in its panel on the right.
