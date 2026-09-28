**Beta testers link FoxSDR from the tester portal with one click.** The tester portal on foxsdr.com now has a "Link FoxSDR" button. It opens a `foxsdr://` link that Windows hands to FoxSDR - whether FoxSDR is already running or not - and FoxSDR asks the site whose link it is, then shows a prompt naming that tester: *"Link this FoxSDR to beta tester NAME?"*, with **Link** and **Not now**. Nothing is stored until you press Link, so a link opened by some other web page cannot quietly tie your copy to somebody else's tester entry. The SYSTEM > Beta tester section now only appears once a copy is linked (or holds an older tester code), and shows who it is linked to, with an **Unlink** button. Testers who pasted the older 32-character code in 0.99.44 do not need to do anything: FoxSDR exchanges it for the new kind on the next launch. A tester whose browser cannot open the link (Linux, or a blocked browser prompt) can run FoxSDR with the hidden `--link-tester` switch and paste the code the portal shows; it goes through the same prompt. Nobody else sees any of this, and nothing is collected for anyone without a tester link or code - see PRIVACY.md for exactly what a tester's report contains.

**The usage report is sent once, not once per launch.** FoxSDR's anonymous usage report describes the previous session and is sent when the next one starts. A launch that closed before its first settings save, or several copies started at the same moment, all sent that same report again - one installation's report arrived fifteen times in one second, and in the last month such duplicates made total running time on our usage dashboard read 19% high. Each report now leaves an empty marker file beside `config.json` and only the copy that creates it sends the report. Nothing about what the report contains has changed; PRIVACY.md names the marker file.

---

**SHA-256 of `foxsdr-setup-0.99.45.exe`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.45-x86_64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.45-linux-x64.tar.gz`:**
`TBD`
