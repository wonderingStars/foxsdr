# FoxSDR 0.99.96

**FoxSDR 0.99.96 puts "Listen to the tower" in the patch page's Blueprints list - pick an airport and its tower frequencies land on the canvas, each with its own AM demodulator and speaker - and makes every patch part plainly resizable, with a brass corner you can see and Smaller, Larger and Default size keys.**

This is a feature release; nothing else changes. The Microsoft Store copy stays on its current version until the next submission is certified.

---

**SHA-256 of `foxsdr-setup-0.99.96.exe`:**
`e03def7bf26a66d3ccab7312e382b65a780fc4c6a865d7a98645d6cae37110e4`

**SHA-256 of `FoxSDR-0.99.96-x86_64.AppImage`:**
`9feea38dc7e3a3a4135103a2c399302acd39222d06a733e8fe31d5cb7abc4cc6`

**SHA-256 of `foxsdr-0.99.96-linux-x64.tar.gz`:**
`219d9f6bf58e06ec39d6597def4819a761acc1c23cbc5bcd0858b74f8c9ca606`

**SHA-256 of `FoxSDR-0.99.96-aarch64.AppImage`:**
`5512c073ae86c591760c2a52c72ccc2c27ed1e1ec3e5ac253d5496f2da465b04`

**SHA-256 of `foxsdr-0.99.96-linux-arm64.tar.gz`:**
`261cc95e9aaab0bc5aad2911f0655324cb1ea7fe11f2a0b8beea50cc9311f588`

## Features and fixes, 2026-10-27

**Listen to the tower, as a blueprint.** The Blueprints list on the patch page now offers a "Listen to the tower" entry for every airport you have looked up in the Airband section, and - when your receiver position is set - for the six airports nearest it. Picking one places the airport's tower frequencies (or, at a field with no tower, its common traffic frequency) as rows of Channel, AM demodulator with its squelch on, and a speaker that plays through your sound output, with a Spectrum on the radio. The radio is centred to hold as many of them as it can, with none sitting on its centre; any that do not fit in one radio's band are left out, and the note under the list says how many. Press START to listen. Each frequency has a speaker of its own, and a patch channel is not narrowed to an airband channel's width the way the Airband section's LISTEN narrows it.

**Make any patch part larger or smaller.** Every part on the patch canvas - the Map, the Spectrum, the radio, the Text out and the rest - could always be resized from its bottom-right corner, but that corner was three faint hairlines that were easy to miss. It is now a brass corner that lights up under the pointer, with the resize cursor, and the part's own controls no longer reach into it. Selecting a part also shows Smaller, Larger and Default size keys in its panel on the right.
