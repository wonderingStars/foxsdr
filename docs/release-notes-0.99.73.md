**FoxSDR now keeps the radio and its sound responsive when many modules are fitted.** The DECODER OUTPUT and plugin picture windows open inside the main window, including on scaled displays where they could appear as a thin strip; RESET WINDOW SIZES places them there again. Each radio now remembers whether Auto gain was on. On Windows the source and DSP threads ask for Pro Audio priority, with an above-normal fallback. The sound buffer starts at 120 ms and, after three starved callbacks in a minute, raises its lead through 240, 480 and at most 960 ms; the audio matcher follows that lead. Automatic depth is remembered for the next launch. The Sinks control can instead hold a chosen depth. A raised lead can cause one longer silence when playback next has to prime; lowering the depth drops old queued sound so the shorter delay takes effect promptly, with a possible one-time jump. Fitted decoders now run while their window, map, output, patch or live browser session needs them, and become IDLE after 30 seconds without a use; opening one starts it again. START, a pressed preset, or **Keep running** pins a module on, while STOP leaves it off. Existing stopped modules stay stopped across an update. Background work such as coverage collection with the map closed now needs **Keep running**. The Fitted modules window distinguishes IDLE from a decoder that is failing to receive data.

Still not fixed, and known: the two previously reported fast-fail deaths remain unexplained; this release has not been tried on the slow computer that reported repeated audio starvation, so its effect there is unproven. Linux builds of the priority branch have not been run locally. The new words in thirty-three languages have not been reviewed by native speakers. A plugin picture from a regional catalogue is not shown. An MP3 patch speaker whose folder disappears says nothing, and a recording whose disk disappears may end as an empty file without an on-screen explanation.

---

**SHA-256 of `foxsdr-setup-0.99.73.exe`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.73-x86_64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.73-linux-x64.tar.gz`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.73-aarch64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.73-linux-arm64.tar.gz`:**
`TBD`
