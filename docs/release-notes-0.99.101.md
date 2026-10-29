# FoxSDR 0.99.101

**FoxSDR 0.99.101 ends the crackle an RSPduo tester heard at 4 MS/s: the buffer between the thread that reads the radio and the thread that processes the samples is now a quarter of a second deep instead of 65 ms, and the reading thread no longer pauses after a read that already waited, so a radio driver that hands its samples over in runs with gaps between them no longer overflows FoxSDR's own buffer.**

This is a receive-path fix with no change to how FoxSDR tunes, demodulates or draws; it also adds one line to the diagnostics bundle. The Microsoft Store copy stays on its current version until the next submission is certified.

---

**SHA-256 of `foxsdr-setup-0.99.101.exe`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.101-x86_64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.101-linux-x64.tar.gz`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.101-aarch64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.101-linux-arm64.tar.gz`:**
`TBD`

## Features and fixes, 2026-10-29

**The buffer between the radio and the processing is a quarter of a second deep.** GitHub issue 6: a tester's RSPduo at 4 MS/s crackled constantly while the SDRplay service streamed cleanly. His 0.99.90 report showed FoxSDR's own buffer dropping 0.66 to 0.88 % of every minute, and his health line showed why: the service hands its samples over in runs, 13 to 14 times a second, with 20 to 43 ms of silence between them, and FoxSDR's buffer held 65 ms at that rate. After a read that found nothing, the reading thread paused for what the code called 1 ms, which on a Windows machine that has not asked for a finer timer is 15.6 ms, and the processing thread sleeps the same when the buffer runs dry; 43 ms of silence plus those two pauses overran the buffer. The buffer is now at least a quarter of a second of the radio's rate (524288 samples at 2 MS/s, 1048576 at 3 and 4 MS/s, 8388608 from 20 MS/s up), and the reading thread no longer pauses after a read that already waited its own bound. A read that returns nothing without waiting is still paused, so a dead radio cannot spin a core. The depth is headroom only, so audio latency does not change. On the bench, a source behaving as that service did cost 0.794 % of the samples with the old buffer and nothing with the new one.

**The diagnostics say what a loss looked like.** A minute in which samples were dropped now writes a second line after its drop line: how many reads lost part of themselves, how full the buffer got, and the longest the processing thread spent on one block of samples. The bundle carries the session's figures as `dsp-ring: 1048576 samples (262 ms), high water 61%, longest DSP block 4.0 ms`. A long block is the processing side stalling; a short one beside drops is the radio's samples arriving late and all at once. Sizes and times only, nothing about the signal; PRIVACY.md lists the line.
