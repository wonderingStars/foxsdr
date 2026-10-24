# FoxSDR 0.99.88

**FoxSDR 0.99.88 installs an RTL-SDR's driver for you. Half of the people who install FoxSDR never get a radio working, and the commonest reason is an RTL-SDR dongle that Windows has given no driver, or its television driver, which hides it from every SDR program until Zadig is run. The RADIO SETUP page now has an INSTALL DRIVER key under such a dongle: press it, say yes to one Windows prompt, and the dongle is ready. This release also stops the usage report from ever sending a radio's serial number or a computer's network address, and lets you choose between stable releases and every release.**

The one-click install covers RTL-SDR dongles only; a HackRF, an Airspy, a Mirics or an RX888 is still bound with Zadig as before. Choosing "stable releases" changes nothing yet: until the team holds a release back, stable and every release are the same. The RSPduo crackle at 4 MS/s (GitHub issue 6) and the 0.99.81 plugins-reload freeze are unchanged. The Microsoft Store copy stays on its current version until the next submission is certified.

---

**SHA-256 of `foxsdr-setup-0.99.88.exe`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.88-x86_64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.88-linux-x64.tar.gz`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.88-aarch64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.88-linux-arm64.tar.gz`:**
`TBD`

## Features and fixes, 2026-10-24

**INSTALL DRIVER: an RTL-SDR ready in one click, without Zadig.** When RADIO SETUP finds an RTL-SDR on the television driver or on no driver at all, it now shows an INSTALL DRIVER key under that radio. Windows asks once for permission; FoxSDR then gives the dongle's radio interface Windows' own WinUSB driver - the Microsoft-signed one Windows already carries - and checks the radios again, and the dongle appears in the Source list. Nothing is downloaded, no certificate is added to the computer, and nothing else is touched: FoxSDR checks the device's own hardware ids first and changes only interface 0 of an RTL-SDR, never the whole dongle, never its remote-control interface and never any other device. Saying no to the prompt changes nothing. The Zadig instructions stay on the page for anyone who prefers them. Tested on an RTL2838 with no driver at all: two seconds from the key to a working radio streaming 2.4 MS/s. To undo it and use the dongle as a television receiver again, uninstall its *WinUsb Device* entry in Device Manager and plug it back in.

**The usage report never sends a serial number or a network address.** The optional usage report names the model of the radio you used, and has always promised to leave out its serial number. For a radio opened from an address or reopened at startup before the radio list was built, it could instead send the text the radio was opened with, which held the dongle's serial number or the address and name of the computer serving it (an rtl_tcp server or an ADALM-Pluto). FoxSDR now sends such a radio by its family name, such as "RTL-SDR" or "rtl_tcp server (network)", and both FoxSDR and the service that receives the reports now strip anything that looks like a serial number or an address, whatever version sent it. The privacy notice says so.

**Choose stable releases or every release.** The Updates section has two new keys under the startup check: Stable releases, the default, and Every release (beta). Stable is the release the FoxSDR team recommends to everyone; beta is every release the day it is published. The check still sends only the version you are running. A Microsoft Store copy keeps the Store's own updates and does not show the choice.
