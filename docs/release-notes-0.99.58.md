**The map's coverage line counts only what a receiver could really have heard.** A tester near Chicago read "72 of 72 bearings, best 17082 km" on the coverage line of their Radar Sweep and ADS-B maps. The distance was measured correctly from their receiver position, but every track on the map was counted, and only positions past half the earth's circumference were refused, so one wrong position set "best" for the rest of the session. Now a satellite never counts, because its position is predicted from its orbit whether or not anything was heard. An aircraft or a vessel counts up to 1,000 km, which is past any real line-of-sight or ducting reception. Everything else keeps the old limit, because an HF station on the far side of the world can be a genuine contact.

**Plugin updates in the catalogue (not part of the app).** ADS-B Decoder 1.8.1 no longer plots a phantom aircraft from one corrupted message: a new aircraft's first position that lies more than 2,000 km from every aircraft already on the map is held until a second position agrees with it. Radar Sweep 1.0.1 uses the same decoder for its reference aircraft and carries the same fix. Both arrive through the plugin catalogue's update check; they do not need this release.

---

**SHA-256 of `foxsdr-setup-0.99.58.exe`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.58-x86_64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.58-linux-x64.tar.gz`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.58-aarch64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.58-linux-arm64.tar.gz`:**
`TBD`
