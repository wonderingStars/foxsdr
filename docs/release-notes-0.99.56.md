**PPM frequency correction.** Every radio's crystal runs a few parts per million away from what is printed on it, and the error grows with frequency - 5 ppm is 500 Hz at 100 MHz and 8.6 kHz at 1.7 GHz, enough to put a narrow signal outside its filter. **Settings > Frequency correction** (also in the Source section, under the converter) now has a **PPM frequency correction** switch, off by default, and a value from -200 to +200 ppm in 0.1 steps - positive when stations show up below their real frequency. The value is remembered for each radio separately, by its serial number, and is put back every time that radio opens; changing it takes effect at once. A radio that can correct its own crystal is sent the value - the RTL-SDR (in whole ppm, and its sample rate is corrected too) and SoapySDR radios whose driver offers it. Every other radio is corrected by retuning it so it lands on the frequency you chose; the centre frequency is then right, but the sample rate keeps its small error. The Source section says which of the two your radio uses. The counter, the spectrum, bookmarks, decoders and the patch page all keep showing the true frequency, and the patch page's own radios use the same value. While it is on, the RECEIVER card in the status column shows the correction, for example **PPM +1.5**. With the switch off nothing changes from before. Requested by a user in Taiwan.

**RTL-SDR dongles are steadier when the computer is busy.** With every CPU core busy, some RTL-SDR dongles briefly stopped accepting tuner commands: the radio could fail to open with "the tuner stopped answering on the I2C bus", or a retune could be refused and stay on the old frequency. FoxSDR now retries those commands. Retunes and gain changes are no longer refused as "the radio is busy" while samples stream, and after a sample-rate change the display and audio no longer get a moment of samples from the old rate.

**Audio no longer turns choppy after a while.** The sound card and the radio each keep their own time, and FoxSDR only built up a 120 ms lead of audio when playback started. Every hiccup that lost a little audio, such as a busy moment on the computer or a USB stall, took its length out of that lead, and nothing ever put it back. After a few hiccups the next one emptied it: the audio broke up, **Audio Underruns** went up, and only a restart put the lead back. FoxSDR now keeps the lead at about 160 ms by playing the audio very slightly faster or slower, never more than 0.5 %, until the lead is back where it belongs. It also absorbs a radio whose crystal runs fast or slow against the sound card's. On a test RTL-SDR on FM broadcast with every CPU core busy, underruns went from 10 in five minutes to none. The patch page's speaker output gets the same fix. Reported by a beta tester.

---

**SHA-256 of `foxsdr-setup-0.99.56.exe`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.56-x86_64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.56-linux-x64.tar.gz`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.56-aarch64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.56-linux-arm64.tar.gz`:**
`TBD`
