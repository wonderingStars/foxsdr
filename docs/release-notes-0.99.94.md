# FoxSDR 0.99.94

**FoxSDR 0.99.94 adds blueprints to the patch page. Pick a plugin from the new Blueprints list - ADS-B, say - and its whole workflow lands on the canvas, wired and tuned: the radio on the right frequency and sample rate, the channel and demodulator it needs, the decoder, and a Text out, a Map and a Spectrum to show what it finds. And a plugin no longer has to be switched on to be used in a patch.**

This is a feature release; nothing else changes. The Microsoft Store copy stays on its current version until the next submission is certified.

---

**SHA-256 of `foxsdr-setup-0.99.94.exe`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.94-x86_64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.94-linux-x64.tar.gz`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.94-aarch64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.94-linux-arm64.tar.gz`:**
`TBD`

## Features and fixes, 2026-10-26

**Blueprints on the patch page.** A new Blueprints list sits under the Decoders row, with one entry for every place each installed plugin listens - its own presets, so a plugin with three presets offers three. Picking one builds the whole chain in one go. A decoder that reads the raw band (ADS-B) gets the radio centred on its frequency at the rate it needs, wired straight in; one that reads sound gets a channel on its frequency, the demodulator its preset asks for, and the radio a quarter of its sample rate away so the signal is clear of the radio's centre spike. Every workflow has a Spectrum on the radio, a Text out for a text decoder, and a Map for one that reports positions. The view moves to show the whole new chain; press START to run it.

**The radio you already have is used first.** The patch page opens with one radio on your receiver's own device. The first blueprint takes that radio over rather than adding a second one - on a machine with a single dongle, a second radio would only have found the signal generator. A later blueprint gets a radio of its own on a free device, up to the patch's five; with all five in use the list says so and places nothing.

**A plugin does not have to be on to be part of a patch.** Until now, stopping a plugin in DECODE also took it out of the patch page's parts. A patch runs its decoders itself, so a stopped plugin is now still offered as a part and as a blueprint, and runs in the patch when the patch runs. Stopping a plugin still stops the receiver's own copy of it.

A plugin with no preset is still offered: its workflow is placed and wired with no frequency, and the channel (or the radio, for a raw-band decoder) says so until you type one. A plugin whose preset asks for single-sideband or CW audio is placed with that mode set, but the patch's demodulator is FM or AM only, so for now such decoders run best from the receiver rather than the patch.
