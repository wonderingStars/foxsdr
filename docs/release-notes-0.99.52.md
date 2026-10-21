**Beta testers on Windows can link FoxSDR from their tester page again.** Pressing **Link FoxSDR** on your foxsdr.com tester page opens FoxSDR, which should then ask "Link this FoxSDR to beta tester ...?". On Windows that question never appeared: Windows hands FoxSDR the link as `foxsdr://beta/?t=...`, with a `/` after `beta` that the page's own link does not have, and FoxSDR only accepted the spelling without it - so it quietly dropped every link clicked on Windows, in every release since 0.99.45. FoxSDR now accepts both spellings of that one link, and still refuses any other. If you tried to link before and nothing happened, press **Link FoxSDR** on your tester page once more.

---

**SHA-256 of `foxsdr-setup-0.99.52.exe`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.52-x86_64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.52-linux-x64.tar.gz`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.52-aarch64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.52-linux-arm64.tar.gz`:**
`TBD`
