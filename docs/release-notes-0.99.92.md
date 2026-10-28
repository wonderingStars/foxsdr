# FoxSDR 0.99.92

**FoxSDR 0.99.92 fixes a start-up in which your saved radio was still held by another program - most often the copy of FoxSDR that had just closed to install an update - and FoxSDR quietly ran on its signal generator instead, so the GAIN dial read N/A and nothing said why. FoxSDR now tries the saved radio again by itself over the next half minute and opens it the moment it comes free; while no radio is open, the GAIN dial and its AUTO key say so and say why; and the log line records the reason.**

This is a repair only; nothing else changes. If you pressed "Install now and restart" on 0.99.90 or 0.99.91 and the new FoxSDR came up with the GAIN dial dead, that was this. The Microsoft Store copy stays on its current version until the next submission is certified.

---

**SHA-256 of `foxsdr-setup-0.99.92.exe`:**
`422081d68a464b65e6c0fb1c2b3a6e29147caf8c7177ea6f07b9095e4ea87831`

**SHA-256 of `FoxSDR-0.99.92-x86_64.AppImage`:**
`9310712302d5401eda2b569eb0106dc13992c63b05c47ebdc39b266ad7b7b846`

**SHA-256 of `foxsdr-0.99.92-linux-x64.tar.gz`:**
`c10267547934c0e2537b4972a5f7a6ec9a9ec0414183fc9fe03a8bb1ab3f20eb`

**SHA-256 of `FoxSDR-0.99.92-aarch64.AppImage`:**
`2dcfd1c339ec7cdb5361bbbee818ee81bd1442fa12c107b5aa87927f346ba601`

**SHA-256 of `foxsdr-0.99.92-linux-arm64.tar.gz`:**
`e20b874a32fd79d3dc23c561a144125ec8323cb2b2c4eacdfa3ab22f27c95843`

## Features and fixes, 2026-10-26

**A saved radio that is busy at start-up is tried again.** When FoxSDR starts it opens the radio you used last time. If that radio is held by another program at that moment, FoxSDR used to give up for the whole session: the receiver ran on the signal generator, which looks and sounds alive, the GAIN dial read N/A, and the only word of it was a red line inside the Source section. The commonest way to meet this is the update itself: "Install now and restart" closes the running FoxSDR and starts the new one, and the new one can start while the old one is still letting go of the dongle. A tester's report on 2026-10-26 ("the Gain button no longer works" on 0.99.91) was exactly this - his anonymous health counts show the radio was busy, not broken, and the session before had been streaming from it. FoxSDR now tries the saved radio again 2, 4, 8 and 16 seconds after the window appears, stops as soon as it opens or as soon as you choose another source, and gives up quietly after the fourth try if the radio is still held. A radio that is unplugged, unbound or refused by its driver is not retried; those reasons stand as before.

**The GAIN dial says why it is dead.** With no radio open, the top-bar GAIN dial and its AUTO key now carry the reason in their tooltips - "No radio is open: Blog V4 did not open: the radio is already in use by another program" - instead of describing a dial you cannot turn.

**The log says why a radio did not open.** The lines `the saved radio (...) did not reopen` and `... did not open` now carry the reason class in brackets - `busy`, `absent`, `bind`, `driver`, `timeout`, `rate`, `vendor` or `other` - the same anonymous word the health counts use. Never the driver's own message, which can quote the radio's serial number.
