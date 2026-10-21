# FoxSDR 0.99.84

**FoxSDR 0.99.84 makes one kind of freeze report finally say what it needs to say. A 0.99.81 field report caught the window frozen for half a minute inside a plugin reload - and could not say which of the reload's six waits was holding it: the reload's own timing line is only written when a reload ends, this one had not ended, and the stalled thread's stack walk died inside system modules under another program's API hook. A freeze report now carries a `reload-phase` line naming the step of the reload that was under way - check, decoders, patch, panels and map, unload, inventory, load or restart, and `none` outside a reload - published by the reload as it runs, read by the watchdog mid-stall, and uploaded only when it is one of those fixed words. Nothing about the reload itself changes: the next such report names the wait, which is the evidence the reload's redesign has been waiting for. And an SDRplay radio's stream-health line now counts the samples the SDRplay service itself lost, from the sample numbering the service puts on every block - the one loss FoxSDR could not see until now, and the one an RSPduo owner's 4 MS/s airband recording turned out to be full of.**

Still not fixed: the 0.99.81 plugins-reload freeze itself remains open - this release is the instrumentation that will diagnose its next occurrence, not the cure. The two 2026-10-05/06 fast-fail crash reports from 0.99.64/65 remain unexplained (no report of that shape has arrived from any newer build), and the one 0.99.75 AppImage start-up death can only be diagnosed if it recurs on 0.99.78 or later. The Microsoft Store copy stays on its current version until the next submission is certified.

---

**SHA-256 of `foxsdr-setup-0.99.84.exe`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.84-x86_64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.84-linux-x64.tar.gz`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.84-aarch64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.84-linux-arm64.tar.gz`:**
`TBD`

## Features and fixes, 2026-10-22

**Samples the SDRplay service lost are counted and named.** An RSPduo owner recorded the air band at 2 MS/s and at 4 MS/s: clean at 2, and at 4 about thirteen broadband clicks a second, with no clipping and no silence - discontinuities in the I/Q stream itself, not distortion. FoxSDR's SDRplay driver could count only the loss it caused: `overflows` in the once-a-minute stream-health line is blocks that did not fit FoxSDR's own ring, the host falling behind, and a stream the *service* had already lost samples from on the USB side looked healthy. The SDRplay API names every block's first sample, so the driver now keeps the number the next block must start at, counts a block that starts anywhere else as a gap the service made, and ends the SDRplay health line with `service gaps N (M samples)`; a minute holding one is written as a warning. The API's `reset` flag, a stream start and a rate change each make the next block a new baseline rather than a false gap. The check runs on the service's own thread as one atomic load and one compare-exchange, no lock and no log line, the rule every SDRplay callback has kept since 0.99.32. The existing fields and the meaning of `overflows` are unchanged, so older logs read the same. What the specification (3.15) says is only that `reset` marks a re-initialisation inside the API; that the numbering is contiguous and wraps at 2^32 is gr-sdrplay3's reading, shared by other independent consumers, and has not been checked against a real RSP on the build machine, which has none. Six tests drive the driver through the fake API (contiguous blocks, a jump, a wrap, a reset block, a restart and a rate change, the warning line). docs/HARDWARE.md and docs/DIAGNOSTICS.md describe the field; the recording's own analysis is the reason to look for it.

## Features and fixes, 2026-10-20

**A freeze inside a plugin reload names the reload's step.** Field report 7591C65EFB69DD94 (0.99.81, Windows 10) is the thirty-second plugins-reload report the diagnostics documentation has been waiting for since 0.99.63 - and it arbitrated nothing. The reload's per-step log line is only written when the reload ends; this reload had not ended, so the report's log stops at the line that opens a rescan, after three rescans the same session finished in under a second. The stalled thread's stack ended four system modules deep - a wait, under the C and C++ runtimes - with the unwind broken below that, and the report's own `foreign-modules` line names a third-party API hook of exactly the kind that breaks unwinding. So the report said `plugins-reload` and could not say which of the reload's waits - each with a different owner and a different fix - had held the window. The reload's stopwatch now *publishes* the name of its open step while it runs, the freeze watchdog writes it into the report as `reload-phase:` beside `frame-scope:`, and the uploader forwards it as one fixed word from a closed list of eight step names - never a plugin, a file, a path, a radio or a frequency (PRIVACY.md documents the field in the freeze table and the upload table, both held by the two-way inventory tests). A removal's unload, a stop and the exit path publish their steps too, without writing any reload log line. Nothing is bounded, moved off the GUI thread or excused by this change: the reload still blocks the window knowingly, and the design decision on restructuring it (docs/DIAGNOSTICS.md, "A plugin rescan runs on the GUI thread") still waits - now on a report that can actually name the phase.
