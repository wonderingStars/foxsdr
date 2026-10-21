**Linux: closing FoxSDR while it was sending a report could crash it.** When FoxSDR closes while a crash report, usage report or tester-link lookup is still being sent, it cancels that request. On Linux the cancel could reach the network connection just after the sending thread had finished with it and thrown it away, so FoxSDR crashed on its way out. The two now take turns, so the connection cannot be thrown away while the cancel is using it. Windows was not affected.

**Slow computers no longer file a "freeze" report just for starting up.** FoxSDR reports a freeze when its window stops responding for 5 seconds. That limit was set by measuring start-up on a fast desktop, where the first frame takes about a hundredth of a second. On an older laptop the first frames can take several seconds, and one user with an RTL-SDR sent two freeze reports that way from successive versions. The first few frames after the window opens are now allowed 30 seconds. After that the 5-second limit applies as before, and a start-up that genuinely hangs for longer is still reported.

---

**SHA-256 of `foxsdr-setup-0.99.46.exe`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.46-x86_64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.46-linux-x64.tar.gz`:**
`TBD`
