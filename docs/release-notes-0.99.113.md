# FoxSDR 0.99.113 — Windows

**0.99.113 repairs faults that users' computers showed in the field with several kinds of radio (SDRplay, USRP B200, RTL-SDR, rtl_tcp, Airspy HF+, Mirics), makes the new window fit your screen, and now says why a radio cannot be chosen when something else holds it.** Everything in the notes for 0.99.112, 0.99.111, 0.99.110 and 0.99.109 still applies.

**Still not fixed.** On a computer that cannot keep up with the chosen sample rate, samples can be dropped for a long time and the window does not warn you: lower the sample rate or stop plugins you are not using. A USRP B200 may not appear in the Source list after the search at start-up; press Refresh. A radio that is saved in a Radio part of a patch on the Patch page is still refused as the Source (see below): FoxSDR now says so, but it does not yet let you take the radio back. Six plugins still do not set their mode on START (weather report, ALE, SITOR telex, AM stereo, FMS status, caller ID). The sheets shown when a radio is in use by another FoxSDR window are in English in every language.

Where a statement could only be tested with test signals or a stand-in, and not with real equipment or a real mouse, the sentence says so.

---

## What is new and what is fixed

### A radio that cannot be chosen now says why
**The radio refusal is explained, not removed.** When a radio cannot be chosen because a Radio part of a saved patch on the Patch page (or another radio, or the transmitter) holds it, FoxSDR now says so on the Source line and, for the patch case, in a popup with a key that opens the Patch page; the radio is still refused in that case: the refusal is explained, not removed (changing who holds a radio comes in a later release).
- The Source line reads, for a patch: "This radio is set as the radio of the part "NAME" on the Patch page. Change or remove that part, or start the patch to use the radio there." NAME is the caption the part shows on the Patch page (a part you have not renamed is called "Radio"; two parts can have the same caption).
- For another radio: "This radio is in use by Radio N. Choose another radio there first." For the transmitter: "This radio is in use by the transmitter."
- The popup (both windows) has the keys "Open the Patch page" and "Close". Neither key changes the patch or who holds the radio.
- A refused "Add a radio" shows the refusal on the Add card as before and raises no popup. The popup never replaces or answers a question that is already open (the "Move it here / Leave it there" question or the Legacy question).
- The new sentences are translated in all 33 languages; the translations are machine-made and no speaker has read them.
- Tested with test signals and a stand-in radio. The popup was not seen in the real window and not tried with a real mouse.

### Fixes from the field
**The new window fits your screen.** It used to open larger than the usable screen on small or scaled screens (for example 1366 x 768, or 125 % and 150 % scaling) and could sit partly off the screen. It now fits itself at start. Where the screen is smaller than the smallest window (1024 x 640), the window is maximised. Interface size "Auto" now lowers its size to what fits. The Legacy window is fitted the same way. Tested with stand-in screen sizes, not on real small screens.

**The map keeps its coastline when map pictures cannot be loaded.** On the plugin maps (for example ADS-B) and the Patch page's map, the coastline stays until a map picture has been drawn. If pictures keep failing, a quiet line over the map says why ("Map pictures could not be loaded: ... The built-in coastline is shown instead."), without any web address in it.

**Gain.** Under automatic gain the gain slider is greyed; it now says "Automatic gain is on. Switch it off to set the gain by hand." On an RTL-SDR, setting a gain by hand now leaves automatic gain in every setting path, and a gain written by software while automatic gain is on switches it off for that session only. Each gain write is one line in the diagnostics log. We could not reproduce the report that "changing gain has no effect" on a real dongle, so this should help, but it is not confirmed.

**Less chance of a sudden close of the new window.** One byte of text that was not valid UTF-8 could stop the new window at once. Text is now repaired before it reaches the window, in every place we know of. The exact source of the bad text was not found, so this should reduce these closes; we cannot yet say it ends them.

**Freeze reports.** Freezes are now recorded with more detail (which part stalled, what it waited for). A slow disk, a radio being stopped or switched, and a screen waking from sleep should no longer be reported as freezes, and the main loop no longer waits for the file system. The opt-out for radio failure reports is now kept across a restart.

