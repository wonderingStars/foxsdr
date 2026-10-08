# Using FoxSDR

Every feature in detail: the receiver, plugins, frequency lists, airband, the patch view, languages, the keyboard, browser access, how it looks, privacy, what happens when it crashes or freezes, and installing.

Back to the [README](../README.md).

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
  decibels — and a bias-tee switch on the radios that have one. The **Auto gain** switch is remembered per radio and put back when that radio is opened again (an Airspy keeps its own gain mode instead). Developed against an Ettus B200
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
- **A bench oscilloscope**, under VIEW. The demodulated audio drawn as a
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
  the lever stays pointing the way it was last flicked. The SAMPLE RATE and
  FRAME TIME meters at the deck's right are on the bar at the window size the
  application first opens with, not only once it is widened. Right-click
  either meter to pick its own face - Classic, Analogue needle, LED ladder or
  Peak meter - without changing what it reads.
- **Band plans for the whole world.** Labelled service allocations drawn behind
  the spectrum, colour-coded by service on a colour-blind-safe palette, with a
  "what am I tuned to" readout that names the *narrowest* match — 145.800 MHz
  reads "ISS Downlink", not "2 m Amateur". Pick your region from Display →
  Region: a baseline for each ITU region plus country refinements that layer on
  top of it. The default is a worldwide plan holding only the allocations that
  are identical everywhere, because the ITU regions genuinely disagree — 40 m,
  mediumwave and the FM broadcast edges all differ — so there is no single plan
  that is correct globally. Informational only; not a licensing reference.
- **Map and decoders.** Aircraft, vessels and stations plotted together,
  coloured by altitude band, with optional map imagery from a basemap plugin;
  wherever its tiles are drawn - the map pages, the patch Map part and the
  radar scope - the plugin's attribution is lettered beneath them.
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
  from your receiver. A **Sort** control above the list orders it by any of
  those eight — callsign, id, altitude (ft), speed (kt), course (deg), distance
  (km), bearing (deg) or age (s) — ascending or descending, so "what is nearest
  me" is one menu pick away. Clicking a row takes the map to that target;
  double-clicking follows it.
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
("Catalogue read 14:07") with **CHECK AGAIN**. **BROWSE** is the catalogue in sections -
AIRCRAFT, MARINE, SATELLITES AND WEATHER, METERS AND PAGING, VOICE AND DATA, MAPS AND TOOLS,
and OTHER for anything the catalogue has not filed - each a grid of cards: a small drawing of
what the plugin is for, its name, a line about it, a badge when it has never decoded a real
signal (**EXPERIMENTAL**) or has no build for your system (**WINDOWS ONLY**, **LINUX ONLY**),
and one key at the right: **GET**, **FITTING...** while its download runs, **INSTALLED** or
**UPDATE** with the two versions beside it. A key that cannot act is drawn greyed and says why
when you rest the pointer on it - the plugin was built for another plugin ABI, there is no build
for this system, the catalogue states no licence, another transfer is running. A click on a
card's name or drawing opens that plugin's **page**; **GET** on a plugin whose maker attached a
legal notice does the same instead of fitting it, because the notice cannot be skipped.

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
never shown.

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

**Fitted modules** is the operating panel. Along its top bar: a search field, five filter chips
with their lamps and counts - **FED**, **NOT DECODING**, **TAKES NO SIGNAL**, **STOPPED**,
**REFUSED**, each a toggle - and **SCAN AGAIN** and **RESET WINDOW SIZES**. Under it one muted line
says whether the receiver is running (a module with a matched decoder is only fed while it is) and,
after a dot, which folder the modules were read from. Then one row per module, sorted by name: its
drawing, name and version, under the name an amber warning when it reaches beyond FoxSDR ("asks to
move the receiver", "may fetch from a server it chose" - nothing for a module that publishes to the
host only) or, for a module the host refused, the host's own reason; at the right what it is doing -
**FED**, **NOT DECODING**, **TAKES NO SIGNAL**, **STOPPED** or **REFUSED**, with its lamp - then
**STOP** (or **START**) and **REMOVE**, which asks twice: **CONFIRM** appears beside it for five
seconds. **CLEAN UP OLD VERSIONS (n)** is at the foot when an update left copies behind. A click on
a module's name opens its **page**: its header with the same **STOP** key, then **ON THIS MACHINE**
- the state with its lamp, what the module reaches, the sentence that explains the state (for a
refused module, why it is not running, in the host's words), the file it was loaded from, its
plugin ABI and the day it was fitted - and, for a module the catalogue knows, the same pictures,
description and notes the store's page shows, and the facts behind **SHOW DETAILS**.

Neither window reopens by itself: since 0.79.1 FoxSDR always starts on the main window alone,
whatever was showing when it was closed - no page, no map, no decoder output, and the bench rather
than the radar scope. Every one of them is a key on the rail away, and nothing opens a window but
your own hand. A plugin's own window - a decoded picture, a plugin's panel - has a row of its own
under DECODE, with a chip saying what it holds (WAIT, RX or IMG for a picture, a row count for a
panel): press the row to open it, its key to close it. A decoder with no window of its own - the
POCSAG and DMR decoders, say - writes its lines in the shared **Decoder output** window, which a
preset on such a decoder opens for you. That window, and a decoded picture's, opens INSIDE the main
window, in the middle of it, wherever the main window is on the screen: it used to open past the
main window's right edge, and on a main window that reached the edge of the screen only a narrow
strip of its frame showed - "a partial vertical bar" - where the lines should have been. Drag it
by its top rail to put it beside the radio or on another screen; **RESET WINDOW SIZES** in the
Fitted modules window brings it back.

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
meter roster, a burst log) is the plugin's own row feed. Instrument windows
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
`audioPluginGaps` and `audioPluginGapFrames`. In the **Plugin store**, a module
that can do this says so on its page, under REACHES, before you fit it.

