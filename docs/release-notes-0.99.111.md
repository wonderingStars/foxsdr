# FoxSDR 0.99.111 — Windows

**0.99.111 fixes three faults of the new window: a plugin's map was squeezed into a corner of its page, the list of radios stayed empty until Refresh was pressed, and a plugin page moved into its own frame jumped about when it was resized or dragged.** Everything in the notes for 0.99.110 and 0.99.109 still applies.

**Still not fixed.** On a computer that cannot keep up with the chosen sample rate, samples can be dropped for a long time and the window does not warn you: lower the sample rate or stop plugins you are not using. The ADS-B plugin does not load its Mode A/C option or your saved options at start. Six plugins still do not set their mode on START (weather report, ALE, SITOR telex, AM stereo, FMS status, caller ID). The sheets shown when a radio is in use by another FoxSDR window are in English in every language.

Where a statement could only be tested with test signals or a stand-in, and not with real equipment or a real mouse, the sentence says so.

---

## What is fixed

**The map is the page.** On every plugin page that shows a map (aircraft, ships, stations, balloon sondes, satellites and the others), the controls above the map took most of the page and the map was left in the bottom corner, cut off in a small window. The controls now take only their own row, and the map takes all the rest at every window size.
- The rarely used controls (coverage, trails, line or ribbon, the GPS receiver) are behind one key, **Map options**, closed until you press it.
- The bar between the target list and the map can be dragged, from hiding the list to half the page. A double-click puts the list back to its usual width.
- A map page moved into its own frame fills that frame, and the frame can now be resized by its right and bottom edges as well as its corner.
- Whether Map options is open, and the width of the list, are kept until you close FoxSDR, not between starts.
- Measured by the program's own tests at three window sizes and seen in pictures of the aircraft map. Not tried with a real mouse, and not opened plugin by plugin.

**Your radios are in the list without pressing Refresh.** The new window only searched for radios when Refresh was pressed, so after a first install the Source list offered only the signal generator, an I/Q file and the sound card. FoxSDR now searches once by itself shortly after it starts. A radio that is already open or being restored is not disturbed. Seen with a real RTL-SDR in a scripted test run: the list filled within seconds with no key pressed. A USRP B200 on the same computer did not appear in that run; that is not explained yet. Refresh works as before.

**A page in its own frame no longer jumps.** A plugin page moved into its own frame grew and shrank wildly while its corner was dragged, and slid about while its title was dragged: the same fault as the window edge in 0.99.109. The frame now follows the pointer from where it was pressed. Tested by the program's own tests, not with a real mouse.

---

## Things to know

- The installer is not code-signed, so Windows shows its "Windows protected your PC" warning. Check the download: the SHA-256 below must match what `Get-FileHash <file>` prints in PowerShell.
- This release is for Windows. The newest Linux build is the 0.99.109 tarball on GitHub's release page; the website still offers 0.99.103 for Linux.
- The new words on the map page (Map options and the list's resize bar) are in English in every language for now.
- This version was released on its own tests and pictures, without the full test run and without a real-mouse check, because the map fault affected every user of the new window.

---

**SHA-256 of `foxsdr-setup-0.99.111.exe`:**
`f77784c957fa2cfee6fc6d4e53dd2c9256ceaf3ed88cc2b03e3e5be574a32898`
