**Airband: type an airport, hear all of its control frequencies at once.** **VIEW > Airband** takes an airport's code - the ICAO code (KORD, EGLL), the three letters on a luggage tag (ORD, LHR) or, in the US, the FAA's own id - and puts its air traffic control frequencies into the frequency list as a group of their own, in AM at the right bandwidth, ticked. ATIS and the weather broadcasts are added unticked, because they never stop talking. **Nearest airports** lists the six closest to your receiver position. The table is built into FoxSDR, so a lookup needs no internet connection: the FAA's frequency file for the United States - every tower, ground, clearance, approach and departure sector, 29 frequencies for O'Hare - and OurAirports for the rest of the world. Europe's 8.33 kHz channel names are tuned to the frequency they really are.

**LISTEN** plays every ticked AM frequency at once, mixed into one speaker. Each channel has its own squelch, so you hear whoever is talking, and each is levelled by its own carrier, so a distant aircraft is as loud as the tower. When the ticked frequencies need more than your radio's band - an RTL-SDR hears about 2 MHz at a time - they are split into blocks, and the monitor scans between them, stops on a block as soon as anyone in it is talking, and moves on once it has been quiet for the hold time. The time each frequency has been heard is kept with it, and **Busiest first** sorts by it, so after an evening you know which of an airport's thirty frequencies are worth ticking. The monitor uses the receiver's radio, so LISTEN switches to the RECEIVER view.

**Ticked frequencies.** Every row of the frequency list now has a tick box, and the Scanner's new **Ticked frequencies** scans just the ticked rows, each with its own mode and bandwidth.

Requested by a tester near Chicago. Not yet tried on a real radio - reports welcome.

---

**SHA-256 of `foxsdr-setup-0.99.60.exe`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.60-x86_64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.60-linux-x64.tar.gz`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.60-aarch64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.60-linux-arm64.tar.gz`:**
`TBD`
