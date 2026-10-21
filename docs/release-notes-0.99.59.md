**Do you own a Red Pitaya? Please email us.** FoxSDR has no Red Pitaya driver of its own and has never been tested with a Red Pitaya board: it can only reach one through the SoapySDR "redpitaya" module. If you have one, please email support@foxsdr.com with your board's model and what happened when you picked it in FoxSDR. What you tell us is what proper support will be built from.

**Picking a radio that does not answer no longer freezes the window.** The radio list could show a "redpitaya" entry on computers with no Red Pitaya at all, because the module that reaches those boards listed its built-in default address without checking that anything was there. Picking it froze FoxSDR for several seconds while it tried to connect. That phantom entry is no longer listed. Starting any SoapySDR radio whose driver is slow to answer now gives the window back within about a second and a half, and FoxSDR gives up on a radio that still has not answered after 20 seconds. This was checked against a stand-in for the driver in FoxSDR's tests, not against a real Red Pitaya.

**A faulty ASIO sound driver no longer makes the radio scan fail.** On some computers a sound card's ASIO driver crashed just as FoxSDR's radio scan was finishing. The scan's answer was already complete, but FoxSDR threw it away, scanned again one driver at a time and filed a crash report. The scan no longer asks SoapySDR about sound cards at all (FoxSDR has had its own sound-card source since 0.99.38), it ends without running other drivers' clean-up code, and an answer that arrived complete is used even if the scan dies afterwards.

**SDRplay radios through SoapySDR no longer freeze the window after the SDRplay service is restarted.** If the SDRplay API Service was restarted while an SDRplay radio was running through SoapySDR, the next retune or start could freeze FoxSDR for several seconds until the driver answered. FoxSDR now waits at most a second and a half, carries on, and applies the driver's answer when it comes; a driver that still has not answered after 20 seconds is given up. The radio scan also closes its connection to the SDRplay service properly when it finishes, as SDRplay's documentation asks. These changes were checked against a stand-in for the SDRplay driver in FoxSDR's tests, not against real SDRplay hardware.

**Privacy: uploaded reports no longer carry network addresses, computer names, serial-port numbers or sound-card names.** Crash and freeze reports, bug reports with the log attached, and the diagnostics bundle all carry the end of FoxSDR's log. That log is now scrubbed of network addresses and computer and host names, which become `<host>` (the port number after them is kept), and of serial-port numbers: `COM5` is sent as `COM#` and `/dev/ttyUSB0` as `/dev/ttyUSB#`. FoxSDR also no longer writes a sound card's name in its log at all, because that name is often one a person chose. PRIVACY.md and foxsdr.com/privacy.html give the exact rules.

---

**SHA-256 of `foxsdr-setup-0.99.59.exe`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.59-x86_64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.59-linux-x64.tar.gz`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.59-aarch64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.59-linux-arm64.tar.gz`:**
`TBD`
