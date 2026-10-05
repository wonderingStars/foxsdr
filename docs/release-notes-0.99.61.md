**The Bandwidth control now narrows what you hear.** With a radio running at 2.4 or 2.048 million samples a second, every bandwidth setting was in fact the same filter, so an AM station 30 kHz away came through only 6 dB down and changing the setting made no audible difference. Each setting is now its own filter: at 10 kHz a station 30 kHz away is rejected completely, and at 3 kHz a signal 5 kHz away is too. In USB, LSB and CW the control now sets the width of the sideband you are listening to (it used to be fixed at 3 kHz whatever you chose). Two things follow that you may notice: the signal meter and the squelch now measure the narrower channel, so a squelch level you set before may need adjusting, and a very narrow AM setting uses a little more processor time. The outline drawn on the spectrum is still symmetric in the sideband modes; that is a display matter and is not changed here. Measured in FoxSDR's tests on generated signals, not on a radio.

**SDRplay radios: live controls now name the radio's tuner.** For every SDRplay model except the RSPduo, FoxSDR sent each frequency, antenna, gain and sample-rate change to the SDRplay service without saying which tuner it was for. SDRplay's own software never does that, and it is the most likely reason the first change after opening a radio was never answered and the service was then found stopped - reported on the RSPdx-R2, and matching earlier reports from RSP1A and RSP2 owners. FoxSDR now names the radio's one tuner, as SDRplay's software does. This has been checked against SDRplay's published interface and its own source, not on a radio: if your RSP still stops, please send the SDRplay diagnostic from the Diagnostics section.

**SoapySDR is no longer a second way into an SDRplay radio that FoxSDR has had to let go of.** After the SDRplay service had stopped answering, the radio list and the patch page could still open the same radio through SoapySDR's own SDRplay and Mirics modules, inside the same program, and SoapySDR's Mirics module could open a radio the SDRplay service was handling. Two crashes on Linux followed those two openings. Now, once the SDRplay connection has been lost, SoapySDR's SDRplay and Mirics entries are left out for the rest of the session; and SoapySDR's Mirics entry is left out for any radio the SDRplay service is handling. A saved setup that asks for one is refused with the reason. SoapySDR's SDRplay module is still offered while the SDRplay service is healthy, and a Mirics dongle on a computer with no SDRplay software is not affected. A SoapySDR driver that hands back more samples than it was asked for is now treated as a faulty radio and stopped, where before its count was trusted.

**Linux (Wayland): a crash when closing FoxSDR is fixed.** With some graphics drivers FoxSDR crashed on exit. The window library FoxSDR uses unloaded the graphics driver before it had finished closing its connection to the desktop; its authors have since corrected the order, and that correction is applied here. It could not be run on a Wayland desktop before this release.

**The window no longer freezes when the settings folder is slow to answer.** Once a second, in every session, FoxSDR looked in its settings folder for a link request from the beta-tester page, and it did so on the same thread that draws the window. On a computer where that folder was slow - a network or synchronised profile, or a scanner holding it - the window stopped for as long as the disk took. The look-up now happens in the background and nothing waits for it.

**A freeze can no longer go unreported, and a frozen session that was ended from the taskbar is counted as a crash.** Two of the reasons FoxSDR accepts for a pause in drawing had no time limit, so a real freeze that began during one of them - a plugin rescan, a display change, or a wait inside Windows' own window code - was never reported. They now last 30 seconds at most. The wait for a Windows permission prompt, which lasts as long as you take to read it, is still never reported. Separately, FoxSDR recorded a clean exit before it had finished shutting its plugins down, so a session that froze there and was ended from the taskbar looked like a normal close; the record is now made after the plugins have stopped.

**Crash reports from the radio scan name the driver that failed.** When a driver loaded part-way through a scan crashed - a sound card's ASIO driver is the usual one - the report carried only a bare address and was filed with every other unidentified fault. It now names the file. A saved patch that names a SoapySDR sound card is refused with a pointer to the Sound card source, since opening it ran that same driver inside FoxSDR itself.

**"No audio" reports can now answer themselves.** FoxSDR's log records when the sound output opens, or why it would not, and the diagnostics bundle now says whether the output is open, the volume, whether and by what the sound is muted, and where the squelch is set and whether it is open. None of these carries the sound card's name or the frequency you are tuned to. The bundle's crystal-correction line, which always read "off", is corrected. PRIVACY.md lists the new fields.

---

**SHA-256 of `foxsdr-setup-0.99.61.exe`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.61-x86_64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.61-linux-x64.tar.gz`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.61-aarch64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.61-linux-arm64.tar.gz`:**
`TBD`
