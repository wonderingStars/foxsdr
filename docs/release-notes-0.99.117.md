# FoxSDR 0.99.117 — Windows

**0.99.117 is a small repair release: Usage reporting set to off now also stops the periodic heartbeat, a tip inside a dropdown list no longer flashes, a transmit radio that is slow or unreachable on the network no longer freezes the engine while it is opened, and an RTL-SDR stream stop now writes one more diagnostic line.** Everything in the notes for 0.99.116, 0.99.115, 0.99.114, 0.99.113, 0.99.112, 0.99.111, 0.99.110 and 0.99.109 still applies.

**Still not fixed.** The items listed as not fixed in the 0.99.116 and 0.99.115 notes are unchanged by this release, and so is everything else that is not named below.

Where a statement could only be tested with test signals or a stand-in, and not with real equipment or a real mouse, the sentence says so.

---

## What is new and what is fixed

### Usage reporting off now sends no heartbeat
**Fixed: with Usage reporting off, the program could still send its periodic heartbeat if the settings file still held the anonymous install id from the time reporting was on.** The heartbeat was switched on by the stored id alone, without looking at the switch. From 0.99.117 it is on only when the switch is on and an id exists, and a start with the switch off removes the stored id, from memory and from the settings file. A check of every other sender (radio failure reports, the report of the last session, the stall and health records, the tester usage report) found that each one already needed the switch as well as the id; the heartbeat was the only one that did not. It is not known from this release's tests how many installs were in the situation described.
How it was tested: with stand-ins for the network connections, the old program opened one connection with the switch off and an id left in the file and the new one opens none (switch on: unchanged, two connections); putting the old code back brought the old result back. The real internet address was never contacted, the real window was not driven, and a second copy of the program running at the same time was not tried.

### A tip inside a dropdown list no longer flashes
**Fixed: a tip on a row of a dropdown list (the dropdown rows of the Patch page are the case that was worked from) could open and close again and again while the pointer drifted on the row.** The tip is now drawn inside the dropdown itself and stays open until the pointer leaves the row. In the test, a pointer left on a row for 3 seconds made the old code open the tip 3 times and lose the pointer 3 times; the new code opens it once and never loses the pointer. This keeps the 0.99.116 tooltip repair (one clock for the whole window). Not changed: a tip whose control sits inside a popup elsewhere (lamps, the meters panel, the extend bank) can still be drawn underneath that popup.
Also in this change: the test-data file `tools/slint-clip-explained.tsv` names the corrected line of `tooltip.slint`, so the test that failed on it in 0.99.116 no longer does. That file is not part of the installer.
Tested only with a simulated pointer in the program's test harness: it was not tried with a real pointer on a real screen, which item the person who reported the flash meant is an inference, and how the tip looks in a real dropdown (position, edges, long texts) was not checked by pixels.

### The engine no longer freezes while a transmit radio opens over the network
**Fixed: opening a transmit radio that is reached over the network (the PlutoSDR) ran on the engine's control thread, so an address that did not answer could hold the whole engine until the network gave up.** The open now runs on a separate worker thread, and the engine keeps running. While it is under way the Transmit page shows "Opening ...". Keying (the latch and push-to-talk), arming, a second open and a change of address are refused as busy until the open has finished, and an open that ends late does not leave a transmitter keyed; when it ends, the address that was reached is written back. An open that fails still says so as before.
How it was tested: with fakes only, including a stand-in that answers slowly and one that never answers. The new tests failed with the fix removed and passed with it, and the main safety guard (nothing keyed by an open that ends late) was removed once more at the end to see its test fail. **No real PlutoSDR was opened, nothing was keyed, and nothing was transmitted in any test.** **No independent code review of this change exists.** It is a change on a transmit path; findings of an earlier review of the first version were closed in this version, and those closing changes were not reviewed again.

### RTL-SDR: one more diagnostic line when the stream stops
**New (a diagnostic, not a repair): when an RTL-SDR stream stops, the program now writes a second log line straight after the existing one, beginning with the same words, "the RTL-SDR stream stopped, timings:".** It gives the time since the last control transfer to the radio and whether one was in progress, the time since the program last listed or scanned USB, how long the radio had been open, and whether the computer was on mains or battery. It holds times in milliseconds and one of the words ac, battery or unknown, and no serial number, name or path. It is meant to show what the program was doing just before a dropout like the ones reported with NESDR sticks. It does not change what the program does when the stream stops, so it does not fix those dropouts.
It is a second line because the existing line is already cut by the log's width limit; that cut is unchanged and still hides the end of the first line ("restart N of 3").
Tested with a fake dongle whose reads all fail; no real stream stop was seen, so what the line prints for a real dropout is not yet known.

### Languages
- No interface sentence changed in this release: no translation is added or changed.

---

## What was and was not tested

- Both full test suites (every headless and new-window test, then every new-window screen drive) were run on the exact files this installer is made from. Result: 1774 tests, 4 failed; the 0.99.116 red (the test-data file `tools/slint-clip-explained.tsv`, not part of the installer) is fixed and passes. The 4 failures are new-window screen drives (`mapj-antenna`, `mapj-sky`, `patch-decoders`, `runs-on-radio2`) that need plugins installed; the test profile has none (the log says no plugin DLLs were copied), and they have not been shown to pass with plugins present.
- **No independent code review of any change in this release was made**, the transmit change included.
- No installer change was made in this release; the installer is built from the same installer script as 0.99.116, with the new version number.
- The installer was not install-tested on a clean machine, and no machine-wide (all users) install was tested.
- Nothing ran on a real radio (no PlutoSDR, and no NESDR drop was seen), and there was no real-mouse drive of every screen.
- The installer is not code-signed, so Windows shows its "Windows protected your PC" warning. Check the download: the SHA-256 below must match what `Get-FileHash <file>` prints in PowerShell. The translations are machine translations; no speaker has read them.
- This release is for Windows. Linux and Raspberry Pi files are not part of it.

---

**SHA-256 of `foxsdr-setup-0.99.117.exe`:**
`87494e96bd310bc39945a0f5fb8ec6dc9248f51d31a06240a8b1df0d8d28ed60`
