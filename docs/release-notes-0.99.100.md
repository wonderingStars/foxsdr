# FoxSDR 0.99.100

**FoxSDR 0.99.100 reports the problems it used to carry on from in silence: when a radio driver's thread has to be let go, a sound device will not close, the graphics driver refuses a second window, a saved recording will not reopen, or any of thirty other handled faults happens, FoxSDR now writes an issue report - with the log that says why - and, with Diagnostics on, sends it to us the way a crash report is sent.**

This is a diagnostics release; nothing changes in how FoxSDR receives, decodes or draws. The Microsoft Store copy stays on its current version until the next submission is certified.

---

**SHA-256 of `foxsdr-setup-0.99.100.exe`:**
`26b5c78436be45a20a64ff4008080dad2fcf0809d9271bd2495ba57306808254`

**SHA-256 of `FoxSDR-0.99.100-x86_64.AppImage`:**
`6ab641933b7691b463bf2979f095a6ace0deace7b5c36dbcfa3aa63b2c22e104`

**SHA-256 of `foxsdr-0.99.100-linux-x64.tar.gz`:**
`c13b11148a077eb83e59b649836fd84b1f877dd2ed93b6768e458ae58a021c1f`

**SHA-256 of `FoxSDR-0.99.100-aarch64.AppImage`:**
`296a92c6f7793cc8da6f4fc8c8087ed688bb53e8ad099f9ff7b3da1d05603067`

**SHA-256 of `foxsdr-0.99.100-linux-arm64.tar.gz`:**
`b0b20a0adc6d0c33cd2d64b648739a0ae7a0b46fa7a5183283241604a807e78c`

## Features and fixes, 2026-10-28

**Issue reports.** Until now FoxSDR told us about the ways it stops - a crash, a freeze, an ending the watcher saw - and nothing about the faults it handles and carries on from. Two testers' machines had the graphics driver refuse FoxSDR a second window for weeks; every page was folded back inside the main window exactly as designed, the map "vanished" session after session, and no report was ever written because nothing had crashed. A survey found twenty-five more places like it. From this release each such place is a word in a fixed list of thirty-three, and the first time one happens in a session FoxSDR writes an issue report beside its crash reports: the word, one fixed sentence saying what happened, the same application context a crash report carries, how long the session had run, and the last lines of the log. No call stack and no module list, because nothing faulted. With **Diagnostics** on it is sent on the next start, through the same channel and under the same rules as a crash report, and it is listed on our side as a fault FoxSDR survived - never counted as a crash. The full list of words and what each means is in PRIVACY.md under *Recovered from* and *An issue report*.

**The same thirty-three words are counted.** Each is also a health event in the usage report (the *Recovered from* table), so we can see how many installations meet a given fault, not only read the reports of those who have Diagnostics on. Eighteen of the words are new: `readerleak`, `closeleak`, `linkstranded`, `streamrestart`, `tunerretry`, `latecall`, `scanlimit`, `usbstuck`, `ratesdefault`, `gainrefused`, `ppmrefused`, `iqfilegone`, `cataloguebad`, `catstop`, `filekept`, `takeheader`, `sentinelgone` and `displaywatch`, beside `viewport` for the refused second window, which is now counted at start-up as well as mid-session.
