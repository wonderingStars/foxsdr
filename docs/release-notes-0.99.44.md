**A user-adjustable interface size, for 4K screens.** Settings → Display now has an "Interface size" dropdown - Auto (the default, which follows what Windows reports for the monitor the window is currently on) or a fixed 100/125/150/175/200/250%, plus Ctrl+= / Ctrl+- / Ctrl+0 as shortcuts. It scales fonts and layout together, everywhere in the application including the patch page, and exists for people whose desktop reports the wrong number, or none, for how big FoxSDR's text and controls should be on a high-resolution display.

**SDRplay: the radio was quietly running at three times the rate you set - fixed by dropping the low-IF front end.** Two field reports from RSP2 owners showed the radio delivering 6 MS/s while FoxSDR had asked for 2 MS/s and built its whole processing chain for that rate: the SDRplay API specification says nothing about what its low-IF front end actually delivers, and every RSP available for testing here disagreed with the reference assumption FoxSDR's rate math was built on. FoxSDR now always asks for zero-IF, which leaves no room for that disagreement, at the cost of the low-IF mode's freedom from a centre spike (the API's own DC correction is on, and removes it). Separately, if the SDRplay session is lost mid-use, FoxSDR now stops trying to close every open SDRplay radio through the dead API service - each teardown used to make that call regardless - and stopping an SDRplay radio can no longer freeze the application for several seconds; it is now bounded to one second. None of this fixes the SDRplay API service's own drop in the first place, whose cause is still unknown, and there is no SDRplay hardware on the development desk, so all of the above is verified in the test suite rather than against a real radio.

**Audio no longer crackles or clips when you widen the channel filter.** Widening a demodulator's channel bandwidth could let occasional samples run past full scale, and the audio path clipped them outright, heard as crackle or distortion. A soft limiter now catches only the rare over-scale peak and brings it back smoothly; anything under about 97% of full scale is completely unaffected; bit-for-bit identical to before.

**A freeze fix for a busy or throttled graphics driver.** FoxSDR was calling into GLFW's mouse-passthrough setting on every single frame regardless of whether it had changed, which is exactly the kind of repeated per-frame system call that can stall under a busy or throttled graphics driver - this is what caused the Microsoft Store certification hang some of you may have seen reports about. The value is now cached and the underlying call is only made when it actually changes.

**Airspy: ADS-B now decodes even if you have raised the radio's decimation.** The ADS-B decoder refuses to run below 2 MS/s and asks its preset for 2.4 MS/s; if an Airspy R2 or Mini's decimation had been raised for something else, the radio could be stuck delivering nothing above roughly 1.25 MS/s (R2) or 750 kS/s (Mini) - below the decoder's own floor - because FoxSDR only ever looked for the nearest rate within whatever decimation was already set. It now looks at every native-rate-and-decimation combination the radio offers and picks the nearest one at or above what was asked, dropping the decimation automatically when that is what it takes to get there.

**Beta tester usage reporting, for testers who have been given a code.** If you have joined the tester list on foxsdr.com and pasted your tester code into Settings → Beta tester, FoxSDR now sends one report per session - application version, platform, session length, which named beta areas and which installed decoders you actually used (never a frequency, never decoded content, never your position) - so the areas testers said they would cover can be checked against what was actually exercised. This is completely separate from ordinary usage reporting and off for everyone else: with no tester code entered, nothing on that page is ever collected or sent. Remove the code in Settings → Beta tester to switch it off immediately - see PRIVACY.md for the exact field-by-field list of what is sent.

**The crash-report cleaner no longer mangles a plugin's own name.** The scrubber that removes anything that looks like a frequency from an uploaded log was turning a plugin name like "406 MHz Beacons" into gibberish, because "406 MHz" reads exactly like a tuned frequency. Plugin names that a report already lists by name are now left alone wherever they appear in it.

---

**SHA-256 of `foxsdr-setup-0.99.44.exe`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.44-x86_64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.44-linux-x64.tar.gz`:**
`TBD`
