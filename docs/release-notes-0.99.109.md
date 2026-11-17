# FoxSDR 0.99.109

This text was checked sentence by sentence against the program's code and build records.
Where a statement could only be tested with test signals or a stand-in program, and not with real equipment, the sentence says so.

[WINDOW-SIZE: to be filled by the release lane]

---

## If you use the web remote: read this first

**What was wrong.** In FoxSDR 0.99.104 a web page on another site could drive FoxSDR when the web remote was switched on and no web password was set.
A hostile page you happened to visit could make your own browser send commands to FoxSDR's built-in web page.
Those commands included starting recordings and installing or removing plugins.
The same page could also read what you were listening to.
This was found by reading the code of 0.99.104.

**What to do if you use 0.99.104 or an older version.**
- Update to 0.99.109, or switch the web remote off.
- If you cannot update yet, set a web password. With a password set, this attack did not work against 0.99.104. That too is from reading its code.
- The web remote is off unless you switched it on. It listens on this computer only unless you changed that.
- Versions older than 0.99.104 and the test builds 0.99.105 to 0.99.108 were not checked one by one. Treat them in the same way.

**What 0.99.109 does.**
- A web page with no password is view-only. It shows status, spectrum and audio. Tuning, start, stop, recording, plugin install or removal and bookmark changes are refused until you set a password.
- The web page answers only to this computer's own addresses (any IP address, `localhost`, the computer's name and `name.local`) and to the host names on your "Allowed web origins" list.
- A request that carries another web site's address is refused, even a request that only reads. A site you have put on "Allowed web origins" may read but never change anything.

**If you reach the web page by another name, such as a dynamic-DNS name.** Add that address to "Allowed web origins" or the page refuses you.
- In the new window, open SYSTEM, then the Web section, then the field "Allowed web origins".
- Type the address as your browser shows it, with `http://` or `https://` in front and the port if there is one. Separate several entries with a semicolon.
- The Legacy window has no field for it. Use the new window to set it.

Other security fixes in this release:
- A listener sign-in can no longer help someone guess the web password. The two have separate counters.
- Text that arrives from the air (RDS station name and radio text) or from the plugin catalogue is shown as plain text in the web page.
- A web page cannot open the TCI port. A web request cannot retune a radio through the rtl_tcp port.
- A settings file you import cannot redirect where FoxSDR connects or where its plugins come from.
- A request with two `Host`, `Authorization` or `Origin` headers is refused by the program interface.
- A client that takes more than 20 seconds to deliver one web request is disconnected. More than 16 connections from one other computer that have not yet sent a request are refused. A normal browser is not affected.
- A damaged state file of FoxSDR's own no longer stops FoxSDR from starting.

---

## What this version sends about your radios

FoxSDR 0.99.109 sends a report when a radio fails to open or stops.
This is a change from earlier versions.
The wording below is the approved privacy text, copied word for word.

> FoxSDR sends usage counts unless you switch them off, and sends one kind of
> report - a radio that did not open - whatever you have switched on or off. This
> document lists exactly what it sends.

The section "Radio failure reports and the radio journal (since 0.99.109)":

