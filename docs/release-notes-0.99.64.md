**FoxSDR now notices when it ends without having been able to say why.** Some endings nothing inside a program can report: a failure so abrupt that it skips every handler, a window frozen so hard that the part that watches for freezes is stuck too, a failure before the window opens, a frozen window that is then ended from the taskbar. In one week the installs counted at least 85 sessions that ended that way and about 12 reports arrived. With Diagnostics on, FoxSDR now starts a second, windowless copy of itself - the *sentinel* - that does nothing but wait for the first to end. It reads no memory and takes no dump: it is told how FoxSDR ended, and reads a small page of numbers FoxSDR keeps up to date (which stage it is in, how many frames it has drawn and when it last drew one, which part of drawing the window is under way). When FoxSDR ends abnormally and wrote no report of its own, the sentinel writes one, in the same folder and format as the others, and it is sent at the next start under the same switch and limits. An ending from outside while the window was drawing or being held (you ended FoxSDR from Task Manager, or were dragging the window), and an ending as Windows closed your session, are kept on your computer and never sent. With Diagnostics off the sentinel is not started. You will see a second `cascade.exe` of about 9 MB in Task Manager; it uses no processor time while it waits and is gone within a tenth of a second of FoxSDR closing. On Linux it can tell only that FoxSDR has gone, not how.

**A slow frame is now measured and named, and a freeze report says which part of the window was being drawn.** Every freeze reported in recent weeks was something slow on the thread that draws the window, and the only detector fired at five seconds. FoxSDR now times each frame and nineteen named parts of it (the rail, the spectrum, the patch page, a plugin's panels, a plugin reload, starting or stopping a recording, and so on), and records any frame of a quarter of a second or more against the part that took the time - in the log, and in one line of the diagnostics bundle a bug report attaches. A freeze report carries the same word (`frame-scope: rail`), so it names the part of the program that held the window before anyone has read a stack. The words come from a fixed list; none is a plugin's name, a file, a radio or a frequency. A hidden or minimised window, a display change, a window being dragged and a computer going to sleep are not counted.

**The anonymous usage record now counts things that quietly did not work.** Five releases once shipped in which no radio of any kind could be detected, and nothing said so, because the program did not crash - it simply did not work. With Usage reporting and Diagnostics both on, FoxSDR now counts, from a fixed list of words: a scan that found no radio at all; a radio that opened, delivered samples, or would not open (by driver kind and one of eight reasons such as `busy` or `absent`); whether sound came out; a failed update check, download or install; a plugin catalogue or plugin install that failed, or a plugin refused at load; and a recording that could not start. Counts only - never a device name, a serial number, a path, a frequency or the text of an error message. Empty for most people. PRIVACY.md lists every word.

**Eight more file operations no longer run on the thread that draws the window.** Four freezes reported between 0.99.58 and 0.99.63 were the same defect in different places: a file being opened or written while the window waited, on a disk that was slow to answer (a synchronised or network folder, a drive that had spun down). The rest have been found and moved: opening a patch speaker's WAV or MP3 file (sound offered while a slow disk opens the file is kept and written first, up to 20 seconds of it), saving bookmarks and markers, opening an I/Q file from the Source list, listing the patch page's recordings, the F12 screenshot, Save as BMP and Export for SDR#. A check in the build now fails if a new file call is added to the window's code outside a short list of named exceptions. Still on that thread, and still reported if they stall: stopping a recording, importing a bookmark file, and reopening a saved I/Q file at start-up.

**Five faults found by testing with a memory checker and with deliberately malformed input.** A malformed answer to the update check, or to a feature request, could end the program. A CAT command consisting only of a vertical tab or a form feed crashed the CAT connection, so anything able to reach that network port could do it. A marker file holding a very large number made the next marker reuse a number already taken. A radio driver that returned more samples than it was asked for made FoxSDR write past the end of a buffer. All five are fixed, and the inputs that found them are now permanent tests.

---

**SHA-256 of `foxsdr-setup-0.99.64.exe`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.64-x86_64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.64-linux-x64.tar.gz`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.64-aarch64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.64-linux-arm64.tar.gz`:**
`TBD`
