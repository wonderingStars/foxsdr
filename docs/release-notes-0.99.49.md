**The patch page can now save and load whole patches.** A new PRESETS key lets you save the patch you have built under a name, load a saved one back, rename it or delete it. Loading a preset stops whatever patch is currently running first, so you are never left with two patches fighting over the same radios. A "(previous patch)" slot is kept automatically and always holds whatever was running immediately before your last load, so switching between two setups - or just trying a preset and changing your mind - is one click either way. Presets are saved in your own configuration file and never leave your machine.

**Early, untested support for AOR's AR5700D and IQ5001-equipped digital-I/Q receivers**, as a native driver written from AOR's own developer documentation with AOR's cooperation. It has **not yet been tried against a real receiver** - this release is asking AOR owners to be the first. On Windows it needs the WinUSB driver bound to the receiver's I/Q interface with Zadig; on Linux it uses the kernel's own usbfs. The receiver also needs AOR's own FX2 firmware file, which AOR will supply separately and is **not included with FoxSDR yet**: without it, FoxSDR says so plainly - "The AOR I/Q interface needs AOR's FX2 firmware, and the firmware file is not installed" - and will not open the receiver, rather than failing silently or guessing. Once the firmware file is in place, FoxSDR loads it into the interface itself on first use. If you own one of these receivers, please try it and report back what you find, working or not.

---

**SHA-256 of `foxsdr-setup-0.99.49.exe`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.49-x86_64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.49-linux-x64.tar.gz`:**
`TBD`
