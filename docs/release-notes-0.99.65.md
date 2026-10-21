**Stop, then Record again straight away, no longer overwrites the recording you just made.** A recording is named for the second it starts in. Pressing Stop and then Record inside the same second gave the new recording the same name as the one just finished, and it was opened over it: one file was left, holding only the second take. A recording now never opens over a file that exists - the second take is named with `-2` (then `-3`, and so on) - and the same holds for two recordings started in the same second by the main recorder and a patch speaker.

**A damaged settings, bookmark or marker file is kept, not overwritten.** If `config.json`, the bookmark list or the marker list could not be read at start-up - cut short by a crash or a full disk, for instance - FoxSDR started on defaults or an empty list and the next save replaced the damaged file, so whatever could have been recovered from it was gone. The damaged file is now renamed aside (`.bad-` and the date and time) before anything is saved, the three newest are kept, and if it cannot be moved FoxSDR does not save over it in that session.

**On Linux, a folder where the bookmark list, the marker list or a band plan should be no longer ends the program.** Reading one of those files when a folder sat in its place - or when the disk returned a read error part way - raised an error nothing caught, and FoxSDR closed at start-up. All of FoxSDR's file readers of that kind now report the problem and carry on.

**Stopping a recording no longer freezes the window while the disk finishes the file.** Stop had to flush the recording, write its header and close it on the thread that draws the window, and a slow disk held the window for as long as that took. It is now finished in the background; a Record pressed while the previous file is still closing is remembered and starts the moment it is closed. The same was done for importing a bookmark file, opening a patch Radio's I/Q recording, and removing an MP3 patch speaker. The one file read left on that thread is the reopening of a saved I/Q file at start-up.

**A freeze report keeps its log when the disk is slow.** The end of the log is the most useful part of a freeze report, and it was the part that went missing when the freeze was caused by a slow disk: the report waited for a log line that was itself waiting on that disk. The two no longer share a lock.

**The anonymous usage record also counts slow frames and quiet recoveries.** With Usage reporting and Diagnostics both on, FoxSDR now counts how many times the window took a quarter of a second, a second or five seconds to draw a frame and in which of eighteen parts of drawing it, and how many times it recovered from a fault on its own, from a fixed list of thirteen places (the sound output restarted, a radio reopened after a driver fault, a settings save that failed, and so on). Counts and words from fixed lists only - never a device name, a serial number, a path, a frequency or the text of a message. PRIVACY.md lists every word. 0.99.64 does not send these two.

**A settings folder that cannot be written is no longer retried every two seconds for ever,** and a failed screenshot no longer writes its folder's path into the log or leaves a broken picture behind.

Still not fixed, and known: a recording whose disk disappears part way ends as an empty file with nothing on screen saying so; an MP3 patch speaker whose folder is removed says nothing; a log file held open by another program at start-up is silently switched off.

---

**SHA-256 of `foxsdr-setup-0.99.65.exe`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.65-x86_64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.65-linux-x64.tar.gz`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.65-aarch64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.65-linux-arm64.tar.gz`:**
`TBD`
