# FoxSDR 0.99.86

**FoxSDR 0.99.86 adds one measurement for the RSPduo crackle at 4 MS/s (GitHub issue 6). The tester's 0.99.85 log showed the SDRplay service streaming cleanly through steady minutes - no service gaps, no overflows in the link between the service and FoxSDR, no starved audio - which left one place samples could still be lost without a word: inside FoxSDR, between the thread that reads the radio and the thread that processes what it read. That loss was always counted, but the count only reached crash reports. The log now says it once a minute, for any minute in which samples were lost, and the diagnostics bundle carries the session's total.**

This release does not fix the crackle. It makes the next report able to say whether the samples are lost before FoxSDR has them (the SDRplay `service gaps` line) or inside FoxSDR (the new `dsp:` line), or whether neither happens and the crackle comes from somewhere else. The 0.99.81 plugins-reload freeze remains open with its instrumentation in place and no newer report yet. The two 2026-10-05/06 fast-fail crash reports from 0.99.64/65 remain unexplained, and the one 0.99.75 AppImage start-up death can only be diagnosed if it recurs on 0.99.78 or later. The Microsoft Store copy stays on its current version until the next submission is certified.

---

**SHA-256 of `foxsdr-setup-0.99.86.exe`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.86-x86_64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.86-linux-x64.tar.gz`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.86-aarch64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.86-linux-arm64.tar.gz`:**
`TBD`

## Features and fixes, 2026-10-23

**Samples FoxSDR's own processing could not keep up with are said in the log, once a minute.** The radio's samples reach the processing thread through a fixed-size buffer, and a processing side that falls behind finds it full: what does not fit is dropped rather than making the radio wait, because a real-time device that is made to wait loses samples anyway, and less predictably. Every dropped sample was counted, but the count went only into the anonymous health figures of a crash report. Now a minute in which any were dropped ends with one line - `dsp: 412000 samples dropped between the source and the DSP in the last minute (0.17%)` - giving the count and its share of everything the radio delivered that minute; a loss too small for two decimals reads `under 0.01%`, never `0.00%`. A clean minute writes nothing, like the audio line beside it. The minute keeps its own clock rather than riding on the sound-output check, which does not run at all on a computer where no sound output has opened. The diagnostics bundle gains one line for the whole session, `dsp-dropped: 324608 of 2400000000 samples (0.01%)`: two counts and a share, nothing about the signal, the frequency or the radio (PRIVACY.md lists it). A new test stalls the processing thread on purpose, through a seam the pipeline already had for its own tests, and checks the line appears once with the pipeline's exact count and share, that a clean minute stays silent, and that the bundle carries the totals.
