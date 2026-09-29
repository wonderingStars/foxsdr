**SDRplay: FoxSDR no longer gives up on the radio when the SDRplay service takes more than a second to apply a setting.** Changing frequency, antenna or LNA state could make the whole SDRplay API session end with "please restart the SDR API", because FoxSDR gave the service exactly one second to answer a control change before treating it as abandoned - the SDRplay API specification sets no such time limit. FoxSDR now waits up to 10 seconds and carries on. A new "Run SDRplay diagnostic" key (SYSTEM > Diagnostics, or `--sdrplay-probe` from the command line) drives the SDRplay API directly and writes a single file recording what the service actually does - every rate it offers, how it answers control changes and how long that takes - so a fault like this one can be diagnosed from real numbers next time, on hardware nobody here has on the desk.

**Raspberry Pi 64-bit downloads**, built for arm64 Linux by the same CI pipeline as every other release, hardware unconfirmed - no Raspberry Pi has run this build yet. Take it to find out what is broken, not to use as a receiver.

---

**SHA-256 of `foxsdr-setup-0.99.51.exe`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.51-x86_64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.51-linux-x64.tar.gz`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.51-aarch64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.51-linux-arm64.tar.gz`:**
`TBD`
