# FoxSDR 0.99.98

**FoxSDR 0.99.98 prints the fax chart inside the Radiofax recorder's window, at its true proportions, with the recorder's keys beside it - so a slant or left-edge correction is made and judged in one window.**

This is a feature release; nothing else changes. The Microsoft Store copy stays on its current version until the next submission is certified.

---

**SHA-256 of `foxsdr-setup-0.99.98.exe`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.98-x86_64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.98-linux-x64.tar.gz`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.98-aarch64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.98-linux-arm64.tar.gz`:**
`TBD`

## Features and fixes, 2026-10-28

**The chart is in the recorder, beside its keys.** Requested from the field (SWL F20806): the WEFAX plugin's slant and left-edge keys live in its Radiofax recorder window, and the picture they correct lived in the plugin's picture window, so every correction meant jumping between the two. Since 0.99.83 the recorder's paper strip carried the chart, but squashed to the strip's height, which is no help for judging a slant. A radiofax recorder whose plugin has a decoded picture now lays itself out in three parts: the machine's face, compacted across the top; the picture below it on the left, fitted to the space with its proportions kept, growing with the window; and the recorder's memory - the key rows (START NOW, STOP NOW, SLANT -10 ppm, SLANT +10 ppm, SET LEFT EDGE) and the event log - on the right, beside the picture. SET LEFT EDGE works on this picture too: press the key, then click the chart's margin in the recorder window, exactly as in the picture window. The plugin's picture window is unchanged and stays the full-size copy for reading and saving the chart; a recorder with no picture yet, or a fax plugin that offers none, is laid out exactly as before. No plugin change is needed: WEFAX 1.1.0 gains this as it stands.
