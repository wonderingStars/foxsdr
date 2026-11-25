# FoxSDR 0.99.116 — Windows

**0.99.116 is a small repair release: it fixes the new window closing by itself, an installer that could wipe your settings and plugins on every update, and a stall that a slow disk could cause while a radio report was being written.** Everything in the notes for 0.99.115, 0.99.114, 0.99.113, 0.99.112, 0.99.111, 0.99.110 and 0.99.109 still applies.

**Still not fixed.** The items listed as not fixed in the 0.99.115 notes are unchanged by this release, and so is everything else that is not named below.

Where a statement could only be tested with test signals or a stand-in, and not with real equipment or a real mouse, the sentence says so.

---

## What is new and what is fixed

### The new window could close by itself
**Fixed: a crash of the new window caused by a tooltip's timer.** A tooltip's timer could run after the control it belonged to had already been removed, and the program then used memory that was no longer its own. This closed the new window by itself (and, with Diagnostics on, produced a crash report). It happened when a page rebuilt a row while the pointer was resting on it. In 0.99.115 and earlier the tooltip timers lived inside every control; now there is one clock for the whole window, which outlives every control, and a removed control can no longer open, close or write a tooltip.
How often it was seen, and how it was measured: on Linux, a scripted run of the new window crashed in 16 of 30 runs (53 %) with the old code and in 0 of 60 runs with the fix; putting the old code back brought the crashes back (9 of 20). On Windows, the 0.99.115 program was made to crash in a reproduction on the same fault (20 of 20 forced cases, and 17 crashes in 125 scripted hover runs of the shipped program); the fixed program ran the same scripted run 60 times on Windows with no crash, but the old Windows program was not run again for that comparison, so there is no before-and-after rate for Windows from this release's own tests. How often it happens to you depends on what you click and hover; the crash reports of 0.99.114 are consistent with this fault, but the identical report was not reproduced.
What a tooltip does for you is the same: it opens 600 ms after the pointer rests, and a tooltip left behind by a moved item is now noticed within 20 ms (it was 150 ms). The same Slint behaviour could in principle still affect other timers in the window (menu rows, palette rows, some pages); they were not changed in this release.

### The installer could wipe your data on every update
**Fixed: the installer wiped settings and installed plugins on every update for anyone who had ever ticked "Clean install".** The installer remembers the choices made on its options page, and it also remembered the "Clean install" tick, so the next installation, including the silent update that FoxSDR runs when you close it, deleted `%APPDATA%oxsdr` and `%LOCALAPPDATA%\FoxSDR` (settings, installed plugins, caches, logs, crash reports) again. The installer of 0.99.115 does this; the installer of 0.99.116 does not.
From 0.99.116: "Clean install" starts unticked on every install, whatever you chose before. Data is deleted only when you tick it on the options page in that run, or when a silent install is given `/CLEANINSTALL=1`. A remembered tick, and `/TASKS` or `/MERGETASKS` naming it in a silent install, never delete anything. Installing 0.99.116 also removes the remembered tick from your computer, so an older installer run later cannot inherit it. Your recordings are never touched by setup.
**Data already lost to an earlier update cannot be brought back by this release.** If your plugins are gone, tick the notices and press GET EVERYTHING in the plugin store to install them again (the same sentence the manual uses for a fresh profile).

### A stall while a radio report was written
**Fixed: no thread of the engine waits any more for the disk while a radio report is written.** A crash feed from 0.99.114 showed the engine's control thread stuck for 5.5 seconds writing a report file to a slow disk. Now the engine hands the bytes to the operating system and a separate thread of the reporter forces them to the disk. Nothing else changes: the same files with the same content are written at the same moments, what is sent and when is unchanged, and with Usage reporting off nothing is filed and nothing is sent. The one difference: after a power loss or a crash of the whole computer, a report written in the last moments before it can be lost, where it was forced to disk at once before (a program that hangs or is killed loses nothing). The contract (`docs/radio-reports-contract.md`, 3.9) says so.

### Languages
- No interface sentence changed in this release: no translation is added or changed.

---

## What was and was not tested

- Both full test suites (every headless and new-window test, then every new-window screen drive) were run on the exact files this installer is made from. Their totals are in the release record, not here, because the notes are fixed before the tests run.
- **No independent code review of any change in this release was made.**
- The installer was tested end to end only in silent runs against a test copy of itself (35 checks passed, 1 skipped; removing either safeguard brought the wipe back). Its wizard pages (the box starting unticked over a remembered tick, a tick in this run still wiping, the desktop shortcut still remembered) were never tested with a pointer or a visible window.
- The installer was not install-tested on a clean machine, and no machine-wide (all users) install was tested.
- Nothing ran on a real radio, and there was no real-mouse drive of every screen.
- The report-writing change was tested with a disk made to answer slowly by a test hook, not with a real slow disk.
- The installer is not code-signed, so Windows shows its "Windows protected your PC" warning. Check the download: the SHA-256 below must match what `Get-FileHash <file>` prints in PowerShell. The translations are machine translations; no speaker has read them.
- This release is for Windows. Linux and Raspberry Pi files are not part of it.

---

**SHA-256 of `foxsdr-setup-0.99.116.exe`:**
`TBD`
