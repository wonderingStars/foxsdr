# FoxSDR 0.99.83

**FoxSDR 0.99.83 answers three field reports. The audio buffer that deepens itself when a computer falls behind now steps back down after fifteen quiet minutes, and a launch that restores a deepened buffer now says so - the fix for "the audio is a second behind the spectrum since yesterday": one bad stretch (a Windows update, a scan) could ratchet the buffer to 960 ms, and the depth was kept silently forever, surviving restarts with nothing on screen to connect the delay to the setting. Typing a frequency that lands in a different band now adopts that band's conventional mode - type 3.620 and the receiver is in LSB, type 118.1 and it is in AM - while typing within the band you are already in never touches the mode, so a digital setup such as FT8 on 7.074 USB survives nearby retypes. And the Radiofax recorder now prints the chart on the paper coming out of its slot as it is received, so the machine and its picture are one window; the plugin's picture window stays the full-size reading copy.**

Still not fixed: two 2026-10-05/06 fast-fail crash reports from 0.99.64/65 remain unexplained (no report of that shape has arrived from any newer build, which carry the diagnostics that would name the faulting module), and the one 0.99.75 AppImage start-up death can only be diagnosed if it recurs on 0.99.78 or later. The chart printed on the Radiofax recorder's paper has not yet been confirmed against an off-air HF fax transmission - the decoder and the picture window are unchanged, only the recorder's own drawing is new. The Microsoft Store copy stays on its current version until the next submission is certified.

---

**SHA-256 of `foxsdr-setup-0.99.83.exe`:**
`da8a560d2c2f6803df4b922c66047a07133e3fce14cfcf7e75ce48882308340f`

**SHA-256 of `FoxSDR-0.99.83-x86_64.AppImage`:**
`e858bda18545a36e9341f84d0c3f2a8d5b9c58a034d124846a5dc3d7e59082f9`

**SHA-256 of `foxsdr-0.99.83-linux-x64.tar.gz`:**
`748ded1142b202c2b453064ca2b9db9dc5bd9310d52a36203c7fca0431725683`

**SHA-256 of `FoxSDR-0.99.83-aarch64.AppImage`:**
`ef3cd849b3e0cea16cfc911cb831e7f5fe29760c064cadf170ab077503ee936b`

**SHA-256 of `foxsdr-0.99.83-linux-arm64.tar.gz`:**
`608b514df28f7976bc11ed53c528a4ccb0072a55a4875fd18eaa5e3c1a0b16a4`

## Features and fixes, 2026-10-19

**The audio buffer comes back down by itself, and says where it stands.** Since 0.99.73 the audio buffer in AUTOMATIC deepens itself - 120, 240, 480, 960 ms - when a minute closes with three or more starved audio callbacks, and the depth it reached was remembered and restored at every launch, deliberately without a notice. A field report showed the cost of "never down": a 1-second delay between the voice and the peak on the spectrum that "appeared yesterday" on an unchanged 0.99.81 and survived restarts - one bad stretch had ratcheted the buffer up, and nothing on screen said so. AUTOMATIC now steps one rung back down after fifteen consecutive minutes with no starved callback at all, never below the 120 ms default, and lowers the remembered depth with it; a quiet minute alone still changes nothing, so a genuinely slow computer keeps the buffer it needs. The log now carries "audio: buffer lowered to N ms after 15 quiet minutes" and "audio: buffer restored to N ms (the depth automatic reached when it last ran)", and the Sinks rail shows the restore sentence, so a deep buffer is always attributable. A fixed buffer setting still never moves in either direction, and choosing AUTOMATIC by hand still starts fresh. On 0.99.81 and 0.99.82 the delay can be cleared by hand: in the Sinks rail pick any fixed buffer value, then AUTOMATIC again.

**The mode follows a typed band change.** Requested from the field: "if I type 3.620 MHz, put me in LSB". A frequency typed into the counter that lands in a different band of the band plan now adopts that band's conventional mode through the same path as a mode key, default bandwidth included - LSB below 10 MHz in the amateur bands (with the 60 m channels USB, as their licences say), USB from 10 to 30 MHz and NFM above; AM for LW/MW/SW broadcast and WFM for the FM bands; USB for HF aviation and marine, AM for VHF/UHF airband, NFM for the VHF marine channels and the ISS. Two deliberate limits keep it from fighting you: the mode moves only when the typed frequency crosses into a different band - typing within the band you are in never touches it, so FT8 on 7.074 USB survives nearby retypes - and a band without a confident convention (satellite, PMR), or a receiver parked in RAW, moves nothing. The wheel, the toggle switches, dragging on the spectrum and the scanner never change the mode at all, and the Radio section's mode keys always win afterwards. The log says "mode follows the band plan: <band>" whenever it acts, so an unexpected mode is attributable too.

**The Radiofax recorder prints the chart on its paper.** Requested from the field: the fax picture and the Radiofax recorder in one window. The paper creeping out of the recorder's slot now carries the chart itself as it is received - first lines at the lip, the sheet lengthening as lines land, printed in the paper's own tone - instead of blank ruled paper, so the phase lamps, the tuning meter, the line counter and the picture they describe are finally one piece of equipment. The plugin's picture window is unchanged and stays the full-size copy for reading and saving the chart. A recorder with no picture yet - or a fax plugin that offers no picture at all - draws the empty slot and blank sheet exactly as before.

**For the test suite** (not user-visible): a timing race in the pipeline's own test, introduced when 0.99.82 moved the GUI setters off the signal path's lock, is fixed - the test now waits for the posted decoder reset instead of racing it.
