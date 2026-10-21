**SDRplay: when the SDRplay API Service is stopped or stuck, FoxSDR now says so and can restart it for you (Windows).** An RSP2 Pro owner on 0.99.52 was told "Check that the SDRplay API service is running" on seven launches in a row - the SDRplay API refused to open in 0 ms every time - and never found out which service, or what state it was in. FoxSDR now asks Windows what the **SDRplay API Service** is doing (no administrator rights needed) and says it in the Source panel: stopped, disabled, still starting, running but not answering, stuck or paused, or not installed. Alongside it is a new **RESTART SDRPLAY SERVICE** key. It shows the normal Windows administrator prompt and restarts the service - `net stop`, ending `sdrplay_apiService.exe` if the stop hangs, then `net start` - and reports how that went; saying no at the prompt changes nothing. If FoxSDR had never reached the SDRplay API this session (the case in the report), the radios are listed again and your saved radio reopens straight away with no FoxSDR restart. If the SDRplay session had already been lost earlier, FoxSDR still asks you to restart it afterwards: a thread may still be stuck inside SDRplay's library from that session, and a restarted service does not release it. A disabled service must be set back to **Automatic** in Windows Services first. The diagnostics bundle (`sdrplay-service:`) and the SDRplay diagnostic report now carry the service's state too.

**Untested on real SDRplay hardware.** No RSP and no SDRplay API are on the machine this was built on. The service name `SDRplayAPIService`, its display name "SDRplay API Service" and its program file `sdrplay_apiService.exe` come from third-party service listings, not from SDRplay's own documentation - so if Windows does not know that name, FoxSDR looks the service up by its display name and then by its program file, and uses whichever name Windows actually has. The Windows query itself was checked against services every Windows machine has; the restart's command line and every decision around it are covered by tests; the elevated restart has not been run against a real SDRplay service.

---

**SHA-256 of `foxsdr-setup-0.99.55.exe`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.55-x86_64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.55-linux-x64.tar.gz`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.55-aarch64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.55-linux-arm64.tar.gz`:**
`TBD`
