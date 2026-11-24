# FoxSDR 0.99.114 — Windows

**0.99.114 is a small release. A radio that is saved in a stopped patch can be chosen again, an RX888 mk2 should now open, a wrong Windows driver on an RTL-SDR is named and can be fixed in one click, the CAT emulation port is reachable from this computer only, and the update check and the network radios are better protected against bad input.** Everything in the notes for 0.99.113, 0.99.112, 0.99.111, 0.99.110 and 0.99.109 still applies.

**Still not fixed.** On a computer that cannot keep up with the chosen sample rate, samples can be dropped for a long time and the window does not warn you: lower the sample rate or stop plugins you are not using. A USRP B200 may not appear in the Source list after the search at start-up; press Refresh. A radio that is saved in a Radio part of a patch that is running is still refused as the Source (FoxSDR says so, and does not yet let you take the radio back from a running patch). Six plugins still do not set their mode on START (weather report, ALE, SITOR telex, AM stereo, FMS status, caller ID). The sheets shown when a radio is in use by another FoxSDR window are in English in every language.

Where a statement could only be tested with test signals or a stand-in, and not with real equipment or a real mouse, the sentence says so.

---

## What is new and what is fixed

### Radios
**A radio saved in a stopped patch can be chosen again.** Before, a Radio part on the Patch page held its radio even when the patch was stopped (or the part was switched off), so the radio was refused as the Source, as another radio, or as the radio of Radio 2 to 8, and the window could not be used with it (a USRP B200 was seen that could not be selected this way). A part now holds its radio only while it is open. While the part is open the choice is still refused, with the same sentence and popup as in 0.99.113. Three more cases were closed in the same change: stopping a patch while one of its parts is still opening its radio no longer lets the radio be opened twice, a radio that is still being opened cannot be given to a second radio meanwhile, and a part that is pointed at another radio or deleted while its old radio is opening no longer frees that radio too early. Tested with stand-in radios; not tried with a real B200 or a real mouse.

**RX888 mk2.** FoxSDR's own RX888 mk2 driver could not open a real RX888 mk2 in the field (one user's logs of 24 November). It now waits longer for the radio's first answers, asks the first request a second time after a refusal or a silence, does not give up on a stop or a close while the tuner is busy, does not touch the VHF tuner on HF, and says "The RX888 is not answering. Unplug the RX888, wait ten seconds, plug it in again, then choose it." when the radio stays silent. Every open also writes one diagnostics line saying which request stopped, how long it took, whether this open loaded the radio's firmware, and whether the link is USB 3 or USB 2. This was fixed from one user's logs and reading the driver's code; it has not been confirmed on a real RX888, and no RX888 was available to test with.

**RTL-SDR on the wrong Windows driver.** When an RTL-SDR cannot be opened, FoxSDR now says which of three things it is: Windows has not given it the WinUSB driver FoxSDR needs, no RTL-SDR is plugged in, or FoxSDR cannot tell. Beside the first sentence the new window shows an INSTALL DRIVER key that does the same install as the Radio setup page; the line under it shows the result. Tested with stand-ins and the window's logic; the key was not clicked in the real window and not tried with a dongle on the wrong driver.

### Network safety
**CAT emulation listens on this computer only.** The "Listen on every address" tick of the "Answer CAT commands" section (the port used by logging programs) is gone: that port had no password and no list of allowed computers, so any computer that could reach it could read the frequency and retune. If you had the tick on, CAT emulation keeps working on this computer, and the page says once, "CAT emulation now listens on this computer only, so other computers can no longer reach its port." The rigctl-style CAT server already worked this way. The classic window never had the tick. Further small fixes to the CAT and rotator servers came out of the review.

**The update check and the plugin list are better protected.** The installer that an update downloaded is checked again, through a handle that is kept open, right before it is started, and the folder that holds it is held too and may not be a link. The update check and the regional plugin list ignore the three environment settings that only the tests use. A plugin helper's own files are held to the regional list's address prefix, more file names that the store reserves for itself are refused, reserved Windows device names are recognised more reliably in plugin file names and companion names, and a plugin file never replaces something that is not a file. A web address is cut where its host ends, not only at "/".

**Network radios survive bad input.** A stray network packet no longer leaves an ASCP radio's reader stuck; a SoapyRemote control reply has one time limit as a whole, and closing it says goodbye at most once; SDRconnect keeps only the properties the program reads, each of bounded size; a sample rate that a peer reports is used only if a receiver could run it (SoapyRemote, SDRconnect, SpyServer, Aaronia, ASCP); a sample that is not a number is zeroed before it reaches the ring; the program that searches for Soapy radios is limited in how much it can say and is ended completely when it is over; and when you type a unicast address for an HPSDR radio, only that address can be the radio. Tested with stand-in servers; not tried with real network radios.

### Languages
- Five new interface sentences are translated in all 33 languages (machine-made; no speaker has read them). Two old sentences, the CAT tick and its note, are removed.

---

## Things to know

- The installer is not code-signed, so Windows shows its "Windows protected your PC" warning. Check the download: the SHA-256 below must match what `Get-FileHash <file>` prints in PowerShell.
- This release is for Windows. The newest Linux build is the 0.99.109 tarball on GitHub's release page; the website still offers 0.99.103 for Linux.
- The automated tests of the engine and of the new window were run on this version. The real-mouse check of every screen was not done for this release.
- Everything above that touches a radio was tested with stand-in radios and recorded logs, not with the radios themselves.

---

**SHA-256 of `foxsdr-setup-0.99.114.exe`:**
`TBD`
