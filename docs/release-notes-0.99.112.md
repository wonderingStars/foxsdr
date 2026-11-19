# FoxSDR 0.99.112 — Windows

**0.99.112 adds, in the new window, a details card with the aircraft's photo on a plugin's map page and a SHOW key on the Fitted modules page, and repairs the serial (COM) port chooser, which did not open.** Everything in the notes for 0.99.111, 0.99.110 and 0.99.109 still applies.

**Still not fixed.** On a computer that cannot keep up with the chosen sample rate, samples can be dropped for a long time and the window does not warn you: lower the sample rate or stop plugins you are not using. A USRP B200 may not appear in the Source list after the search at start-up; press Refresh. Six plugins still do not set their mode on START (weather report, ALE, SITOR telex, AM stereo, FMS status, caller ID). The sheets shown when a radio is in use by another FoxSDR window are in English in every language.

Where a statement could only be tested with test signals or a stand-in, and not with real equipment or a real mouse, the sentence says so.

---

## What is new and what is fixed

**The details and the photo are on the map.** In the new window, clicking a target on a plugin's map (or its row in the list) used to fill only the "Target details" section in the left panel, away from the map, and no photo was ever shown. Now a card opens at the foot of the list beside the map with the target's name, every detail the plugin gives and, when the plugin offers one, its photo.
- The photographer's credit is written directly under the photo, and a click on the photo opens its page in your browser.
- A photo is kept in memory only while it is shown. Nothing is saved to disk.
- The card closes with its Close key or a click on empty map. The Escape key does not close it yet.
- The "Target details" section in the left panel shows the same photo and credit.
- For aircraft, the photo comes from the ADS-B plugin, version 1.13.0 or later, from the plugin store, and only while its setting "Online aircraft info + photos" is ticked. With that setting ticked the plugin asks foxsdr.com, api.adsbdb.com and api.planespotters.net about the aircraft you look at; its page in the store says exactly what is sent.
- Tested with a stand-in photo. No photo from the real service has been seen in this window yet, and it was not tried with a real mouse.

**SHOW on the Fitted modules page.** Every installed module's row has a SHOW key. It opens that module's own page and brings it to the front: a page that was closed comes back, and one that is already open is not opened twice. A module that is stopped is started first. A module with no page of its own has the key greyed. Not tried with a real mouse; the starting of a stopped module and the "not opened twice" rule were checked by reading the code, not by a test of their own.

**The serial (COM) port chooser opens again.** The GPS port chooser on a map page (under Map options) and the one in the Radar section did nothing when clicked: the list of ports was read only once, when the SYSTEM panel's Serial section was first opened, and a chooser with an empty list ignored the click. The chooser now reads the ports every time it is opened, and when there is none it opens and says "No serial port was found." Not tried with a real serial device.

---

## Things to know

- The installer is not code-signed, so Windows shows its "Windows protected your PC" warning. Check the download: the SHA-256 below must match what `Get-FileHash <file>` prints in PowerShell.
- This release is for Windows. The newest Linux build is the 0.99.109 tarball on GitHub's release page; the website still offers 0.99.103 for Linux.
- The new words of this version (the photo card, SHOW, the port chooser's line) are in English in every language for now.
- This version was released on its own tests and pictures, without the full test run and without a real-mouse check.

---

**SHA-256 of `foxsdr-setup-0.99.112.exe`:**
`TBD`
