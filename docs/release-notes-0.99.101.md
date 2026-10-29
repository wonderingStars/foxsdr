# FoxSDR 0.99.101

**FoxSDR 0.99.101 ends the crackle an RSPduo tester heard at 4 MS/s: the buffer between the thread that reads the radio and the thread that processes the samples is now a quarter of a second deep instead of 65 ms, and the reading thread no longer pauses after a read that already waited, so a radio driver that hands its samples over in runs with gaps between them no longer overflows FoxSDR's own buffer - and it sends an issue report in the session that met the fault, not at the next start.**

This is a receive-path fix with no change to how FoxSDR tunes, demodulates or draws; it also adds one line to the diagnostics bundle and sends issue reports straight away. The Microsoft Store copy stays on its current version until the next submission is certified.

---

**SHA-256 of `foxsdr-setup-0.99.101.exe`:**
`2dbe5fe11ba6af8989d4f439dff9b06c3a351ab725e4f59ed1c057b2afb7fb25`

**SHA-256 of `FoxSDR-0.99.101-x86_64.AppImage`:**
`b7c69e3ae67802c4df35ad3eef0c35e6d0538654b37fad2cd82749b67560ad35`

**SHA-256 of `foxsdr-0.99.101-linux-x64.tar.gz`:**
`20c223c680d79391f93b0e8c037fbe06acd60158cf34d352525c59ef7be5c45a`

**SHA-256 of `FoxSDR-0.99.101-aarch64.AppImage`:**
`1c529e21fc054317556c3eda02bfabe01c1727786febbb5ef2f6c7fdc902c5ed`

**SHA-256 of `foxsdr-0.99.101-linux-arm64.tar.gz`:**
`7c1bf8b19e8ef6b24a9233590ea13e7ac897ea43b5227f44f2f3895adf37f0c7`

## Features and fixes, 2026-10-29

**The buffer between the radio and the processing is a quarter of a second deep.** GitHub issue 6: a tester's RSPduo at 4 MS/s crackled constantly while the SDRplay service streamed cleanly. His 0.99.90 report showed FoxSDR's own buffer dropping 0.66 to 0.88 % of every minute, and his health line showed why: the service hands its samples over in runs, 13 to 14 times a second, with 20 to 43 ms of silence between them, and FoxSDR's buffer held 65 ms at that rate. After a read that found nothing, the reading thread paused for what the code called 1 ms, which on a Windows machine that has not asked for a finer timer is 15.6 ms, and the processing thread sleeps the same when the buffer runs dry; 43 ms of silence plus those two pauses overran the buffer. The buffer is now at least a quarter of a second of the radio's rate (524288 samples at 2 MS/s, 1048576 at 3 and 4 MS/s, 8388608 from 20 MS/s up), and the reading thread no longer pauses after a read that already waited its own bound. A read that returns nothing without waiting is still paused, so a dead radio cannot spin a core. The depth is headroom only, so audio latency does not change. On the bench, a source behaving as that service did cost 0.794 % of the samples with the old buffer and nothing with the new one.

**The diagnostics say what a loss looked like.** A minute in which samples were dropped now writes a second line after its drop line: how many reads lost part of themselves, how full the buffer got, and the longest the processing thread spent on one block of samples. The bundle carries the session's figures as `dsp-ring: 1048576 samples (262 ms), high water 61%, longest DSP block 4.0 ms`. A long block is the processing side stalling; a short one beside drops is the radio's samples arriving late and all at once. Sizes and times only, nothing about the signal; PRIVACY.md lists the line.

**Issue reports are sent straight away.** 0.99.100 began writing an issue report for each of thirty-three handled faults - a radio driver's thread let go, a sound device that would not close, the graphics driver refusing a second window - but, like a crash report, it waited to be sent until the next time FoxSDR started. A crash report has to wait: the program that wrote it was dying. An issue report is written by a healthy program that carries on, and the person who met the fault may never start FoxSDR again. From this release, with **Diagnostics** on, the report is sent within seconds of being written, in the same session, under the same switch, the same once-a-day-per-fault rule and the same five-a-day limit. Nothing new is sent and no field changes; with Diagnostics off nothing is sent at all. PRIVACY.md, *An issue report*, says so.

**Every issue report is kept.** On foxsdr.com a crash keeps three reports in full and counts the rest, which is right for a crash (the same stack again) and wrong for an issue (each report is a different machine's log of why). From site 2.106.0 every report of an issue FoxSDR names is kept in full - the first for good, then the newest hundred - so each one can be read and acted on.
