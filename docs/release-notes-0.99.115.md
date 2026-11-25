# FoxSDR 0.99.115 — Windows

**0.99.115 is a small release about one promise: with Usage reporting switched off, FoxSDR now sends nothing about your radios or your use.** That includes the report it used to send when a radio did not open. Everything in the notes for 0.99.114, 0.99.113, 0.99.112, 0.99.111, 0.99.110 and 0.99.109 still applies.

**Still not fixed.** The items listed as not fixed in the 0.99.114 notes are unchanged by this release.

Where a statement could only be tested with test signals or a stand-in, and not with real equipment or a real mouse, the sentence says so.

---

## What is new and what is fixed

### Privacy
**With Usage reporting off, FoxSDR sends no report about a radio that did not open.** Versions 0.99.109 to 0.99.114 sent that report whatever Usage reporting said (without an install identifier when it was off). From 0.99.115 the report is not built, not written to disk and not sent when Usage reporting is off. Crash and freeze reports are not part of this: they follow the Diagnostics switch, and a crash report can still name the radio that was in use.

**Reports queued by an earlier version are deleted.** Failure reports that 0.99.109 to 0.99.114 left waiting on your computer are deleted when you switch Usage reporting off, and at start-up when it is already off. Failure reports held in memory are forgotten when you switch it off, so switching it back on does not send what was held.

**Switching Diagnostics off takes effect at once.** In the new window the sending of crash and freeze reports now stops the moment you switch Diagnostics off (a report being sent is stopped), not at the next start. The previous window does the same.

**Nothing noted while a switch was off is ever sent.** Switching Usage reporting or Diagnostics off now also discards what was collected before it, and anything noted while a switch is off is not collected at all: switching it on again starts from empty, and nothing from the time it was off is sent afterwards. For Diagnostics, a crash or freeze report that was written while Diagnostics was off, or that was still waiting when you switched it off, is kept on your computer as your own copy and is never uploaded, even if you switch Diagnostics on again later. The choice is also saved to your settings file at once, not at the next periodic save.

**Two copies of FoxSDR on one profile.** When two copies run at once on the same settings, only the first one collects or sends anything and makes the update check; the second one reports nothing of its own (no usage record, no radio reports, no crash or freeze upload, no update check) and no longer overwrites the first one's switches or identifier when it saves. Switching Usage reporting, Diagnostics or the update check off in either copy silences both within a few seconds, and the first copy never writes a switch back on. The price: with Usage reporting on, the second copy's use is not counted, and in the second copy the Usage tick shows unticked and the Diagnostics and update-check ticks show off whatever the settings file says (a click there is a request the first copy adopts). Limits, stated honestly: a message that is already being sent at the moment of switching off is not recalled; for about a second after the other copy switches off, a tester's queued report (previous window) can still go; two copies saving settings in the same few milliseconds can race, and then a switch-off made in the first copy can be undone until you click again. A second copy that crashes in its first moments can write a crash report that the first copy may upload, because crash capture is armed from the stored switch before the window turns it off; the two copies also share one log file.

**For beta testers (previous window):** with Usage reporting off the tester session report is not built or sent, and the tester panel says so (one new interface sentence, machine-translated in all 33 languages).

**With Usage reporting, Diagnostics and the update check all off, FoxSDR contacts no server unless you press something that needs one** (the plugin store, a bug or feature report, an installer download). The previous window's beta-tester sender, which used to send reports and exchange a portal link on its own, now also waits for Usage reporting to be on.

**Wording.** The privacy page (`PRIVACY.md`) and `docs/DIAGNOSTICS.md` now say the above, and the sentence that said the server refuses a message holding a frequency, a file name or similar is replaced by what the server really checks and what it cannot recognise (FoxSDR removes or never writes those things before it sends). Two sentences in the program that describe this are changed.

What did not change: with Usage reporting on, the failure report is sent as before, with the same fields. The server still accepts the anonymous form that 0.99.109 to 0.99.114 sent, and a radio's serial number is still the way to ask for one of those to be erased.

### Languages
- Three changed or new interface sentences (the two on the Usage reporting page and the tester-panel sentence) are translated in all 33 languages (machine-made; no speaker has read them). The two older sentences they replace are removed.

---

## Things to know

- Not verified: no real radio was opened (a failing radio was a stand-in on a closed local port), and in the end-to-end runs the update check was switched off, so "contacts no server" for the update check was not observed with it on. The "nothing from a switched-off period is sent" cases were run as three starts with the switches set in the settings file (on, then off, then on again), not by clicking the switch in a running program. The two-copies cases were run with two real programs on one scratch profile, but the switch-off was written into the settings file from outside, not clicked in either program, and the first minute of a radio stream was tested with a stand-in receiver, not a real one; the clicks themselves are covered by automated tests with stand-ins, and the previous window's Diagnostics tick by a test of its click handler, not by a real pointer click.
- The installer is not code-signed, so Windows shows its "Windows protected your PC" warning. Check the download: the SHA-256 below must match what `Get-FileHash <file>` prints in PowerShell.
- This release is for Windows. The newest Linux build is the 0.99.109 tarball on GitHub's release page; the website still offers 0.99.103 for Linux.
- The real-mouse check of every screen was not done for this release.

---

**SHA-256 of `foxsdr-setup-0.99.115.exe`:**
`6de9ca32f76c191a95dd5b8d803c7c25b769e1cb6316ce8062fa963341672b53`