**Sound device that comes back.** A sound output that disappears and returns (for example a headset) should be found again within about 5 seconds, and the search for it no longer holds up the window's other commands. Checked by reading the code and a stand-in, not with a real device that dies and returns.

**Too-large messages on the program interface.** When the program interface (WebSocket and server-sent events) receives a WebSocket message that is too large, it is now always answered with close code 1009 before the connection is closed; before, the connection could be reset without the code.

### Radios
**SDRplay.** A second radio in FoxSDR that asked for an RSP that another radio of the same FoxSDR already had open failed with a bare "Fail (1)". It now gets a clear sentence. A stream that merely went quiet no longer disables SDRplay until FoxSDR is restarted: the radio is closed cleanly and you pick it again. The stall sentence is now one sentence: "the SDRplay service stopped delivering (nothing for 5 s) - pick the radio again; if that fails, restart the SDRplay API service, then restart FoxSDR". The service gap counter can no longer show billions. Tested with a stand-in SDRplay library, not with a real RSP.

**USRP B200 and other SoapySDR radios.** If a USRP's stream stops delivering samples for more than 5 seconds, FoxSDR now restarts the stream, up to three times, and if it still delivers nothing it says so and tells you to power-cycle the radio. This restart is for USRP radios only; other SoapySDR drivers wait as before. If a driver's radio search crashes, the Source panel says so ("The search for ... radios crashed; press Refresh to try again"); Refresh asks that driver again (twice at most per session), and if the search runs out of time it says that instead. A saved USRP is opened by its driver, serial and type only. We have not been able to show that this fixes the "No devices found" reports.

**RTL-SDR.** When an RTL-SDR drops off the USB bus and comes back within 5 seconds, FoxSDR now opens it again, puts your settings back and carries on, and a recording continues. Tested with a stand-in; it should work on a real dongle but has not been tried on one.

**rtl_tcp.** Opening an rtl_tcp server right after leaving it failed, because the real server needs several seconds. FoxSDR now waits up to 12 seconds for the server's first answer and retries a refused connection for 6 seconds, so an absent server now answers after about 7 seconds instead of 2. The saved rtl_tcp server is retried at start-up. Addresses typed as "rtl_tcp://host:port" or with a trailing slash or spaces are now accepted. A second open can no longer hold up the window. The error sentences are clearer. Tested against a real rtl_tcp program on this computer; not tested with a remote server (Wi-Fi or Raspberry Pi).

**Airspy HF+ and Mirics (MSi2500).** These radios opened and then closed at every START with "WinUsb_SetPipePolicy(RAW_IO) failed (Windows error 87)", and gave no samples. FoxSDR now selects the radio's streaming setting at open and no longer ends the stream when that call is refused. This should fix it; it has not been tried on a real HF+ or Mirics radio.

### Speed
**Faster demodulation.** The filters, the stereo decoder and the spectrum use less processor time, so a slow computer should keep up at a higher sample rate. The results are the same bit for bit except for the tuning oscillator, which differs by one unit in the last digit in about one value in twenty million (more than 200 dB below the signal). Nothing in transmit changed. No percentage is given because it depends on the computer.

### Privacy and reports
- A crash report that contains a Rust panic message now keeps the line and column but never the file name.
- The log lines that go with a report now also say how the computer was doing (on battery or power saver, number of processors, processor speed as a percentage of its top speed, window hidden or screen off, memory in use, FoxSDR's own processing time). The privacy page says so. The processor speed is never sent in MHz.

### Languages and manual
- A Korean user manual (MANUAL.ko.md) is added to the repository; the installer does not install it.
- Twenty-four new interface sentences are translated in all 33 languages (machine-made; no speaker has read them).

---

## Things to know

- The installer is not code-signed, so Windows shows its "Windows protected your PC" warning. Check the download: the SHA-256 below must match what `Get-FileHash <file>` prints in PowerShell.
- This release is for Windows. The newest Linux build is the 0.99.109 tarball on GitHub's release page; the website still offers 0.99.103 for Linux.
- The full automated tests were run on this version. The real-mouse check of every screen was not done for this release.

---

**SHA-256 of `foxsdr-setup-0.99.113.exe`:**
`ccbde4ef1a7096a1a00d13998f4ffb5b0a50e63b55bd671278cc0d51cb82bcdd`
