# Using FoxSDR

Every feature in detail: the receiver, plugins, frequency lists, airband, the patch view, languages, the keyboard, browser access, how it looks, privacy, what happens when it crashes or freezes, and installing.

Back to the [README](../README.md).

## Contents

- [What is in the current build](#what-is-in-the-current-build)
- [CAT control](#cat-control)
- [Plugins](#plugins)
- [Frequency lists](#frequency-lists)
- [Airband](#airband)
- [Tuning with the mouse on the spectrum and the waterfall](#tuning-with-the-mouse-on-the-spectrum-and-the-waterfall)
- [Markers on the waterfall](#markers-on-the-waterfall)
- [Peak hold and average on the spectrum](#peak-hold-and-average-on-the-spectrum)
- [The range of the spectrum and the waterfall](#the-range-of-the-spectrum-and-the-waterfall)
- [The patch view](#the-patch-view)
- [The new window](#the-new-window)
- [Several radios at once](#several-radios-at-once)
- [Language and country](#language-and-country)
- [Keyboard](#keyboard)
- [Browser access](#browser-access)
- [HTTPS and listeners](#https-and-listeners)
- [Network features and what they send in the clear](#network-features-and-what-they-send-in-the-clear)
- [How it looks](#how-it-looks)
- [The satellites map](#the-satellites-map)
- [Privacy](#privacy)
- [When it crashes or freezes](#when-it-crashes-or-freezes)
- [Installing](#installing)
- [Installing from a script, and the command line](#installing-from-a-script-and-the-command-line)
- [Libraries you add yourself](#libraries-you-add-yourself) <!-- PENDING MERGE: libraries -->
- [Helper plugins](#helper-plugins) <!-- PENDING MERGE: helpers -->
- [New and updated decoder plugins](#new-and-updated-decoder-plugins) <!-- PENDING MERGE: new-plugins -->
- [Decoder plugins and the network](#decoder-plugins-and-the-network) <!-- PENDING MERGE: new-plugins -->
- [How far each decoder plugin has been proved](#how-far-each-decoder-plugin-has-been-proved) <!-- PENDING MERGE: plugin-proof -->

## What is in the current build

Working and in use, and still pre-1.0 — the interface and the plugin catalogue
are still moving. What is in the current build:

- **Receiver.** Spectrum and waterfall, NFM/WFM/AM/DSB/USB/LSB/CW, squelch,
  AGC, noise reduction, manual and automatic notch, de-emphasis, stereo FM with
  pilot lock, and RDS (programme service name, radio text, PI, PTY).
- **Hardware.** An RTL-SDR, a HackRF, an Airspy R2/Mini, an Airspy HF+, a
  HydraSDR RFOne (not yet tested on hardware - see [HARDWARE.md](HARDWARE.md)), a
  Mirics MSi2500 or an RX888 mk2 through FoxSDR's OWN drivers, needing no
  SoapySDR install of any kind - only that the radio is bound to WinUSB
  (Zadig), which every SDR application needs anyway. An **SDRplay RSP** is
  driven natively too, through the SDRplay API the user installs, because
  SDRplay publish no device protocol and an RSP cannot be reached any other
  way. An **ADALM-Pluto** is driven natively over the network, with no libiio
  and no vendor module at all - its address is typed rather than discovered,
  because a network cannot be walked. A remote RTL-SDR is reached the same way
  through an **rtl_tcp** server (see [The rtl_tcp network source](HARDWARE.md#the-rtl_tcp-network-source)).
  Anything else -
  a USRP, a LimeSDR - through SoapySDR as before.
  Antenna, sample-rate and per-stage gain selection on all of them,
  with each gain slider spanning what that stage will actually accept and
  lettered in the unit that stage is really measured in — decibels on every
  radio but the Airspy R2/Mini, whose gains are the hardware's own
  register steps and are shown as bare step numbers rather than invented
  decibels — and a bias-tee switch on the radios that have one. The **Auto gain** switch is remembered per radio and put back when that radio is opened again (an Airspy keeps its own gain mode instead); the AUTO key under the GAIN dial on the top bar is the same switch. Switching it off puts your manual gains back on the radio — some radios, a B200 among them, fall to the bottom of every stage when they leave automatic gain — and the sliders and the GAIN dial then show what the radio really holds. From 0.99.75 the gain you set with a slider, the GAIN dial or the scope's gain knob is remembered per radio, stage by stage, and put back when that radio is opened again (it replaces the 30 dB a SoapySDR radio such as a B200 used to start every stage at; a stage the radio no longer has is ignored, and an Airspy keeps its gains with its gain mode). Developed against an Ettus B200
  and an RTL2838 (R820T); the built-in signal generator and IQ-file playback
  mean it runs with no radio at all.
  A saved SoapySDR RTL-SDR, HackRF, Airspy, Airspy HF+, HydraSDR
  (`driver=hydrasdr`), SDRplay, Mirics
  (`driver=miri`) or RX888 (`driver=sddc`) is OPENED NATIVELY on
  the next launch without being asked, and the log says so; a dongle whose
  tuner the native driver does not support (E4000, FC0012/13) falls back to the
  SoapySDR path and says why. An RSP1, RSP1A or RSP2 is a Mirics chip that can
  be reached four ways, and since 0.99.61 the SoapySDR ones do not go round the
  SDRplay API: once the SDRplay connection has been lost in a session, no
  SoapySDR `sdrplay` or Mirics row is offered and a saved setup that names one
  is refused with the reason, and a SoapySDR Mirics (`miri`) row is not offered
  for a radio the SDRplay API manages. A Mirics dongle on a computer with no
  SDRplay software is not affected, and the SoapySDR `sdrplay` row stays
  available while the connection is healthy. The Source section lists the native radios first, labelled
  "(native)", and names any radio that is plugged in but not bound to WinUSB -
  an RTL dongle still on the DVB-T driver, an Airspy still on its vendor one, a
  television stick still on its DVB-T one - rather than leaving it silently
  missing. An RSP that cannot be listed because the SDRplay API is not
  installed - or is installed but older than 3.07, which FoxSDR cannot drive -
  gets a sentence saying exactly that, rather than nothing at all.
  The SoapySDR device scan still waits for the radio to close - the vendor
  probe opens and resets every dongle it finds, the streaming one included -
  but Refresh is live again, because the native enumeration reads SetupAPI
  properties and opens nothing.
- **PPM frequency correction** (0.99.56). Every radio's crystal is a few parts
  per million off, and the error grows with frequency - 5 ppm is 500 Hz at
  100 MHz and 8.6 kHz at 1.7 GHz. **Settings > Frequency correction** (the
  SYSTEM bank; the same controls sit in the Source section under the
  converter) has one switch, off by default, and a value per radio from
  -200 to +200 ppm in 0.1 steps - positive when stations show up below their
  real frequency. The value belongs to the radio it was set on (its driver and
  serial number, named beside the field) and is put back on every open of that
  radio; a change takes effect at once, with no reopen. A radio that corrects
  its own crystal is sent the value - the native RTL-SDR driver (whole ppm
  only: the typed value is rounded to the nearest whole ppm, halves away from
  zero, and the Source section says which number went in; its sample rate is
  corrected too), the rtl_tcp client (the same whole ppm, sent to the remote
  dongle) and any SoapySDR radio whose driver reports a frequency
  correction. Every other radio - HackRF, Airspy, RX888, Pluto, AOR, SDRplay
  for now, and SoapySDR drivers without one - is corrected by retuning: it is
  asked for the frequency that lands it on the one you chose, so the centre
  frequency is right but the sample rate keeps its error. The Source section
  says which of the two is in use. Everything you see - the counter, the
  spectrum, bookmarks, decoders, the patch page - stays in the true
  frequency, and the patch page's own radios take the same per-radio value.
  The signal generator, a sound card and an I/Q file have no crystal to
  correct and are left alone. While it is on, the status column's RECEIVER card
  reads "PPM +1.5" and the diagnostics bundle carries a `ppm:` line.
- **Working with signals.** Bookmarks, a band scanner with a Skip key and a
  listen limit so a station that never goes quiet cannot stop it (or that
  scans only the bookmarks you tick), the Airband monitor (type an airport,
  hear all of its control frequencies at once - see **Airband** below), and
  recording of both audio and raw I/Q. The receiver's own position - what
  every range and bearing on the map and the radar scope is measured from -
  can be typed, taken from a map click, or read from a GPS receiver on a
  serial port: name the port (on a map page's bar, under the rail's Radar
  section, or under the rail's SYSTEM > Serial ports, which also lists every
  port the machine currently has), press "Read position from GPS", and the
  first fix (`$GPGGA`, `$GPRMC` or `$GPGLL`, any talker, up to 128 characters
  for high-precision receivers; dead-reckoned and simulated positions are
  waited out) sets it exactly as a typed one would; the port is held only
  until that fix arrives, at most 60 s, and neither the position nor a typed
  device path is ever written to the diagnostic log. `FOXSDR_GPS_PORT=<port>`
  (with `FOXSDR_GPS_BAUD`, default 9600) does the same read at start-up, in
  every run mode, for a bench with a receiver on it.
- **A bench oscilloscope**, under TOOLS. The demodulated audio drawn as a
  phosphor trace on a ten-by-eight graticule, with a 1-2-5 time base from 1 to
  50 ms a division, an attenuator that ranges itself (or steps by hand), and a
  rising-edge trigger with hold-off, which is what makes a tone or a vowel
  stand still instead of sliding. The same tube shows three other things from
  one row of keys: the audio's own spectrum, 0 to 20 kHz at 10 dB a division;
  the channel I/Q at the demodulator's input as separate I and Q traces; and
  that same I/Q plotted against itself as a vector display, which is where AM,
  FM and SSB stop looking alike. When a plugin is playing through the host the
  scope is showing the plugin's audio and says whose it is - the tap sits below
  that handover, so the tube and the speakers can never disagree.
- **A tuner plate for a counter**, bolted onto the front of the deck after a
  1950s military receiver: an olive-drab riveted plate with an engraved
  "TUNED - HERTZ" name plate, a receiver lamp and a MHz readout across its
  head, and a black bezel holding ten Nixie tubes - one per digit, 1 GHz down
  to 1 Hz - each with a chrome toggle switch beneath it. Scroll a tube to step
  that digit, click one to type a frequency, or flick the switch: the upper
  half steps the digit up, the lower half down, holding either repeats, and
  the lever stays pointing the way it was last flicked. A typed frequency that
  lands in a different band of the band plan also adopts that band's
  conventional mode - type 3.620 and the receiver is in LSB, type 118.1 and it
  is in AM - while typing within the band you are already in never touches the
  mode, so a digital setup (FT8 on 7.074 USB) survives nearby retypes; wheel,
  switch, drag and scanner steps never change the mode at all, and the Radio
  section's mode keys always win afterwards. The SAMPLE RATE and
  FRAME TIME meters at the deck's right are on the bar at the window size the
  application first opens with, not only once it is widened (the whole deck
  is drawn about 8% smaller there, to keep them beside the three dials
  below). Right-click either meter to pick its own face - Classic, Analogue
  needle, LED ladder or Peak meter - without changing what it reads.
- **Three dials on the deck: GAIN, SQUELCH and VOLUME**, left to right, in the
  order the signal goes through the system, all one size, each answering a
  drag round the dial or the wheel over it, with its setting printed under it.
  **GAIN** sets the radio's first gain stage (the one the Source section lists
  first, and the scope's gain knob drives) over that stage's own range, and
  says N/A with no radio open and AUTO while automatic gain is on, when it is
  dead; hover it for the stage's name. The **AUTO key** under it is the Source
  section's Auto gain box - lit while the radio is gaining itself, and dark,
  with a tooltip saying why, on a radio that has no automatic gain. **SQUELCH**
  sets the level the receiver's channel must reach to be heard, the same
  threshold as the Radio section's slider, from -120 to 0 dB. Its **AUTO key**
  turns on AUTOMATIC squelch: the receiver measures the quietest the channel has
  been over the last ten seconds - the noise floor - and keeps the threshold
  8 dB above it, so weak carriers open it and the noise does not; the dial
  follows. While the squelch is open the signal is being passed, which is not
  the noise, so what the receiver reads then is not counted once it has a floor:
  a carrier, however long, does not teach it. Noise that rises and stays risen
  becomes the floor within ten seconds, if it stays under the threshold; if it
  rises past the threshold the squelch opens and holds, and a squelch that has
  stayed open for thirty seconds without a break is taken to be a floor that
  rose, and is learnt. A signal that holds the channel for that long from the
  moment you tune to it - a continuous broadcast - is the floor too, and
  AUTOMATIC will close on it. A hand on the dial, the Radio section's slider, the
  squelch keys or the web control is a threshold of your own and switches
  AUTOMATIC off again, and a new frequency, bandwidth or mode starts the
  measurement afresh.
  **While the Airband monitor is listening, the SQUELCH dial and its AUTO key are
  the monitor's:** the dial reads and sets the Airband section's own squelch
  (-110 to 0 dB, the same one as that section's slider) and leaves the
  receiver's threshold where it was, its tooltip says so, and a new threshold
  takes effect when you let go of the dial. AUTO then applies the monitor's
  "Above the noise" rule - 8 dB over the median channel level - and re-applies it
  every quarter second while the key is lit. The Airband section's own slider and
  its Above the noise key switch AUTO off, like the dial. AUTO is one setting for
  both squelches, saved as one; when the monitor stops, the dial and AUTO are the
  receiver's again.
- **Band plans for the whole world.** Labelled service allocations drawn behind
  the spectrum, colour-coded by service on a colour-blind-safe palette, with a
  "what am I tuned to" readout that names the *narrowest* match — 145.800 MHz
  reads "ISS Downlink", not "2 m Amateur". Pick your region from SYSTEM →
  Display → Region: a baseline for each ITU region plus country refinements that layer on
  top of it. The default is a worldwide plan holding only the allocations that
  are identical everywhere, because the ITU regions genuinely disagree — 40 m,
  mediumwave and the FM broadcast edges all differ — so there is no single plan
  that is correct globally. Informational only; not a licensing reference.
- **Map and decoders.** Aircraft, vessels and stations plotted together,
  coloured by altitude band, with optional map imagery from a basemap plugin;
  wherever its tiles are drawn - the map pages, the patch Map part and the
  radar scope - the plugin's attribution is lettered beneath them.
  An aircraft that reports **no altitude** is drawn **grey**, with a hollow
  marker, rather than in an altitude colour - so it is never mistaken for one
  on final approach, which is the lowest band - and the altitude key on the map
  gains a "no altitude" row for it when there is one to explain.
  A target's **trail is coloured along its length** by the same bands, so a
  climb-out and a cruise read differently at a glance — from the altitudes the
  host watched the aircraft report as it flew the line, not from anything
  inferred, so a stretch it never saw stays in the target's plain colour rather
  than being given a number. **Trails** and **Altitude colours** are separate
  checkboxes on every map page: some people do not want the colouring and some
  do not want the lines, and both are one tick away.
  Beside the map is a list of everything being heard: callsign, id, and a
  details button per row. The button opens the full block for that target —
  registration, type, operator and country where a track-info plugin can supply
  them, then position, altitude, speed, course, age, and distance and bearing
  from your receiver. Where that plugin also offers photos (the Aircraft Info
  plugin does from 1.2.0, from Planespotters.net), a photo of the aircraft
  appears under the block, with the photographer's credit beneath it; click the
  photo to open its page in your browser. A photo is asked for only when you
  open a target's details — never for everything on the map — and is not kept
  once the details are closed. A **Sort** control above the list orders it by any of
  those eight — callsign, id, altitude (ft), speed (kt), course (deg), distance
  (km), bearing (deg) or age (s) — ascending or descending, so "what is nearest
  me" is one menu pick away. Clicking a row takes the map to that target;
  double-clicking follows it. Tick **On screen only**, beside the target count,
  and the list shows only the aircraft inside the map as you are looking at it:
  zoom in on an airfield or an air show and the list follows, instead of naming
  everything the receiver has heard. The count then reads "3 of 41 targets",
  and the setting is remembered. An aircraft the receiver has heard but cannot
  yet place (its position message has not arrived) is listed too, below the
  others and dimmed, with its altitude where it is known and "no position yet",
  and the count says so: "12 targets, 3 heard without a position". It is not
  drawn on the map, and it is not in the list while **On screen only** is
  ticked; it moves up into the list proper, and onto the map, when its position
  arrives. This needs ADS-B plugin 1.9.0 or newer; older plugins list only the
  aircraft they can place. An aircraft at the edge of range is heard in bursts
  tens of seconds apart; between them (from 0.99.78 with ADS-B plugin 1.9.1 or
  newer) it stays on the map where its last course and speed would have taken
  it, drawn with a thin ring and listed with the word "estimated" beside its
  id, fading from 30 s after its last fix and leaving the map at 180 s instead
  of 60 s. The age shown is always the age of the last real fix.
  Give it your antenna's position once and it also builds a **coverage map**:
  the furthest anything has been heard in each direction, drawn over the map,
  which is the cheapest antenna diagnostic there is. Decoder plugins produce
  text or pictures (slow-scan and weather-satellite images, shown in their own
  windows and saveable). A plugin that reports **satellites** gets a whole
  instrument of its own instead of a map page — see
  [The satellites map](#the-satellites-map).
- **Browser access.** The full interface over HTTP on your LAN, at feature
  parity with the desktop, with password authentication and live audio. Off by
  default; see [Browser access](#browser-access) for the security posture.
- **A REQUEST A FEATURE key**, and under it **a REPORT A BUG / DISLIKE key**,
  above the maker's plate in the STATUS column on the main screen. Each opens
  a page with a text box and an optional contact line (the second first asks
  whether something is broken or something you dislike); nothing is sent until
  you press SEND, and there is no queue. See [Privacy](#privacy) for exactly
  what leaves the machine.

- **Several radios at once** (on the `engine/multi-radio` branch, merged, not yet released). The main window opens up to
  eight radios, each with its own source, frequency, mode, squelch and decoders, shown one, two or four to a page and
  mixed to the speakers with a route, mute, solo and volume for each. See [Several radios at once](#several-radios-at-once).
  The patch canvas keeps its own limit of five Radio nodes.
- **The new window** (same branch). The window `foxsdr-slint.exe` starts, with saved layouts, waterfall export, more trace
  and display choices, A/B markers, scope analysis keys, scanner lockouts and memories, and a status-bar clock. See
  [The new window](#the-new-window).

**Verification, honestly stated.** The DSP core and every decoder carry unit
tests (`ctest` runs 82 entries: 75 test binaries and seven checks on the
application itself), and the audio chain has been confirmed by ear on
broadcast FM. Of the decoders, **ADS-B is the one confirmed against real
off-air signals** — aircraft decoded live, with ICAO address blocks and
callsigns agreeing across independent message types. The others (AIS, APRS,
SSTV, Morse, RTTY, POCSAG) are verified against synthesised signals and
published constants, which is real evidence but not the same thing. The
Inmarsat-C plugin is published at 0.1.1 and explicitly marked EXPERIMENTAL:
roughly ten of its air-interface constants are reconstructed guesses, and it
will most likely decode nothing off air. Each plugin's catalogue entry says
where it stands.

See [PLAN.md](../PLAN.md) for the roadmap and architecture.

## CAT control

The receiver can be driven by logging software, digital-mode applications and
anything else that speaks the protocol Hamlib's `rigctld` uses. Open **CAT
control (rigctld)** in the **EXTEND** group of the FUNCTION SELECT rail down
the left of the window and turn it on, then point the client at this machine
and choose a Hamlib **NET rigctl** radio. Port 4532 is the default because it
is the one those clients already expect.

Frequency and mode can be read and set; a set that lands inside the band
already being received moves only the VFO, so there is no retune and no gap in
the waterfall. PTT always reads *receive* and refuses to key — this is a
receiver, and saying so plainly is what stops a client from waiting on a
transmission that will never happen.

**This protocol carries no authentication**, so the server listens on this
machine only unless you deliberately widen it. Anything that can reach the port
can retune the radio.

Nothing from Hamlib is used: it is GPL and is neither linked nor read here. The
implementation follows the published `rigctld(1)` protocol description, the
same way every decoder in this project is written from its own specification.

## Plugins

Decoders can be installed as separate native plugins, from an in-app catalogue. Two keys in
the **DECODE** group of the FUNCTION SELECT rail open the two windows this lives in, and they
are two windows on purpose, each meant to be simple: **Plugin store** is what you could have,
and **Plugins** opens **Fitted modules**, which is what you have and whether it is working.
Each is a real window you can drag as large as your screen, and each comes back at the size
and place you left it, because its rectangle is written to the configuration file with
everything else. The store opens *inside* the main window rather than overhanging its right
edge, where on a single monitor it could land off the screen entirely.

**The Plugin store** has one bar along the top and two tabs. The bar holds a search field
(it looks through names, one-line summaries, descriptions and the category's name), the
tabs **BROWSE** and **UPDATES (n)**, and at the right when the catalogue was last read
("Catalogue read 14:07") with **CHECK AGAIN** and, beside it, **SITES** (see **Plugin sites**,
below). **BROWSE** is the catalogue in sections -
AIRCRAFT, MARINE, SATELLITES AND WEATHER, METERS AND PAGING, VOICE AND DATA, MAPS AND TOOLS,
and OTHER for anything the catalogue has not filed - each a grid of cards: a small drawing of
what the plugin is for, its name, a line about it, a badge when it has never decoded a real
signal (**EXPERIMENTAL**), has no build for your system (**WINDOWS ONLY**, **LINUX ONLY**) or
comes from a site you added rather than from the FoxSDR catalogue (**THIRD PARTY**),
and one key at the right: **GET**, **FITTING...** while its download runs, **INSTALLED** or
**UPDATE** with the two versions beside it. A key that cannot act is drawn greyed and says why
when you rest the pointer on it - the plugin was built for another plugin ABI, there is no build
for this system, the catalogue states no licence, another transfer is running. A click anywhere
on a card - its name, its drawing, the line about it, the strip along the bottom (0.99.77; it
used to be the name and the drawing only) - opens that plugin's **page**, and a click on the
card's key is the key's alone: it does not also open the page. **GET** on a plugin whose maker
attached a legal notice opens the page instead of fitting it, because the notice cannot be
skipped.

A plugin's page is what an app store's would be: a header with its name, its maker, version,
download size and the build for this system, and the one key; then **SCREENSHOTS**, a strip of
the pictures its maker published, two arrows and the mouse wheel moving along it (a plugin with
none shows a frame saying so); **WHAT IT DOES**; **WHAT'S NEW** in this version; and, for a
plugin with a legal notice, **BEFORE YOU FIT IT** - the notice, verbatim, in a box with a
tick, "I have read the notice above and accept responsibility", which the key stays greyed
until you give; and last **SHOW DETAILS**, which opens the facts: maker, licence, version, the
plugin ABI and whether it matches this build, download size, the systems it is built for, what
it reaches (in the words the Fitted modules page uses), its home page, the SHA-256 of the build
for this system, the day it was published and, if it is fitted, where it is. The reach list is
what the module *declares*, not a limit on it: a fitted module runs inside FoxSDR with every
privilege FoxSDR has, and the page says so beside the list. Esc, or **< BROWSE**, comes back to
the list. An install's result - "Installed ... to ..." or the reason it failed, word for word -
is shown under the key of the plugin it concerns.

**UPDATES (n)** lists what the catalogue offers that you are behind on: the plugin, "1.8.0 to
1.8.1", the first line of what is new, and an **UPDATE** key; **UPDATE ALL** at the top right
does them one after another. An update that succeeds takes the older copy of that plugin with
it, by itself (an older file still in use goes at the next start), and **CLEAN UP OLD VERSIONS
(n)** at the foot of the tab removes any other leftovers, asking once. Nothing updates itself:
"Updates are fetched when you press CHECK AGAIN."

At the foot of BROWSE, a muted **GET EVERYTHING** fetches everything that is not installed and
updates everything that is behind, one after another, through exactly the path a single **GET**
takes - https, the byte cap, the exact ABI match, and the sha256 the catalogue published, which
every download must hash to before it is moved into the modules folder. It says what it will do
before you press it ("14 to fetch and 2 to update ..."), names any plugin it will pass over and
why, and reports what happened when it finishes ("24 installed, 0 failed", or the names that
failed with the reason each gave); a plugin that fails does not stop the rest. Plugins whose
maker attached a legal notice are counted and named and are added only if you tick the
acknowledgement beside the key - "get everything" is not a way to consent to something you were
never shown. It fits the FoxSDR catalogue only: plugins from sites you added (below) are left out
of it, and of **UPDATE ALL**, and the key says so.

**The catalogue** is contacted when you open the store window (the first time in a session) or
press **CHECK NOW** / **CHECK AGAIN**: nothing is fetched at startup, and no plugin ever updates
itself. The last catalogue read successfully is kept beside the install records
(`catalogue.json`) and is what the store shows, at once, the next time; if a refresh then fails the
list stays and the top bar says so in amber - "Catalogue from 2026-10-07; could not refresh:
<the reason>" - instead of the store going empty. **A plugin's pictures** are fetched only when
you open its page, one at a time, over https, at most 4 MiB each, and each is checked against the
sha256 the catalogue published before it is kept (under `store-cache` in the plugins folder, named
by that digest, and removed again when the catalogue stops naming it); a request carries the
picture's address and nothing else, and a plugin that comes from the regional list shows none.
Nothing is fetched for the grid.

**Plugin sites.** Someone else may publish plugins of their own in the same `index.json` format
(for a hobbyist's decoder, say); [docs/THIRD-PARTY-SITES.md](THIRD-PARTY-SITES.md) is written for
them. To list one beside the FoxSDR catalogue, press **SITES** in the store's top bar: a panel opens
under the bar with the sites you have added (each with a **REMOVE** key), an *index.json URL* field
and **ADD**. Paste the address of the site's `index.json` (https, or the path of a file on this
computer) and tick the box under the field - "Plugins from this site are not reviewed by FoxSDR.
They run with the same access as this application. Only add a site you trust." - or **ADD** stays
greyed; the tick is cleared each time, so each site is a decision of its own. Up to eight sites can
be added. Nothing is fetched when you add a site except through the same read **CHECK NOW** makes:
a site is read after the FoxSDR catalogue, each in turn and each with only a few seconds to answer,
and a site that is down, or is not a catalogue, says so in amber under the top bar - "<host>: could
not be read: <the reason>" - and never empties the rest of the list (its last good read, kept
beside the install records, stays listed until it answers).

What you will see of a site's plugins: the **THIRD PARTY** badge on every card, "from <host>" in
the line under the name on its page, a one-line note of where it came from, and - whether or not its
maker wrote a legal notice - the **BEFORE YOU FIT IT** box, which begins "This plugin comes from
<host>, not from the FoxSDR catalogue. FoxSDR has not reviewed it." and then gives the maker's own
notice, if any, word for word. The tick under that box is your consent, and the key stays greyed
until you give it; **GET** on a card opens the page rather than fitting it. Under the top bar each
site has a status line - "<host>: 7 plugins" - and says what it hid and why: "N hidden: same id as
a plugin already listed" (a site can never replace or shadow a plugin the FoxSDR catalogue lists:
the catalogue's wins), "N hidden: files not on the site's own host" (a site's plugin files and
pictures must be served from the same host as its `index.json`, and a plugin that names another
host is not listed at all).

A module installed from a site also never takes a *declared name* from an official plugin: the host
loads one module per name the plugin's own binary declares, normally the highest version, but when the
FoxSDR catalogue's plugin holds the name the site's module is not loaded whatever its version, and its
row in the Fitted modules window (and the log) says "Not loaded: it declares the same name as the
official plugin ...". Nothing is deleted; **REMOVE** takes the file away. The same holds for the
file's name: a site's module whose file name is an official plugin's (the name before the version,
`tetra-decoder` in `tetra-decoder-0.1.2-abi3-win-x64.dll`) is not loaded either, and says "Not loaded: its
file name is the official plugin ...".

Everything a site's plugin goes through is what any plugin goes through - https only, the byte
cap, the exact ABI match, the sha256 the site published, no redirect to another host - and a site's
plugin is known by *which site* it came from as well as by its id, so a site can neither update an
official plugin nor be updated by one, and a site cannot overwrite another plugin's file in the
modules folder. What FoxSDR does *not* do is review it: the plugin is native code that runs inside
FoxSDR with everything FoxSDR can do, and nothing in this product checks who wrote it. Add only
sites you trust. **GET EVERYTHING** and **UPDATE ALL** leave a site's plugins out (fit each from
its page, update each with its own **UPDATE** key), and the web page can list them but not fit
them. **REMOVE** forgets a site and deletes its kept copy; the plugins you installed from it stay
installed, the Fitted modules window says they are from a site you removed, and they get no more
updates. The addresses are in the configuration file (`pluginSites`). The rail's *catalogue
index.json URL* field is a different thing: it replaces the FoxSDR catalogue's own address, and
sites are added beside it.

**Fitted modules** is the operating panel. Along its top bar: a search field, six filter chips
with their lamps and counts - **FED**, **IDLE**, **NOT DECODING**, **TAKES NO SIGNAL**, **STOPPED**,
**REFUSED**, each a toggle - and **SCAN AGAIN** and **RESET WINDOW SIZES**. Under it one muted line
says whether the receiver is running (a module with a matched decoder is only fed while it is) and,
after a dot, which folder the modules were read from. Then one row per module, under the store's own
headings - AIRCRAFT, MARINE, SATELLITES AND WEATHER, METERS AND PAGING, VOICE AND DATA, MAPS AND TOOLS
and OTHER - and sorted by name inside each (0.99.77). A module's heading is the catalogue's category
for it: one the catalogue has not filed - a module you dropped into the folder by hand, or any module
before a catalogue has been read or kept - is listed last, under OTHER, and while no module has a
category at all, as on a fresh install, the list is the plain list sorted by name with no headings.
A row has its drawing, name and version, under the name an amber warning when it reaches beyond FoxSDR ("asks to
move the receiver", "may fetch from a server it chose" - nothing for a module that publishes to the
host only) or, for a module the host refused, the host's own reason; at the right what it is doing -
**FED**, **IDLE**, **NOT DECODING**, **TAKES NO SIGNAL**, **STOPPED** or **REFUSED**, with its lamp
- then **STOP** (or **START**: a stopped module and an idle one both offer it) and **REMOVE**, which
asks twice: **CONFIRM** appears beside it for five seconds. **CLEAN UP OLD VERSIONS (n)** is at the
foot when an update left copies behind. One version of a plugin runs, never two: an older file of a
plugin that is still in the folder reads **REFUSED** with "Ignored: another copy of ... is installed",
and that holds when the plugin's own name changed between the two files (0.99.77: a US tester had both
the "TETRA" file and the "TETRA Monitor (data only, no voice)" file, and the DECODE bank listed the
plugin twice). Two files are one plugin when their names are the same *or* their file names are the
same before the version; the higher version runs, and the older file is one **CLEAN UP OLD VERSIONS**
counts. A click on a module's name opens its **page**: its header
with the same **STOP** (or **START**) key, then **ON THIS MACHINE** - the state with its lamp, what
the module reaches, the sentence that explains the state (for a refused module, why it is not
running, in the host's words), the **Keep running** tick (see **When a plugin runs**, below), the
file it was loaded from, its plugin ABI and the day it was fitted - and, for a module the
catalogue knows, the same pictures, description and notes the store's page shows, and the facts
behind **SHOW DETAILS**.

**Double-click a row**, anywhere but on its keys, to open the plugin's own window instead of its
page (0.99.77): its panel if it has one, otherwise its picture window, its instrument or its map
page. A dormant plugin wakes as the window opens (see **When a plugin runs**), as it does from its
row on the rail, but nothing is tuned - the rail's rows also tune to the plugin's preset, and this
does not. A module with no window of its own - a text decoder, read in the shared **Decoder
output** window - gets its page, and so does a stopped one: with its instances gone no window of
its could draw, and the page says so and offers **START**.

Neither window reopens by itself: since 0.79.1 FoxSDR always starts on the main window alone,
whatever was showing when it was closed - no page, no map, no decoder output, and the bench rather
than the radar scope. Every one of them is a key on the rail away, and nothing opens a window but
your own hand. A plugin's own window - a decoded picture, a plugin's panel - has a row of its own
under DECODE, with a chip saying what it holds (WAIT, RX or IMG for a picture, a row count for a
panel, **IDLE** while the plugin is not running): press the row to open it - which also starts the
plugin, see **When a plugin runs** - and its key to close it. A decoder with no window of its own - the
POCSAG and DMR decoders, say - writes its lines in the shared **Decoder output** window, which a
preset on such a decoder opens for you. That window, and a decoded picture's, opens INSIDE the main
window, in the middle of it, wherever the main window is on the screen: it used to open past the
main window's right edge, and on a main window that reached the edge of the screen only a narrow
strip of its frame showed - "a partial vertical bar" - where the lines should have been. Drag it
by its top rail to put it beside the radio or on another screen; **RESET WINDOW SIZES** in the
Fitted modules window brings it back.

**A row in a plugin's panel can be a key** (0.99.77). Where a plugin lists things to choose between -
the IBOC Radio plugin's programmes HD1 to HD8, say - press the row to choose it, rather than going
into the plugin's settings to type the number: the row lights under the pointer, and the plugin gets
the press exactly as it gets a press of one of the keys on its page in Fitted modules (a plugin may
now have sixteen of those, where it had eight). A row that is only text - a heading, or a line the
plugin has not made into a key - does nothing when pressed. A plugin that is written for an older
FoxSDR draws the same rows as text.

A plugin may declare several capabilities. Decoders are fed real samples —
either the tuned, demodulated audio or the raw receiver band — and produce
either text lines or **images** (slow-scan and weather-satellite pictures,
shown in their own window and saveable as BMP). A plugin may also put targets
and tracks on the map, and declare a window of its own. An audio decoder is
fed at the rate it asks for: since 0.83.0 the host resamples the tuned audio
to a decoder's own clock (a 16 kHz tone decoder, a 22.05 kHz pager decoder),
where before it idled any decoder not built around 48 kHz. A raw-band decoder
still gets whatever rate the device is running at, because only the device
can change that.

**Instruments** (0.83.0). A plugin may also declare an *instrument*: it names
a kind — a pager, a message printer, a tone-alert receiver, a VOR course
indicator, a fax machine, a distress-beacon receiver, a utility meter, a
weather-station console — and feeds the host the figures and words that kind
shows, and the host draws the window as that piece of equipment, in the same
brass-and-glass as the rest of the bench. The plugin never draws: it fills
slots whose meaning the ABI fixes per kind, and a host that has no face for a
kind (an older host, or a newer plugin) draws a plain readout of every filled
slot instead, so nothing sent is ever hidden. Each instrument has a row under
DECODE like a panel's, whose chip reads **NEW** while something has arrived
that the window has not shown — the closed pager still says it has a message
— and the optional memory beneath the face (a pager's stored messages, a
meter roster, a burst log) is the plugin's own row feed. The fax machine's
paper prints the chart itself as it is received — the machine and its picture
in one window — while the plugin's picture window stays the full-size reading
copy. Instrument windows
are desktop-only for now; the browser interface does not carry them yet.
Developers: `FOXSDR_DEMO_INSTRUMENT=pager` (or any kind name, or `all`) opens
demonstration instruments fed with sample state, so a face can be worked on
with no radio and no plugin — the only windows that open by themselves, and
only under that switch.

**Sound of its own** (0.93.0). A plugin may also declare that it *plays sound
through the host*. Up to here a decoder could hand FoxSDR text, pictures, map
targets, a window or an instrument face and could not hand it audio — so a
DAB/DAB+ decoder, whose entire output is PCM, could only write a WAV file and
leave the speakers playing the raw hiss a digital carrier makes of an FM
demodulator. A plugin that declares it hands the host blocks of samples at its
own rate, and while it is decoding, **its audio replaces the demodulated
audio** rather than mixing with it: the band is not underneath, because a
programme and the carrier it was decoded from are not two things to listen to
at once. Exactly one plugin plays at a time — if a second asks while the first
is playing, the first keeps the speakers and the refusal is written to the log
once, not once a block. When it stops, the demodulated audio comes back. Both
changeovers are faded over 5 ms, because a hard cut between two unrelated
signals cannot be made click-free. The handover happens one step above the
sink, after noise reduction and before the mute, so your volume, the mute key,
the audio recorder and the browser's audio stream all carry what the speakers
carry, and the mute lamp still tells the truth. The **SINK** card in the status
column names the plugin holding the speakers (`playing: …`), the **AUDIO -
UNDERRUNS** card carries the plugin path's own gap count beside the sink's
starved-callback count — a decoder that cannot keep up and a device that is
starving sound identical and are repaired in different places — and the browser
reads the same two facts from `/api/status` as `audioSource`,
`audioPluginGaps` and `audioPluginGapFrames` (and the audio buffer in force, in
milliseconds, as `audioLeadMs`). In the **Plugin store**, a module
that can do this says so on its page, under REACHES, before you fit it.

**When a plugin runs** (0.99.73). A fitted plugin does not run all the time: it runs while
something is *using* it, and is dormant the rest of the time — no decoder, no map target, no polling,
nothing handed to it. Each plugin is in one of three states, kept against the plugin's **id** (its
file name without the extension and the version, so an update does not lose your choice):

- **AUTO** — the default for every plugin. It starts, within a frame, the moment it is *in use*,
  and goes dormant 30 seconds after the last use ends, so opening and closing a window does not
  start and stop it each time.
- **ALWAYS** — the plugin runs all the time, as every plugin used to. It is what **Keep running**
  (the tick on the module's page in the Fitted modules window) sets, and what **START** and a
  preset press set: a preset is a deliberate "run this".
- **STOPPED** — as below.

A plugin is *in use* while any of these is true, and nothing else counts: one of its windows (a
picture, a panel, an instrument) is open; its map page is open; the radar scope is showing and it is
a track source; a running patch has a node using it; it is the plugin playing through the host's
speakers; the Decoder output window is open and it is a text decoder; a browser is connected to the
web remote and it publishes targets or pictures; its decoded output is wired to a Text sink in a
patch. A plugin that reports only to the host, or that processes the audio you hear, has nothing to
open and is in use as long as it is fitted. **Coverage accumulation, the web snapshot and aircraft
look-ups do not keep a plugin running** — a plugin you want collecting coverage with its map closed,
or answering a browser that arrives later, wants the **Keep running** tick. A window you have open,
a map page you have open or a patch that is running starts its plugin at once, so nothing you are
looking at stops. Opening a plugin's window from its row on the rail starts it (a dormant plugin's
rows are still there, reading **IDLE**), and closing the window lets it go dormant after the 30
seconds. A dormant plugin reads **IDLE** in the Fitted modules window and on the rail's DECODERS
section — not as a fault: it is never counted as "not being fed", and the module page says what it
is waiting for. The log records both changes: `plugin: <name> woke - <what started it>` and
`plugin: <name> dormant after 30 s without a use`.

If you upgrade from a release that ran every plugin all the time, every plugin becomes AUTO (those
you had stopped stay stopped, now remembered by id) — so a decoder that used to collect data with
nothing on the screen stops doing that until you tick **Keep running** for it.

**Stop and start.** In the Fitted modules window every loaded module's row
carries a **STOP** key, and a stopped or an idle one carries **START** (which pins
it: it sets ALWAYS); the module's
page carries the same key at the right of its header.
Stopping destroys everything that plugin had
running — its decoders, its map targets and trails, its window, its basemap
tiles — while leaving the module loaded and the row where it was, so a stopped
plugin decodes nothing, draws nothing, and cannot move the receiver. Only that
plugin is touched: the others, and the stream they are fed from, carry on as they were. The row
then reads **STOPPED**, lettered in plain ivory rather than in anything
that reads as a fault, because a module you switched off is a choice — and a
plugin that produces nothing for a reason you have forgotten choosing is
exactly what this must not become. It is
remembered between sessions and across a rescan, and across an update of the plugin (it is kept
against the plugin's id, not its versioned file name). Pressing one of the plugin's
own preset buttons starts it first: pressing "ADS-B 1090 MHz" is an unambiguous
request for that plugin, and tuning there with the decoder still switched off
would be worse than useless — and it pins the plugin to ALWAYS, so it goes on running when you
close its window. Opening a window from its rail row also applies the plugin's preset, but does
not pin it. A plugin can request reception setup at the current tuned frequency (0.99.74):
opening or starting POCSAG 1.0.3 or newer in FoxSDR 0.99.74 or newer keeps the channel you tuned manually and applies its NFM bandwidth
and flat audio settings, including when already on a listed channel. Explicit frequency preset
buttons still tune to their named channels.
POCSAG opens its settings page and the shared **Decoder output** window together,
so configuring the plugin does not hide received message text.

On the **POCSAG settings** page, **Numeric messages** and **Use function codes**
are both off by default, so all functions decode as text. **Numeric messages**
forces numeric decoding. **Use function codes** takes precedence when enabled:
function 3 decodes as text and functions 0-2 decode as numeric messages. Baud
rate and discriminator polarity are detected automatically.

**Order in the rail.** The rail's **Turn on and off plugins** section (the DECODE bank) lists each
loaded decoder under the same headings as the store and the Fitted modules window (0.99.77), by
name inside each, when the catalogue has filed any of them; a decoder it has not filed is listed
last under OTHER. With none filed it lists them in the order the plugin folder is read, with no
headings.

**Plugin settings.** A plugin that has a settings form of its own (a callsign to report
under, a tolerance) shows it on its row in the **Decoders** section of the rail, under its
keys, in a **Plugin settings** line that is folded (0.99.74): click the line to open the
form and click it again to fold it away, so scrolling down through the decoders does not
mean scrolling past every plugin's fields. The line is folded each time FoxSDR starts; the
values you type in the form are saved as before, whether it is open or not.

**Mute audio while running.** A decoder that consumes raw I/Q is handed the
whole receiver band and tunes inside it, so the channel your speakers are fed
is not the signal being decoded: on ADS-B at 1090 MHz it is hiss, at whatever
the volume happens to be. The **Decoders** section of the rail therefore gives
every loaded decoder a **Mute
audio while running** checkbox, ticked by default for exactly the plugins that
declare they consume raw I/Q and clear for every other. The checkbox on the row
is the answer for any given plugin, because that is read from the plugin
itself — a list printed here would be a list of whatever happened to be
installed the day it was written. Decoders that work on the demodulated audio
are the ones left clear, and deliberately: that audio is the very thing they
are decoding, SSTV's warble and RTTY's diddle are how people tune them by ear,
and silencing them would take away a diagnostic. While a plugin with the box
ticked is running **and** the receiver is on one of that plugin's presets, the
receiver's demodulated audio is silenced. A decoded programme supplied by a
plugin or patch radio replaces that suppressed carrier and remains audible,
including DAB and IBOC audio. User mute and transmit mute still silence the
complete output. The speakers, browser audio stream and **audio recording**
receive the same final sound, so a WAV contains silence while a decoder is
muting and no decoded programme is playing; the Recorder panel says so. (An I/Q
recording is untouched: it is taken before any of this.) The volume setting is
not touched, the decoders keep receiving samples throughout, and **Sinks** says
"Muted by *plugin*" when no decoded programme is playing, so the silence is
never unexplained. Your choice per plugin
is remembered between sessions.

"Running" here means actually decoding, not merely loaded. A plugin you have
stopped mutes nothing, and neither does one sitting idle because the receiver
is not producing the sample rate it asked for — the Fitted modules window
already says so on that module's row (**NOT DECODING**), and quotes the reason on its
page, and taking the sound away on behalf of a decoder the
program itself says is not being fed would be silence for no benefit.

**Tuning away.** Leave a running decoder's preset and FoxSDR asks once whether
to stop it so the sound comes back, with a button that does exactly that.
Decline and the plugin keeps running, the audio stays muted — sound returns
when the plugin stops, which is what the question said — and a small
**Sound muted by *plugin*** banner sits beside the frequency readout, with its
own Stop button, until the plugin is stopped or you tune back. It asks again
only after you have returned to a preset and left it again.

**Tune permission.** A plugin that can move the receiver can also take it away
from you, so a plugin may only retune the radio if you press **GRANT RECEIVER
CONTROL** on that module's page in the Fitted modules window (in its ON THIS
MACHINE box, under what the module reaches). The key is
offered only for a module that can actually ask — one that declares no host
client could never use the grant, and a control that sets something nothing
reads is a control that lies about having done something. It is off by default
and off for every newly installed plugin; a plugin that asks and is refused is
named in the rail's **Decoders** section, under "Receiver control", so a
satellite tracker that needs Doppler correction is one visible click away
rather than mysteriously idle. That same place lists any grant still held by a
module which is no longer fitted — a permission nobody can see is one nobody
can take back. The grant is per plugin and is remembered
between sessions.

**Plugins that transmit** (0.99.74, host API level 2). A plugin that sends data
over the air - a packet radio or Winlink-compatible link, say - can ask FoxSDR
to key the Pluto transmitter and send its audio. Until now only a hand could
key it; a plugin can now too, but only when you have said so twice, in two
different places, and the second time is a window that closes by itself:

1. **Grant it.** Once the plugin has asked to transmit at least once, a
   **GRANT TRANSMIT** key appears on its page in Fitted modules (and
   **REVOKE TRANSMIT** once it is granted). It is off for every plugin until you
   press it, is remembered between sessions, and can be revoked at any time.
2. **Arm the page.** On the **TRANSMIT** page, under the PTT and LATCH,
   **ARM FOR PLUGINS** has **5 MIN**, **15 MIN**, **60 MIN** and **OFF**. While
   the window is open the page counts it down and names the plugin it lets in;
   it closes by itself when the time runs out, and also when you press OFF,
   close or roll up the page, close the transmit radio, take the grant back, the
   radio faults, or the program stalls for more than a second. It is never
   remembered: start FoxSDR again and no plugin can transmit until you arm it
   again. It will not arm with no transmit radio open or no plugin granted, and
   says which.

Even then FoxSDR is in charge, not the plugin: the frequency, the power and the
mode are the ones on the TRANSMIT page (or, with the **DATA (flat)** tick on, the
default, narrow FM with the pre-emphasis off, which is what a data modem wants),
a hand on the PTT, the LATCH or the web remote's key always wins, one plugin at a
time holds the transmitter, and no plugin can hold the key for more than a
minute: at the minute it is cut and the window closes with it. The log writes a
line each time (`tx: plugin <name> keyed`, `unkeyed (<why>)`, `armed`,
`disarmed`). A plugin only gets asked whether it wants to transmit while it is
running, so a transmitting plugin should have its window open or **Keep running**
ticked. Like everything on the TRANSMIT page this puts a signal on the air, and
whether you may is your responsibility and your country's licensing
authority's. See [Transmitting](TRANSMITTING.md) for the full list of rules.

**The host API for plugins** (0.99.31, host API level 1). A plugin that declares
the host-client capability can now read the whole receiver - frequency, VFO
offset, mode, bandwidth, squelch, gains, the radio's sample rates, volume,
mute, whether it is running, which radio it is, the signal level and the
S-meter - and can ask to change any of it. Asking to **tune** needs the same
**GRANT RECEIVER CONTROL** as before; asking to change anything else (mode,
bandwidth, squelch, gains, sample rate, volume, mute, start or stop) needs a
separate **GRANT RADIO SETTINGS** key, which appears on the module's page once
the module has asked. The two are kept apart so that a grant you already gave a
satellite tracker still means only what it meant. A request is applied by the
application itself on its next frame, through the same code a click or a
browser request goes through, and a stopped or ungranted module's requests are
refused outright. A plugin can also put **marks** on the spectrum and waterfall,
keep **its own settings** (saved in the config file under the plugin's name, so
they survive an update; the application can draw them as boxes for you to fill
in, and a setting that names a file - the program a plugin drives, say - has a
**BROWSE FILES** key beside its box that opens the system's file dialog, on
Linux through zenity or kdialog where one is installed), show **messages** in
the Decoder output window (a
warning or an error also appears on its page), and offer **keys of its own** on
its page. And a new capability lets a plugin **process the audio you hear** in
place - a filter, a limiter - after the receiver's own processing and before
the volume, the mute, the recorder and the speakers; stopping the module takes
it out of the chain. None of this lets a plugin reach files or the network
through the application, and none of it changes the plugin ABI: every plugin
built before it keeps loading and working unchanged. Authors: see
[docs/PLUGIN-API.md](PLUGIN-API.md), and the "Host API tour" example in
the plugin repository.

Security model, in one line: every download is https, sha256-verified against
the catalogue before it is allowed to become a file, size-capped, refused on a
cross-host redirect, and written under a sanitised bare filename inside the
plugins directory; a plugin's pictures are held to the same https and sha256
rule and a 4 MiB cap, and are decoded by a PNG reader that refuses anything that
is not a PNG or claims a size no screen needs. A plugin from a site you added
(**Plugin sites**, above) goes through all of it, and in addition its files and pictures must
come from the same host as the site's own `index.json`, it is known by its site as well as
its id, it carries a notice you must tick, and it may not overwrite another plugin's file.

**Where that directory is** depends on whether the application can write to its
own: a portable copy keeps plugins in `plugins/` beside the executable, while an
installation under a directory the user does not own — `C:\Program Files\FoxSDR`
being the ordinary case — uses `%LOCALAPPDATA%\foxsdr\plugins` instead
(`$XDG_DATA_HOME/foxsdr/plugins`, or `~/.local/share/foxsdr/plugins`, on Linux).
The Fitted modules window prints the one in use, in full, after "read from" on the
line under its top bar, and each module's page names its **FILE**.
Nothing needs elevating either way.

Compatibility is ABI-exact. A plugin must be built against this host's
`src/core/plugin_abi.h` and declare exactly its ABI version — a near miss is
refused rather than loaded, because a struct-layout difference becomes memory
corruption days later. A plugin built for an older FoxSDR therefore needs a
new build from its author; no update can fix it. Within ABI 3, though, the
header only ever grows: new capabilities are new bits, and the host table
grows at its end with its size as its version, so a plugin built against a
newer header still loads on an older FoxSDR and simply goes without what that
FoxSDR does not offer.

Retirement: the catalogue may publish a minimum supported version per plugin.
That floor is cached locally the moment a catalogue is seen, so it applies
offline and forever after, and an installed plugin below it is **disabled** —
renamed out of the scan, so it is never loaded into the process. Those are
listed in red under **Disabled**, on the rail beneath the Plugin store key
rather than in either window: the host never loaded them, so the Fitted
modules window has nothing to list. Each carries a one-click **Update** once a
catalogue has been fetched that offers a newer build, and a **Remove** whether
it does or not — an ABI mismatch has no other way out. Plugins the
catalogue has never described (private or hand-installed builds) are left
alone and keep loading.

## Frequency lists

**TOOLS > Bookmarks** holds any number of named frequencies - tens of thousands
is fine - with a group, a mode, a bandwidth and a favourite star each. Click a
row to tune it. The box at the start of each row ticks it: the Scanner's
**Ticked frequencies** scans just the ticked rows (each with its own mode and
bandwidth), and the Airband monitor plays the ticked AM and NFM ones.

- **Import** an SDR# `frequencies.xml`, or a CSV saved from Excel: type or
  paste its path and press Import, or drop the file anywhere on the window.
  Press Import with the box empty and a window opens with a drop zone and a
  **Browse...** key that opens the system's file dialog (where there is one);
  it opens by itself too when a file could not be imported, saying why, and it
  closes when a file is read. A bare file name, with no folder in it, is looked
  for in the recordings folder - where Export for SDR# writes - when the
  process's working folder does not have it. Windows import also accepts
  filenames with accents or other Unicode characters. SDR#'s Name, GroupName,
  Frequency, DetectorType, FilterBandwidth and IsFavourite come across; the
  file does not have to be perfectly formed (a hand-merged list with one entry
  per line reads fine). A CSV can have its
  columns in any order under a header naming them - frequency, name, group,
  mode, bandwidth, favourite - or no header at all (frequency, name, group,
  mode, bandwidth); frequencies are Hz, or MHz when the header says so or the
  number is plainly MHz; commas, semicolons and tabs all work, and so does a
  decimal comma. Importing the same file twice adds nothing twice. SDR#'s
  converter Shift is not applied: the frequency is taken as written, and the
  import says when a list carried one.
- **Import a CHIRP or an SDR++ file** the same way: FoxSDR looks at what is in
  the file, not at its name, so a `.csv`, `.txt` or `.json` all work.
  - *CHIRP*: the CSV that CHIRP exports (its first row starts with
    `Location,Name,Frequency,Duplex,Offset,Tone,...`). The Frequency (MHz),
    Name and Mode columns come across. CHIRP's modes become FM and NFM to NFM,
    WFM, AM and NAM to AM, USB, LSB, and CW and CWR to CW; any other mode (DV,
    DMR, ...) is imported as NFM and the import says how many it did that to.
    A bookmark has no tone field, so a row with a CTCSS or DCS tone keeps it in
    the name, as `Hilltop [CTCSS 88.5 Hz]` or `Hilltop [DCS 023]`. Only the
    receive frequency is taken: a repeater's offset and a split transmit
    frequency are ignored, because FoxSDR listens.
  - *SDR++*: the frequency manager's `frequency_manager_config.json`. Every
    list in it becomes a group of the same name, and each bookmark's frequency,
    bandwidth and mode come across (the mode is SDR++'s index: NFM, WFM, AM,
    DSB, USB, CW, LSB, RAW). This reader has not yet been checked against a
    real SDR++ export, only against the format as published descriptions give
    it; if a mode looks wrong after an import, tell us.
  - A file that is neither is refused with a sentence saying what it was
    expected to be, and nothing is added. The result says how many rows were
    imported and how many were skipped (no usable frequency).
- **Groups of your own:** the box under the name is the group the next
  **Add current** joins - type "MILANO MALPENSA" once and every frequency you
  add after it lands in that group until you change or clear the box; the
  arrow beside it lists the groups the list already has. Right-click a row to
  move it to another group, to none, or to a new name typed in the menu.
- **Find** with the search box (names, groups, or the start of a frequency in
  MHz), the group list, and Favourites only. With a group chosen, "Remove this
  group" takes the whole group out again and "Rename this group" opens a field
  for its new name - a name the list already has merges the two groups.
- **Export for SDR#** writes the entries currently shown - so a search or a
  group picks what is shared - as a `frequencies.xml` in the recordings
  folder.
- **On the spectrum**, every bookmark inside the visible span is marked: a
  short tick in a strip above the frequency scale, a full line and its name
  for favourites, and names for everything once few enough are on screen to
  be read. Only the span on screen is ever looked at, so a list of 33 000
  costs the display nothing; the browser page is sent the favourites and the
  few hundred nearest the tuned frequency, not the whole list.

## Airband

**TOOLS > Airband** listens to an airport's air traffic control:

- **Type the airport's code** - the ICAO code (KORD, EGLL), the three letters
  on a luggage tag (ORD, LHR) or, in the US, the FAA's own id - and press
  **Look up** (or Enter). Its frequencies go into the frequency list as a
  group of their own, named after the airport, in AM at the right bandwidth
  (10 kHz on 25 kHz channels, 6 kHz on Europe's 8.33 kHz channels, whose
  published channel names are converted to the frequency they really are).
  Every one is ticked except ATIS and the weather broadcasts, which talk all
  day long. **Nearest airports** lists the six closest to your receiver
  position. The table is built into FoxSDR - the FAA's 28-day frequency file
  for the United States (O'Hare has 29 VHF frequencies in it, every tower,
  ground, clearance, approach and departure sector), OurAirports for the rest
  of the world - so a lookup needs no internet connection. Both sources are
  public domain, and neither is for navigation.
- **Your own frequencies:** under the airport box, type a frequency in MHz,
  choose AM or NFM, optionally give it a name, and press **Add**. It joins
  the frequency list as a ticked row of the preset named in the **Preset**
  box above it ("Manual" until you change it), so the group list shows it,
  it is marked on the spectrum, and LISTEN plays it with the rest. WFM is not
  offered: broadcast FM is too wide for the monitor. A frequency outside what
  your radio covers is refused, with the range it does cover.
- **Presets:** a preset is a named group of the frequency list. Type or paste
  the path of a CSV or an SDR# frequencies.xml and press **Import** to read
  the file into the preset, its AM and NFM rows ticked whatever the file said
  (a WFM, SSB or CW row is kept unticked: the monitor does not play it). With
  the box empty, Import opens a window with a drop zone and a **Browse...**
  key; a file dropped or chosen there joins the preset, and the window also
  opens, saying why, when a file could not be imported. A bare file name is
  looked for in the recordings folder, where Export CSV writes. **Export CSV**
  writes the preset to the recordings folder as
  `foxsdr-<name>-<time>.csv` - the file Import reads back, ticks included -
  while **Remove preset** takes its rows out of the list. Add and Import show
  the preset they filled unless LISTEN is playing, and Remove preset stops
  LISTEN when it is playing that preset.
- **LISTEN** plays every ticked AM or NFM frequency at once, mixed into one
  speaker. Each channel has its own squelch, so what you hear is whoever is
  talking, and each AM channel is levelled by its own carrier, so a tower 5 km
  away and an aircraft 80 km away play at the same loudness (an NFM channel
  is mixed at a level that matches an AM one). When the ticked frequencies
  span more than your radio's band (an RTL-SDR hears about 2 MHz at a time,
  and O'Hare's span 17 MHz), they are split into blocks the radio can hold:
  the monitor scans between the blocks, stops on a block as soon as anybody
  in it is talking - every channel in that block keeps playing - and moves on
  once the block has been quiet for the **hold** time. **Next block** moves
  on by hand. A block also holds no more channels than the processor is
  asked to carry - ten at 2.4 MS/s, fewer at higher sample rates, since every
  channel works on every sample the radio delivers - so a lower sample rate
  fits more channels in each block. Two ticked rows on one frequency (CTAF
  and UNICOM often share one) are played as one channel. While it plays, what
  is being heard is written in two places: in the section, just above the
  list of frequencies - the block on the air, and **Hearing:** the names
  whose squelch is open ("Hearing: -" while nobody is talking) - and, since
  0.99.71, at the top left of the spectrum, over the channel marks, so an
  airport whose list fills the rail (O'Hare's twenty-nine rows) need not be
  scrolled to see who is talking.
- **Squelch** sets the level a channel must reach to be heard; **Above the
  noise** sets it 8 dB over the quiet channels. Hover a row while listening
  to see its level. A row's lamp lights while it is heard.
- **Which ones are busy:** the time each frequency has been heard is kept
  with it in the frequency list and shown on its row, and **Busiest first**
  sorts by it - measured at your aerial, so after an evening it tells you
  which of an airport's thirty frequencies are worth ticking.

The monitor uses the receiver's radio, so LISTEN switches to the RECEIVER
view; showing the PATCH view, starting the Scanner or tuning the receiver by
hand stops it. Clicking a row tunes the receiver to that one frequency alone.

## Tuning with the mouse on the spectrum and the waterfall

| Gesture | What it does |
|---|---|
| **Click** | Tunes to that frequency, on the mode's step (NFM 12.5 kHz, WFM 100 kHz, AM 9 kHz, the single-sideband, CW and RAW modes 1 kHz). <kbd>Shift</kbd>+click tunes to exactly that frequency. |
| **Drag the shaded band** (spectrum) | Moves the tuned band; <kbd>Shift</kbd> drops the step. Dragging an edge of the band widens or narrows it. |
| **Drag the waterfall** | Pans the picture when it is zoomed in. |
| **Wheel** | Zooms about the pointer, as does <kbd>Ctrl</kbd>+wheel (a touchpad pinch). |
| <kbd>Shift</kbd>+**wheel** | **Slides the tuned band along the band**, one step of the mode (the same step as a click) for each notch; away from you is up in frequency. It works over the spectrum and over the waterfall. Within what the radio is delivering it moves the tuned band, as a click does; past the end of that it moves the radio itself, as the counter does, and the band stays at the end of the picture, so a wheel turned for long enough walks across the whole band. A tuned band that was left off the step by a <kbd>Shift</kbd>+click goes to the next step on its first notch. |
| **Double-click** the waterfall | Tunes to the signal that was clicked and shows the whole span again. |
| **Right-click** | The display menu (and, on the waterfall, the markers). |

While the tuned band is being slid, a zoomed-in picture follows it: if the band would leave the picture, the picture pans, no further than it has to, to keep the band in view. The picture follows for a second and a half after the last notch and stops at once when you take hold of it yourself - a drag of the band, a pan of the waterfall or a zoom. A slide is ignored while you are holding the band or panning the waterfall, and while the Airband monitor is listening (the tuned band is not drawn then, and <kbd>Shift</kbd>+wheel zooms as the wheel does).

## Markers on the waterfall

A marker notes a frequency without tuning to it - something to come back to
later. **Right-click the waterfall**, the receiver's or a patch Spectrum
part's, for the marker menu - on a touch screen, press and hold for about
half a second. Holding the left button still that long no longer tunes the
receiver on release, so a long press only opens the menu:

- **Drop marker here** puts a marker at the frequency under the pointer,
  rounded to what one pixel of the waterfall can show (the menu names it). A
  dashed line runs down the waterfall (and, fainter, through the spectrum
  trace above it) and a tab along its foot reads, for
  example, **M3 145.502**; tabs that would touch stack up to three rows high,
  and past that a marker keeps its line. Right-clicking the same signal again
  does not add a second one. Hover over a marker for its frequency and note.
- **Remove marker M3** appears when the pointer is on a marker.
- **List markers...** opens the Markers window: every marker in frequency
  order with the time it was dropped, a note you can type, and **Tune**
  (tunes the receiver there), **Bookmark** (adds it to the Bookmarks in the
  current mode, named by its note) and **x** (removes it).
- **Copy markers to clipboard** copies them one to a line - number, frequency
  in MHz, time and note, separated by tabs, so they paste into a spreadsheet
  as columns.
- **Clear all markers** asks once more before it removes them.

Markers are numbered M1, M2, ... in the order they are dropped, and a number
is never reused until the list is cleared, so a note that says "M3" keeps
meaning the same frequency. They are one list, drawn on every waterfall that
shows their frequency, and kept in `markers.json` beside the bookmarks, so
they are still there after a restart. Up to 200 can be kept.

## Peak hold and average on the spectrum

**Right-click the spectrum** (the trace above the waterfall, on the receiver
or on a patch Spectrum part) to choose how it is drawn:

- **Normal** - the live trace only, as always.
- **Peak hold** - a bold orange trace of the highest level each frequency has
  reached, held until you reset it, so a short burst stays on screen.
- **Average** - a bold ivory trace of the average over the last
  **Average length** (100 ms to 10 s from the submenu, or anything from 50 ms
  to 10 s on its slider). It averages the power, not the decibels, so a
  noise floor reads where it really is.

The live trace keeps drawing underneath in both. **Reset trace** starts the
peak or the average again; so does retuning, or changing the sample rate,
because a held trace from another frequency would be wrong. The mode and the
length apply to every spectrum and are remembered.

## The range of the spectrum and the waterfall

If the spectrum and the waterfall look milky - the noise floor sitting in the
middle of the colours, so a weak signal does not stand out - the picture is
being drawn through too wide a window. **Right-click the spectrum or the
waterfall** (the receiver's, or a patch Spectrum part's): below the trace
choices, or below the marker list, the menu ends with a **Display** block:

- **Range: Automatic** is the window FoxSDR starts with: -110 to 0 dB on the
  receiver's spectrum and waterfall, and, on a patch Spectrum part, a window it
  fits to the signal on every line (the 20th-percentile bin, which is the
  noise, up to the loudest bin, never less than 30 dB).
- **Range: Manual** uses your own **Floor** and **Ceiling** on all of them
  together: the receiver's spectrum scale, the waterfall's colours, the
  strength key, and every patch Spectrum part. The two sliders are live only
  in Manual. The floor is the level shown as the quietest colour and the
  ceiling the level shown as the strongest, so raising the floor to just under
  the noise - look at where the trace sits - and lowering the ceiling toward
  your strongest signal stretches the colours over what you want to see. The
  ceiling is kept at least 10 dB above the floor: the slider you are dragging
  is the one that stops. The strength key on the waterfall says **MANUAL**
  beside its title while your window is in force, and its **FLOOR / CEILING**
  line names the two numbers.
- **Reset display to defaults** puts back Automatic, forgets the floor and
  ceiling you had, and returns the spectrum to its normal trace.

The two sliders are the same two numbers as **Min dB** and **Max dB** in
SYSTEM → Display (which now shows the window in force, and moves to Manual when
you drag it), so either place changes both. The choice is remembered in the
configuration file (`waterfallRangeMode`, with `dbMin` and `dbMax` for the
floor and ceiling); a file from before the menu that already had a range of
its own opens in Manual. Choosing Automatic again keeps the floor and ceiling
for the next time you choose Manual. The waterfall keeps the colours it has
already painted: after a change the key says **APPLIES TO** the newest lines
only, with a **RANGE CHANGED** rule across the picture, and the older lines
scroll away.

Hold the pointer still over the spectrum or the waterfall for about four
seconds and a small tip says **Right-click to change the range and display**.
Moving the pointer, pressing a button, opening the menu or leaving the panel
puts it away; it comes back after another four seconds of rest.

## The patch view

The patch is FoxSDR's main view (0.99.40): a canvas where a receiver is built
by hand, filling the window where the spectrum and waterfall are otherwise
drawn and resizing with it. FoxSDR opens on it. The **RECEIVER** and **PATCH**
keys under the bank keys at the top of the rail switch between it and the
receiver's spectrum, waterfall and status (so does **SIGNAL PATH → Patch**),
and the view last chosen is the one FoxSDR opens on next time. Showing the
patch starts nothing, and opening on it does not search for radios - the
device lists are read when a Radio's device list is opened, **Look for
radios** is pressed or a Radio part is added. Press a part in the bin at the top and it appears at the
top-left of the canvas you are looking at (pressing again steps each new one
down and across), then wire them port to port. Each part is the instrument
itself, operated on its own face.

| Part | What it does |
|---|---|
| **Radio** | One device of its own - up to five Radio nodes in a patch, all running at once. (This is the patch canvas's own limit; the main window opens up to eight radios, see [Several radios at once](#several-radios-at-once).) Choose the device and its sample rate in the panel on the right and type its centre on the node. A device can be on one Radio only; the list greys out a device another Radio already has. Its **ON/OFF** switch, first on its face, closes that radio alone while the rest of the patch keeps running. A Radio can also play an **I/Q recording**: every 2-channel WAV (16-bit PCM or 32-bit float) in the recordings folder is in its device list (the patch's own speaker recordings are left out), played on a loop in real time. The recording sets the rate; the Radio's centre is the frequency the recording was made at, so type the frequency it was tuned to. Several Radios can play different recordings at once; one recording can be on one Radio only. |
| **Channel** | One frequency out of that capture, tuned, filtered and decimated. Its frequency is typed on its face; its live level is shown above. |
| **Demod** | AM or FM demodulation of a channel, with a **squelch**: on by default at -50 dB, its threshold on a slider and the channel's live level beside it, so a speaker or a recording hears signals rather than the noise between them. Decoders behind it still get every sample. |
| **Speaker** | Where the demodulated channel wired to it goes: a **WAV file** (the default), an **MP3 file**, **the speakers**, or any other sound output - chosen in the panel on the right. Files go in the recordings folder, one per speaker, named after it. |
| **Spectrum** | A live trace and waterfall: of the whole capture (every channel marked and named) when wired to the Radio, of one channel when wired to that channel. Right-click its waterfall for the [marker menu](#markers-on-the-waterfall), its trace for [peak hold and average](#peak-hold-and-average-on-the-spectrum); both menus end with the [display range](#the-range-of-the-spectrum-and-the-waterfall): Automatic (the part fits its window to the signal) or your own floor and ceiling. |
| **Text out** | A log of the lines from every decoder wired to it. |
| **Map** | Aircraft, ships and stations from up to five decoders on one live map - each decoder's map output wired to one of its five inputs, so ADS-B from one radio and AIS from another share it. The same map, basemap and target details as the map pages; with a map imagery plugin fitted, its attribution is lettered under the chart, as on the map pages. |
| **Decoders** | One part per installed decoder plugin, by name. An I/Q decoder wired to a Channel is fed *that channel*, tuned, so several decoders on several frequencies run off one radio at once; wired to the Radio it gets the whole capture. An audio decoder goes behind a Demod. Picture decoders (APT, WEFAX, SSTV) are parts too and show their picture on the node. |

A connection that cannot carry what a port produces is refused while it is
being drawn, and says why. A node that cannot run gets a rust edge and the
reason in words — nothing feeds it, no frequency, outside the band the radio is
receiving, plugin not installed, the plugin needs a faster rate than its source
has, or another radio already uses that device. Nodes close from the key in
their title bar and resize from the grip in their bottom-right corner. A part
that hangs over the edge of the canvas keeps drawing, cut off at the edge. The
patch, with every radio's switch, is saved with the rest of the settings.

**The patch runs from its own START key**, the same round key as the main
panel's, at the top of the page, so nothing needs the main panel. START opens
every radio that is switched on (all of them, if none is) and hands the
receiver's own radio to the patch - the receiver switches to the signal
generator, its decoders stand down and the Source list greys its radios out.
STOP, the large red **ALL OFF** (which also switches every radio off), or
switching to the receiver view stops every patch radio, finishes every file and
gives the receiver its radio and decoders back. Showing the patch view starts
nothing. MP3
uses Windows' own encoder; on Linux the MP3 choice is unavailable and WAV is
written instead. A decoder module that can decode straight from the radio's I/Q
is offered only that way - its audio variant, which needs a demodulator in
front of it, is not shown.

## The new window

*Status: built on the `engine/multi-radio` branch (merged there at 090e7039, not yet released). Everything below was
exercised with scripted input on a hidden desktop and with test radios (the signal generator, I/Q-file radios and a
stand-in RTL-SDR); no second real radio has been used with it and no pass with a real mouse and keyboard on a visible
desktop has been recorded yet. Where a choice is not remembered, or a control only stores a value, the paragraph says
so.*

FoxSDR installs two programs. **`foxsdr-slint.exe`**, the new window, is the one the Start menu, the Desktop icon and
`foxsdr:` links start; **`cascade.exe`**, the classic window, stays installed beside it and has its own Start-menu entry,
"FoxSDR (classic window)". The rest of this manual describes the controls of the classic window in the places where the
two differ; this section is what is particular to the new one. The five banks are the same keys on the rail:
**SIGNAL** (F1), **DECODE** (F2), **TOOLS** (F3), **EXTEND** (F4) and **SYSTEM** (F5), and the tab row above the
spectrum holds **RECEIVER**, **PATCH**, **SCOPE**, the plugins' own pages and **+ Window**.

**The App menu.** Click the brand at the left of the title bar ("FoxSDR" and the version). The menu holds **About
FoxSDR...**, **Key bindings** (F7), **Save a screenshot** (Ctrl+W), **Help: the manual** (opens this manual in your web
browser), **Keyboard shortcuts** (Shift+F1), **Recordings...** and **Quit**. A right-click on the title bar does nothing.

**Keyboard shortcuts and recordings.** The Shift+F1 sheet lists every action in your saved key table with its key.
**Recordings...** lists the recorder's folder, newest first: the kind (I/Q or Audio), the size and the date, with **Open**
on each row, **Open folder** and **Close**. **Open** plays an I/Q take as the I/Q file; any other file is handed to the
program Windows has for it. Neither the clipboard nor the Windows hand-over has been tried against the real thing.

**Layouts.** The **Layouts** key at the right of the title bar, beside the notification bell, opens a menu of
arrangements of the window. A layout holds the bank and which of its sections are open, whether the left panel is
folded, the tab (RECEIVER, PATCH or SCOPE), the split between spectrum and waterfall, the radio layout (One, Side by
side or 2 x 2), Link tuning, which radio is selected and the 3D waterfall setting. It never starts or stops a radio, a
recording, a decoder, a patch or a scan, and it does not remember which radios are open. Two layouts come with the
program: **Airband** (the TOOLS bank with only the Airband section open, the radio layout Side by side) and **HF DX**
(the SIGNAL bank with Source and Radio open, layout One). Choosing one in the menu applies it. **Save the current
layout...** and **Manage layouts...** open the same sheet: a row per layout with **Apply**, and for your own layouts
**Rename** and **Delete**; a **Layout name** field (up to 24 bytes); the tick **Include tuning (the frequency and
mode of the selected radio)**, off by default, which also stores that radio's frequency and mode and re-tunes to them
when the layout is applied; and **Save the current layout**. Saving under a name that exists, in any letter case,
replaces that layout. You can keep 12 layouts of your own. Applying a layout while a patch is running leaves the window
on the PATCH tab and says so. Layouts are saved with the settings. There is no key binding for a layout and the command
palette does not list them. Not yet tried: a layout across a restart of the window, and one applied while a patch is
running.

**Display choices.** At the foot of **SYSTEM > Display** are the choices of the spectrum and the waterfall:

| Label | Choices (default first) |
|---|---|
| **Colour map** | Classic, Deep sea, Ember, Slate, Spectrum, Blue-yellow safe (can be told apart by people who confuse red and green). Changing it clears the waterfall: old lines cannot be recoloured. |
| **Waterfall speed** | Every frame; 10, 4, 2 or 1 lines a second; 1 line every 2, 5, 10 or 30 seconds; 1 line every minute. A slow line keeps the strongest level of every bin since the last line, so a short signal is not lost. |
| **Time stamps** | Age (seconds ago); Clock time, UTC; Clock time, local. |
| **Trace** | Peak hold, Normal, Average, Persistence, Min hold. Persistence draws the last seconds as a fading density. Min hold keeps the lowest level each column has reached and is offered only here and in the right-click menu. |
| **Trace shape** | Line, Filled, Bars. Works with every trace but Persistence. |
| **Peak labels** | Off, 3, 5 or 8 strongest: names the strongest signals on the spectrum and the waterfall with frequency and level, four times a second. |
| **Band zoom** | Full span; one entry for each band of the band plan inside what the radio is delivering; the tuned frequency at 50 kHz, 200 kHz or 2 MHz wide (only widths narrower than the full span). |
| **SNR readout**, **Channel power**, **Frequency tracker**, **Signal history graph** | Four ticks, all off. SNR compares the strongest bin in the passband with the median of the bins outside it. Channel power adds the passband's bins and is relative, not corrected for the window's noise bandwidth; with markers A and B both set it reads the power between them. The tracker shows the offset of the strongest peak from the tuned frequency and its drift over ten seconds: it is not a frequency counter, and a drift of less than about one bin in ten seconds cannot be seen. The graph shows the SNR over the last two minutes. |
| **Save signal history (CSV)** | Writes `foxsdr-signal-history.csv` (utc, peak_dbfs, floor_dbfs, snr_db, power_dbfs) into the `exports` folder beside the settings file. It never overwrites a file. |
| **Save the trace as the reference**, **Show the reference trace**, **Clear the reference trace** | Keeps the current spectrum as a violet line behind the live one, tied to frequency so that it follows zoom, pan and retuning. |
| **Compact window** | Forces the compact receiver strip and radio bar at any width. |

**These choices are the window's, not saved with the settings: the colour map, speed, time stamps, trace, shape, peak
labels, readouts, reference and compact setting return to their defaults the next time FoxSDR starts.** This differs from
the classic window, which remembers its trace mode (see [Peak hold and average on the spectrum](#peak-hold-and-average-on-the-spectrum)).
The same Display section also holds **Digit grouping** (`1.090.000.000` or `1090.000.000`; typing a frequency accepts
either style, two or more dots being grouping marks and one dot a decimal point) and **High contrast** (black grounds,
white lines and text and one yellow accent; it also turns itself on whenever Windows' own high-contrast setting is on).
Whether these rows are cut off in the longest languages has not been swept.

**Right-click menus.** Right-click the spectrum for the **Trace** menu (Normal, Peak hold, Average, Persistence, Min
hold, Reset trace, **Average length** from 100 ms to 10 s, **Persistence decay** Fast, Medium or Slow, then the display
range as described in [The range of the spectrum and the waterfall](#the-range-of-the-spectrum-and-the-waterfall)); right-click the waterfall for the marker menu
([Markers on the waterfall](#markers-on-the-waterfall)). Both end with **Set marker A here**, **Set marker B here** and
**Clear markers A and B**; B's name carries the difference to A, in frequency and in level. **Reset display to
defaults** also puts the trace back to Normal. A bookmark's name drawn on the spectrum is a key: click it to tune there.
Two-finger pinch on the plot is built to zoom about the point between the fingers; it has not been tried on a touch
screen.

**Names of plugin marks.** Marks from plugins are named on the spectrum and now on the waterfall too, stacked left to
right in rows (one to three on the spectrum, two or three on the waterfall); the line is always drawn. A name that fits no
row is counted into a tab marked **+N**; click it for a list headed **Names that did not fit**, and pick one to tune
there. The tip of a name gives its frequency and, when a plugin made it, "from" that plugin's name. No credit for the
data source (HamQTH, say) can be shown yet.

**Export.** **TOOLS > Export** writes the waterfall of the selected radio as a file. The line at the top reads, for
example, "Radio 1 at 100.300 MHz, span 2.4 MHz: 5 min stored, 3.2 MB". **Save to** takes a folder and file name typed in
full; a name with no folder goes into the `exports` folder beside the settings file, the extension is added, and a file
that exists is never overwritten (" (2)" is added). There is no file chooser, and the path, length and format are not
remembered. **Data length** is 1, 5 (the default), 15 or 60 minutes and **Data format** is CSV (the default) or FSW, the
FoxSDR spectrum file, one byte for each level in steps of 0.6 dB. **Save waterfall picture** writes a PNG of the waterfall
as it is drawn, with a frequency scale, a time scale and a caption; the picture holds about the last eight seconds.
**Save waterfall data** writes the levels, in dBFS, of the chosen length: at most 1024 columns, a row every 250 ms, an hour
kept. Only a radio whose pane is shown keeps levels.

**The Demod scope.** The window opened from **TOOLS > Demod scope** has four more keys under the input keys: **WATERFALL**
(the audio spectrum over time), **AUTOCORR** (the autocorrelation of the audio: a repeating pattern is a peak at its
period), **CEPSTRUM** (a voice's pitch shows as a peak at the spacing of its harmonics) and **EYE** (the baseband I
folded on two symbols; there is no symbol-clock recovery, so set TIME until one symbol fills five divisions). A second
press of the lit key returns to the tube. The set of keys is a window setting and is not saved. The pictures are lettered
in capital words in a fixed small type that is not translated.

**Tuning aids.** Right-click the counter for the **Counter face** (Nixie tubes, Neon, Plain), **Show tuner switches**,
**Copy frequency**, **Paste frequency**, **Lock the tuning step** (every digit and the wheel then step by the Tune step),
**Delta lock** (click-to-tune keeps the VFO offset and moves the centre) and **Remember the width of each mode**, all off.
**Enlarge figures**, **Normal size** and **Enlarge every reading** are in the same menu and are saved but do not yet make
anything larger. A **padlock** key above the counter, left of the RCVR lamp, locks the frequency of the selected radio:
the counter, wheel, click-to-tune, slider, bookmarks, scanner and presets sent from this window are refused and the
counter says "Frequency locked". The lock stops this window only: a plugin, the web page, CAT or another client can still
tune. On the airband, 118 to 137 MHz, the counter's caption names the 8.33 kHz channel ("Channel 118.005").
The **status bar's clock** at its right end reads UTC by default; a press changes it to local time and then to none, and
the choice is saved.

**Scanner and memories (TOOLS).** In the Scanner section: **Mute while scanning** silences the receiver while the scan
steps and lets you hear it when it stops on a signal; **Log what the scan finds** writes one line a signal, with the UTC
time, the frequency in hertz and the seconds heard, to `scan-log.csv` in the settings folder (the file itself has not been
inspected); during a scan of ticked bookmark rows **Lock out 10 min**, **Lock out for good** and **Clear lockouts** are
live (they are not offered in a range scan). **Priority MHz**, **Every s** and **Scan threshold dB** are only saved: the
scanner does not use them yet, and the window says so. Below the bookmark list, a tuned bookmark has a memory of
**Squelch dB** (sent when the receiver comes onto the bookmark with no scan running), **Scan threshold dB** and **Tone Hz**
(both stored only), which the bookmark file and its export do not carry. **Store into a memory** and the eight keys M1 to
M8 keep and recall a frequency, mode and width. These fields are kept in `lanec.json` beside the settings file.

**Pages.** The decoder output bar has **Copy**, which puts every held line on the clipboard. A plugin's picture zooms with
the wheel from 1x to 16x about the pointer, pans by dragging and has a **Fit** key while zoomed. **Save as BMP** writes into
a `pictures` folder beside the settings file, not into the recordings folder, and greys while it writes. A map page has
**Trails** and **Altitude colours** ticks and a **Line** or **Ribbon** choice (with **Trail width** and **Trail style** also
under SYSTEM > Display). A plugin's setting that names a file has **BROWSE FILES** beside it. Dropping a `.wav` on the window
opens it as the I/Q file, dropping an `.xml` or `.csv` imports it as a frequency list, and any other file is answered with
"That file is neither a recording nor a frequency list." (A real drag from Windows has not been tried.) **SYSTEM >
Diagnostics > Copy diagnostics** puts the diagnostics bundle on the clipboard. When the SDRplay driver has trouble reaching
its service, the Source section shows **RESTART SDRPLAY SERVICE**; Windows asks permission before it runs, and it has not
been run against hardware.

**Not built.** Spoken announcements, band stacking, mode presets, the SDRplay diagnostic dialog, controller knobs and
gamepads, and sending the symbol rate to plugins do not exist in this build.

**Narrow windows.** Several radios at once need a window of at least 1280 x 720; in a smaller one a multi-pane layout is
kept but one radio is shown, with a notice. At 800 pixels wide the strip, tab row and title bar are wider than the window
in 20 to 30 of the languages.

## Several radios at once

*Status: built on the `engine/multi-radio` branch (merged there at 090e7039, not yet released); the screen is in the
new window only ([The new window](#the-new-window)). Proof: eight test radios (the signal generator) ran together for the
engine's measurements, and a B200 and an RTL-SDR ran together for 30 minutes with the sound at zero. Nobody has listened
to several radios at once, and no other real radio has been tried.*

**How many.** The main window opens up to **eight** radios at once, numbered Radio 1 to Radio 8. That limit counts every
open radio, including the radios of a patch: the ninth is refused with "at most 8 radios can be open at once", followed by
who holds the others. A given physical radio can be open on one radio at a time ("in use by Radio 2"); any number of the
eight can be signal generators. **The patch canvas is a separate limit:** a patch holds at most **five** Radio nodes (see
[The patch view](#the-patch-view)).

**Opening a second radio.** The **Add a radio** key at the right of the status bar, or the same card drawn in an empty pane,
offers: **Signal generator**, each radio found that no other radio holds, and boxes for **I/Q file** (an I/Q file path),
**rtl_tcp** (host and port) and **ADALM-Pluto (network)** (an address), each with an **Add** key, then **Sound card**. **Look
for radios again** searches again; **No other radio found.** says there is nothing to add. A radio added from the card
starts running, on 100.000 MHz. The sound card belongs to the first radio, so the engine refuses it as a second radio and
the card shows its words. The first radio you add beside radio 1 switches the layout to Side by side. A network radio that
cannot be reached is added without a sentence about why.

**Removing one.** Radios 2 to 8 have a close key at the right of their pane's header ("Remove Radio n"). A stopped radio
goes at once; a running or recording radio asks first ("This radio is running. Removing it stops it and lets go of its
device." or "...is recording. Removing it ends the recording..."). Radio 1 cannot be removed. The plugins of a removed radio
go back to radio 1 and are stopped.

**The layout.** With more than one radio open, a bar appears over the receiver view: **Layout** with **One** (today's
single receiver view), **Side by side** (two panes) and **2 x 2** (four panes), and, when a radio lives beyond the first
page, page keys such as **Radios 1-4** and **Radios 5-8** (in Side by side the pages are pairs). Never more than four panes
are drawn. A pane holds a header (the radio's coloured tag with its number, the device, the frequency, the mode, the lamps
RUN, DEC, MUTE and FAIL, a STOP or START key, **Link** and the close key), a row of readouts (GAIN, SQL, VOL, RATE, BIAS,
the decoder, the span, PEAK IN PASSBAND), a compact spectrum and waterfall and a splitter between them that is
remembered for each radio. A click on a pane's tag, header or readouts selects that radio, as does a right-click; click,
drag, wheel and the other pointer actions on a pane act on that pane's radio. A window narrower than 1760 pixels carries the
radio selector, the STOP keys and the MASTER lamps in this bar; a wider one carries them at the left end of the receiver
strip (**Controls follow** with **R1** to **R8**, **STOP R**n, **STOP ALL** or **START ALL**, and one MASTER lamp for each
open radio: green running, red failed, dark stopped). The banks (SIGNAL, DECODE and the rest), the gain, antenna, sample-rate
and decoder lists follow the **selected** radio.

**Keys.** **Ctrl+1** to **Ctrl+8** select a radio; **Alt+PageUp** and **Alt+PageDown** turn the page of radios (in layout One
they select the neighbouring radio). They are rows of the key table, group Radio, and can be changed like any other. A radio
that is not open is left alone.

**Link tuning and Start together.** **Link tuning** (off by default) makes the open radios move together: a change of any
linked radio's frequency, by click, counter, key, bookmark or another window, is copied to the others by the same number of
hertz, so the gaps between them are kept. A radio added while it is on joins; the **Link** key in a pane header, shown only
while Link tuning is on, takes that radio out and puts it back; a pane whose radio could not follow goes red. At a device's
tuning limit the radio stays where the device lets it. **Start together** starts every open radio with one command.

**The sound.** The foot of the sections panel carries **Audio mix** while several radios are open: for each radio a key
**Output of Radio n** that cycles **Both**, **Left**, **Right** and **None** (default Both), **MUTE**, **SOLO** ("Hear only
the radios marked SOLO") and a volume slider. Each radio plays through its own stream to the first radio's output device and
Windows mixes them. Solo is not saved: a restart clears it.

**What each radio keeps.** Its own source and device, sample rate, antenna, frequency, mode, bandwidth, VFO offset, squelch,
volume, mute, FM de-emphasis, stereo, noise reduction, AGC, noise blanker, notch and audio route, and whether it was
running. Gains and device options belong to the device and come back when that device is opened again. Shared by all
radios: the bookmark list, the band plan, the recordings folder, the one scanner (it works on the radio it was started on),
the Airband monitor, plugin settings and the sound card. Radios 2 to 8 are saved with the settings and restored in order;
one whose device is missing or busy stays on the generator with its settings and says why in its pane ("RTL-SDR did not
open: ..." or "Radio 3: in use by Radio 2"). **An older version of FoxSDR reads radio 1 and forgets radios 2 to 8 the next
time it saves.**

**Decoders.** With more than one radio open each plugin's entry in DECODE > Decoders carries a **Runs on** row of keys, one
for each radio ("The radio this plugin listens to. Choose another to move it there."). The default is radio 1; the choice is
saved; a plugin that transmits stays on radio 1. 43 installed plugins were placed on radio 2 in a test and four real decoders
decoded their own recordings on radios 2 to 5; no plugin has run on a second real radio.

**New audio and noise controls.** The SIGNAL bank's Audio filters section has **AGC** (Auto, Fast, Medium, Slow, Off, with
**Fixed gain** from -20 to +40 dB live only in Off), a **Noise blanker** tick with a **Threshold** slider (6 to 30 dB,
default 15) and **De-emph** (50 us, 75 us or off, live in NFM and WFM only). All three apply to the selected radio. Auto is the
original loop; Fast, Medium and Slow differ in how quickly the gain recovers after a loud signal. These were proved on
generated signals only and nobody has listened to them.

**Not built, or without a control.** The engine also has a tone squelch (CTCSS and DCS), S-meter units and calibration, a
choice of noise-reduction method, a bank of ten notches, a CW pitch, an FM noise squelch, an equaliser, timed recordings, a
recording file-name template and macros; **none has a control in either window**, and they are reachable only through the
settings file or a program that talks to the engine. They are not described here as features.

## Language and country

The interface is available in 34 languages: English, Български, Català,
Čeština, Dansk, Deutsch, Ελληνικά, Español, Eesti, Suomi, Français, Hrvatski,
Magyar, Bahasa Indonesia, Italiano, 日本語, 한국어, Lietuvių, Latviešu, Norsk
bokmål, Nederlands, Polski, Português (Brasil), Português (Portugal), Română,
Русский, Slovenčina, Slovenščina, Svenska, Türkçe, Українська, Tiếng Việt,
简体中文 and 繁體中文. **SYSTEM → Language & country** chooses one;
**Automatic**, the default, follows the language Windows (or, on Linux,
`LC_MESSAGES`/`LANG`) is set to, and falls back to English when there is no
translation for it. A change applies at once. The 33 translations are machine
translations and say so in the list (the Japanese one was checked against a
second independent translation and a reviewer); **SUGGEST A BETTER
TRANSLATION** opens the bug-report page with the language already filled in.

Latin, Greek and Cyrillic letters are drawn from the program's own fonts
(Georgia or Saira, with an embedded Noto Sans subset for what they lack).
Chinese, Japanese and Korean use the fonts the operating system provides -
Microsoft YaHei, JhengHei, Yu Gothic and Malgun Gothic on Windows, Noto CJK on
Linux - and a language whose script no installed font can draw is shown as
unavailable in the list, with the reason, rather than as empty boxes.
Right-to-left and Indic scripts are not offered: the interface library draws
text without the shaping those scripts need.

Choosing your **country** in the same section picks the band plan for it - the
country's own plan where FoxSDR has one (UK, US, Canada, Australia, Japan), the
ITU region's plan everywhere else. Nothing about either setting leaves your
machine.

What is not translated: text a plugin draws itself (plugins ship their own
words), device and product names, units, and the names of modes and protocols.

For translators: the catalogues are `resources/lang/<code>.json`, keyed by the
English text. `py -3.14 tools/i18n_keys.py` reports what each one is missing,
`--skeleton <code>` starts a new language, and a catalogue can be tried without
rebuilding by starting FoxSDR with `FOXSDR_LANG_FILE=<path to the json>` and
`FOXSDR_LANGUAGE=<code>`. `py -3.14 tools/embed-lang.py` compiles the catalogues
into the program; `test_i18n` checks every catalogue covers every string, keeps
every `%` conversion in order, and `test_i18n_glyphs` that the fonts can draw
every letter.

## Keyboard

Every shortcut below can be changed: open **Key bindings** in the **SYSTEM**
group of the rail (or press <kbd>F7</kbd>, which goes there and opens that row),
click the key you want to change, then press the keys you want it to be.
<kbd>Esc</kbd> leaves it alone and <kbd>Backspace</kbd> clears it, so an action
can be left with no key at all. **Reset to defaults** puts the whole table back.

Only the keys you change are stored, so a later version's improved default
still reaches you if you never touched that one.

**No shortcut fires while you are typing** — a frequency in the counter, a
bookmark name, a password — and none fires while a dialog is open.

| | Key | |
|---|---|---|
| Start / stop the receiver | <kbd>Ctrl</kbd>+<kbd>F2</kbd> | |
| Mute the sound | <kbd>M</kbd> | |
| Volume up / down | <kbd>+</kbd> / <kbd>-</kbd> | 5% a press |
| Squelch up / down | <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>PgUp</kbd> / <kbd>PgDn</kbd> | 2 dB a press; switches AUTOMATIC squelch off |
| Mode: NFM | <kbd>Ctrl</kbd>+<kbd>F</kbd> | |
| Mode: WFM | <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>F</kbd> | |
| Mode: AM | <kbd>Ctrl</kbd>+<kbd>A</kbd> | |
| Mode: DSB | <kbd>Ctrl</kbd>+<kbd>D</kbd> | |
| Mode: USB | <kbd>Ctrl</kbd>+<kbd>U</kbd> | |
| Mode: CW | <kbd>Ctrl</kbd>+<kbd>C</kbd> | |
| Mode: LSB | <kbd>Ctrl</kbd>+<kbd>L</kbd> | |
| Mode: RAW | <kbd>Ctrl</kbd>+<kbd>R</kbd> | |
| Tune up / down one step | <kbd>Ctrl</kbd>+<kbd>↑</kbd> / <kbd>↓</kbd> | 10 kHz |
| Tune up / down one screen width | <kbd>Ctrl</kbd>+<kbd>PgUp</kbd> / <kbd>PgDn</kbd> | by the visible span |
| Type a frequency | <kbd>T</kbd> | opens the counter's editor |
| Zoom the spectrum in / out | <kbd>Ctrl</kbd>+<kbd>+</kbd> / <kbd>Ctrl</kbd>+<kbd>-</kbd> | about the centre |
| Zoom back to the full span | <kbd>Ctrl</kbd>+<kbd>Del</kbd> | |
| Record the audio | <kbd>Shift</kbd>+<kbd>R</kbd> | the I/Q take stays mouse-only |
| Transmit | <kbd>Space</kbd> | HELD, and only while the TRANSMIT page has focus |
| Save a screenshot | <kbd>Ctrl</kbd>+<kbd>W</kbd> | |
| Maximise / restore the window | <kbd>F11</kbd> | |
| Bank: SIGNAL PATH / DECODE / TOOLS / EXTEND / SYSTEM | <kbd>F1</kbd>…<kbd>F5</kbd> | |
| Settings and key bindings | <kbd>F7</kbd> | |

The transmit key is the one entry that behaves differently from every other:
it is **held** rather than pressed, it is read only while the TRANSMIT page has
focus, and it does nothing anywhere else in the application. A bare
<kbd>Space</kbd> is safe as a transmit key for exactly that reason — and if you
rebind it, it is still only read on that page. See [Transmitting](TRANSMITTING.md) for the rest
of the rules about when a radio may be keyed.

Two keys are **not** in the table and cannot be rebound. <kbd>F12</kbd> always
saves a screenshot — it is the key every note and every test script tells you to
press, and an instruction that can be rebound is not an instruction — and
<kbd>Esc</kbd> always cancels what is being typed, or leaves the full-screen
scope.

The list is HDSDR's where FoxSDR has the same control to offer, with three
deliberate differences. HDSDR starts and stops on <kbd>F2</kbd>; here
<kbd>F1</kbd>–<kbd>F5</kbd> are the five FUNCTION SELECT keys engraved on the
rail, so start/stop takes <kbd>Ctrl</kbd>+<kbd>F2</kbd>. HDSDR has one FM and
this receiver has two, so narrow FM keeps <kbd>Ctrl</kbd>+<kbd>F</kbd> and wide
FM takes <kbd>Shift</kbd> with it. And HDSDR's DIG and ECSS modes do not exist
here, so <kbd>Ctrl</kbd>+<kbd>D</kbd> is DSB and RAW takes its own initial on
<kbd>Ctrl</kbd>+<kbd>R</kbd>.

## Browser access

FoxSDR can serve its interface to a browser, so the receiver can live where the
antenna is. Open **Web access** in the **EXTEND** group of the rail, set a
password, tick it and press Apply; the section then shows the address to open
on another machine.

Read the security posture before exposing it:

- **It is off by default, and defaults to loopback** (this machine only).
- **A password is required for any binding beyond this machine.** Without one,
  a LAN bind is refused outright — no socket is opened at all, rather than a
  socket opened without a gate.
- **Plain HTTP unless you switch HTTPS on** (see [HTTPS and listeners](#https-and-listeners)
  below). The default server links no crypto library of its own; on a home LAN
  plain HTTP is a reasonable trade. **Do not port-forward it to the internet,
  with or without HTTPS: read the warning in the next section first.**
- Passwords are stored hashed (PBKDF2-HMAC-SHA256), never in the clear;
  sessions are cookie-based, expire, and are revoked whenever the settings
  change.

The browser client does everything the desktop does, with two deliberate
exceptions: it cannot name an I/Q file or a recording directory, because those
are host filesystem paths rather than settings.

The toolbar has a **Start**/**Stop** key for the receiver and a **Mute** key
for the audio. **Mute** is the same mute as the window's MUTE key, and it
shows **Unmute** while it is on, also when the mute was set at the desk, because
the page reads the state back from the receiver. The MUTE lamp lights for it and
for a decoder that is silencing the audio.

The page does SAY which plugins are stopped, so it never claims output from one
that is switched off, and pressing a stopped plugin's **preset** there starts
it, just as pressing it in the window does. A preset press is the same
unambiguous "I want this plugin now" wherever it comes from, and the alternative
— retuning the receiver for a decoder that is switched off, or silently doing
nothing — would be worse from a page that cannot explain itself. When a decoder
is silencing the audio the toolbar also carries a **muted by *plugin*** badge —
a browser hearing nothing has no other way to tell a muted radio from a dead
stream.

**Live updates.** The page asks the receiver to push its status when it changes
(server-sent events, plain HTTP, no extra software) instead of asking four times
a second. Up to four browsers can hold the stream at once; a fifth, a browser
without the feature, or a proxy that buffers it, is not a problem: the page
notices the stream is not there and goes back to asking, then tries the stream
again every ten seconds. While the stream is up the page still asks twice a
second, only to tell the receiver the page is alive (a held transmit key depends
on that). The stream needs the same sign-in as the rest of the page, and signing
out, an expired sign-in or stopping the web server ends it.

## HTTPS and listeners

*Status: built into the web server and proved with tests (HTTPS on Windows
only; on Linux the switch is refused with a sentence that says it is
unproven there). The **Web access** section of the window and the
configuration file do not carry these switches yet: they arrive with the
settings screen described in `docs/feature-records/web-secure.md`. Every one
is **off** by default.*

**A plain warning first.** Putting this page on the internet lets strangers
find a radio that sits behind your home connection and try passwords at it all
day. HTTPS hides what is sent from people on the path; it does **not** make a
weak password strong, it does not hide that the page exists, and it does not
make FoxSDR a hardened public service. A self-signed certificate protects only
as far as you check it. If you open the page to the internet anyway: use long
passwords, give listeners the smallest cap you can live with, leave chat off,
and prefer a tunnel or VPN into your own network to a forwarded port.

**HTTPS.** With HTTPS on the page is served over TLS using Windows' own
Schannel: TLS 1.2 at least, TLS 1.3 where Windows offers it; TLS 1.0, 1.1 and
SSL are refused. The plain server is then bound to this machine only, so plain
HTTP is no longer offered to the network at all, and the sign-in cookie is
marked Secure. By default FoxSDR makes a self-signed certificate (an elliptic
curve key that cannot be exported, with this computer's name and every address
of it in the certificate); the page's status shows its **SHA-256 fingerprint**,
and your browser will warn about an unknown certificate the first time: compare
the fingerprint it shows with FoxSDR's before you accept. Instead you can name a
certificate you already have, by the 40 digits Windows shows as its thumbprint,
from the Personal store of your account or of the computer (it needs its private
key, and the key must be in one of Windows' own software providers: a smart card, a token or a TPM-backed key is refused, because it could put a PIN
dialog on the screen in the middle of a connection; FoxSDR only looks at the certificate list, never changes it). A client that does not finish the handshake,
says nothing for half a minute, or stops reading is dropped.

**Listeners.** With listeners on, there are two kinds of people on the page. You,
the **owner**, sign in with the account the page always had and control
everything. A **listener** types any name of their own choosing (up to 24 letters,
digits, spaces, dots, dashes or underscores) with the one shared **listener
password** you set, and may only look and listen: the spectrum, the status and the
sound. Every operation that tunes or changes anything is refused, including the
transmit key; one switch, **listeners may tune**, lets them change frequency, mode
and bandwidth and nothing else. A listener is not shown where your antenna is, the
folder recordings go to, device serial numbers or plugin file names.

- **Limits.** At most 4 listeners by default (1 to 32); a listener who does
  nothing for 15 minutes, or has been on for 8 hours, is signed out. Each
  listener holds one audio stream and one live-update stream of the server.
- **Moderation.** The owner sees who is listening, with the address each came from
  (shown live to you and nowhere else), and can **kick** (end the session; they may
  come back), **ban** (by name and by address; ends the session and refuses
  them) and **allow** again. A ban never stops the owner signing in. Bans, like
  everything below, last until the web server is stopped or restarted: addresses
  are not written to disk.
- **Note.** The owner can set one line of text that every listener sees as a banner.
- **Chat.** Off by default. When on: text only, 200 characters, three lines every
  ten seconds per person, shown exactly as typed (never as markup). Chat is
  never written to a file, a log or a report, and neither are names or addresses;
  the owner's moderation view keeps a short in-memory trail of joins, leaves,
  kicks and bans that carries the listener's chosen name and never an address
  or anything anyone typed.
- **Not built:** a listener tuning on their own, independently of the owner and
  of other listeners. That needs a receiver chain and an audio stream per listener;
  everyone listens to the same radio.

## Network features and what they send in the clear

*Status: built on the `engine/multi-radio` branch (merged there at 090e7039, not yet released). Every server below is
**off** by default and, once switched on, listens on this machine only until you say otherwise. No real browser, Hamlib
program or TCI program has been run against this build; the behaviour described is what the code and its stand-in tests
do.*

**A plain warning first.** None of the servers and clients in this section encrypts its traffic in this build. The page in [Browser access](#browser-access) is plain HTTP in this build (the HTTPS switch described in
[HTTPS and listeners](#https-and-listeners) does not exist yet). Anyone who can see the traffic between FoxSDR and the
other end, on the same Wi-Fi, the same switch or any router in between, can read what is listed in the table and, for the
servers that accept commands, can send the same commands themselves. The safe uses are **this machine only (127.0.0.1)**
and **a LAN you trust**. Do not forward any of these ports from your router to the internet, and do not switch a server to
listen on every interface on a network you do not control. A password protects only the web page; the CAT, TCI and UDP
audio ports have none.

| Feature | What it sends or accepts, with no encryption | Default | How it is switched on |
|---|---|---|---|
| **Web page** ([Browser access](#browser-access)) | HTTP. Everything the page shows: tuning, spectrum, audio, decoded plugin text, bookmarks, and, to the owner, the antenna location, the recordings folder and radio serial numbers. The sign-in password travels in the clear in the login request, and the session cookie can be copied by anyone on the path (it is marked Secure only with HTTPS, which has no switch). | Off; address 127.0.0.1, port 8073 | **EXTEND > Web access**: tick **Serve a browser page**, then **Reachable from** (**This machine only** or **Every network interface**), **Port**, **User name**, **Password**, **Confirm**, and press **Apply**. A bind beyond this machine without a password is refused: "set a password before allowing access from other machines". |
| **CAT (Hamlib rigctld protocol)** ([CAT control](#cat-control)) | Text lines: the receiver's frequency, mode and bandwidth to anyone who asks, and it **accepts commands that retune the receiver**: centre frequency, VFO offset, mode and bandwidth. A request to transmit is answered as receive and refused. **There is no password.** At most 4 connections. | Off; 127.0.0.1, port 4532 | **EXTEND > CAT**: tick **Accept CAT connections**, set **Port**; tick **Reachable from other machines** to listen on every interface. With that ticked the window says: "This protocol has no password. Anything that can reach the port can retune the receiver." |
| **TCI server** (TCI 2.1 over a plain `ws://` WebSocket) | The receiver's state (frequency, mode, filter, squelch, start or stop) and, when a client asks, **the receiver's audio**. It accepts start, stop, centre frequency, VFO offset, mode, bandwidth and squelch. Every transmit command is answered false. **No password.** The code was not found to check which program or web page is connecting (not tested), so a web page open in a browser on this same machine might be able to try it. At most 4 clients. | Off; 127.0.0.1, port 50001 | **No control in the window.** Settings file only: see below. |
| **Audio over the network (UDP)** | The audio of one radio as raw 48 kHz, 16-bit, mono datagrams to **one** host and port, with nothing read back. Whoever can see the path to that address can listen. | Off; sent to 127.0.0.1, port 7355, radio 1 | **No control in the window.** Settings file only: see below. |
| **rtl_tcp radio** (a client) | FoxSDR connects out to an rtl_tcp server you name and sends it tuning and gain commands; the server returns raw 8-bit I/Q. Both directions are plain TCP with no password. | Nothing is contacted until you press **Open** | Source list entry **rtl_tcp**, box **rtl_tcp host and port** (port 1234 if left out). |
| **ADALM-Pluto radio** (a client) | FoxSDR connects out to the Pluto's own service on port 30431 over the network. Control and I/Q travel in the clear. | Nothing is contacted until you press **Open** | Source list entry **ADALM-Pluto (network)**, box **Pluto address**, for example `ip:192.168.2.1`. |

**Switching on a server in the settings file (TCI and UDP audio).** These two have no switch on any screen. They are keys in
`%APPDATA%\foxsdr\config.json`, read when FoxSDR starts; edit the file with FoxSDR closed, because the program writes the
file itself when it saves. Whether a hand-edited key survives in the installed program has not been confirmed on a real
installation; the engine's tests restore these keys from a settings file the engine wrote itself.

| Key | Default | Meaning |
|---|---|---|
| `tciEnabled` | `false` | The TCI server on or off. |
| `tciBindAll` | `false` | `true` listens on every interface instead of 127.0.0.1. |
| `tciPort` | `50001` | 1024 to 65535. If another program holds the port, the server reports that it cannot listen and the setting stays on. |
| `udpAudioEnabled` | `false` | The UDP audio sender on or off. |
| `udpAudioHost` | `"127.0.0.1"` | Where the datagrams go. An empty value resets to 127.0.0.1. Any other address is a deliberate choice to send your radio's audio across the network. |
| `udpAudioPort` | `7355` | The destination port. |
| `udpAudioRadio` | `1` | Which radio, 1 to 8. |
| `udpAudioPacketFrames` | `480` | 480 (10 ms) or 960 (20 ms) samples in a datagram. |

A settings bundle imported from another machine always arrives with the web page, CAT, TCI and UDP audio switched **off**:
the program says "was imported as OFF; switch it on again yourself if you want it". The operation that starts a listener is
refused to a remote, web, TCI or CAT session, so a connection cannot switch another server on. No real TCI program has been connected to this server, so whether a particular logging or
digital-mode program works with it is **not known**.

**A KiwiSDR as a radio.** The engine can open a KiwiSDR receiver over the network as audio only, but no control in the
window offers it, so it is not described further here.

**Plugins have their own network connections.** The host gives a plugin no network access; a plugin that needs one opens
its own socket and encrypts nothing unless its page says so. Read a plugin's page before you switch one of
its network outputs on.

## How it looks

FoxSDR is styled as a 1960s bench receiver: a brass and dark-enamel instrument
with amber counter digits, ivory legends and phosphor-green displays. That is
not only a skin - the colours carry meaning, and the rule is worth knowing
before you use the application:

- **Ivory letters a control.** If it is written in ivory on brass, it is
  something you operate.
- **Amber is a reading.** The tuned frequency, the range, the status chips down
  the rail - anything amber is a number the receiver is reporting.
- **Phosphor green is what the radio actually heard.** The spectrum trace, the
  waterfall, the radar scope and the demod scope's beam are all the same tube.
- **Rust is trouble**, and never a reading, so a fault can never be mistaken
  for a figure.

The controls live on the **FUNCTION SELECT** rail down the left, and the rail
is a bank selector, the way a bench instrument's is: five brass pushbuttons
under its title - **SIGNAL**, **DECODE**, **TOOLS**, **EXTEND**, **SYSTEM** -
one of them pressed in with a phosphor strip lit beneath it, and the column
below showing that bank's sections and nothing else. SIGNAL is the receiver
itself (source, radio, audio filters, sinks, the recorder); DECODE is the
plugin store, the fitted modules, the decoders, target details and the
satellites map; TOOLS (called VIEW before 0.99.74) is the demod scope, the
radar scope, bookmarks, the scanner and airband; EXTEND is browser access and
CAT control; SYSTEM is the **Display** settings (interface size, theme, the
spectrum's range - also on the right-click menu of the spectrum and the
waterfall - the band plan), language, updates, diagnostics and usage
reporting. **F1 to F5** press the same five keys from
the keyboard. Each section still opens and closes with its own key and
keeps that state as you move between banks; a section unfolds rather than
appearing, a bank comes up like a lamp rather than switching in one frame, and
the rail opens on whichever bank you left it on. **Display** is the first
section of SYSTEM and starts folded; the configuration file records whether
you left it open or folded, so it comes back that way, and every other
section comes back at its usual state on the next launch. Every chip on a row
still reports what that section is doing without opening it.

**Both side panels fold away.** The `<<` key at the right end of the
FUNCTION SELECT title folds the rail to a thin strip, and since 0.99.74 the
`>>` key at the left end of the STATUS column's title does the same for the
column on the right, for a user who would rather have the whole width for the
spectrum than the cards beside it. The patch page or the spectrum takes the
width that frees, and the one key on each strip brings its panel back. The two
are separate, and both are remembered (`railCollapsed` and `statusCollapsed` in
the configuration file). The STATUS column belongs to the receiver view, and a
window too narrow for the full column drops it, but its folded strip needs so
little room that it stays. The REQUEST A FEATURE and REPORT A BUG / DISLIKE
keys are part of that column: they are not shown while it is folded, and come
back when it is opened.

**Sinks**, in SIGNAL, is where the sound goes: the output device, the reason
when there is no sound ("Muted by ..."), and, since 0.99.73, the **Audio
buffer** - how much sound FoxSDR holds back before it plays. A fuller buffer is
what lets the speakers ride out a computer that is busy for a moment; the price
is that what you hear is that much behind the radio. The choices are
**AUTOMATIC** (the default), **120**, **240**, **480** and **960 ms**, and the
value in force is shown in the box ("AUTOMATIC  240 ms").

- **AUTOMATIC** starts at 120 ms, as FoxSDR always did, and watches the sound
  card. When a minute closes with the card having run out of audio three times
  or more, the buffer is made one step deeper (120, 240, 480, 960 ms, and never
  beyond 960), and FoxSDR says so in the **AUDIO - UNDERRUNS** card, under the
  setting ("Audio buffer raised to 240 ms: this computer fell behind 5 times in
  the last minute.") and in the log. **Coming back down is much slower than
  going up** (since 0.99.83): only after fifteen minutes in a row in which sound
  was playing and the card never once ran out does the buffer come back one
  step ("Audio buffer lowered to 240 ms after 15 quiet minutes."), and each
  further step needs a quiet quarter hour of its own - so one bad spell still
  buys lasting protection, but no longer a permanent delay. Minutes in which
  nothing is playing prove nothing and do not count. **The depth it reached is
  remembered**: the next launch starts AUTOMATIC at that depth (never below
  120 ms, never above 960) instead of paying the same bad minutes again, and
  says so under the setting ("Audio buffer restored to 480 ms: the depth this
  computer needed when it last ran.") and in the log - it is not a "raise",
  nothing has just happened, but a launch that starts half a second behind the
  spectrum should never be a mystery. When the buffer steps down, the
  remembered depth steps down with it. Choosing AUTOMATIC again, from one of
  the fixed values, starts from 120 ms once more and forgets the remembered
  depth.
- **A fixed value** is the buffer from the moment it is chosen and never changes
  by itself. It is remembered between sessions, and a remembered AUTOMATIC depth
  is ignored while a fixed value is in force (and kept, in case you go back).
  Choosing a shorter buffer drops the oldest queued sound on the next playback
  callback, so the shorter delay takes effect promptly; you may hear one jump.
- **What a deeper buffer costs.** The new depth is used when the sound next runs
  out and starts again, so the step is paid for once, as about that many
  milliseconds of silence at that moment; and everything you hear is that much
  later than the radio. A speaker on the patch page that is a sound card has a
  buffer of its own and follows the same setting.

FoxSDR also asks Windows to schedule its signal-processing threads ahead of
ordinary programs (the Multimedia Class Scheduler, class "Pro Audio"; on Linux a
round-robin priority where the system allows it), so that the thread making the
sound is not kept waiting by a busy machine. The first lines of the log say how
that came out (`dsp: thread priority mmcss`). Neither change is a promise about
any one computer: a machine that is too slow still is, and the buffer is what
gives it room.

The **Demod scope**, under TOOLS, is the other instrument on the bench, and it
is deliberately not drawn like the radar one. That is a plan-position
indicator: a round tube, a rotating sweep, a long-persist phosphor. This is a
rectangular service oscilloscope — ruled ten divisions by eight, minor ticks
five to a division down the two axes, a short-persist beam and a dark glass
between sweeps. It opens as its own window, so it can sit beside the spectrum
rather than replacing it.

Four keys across the top choose what is on the tube. **AUDIO** is the
demodulated audio, triggered on a rising zero crossing so the waveform stands
still; the hold-off that makes that work on speech scales with the time base,
which runs 1, 2, 5, 10, 20 and 50 milliseconds a division. **SPEC** is the same
audio as a spectrum, 0 to 20 kHz (or the sink's Nyquist, whichever is lower) at
10 dB a division. **I/Q** is the channel at the demodulator's input, before it
has been turned into one real number, as an I trace above the axis and a Q
trace below it. **VECTOR** plots that same I against Q, which is the one
picture that makes the modes look different from each other: an FM carrier
draws a circle, AM a line through the origin that breathes with the audio, SSB
a figure that never sits still.

The attenuator runs 2 mV to 1 V a division on the same 1-2-5 detents, and
**AUTO** ranges it a detent at a time to keep the signal between about a third
of the tube and the top of it. Silence holds the setting where it is rather
than winding all the way in — a scope that made the noise floor leap up the
screen every time the transmission stopped would be reporting something that
did not happen. Nothing on the tube is drawn when the receiver is stopped: the
graticule stays and the glass says so, because a flat line at zero volts is a
measurement, and there was not one.

The **Radar scope**, under TOOLS, is drawn from the receiver's own position —
every mark on it is a range and a bearing from the antenna — and until that
position is set there is nothing to draw. Rather than leave two zeroed cells
on an empty panel, the Radar section and the scope's empty state both offer
two places the application already knows: the middle of the aircraft it has
heard, once there are three of them, and the centre of the map a plugin is
showing. Either key sets the receiver and the scope draws on the same frame;
the exact figure can still be typed, or taken from a click on a map page. One
pair is refused at every door, typed or offered or read from the config file:
0 N 0 E, which is a point in the Gulf of Guinea and the value every empty
field holds, and which one receiver was found measuring from.

The scope's range ladder runs 10, 25, 50, 100, 200, 400, 800 and 1600 NM. The
top two are not for hearing further — nothing on this band is heard past
about 300 NM — but for placing: a region, then a continent, under the traffic.
They are possible because the ground under the face is drawn in the scope's
own polar projection, each map tile cut into cells and every corner placed by
the same pair of functions that places an aircraft, so a coast and the
contact over it sit on the same pixel at any range. Until 0.78.0 the ground
was laid on in Mercator, matched at the middle and about 9% out at the edge
of a 400 NM picture, which is why 400 was the ceiling.

**No title bars.** The operating system used to draw its own strip above the
main window and above every page torn out of it — a white bar in another
decade's style, with the desktop's minimise, maximise and close. They are
gone. The cabinet's own top rail carries the window's name, engraved between
the screws, and three brass keys at its right; the rest of the rail is what
the window is dragged by, a double-click on it fills the screen, and a
right-click offers the system menu. On Windows the main window keeps every
convenience a framed one has — snapping to an edge, the keyboard's window
shortcuts, resizing from its edges — because only the *caption* is taken off
the window's style: it is still an ordinary resizable window as far as the
system is concerned, and the system is told which part of it is the rail. Every
page is drawn the same way, as a cabinet with its name and keys on the rail;
a page inside the main window rolls up to its rail when minimised and fills
the main window when maximised, and a page torn off to its own window does
both to the desktop, with a taskbar button to come back from. Since 0.88.2 no
page can be dragged smaller than 240 x 140, which is the size below which its
body stopped being drawn at all and left a rail with nothing under it, and
**RESET WINDOW SIZES** in the Fitted modules window puts every decoder, panel,
instrument and map window back to its default size and position - for the Decoder
output window and a picture window, the middle of the main window. On Linux the
main window keeps the frame the desktop gives it, and the rail carries the
name alone.

Since 0.79.0 the main window's rail drags the window by two routes. Where the
system's hit test accepts the rail as the caption, the system moves the window
itself, with snapping and the shake gesture. On one laptop it did not, and the
window could not be moved at all; the application now also catches the press
on its own side and moves the window through GLFW while the rail is held, so
the window moves whichever route a machine takes. The first time the second
route engages, a line saying so goes into the diagnostic log, which is what to
send with a report of a window that will not move.

Every one of those lives in `src/gui/theme.hpp`, in one palette named by role.
Two things it deliberately does NOT govern, because they are measurements
rather than decoration: the waterfall's colour ramp, which is built to keep a
stronger signal always brighter than a weaker one, and the map's altitude
bands, which are a scale an operator reads.

The lettering is part of the same rule. Since 0.84.0 the bench is lettered in
**Georgia** — Regular for anything a hand operates and for all prose, Bold for
the engraved captions — read from the Windows font directory at start-up,
because Georgia is Microsoft's typeface and licensed with Windows rather than
for redistribution, so it cannot travel inside the executable. On a machine
without it (Linux, or a Windows with the font removed) both roles fall back
together to the embedded **Saira Condensed**, so no window ever mixes the two
pairs; the diagnostics log says which pair loaded. Counter digits stay in
**Nova Mono** — monospaced so the frequency stops shuffling sideways as it
changes, and because Georgia's numerals are old-style and proportional, which
on a counter is exactly the dance an instrument's glass must not do. Saira and
Nova Mono are SIL Open Font License faces, embedded unmodified;
`third_party/THIRD_PARTY.md` records exactly which release each one is. Nova
Mono is used for figures and not for words, which is measured rather than
fussy: its capital M is three close stems and rasterises as a solid block
below about 20 pixels. The browser interface names Georgia the same way and
falls back to the served Saira.

The sizes live in one place, `src/gui/fonts.hpp`, and 0.79.0 raised every one
of them by three pixels (controls and prose 21, engraved captions 19, readings
20, the smallest engraving 17) because the panel was still hard to read at
arm's length. The rail's column grew eight pixels with them so that every
shipped word still fits beside the widest status chip; a test measures each
one against the real typefaces, so a word that stops fitting fails the build
rather than clipping on the panel.

## The satellites map

A plugin that reports satellites gets a **window of its own**, and that window
is the whole instrument. It opens from its key in the **DECODE** group of the
rail — one key per such plugin, lettered with that plugin's own name and
carrying its live target count — and there is deliberately nothing
satellite-shaped anywhere else in the main window: the key is a switch that
puts the instrument on screen, never a second, smaller copy of its controls.
With no satellite reported yet the key reads **NONE** and says which of the two
reasons applies — nothing installed that publishes satellite positions, or
something installed that has not reported one yet. Those are different
problems, and only one of them is answered by a trip to the plugin store.

Inside, in one panel: the receiver's position, with the coordinate cells and
the key that applies them; **MAP OVERLAYS** — coverage, ground tracks and
altitude colours, each a rocker with its own caption; **TRAIL STYLE** as LINE,
RIBBON or OFF; **COVERAGE** with a RESET and a note saying what the ring
actually holds; the **TARGETS** register, sorted by CALLSIGN, NORAD, ALT or AGE
in either direction, with a card for the selected target; and the map itself.
**FIT** centres on everything plotted and stays visibly dead when nothing is —
a key that would silently do nothing is worse than one that says it cannot.
**WHOLE WORLD** backs out until the whole planet is in the window, which is the
way out of a fit onto a single target. While a target is being followed the
strip says **FOLLOWING** with its id, in gold, and offers **STOP FOLLOWING**;
when none is, it says outright that the map moves on its own only while a
target is followed. It remembers the rectangle it sat in, and nothing but your
own hand puts it on screen: since 0.79.1 it does not come back at a restart,
and within a session it no longer opens itself when a plugin that had nothing
plotted starts reporting again. A propagator reports a full sky on the first
frame of every launch, with no radio and no action from you, which is exactly
the window that must never appear by itself.

The map here is drawn **equirectangular**, not Web Mercator, and the trade is
deliberate: the poles are on screen and a polar orbit reads as the sinusoid it
really is, at the price of basemap tiles — a Mercator raster under an
equirectangular graticule would put every coastline in the wrong place, which
is worse than having no imagery. The built-in Natural Earth coastline is drawn
instead. Every other map page is untouched by this and still uses tiles when a
basemap plugin supplies them.

**What it will not show you, and why that is not a gap to be fixed.** Every
figure on this window comes from something the application actually has. The
plugin ABI carries a track's id, label, position, altitude, course, speed and
the age of the fix, plus polylines — and that is the whole of it. So there are
two different kinds of missing here, drawn differently on purpose:

- **Recoverable.** DISTANCE and BEARING are blank — hatched, never zeroed —
  until you set the receiver's position, and a note beside them says so.
  Setting one fills them in, and the coverage ring with them. A bearing to a
  target directly overhead stays hatched even then: there is no direction to
  it, and 000 would be read as north.
- **Not reported, ever.** **INCLINATION**, **ORBITAL PERIOD**, the **age of the
  element set** and **next-pass predictions** are not in the plugin ABI at all.
  A track source reports a position, not the elements that position was
  computed from, so no receiver position and no amount of clicking will produce
  them. The window says that in words rather than leaving a blank a user would
  keep poking at. An earlier draft of this panel printed four such numbers and
  every one of them was invented; they are gone.

The coverage ring is worded the same way, because it had the same problem: it
records how far out a target has been **plotted** on each of 72 bearings, from
every plugin, and a satellite's position is normally *computed* rather than
heard — so it does not claim to be a record of what the antenna received.

## Privacy

FoxSDR reports anonymous usage counts. **Usage reporting**, in the **SYSTEM**
group of the rail, is
**on by default** — untick it and reporting stops. It sends counts only:
version, operating system, session length, which modes and plugins get used,
which radio model, how many times the window froze because the display
driver was waiting (a bare number, nothing about the freeze itself), and — since
0.99.64 — how many times something quietly did not work: a radio that would not
open, no sound output, an update or a plugin install that failed, and — since
0.99.65 — how many times the window was slow (a frame of a quarter of a second or
more) and which part of it was (`rail`, `spectrum`, `saves` - FoxSDR's own names
for its parts), and how many times FoxSDR met a fault at one of a fixed list of
places and carried on (the sound output restarted, a radio reopened, a settings
file that could not be written) - as counts of a fixed list of kinds and never any
message, name or path, and only while Diagnostics is on as well. All against a
random identifier created on your machine. Switching it off deletes that
identifier.

**It never sends frequencies, anything decoded, your location, or your IP
address**, and hardware serial numbers are stripped before the radio model is
sent. The complete list of what is and is not transmitted is in
[PRIVACY.md](../PRIVACY.md), the payload is held to it by an automated test, and
the receiving endpoint's source is in [telemetry-worker/](../telemetry-worker/).

**Everything else that reaches the network, in full.** Apart from the usage
report above, these are the only things this application sends or fetches, and
each says what starts it:

- **The update check.** Once per launch, to foxsdr.com, sending the version you
  are running and nothing else — no identifier and no cookie. It is on by
  default and is the **Check for updates at startup** tick in **SYSTEM →
  Updates**; nothing is downloaded or installed unless you press the button.
  It is not the usage report and the two share nothing.

  **A message from the FoxSDR team** (since 0.99.79). The same answer may carry
  a few lines written when something is wrong with the software or the service,
  or when no one is there to answer. They are shown as a card, headed
  **Message from the FoxSDR team**, at the top of the menu column, above the
  update banner and whether or not there is an update — red when it is marked
  important, amber otherwise. Press **Seen** and it does not come back; **Not
  now** on the update banner does not hide it, and **Seen** does not hide the
  banner. SYSTEM → Updates says how many unseen messages are waiting. The ids
  of the messages you marked **Seen** are kept in `config.json` on this
  computer only, and are forgotten once the service stops sending that message
  (so one that is withdrawn and later sent again is shown again). Nothing is
  sent to receive a message, none is shown with the tick off, and a copy from
  the Microsoft Store never receives one: it asks the Store, not foxsdr.com.
- **The plugin catalogue.** Fetched when you open the Plugin store window
  (the first time in a session, since 0.99.16) and whenever you press
  **CHECK NOW** in it. Nothing is fetched at startup — since 0.79.1 that
  window does not reopen by itself — and no plugin ever updates itself.
  Since 0.99.47 the same moment also asks foxsdr.com for a short list of
  plugins offered only to some countries; it answers from Cloudflare's
  country code for your connection, stores nothing, logs nothing, and is not
  asked for when you have pointed the store at a different catalogue. See
  [PRIVACY.md](../PRIVACY.md).
- **A plugin site's catalogue**, only if you added one (**Plugin sites**, above).
  A site is an address *you* chose, and it is read at the same two moments as the
  catalogue — when you open the store (the first time in a session) and whenever
  you press **CHECK NOW** — after it, one site after another, each with a few
  seconds to answer. The site receives your IP address and the path of its
  `index.json`, as any web server does, and nothing else from FoxSDR: no
  identifier and no cookie (the request names its client as
  `cascade-plugin-repo/1.0`, the same for every copy). Nothing is fetched at
  startup and nothing
  from a site is installed unless you press **GET** or **UPDATE** on one of its
  plugins.
- **A plugin download**, when you press **GET** or **UPDATE** for
  one — always https, sha256-verified against the catalogue, size-capped and
  refused on a cross-host redirect. For a plugin from a site you added, the file
  comes from that site's own host and is verified against the sha256 that site
  published.
- **A plugin's pictures**, when you open its page in the Plugin store: each is
  fetched once from the address the catalogue gives, over https, checked against
  the catalogue's sha256 and a 4 MiB cap, and kept under `store-cache` in the
  plugins folder. The request carries that address and nothing else; a plugin
  from the regional list shows none, and a plugin from a site you added shows
  only pictures served by that site's own host. See [PRIVACY.md](../PRIVACY.md).
- **A crash or freeze report**, and only with Diagnostics on: the report's text
  goes out the *next* time you open the application, never from inside the
  fault. See [When it crashes or freezes](#when-it-crashes-or-freezes).
- **A feature request**, only when you press SEND on the REQUEST A FEATURE
  page: your text, an optional contact line, and the version/platform/
  architecture you are running - never automatically, never retried, never
  queued. See [PRIVACY.md](../PRIVACY.md) for the complete field list.
- **A bug report or a dislike**, only when you press SEND on the REPORT A BUG /
  DISLIKE page: the same fields as a feature request plus which of the two you
  chose, on the same terms. See [PRIVACY.md](../PRIVACY.md).

A plugin is native code loaded into this application's own process, so an
installed plugin may make requests of its own — a basemap plugin fetching map
tiles from the server you pointed it at is the ordinary case. That is the
plugin's traffic, not the host's, and it is why each module's page says
plainly that a fitted module runs with every privilege the application has,
with no sandbox and no permission model, and that its declared capability list
is what the maker says the module PROVIDES rather than a limit on what it can
take.

## When it crashes or freezes

FoxSDR records faults **on your machine**, and — with Diagnostics on — sends the
report's *text* the **next** time you open it. Never from inside the crash: a
program that has just failed cannot safely use the network, so the file waits on
disk until there is a healthy process to send it. **A memory dump is never sent,
under any setting**, and the switch that stops the writing stops the sending too.
[PRIVACY.md](../PRIVACY.md) lists the payload field by field, and an automated test
holds the request, the code and that document to each other in both directions.

- A crash writes a report to `%LOCALAPPDATA%\FoxSDR\crashes\` naming the fault,
  the faulting module and offset, the loaded modules, and the last 256 log
  lines.
- A **freeze** does too, which matters more: every fault this product has
  actually shipped was a hang rather than a crash, and a crash handler catches
  none of those. If the window stops responding for five seconds, a watchdog
  captures the stack of **every** thread — a deadlock is only legible as a pair
  — and then lets the application carry on if it recovers.
- **A machine that cannot keep up no longer freezes the interface.** Until
  0.96.4 the panel read its settings - the tuning offset, the sample rate, the
  channel width - through the same lock the audio chain holds while it is
  working, several times per drawn frame. On a computer fast enough that never
  showed; on one that was not, the display queued behind the audio and a user
  with a slow USB dongle got a freeze report against an application that was
  working exactly as designed. Those readings no longer wait for anything.
- **And the device search can no longer take FoxSDR down with it.** Looking for
  radios runs in a small separate process precisely because a driver falling
  over there must not end the session - but the note FoxSDR wrote about each
  such death used to be written in a way that could, and twice was, fatal to
  the survivor. It now records what the search process died of and nothing that
  requires walking a stack.
- **A stalled DISPLAY is not a stalled application, and 0.96.4 stops reporting
  it as one.** Switching a monitor off, changing resolution, a GPU driver reset
  or a remote session reconnecting all freeze presentation inside the graphics
  driver, and on one user's AMD machine that lasted over five seconds - long
  enough for the watchdog to file a report about a program that was doing
  nothing wrong. FoxSDR now stands the watchdog down for ten seconds when it
  sees the display change, and while its window is minimised (a stall excused by
  either is excused for at most 30 seconds since 0.99.61, so a frame loop that
  genuinely stops there is still reported; a minimised window whose loop keeps
  turning never is); and if a stall gets past both, the report is marked `stall` rather than `hang`, kept on
  the machine, and never counted among faults in FoxSDR (since 0.99.62 only
  how many there were goes into the anonymous usage report, and only while both
  usage reporting and Diagnostics are on). What it is **not** is
  "ignore anything that looks like presenting": a report from 0.96.2 had the
  same call on top and was a dead radio service, which is a real fault and is
  still reported.
- **The update button no longer files a report about its own consent prompt.**
  Starting the downloaded installer asks Windows for elevation, and that dialog
  waits for you - which 0.96.2 recorded as a five-second freeze. Every place
  FoxSDR hands something to Windows (the installer, the reports folder, the
  privacy-policy link) now tells the watchdog it is deliberately waiting - and
  that is the one wait with no time limit, because what it waits for is you
  reading the prompt. A shell call that never returns and shows no dialog is,
  by design, still not reported.
- **A freeze can no longer be excused for ever** (0.99.61). Two of the reasons
  the watchdog accepts for a pause in drawing had no end: a pause FoxSDR takes
  itself (a plugin rescan, opening the sound output or the microphone, the
  display change above) and a thread sitting inside Windows' own window code
  (`win32u.dll`, which is also where a graphics driver's waits and a message
  sent to a window nobody answers end up). A real freeze that began during
  either was never reported. Each now excuses a stall for at most 30 seconds.
  Checked in FoxSDR's tests against a real watchdog on a shortened clock; no
  freeze of that kind was reproduced on a real session.
- **The window no longer waits on the settings folder** (0.99.61). Once a second,
  in every session, FoxSDR checked its settings folder for a link request from
  the beta-tester page, on the thread that draws the window, and a report
  (`hang ntdll.dll @ __std_fs_get_stats`) showed that thread stuck there. The
  check now runs on a background thread and nothing waits for it; the log says
  once when the folder has not answered for five seconds and once when it does.
  Why that user's folder was slow is not known. Two other places that ask the
  disk from that thread were not covered by that change: the sizes shown in the
  Fitted modules window, which 0.99.63 fixed (below), and the plugin rescan,
  which 0.99.63 only measures and sometimes skips (below).
- **Closing counts as well, on a longer clock.** Shutting down is where this
  product's worst freeze ever happened, so the watchdog stays armed right
  through it — but closing legitimately waits on the radio driver for a few
  seconds, so the teardown is given twenty rather than five. Until 0.75.0 it
  was judged by the same five, and a slow but perfectly healthy close could
  file a freeze report against an application that had already exited. The
  report says which clock it was measured against.
- **A freeze while closing, ended from the taskbar, is counted as a crash**
  (0.99.61). FoxSDR used to record "closed normally" before it had finished
  shutting its plugins down, so a session that froze inside a plugin's own
  shutdown and was ended from the taskbar looked like an ordinary close on the
  next start - one user's bundle read no crash in 79 launches. The record is now
  made after the plugins have stopped. A fault in the very last step of closing,
  the graphics teardown, is still not counted as unclean; the watchdog, stopped
  last, is what covers that.
- **A freeze is grouped by FoxSDR's own code** (0.99.62). A frozen window is
  nearly always parked in the same few Windows wait routines whatever it is
  waiting for, and a freeze report's grouping signature was taken from that
  wait, so freezes from different causes shared one signature. The upload sends
  one report per signature per day, so the second kind of freeze in a day was
  set aside on the machine and never arrived. The signature is now taken from the
  first frame of FoxSDR's own code on the frozen thread. What a report contains,
  and what is sent, does not change, and a freeze report written by an earlier
  version keeps the signature it was written with. Checked in FoxSDR's tests with
  a real watchdog and real blocked threads; no freeze of this kind was
  reproduced on a real session, and the Linux path was not compiled.
- **One failure of the radio search is one report** (0.99.62). When the separate
  process that looks for radios died, one death could file up to three reports -
  the process's own, and two from FoxSDR about the same death - and so use three
  of the five uploads a machine may send in a day, for a fault the application
  survived. The process's own report is now the one whenever it wrote one, and it
  says on its `reason` line which search it was, which try it was and that
  FoxSDR survived; no field is added to what is sent. FoxSDR files its own report
  only for a death that process could not report itself (a heap corruption, a
  driver ending the process itself, a death before the process had set up its own
  report). Checked on Windows against the real program and test drivers that are
  made to fail; the Linux path was not compiled.
- **A report describes the radio that was in use** (0.99.62). The receiver's
  state in a report or a diagnostics bundle - source, sample rate, mode, radio
  model, loaded plugins - was refreshed once a second, so a fault in the first
  second after a radio was opened described the radio before it. It is now
  refreshed on every frame and at the moment any of those changes. A new
  `patch-radios` line lists the radios the patch page is running by driver kind
  only (`rtlsdr`, `soapy`, ...), never a name, a serial number or an address; it
  is in the report file on your machine and in the bundle, and it is not part of
  the report that is sent automatically. Whether this clears the field faults
  that prompted it is untested.
- **Pressing Record no longer waits for the disk** (0.99.63). A freeze report
  from 0.99.58 had the window stopped for more than five seconds inside the
  call that creates a recording's folder and file, on the thread that draws the
  window; the Record IQ and Record audio buttons, the Record key and the web
  remote's record controls all made that call. The file is now opened in the
  background: until it exists the button reads **Starting IQ - click to cancel**
  (or **Starting audio - ...**), after a second a line under it says how long
  the disk has been taking, and Stop, or a click on that button, withdraws the
  start (the file is closed when it arrives and no recording begins). A failed
  open is reported in the same words as before, a moment later. The log says
  once when an open has been out for five seconds and once when it returns,
  naming neither the folder nor the file. Checked in FoxSDR's tests with a
  stand-in for a slow disk and a real freeze watchdog, not on a real slow disk.
  **Not covered:** a recording started by a speaker on the patch page still
  creates its file the old way, on the window's thread.
- **A plugin reload is skipped when nothing in the plugins folder changed, and
  says where its time went** (0.99.63). Fetching the plugin catalogue used to
  unload and load every plugin again whether or not anything had changed, on the
  thread that draws the window. It now compares one listing of the plugins
  folder (each file's name, size and last-write time) with the one taken for the
  last completed reload and skips the reload when they are the same; the log
  says so in one line that names nothing. Anything else - a file added, removed,
  renamed or replaced, a retirement rule that changed, a plugin the last reload
  refused, no earlier reload to compare with - reloads as before, and the
  Rescan key, an install, an update and a removal always do. Every reload that
  runs now writes one log line with the seconds each of its seven steps took
  (a reload of five seconds or more is a warning naming the slowest step), with
  no plugin name, file or path. **Not fixed:** a reload that is needed still
  runs on the window's thread with no limit and can still hold the window for as
  long as a plugin takes to stop. One 0.99.58 session froze for two minutes
  inside one, and what held it is not known; the timing line, and the freeze
  report written for a reload that runs past 30 seconds, are there to find out.
  Checked in FoxSDR's tests against the real window and real plugin modules
  that record when they are loaded and unloaded, with a catalogue read from a
  local file; not over the network, not against a slow plugin.
- **The Fitted modules window no longer asks the disk for sizes** (0.99.63).
  While it was open it read the size of every plugin's file on every frame, on
  the thread that draws it, and a slow or sleeping disk or an unreachable share
  held the window on one slow answer. The size is now taken once, when the
  plugins are scanned, and the window draws that: a file replaced by hand shows
  its old size until the next reload. Checked in FoxSDR's tests, one of which
  holds the drawing function to no file-system call at all; not seen on a real
  slow disk.
- **A freeze report names a module loaded after start-up** (0.99.63). A frame in
  code mapped after the module list was last refreshed - a graphics driver the
  system reloaded, a vendor DLL, a shell extension - used to print as a bare
  address, so a report could not say where the thread was and the display-stall
  rule could not recognise a graphics driver it had never seen: that freeze was
  filed as a hang instead of a stall. Such a frame is now named by file name and
  offset like any other, and a freeze inside a late-loaded display driver is
  kept on the machine as a stall. What is sent for such a frame is the file's
  name (never a path) and the offset, where it was an empty module and an
  address; PRIVACY.md says so. Windows only. Checked in FoxSDR's tests with a
  real watchdog and a DLL loaded after the module list, not on a real driver
  freeze.
- **An ending nothing inside FoxSDR could report is now written up by a watcher**
  (0.99.64). A fast-fail, a heap corruption or a stack overflow runs no handler; a
  window frozen so hard that the watchdog is stuck too cannot report itself; one
  ended from the taskbar never gets the chance; and a death during start-up comes
  before any of it is armed. Those reached us as a bare count of unclean exits, with
  no cause, and no way to tell a crash from a PC that was shut down. With
  Diagnostics on, an interactive session now starts a second copy of its own program
  with no window - the *sentinel* - that does nothing but wait for the first to end.
  It is given the right to wait for FoxSDR and read its exit code, and a 64-byte page
  of numbers FoxSDR keeps up to date (the stage it is in, whether a radio or plugins
  are being opened, when it last drew a frame); it reads none of FoxSDR's memory and
  takes no dump. When FoxSDR ends it writes one report in the usual folder - or none
  at all if FoxSDR ended normally or already reported the death itself - saying the
  exit code in words (access violation, fast-fail, heap corruption, stack overflow),
  the stage, how long it had run and how long the window had been silent, with the
  end of the log. A crash exit code, a window that had stopped drawing, and a death
  before the first frame are sent like any other report; **an ending from outside
  while the window was drawing (Task Manager, `taskkill`) and one as Windows closed
  the session are kept on your machine and never sent**, so a healthy FoxSDR you end
  that way cannot use up the five reports a day a crash needs. The sentinel sends
  nothing itself, is not started with Diagnostics off (and is ended if you switch it
  off), leaves within a second of FoxSDR ending so the installer can replace the
  program, and if it cannot start or dies FoxSDR carries on and says so once in the
  log. Starting it costs the program about a millisecond. Checked on Windows against
  real processes that really die and against the real program; **the Linux half
  compiles only against stand-in declarations and has never run** (it can learn that
  FoxSDR has gone, never how, and says so in the report), and a real log off or
  shutdown, a real Task Manager *End task* and the Microsoft Store package were not
  exercised. The design, the decision table, every fixed sentence and what is
  proven are in [docs/DIAGNOSTICS.md](DIAGNOSTICS.md) ("The sentinel"); what a
  report carries is in [PRIVACY.md](../PRIVACY.md), and a test holds the two to the code.
- **On Linux, an ending before the window appears now says how the process ended
  and which window system was used, and you can choose the window system**
  (0.99.78). A report from FoxSDR 0.99.75 on Ubuntu 24.04 (an AppImage) said only
  that it ended "while creating the window", with no exit status and no word from the
  window library, so nobody could tell a crash from a refused connection to the
  display. The watcher on Linux cannot ask the operating system how FoxSDR ended
  (it is not FoxSDR's parent), so FoxSDR now writes it down itself as it ends - the
  status of an `exit`, or the number of the signal that ended it - and the report
  says `exit code 1 (the process ended itself)` or `killed by signal 11 (SIGSEGV)`.
  An ending by `SIGKILL` (the out-of-memory killer, `kill -9`) cannot be written
  down by the process that is being killed, and the report says that it looked and
  found nothing. The log also gets one line per step of opening the window, each
  beginning `window:`: which window systems this build has, whether `DISPLAY` and
  `WAYLAND_DISPLAY` are set (set or not - never their values), which one was chosen,
  the window library's own error messages, and the graphics driver's name and version.
  **If FoxSDR ends before its window appears on Linux**, start it with
  `FOXSDR_WINDOW_PLATFORM=x11` (or `wayland`) in front of the command, for example
  `FOXSDR_WINDOW_PLATFORM=x11 ./FoxSDR-0.99.78-x86_64.AppImage`, and send the lines of
  the log (`~/.local/state/foxsdr/logs/foxsdr.log`) that begin `window:`. `x11`,
  `wayland` or `any` (the default - the window library chooses); anything else, or a
  system this build does not have, is noted in the log and ignored. Checked in
  FoxSDR's tests on Windows - the lines, their order, the value of `DISPLAY` kept out,
  and a value that names nothing ignored with the run still starting - and the Linux
  side against stand-in declarations only: **none of it has run on Linux**, and whether
  forcing a window system cures that report is not known. Details in
  [docs/DIAGNOSTICS.md](DIAGNOSTICS.md) ("If FoxSDR ends before its window appears").
- **On a Windows computer with no working graphics driver** - none installed, a virtual
  machine without 3D, some remote desktop sessions - FoxSDR (0.99.90 and later) starts
  anyway: when the graphics driver offers no usable OpenGL, or one older than 3.0, it
  starts itself again on the software renderer it carries in its `softgl` folder (Mesa,
  drawn by the processor). The window looks the same and works the same; it costs more
  processor time, so on a slow computer the picture may not be as smooth. Installing the
  graphics card's own driver is still the better fix. To choose for yourself, set the
  environment variable `FOXSDR_GL` before starting FoxSDR: `software` always uses the
  software renderer (useful if a graphics driver crashes), `hardware` never does. The
  log's `window:` lines say which one drew the window. Details in
  [docs/DIAGNOSTICS.md](DIAGNOSTICS.md) ("When the graphics driver cannot be used").
- A rotating log lives in `%LOCALAPPDATA%\FoxSDR\logs\foxsdr.log`. Since
  0.89.0 it records what the **radio driver** says as well as what FoxSDR
  does: SoapySDR's own log is bridged in (lines beginning `soapy:`), and in a
  windowed session the standard error stream — where librtlsdr and UHD print
  warnings such as `rtlsdr_read_async: dev_lost` — is captured into it (lines
  beginning `vendor:`). A radio going quiet is diagnosed from those lines and
  nothing else, and until now they went to a console nobody was reading. Both
  are limited to twenty lines a second, serial numbers are stripped and the
  digits of any line that mentions a frequency are masked before a line is
  kept, and a developer running from a terminal keeps their stderr untouched.
  Since 0.99.33 every line - FoxSDR's own as well - is scrubbed again as a
  report or a diagnostics bundle is put together: serials, USB device
  instance ids, account names in paths, quoted names and any unlabelled
  number on a line about a frequency are masked, and libusb's listing of the
  machine's USB devices is left out (PRIVACY.md has the exact rule).
  A crash report also now says how long the session had been running
  (`uptime-sec`) and whether the thread that faulted was one of FoxSDR's own
  or one a driver created (`fault-thread-own`), and the uploaded copy carries
  at least 80 log lines wherever it carries any.
- A driver fault that FoxSDR absorbed reopens the radio once: the same device
  is opened again at the same rate, tuned back, its gains and antenna put
  back, and started again if it was running — and if that reopen fails the
  radio stays closed with the reason on the Source panel, and nothing tries
  again for a minute.
- Each report that has been sent — or could not be — carries a small `.upload`
  file beside it saying which, in plain words. At most five reports a day leave
  one machine, and the same fault only once a day, so a machine stuck in a crash
  loop stops sending on its own.
- **SYSTEM → Diagnostics** on the rail shows both paths, has the on/off switch, and has
  **Copy diagnostics**: one click that puts the version, commit, operating
  system, loaded plugins with versions, source and device state, the sound
  output's state, the volume, whether the sound is muted and by what, the squelch
  threshold and whether it is open (since 0.99.61 - never the sound card's name
  or a frequency), the recent log, and - since 0.99.62 - the end of the
  previous session's log and one line per crash or freeze report on the
  machine (what it was, when, and what became of sending it), on your clipboard,
  so you can read it before you send it to anybody. Off
  means off — no directory, no file, and off from the first instruction the
  application runs rather than from its first frame: the switch is read before
  anything is armed or created.

What a report contains is listed field by field in [PRIVACY.md](../PRIVACY.md) and
held to that list by an automated test. It never contains a frequency, anything
decoded, or your position. A full memory dump is off by default, written locally
if you switch it on, and never sent by the application.

The engineering side — how a handler writes a report from a broken process,
how the five-second threshold is derived and measured, and where the symbols
that make a report readable are kept — is in
[docs/DIAGNOSTICS.md](DIAGNOSTICS.md).

## Installing

Download `foxsdr-setup-<version>.exe` from the releases page and run it. It
installs `cascade.exe`, the SoapySDR runtime, the app-local Microsoft C runtime,
the licence and post-install notes; it writes nothing outside the install
directory and your own user profile, and it uninstalls cleanly from
Add/Remove Programs.

Your settings, the plugins you install and the caches and logs live in two
folders in your profile: `%APPDATA%\foxsdr` and `%LOCALAPPDATA%\FoxSDR`.
Recordings are not among them; they go to `Documents\SDR-recordings` and are
never touched by setup. The installer offers **a clean install** ("Delete
existing FoxSDR settings, plugins, caches and logs first", unchecked by
default) for reinstalling over a profile that has gone wrong, and the
uninstaller ASKS whether to remove those two folders, so leaving and
upgrading are not the same thing. For a scripted install or removal:
`/CLEANINSTALL=1` on setup, `/CLEANDATA=1` or `/KEEPDATA=1` on the
uninstaller; an unattended uninstall keeps your data unless it is told
otherwise, because that is the command an upgrade runs.

**The setup executable is not code-signed yet**, so Windows SmartScreen will
show "Windows protected your PC". Choose **More info → Run anyway** if you are
happy to proceed. Signing is planned.

**An RTL-SDR, a HackRF, an Airspy R2/Mini, an Airspy HF+, a Mirics MSi2500 or
an RX888 mk2 needs no extra install at all** - FoxSDR drives those six itself,
over its own WinUSB transport. The one step Windows requires is binding the
radio to WinUSB with Zadig, which every SDR application needs and which
`cascade.exe --rtlsdr-check` will tell you about for a dongle. An **SDRplay
RSP** needs the SDRplay API 3.x from sdrplay.com and nothing else - no SoapySDR
module - because SDRplay publish no device protocol and the tuner is programmed
by a Windows service. An **ADALM-Pluto** needs nothing installed at all: FoxSDR
speaks to the board's own daemon over the network. So does an **rtl_tcp**
server: FoxSDR is the client, and the server runs wherever the dongle is. Any OTHER radio - a USRP, a
LimeSDR - still reaches FoxSDR through SoapySDR vendor
modules, which are a separate install (PothosSDR or radioconda); see
`POSTINSTALL.txt` in the install folder. FoxSDR runs with no hardware at all
using the signal generator or I/Q playback.

FoxSDR locates either of those in its default location and adds it to the
SoapySDR module search path itself; `SOAPY_SDR_ROOT` overrides the guess and a
`SOAPY_SDR_PLUGIN_PATH` you set by hand is appended to, never replaced. **An
RTL-SDR dongle needs one further step on Windows**: it ships bound to the DVB-T
television driver, under which it is invisible to every SDR application, and
Zadig must be used to bind WinUSB to "Bulk-In, Interface (Interface 0)" instead.

**Since 0.99.88 FoxSDR does that step itself.** When the **RADIO SETUP** page
finds an RTL-SDR on the television driver, or on no driver at all, it shows an
**INSTALL DRIVER** key under that radio. Press it and Windows asks once for
permission; FoxSDR then gives the dongle's radio interface (interface 0 - never
the whole dongle and never its remote-control interface) Windows' own WinUSB
driver, the Microsoft-signed one Windows already carries, and checks the radios
again. Nothing is downloaded, no certificate is added to the computer, and no
other device is touched: the installer checks the device's own hardware ids
before it changes anything. If Windows says a restart is needed, or the dongle
is not listed straight away, unplug it, plug it back in and press **CHECK
AGAIN**. Saying no to the permission prompt changes nothing. The Zadig
instructions stay on the page beneath the key for anyone who prefers them.

**To undo it** - to use the dongle as a television receiver again - open Device
Manager, find the dongle's interface under *Universal Serial Bus devices*
(*WinUsb Device*), choose **Uninstall device**, then unplug and replug it:
Windows puts back the driver it would have chosen itself.

This covers RTL-SDR dongles only so far, the radio most people start with; a
HackRF, an Airspy, a Mirics or an RX888 is still bound with Zadig as below.

An **RX888** is two devices. Plug it in and run Zadig once for
**"WestBridge"** (USB ID `04B4:00F3` — the FX3 bootloader), then open it once
in FoxSDR so the firmware loads: the radio disappears and comes back as
**"RX888mk2"** (`04B4:00F1`). Run Zadig again for that one. Both need the
**WinUSB** driver. If you only do the first, FoxSDR will report that the
radio took the firmware but did not come back; if you only do the second, it
will not see the radio at all until something else has loaded its firmware.
(Tick *Options → List All Devices* in Zadig if either one is not in the list.)

A **Mirics** receiver needs the same one-off binding, and most of them arrive
as something other than a radio. Choose the entry whose USB ID is `1DF7 2500`
(or `1DF7 3000` / `1DF7 3010` for an early SDRplay RSP1 or RSP2, `2040 D300`
for a Hauppauge WinTV 133559 LF, `07CA 8591` for an AverMedia A859, `04BB 0537`
for an IO-DATA GV-TV100, or `0511 0037` for a Logitec LDT-1S310U/J), select
**WinUSB** and press Replace Driver; on a composite device it is the entry
Zadig shows as *(Interface 0)*. A television stick arrives running its DVB-T
driver, which is what Zadig replaces — **it will stop working as a television
receiver** until the original driver is put back.
`cascade.exe --soapy-check` prints the search paths, the loaded modules and
either the device it opened or the reason there was none, and
`cascade.exe --rtlsdr-check` does the same for the native RTL-SDR path: what is
bound to WinUSB, which tuner answered, the gain ranges, how many samples
arrived in three seconds, and whether they are a signal at all - a dongle that
has stopped receiving can deliver the same few thousand samples over and over
at the right rate and level, and the check says so and tells you to unplug it
and plug it back in.

**The hardware search runs in a separate short-lived process.** Looking for
radios means loading every SDR driver installed on the machine and letting each
one scan the USB bus, and a driver that falls over while doing that used to
take the whole application with it. It now takes only that small process, and
the session carries on with an empty device list. If a scan finds nothing, the
diagnostics log distinguishes "no devices" from "the search crashed" and from
"a device stopped answering and the search was cut off" — three answers that
used to look identical.

## Installing from a script, and the command line

*Status: read from `installer/cascade.iss` and `src/engine/process_boot.cpp` at the merged tip (090e7039). The switches
marked "run" are the ones `tools/release.ps1` uses in its install test; the others were read from the installer script
and not run. `docs/silent-install-switches.md` lists where each statement comes from.*

The setup is `foxsdr-setup-<version>.exe`, an Inno Setup installer. It is not code-signed, so an interactive double-click
may meet SmartScreen. These switches are accepted on its command line:

| Switch | What it does |
|---|---|
| `/VERYSILENT` | No wizard and no progress window (run). `/SILENT` shows a progress window instead (run). |
| `/SUPPRESSMSGBOXES` | Message boxes are answered with their default, so a script never waits for a click (run). Give it with a silent switch. |
| `/NORESTART` | Never restart Windows. The setup needs no restart (run). |
| `/CURRENTUSER` | Install for the current user only, with no administrator prompt (run). Without it, or with `/ALLUSERS`, the setup asks for administrator rights and installs under Program Files. |
| `/DIR="<folder>"` | The install folder. Quote it if it contains a space: an unquoted `C:\Program Files\FoxSDR` was seen installing into `C:\Program\`. |
| `/TASKS="desktopicon"` | Chooses exactly the listed tasks. The script has two: `desktopicon` (the Desktop shortcut, on by default) and `cleaninstall` (off by default). |
| `/LOG="<file>"` | Writes the setup's log (run). |
| `/CLEANINSTALL=1` | FoxSDR's own switch: deletes your existing FoxSDR settings, plugins, caches and logs (`%APPDATA%\foxsdr` and `%LOCALAPPDATA%\FoxSDR`) **before** the files go down. Recordings are never touched. Write it with `=1`. |

An upgrade over an existing install keeps your settings. A silent install never starts the program afterwards. Close FoxSDR
first: a running FoxSDR can stop an install. Before installing, the setup silently uninstalls a FoxSDR found in the other
scope (a per-user install removes a machine-wide one and the reverse); your settings are not touched by that.

The uninstaller is `unins000.exe` in the install folder and takes the same `/VERYSILENT`, `/SILENT`, `/SUPPRESSMSGBOXES`,
`/NORESTART` and `/LOG` switches. FoxSDR's own two are `/CLEANDATA=1`, which also deletes `%APPDATA%\foxsdr` and
`%LOCALAPPDATA%\FoxSDR` without asking, and `/KEEPDATA=1`, which keeps them without asking and wins if both are given. **A
silent uninstall keeps your data unless `/CLEANDATA=1` is given**, because an upgrade runs exactly that command.
Recordings in `Documents\SDR-recordings` are never removed by setup or uninstall.

```powershell
# for the current user, no administrator prompt, into a folder of your choice
$p = Start-Process -FilePath .\foxsdr-setup-<version>.exe -Wait -PassThru -ArgumentList @(
    '/CURRENTUSER', '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/DIR="D:\Apps\FoxSDR"', '/LOG="D:\Apps\foxsdr-install.log"')
$p.ExitCode    # 0 is success
```

**The programs' command lines.** `foxsdr-slint.exe` and `cascade.exe` read the same flags. The ones meant for you:

| Flag | What it does |
|---|---|
| `--version` | Prints one line, `FoxSDR <version>`, and exits. |
| `--safe-plugins` | Starts with no plugin loaded for this run. Holding **Shift** while the window starts does the same (Windows only). |
| `--rtlsdr-check`, `--soapy-check` | Bench checks that print what was found and exit; see [Installing](#installing). They need the hardware. |

Support tools, hidden from the usage line on purpose: `--selftest` (a headless check of the receiver chain, exit 0 on pass),
`--benchmark` (times the DSP blocks, about ten seconds of CPU), `--record-check`, `--rds-check <MHz>` and `--tone-check`
(each needs hardware, writes to disk or plays a sound), and `--update-check [download]`, which **makes a network call** to
the update server and, with `download`, also downloads and verifies the installer without running it. An unknown flag prints
`unknown argument '...'` and the usage line and exits 1. No flag opens a radio, picks a source or sets a frequency.

`--frames <N>`, `--diag-stall`, `--diag-toggle`, `--link-tester`, `--sentinel`, `--enumerate-json`, `--list-drivers`,
`--sdrplay-service-state` and `--sdrplay-probe` are for tests and for FoxSDR's own helper processes. They are not supported
and may change without notice. The headless `foxsdr-engine` program has its own flags (`--seconds`, `--config`,
`--use-config`, `--web-enable`, `--cat-enable` and their companions); the setup does not install it.

**Environment variables you can set.**

| Variable | Effect | Window |
|---|---|---|
| `FOXSDR_LANGUAGE` | A language code that overrides the saved language for this run only. | both |
| `FOXSDR_DIAG_DIR` | Moves the diagnostics and crash-report folder. | both |
| `FOXSDR_SOAPY_DEBUG` | `1`, `true`, `yes` or `on` passes SoapySDR's debug logging into the log. | both |
| `SOAPY_SDR_PLUGIN_PATH`, `UHD_IMAGES_DIR`, `CONDA_PREFIX` | Where SoapySDR vendor modules are looked for and, for a B200, where the UHD firmware images are; see [Installing](#installing). | both |
| `FOXSDR_GL` | `auto`, `software` or `hardware`: whether the bundled software OpenGL may stand in for the graphics driver. | classic only |
| `FOXSDR_UI_SCALE`, `FOXSDR_LANG_FILE` | The interface scale for this run; a translator's catalogue file to try without a build. | classic only |
| `FOXSDR_GPS_PORT`, `FOXSDR_GPS_BAUD` | Read the receiver's position from a GPS on that serial port at start-up (see [What is in the current build](#what-is-in-the-current-build)). | classic only |

Other `FOXSDR_*` and `CASCADE_*` variables exist for tests and measurements; do not set them.

<!-- PENDING MERGE begin: libraries (oss-userlibs, oss-voice, oss-libs, oss-radios). Drop everything to the matching end marker, and its line in the contents list, if the branch does not ship. -->
## Libraries you add yourself

*Status: **pending merge.** The code for the libraries below is on lane branches (oss-userlibs, oss-voice, oss-libs,
oss-radios) that are not merged into `engine/multi-radio`; none of it is in the merged tip 090e7039. Every statement is
from the lane records. FoxSDR ships **none** of these libraries and never downloads them; you obtain the file from its
maker and put it where the record says. Every proof so far used a stand-in library written for the test, not the real
one, except where stated.*

A library you add is a program file that runs inside FoxSDR with the same rights you have. The lane that built the loader
says a person who picks one must be told so, in the same words as for an ExtIO driver. The loader reads the file's bytes
first: a 32-bit file in the 64-bit program is named plainly and not loaded, and a file that will not load is reported
without a Windows error box. It looks only in a folder or at a path you gave, or in the folders on PATH, never in the
current directory and never beside the program. Messages carry the file name, never its folder.

| Library | What it enables | File name | Where you put it | Status text when it is missing |
|---|---|---|---|---|
| **mbelib** (ISC licence) | The half-rate voice of DMR, NXDN, P25 phase 2 and narrow-band Fusion in the DMR and D-STAR Monitor plugin. FoxSDR has no half-rate decoder of its own. | `mbelib.dll`, then `libmbe.dll`, then `mbe.dll`; it must export `mbe_initMbeParms`, `mbe_processAmbe3600x2450Frame` and `mbe_processAmbe2450Data` (a build from mbelib's own CMake does not export them on Windows unless the builder arranges it, and the status text says so) | In the plugin folder, or in the folder named by the plugin setting **mbelib library folder** (empty means the folder the plugin file is in). Turn on **Half-rate voice (your mbelib)**; it is off by default. | "half-rate voice (DMR, NXDN, P25 phase 2) needs an mbelib library that you add: put mbelib.dll in the plugin folder". There are other sentences for a file that fails to load, a missing export and a fault; their wording is not quoted in the record. |
| **Codec 2** (LGPL) | The FreeDV receive plugin (modes 1600, 700C, 700D, 700E and 2020). The plugin contains no FreeDV code, no modem and no codec. | `codec2.dll` | Beside the plugin, or give its file or folder in the plugin setting **Codec 2 library**. Choose the mode in **FreeDV mode** (default 700D). A changed mode or path takes effect the next time the plugin is started. | "FreeDV needs the Codec 2 library, which you add yourself: put codec2.dll next to this plugin or give its path in the settings." |
| **FFmpeg libraries** (a shared LGPL build) | DAB+ sound decoded inside the DAB plugin. Without them the plugin uses the `ffmpeg` program if one is installed (see below). | `avcodec-NN.dll` and `avutil-NN.dll` | The plugin setting **FFmpeg libraries (folder)**, or the plugin's own folder. | "AAC audio needs the FFmpeg libraries (avcodec and avutil, a shared LGPL build): put them in the plugin's folder or name the folder in the settings" |
| **FFmpeg program** (older route) | DAB+ sound through `ffmpeg.exe` started as a separate program. DAB with MPEG Layer II sound needs nothing. | `ffmpeg.exe` | The plugin setting **DAB+ decoder (ffmpeg)**, else the PATH, else the usual install places (Program Files, winget, Chocolatey). | The panel's Decoder row reads "DAB+ sound needs ffmpeg". |
| **SDRplay API 3.x** | Every SDRplay RSP. This one is in the current build. | `sdrplay_api.dll`, found in `C:\Program Files\SDRplay\API\x64\` first | Install the API from sdrplay.com; it also installs the service and the RSP's driver. | The **RADIO SETUP** page: "vendor software missing" with "Install the SDRplay API 3.x from sdrplay.com (it installs the service and the RSP's driver), then press CHECK AGAIN." |
| **WiNRADiO G39DDC and G33DDC** | Those receivers as sources. | `G39DDCAPI.dll`, `G33DDCAPI.dll` | The source's **WiNRADiO library folder** (empty means the PATH folders). | Not in the merged build: no source or control for them is registered yet. |

How well each is proved:

* **mbelib.** The bridge has never been run against a real mbelib file; the test uses a stand-in that decodes nothing. Three real
  amateur-band recordings contained no DMR, D-STAR, Fusion or P25 frame at all, so every voice path is proved on generated
  signals only. Nobody has listened to the audio. The half-rate vocoder is covered by a patent the record gives as active
  until 2028-05-20, and the record has not assessed what that means for a user; you are responsible for what the law where you
  live allows you to listen to, and the plugin says so once when a voice switch is first turned on. Calls the radio marks
  encrypted are muted, never decrypted.
* **Codec 2.** The real, unmodified Codec 2 1.2.0 `codec2.dll` was run for its plain decode calls and nothing faulted. No
  FreeDV signal and no real FreeDV session has been run through the plugin.
* **FFmpeg libraries.** Tested only against stand-ins; none was loaded. The `ffmpeg` program route was timed with a real
  `ffmpeg` on the lane's machine.
* **WiNRADiO.** Stand-in only. The maker's pages call the file a 32-bit library and also say 64-bit programs can use it; which
  is true was not found out, and a 32-bit file is refused by FoxSDR's 64-bit program.
* **FTDI D2XX, the Icom IC-R8600 firmware, Fobos, Perseus, RFNM, Harogic and RF Explorer.** Not usable. The SDR-IQ source
  stops short of the FTDI call (it says "the SDR-IQ talks through FTDI's D2XX driver, which this build does not include");
  the IC-R8600 source waits for Icom's own firmware, which FoxSDR neither includes nor downloads; the others were not built.
  No control exists for any of them.

The bitness (32-bit or 64-bit) of the file you need is not stated in the records for mbelib, Codec 2 or FFmpeg; the loader
refuses a 32-bit file.
<!-- PENDING MERGE end: libraries -->

<!-- PENDING MERGE begin: helpers (oss-helperhost and the gh-* helper repository). Drop everything to the matching end marker, and its line in the contents list, if the branch does not ship. -->
## Helper plugins

*Status: **pending merge, and not yet complete.** The helper host is on the lane branch oss-helperhost and the helpers are
in a separate GPL repository that is local and unpublished; none of it is in the merged tip 090e7039. Nothing here has run
through FoxSDR's real window: the host side was tested against a test helper and a fake host only, and no real helper has
been run through it. This section says only what the design and the lane records say; where they are silent it says so.*

**What a helper is.** Some decoders exist only under a licence (GPL or AGPL) that does not allow their code inside
FoxSDR. Each is therefore built as a **separate program**, a command-line program that is useful without FoxSDR, in its own
GPL repository. FoxSDR never links its code and it never links FoxSDR's. A small FoxSDR plugin, the **Helper bridge**,
starts it as a separate process, feeds it samples and shows what it prints. A helper is GPL software with its own licence
and its own source; it is not part of FoxSDR and cannot see FoxSDR's data.

**How they talk.** The bridge writes raw samples to the helper's standard input (16-bit or float audio, or 16-bit or float
I/Q) and reads one line of JSON for each event from its standard output. Nothing flows back to the radio. Every helper must
be receive-only, must not decrypt anything, must have no network access, no telemetry and no window, and must exit when its
input closes. Each ships a manifest file (`<name>.helper.json`) saying its name, version, licence, where the source is, what
input it takes and at what rate.

**The Helper bridge's settings.** **Helper program** is the path of the helper (up to 520 characters, empty by default;
for example `C:\path\to\helper.exe`); **Helper options** is a line of extra command-line options (up to 400 characters). The
bridge's panel, headed **Helper bridge**, shows State, Program, Licence, Source, Input, Samples (sent and dropped), Restarts,
Output (lines and bad lines), what the helper says, and a log. Before it starts anything the bridge checks that the file is a
runnable 64-bit program for this Windows, so a wrong file produces a sentence, not a Windows error box. Its status lines
include "helper not installed - set the helper program in the plugin settings", "helper refused: not an executable for this
Windows", "helper started", "helper stopped: exited with code N after T; restarting in S" and "helper stopped: ... quick
failures in a row ... not being restarted again", and "too slow: N samples dropped so far" when the helper cannot keep up. A
helper that stops is restarted after 1, 2, 4 and 8 seconds and given up on after five quick failures in a row. A helper runs
in a job that ends with FoxSDR, so it cannot outlive it.

**How the helpers are proved.** The helpers were built and tested outside FoxSDR. Only the **generated signals only** rows
are listed here; wherever the README says a real recording was used it is named:

| Helper | What it decodes | Evidence |
|---|---|---|
| atn | The network layers carried in VDL Mode 2 frames (X.25, CLNP, CPDLC, ADS-C and others), from frames given as hex text; not a demodulator | Synthetic only: nothing was decoded from a real over-the-air frame. |
| fldigimodes | Olivia, Contestia, MFSK, MT63, THOR, RSID | Generated signals only. |
| hamdrm | HamDRM (digital SSTV) pictures | Not proved against a real transmitter or the real programs. |
| msumrcal | Meteor-M infrared calibration to brightness temperature (not a signal decoder) | Proves the arithmetic, not agreement with a real satellite pass. |
| trunkctl | Motorola SmartNet and SmartZone Type II and EDACS control channels | Generated signals only. |
| tetrapol | TETRAPOL control-channel signalling (no voice, data content or keys) | Generated signals only; no real recording. |
| trunkfmt | LJ1200, Tait 1200, Fleetsync II, MPT-1327, LTR and Passport signalling | Generated from the upstream program's own constants only. |
| hfserial | STANAG 4285, MIL-STD-188-110 serial tone, PACTOR-1/2/3 receive | Generated signals only, apart from two real PACTOR-3 test fields. |
| dvframes | The frames and signalling (no voice) of Yaesu Fusion, DMR, NXDN and dPMR | Generated signals only; one real try decoded nothing. |
| sondes | Radiosondes: LMS6, Mk IIa, MRZ, MTS01, CF-06, RS92 | Generated from upstream constants; RS92 unverified. |
| hfmodes | HF telegraphy (SITOR-A and B, ARQ-E and N, G-TOR, Piccolo, Coquelet, CIS modes and others) | Generated signals only, except SITOR-B, which ran on one real recording. |

Other helpers have some real-signal evidence: hfdl (a 127 s off-air recording gave 50 decoded messages), stdc (a published
Inmarsat-C recording), js8 (the program's own sample files), multimon (real POCSAG and FLEX recordings), weakmodes (real
MSK144 and JT9 samples; FT2L never decoded a real signal), weakmodes2 (FST4, FST4W and Q65 on real sample recordings), and
rtl433 (a cross-check on four real recordings). direwolf's README states both that it was proved on generated signals and
that two cases decode a real recording; it is not clear which holds for the rest. For the helpers that have no helper
README, there is nothing to document.

**Installing and switching one on.** The records do not describe this from the user's side, so this manual does not
either. What exists is: you type the path of the helper into **Helper program** in the bridge's settings (it is read at
least every 250 ms); a store that installs a helper's files beside the bridge, with consent lines, a row reading "GPL
helper - source: ..." and a detail text that names the helper, its version, its licence and its source, is a design that is
not built; and the helpers' repository is local and unpublished until the owner decides otherwise. The following are
**not answered by any document**: where you obtain a helper, which folder it goes in, what the Plugins page shows for it,
how it is removed, where its full licence text is shown, whether Windows will warn about running it, and which helper
needs which bridge. Several helpers take text, or the FM discriminator's audio, which nothing documented provides to the
generic bridge.
<!-- PENDING MERGE end: helpers -->

<!-- PENDING MERGE begin: new-plugins (oss/aero, dvbs2drm, files, hfmodes, misc, pictures, remoteid, signalling, tables, voice). Drop everything to the matching end marker, and its line in the contents list, if the branch does not ship. -->
## New and updated decoder plugins

*Status: **pending merge.** These are plugin lanes of 2026-11-12 on the branches oss/aero, oss/dvbs2drm, oss/files,
oss/hfmodes, oss/misc, oss/pictures, oss/remoteid, oss/signalling, oss/tables and oss/voice of the plugins repository. None
is in the merged tip 090e7039, none is in the plugin catalogue yet (each record says the merge lane regenerates it), and
none was run in the FoxSDR window: each was driven through the plugin interface by a test program that stands in for the
host. **Where this section says "experimental", the only evidence is generated signals or a stand-in program; the plugin has
never decoded a real signal of that kind.** All of them are receive-only. How you install or enable one is the ordinary
store flow in [Plugins](#plugins); the records add nothing to it.*

The version collision is unresolved: two lanes both raise the ADS-B decoder from 1.11.0 to 1.12.0, so the number it ends up
with is not known.

| Plugin (version change) | What it does | Settings (default) | Evidence |
|---|---|---|---|
| **ACARS 1.1.2, VDL Mode 2 0.2.2, HFDL 0.2.2** | ACARS text lines now carry the decoded layers inside: ADS-C reports and contracts, MIAM, OHMA (protected payloads reported, never decrypted) and the ARINC 622 wrapper. ADS-C reports put aircraft positions in the text lines; whether a setting should hide them is an open owner question. | None new. | **Experimental**: generated signals and one published decode; no real MIAM, OHMA or uplink ADS-C message. ATN header compression is not built. |
| **UAT 978 MHz 0.2.0** | The 978 MHz UAT payloads: state vectors, mode status, target state, TIS-B and ADS-R counts, ground uplink and FIS-B headers. Aircraft addresses, call signs and positions are not in any text line. | None new. | **Experimental**: generated bursts only; no real 978 MHz recording exists on the machine. FIS-B weather and NOTAM products are not decoded. |
| **DVB receiver 0.3.0** | DVB-S2 normal frames, all 28 modulation and coding combinations, with ACM and VCM, out through the existing UDP sink. No video or audio decoding, no conditional access. Short frames, multiple input streams and DVB-S2X are counted, not decoded. | In `dvb.cfg`: `system dvbs2`, `symbolrate`, `s2rolloff` (0.35), `s2code` (0). | **Experimental**: "Nothing here has seen a broadcast." |
| **DRM 0.3.0** | Robustness mode E (DRM+) channel information and a packet-mode data layer. The mode E audio is **not** decoded. | In `drm.cfg`: `robustness e` or `abcd` (default). Mode E needs a receiver running at 192 kS/s or more. | **Experimental**: nothing here has seen an off-air DRM+ signal. |
| **Rig link 0.1.0** | Follows DXLab Commander or N1MM+ and tunes FoxSDR to the logger's frequency; can send FoxSDR's frequency back. Needs the TUNE grant, and the SETTINGS grant for the mode. | Everything off; labels include **Follow DXLab Commander**, **Follow N1MM+ (UDP on this PC)**, **N1MM+ broadcast port (UDP)** (12060), **Send FoxSDR's frequency to Commander**, **Mark the logger's frequency**. | **Experimental**: tested against stand-ins only; never against the real logging programs. |
| **UK CB channels 0.1.0** | Presets for five UK CB channels (27 and 81 MHz). | None. | The channel table could not be checked at its source. |
| **DX spots 0.4.1, Demod Analyzer 0.3.1, Meteor ping 0.1.1, Touchstone viewer 0.2.0, RF measurements 0.1.1** | Reverse Beacon Network comment parsing; reading the `auxi` and `inf1` chunks of an I/Q file for its frequency and time; the RMOB month table; NanoVNA import from a saved capture (no serial link); the equivalent noise bandwidth of the window. | None new. | Tests and a few real lines or files only; a real NanoVNA was never used. |
| **CCIR selcall 0.2.0, HF ARQ decoder 0.2.0, HF modem identifier 0.2.0, GSC decoder 0.2.0, PACTOR monitor 0.1.0** | More selcall tone sets (CCIR, EEA, EIA, ZVEI-1 to 3, DZVEI, PZVEI); ARQ-E; the tone header of a MIL-STD-188-110A hybrid waveform; Golay Sequential Code paging; and **identification only** of PACTOR-II and PACTOR-III (no payload). | `ccir.set` (1 to 8, 1 = CCIR); `hfarq.mode`; `known_words`; `pactor.details` (off). | **Experimental**: generated signals only; no PACTOR recording exists. |
| **Falcon 9 legacy 0.1.0** | Decodes **recordings** of the stage-2 S-band downlink made before April 2021, when it was reported to have become encrypted; it cannot be pointed at today's signal. Holds no key. | **Show debug-log text** (on); **Video file (MPEG-TS)** (empty = off). | **Experimental**: generated signals only. |
| **Call uploader 0.1.0** | Uploads finished call recordings from a folder to **your own** Rdio Scanner server. | Off; no address built in. **Upload calls**, **Folder to watch**, **Rdio Scanner address (http://)**, **Plain http to private LAN**, **Server API key**. | **Experimental**: tested only against a stand-in server. See the warning in [Decoder plugins and the network](#decoder-plugins-and-the-network). |
| **EnOcean 0.2.0** | Shows the equipment profile name for a telegram (names only; data bytes stay raw). | **Equipment profile (name shown)**, empty. | Tests only. |
| **Map imagery 1.3.0** | Named tile presets: OpenStreetMap, OpenTopoMap, NASA GIBS, CARTO Voyager, Positron and Dark Matter. | `presets=` in `openmaptiles.conf` (none by default); **Map key (your own API key)**. CARTO presets need **your own** key, which FoxSDR never ships. | Tests only; no outside connection was made. |
| **LRPT 0.4.0** | Adds Meteor-M 80k interleaved to the 72k and plain 80k it had. | `interleaved-80k` in `lrpt.cfg`, yes by default. | 80k: **experimental**, generated only. The 72k path still decodes the real recording it always did. |
| **KG-STV decoder 0.1.0** | Receives the KG-STV amateur image and text mode. Sends nothing; text and callsigns are kept nowhere. | No settings. | **Experimental**: generated signals only; the program's author says full compatibility is not yet established. |
| **Radiosonde decoder 0.3.0** | Adds Meteomodem M10 and M20, Meisei iMS-100 and RS-11G, Graw DFM and InterMet iMet-4. | None new. | **Experimental**: generated signals only; the DFM scaling was checked against 19 telemetry lines printed in a thesis. |
| **C-QUAM stereo 0.1.0** | Decodes AM stereo; stereo audio through the host. | None. | **Experimental**: generated signals only. |
| **Bluetooth LE advertising 0.4.0** | LE Coded PHY (long range) packets and Bluetooth 5 extended advertising, and the Remote ID packs inside them, shown with identifiers hidden. | **Long-range (Coded PHY) receive** (on), **Decode long-range packets** (on), **Scan secondary channels** (off: about three times the CPU). | Coded PHY and Remote ID: **experimental**, generated only. The ordinary LE 1M path met three real recordings (1,782 advertising packets, no Remote ID because no drone was near). Wi-Fi Remote ID is not built. |
| **Z-Wave monitor 0.1.0** | Z-Wave 868 MHz (EU), 9.6, 40 and 100 kbit/s, one line a frame. Nothing is decrypted; the payload is not shown. | **Hide network and device IDs** (on); a preset at 869.1 MHz, 2 MS/s. | 40 and 100 kbit/s: one real 300 s recording gave 15 valid frames. 9.6 kbit/s: **experimental**. |
| **P25 trunk follower 0.1.0** | Follows a Phase 1 control channel and, only when you switch following on, tunes to the talkgroups you list. Encrypted, data and unit-to-unit calls are never followed; unit addresses are never printed. Needs the TUNE grant. | **Follow talkgroups** (off), **Talkgroups to follow**, **Control channel (MHz)**, **Only this NAC (decimal)**, **Hold after last frame (s)** (3), **Longest call to stay on (s)** (120). | **Experimental**: no off-air P25 recording. Phase 2 and recording per talkgroup are not built. |
| **Tait CCDI monitor 0.1.0** | A read-only monitor of a Tait radio's CCDI link, from a log file or a COM port opened read-only. | **CCDI source (file or COMn)**, empty = off; **CCDI baud (COM only)** (28800). | Not verified: no radio and no real capture. |
| **M17 0.1.1, packet TNC 0.2.1** | M17 end-of-transmission and restart handling; FX.25 with four Reed-Solomon conventions. | None new. | Generated signals and one published worked example. |
| **APT 1.2.0, ADS-B decoder 1.12.0, Aircraft Info 1.3.0, Satellites 1.4.0** | NOAA infrared constants so the picture's temperature scale needs no file; magnetic variation, a military flag, airline and route in the JSON outputs; an offline table of 17,139 airframes; import of a SatNOGS transmitter list you save yourself. | Mostly none new; Satellites reads `transmitters <file>`. | Driven by test programs only. The APT temperature was judged plausible, not checked against a reference. |
| **DMR and D-STAR Monitor 0.8.0** (named "EXPERIMENTAL, voice off by default") | Voice through the host's audio for D-STAR, P25 phase 1 and Fusion wide, and, with your own mbelib ([Libraries you add yourself](#libraries-you-add-yourself)), half-rate DMR. Encrypted calls are muted, never decrypted. One DMR timeslot at a time. | All off: **D-STAR voice decoding**, **P25 voice decoding**, **Fusion wide voice (experiment)**, **Half-rate voice (your mbelib)**; **mbelib library folder**. | **Experimental**: generated signals only; three real amateur-band captures held no voice frame. Nobody has listened to the audio. |

**Not built.** Inmarsat Aero (JAERO), FIS-B products, DVB-S2 short frames and DVB-T2, DRM hierarchical modulation and
mode E audio, Hamlib CAT in Rig link, the NanoVNA serial link, STANAG 4285, PACTOR payloads, Falcon 9 GPS fields,
Broadcastify and OpenMHz upload, LMS6 radiosondes, MDC-1200, RADE, P25 phase 2, ProVoice, NXDN voice and narrow-band
Fusion voice.

## Decoder plugins and the network

*Status: the rows say where each plugin stands. Plugins have their own sockets; the host gives them none. Nothing below is
encrypted unless it says so.*

* **Call uploader** (pending merge). Uploads each finished call recording to the Rdio Scanner address you type, over plain
  `http://` only (a numeric address or localhost; a private LAN address only with **Plain http to private LAN** ticked; a
  public address is refused). **The server API key and the audio cross the network unencrypted.** Off until you switch it on.
* **Rig link** (pending merge). Connects to DXLab Commander on 127.0.0.1 and listens for N1MM+'s UDP broadcast on 127.0.0.1
  only, so nothing leaves this machine. Port 12060 is also the DX-spots logger-feed default: both bind it exclusively, so one
  must change port.
* **CW skimmer** (a plugin lane, not merged). An optional spot server that sends DX-cluster-style spot lines to a logging
  program: off by default, 127.0.0.1 port 7300. When **Spot server address** is changed so that it is reachable from other
  computers the host log warns that it is. It sends nothing to any outside service. No real logging program has been tried
  with it.
* **Event outputs** (a plugin lane): PostgreSQL rows and e-mail. **Neither has TLS or STARTTLS**; credentials are sent only to
  loopback or private-network addresses and are otherwise refused. Both outputs are off by default. Never tried against a
  real database or mail server.
* **APRS decoder 1.2.0 (APRS-IS)** (a plugin lane): optional receive-only login, or, if you type a callsign and passcode, a
  login that **sends the passcode in the clear**, and an optional iGate of the packets it hears. Both are off by default;
  nothing is pre-filled. Never tried against a real APRS-IS server.
* **DX spots** (a plugin lane): a DX-cluster or Reverse Beacon Network client over plain Telnet, off until you switch it on.
* **Tait CCDI monitor** (pending merge). Only reads a file or a COM port.

Whether the lane plugins of the plugins repository are in the catalogue you see is not known: they are on the branch
`plugins-linux` of that repository, not on its `master`.
<!-- PENDING MERGE end: new-plugins -->

<!-- PENDING MERGE begin: plugin-proof (plugins repository, branch plugins-merged). Drop everything to the matching end marker, and its line in the contents list, if the branch does not ship. -->
## How far each decoder plugin has been proved

*Status: from the lane records of the plugins repository, whose lane branches are merged into its `plugins-merged` branch
(pushed as `plugins-linux`, not on its `master`). Whether the live catalogue serves these versions, and whether each plugin
loads and shows its window in the current FoxSDR window, was not checked: no lane ran FoxSDR itself. No record claims
hardware proof for any plugin.*

The manual has described how plugins work, not what each plugin is worth. This table says, for each plugin in the catalogue
file at that branch, the evidence its lane record gives, in four words used the same way throughout: **generated signals
only** means the plugin has decoded signals made by a test program and nothing else; **some real recordings were used**
means at least one real off-air recording was decoded, while anything added later (named in the record) rests on generated
signals; **stand-in server only** means it was tested against a program on 127.0.0.1 that imitates the real service; and
**model or published test vectors only** means a calculation checked against published examples. The **catalogue badge**
column is the catalogue file's own EXPERIMENTAL flag. Six plugins carry no badge although their evidence is generated
signals only: treat any plugin whose evidence is "generated signals only" as experimental. A plugin on this list that has
never decoded a real signal may decode nothing on air.

| Plugin | The evidence the lane record gives | Catalogue badge |
|---|---|---|
| acars-decoder (ACARS) | Some real recordings were used; the newer parts are generated signals only | not marked |
| adsb-decoder (ADS-B) | Some real recordings were used; the newer parts are generated signals only | not marked |
| ils-meter (ILS meter) | Generated signals only | EXPERIMENTAL |
| per-tester (Packet error rate tester) | Generated signals only | EXPERIMENTAL |
| demod-analyzer (Demod Analyzer) | Some real recordings were used; the newer parts are generated signals only | EXPERIMENTAL |
| vor-decoder (VOR) | Generated signals only | not marked, although the evidence is generated signals only |
| aircraft-info (Aircraft info) | Generated signals only | not marked, although the evidence is generated signals only |
| avdb (Airports & Navaids; not in index.json by that id) | Model or published test vectors only | not in the catalogue |
| selcal-decoder (SELCAL) | Generated signals only | not marked, although the evidence is generated signals only |
| uat-978 | Generated signals only | EXPERIMENTAL |
| hfdl-decoder (HFDL) | A real off-air recording decoded | EXPERIMENTAL |
| vdl2-decoder (VDL Mode 2) | Some real recordings were used; the newer parts are generated signals only | EXPERIMENTAL |
| ais-decoder (AIS) | Some real recordings were used; the newer parts are generated signals only | not marked |
| aprs-decoder (APRS) | Some real recordings were used; the newer parts are generated signals only | not marked |
| packet-tnc (Packet TNC KISS) | Some real recordings were used; the newer parts are generated signals only | EXPERIMENTAL |
| radio-email (Winlink-compatible) | Stand-in server only | EXPERIMENTAL |
| cw-decoder (Morse) | Some real recordings were used; the newer parts are generated signals only | not marked |
| rtty-decoder (RTTY) | Some real recordings were used; the newer parts are generated signals only | not marked |
| cw-skimmer (CW Skimmer) | Some real recordings were used; the newer parts are generated signals only | EXPERIMENTAL |
| ft8-decoder (FT8, +FT4) | FT8: real off-air decodes (a real 28.074 MHz recording). FT4: synthetic signals only | not marked |
| psk31-decoder (PSK31/63/125) | Generated signals only | EXPERIMENTAL |
| wspr-decoder (WSPR) | Generated signals only; one real 10 m recording gave no decodes | EXPERIMENTAL |
| jt65-decoder (JT65) | Some real recordings were used; the newer parts are generated signals only | EXPERIMENTAL |
| m17-decoder (M17, no voice) | Generated signals only | EXPERIMENTAL |
| dtmf-decoder (DTMF) | Generated signals only | EXPERIMENTAL |
| olivia-decoder (Olivia, MFSK16) | Generated signals only | EXPERIMENTAL |
| dominoex-decoder | Generated signals only | EXPERIMENTAL |
| feld-hell (Feld-Hell, picture) | Generated signals only | EXPERIMENTAL |
| timesig-decoder (time signals) | Some real recordings were used; the newer parts are generated signals only | EXPERIMENTAL |
| ccir-selcall | Generated signals only | EXPERIMENTAL |
| sitora-decoder (SITOR-A) | Generated signals only | EXPERIMENTAL |
| synop-decoder | Some real recordings were used; the newer parts are generated signals only | EXPERIMENTAL |
| ale-decoder (ALE 2G) | Generated signals only | EXPERIMENTAL |
| hfarq-decoder (ARQ-M2/M4, E3, G-TOR, Twinplex) | Generated signals only | EXPERIMENTAL |
| hf-modem-id (MIL-STD-188-110C ID only) | Generated signals only | EXPERIMENTAL |
| stanag4285-id | Generated signals only | EXPERIMENTAL |
| pocsag-decoder (POCSAG) | Some real recordings were used; the newer parts are generated signals only | not marked |
| flex-decoder (FLEX) | Some real recordings were used; the newer parts are generated signals only | not marked |
| eas-decoder (EAS/SAME) | Some real recordings were used; the newer parts are generated signals only | not marked |
| twotone-decoder | Some real recordings were used; the newer parts are generated signals only | not marked |
| gsc-decoder (GSC Golay) | Generated signals only | EXPERIMENTAL |
| voice-inversion-detector | Some real recordings were used; the newer parts are generated signals only | EXPERIMENTAL |
| mpt1327-monitor | Generated signals only | EXPERIMENTAL |
| dmr-monitor (DMR, D-STAR, Fusion, dPMR; signalling only) | Generated signals only | EXPERIMENTAL |
| tetra-decoder (TETRA, signalling only) | Generated signals only | not marked, although the evidence is generated signals only |
| x10rf-decoder | Generated signals only | EXPERIMENTAL |
| fmsfsk-decoder (FMS) | Generated signals only | EXPERIMENTAL |
| clipfsk-decoder (Caller ID) | Generated signals only | EXPERIMENTAL |
| remote-monitor (keyless remotes) | Generated signals only | EXPERIMENTAL |
| lora-monitor (LoRa, Meshtastic header) | Generated signals only | EXPERIMENTAL |
| enocean-monitor | Some real recordings were used; the newer parts are generated signals only | EXPERIMENTAL |
| ble-advertising (BLE + Remote ID) | Generated signals only | EXPERIMENTAL |
| wifi-occupancy / lte-cell-search / gsm-broadcast / ieee802154-frames | Generated signals only | EXPERIMENTAL |
| weather-decoder (433 MHz Sensors) | Some real recordings were used; the newer parts are generated signals only | not marked |
| ert-decoder (ERT) | Generated signals only | not marked, although the evidence is generated signals only |
| wmbus-decoder (Wireless M-Bus) | Some real recordings were used; the newer parts are generated signals only | EXPERIMENTAL |
| nrf24-framing / fpv-detector | Generated signals only | EXPERIMENTAL |
| fox-hunt / meteor-ping | Generated signals only | EXPERIMENTAL |
| signal-identifier (Signal Identifier) | Some real recordings were used; the newer parts are generated signals only | EXPERIMENTAL |
| survey-engine (Survey Engine) | Some real recordings were used; the newer parts are generated signals only | not marked |
| openmaptiles-basemap (Map imagery) | Stand-in server only | not marked |
| touchstone-viewer | Model or published test vectors only | EXPERIMENTAL |
| clip-uploader | Stand-in server only | EXPERIMENTAL |
| effect-chain / screen-recorder | Model or published test vectors only | EXPERIMENTAL |
| workbench (Decoder Workbench) | Generated signals only | not marked, although the evidence is generated signals only |
| dx-spots (DX spots) | Real HamQTH and PSK Reporter answers parsed; the cluster, RBN, POTA and logger inputs only against stand-in servers | EXPERIMENTAL |
| metar (METAR) | Stand-in server only | EXPERIMENTAL |
| station-labels (EiBi) | Some real recordings were used; the newer parts are generated signals only | EXPERIMENTAL |
| propagation-tools | Stand-in server only | EXPERIMENTAL |
| solar-system-view | Model or published test vectors only | EXPERIMENTAL |
| satellites (Satellites) | Model or published test vectors only | not marked |
| radiosonde-decoder | Generated signals only | EXPERIMENTAL |
| apt-decoder (NOAA APT) | Some real recordings were used; the newer parts are generated signals only | not marked |
| wefax-decoder (WEFAX) | Some real recordings were used; the newer parts are generated signals only | not marked |
| lrpt-decoder (Meteor LRPT) | A real off-air recording decoded | EXPERIMENTAL |
| goes (GOES HRIT/LRIT) | Some real recordings were used; the newer parts are generated signals only | not marked |
| hrpt-decoder (NOAA HRPT) | Generated signals only | EXPERIMENTAL |
| sstv-decoder (SSTV) | Some real recordings were used; the newer parts are generated signals only | not marked |
| analogue-tv | Generated signals only | EXPERIMENTAL |
| loran-c | Generated signals only | EXPERIMENTAL |
| dvb-receiver (DVB-S, DVB-T) | Generated signals only | EXPERIMENTAL |
| dab (DAB/DAB+) | Some real recordings were used; the newer parts are generated signals only | EXPERIMENTAL |
| nrsc5-radio-dynamic (IBOC Radio / HD Radio) | Some real recordings were used; the newer parts are generated signals only | EXPERIMENTAL |
| drm (DRM; built by CMake, NOT in index.json) | Generated signals only | not in the catalogue |
| rade-decoder (RADE; CMake only, NOT in index.json) | A stand-in interface only; it decodes nothing | not in the catalogue |
| navtex-dsc-decoder | Some real recordings were used; the newer parts are generated signals only | not marked |
| epirb-decoder (406 MHz) | Some real recordings were used; the newer parts are generated signals only | not marked |
| stdc-decoder (Inmarsat-C EGC) | Generated signals only | EXPERIMENTAL |
| dgps-decoder | Generated signals only | EXPERIMENTAL |
| rf-measurements | Generated signals only | EXPERIMENTAL |
| event-outputs (PostgreSQL / e-mail) | Stand-in server only | EXPERIMENTAL |
<!-- PENDING MERGE end: plugin-proof -->
