# FoxSDR 0.99.99

**FoxSDR 0.99.99 treats a monitor that goes away - to sleep, or unplugged - as one event rather than one every frame: the log no longer fills while it is gone, the freeze watchdog stays armed, and when the graphics driver refuses FoxSDR a second window the log now says why.**

This is a fix release; nothing else changes. The Microsoft Store copy stays on its current version until the next submission is certified.

---

**SHA-256 of `foxsdr-setup-0.99.99.exe`:**
`967cba22d2d0f84469edfbfa9e19d8ce6291fdcf3f29d3e90f1d4af29af00d9b`

**SHA-256 of `FoxSDR-0.99.99-x86_64.AppImage`:**
`273323c1a84fe490a7dde9b2b8f840a3eb31b6abbada9b47ff4190132de3d52c`

**SHA-256 of `foxsdr-0.99.99-linux-x64.tar.gz`:**
`f84c94c21c574932313ce74a78761a47931097e8b82613bea83a44eddaa87aed`

**SHA-256 of `FoxSDR-0.99.99-aarch64.AppImage`:**
`4e7efb63ad74a2eb52547f8fbf338d2ff60460e93717b8bfa3befeae2fc561f0`

**SHA-256 of `foxsdr-0.99.99-linux-arm64.tar.gz`:**
`e9dbb34aa3a62c39eb880e1841226194952da43c2692b1034cfa199ad5899d59`

## Features and fixes, 2026-10-28

**A monitor that goes away is one event, not fifteen a second.** From a tester's log (a desktop with Intel UHD 630 graphics whose monitor sleeps): while the monitor was away, Windows refused every question about its display settings, FoxSDR's interface library asked that question on every frame, and FoxSDR wrote two log lines for every refusal - over nine hundred a minute - until the start of the session had scrolled out of the log. Worse, each refusal was treated as a fresh display change, and the ten-second pause FoxSDR gives its freeze watchdog after a display change was restarted every frame: the watchdog was off for as long as the monitor was away, which is exactly when that machine's sessions went wrong without a report. Now a run of refusals is one episode: one line when it begins, one when the display answers again saying how long it lasted and how many questions failed, and one pause of the watchdog at its start. Those refusals also no longer use up the fifty graphics-library messages a session may log, so a later, different message - the one that matters - is still written.

**When the driver refuses a second window, the log says why.** Two machines with Intel UHD graphics have had the driver refuse FoxSDR a second OpenGL window part-way through a session (the "map vanished" reports). Since 0.99.97 FoxSDR brings every page back inside the main window when that happens, but the message carried no reason. It now carries the Windows error code, so the next report can say whether the display was lost, resources ran out, or something else refused it.
