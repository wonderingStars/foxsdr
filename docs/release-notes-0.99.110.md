# FoxSDR 0.99.110 — Windows

**0.99.110 repairs the faults that the first hours of 0.99.109 showed on users' computers, and adds one feature: the FoxSDR team can send a short message to the owner of one particular radio.**
Everything in the notes for 0.99.109 still applies.

**Still not fixed.** On a computer that cannot keep up with the chosen sample rate, samples can be dropped for a long time and the window does not warn you: if the sound or a decoder breaks up, lower the sample rate or stop plugins you are not using. The Airspy HF+ fault with a crystal trim that will not apply is still open. Six plugins still do not set their mode on START (weather report, ALE, SITOR telex, AM stereo, FMS status, caller ID): set the mode yourself.

Where a statement could only be tested with test signals or a stand-in program, and not with real equipment, the sentence says so.

---

## What is fixed

**A radio that another FoxSDR window is using.**
- When you pick a radio that another FoxSDR window on this PC already has open, FoxSDR now asks: "Move it here?" with the keys **Move it here** and **Leave it there**. On "Move it here" the other window stops using the radio, changes to the signal generator and shows a notice.
- If the other window is recording, nothing is moved: you are told to stop the recording there first. If the other window does not answer within 5 seconds, nothing is changed and you are told so.
- If another program has the radio (or the Legacy window, which does not take part), FoxSDR says so. It cannot move a radio out of another program.
- The two windows talk over a local channel that only your own Windows account can open. Nothing goes over the network.

**RTL-SDR and HackRF that stopped for no reason.**
- One failed read from an RTL-SDR used to close the radio with "it may have been unplugged", and you had to pick the radio again. FoxSDR now restarts the stream by itself, up to three times. Only when Windows says the device has gone does it say "unplugged". If the restarts do not help, the message carries the Windows error.
- An RTL-SDR or HackRF stream that delivers nothing for three seconds is restarted the same way, and a radio whose USB transfer never returns is reported as a fault and no longer looks open while it delivers nothing.
- These were tested with a stand-in for the USB device, not on a real radio.

**Changing the sample rate while running.** The buffer between the radio and the signal processing kept the size of the old rate after a change of rate while running, which could lose samples at a higher rate. It now gets the size a fresh start at the new rate gets. Tested with test sources, not with a real RSPduo.

**RTL-SDR at 2.0 MS/s.** The default rate of 2.0 MS/s was not on the RTL-SDR's list, so the radio ran at 2.048 MS/s and each start was recorded as a setting out of range. 2.0 MS/s is now on the list. Not tried on a real dongle.

**Maps.**
- The Map Imagery (OpenStreetMap) plugin draws its tiles under the aircraft map. This was seen on 0.99.109 with a generated aircraft. The Satellites map uses a different projection and keeps the coastline; it now says so in its chart key when the plugin is running. The ship and station maps were not tried.
- The line under a map said "No map tiles are kept on disk." while the plugin kept them. It now counts the plugin's tiles, and **Clear tile cache** deletes them.
- Map Imagery 1.3.1 (a change of words only) says in the plugin store which maps get tiles, and what a tile request tells the map server: roughly where the map is looking.

**VOR.** A click on the round OBS course knob did nothing. A click now sets the course to the direction of the pointer from the knob's middle; dragging works as before.

**Reports.**
- A saved radio that is busy at start-up is retried several times. Each retry sent its own failure report; now the first and the last failure are sent, and the tries between are counted in them.
- A sound output that was opened again is no longer reported as "this computer fell behind".
- A radio that both FoxSDR's own driver and a SoapySDR module list is reported once, not twice.
- A piece of a folder path with a space in it (for example under "Program Files") is now masked whole in a report.

---

## What this version sends: the two changes

**Crash reports can carry the program's last words.** If FoxSDR ends in a way it cannot report itself, and Diagnostics is switched on, the crash report now also carries the last lines (at most 40) that FoxSDR or a library inside it printed to its error output. Account names, network addresses, serial numbers and quoted names are masked. This is typically a library's own error message and can name files or components, but never signal content. The lines are kept in a file in FoxSDR's log folder for up to 72 hours; the file itself is never sent. With Diagnostics off nothing of this is kept or sent.

**A successful change of sample rate while running** is now recorded in the radio's report as a settings change, with the rate that was reached. It uses the fields 0.99.109 already had.

The website's privacy page says both.

---

## A message from the FoxSDR team for one radio

- When FoxSDR checks for updates, the answer can carry a short message meant for one particular radio, for example when a report shows that one radio has a fault its owner should know about.
- FoxSDR shows it as a sheet titled "A message from the FoxSDR team", with the radio's model and the last four characters of its serial number, the date and the text. One key, **Seen**, closes it. Each message is shown once.
- **The website is never told which radios you have, and never told whether you saw the message.** The message is not addressed by serial number. It is locked with a one-way scrambled code; your copy of FoxSDR tries that code against the radios it has itself found on your computer and shows the message only if one matches. FoxSDR sends nothing new for this.
- The text is shown as plain text: nothing in it can be clicked.
- It needs the update check to be on. It is shown in the new window only, not in the Legacy window, and not in the Microsoft Store copy.
- A radio with a factory serial number that many radios share (such as 00000001) cannot be sent a message from the website.

---

## Things to know

- The installer is not code-signed, so Windows shows its "Windows protected your PC" warning. Check the download: the SHA-256 below must match what `Get-FileHash <file>` prints in PowerShell.
- This release is for Windows. The Linux download stays at 0.99.109.
- When these notes were written, the new sheets (move the radio, the message from the FoxSDR team), the OBS knob click and the tile-cache line had been tested by the program's own tests only. A check with a real mouse and a real RTL-SDR was still to be done; what it found is on the release page.

---

**SHA-256 of `foxsdr-setup-0.99.110.exe`:**
`TBD`
