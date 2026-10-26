# FoxSDR 0.99.95

**FoxSDR 0.99.95 makes the patch page's demodulator speak every mode on its list. A Demod part set to USB, LSB, CW or DSB now really demodulates that way - with the receiver's own sideband filter and its 700 Hz CW tone - so a blueprint for a single-sideband or CW plugin decodes in the patch, not only in the receiver.**

This is a fix to yesterday's blueprints; nothing else changes. The Microsoft Store copy stays on its current version until the next submission is certified.

---

**SHA-256 of `foxsdr-setup-0.99.95.exe`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.95-x86_64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.95-linux-x64.tar.gz`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.95-aarch64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.95-linux-arm64.tar.gz`:**
`TBD`

## Features and fixes, 2026-10-26

**USB, LSB, CW, DSB and RAW in the patch.** The Demod part has always offered the receiver's eight modes, but until now the patch played everything except the two FM modes as AM. For a single-sideband or CW signal, which has no carrier to follow, that was silence - so 0.99.94's blueprints for such plugins were placed correctly and then heard nothing. The patch now runs the receiver's own demodulator for these modes: the upper or lower sideband is selected with the other one cut by more than 60 dB, and a CW carrier on the channel's frequency plays as a 700 Hz tone. AM and FM sound exactly as they did.

The sideband width is the receiver's standard 3 kHz; a patch channel still has no bandwidth control of its own.
