**The Airband monitor says what it is hearing where you are looking.** While LISTEN plays, the spectrum carries a line at its top left, over the channel marks: the block of frequencies on the air and, in amber, the names of the channels whose squelch is open - "Hearing: O'Hare Tower, O'Hare Ground". The same two lines have moved up in the Airband section, from under the keys to just above the list of frequencies, and the second line is always there while the monitor plays ("Hearing: -" while nobody is talking), so the rows and the keys no longer shift down a line when someone starts to speak. A tester listening to O'Hare, whose twenty-nine frequencies make the section taller than the window, had to hold the rail scrolled to its foot to read which frequency was talking: measured on 0.99.70 in a 1600 by 1000 window with the rail scrolled to the section, the LISTEN key sat at pixel row 909 of 1000 and the two lines under it, off the bottom of any shorter window. Asked for on 2026-10-07. The same tester asked for the Airband controls as a floating window; FoxSDR's window has no floating panels, and the caption puts the message where the eye already is, so that is not done. Nothing else changes: the strings are the ones the section already had, in every language.

Still not fixed, and known: the two fast-fail deaths reported from the field (0.99.64 on an RSP1 after 48 minutes, 0.99.65 on an RTL-SDR after 12 seconds) are not explained - since 0.99.69 every report names the other programs' DLLs in the process, and since 0.99.66 the sentinel names the faulting module, so the next one says more; the other twenty-eight languages' airband strings were not reviewed, and Italian and Dutch call the frequency list "Segnalibri" and "Bladwijzers" where German, French and Spanish call it by a name of its own, a difference older than 0.99.65 that was followed, not resolved; a recording whose disk disappears part way ends as an empty file with nothing on screen saying so; an MP3 patch speaker whose folder is removed says nothing; a log file held open by another program at start-up is silently switched off; the rtl_tcp source's Stop and Start keys in the window itself, and its Linux build, have not been tried against a real server.

---

**SHA-256 of `foxsdr-setup-0.99.71.exe`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.71-x86_64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.71-linux-x64.tar.gz`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.71-aarch64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.71-linux-arm64.tar.gz`:**
`TBD`
