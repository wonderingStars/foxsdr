# FoxSDR 0.99.89

**FoxSDR 0.99.89 fixes three faults found in a tester's diagnostics bundle and in our own tests. An SDRplay radio whose service reported a device failure now starts again when you press START, instead of staying silent until it is picked again from the Source list. A start-up that hung for minutes is now reported to us even when Windows was shutting down when it ended. And FoxSDR no longer risks crashing as it closes, if it is closed within a second or so of being opened.**

These are repairs only; nothing on screen changes. The RSPduo crackle at 4 MS/s (GitHub issue 6) and the 0.99.81 plugins-reload freeze are unchanged. The Microsoft Store copy stays on its current version until the next submission is certified.

---

**SHA-256 of `foxsdr-setup-0.99.89.exe`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.89-x86_64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.89-linux-x64.tar.gz`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.89-aarch64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.89-linux-arm64.tar.gz`:**
`TBD`

## Features and fixes, 2026-10-24

**An SDRplay radio comes back after a device failure.** During a burst of strong signals the SDRplay service can report a "device failure" and still go on sending samples. FoxSDR stopped the receiver, as it should, but every START after that received nothing: the receiver showed FAIL again within a moment, and only picking the radio again in the Source list brought it back. A START now gives the radio a clean start, and it streams. A radio that has really gone - unplugged, or an RSPduo whose other half has closed - still asks to be picked again, and if the SDRplay service itself has stopped answering, FoxSDR still says to restart it. If the failure keeps coming back, reduce the gain or add attenuation: an ADC overload warning beside it is the sign.

**A start-up that hung is reported, even at shutdown.** If FoxSDR is still starting after more than 30 seconds - no window yet - something is wrong, and FoxSDR's crash reporting should tell us. When such a start-up was ended by Windows shutting down or logging off, the report was filed as "ended as the session closed" and kept on your computer, so a start-up that sat for two minutes without a window never reached us. It is now reported like any other start-up that never got to its window. A start-up a shutdown cut short in its first 30 seconds is still treated as the shutdown's, and kept on your computer.

**No crash when FoxSDR is closed straight after opening.** Since 0.99.88, RADIO SETUP checks the USB bus in the background at every start. If FoxSDR was closed while that check was still running - within about a second of opening it - the check could read a list that Windows had already freed as FoxSDR closed, and FoxSDR crashed on its way out. The lists the check reads now stay in place until the very end. We found this in our own tests; no report of it has come in from anyone's computer.
