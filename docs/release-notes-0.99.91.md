# FoxSDR 0.99.91

**FoxSDR 0.99.91 fixes a false freeze report, and a window that went grey, when you pressed "Install now and restart" on an update - or INSTALL DRIVER in RADIO SETUP, or a link to foxsdr.com - and Windows took a few seconds to answer. While Windows checked the downloaded installer or showed its permission prompt, FoxSDR's window could turn "(Not Responding)" behind it, and since 0.99.87 FoxSDR filed that as a freeze. Nothing had frozen. The window now stays responsive while Windows asks, and no report is filed.**

This is a repair only; nothing else changes. If you updated from 0.99.87, 0.99.88 or 0.99.89 to 0.99.90 and saw the window grey out while Windows asked for permission, that was this. The Microsoft Store copy stays on its current version until the next submission is certified.

---

**SHA-256 of `foxsdr-setup-0.99.91.exe`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.91-x86_64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.91-linux-x64.tar.gz`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.91-aarch64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.91-linux-arm64.tar.gz`:**
`TBD`

## Features and fixes, 2026-10-26

**Waiting for Windows no longer looks like a freeze.** When FoxSDR hands something to Windows - the update installer it has just downloaded, the one-click RTL-SDR driver, a link to the website - Windows may take its time: SmartScreen looks at a new download, an antivirus scans it, or a permission prompt waits for your answer. Until now FoxSDR made that call on the thread that draws its window, and that call does not let the window answer Windows while it waits, so after about five seconds Windows greyed the window and offered to close it, and FoxSDR's own freeze watchdog, which since 0.99.87 trusts Windows' verdict, sent us a report of a freeze that was not one. A tester's report on 2026-10-25, 40 seconds into a session, pressing the key to install 0.99.90, showed exactly this. FoxSDR now makes the call on a helper thread and keeps the window answering Windows until it returns. You still cannot use FoxSDR while the prompt is up - that was always the case - but the window no longer greys, Windows no longer offers to close it, and nothing is reported. If the shell call fails, the message you see is the same as before.

**Nothing else about freeze reporting changes.** A window that really has stopped is reported exactly as in 0.99.87 to 0.99.90.
