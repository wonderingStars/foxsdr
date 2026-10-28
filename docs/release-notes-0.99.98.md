# FoxSDR 0.99.98

**FoxSDR 0.99.98 prints the fax chart inside the Radiofax recorder's window, at its true proportions, with the recorder's keys beside it - so a slant or left-edge correction is made and judged in one window.**

This is a feature release; nothing else changes. The Microsoft Store copy stays on its current version until the next submission is certified.

---

**SHA-256 of `foxsdr-setup-0.99.98.exe`:**
`aab883ecc1008a47079433677792ae30d665f7120f274b2fcb8f8172ea36216b`

**SHA-256 of `FoxSDR-0.99.98-x86_64.AppImage`:**
`d11b90ce385a3276c6735d306212b85e387b78c2591ad27f57cb2c1733fbc17b`

**SHA-256 of `foxsdr-0.99.98-linux-x64.tar.gz`:**
`3f5501908ea1a211c2162797eca82b376e25507f85a3bd08c94c2d9f2c805e56`

**SHA-256 of `FoxSDR-0.99.98-aarch64.AppImage`:**
`73aae8dc9ba96dbf68124b038a958542f465a39ddb0a5e4ec556ef5a8f0287bb`

**SHA-256 of `foxsdr-0.99.98-linux-arm64.tar.gz`:**
`f654164f0bec567fadde5f670634784dbb19cdd5f891024321ab66bd169f0aab`

## Features and fixes, 2026-10-28

**The chart is in the recorder, beside its keys.** Requested from the field (SWL F20806): the WEFAX plugin's slant and left-edge keys live in its Radiofax recorder window, and the picture they correct lived in the plugin's picture window, so every correction meant jumping between the two. Since 0.99.83 the recorder's paper strip carried the chart, but squashed to the strip's height, which is no help for judging a slant. A radiofax recorder whose plugin has a decoded picture now lays itself out in three parts: the machine's face, compacted across the top; the picture below it on the left, fitted to the space with its proportions kept, growing with the window; and the recorder's memory - the key rows (START NOW, STOP NOW, SLANT -10 ppm, SLANT +10 ppm, SET LEFT EDGE) and the event log - on the right, beside the picture. SET LEFT EDGE works on this picture too: press the key, then click the chart's margin in the recorder window, exactly as in the picture window. The plugin's picture window is unchanged and stays the full-size copy for reading and saving the chart; a recorder with no picture yet, or a fax plugin that offers none, is laid out exactly as before. No plugin change is needed: WEFAX 1.1.0 gains this as it stands.
