# FoxSDR 0.99.87

**FoxSDR 0.99.87 makes one kind of freeze reportable that never was. Two crash reports, from 0.99.78 and 0.99.84, came from windows that had stopped responding and were closed by their users from Windows' own "is not responding" dialog. FoxSDR had excused those freezes because it believed someone was reading a prompt or holding the window, so neither report said where the program had stopped. A freeze like that is now reported after about ten seconds with the stack of every thread, and the closing itself is named for what it is.**

This release does not fix the freeze those two users saw: the reports cannot say where it was, and that is what this release changes. The 0.99.81 plugins-reload freeze remains open with its instrumentation in place and no newer report yet. The RSPduo crackle at 4 MS/s (GitHub issue 6) is unchanged, with 0.99.86's measurement in place. The Microsoft Store copy stays on its current version until the next submission is certified.

---

**SHA-256 of `foxsdr-setup-0.99.87.exe`:**
`1ed49cb8cf82b0bf575bbe35df233a6ee1c209fb3c0be643d19053cced66eaeb`

**SHA-256 of `FoxSDR-0.99.87-x86_64.AppImage`:**
`0df067b4dca6c0c2c446d2f3cbbef6ba8f43fb138bbb084aeb030408c8ba44f8`

**SHA-256 of `foxsdr-0.99.87-linux-x64.tar.gz`:**
`1212c5876ede667627e20c946e3b57d89a9fa8f0167a0b951480bb209a2d7d69`

**SHA-256 of `FoxSDR-0.99.87-aarch64.AppImage`:**
`44a4d6a35117d748941855e4a142d8292220faba1be383e7d540d53bd75ac51b`

**SHA-256 of `foxsdr-0.99.87-linux-arm64.tar.gz`:**
`ee623d3575365578af1383e904166bc5e82da9f0477f8e13fbbae62c8b5dd865`

## Features and fixes, 2026-10-23

**A window Windows calls "Not Responding" is reported as a freeze, with its stacks.** FoxSDR watches its own window for freezes and, after five seconds without a drawn frame, writes a report with the stack of every thread. It deliberately does not do this while a person is holding the program up: reading a Windows security prompt, choosing a file, dragging the window or holding a menu open can stop the drawing for as long as the person likes. Those waits were excused without limit, and so a freeze that happened inside one of them was never reported. Two reports show exactly that: the window stopped, Windows greyed it as "Not Responding", and the user closed it. A person is always waited for in a loop that keeps answering Windows, and Windows only calls a window not responding when it has stopped answering for about five seconds. That verdict now ends the excuse: from it, the freeze is timed like any other and reported about five seconds later. A prompt or file dialog that is simply left open is still never reported.

**Closing a frozen window from Windows' dialog is named, and grouped as a freeze.** When the person presses "Close the program" on Windows' "is not responding" dialog, the program ends with exit code 0xCFFFFFFF (measured on Windows 11). FoxSDR's watcher process called that an unknown crash code. It now says "closed by Windows as not responding" and files the report as a freeze, grouped with freezes of the same part of the program rather than with crashes. When the freeze report was already written, as it now will be, the watcher adds nothing: one report per ending.