> **When a radio does not open, FoxSDR sends ONE report about that failure, every
> time it happens - whether or not Diagnostics is on, and whether or not Usage
> reporting is on.** It is the one report that the Diagnostics switch does not
> govern: a radio that will not open is the commonest way FoxSDR does not work,
> and nobody can fix what they are not told about. It does not wait for the
> next start; it is written to your computer first, so a crash cannot lose it,
> and sent as soon as it can be.
>
> **What such a report contains:** the radio's driver and model; **its serial
> number** (see below); the radio's firmware and library version where FoxSDR
> knows them; the reason the driver gave, with serial numbers, account names,
> computer names, addresses and frequencies taken out and every file path replaced
> by `<path>`; a class word for the reason (`busy`, `bind`, `absent`, `timeout`,
> ...); which part of FoxSDR tried to open it (the start-up restore, a radio you
> picked, a retry of the saved radio, a patch-page radio, the automatic reopen
> after a driver fault); how many times that radio has failed in this run; how
> long the open took and the sample rate asked for; whether you had already moved
> on to another source; how many reports for this radio were not sent because of
> the daily limit; whether Diagnostics was on; up to 40 radio-related lines of the
> log, scrubbed the same way; the FoxSDR version, install channel and update
> channel; the operating system and build and the processor architecture; and the
> seconds since FoxSDR started.
>
> **With Usage reporting off** the report carries **no install identifier, no
> session identifier and no sequence number**, only a random number made for that
> one message, so two such reports cannot be tied to each other except by the
> radio's own model and serial number. (The combination of FoxSDR version,
> operating-system build, architecture and channel can identify a machine in
> practice; nothing uses it to link reports, and the server does not group by it.)
> As with every report, the network necessarily shows your address to the server;
> the server does not read, log or store it.
>
> **With Usage reporting on** (the default) FoxSDR also keeps a **radio journal**:
> every event in the life of every radio - a radio FoxSDR saw (including one it
> cannot open because no usable driver is bound to it), each open and what it
> asked for, the settings applied (sample rate, bandwidth, gain mode and value,
> frequency correction in ppm, bias tee, antenna port name, direct sampling,
> decimation, and **the band the radio was in by name, such as "fm-broadcast" -
> never the frequency**), once a minute how the radio's stream is doing (samples
> per second achieved, samples dropped, buffer fill, USB errors), each recovery,
> and how and why the radio closed - and, once a session, a description of the
> machine around it: processor model (without its clock speed), thread count,
> memory, graphics adapter and driver version, the USB host controllers' product
> names, the audio system in use, how many displays and each one's scaling. The
> journal and that description carry your install identifier. The first launch
> of an install also sends a **first-session record** (the same anonymous counts
> as the usage report, about a minute in and again when something fails) so that
> people who run FoxSDR once are counted.
>
> Turning **Usage reporting** off stops the journal, the machine description, the
> first-session record, the still-running beat and the usage report, deletes the
> install identifier, and deletes any queued journal that carried it. It does
> **not** stop the radio failure report.
>
> **Never in any of it:** a tuned or centre frequency, a bookmark, the name of a
> file or recording, anything decoded, a position, an IP address or host name, a
> user or computer name, a file path. The server refuses a message that holds one,
> and tests search every queued message for each of them.
>
> **The serial number is collected.** A failure report carries the radio's serial
> number, as its own field. It lets two reports, from two computers or two days,
> be recognised as the same physical radio, so a faulty unit can be told from a
> fault in FoxSDR or in a driver, and a radio can be found again when somebody
> quotes its serial. It identifies a radio, not you; but with Usage reporting on
> it is held with your install identifier, so it should be treated as personal
> data. Serial numbers are still removed from everything else FoxSDR sends (the
> usage report, the crash and freeze reports, the diagnostics bundle).
>
> **How long it is kept:** radio failure reports and the radio journal are **kept
> indefinitely** (crash reports are kept 90 days). **To have them deleted:** write
> to the contact address on the website quoting your install identifier (Settings
> shows it while Usage reporting is on) - every message from that install, and the
> radios only that install reported, are erased. A failure report made with Usage
> reporting off has no install identifier, so it cannot be found by one; it is
> erased by quoting the radio's serial number.
>
> **If a daily limit stops a report,** the server is told: at most 100 failure
> reports and 3,000 journal events are sent per install per day, the first of each
> kind of failure of each radio is always sent, the newest later one is kept and
> sent at the day's end, and the number not sent is stated in it.
>
> **Where the data goes:** `https://telemetry.foxsdr.com` (the Worker in
> `telemetry-worker/`), which forwards these messages to the FoxSDR site. Each
> message is first written to the `radio-reports` folder beside `config.json` and
> then sent; one that cannot be sent is kept there and tried again for up to 14
> days, then deleted.

Two things to know that the privacy text does not say in so many words:
- When a report is waiting and the report service cannot be reached, closing FoxSDR can take up to about 1.5 seconds longer. The waiting report is kept and tried again.
- To have your records erased, write to support@foxsdr.com and quote your install identifier or a radio's serial number. The Usage reporting page shows both.

---

## Things to know before you install

