# FoxSDR 0.99.95

**FoxSDR 0.99.95 makes the patch page's demodulator speak every mode on its list. A Demod part set to USB, LSB, CW or DSB now really demodulates that way - with the receiver's own sideband filter and its 700 Hz CW tone - so a blueprint for a single-sideband or CW plugin decodes in the patch, not only in the receiver.**

This is a fix to 0.99.94's blueprints; nothing else changes. The Microsoft Store copy stays on its current version until the next submission is certified.

---

**SHA-256 of `foxsdr-setup-0.99.95.exe`:**
`5162d47427efaeeb9343081707166dc2bd617cdb0d5ad8fe361ceb0718254034`

**SHA-256 of `FoxSDR-0.99.95-x86_64.AppImage`:**
`5519548a260def5c08536e78bf3b760bf490075438e451d38f349a53123e0c0d`

**SHA-256 of `foxsdr-0.99.95-linux-x64.tar.gz`:**
`c9cd179944f2e10d9ad943861573957bd715e89d543f4d26d254a4ee6a32285c`

**SHA-256 of `FoxSDR-0.99.95-aarch64.AppImage`:**
`a75da12dbcbcc231f0b4c755f5e828d963b7d1812a44d753348b1afe4fc76ecd`

**SHA-256 of `foxsdr-0.99.95-linux-arm64.tar.gz`:**
`fbe29818626a863e6e5478bf919a62b9950a948f90e90e747d0c9bd001fd60ce`

## Features and fixes, 2026-10-26

**USB, LSB, CW, DSB and RAW in the patch.** The Demod part has always offered the receiver's eight modes, but until now the patch played everything except the two FM modes as AM. For a single-sideband or CW signal, which has no carrier to follow, that was silence - so 0.99.94's blueprints for such plugins were placed correctly and then heard nothing. The patch now runs the receiver's own demodulator for these modes: the upper or lower sideband is selected with the other one cut by more than 60 dB, and a CW carrier on the channel's frequency plays as a 700 Hz tone. AM and FM sound exactly as they did.

The sideband width is the receiver's standard 3 kHz; a patch channel still has no bandwidth control of its own.
