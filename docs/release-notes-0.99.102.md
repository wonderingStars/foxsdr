# FoxSDR 0.99.102

**FoxSDR 0.99.102 answers the first day of issue reports: a torn-off page that the graphics driver refuses comes back after a short rest instead of being gone for the session, the audio buffer is no longer deepened for a plugin install or for pressing Stop, a report about dropped samples needs a loss you could hear, and a sound device that would not stop is left alone at exit rather than closed under its driver.**

Every change here came from a report the application sent about itself on 2026-10-29 (the issue reports that 0.99.100 began writing and 0.99.101 began sending at once). None of them changes how FoxSDR tunes, demodulates or draws. The Microsoft Store copy stays on its current version until the next submission is certified.

---

**SHA-256 of `foxsdr-setup-0.99.102.exe`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.102-x86_64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.102-linux-x64.tar.gz`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.102-aarch64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.102-linux-arm64.tar.gz`:**
`TBD`

## Features and fixes, 2026-10-29

**A refused torn-off page comes back.** Two machines with Intel UHD graphics reported the same thing within two hours: the driver refused FoxSDR a second OpenGL window for a page torn off the main one, and since 0.99.97 FoxSDR answered by keeping every page inside the main window for the rest of the session. The refusals carried Win32 error 0xc00705aa - "insufficient system resources", a passing shortage, in one case two seconds after the display came back from thirteen minutes off - not a driver that cannot share a context at all. So a refusal now rests the feature rather than ending it: the pages come inside as before, and a minute later torn-off windows are offered again (ten minutes after a second refusal). A third refusal in one session is taken as that machine's answer and keeps the old behaviour, with the log line that says so. A machine whose driver refuses the probe at start-up is unchanged: it is never offered the feature.

**The audio buffer is not deepened for your own actions.** FoxSDR deepens its audio buffer when a minute closes with three or more starved sound-card callbacks, on the reading that the computer fell behind - and two reports showed it counting things that were not that. One person installing plugins from the store: nine reloads in fifty-five seconds, each a deliberate pause of the window, twenty-seven starved callbacks, and the buffer raised to 960 ms - a second of added latency that a quiet quarter of an hour then had to earn back. Another pressed Stop three times in a minute: each stop of a running receiver starves the sound card's last callback once, and three of them met the threshold on their own. A minute in which plugins were reloaded is no longer judged at all, and one starved callback per Stop is set against the count before the buffer is judged. Starvation beyond that still counts in full, and the log says when a raise was withheld and why.

**A report about dropped samples needs a loss you could hear.** The `ringdrop` issue report was written on the first dropped sample of a session, and its first day was six reports from five machines, every one a few milliseconds at the start of a stream - 3.5 ms, under 0.01 % of the minute - that nobody could hear and no decoder noticed, while the fault of GitHub issue 6 lost 0.69 % of every minute. The report is now written for a minute that lost at least 0.05 % of what it read. The log line on the machine and the diagnostics bundle still record every drop; only the report has the floor.

**A sound device that would not stop is left alone at exit.** One 0.99.81 report was an access violation at the very end of a session, in the Windows audio driver with none of FoxSDR's own code on the faulting thread. The one place FoxSDR could have caused it: when the device refused to abort its stream, FoxSDR went on to close it, and closing frees the buffers the driver may still have been writing. A stream that will not abort is now left open on purpose - the operating system reclaims it a moment later at exit - and the log says so.

## Diagnostics

The log gains three lines, each only when it applies: `viewports: torn-off pages are offered again after the refusal (1 of 3 this session)`, `audio: buffer kept at 120 ms - the starved callbacks came in a minute that reloaded plugins` (or `... - 3 of the 3 starved callbacks came with receiver stops`), and `audio: the output stream would not abort (...); it is left open rather than closed under a driver that may still be writing`. The issue report's sentence for `viewport` no longer promises "for the rest of the session"; PRIVACY.md's table says the same. Nothing new is sent, and nothing about the signal.