**The installer is not signed.**
- Windows shows its "Windows protected your PC" warning when you run it.
- Check the download before you run it. Each release file has a SHA-256 checksum on the release page. In PowerShell, `Get-FileHash <file>` prints the checksum of your copy. The two must match.

**The translations are machine-made.**
- All 33 languages, including every new line in this release, were translated by a program. No speaker has read them.
- Some words may read oddly. The privacy text in other languages is a machine translation of the English.
- Corrections are welcome. Use the "Report a bug / dislike" key in the window to send one through the in-app issue report.

**The website copy downloads no companion program in this version.**
- The Companion applications shelf in the plugin store is visible.
- Its cards read NOT AVAILABLE, with the sentence "The program for this companion is not available in this copy of FoxSDR."
- Companion downloads come in a later version.
- Nothing that 0.99.104 downloaded is lost by this.
- Because no companion program is available, GET EVERYTHING in the plugin store fetches none.

**Some controls are hidden because they are not proved.**
The controls below are not drawn in this version.
Each did nothing useful, showed nothing, or was shown by measurement not to do what its name says.
- RTL-SDR "Analogue IF bandwidth". A real dongle did not narrow its passband to the width typed. The automatic setting is unchanged.
- "Automatic ppm" (with "Measure the crystal error" and "Use this correction"). It follows the broadcast station's pilot, not the radio, so applying it could mistune you. The manual ppm setting is unchanged.
- The CAT "Reachable from other machines" tick, in both windows. CAT is available on this computer only. A saved "reachable" value stays in the settings file but starts nothing beyond this computer.
- The "Bench Classic XL" theme. A saved choice shows as "Today's bench".
- The Map switches "Weather radar", "Receiver map" and "Repeaters". Each returns when it has a data source.
- The "Generator" and "Scope" parts in the PATCH page's parts bin. A patch that already holds them keeps them.
- The header chip on the TOOLS, EXTEND and SYSTEM banks, which always read "--".
- The "Community" row of the SYSTEM list.
- Views that need data nothing supplies yet are not drawn: EVM, SNR, symbol rate, lock and colour-by-symbol on constellation views; the survey views' occupancy and floor readouts; the radiosonde launch point, landing point and scrubber; the propagation city chips; the FT8 reporter count; the GOES band, segments and satellite list; and the satellites' orbit path, scrubber, pass timeline and Doppler read-out. They appear when a plugin supplies the data.
- Wording that described a working feature as missing was corrected.

**The Airspy HF+ opening fault is fixed, but not yet confirmed on a real HF+.**
- On Windows an Airspy HF+ could fail to open. The radio refused two writes of its stored calibration, and FoxSDR treated that as the radio being dead.
- FoxSDR now logs the refusal and carries on.
- This was proved against a stand-in for the radio. No Airspy HF+ has been used to test it.

**Not yet proved on a real radio.**
This sentence appears beside each of these controls: SoapySDR bandwidth and driver settings; rig following (serial CAT, virtual COM and OmniRig); SDRplay low-IF (off by default); and the Fobos SDR bridge, which needs the maker's own library that you install.
ELAD radios are reachable only through a SoapySDR module that you install, and have not been tried either.