**Stop and start.** In the Fitted modules window every loaded module's row
carries a **STOP** key, and a stopped one carries **START**; the module's
page carries the same key at the right of its header.
Stopping destroys everything that plugin had
running — its decoders, its map targets and trails, its window, its basemap
tiles — while leaving the module loaded and the row where it was, so a stopped
plugin decodes nothing, draws nothing, and cannot move the receiver. The row
then reads **STOPPED**, lettered in plain ivory rather than in anything
that reads as a fault, because a module you switched off is a choice — and a
plugin that produces nothing for a reason you have forgotten choosing is
exactly what this must not become. It is
remembered between sessions and across a rescan. Pressing one of the plugin's
own preset buttons starts it first: pressing "ADS-B 1090 MHz" is an unambiguous
request for that plugin, and tuning there with the decoder still switched off
would be worse than useless.

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
audio output is silenced — the speakers, the browser's audio stream and an
**audio recording** alike, since all three are
fed from the same point, so a WAV taken while a decoder is muting contains
digital silence and the Recorder panel says so while the take runs. (An I/Q
recording is untouched: it is taken before any of this.) The volume setting is
not touched, the decoders keep receiving samples throughout, and **Sinks** says
"Muted by *plugin*" so the silence is never unexplained. Your choice per plugin
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
they survive an update), show **messages** in the Decoder output window (a
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
is not a PNG or claims a size no screen needs.

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

**VIEW > Bookmarks** holds any number of named frequencies - tens of thousands
is fine - with a group, a mode, a bandwidth and a favourite star each. Click a
row to tune it. The box at the start of each row ticks it: the Scanner's
**Ticked frequencies** scans just the ticked rows (each with its own mode and
bandwidth), and the Airband monitor plays the ticked AM and NFM ones.

- **Import** an SDR# `frequencies.xml`, or a CSV saved from Excel: type or
  paste its path and press Import, or drop the file anywhere on the window.
  SDR#'s Name, GroupName, Frequency, DetectorType, FilterBandwidth and
  IsFavourite come across; the file does not have to be perfectly formed (a
  hand-merged list with one entry per line reads fine). A CSV can have its
  columns in any order under a header naming them - frequency, name, group,
  mode, bandwidth, favourite - or no header at all (frequency, name, group,
  mode, bandwidth); frequencies are Hz, or MHz when the header says so or the
  number is plainly MHz; commas, semicolons and tabs all work, and so does a
  decimal comma. Importing the same file twice adds nothing twice. SDR#'s
  converter Shift is not applied: the frequency is taken as written, and the
  import says when a list carried one.
- **Find** with the search box (names, groups, or the start of a frequency in
  MHz), the group list, and Favourites only. "Remove this group" takes a whole
  imported group out again.
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

**VIEW > Airband** listens to an airport's air traffic control:

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
  (a WFM, SSB or CW row is kept unticked: the monitor does not play it), and
  **Export CSV** writes the preset to the recordings folder as
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
| **Radio** | One device of its own - up to five in a patch, all running at once. Choose the device and its sample rate in the panel on the right and type its centre on the node. A device can be on one Radio only; the list greys out a device another Radio already has. Its **ON/OFF** switch, first on its face, closes that radio alone while the rest of the patch keeps running. A Radio can also play an **I/Q recording**: every 2-channel WAV (16-bit PCM or 32-bit float) in the recordings folder is in its device list (the patch's own speaker recordings are left out), played on a loop in real time. The recording sets the rate; the Radio's centre is the frequency the recording was made at, so type the frequency it was tuned to. Several Radios can play different recordings at once; one recording can be on one Radio only. |
| **Channel** | One frequency out of that capture, tuned, filtered and decimated. Its frequency is typed on its face; its live level is shown above. |
| **Demod** | AM or FM demodulation of a channel, with a **squelch**: on by default at -50 dB, its threshold on a slider and the channel's live level beside it, so a speaker or a recording hears signals rather than the noise between them. Decoders behind it still get every sample. |
| **Speaker** | Where the demodulated channel wired to it goes: a **WAV file** (the default), an **MP3 file**, **the speakers**, or any other sound output - chosen in the panel on the right. Files go in the recordings folder, one per speaker, named after it. |
| **Spectrum** | A live trace and waterfall: of the whole capture (every channel marked and named) when wired to the Radio, of one channel when wired to that channel. Right-click its waterfall for the [marker menu](#markers-on-the-waterfall), its trace for [peak hold and average](#peak-hold-and-average-on-the-spectrum). |
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
| Squelch up / down | <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>PgUp</kbd> / <kbd>PgDn</kbd> | 2 dB a press |
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
| Bank: SIGNAL PATH / DECODE / VIEW / EXTEND / SYSTEM | <kbd>F1</kbd>…<kbd>F5</kbd> | |
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
- **There is no TLS.** The server speaks plain HTTP deliberately: it links no
  crypto library, so it cannot honestly offer transport security. On a home
  LAN that is a reasonable trade. **Do not port-forward it to the internet.**
  To reach it from outside, terminate TLS in front of it — a reverse proxy
  (Caddy, nginx) or a tunnel such as Tailscale or Cloudflare Tunnel — and never
  expose its port directly.
- Passwords are stored hashed (PBKDF2-HMAC-SHA256), never in the clear;
  sessions are cookie-based, expire, and are revoked whenever the settings
  change.

The browser client does everything the desktop does, with two deliberate
exceptions: it cannot name an I/Q file or a recording directory, because those
are host filesystem paths rather than settings.

It has no Stop/Start button of its own — that lives in the window — but it does
SAY which plugins are stopped, so it never claims output from one that is
switched off, and pressing a stopped plugin's **preset** there starts it, just
as pressing it in the window does. A preset press is the same unambiguous "I
want this plugin now" wherever it comes from, and the alternative — retuning
the receiver for a decoder that is switched off, or silently doing nothing —
would be worse from a page that cannot explain itself. It has no mute checkbox
either, for the same reason, but when a decoder is silencing the audio the
toolbar carries a **muted by *plugin*** badge — a browser hearing nothing has
no other way to tell a muted radio from a dead stream.

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
under its title - **SIGNAL**, **DECODE**, **VIEW**, **EXTEND**, **SYSTEM** -
one of them pressed in with a phosphor strip lit beneath it, and the column
below showing that bank's sections and nothing else. SIGNAL is the receiver
itself (source, radio, audio filters, sinks, the recorder); DECODE is the
plugin store, the fitted modules, the decoders, target details and the
satellites map; VIEW is the display range, the demod scope, the radar scope,
bookmarks, the scanner and airband; EXTEND is browser access and CAT control; SYSTEM is updates,
diagnostics and usage reporting. **F1 to F5** press the same five keys from
the keyboard. Each section still opens and closes with its own key and
keeps that state as you move between banks; a section unfolds rather than
appearing, a bank comes up like a lamp rather than switching in one frame, and
the rail opens on whichever bank you left it on — the only part of this the
configuration file records, so a section comes back at its usual state on
the next launch. Every chip on a row still reports what that section is
doing without opening it.

The **Demod scope**, under VIEW, is the other instrument on the bench, and it
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

The **Radar scope**, under VIEW, is drawn from the receiver's own position —
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
- **The plugin catalogue.** Fetched when you open the Plugin store window
  (the first time in a session, since 0.99.16) and whenever you press
  **CHECK NOW** in it. Nothing is fetched at startup — since 0.79.1 that
  window does not reopen by itself — and no plugin ever updates itself.
  Since 0.99.47 the same moment also asks foxsdr.com for a short list of
  plugins offered only to some countries; it answers from Cloudflare's
  country code for your connection, stores nothing, logs nothing, and is not
  asked for when you have pointed the store at a different catalogue. See
  [PRIVACY.md](../PRIVACY.md).
- **A plugin download**, when you press **GET** or **UPDATE** for
  one — always https, sha256-verified against the catalogue, size-capped and
  refused on a cross-host redirect.
- **A plugin's pictures**, when you open its page in the Plugin store: each is
  fetched once from the address the catalogue gives, over https, checked against
  the catalogue's sha256 and a 4 MiB cap, and kept under `store-cache` in the
  plugins folder. The request carries that address and nothing else; a plugin
  from the regional list shows none. See [PRIVACY.md](../PRIVACY.md).
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
