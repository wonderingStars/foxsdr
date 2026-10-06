**The Airband monitor takes a list you type in yourself, and plays NFM as well as AM.** Under the airport box, enter a frequency in MHz, choose AM or NFM, optionally name it, and press Add: it joins the frequency list, ticked, and LISTEN plays it with everything else you have ticked. NFM rows - marine, business and public-service channels - play alongside airband at a level that matches the AM ones. A frequency outside what your radio covers is refused, with the range it does cover. WFM is not offered: broadcast FM is too wide for the monitor.

**Presets: a named list you can fill by hand, from a file, and export.** A preset is a named group of your frequency list. Type its name in the new Preset box, add frequencies to it, or type or paste the path of a CSV or an SDR# `frequencies.xml` and press Import to read the file into it, its AM and NFM rows ticked. Export CSV writes the preset to your recordings folder as a file that Import reads back, ticks included, and Remove preset takes its rows out of the list.

**While the monitor is listening, the spectrum shows what you are hearing.** Each channel the monitor is playing is shaded at its own frequency - brighter, with a line down the middle, while someone is talking - and the tuned-frequency band, its line down the waterfall and the "peak in passband" figure are hidden until you press STOP, because they showed a frequency that was not the one you were hearing.

**A native driver for the HydraSDR RFOne.** The RFOne speaks Airspy's USB protocol with its own ids and a few constants, so it is driven by the Airspy driver given its profile: the device scan, the Source list, the patch page, the web control and the Linux udev rule all know it. Not yet tested on hardware - nobody on the project has one; if you do, please report what you see.

**A crash report from the sentinel now says where a fast-fail was.** On Windows, when FoxSDR dies by a fast-fail - which cannot be caught inside the process - the sentinel reads Windows' own record of the crash and adds the faulting module and offset, so the next such report names the DLL and, when it is FoxSDR's own, the function.

Still not fixed, and known: a recording whose disk disappears part way ends as an empty file with nothing on screen saying so; an MP3 patch speaker whose folder is removed says nothing; a log file held open by another program at start-up is silently switched off.

---

**SHA-256 of `foxsdr-setup-0.99.66.exe`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.66-x86_64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.66-linux-x64.tar.gz`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.66-aarch64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.66-linux-arm64.tar.gz`:**
`TBD`