**Not tried with real equipment.**
These were tested with stand-ins or test signals only:
- the pass-band edges and shift (a tone outside the edges falls by about 37 dB with test signals);
- touch on the spectrum, on the waterfall and on the web page (tested with emulated touch, not a real touch screen or phone);
- HTTPS for the web page and the shared receiver for several listeners (no real browser, no real listeners, no certificate from the real Windows certificate store);
- ShuttleXpress and PowerMate knobs;
- the rotator;
- desktop notifications (the real Windows notification has not been seen);
- MQTT, webhooks, InfluxDB, the rtl_tcp server, TCP audio, the program interface and the MCP server (tested against loopback stand-ins, with no real broker, database, web hook or client program);
- an audio recording that begins with the replay buffer (the buffer's size reaches the radio, but the recording itself has not been tested);
- the typed-number rule with a comma regional format (tested with stand-in symbols, not on a PC set to a comma format);
- the installer's two Start-menu entries (the installer script could not be compiled where this was built).

**The in-app help.** Help topics (28, written inside the program) were each checked against the code when written. Nobody else has read them yet.

---

## What changed in behaviour (things you could trip on)

1. **TCI from another computer needs that computer's address.**
   If you tick "Reachable from other computers" for TCI, a field "Computers allowed to connect for TCI" appears.
   Type the address of each computer, for example 192.168.1.20, separated by commas. A network of 8 to 32 bits is also accepted. At most 16 entries.
   Until the field holds one, the TCI interface does not start beyond this computer and says so.
   Connections from other addresses are closed at once. Programs on this computer are not affected.
   If you ticked "Reachable from other computers" in an earlier version, type the list once.
2. **The same rule applies to TCP audio and the rtl_tcp server.**
   Tick "Let other computers connect" and fill in "Computers allowed to connect for the audio" or "Computers allowed to connect for rtl_tcp".
   An rtl_tcp connection that does not begin with a real rtl_tcp command is dropped.
3. **Audio over UDP goes to one computer.** A broadcast, multicast or 0.x.x.x address is refused with a sentence.
4. **The program interface (WebSocket and server-sent events) refuses a request whose Host is not this computer's own or a name on "Allowed web origins".**
   A program that connects to `127.0.0.1` or `localhost` is not affected.
   Changing or clearing the web password ends its open connections at once.
   At most four unfinished requests are kept per address.
5. **Importing a settings file is stricter.**
   An imported file cannot change where plugins come from, what FoxSDR listens on, the Pluto address, the rtl_tcp address or the I/Q file opened at start.
   Saved radios and arguments that name another computer, an address, a file or a path are left out, and the import notes say so in words.
   Your USB radios, sound card and source choice come back whole, including IC-R8600, FUNcube, G39DDC, G33DDC and SDR-IQ radios.
   An import can switch Usage reporting and Diagnostics off but never on.
   This computer keeps its own values of everything left out.
6. **A saved password or token that Windows cannot open on this PC is never erased by a save.**
   The web server then refuses to start, with "the saved web password could not be opened on this PC ... set the password again before turning web access on".
   This happens if the profile was copied to another PC or another Windows user.
7. **Passwords and tokens moved into the Windows per-user store.**
   The settings file no longer holds them in plain text (16 kinds: the web, listener and API passwords, the MQTT password, the InfluxDB token, the webhook address, the MCP token and others).
   Backups next to the settings file (dated `.bak`, "before import", "before reset", hand-made `.old`) are cleaned of them.
   Copies made outside FoxSDR's settings folder, such as OneDrive or File History versions, cannot be cleaned by the program.
   On Linux there is no per-user store. The values are kept in a file that only you can read, and they are not encrypted. That Linux code has not been tested on Linux.
8. **Typed numbers with a comma.**
   A single comma followed by exactly three digits with no dot, for example `14,200`, is ambiguous. It is refused with a sentence asking you to type it with a dot.
   If your Windows format uses a comma as the decimal sign, it is read as a decimal instead.
   `14,2`, `14.200` and `145,5` read as you would expect.
9. **A failed radio open is always reported**, even with Usage reporting off (see "What this version sends about your radios").
10. **The Usage page line for a switched-off install changed.** "Off. Nothing is transmitted." was not true, because a radio that does not open is still reported. The page now carries the approved sentence.
11. **Radio failure reports are scrubbed harder.** Host names, MAC addresses, Linux and device paths, file names, other devices' serial numbers and bare tuning numbers are replaced by `<host>`, `<mac>`, `<path>`, `<file>`, `<id>` and `#`.
    A bare host name with no dot and no "host" cue word in front of it cannot be told from an ordinary word and is not masked.
12. **The RDS log CSV** starts a text with `'` if the text begins with `=`, `+`, `-` or `@`.
13. **The Outputs page** no longer calls a host name that merely starts with `127.` "this computer only".
14. **A damaged `swl-log.json`** no longer stops FoxSDR starting. It is kept as `swl-log.json.damaged`.
15. **An HTTPS web page drops a client** that has not finished its first request within 15 seconds.
    The plain web page drops a client that takes more than 20 seconds to deliver one request.
16. **A hand-over between windows** is refused with a sentence if the other program is missing or the settings are still in use.
17. **TCI "reachable from other computers" has no password** (TCI programs send none). The page warns about it.
    A clear-text MQTT password or InfluxDB token sent to another computer is sent without encryption. The page says so.
18. **Some limits you might expect do not exist yet.**
    The pass-band edges and the AM detector choice are not saved with a radio. They reset at each start.
    The colour map you pick is not remembered between runs.
    The autosave and log folders and intervals are not remembered. Autosave and the log are off at every start.
    The clip files from a squelch-triggered recording are named `radioN_clip_...` and do not follow your file-name template.
    The number format reaches the SIGNAL page and the number fields but not every tooltip, map and 3D text.
    The clipboard copy of a frequency always uses a dot.

---

## What is new

### Listening and filters (SIGNAL page)

- **Pass-band edges and shift.** Set the low cut and the high cut separately, in hertz (USB, LSB, CW, AM), and move the whole pass band with an IF shift.
- **Filter width and shape.** Width presets for each mode (AM, FM, wide FM, and 3, 4 and 6 kHz for SSB and CW) and a choice of filter shape.
- **CW pitch and a CW peak filter**, with a CW auto-tune switch.
- **Several notch filters** (ten, each with its own frequency and width), an **audio equaliser** (eight bands, with a key to flatten it), an **audio high-pass and low-pass**, and a choice of **noise-reduction method**.
- **Squelch and tone squelch.** A noise squelch (threshold and tail) and CTCSS and DCS tone squelch, with previous and next tone keys.
- **AM detector.** Choose synchronous AM and which sideband it follows.
- **Automatic frequency tracking** (AFC), **I/Q correction** (balance, DC removal, swap), **stereo placement** and pseudo-stereo, and a **multiplex (MPX) recording** of an FM broadcast.
- **Signal strength in S-units or dBm.**
- **Typed numbers follow your Windows regional settings** on the SIGNAL page readouts and in the number fields of the SYSTEM page.

### Spectrum and tuning

- **SYSTEM, Spectrum tools:** the FFT size (1024 to 131072), the FFT window, the frame rate, an **IF zoom** drawn as a picture, a **waterfall autosave** (a picture written to a folder you choose every 1, 5, 15, 30 or 60 minutes) and a **power and SNR log** (a CSV file).
- **Six more colour maps:** Viridis, Magma, Plasma, Inferno, Cividis, Turbo.
- **Touch:** a finger drag over the spectrum pans it, and a two-finger pinch on the waterfall zooms.
- **SYSTEM, Tuning and memories:** centre tuning, **band stacking** (twenty band keys, three remembered places per band), and **VFO A and B** (use A, use B, swap, copy).
- **Waterfall:** a visible break where reception had a gap, a note saying how many top lines a colour range applies to, and a "RANGE CHANGED" rail.
- **Airband marks** on the spectrum and waterfall, also on every pane of a multi-radio layout.
- **Maps:** the title rail's system menu works, a trail-width slider, a "Stop following?" question when you drag a map that is following something, the classic window's eleven aircraft pictures, and a clearer note when a map is empty and why. Satellite pages gain a NIGHT SIDE switch and the receiver-position fields.
- **Looks:** three more themes (Glass Cockpit, Daylight Lab, Field Radio), "Enlarge figures" and "Enlarge every reading", Ctrl+= and Ctrl+- to step the interface size from 100 to 250 percent, a band-plan colour palette and size, and a choice of meter style for the sample-rate and volume meters.
- **In-app help:** Help topics in the application menu, or Ctrl+F1.

### Recording and playback

- **File-name templates** for recordings, checked as you type.
- **Roll-over:** start a new file after a size or a time you set.
- **Sample format choice:** I/Q as 32-bit float or 16-bit, audio as 16-bit, 24-bit or 32-bit float, with the cost per second shown.
- **A clip for each transmission:** a recording that opens when the squelch opens and closes after a hang time, one file per transmission.
- **An audio replay buffer** (0 to 120 seconds) so a recording can start with what was said just before you pressed record.
- **Scheduled recording:** a panel to set start times (local time), with run now, skip, switch off, switch on and remove, and a warning for missed starts. Nothing is added to Windows (no task, service or wake timer).
- **IF recording** (the pass band) with its own start and stop.
- **Playback of raw I/Q files** (cu8, cs16, cf32 and 24-bit, with the format and rate you state), of **SigMF** recordings, and of WAV files that carry the centre frequency in SDRuno's header.
- **A seek bar and an A-B loop** for recorded files, and a **whole-file waterfall** overview of a long recording (a 2 GB file in about half a second on the test PC).
- **The key reads "Saving IQ..." or "Saving audio..."** and stays dead until the file is closed.

### Radios

- **The first-run check** runs on its own once when the new window starts. It has not been tried against a real USB bus. It names HydraSDR, AOR, Icom IC-R8600 and Mirics radios that Windows can see but has no driver for, and says what to install.
- **RTL-SDR:** direct sampling (automatic, never, I branch, Q branch).
- **Raw I/Q over UDP and TCP** as a source, with the rate and centre frequency you type.
- **Tuner pooling:** claim a second radio's tuner for a frequency, mode and bandwidth, and release it.
- **Radios and servers that need a library you install yourself** say what to install and from where. FoxSDR downloads nothing for them. SoapySDR modules, including SoapyELAD, are listed and opened when installed.
- **Rig following (read only).** FoxSDR can follow the frequency and mode of a rig over a serial or virtual COM port, emulate a rig for a logging program, or read OmniRig.
  It only asks the rig for its frequency and mode. It never sends a frequency or mode to the rig. A logging program's transmit commands are refused.
  All three are off until you switch them on.

### Scanner

- The scanner's **Priority MHz** field now reaches the scanner. It was stored and never used.
- **Lockouts work in range and plan scans**, with the count and the time left on timed ones, and a typed list of locked frequencies. Per-memory thresholds are sent to the scanner.
- **Automatic bookmarks:** when a scan stops on a signal, a bookmark named "Scan hit <MHz>" is added. This is off until you switch it on.

### Outputs and alerts (SYSTEM, Outputs)

Every output is off by default. When you switch one on, it listens or sends on this computer only unless you say otherwise.

- **MQTT** publishing of decoded lines, **webhooks** (HTTPS to any host; plain http only to this computer), an **InfluxDB** level writer, and **keyword alerts** with a repeat gap.
- **TCI** and **UDP audio** switches, **TCP audio**, an **rtl_tcp server**, a program interface over **WebSocket and server-sent events**, and an **MCP server**. Beyond this computer each needs a typed list of allowed computers (see "What changed in behaviour").
- **A rotator:** turn, stop and park by hand (GS-232 or rotctld). It does not follow aircraft or satellites.
- **An SWL log** (ADIF export), a **macro recorder** and macro page, and a **Receiverbook listing preview** (a page showing exactly what would be published; nothing is sent to Receiverbook).
- **Desktop notifications** for alerts ("Show each alert as a desktop notification").
- **An RDS log** to a CSV file (station name, programme type, radio text; never the frequency or the PI code).
- **The Windows-style "Number format"** setting.

### Plugins and companions

- **A Companion applications shelf** in the plugin store (see "Things to know before you install").
  Each entry shows what it decodes, the project it comes from, its licence, why it is separate, and its source address before anything is fetched.
  A program runs only if its bytes still match the checksum in the catalogue, and is checked again every time it is about to start.
- **Plugin pages** show the catalogue's pictures (a strip with arrows), "what it does", "what is new", the capabilities a module declares, and the legal notice before you can install.
  The Fitted modules page can **clean up old versions**. It asks first.
- **Start-up notices:** a plugin that failed ("Started without a plugin", with a RE-ENABLE key), a team-message card with a Seen key, a "FoxSDR did not close normally last time" sheet, and a "Link this FoxSDR to tester ...?" question.
- **The PATCH page's Spectrum and Map parts now draw** (the trace and waterfall, and the map with its targets). A running patch is named on the SIGNAL page.

### The web page

- **Touch.** The page's dividers and column grip work with a finger, a tap on the upper or lower half of a frequency digit tunes that digit, and touch targets are larger on a phone.
- **HTTPS for the web page** ("Use HTTPS for the web page" in the Web section), using Windows' own secure layer. Use a certificate of your own by its 40-digit thumbprint, or the program's own. The page shows the fingerprint and the date it is valid until.
- **A shared receiver for several listeners.** Everyone who signs in with the listener password hears the same tuned receiver. The owner's account tunes. A switch lets listeners tune as well. You set the user limit (1 to 32) and the idle and session limits.

### Also

- **A DSP speed test** (SYSTEM, Performance), **HID knob support** (ShuttleXpress and PowerMate, under SYSTEM, Accessibility), and a **tester Preview box** that shows what a tester report would contain. Nothing is sent.
- **Legacy is a choice inside the new window** (Layouts menu, "Legacy"). The Legacy window has "Switch to the new window" in the SYSTEM bank.
  Windows are switched cleanly: settings are released first, and if a recording or a patch is running you are asked first.
  The main Start-menu entry opens the window you last chose. "FoxSDR (Legacy)" always opens Legacy.
- **The SYSTEM page's Usage reporting** shows your install identifier and the serial number of each radio, and says how to ask for your records to be erased.

---

## Linux

0.99.109 for Linux is a tarball. There is no in-app updater and no update installer on Linux: update from the tarball.
There is no AppImage in this release.

- **The new window runs on Linux (x86-64).** The tarball has a launcher, `foxsdr`. It opens the new window, or the Legacy window with `--legacy` or `FOXSDR_WINDOW=legacy`. It also opens Legacy when there is no display or a library is missing, and says which.
- **Needs glibc 2.35 or newer** (Ubuntu 22.04 or newer).
- **Packages you need** (Ubuntu and Debian names):
  - for every program: `libasound2`, `libsoapysdr0.8`, `libssl3`, `libunwind8`, `libopengl0`, `libstdc++6`, `libgcc-s1`;
  - for the new window: `libxkbcommon-x11-0` (under an X11 session), `libxkbcommon0`, `libinput10`, `libudev1`, `libgbm1`, `libfontconfig1`, `libfreetype6`, `libdrm2`, the X11 client libraries (`libx11-6`, `libxcursor1`, `libxi6`, `libxrandr2`, `libxcb1`) or Wayland (`libwayland-client0`), and working OpenGL (Mesa: `libgl1-mesa-dri`, `libegl1`) or `SLINT_BACKEND=winit-software`;
  - for opening links and folders: `xdg-utils`;
  - for the clipboard: one of `wl-clipboard` (Wayland), `xclip` or `xsel` (X11);
  - for the access test tone: a sound server or an ALSA `default` device;
  - for Japanese, Korean and Chinese: `fonts-noto-cjk`. The window refuses a language it cannot draw.
  - Whether a standard desktop already has all of these has not been checked.
- **Things that now work on Linux:** opening a link or a folder, copying to and pasting from the clipboard, dragging a file onto the window, the access test tone, and the Performance page's memory and CPU figures.
- **What a Linux user does not get:**
  - The spoken readout is silent (there is no speech engine).
  - The title rail's system menu, the SDRplay service restart and the GPU figures on the Performance page do not exist on Linux. The pointer ledger line is empty.
  - Companion programs are for Windows only.
- **Not tried on a real desktop.** The window was tested on a virtual display only, never on a real Linux desktop (X11 or Wayland), and not with a real mouse or keyboard.
  Whether Wayland needs `libxkbcommon-x11-0` is not known.

---

## Also not in this release

- Vendor-DLL radio bridges (ExtIO and similar), Windows notifications and the Windows secure store for passwords are Windows only.
- LimeSDR, USRP and BladeRF are reached through SoapySDR modules that you install. FoxSDR does not drive them itself.
- Plugin and companion downloads are checked against a checksum from the plugin catalogue, which arrives over HTTPS. The catalogue itself carries no digital signature. The update check works the same way.
- The plain (non-HTTPS) web page cuts a request that takes more than 20 seconds to arrive. An attacker using many different addresses could still hold many connections for that time. This matters only if you bind the page beyond this computer. HTTPS adds its own 15-second limit.

---

**SHA-256 of `foxsdr-setup-0.99.109.exe`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.109-x86_64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.109-linux-x64.tar.gz`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.109-aarch64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.109-linux-arm64.tar.gz`:**
`TBD`
