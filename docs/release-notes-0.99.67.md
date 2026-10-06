**Closing the console window no longer ends FoxSDR on the spot.** On Windows, FoxSDR opens with a console window of its own - a black window, or a Windows Terminal tab, titled cascade.exe - beside the real one. Closing that window, or pressing Ctrl+C or Ctrl+Break in it, used to end the program at once: nothing was saved, the radio was left open, and the next start found a session that had ended unexpectedly. Now those three ask FoxSDR to close the way its own close button does, and the shutdown runs: settings and bookmarks written, the radio closed. If the shutdown takes longer than the five seconds Windows allows a closing console, Windows ends the program as before. From a field report of 2026-10-06.

Still not fixed, and known: the console window itself still opens with FoxSDR on Windows; a recording whose disk disappears part way ends as an empty file with nothing on screen saying so; an MP3 patch speaker whose folder is removed says nothing; a log file held open by another program at start-up is silently switched off.

---

**SHA-256 of `foxsdr-setup-0.99.67.exe`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.67-x86_64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.67-linux-x64.tar.gz`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.67-aarch64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.67-linux-arm64.tar.gz`:**
`TBD`
