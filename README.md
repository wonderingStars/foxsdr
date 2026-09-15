# FoxSDR

A from-scratch software-defined radio receiver for Windows: spectrum and
waterfall, multi-mode demodulation (NFM/WFM/AM/DSB/USB/LSB/CW), stereo FM with
RDS, recording, bookmarks, a scanner, band plans, native drivers for the
RTL-SDR, the HackRF, the Airspy R2/Mini, the Airspy HF+, the SDRplay RSPs, the
Mirics MSi2500, the RX888 mk2 and the ADALM-Pluto, and hardware support for any
other radio SoapySDR can reach.

> ### ⚠️ Linux is in development and not usable yet
>
> **Windows is the only supported platform.** A Linux build exists and its test
> suite passes, but it is unfinished work rather than a release, and it is not
> expected to work properly for real use yet:
>
> - **Never tested with a radio on Linux.** The port was verified against the
>   test suite, the built-in signal generator and IQ-file playback. No SDR has
>   been driven through it on a Linux machine, so the hardware path is unproven,
>   and that is the single biggest reason not to rely on it yet.
> - **The plugins are built and installable, but unproven on air here.** Ten of
>   the eleven catalogued plugins now ship for Linux and install from the in-app
>   catalogue, including the aircraft registry lookup and the map basemap; only
>   the example plugin is Windows-only.
> - **Never run on a real Linux desktop.** It has been exercised under WSL and
>   under a virtual display in CI — neither is a real graphical session with a
>   real sound card.
>
> Treat it as something to build and experiment with, not something to rely on.
> Reports of what breaks are welcome; it will be announced as supported when it
> has been proven against real hardware and the plugins exist.

It also has a **map** for decoded targets — aircraft, ships, stations — and can
serve its whole interface to a **browser** on your own network, so the receiver
can sit where the antenna is and be driven from anywhere in the house.
Decoders arrive as optional [plugins](#plugins) from an in-app catalogue.

Built clean-room — no GPL code and no GPL-linked dependencies anywhere in the
tree. That discipline is what leaves the licensing free to choose (see
[License](#license)); it is not itself a licence claim.

Internal project/binary name: `cascade`.

## Status

Working and in use, and still pre-1.0 — the interface and the plugin catalogue
are still moving. What is in the current build:

- **Receiver.** Spectrum and waterfall, NFM/WFM/AM/DSB/USB/LSB/CW, squelch,
  AGC, noise reduction, manual and automatic notch, de-emphasis, stereo FM with
  pilot lock, and RDS (programme service name, radio text, PI, PTY).
- **Hardware.** An RTL-SDR, a HackRF, an Airspy R2/Mini, an Airspy HF+, a
  Mirics MSi2500 or an RX888 mk2 through FoxSDR's OWN drivers, needing no
  SoapySDR install of any kind - only that the radio is bound to WinUSB
  (Zadig), which every SDR application needs anyway. An **SDRplay RSP** is
  driven natively too, through the SDRplay API the user installs, because
  SDRplay publish no device protocol and an RSP cannot be reached any other
  way. An **ADALM-Pluto** is driven natively over the network, with no libiio
  and no vendor module at all - it is the one radio whose address is typed
  rather than discovered, because a network cannot be walked. Anything else -
  a USRP, a LimeSDR - through SoapySDR as before.
  Antenna, sample-rate and per-stage gain selection on all of them,
  with each gain slider spanning what that stage will actually accept and
  lettered in the unit that stage is really measured in — decibels on every
  radio but the Airspy R2/Mini, whose five stages are the hardware's own
  register steps and are shown as bare step numbers rather than invented
  decibels — and a bias-tee switch on the radios that have one. Developed against an Ettus B200
  and an RTL2838 (R820T); the built-in signal generator and IQ-file playback
  mean it runs with no radio at all.
  A saved SoapySDR RTL-SDR, HackRF, Airspy, Airspy HF+, SDRplay, Mirics
  (`driver=miri`) or RX888 (`driver=sddc`) is OPENED NATIVELY on
  the next launch without being asked, and the log says so; a dongle whose
  tuner the native driver does not support (E4000, FC0012/13) falls back to the
  SoapySDR path and says why. The Source section lists the native radios first, labelled
  "(native)", and names any radio that is plugged in but not bound to WinUSB -
  an RTL dongle still on the DVB-T driver, an Airspy still on its vendor one, a
  television stick still on its DVB-T one - rather than leaving it silently
  missing. An RSP that cannot be listed because the SDRplay API is not
  installed gets a sentence saying exactly that, rather than nothing at all.
  The SoapySDR device scan still waits for the radio to close - the vendor
  probe opens and resets every dongle it finds, the streaming one included -
  but Refresh is live again, because the native enumeration reads SetupAPI
  properties and opens nothing.
- **Working with signals.** Bookmarks, a band scanner with a Skip key and a
  listen limit so a station that never goes quiet cannot stop it, and
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
- **A tuner plate for a counter**, bolted onto the front of the deck after a
  1950s military receiver: an olive-drab riveted plate with an engraved
  "TUNED - HERTZ" name plate, a receiver lamp and a MHz readout across its
  head, and a black bezel holding ten Nixie tubes - one per digit, 1 GHz down
  to 1 Hz - each with a chrome toggle switch beneath it. Scroll a tube to step
  that digit, click one to type a frequency, or flick the switch: the upper
  half steps the digit up, the lower half down, holding either repeats, and
  the lever stays pointing the way it was last flicked. The SAMPLE RATE and
  FRAME TIME meters at the deck's right are on the bar at the window size the
  application first opens with, not only once it is widened.
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
  coloured by altitude band, with optional map imagery from a basemap plugin.
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

**Verification, honestly stated.** The DSP core and every decoder carry unit
tests (`ctest` runs 82 entries: 75 test binaries and seven checks on the
application itself), and the audio chain has been confirmed by ear on
broadcast FM. Of the decoders, **ADS-B is the one confirmed against real
off-air signals** — aircraft decoded live, with ICAO address blocks and
callsigns agreeing across independent message types. The others (AIS, APRS,
SSTV, Morse, RTTY, POCSAG) are verified against synthesised signals and
published constants, which is real evidence but not the same thing. The
Inmarsat-C plugin is published at 0.1.0 and explicitly marked EXPERIMENTAL:
roughly ten of its air-interface constants are reconstructed guesses, and it
will most likely decode nothing off air. Each plugin's catalogue entry says
where it stands.

See [PLAN.md](PLAN.md) for the roadmap and architecture.

## The native HackRF driver

FoxSDR talks to a HackRF One directly — its own USB transport, its own reader
thread, no libhackrf, no libusb, no SoapySDR module in the path. Jawbreaker and
rad1o boards run the same firmware and are listed too.

**Why, given that SoapyHackRF exists.** Every crash this product has received
from a USB radio in its first months landed inside somebody else's libusb, on a
thread FoxSDR did not create, behind a vendor module installed from somebody
else's toolchain: a lock freed and reused, a reader we could not guard, an
enumeration probe that opened and reset the very dongle that was streaming.
None of that code is ours, so none of it could be fixed from here. A native
driver can be. Three rules hold throughout (`src/usb/usb_device.hpp`):
enumeration never opens a device, FoxSDR owns every thread that can be inside
the radio, and every wait is bounded — a wedged device costs a timeout, never a
frozen window.

**What it does.** 2–20 MS/s, with the MAX2837 baseband filter following the
rate automatically to the widest setting no more than 75% of it; 1 MHz to
6 GHz; LNA (0–40 dB in 8 dB steps), VGA (0–62 dB in 2 dB steps) and the
front-end amplifier as a two-position 14 dB gain; the bias-T as its own
control, deliberately not disguised as an antenna choice, because it is 3.3 V
on the connector rather than a choice of where to listen. A rate change on a
live stream is made with the radio quiet — receive off, reader stopped, ring
torn down, the new rate programmed, then all of it again — which is the shape a
live rate change had to be given on the SoapySDR path after one killed the
process on a driver's own reader thread. Opening a HackRF also puts it into a
known state (10 MS/s, 100 MHz, 16 dB of each gain, amplifier off, bias-T off),
because the hardware otherwise keeps whatever the last application left it at,
including a bias-T quietly feeding an antenna.

Asking for less than 2 MS/s is refused with a reason rather than silently
rounded up to 2: the documented floor is 2 MS/s, and a caller that wanted
narrowband behaviour being given twice the bandwidth is a lie nothing on screen
would reveal. Above 20 MS/s the request is coerced down and says so.

**Windows only for now.** The transport is WinUSB; on Linux the HackRF is still
reached through SoapySDR, and native enumeration returns nothing and says why
in the log. The protocol layer itself is plain C++20 and builds everywhere.

**How it is verified.** There is no HackRF on the bench this was written on, so
the proof is byte-exactness rather than a spectrum: `tests/test_hackrf_source.cpp`
drives the driver through a fake that implements the transport interface and
records every control transfer, and checks the request numbers, values, indices
and payloads against libhackrf — the sample-rate and filter arithmetic against
numbers produced by compiling libhackrf's own functions and running them, not
read off by eye. The streaming, device-loss and wedged-reader paths are proven
the same way. The protocol was ported from libhackrf under its BSD-3-Clause
licence; the notice is in `installer/THIRD-PARTY-LICENSES.txt`, and nothing of
libhackrf is linked or shipped.

## The native Airspy R2 / Mini driver

**Airspy R2 and Airspy Mini** are opened by FoxSDR's own driver, over the same WinUSB
transport as the RTL-SDR and the HackRF — no libairspy, no libusb, no SoapySDR module.
Bind the radio to WinUSB with Zadig and it appears in the Source list by itself.

The Airspy is a real-sampling receiver: its ADC runs at twice the rate the radio
advertises and the host turns that into complex samples, so a device set to 10 MS/s
streams 20 million 12-bit samples a second and FoxSDR delivers 10 MS/s of I/Q from them.
The rates on offer are read out of the radio's own firmware rather than compiled in, so
an R2 shows the two it has and a Mini shows its own; samples arrive packed twelve bits to
twelve, which is a third less USB traffic than the unpacked format for exactly the same
signal. The conversion uses libairspy's own half-band kernel, so the spectrum agrees with
every other Airspy application about where a signal is, and the mirror image that a
real-to-complex conversion has to suppress is measured 61 dB down.

Five gains are exposed. **LNA**, **MIXER** and **VGA** drive the R820T's three stages
directly, and the numbers are the hardware's own steps rather than decibels — libairspy
publishes no decibel mapping for them and FoxSDR does not invent one. Everywhere a gain
is shown — the Source sliders, the RECEIVER card, the scope deck's GAIN knob and the
browser interface — these five are lettered as bare step numbers with no unit, because
the honest thing to put after a register position is nothing. **LINEARITY** and
**SENSITIVITY** are libairspy's two curated walks up all three at once: linearity trades
sensitivity for headroom against a strong neighbouring signal, sensitivity does the
opposite. Unlike the HackRF the Airspy has automatic gain control, on the LNA and the
mixer, and FoxSDR's auto-gain switch drives both. The bias tee is a separate control —
a **Bias tee** checkbox below the gain sliders — and it is switched off every time FoxSDR
opens or closes the radio, so a previous application cannot leave 4.5 V on your antenna
port without anything on screen saying so. FoxSDR remembers the setting across restarts
and puts it back after the open, because a mast-head amplifier does not stop needing
power because the application was closed.

Tuning range 24 MHz to 1.75 GHz.

## The native Airspy HF+ driver

FoxSDR opens an Airspy HF+ directly too — the same WinUSB transport, the same
three rules, no libairspyhf, no libusb, no SoapySDR module in the path. The
HF+ Dual and the HF+ Discovery are the same two USB ids and both are listed.

**What it does.** The sample rates come off the DEVICE rather than out of a
table, because a Discovery's list differs from a Dual's and differs again with
firmware; so does which of those rates is zero-IF and which is low-IF, which of
the nine attenuator steps the board actually has, whether it has a bias tee at
all, and the crystal calibration stored in its own flash. 9 kHz – 31 MHz and
64 – 260 MHz, the two bands the manufacturer publishes, with the gap between
them refused rather than silently landed in. The attenuator is presented as a
NEGATIVE gain (−48 to 0 dB in 6 dB steps) so that louder is to the right like
every other control in the Source panel, the preamp as a two-position 6 dB
gain, and the hardware AGC — which no other native driver here has — with its
low/high threshold. Opening one puts it into a known state, because the
hardware otherwise keeps whatever the last application left it at.

**Three corrections that have to happen on the host.** An Airspy HF+ delivers
16-bit I/Q and nothing else: the filter gain the firmware reports for the
current rate, the rotation that undoes both the whole-kilohertz tuning grid and
the deliberate 5 kHz zero-IF offset, and the adaptive IQ balancer that rejects
the zero-IF image are all done here, on the reader thread, exactly as
libairspyhf does them on its own. Skip any of them and the radio is not broken,
it is subtly wrong — five kilohertz off, or showing a mirror of every signal
folded across the centre of the span. This is also how the receiver reaches
9 kHz: the local oscillator cannot go below 180 kHz, so it sits at its floor
and the whole difference is rotated out in software.

**Windows only for now**, like the other two and for the same reason: the
transport is WinUSB, and native enumeration returns nothing and says why in the
log on other platforms. The protocol and DSP layer is plain C++20 and builds
everywhere.

**How it is verified.** There is no Airspy HF+ on the bench this was written
on, so the proof is byte-exactness rather than a spectrum:
`tests/test_airspyhf_source.cpp` drives the driver through a fake that
implements the transport interface and records every control transfer, and
checks the request numbers, values, indices, lengths and payloads against
libairspyhf — the tuning arithmetic computed a second time in the test from the
reference's own formula and then again by hand as literal wire bytes, because
two implementations that agree are worth more than one that is merely
self-consistent. The image rejection is measured rather than asserted: a tone
damaged by 2% of amplitude and phase error starts 31.0 dB above its image and
ends 76.6 dB above it after 700 blocks, with the estimator landing on the exact
imbalance applied. Streaming, device loss, a wedged reader and a firmware too
old for five of these requests are all proven the same way. The protocol and
the balancer were ported from libairspyhf under its BSD-3-Clause licence; the
notice is in `installer/THIRD-PARTY-LICENSES.txt`, and nothing of libairspyhf
is linked or shipped.

---

## The SDRplay driver

An RSP is the one radio FoxSDR cannot reach the way it reaches the others.
SDRplay publish no device protocol, and the tuner is programmed by a Windows
service that owns the USB handle — so the only door is `sdrplay_api.dll`, and
the driver goes through it directly: FoxSDR loads the user's own SDRplay API
3.x at run time, resolves the entry points it needs, and drives the API itself.
No SoapySDR module in the path, nothing of SDRplay's linked at build time, and
nothing of theirs in the installer. If the API is not installed, no RSP is
listed and the Source panel says exactly what to do about it: install the
SDRplay API 3.x from sdrplay.com and restart FoxSDR.

**What it does.** RSP1, RSP1A, RSP1B, RSP2, RSPduo, RSPdx and RSPdx-R2, 1 kHz
to 2 GHz, 62.5 kS/s to 10 MS/s. Everything below 2 MS/s is produced the way the
hardware actually produces it — the binary fractions of 2 MS/s by decimating a
6 MHz front end at the 1.62 MHz IF, the audio rates (96/192/384/768 kS/s) by
decimating a fast zero-IF one — because the RSP's front end does not run below
2 MS/s and getting that wrong puts the receiver 1.62 MHz off frequency. The IF
gain is presented as a NEGATIVE gain (−59 to −20 dB) because the hardware's own
number is a gain REDUCTION and a slider whose right-hand end is quieter is the
kind of inconsistency that gets blamed on the radio; the LNA is presented in
steps, not invented decibels, because the API takes an index into a per-model,
per-band table and publishes no decibel mapping for it. The API's own AGC is
there with its set point. An RSPdx's three inputs and an RSPduo's two tuners
appear as antennas; the bias tee, the broadcast-FM and DAB notches and the
RSPdx's HDR mode are switches of their own, per model, because a control that
can put power on a connector should never be reachable by something iterating a
list of port names. ADC overloads are logged AND acknowledged — the service
re-reports one until it is, so a host that merely logs gets a log full of it —
and a device removal or a service failure stops the stream with a reason rather
than freezing the display.

**Windows only**, because the SDRplay API is a Windows service. On other
platforms enumeration returns nothing and says why in the log.

**A note on versions.** The driver's declarations of the API's structures were
checked against SDRplay's published headers for 3.07, 3.11 and 3.15, compiled
with FoxSDR's own compiler, and every size and member offset is pinned by a
`static_assert` — a layout change fails the build instead of quietly writing
into the wrong field of the service's memory. Two members did move between
those versions, and both are handled rather than guessed: the device list's
`valid` flag is not believed below 3.08, where it is padding, and the RSPdx's
HDR bandwidth is left at the API's default below 3.15, where it sits four bytes
lower in the parameter block. Anything older than 3.07 is refused with a
sentence naming the version it found.

**How it is verified.** There is no RSP on the bench this was written on and
the SDRplay API is not installed on it, so nothing here has met the real
service — that is stated plainly rather than implied. What is proved is the
driver: it reaches the API through one table of function pointers, and
`tests/test_sdrplay_source.cpp` fills that table with a fake that answers the
way the vendor's header says the service does. 442 checks cover the exact call
sequence at open, the exact parameters the radio is started with, every update
reason against the change that caused it, the sample conversion, the per-model
antenna, notch and bias-tee routing, the rate-and-decimation plan for every
published rate, the refusals (no API, an API too old, a frequency out of range,
an RSPduo another application already holds), the fault path for a removed
device, and the teardown bound. Each was confirmed to go red against a
deliberately broken driver before being believed.

## The native Mirics driver

**Mirics MSi2500 (native).** FoxSDR opens a Mirics MSi2500 + MSi001
receiver directly over its own WinUSB transport - no SoapySDR module, no
libusb and no vendor runtime. That covers the Mirics reference device, the
early SDRplay RSP1 and RSP2, and the television sticks built on the same
pair (Hauppauge WinTV 133559 LF, AverMedia A859, IO-DATA GV-TV100, Logitec
LDT-1S310U/J). It tunes 150 kHz to 2 GHz through the tuner's five bands,
runs 1.3 to 15 MS/s, and offers the tuner's own gain stages separately -
low-noise amplifier (24 dB), mixer (19 dB) and baseband (0 to 59 dB), plus
the buffer that stands in for the amplifier on the medium-wave inputs. The
baseband filter follows the sample rate automatically. Like every USB radio
in FoxSDR, the device has to be bound to WinUSB first (see Zadig, below);
a stick still running its television driver is listed separately with what
to do about it.

## The native RX888 mk2 driver

FoxSDR opens the RX888 mk2 natively over its own WinUSB transport: no CyAPI
or Cypress driver install, no SoapySDR module, and no separate firmware
loader. The FX3 firmware is built into FoxSDR and uploaded automatically —
an RX888 has no flash, so out of a power cycle it is a Cypress bootloader
rather than a receiver, and the Source section lists it as "RX888 (needs
firmware, will load on open)" and loads the image when you open it. That
takes a few seconds the first time after each power cycle and nothing
thereafter.

**Zadig has to be run twice**, because an RX888 has two USB identities and
FoxSDR has to reach both: `04B4:00F3` (the FX3 bootloader, before the
firmware) and `04B4:00F1` (the radio, after it). See the Zadig section below.

Direct sampling gives the whole 0 – 32 MHz band from the 64 MHz ADC clock,
and above that the R828D tuner takes over up to 1.75 GHz. The sample rates
offered are 2, 4, 8, 16 and 32 MS/s complex, each a power-of-two decimation
of the ADC clock; above 32 MHz the widest is 8 MS/s, because the tuner puts
its output at a 4.57 MHz IF and a wider band would reach below 0 Hz. The
gains are the DAT-31 attenuator (−31.5 to 0 dB in half-decibel steps) and the
AD8340 IF amplifier on the HF side, and the tuner's own RF and IF gains on
the VHF side; the ADC's dither and output randomiser, the HF PGA and both
bias tees are switches on the radio.

**Be honest with yourself about the rate.** The RX888 delivers 128 MB/s and
the conversion from real samples to complex baseband happens on your
computer, not on the radio. FoxSDR's DSP chain has a measured single-core
ceiling around 16 – 20 MS/s, so 32 MS/s is offered because the hardware has
it, not because this application can keep up with it on every machine. If
the diagnostic log's `stream health` line shows overflows climbing, drop a
rate.

## The native ADALM-Pluto driver

An **ADALM-Pluto** is opened by FoxSDR's own driver, over the network — no
libiio, no libad9361, no SoapySDR module. It is the first radio FoxSDR reaches
that is not on the USB bus at all: a Pluto presents a USB Ethernet gadget and
runs an `iiod` daemon on TCP port 30431, so where the other native drivers send
control transfers this one sends one-line text commands, and where they read a
bulk endpoint this one reads a socket.

**Receive only.** The Pluto is a transceiver; this is a receiver driver, and
what a transmit path would need is written down in `src/source/pluto_source.hpp`
rather than half-built.

**The limits come off the board, not out of a table.** A Pluto may be a stock
AD9363 that tunes 325 MHz to 3.8 GHz, or one with the AD9364 unlock applied that
reaches 70 MHz to 6 GHz, and the same firmware serves both — so publishing a
range would be wrong for half of the owners in one direction or the other.
FoxSDR reads the tuning range, the sample-rate range, the bandwidth range and
the gain range out of the board's own `*_available` attributes when it opens,
reports those, and says in the log which kind of board it found. When a board
publishes no range at all, FoxSDR says the range is unknown rather than
inventing one.

**What it does.** Whatever rate the board says it takes (2.083–61.44 MS/s on a
stock one), with the AD9361's analogue bandwidth following the rate
automatically — including at open, because a Pluto keeps whatever bandwidth the
last application left it at and there is nothing on screen that would reveal an
inherited 200 kHz filter. One RX gain in real decibels, and the AD9361's own
automatic gain control (slow attack, the mode that settles rather than the one
that chases bursts); moving the gain by hand switches the AGC off first, because
the chip ignores a manual gain while it is attacking and a slider that moves and
changes nothing is worse than one that refuses. Two connections are opened, a
control one and a stream one, so a retune does not queue behind a sample buffer
the board has not sent yet.

Asking for a rate below the board's own minimum is refused with a reason rather
than silently rounded up: going lower needs a FIR filter written into the
AD9361 through `filter_fir_config`, which FoxSDR does not generate. Above the
maximum the request is coerced down and says so.

**Every wait is bounded.** A connect gives up after three seconds; every send
and receive after two. A Pluto unplugged mid-stream produces a socket error and
a stopped source within that, never a frozen window.

**How it is verified.** There is no Pluto on the bench this was written on, so
the proof is the protocol rather than a spectrum: `tests/test_pluto_source.cpp`
runs a fake `iiod` on the loopback interface — a real socket, real threads,
written from the daemon's own published grammar — and checks the whole
conversation command for command, including the channel mask that has to be
read back before every sample buffer and the byte counts on every attribute
write. What cannot be verified here is the content of a real board's context
description, so the tests are written not to depend on it: the same code is
served two different boards, and the driver's answers have to differ
accordingly. A driver with a built-in table passes neither. The protocol
description came from libiio's daemon under its LGPL-2.1 licence; the notice is
in `installer/THIRD-PARTY-LICENSES.txt`, and nothing of libiio is linked or
shipped.

## The native RTL-SDR driver

FoxSDR opens an RTL2832U dongle directly as well — the same WinUSB transport,
the same rules, no librtlsdr, no libusb, no SoapySDR module in the path. The
generic Realtek ids and the several dozen rebadged dongles that share the chip
are all listed.

**Why.** The first month of crash reports from RTL-SDR users is the reason this
exists at all: every one of them landed inside `libusb-1.0.dll`, reached
through a vendor module that came from somebody else's install, on a reader
thread FoxSDR did not create. The remedies available were "restart FoxSDR" and
"do not scan for devices while streaming", and both of those are apologies
rather than fixes. This driver owns its USB traffic, its reader thread and its
enumeration, so a radio that goes wrong is something this code can be held
responsible for.

**What it does.** The standard rate ladder from 250 kS/s to 3.2 MS/s, with the
tuner's IF filter following the rate and the demodulator's down-converter
re-pointed at the intermediate frequency the filter settles on; 24 MHz to
1766 MHz on an R820T or R820T2; three named gains (LNA, MIXER and VGA) taken
from the measured step tables, plus the single aggregate "TUNER" ladder every
other RTL-SDR application offers, so a setting means the same thing here as it
does there; the tuner's automatic gain, with the demodulator's own digital AGC
brought in step so the two cannot fight over one signal; the crystal trim in
parts per million; and the bias tee as its own control. A rate change on a live
stream is made with the stream stopped and restarted, because the resampler is
reset as part of the change and a stream running across that reset delivers
half a buffer of each rate.

**The bias tee is off on every open** unless the dongle's EEPROM says it is
wired permanently on — and the EEPROM's own header is checked before that byte
is believed, because a dongle with no EEPROM at all reads as zeroes and zero
means "force it on". Switching 4.5 V onto somebody's antenna because their
dongle had no configuration memory is not a default worth having.

**The RTL-SDR Blog V4** is supported as a V4 rather than as a generic R828D:
below 28.8 MHz the tuner is asked for the upconverted frequency, the dongle's
own GPIO throws the upconverter switch, the tracking filter is bypassed on that
path, and the notch filters open inside the bands they notch. Without that a V4
hears nothing at all below 24 MHz. On any other R82xx dongle, tuning below
24 MHz switches the demodulator to direct sampling instead, which is what the
common HF modification wires an antenna to.

**Windows only for now**, like the HackRF driver and for the same reason: the
transport is WinUSB. On Linux an RTL-SDR is still reached through SoapySDR, and
native enumeration returns nothing and says why. **A dongle must be bound to
WinUSB** (with Zadig) to be opened natively; one still running the DVB-T driver
is not listed, because it cannot be opened.

**How it is verified.** Both ways, because neither alone is enough.
`tests/test_rtlsdr_source.cpp` drives the whole driver through a fake transport
that records every control transfer and asserts the exact register sequences —
the resampler ratio and the achieved rate, the down-converter's three bytes, the
tuner's PLL divider and sigma-delta at 100 MHz and at 1090 MHz, the gain ladder,
and the V4's upconverter path at 7 MHz — against arithmetic worked out from the
chips' behaviour and written out in the test beside each expectation.
`tests/test_rtlsdr_live.cpp` then measures a real dongle: five seconds at
2.4 MS/s must deliver within 2% of the rate the radio reports, the samples must
be neither silent nor saturated, a rate change on a live stream must deliver the
new rate, and `stop()` must return within 500 ms while samples are still
flowing. On a machine with no dongle that file says so in one line and passes,
rather than passing quietly for the wrong reason.

The driver is an independent implementation written from the register-level
behaviour of the RTL2832U and R82xx. librtlsdr is GPL-2.0 and is not linked or
shipped; see `installer/THIRD-PARTY-LICENSES.txt` for the full position.

## Building (Windows)

Requires Visual Studio 2022 Build Tools and CMake. All GUI/DSP/audio/JSON
dependencies (Dear ImGui, GLFW, PortAudio, nlohmann/json, pffft) are vendored
at pinned revisions under `third_party/` and built from source — see
[third_party/THIRD_PARTY.md](third_party/THIRD_PARTY.md). The only external
dependency is SoapySDR, consumed from vcpkg at `C:\vcpkg` (`soapysdr`
installed): it is the hardware ABI boundary — runtime vendor modules
(SoapyUHD etc.) must match the system SoapySDR ABI, so it is deliberately
not vendored.

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

**Photographing the window.** Press **F12** in a running FoxSDR and it writes
what it has just drawn, from its own framebuffer, as `shot-<frame>.bmp` into
`FOXSDR_SHOT_DIR` (or the current directory), and logs the path. Set
`FOXSDR_SHOT_AT_FRAME=<n>` to have it take one at that frame without a key,
which is what a script wants. This exists because a screen grab cannot always
see an OpenGL window — on one desktop here PrintWindow returned white and a
desktop capture showed the icons through the window while it was plainly on
screen — and the only picture always true to what was drawn is the one the
application takes of itself. A torn-off window (a map, a picture) is its own
framebuffer and is not in that picture; set `FOXSDR_SINGLE_VIEWPORT=1` to keep
every window inside the main one for a sweep. A rail section's open/closed
state is not in the config a script can pre-write (only which BANK is showing
is); `FOXSDR_OPEN_SERIAL_PORTS=1` opens SYSTEM's Serial ports section on the
first frame it is drawn, for a shot that needs to show its contents.

## Building (Linux — in development, see the notice at the top)

**This is unfinished work.** It builds and the tests pass, but no plugins are
available, it has never been driven with a radio on Linux, and it has never run
on a real desktop session. Build it to experiment or to help find what is
broken, not to use as a receiver.

The same vendored dependencies build from source here too. Three system
packages are needed: OpenGL headers, SoapySDR, and OpenSSL — the last of these
supplies SHA-256, PBKDF2 and secure randomness, which on Windows come from the
operating system's own CNG and need no package.

On Debian or Ubuntu:

```
sudo apt install build-essential cmake libgl1-mesa-dev libsoapysdr-dev libssl-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libwayland-dev libwayland-bin wayland-protocols libxkbcommon-dev libasound2-dev
```

The window layer builds for both X11 and Wayland. `libwayland-bin` is easy to
miss because it supplies a build tool rather than a library: without it the
configure step fails looking for `wayland-scanner`.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

Audio goes through ALSA. On a machine whose audio is managed by PulseAudio or
PipeWire, install `libasound2-plugins` so ALSA's default device routes to the
sound server rather than claiming the hardware directly.

**Plugins: one catalogue, both platforms.** The catalogue lists every build of
a plugin and each installation picks the one matching its own os and
architecture, so a Windows and a Linux machine read the identical file and
install different binaries from it. Ten of the eleven plugins ship for both;
only the example plugin is Windows-only, and it is built from this repository
rather than the plugin repository.

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

Decoders can be installed as separate native plugins, from an in-app
catalogue. Two keys in the **DECODE** group of the FUNCTION SELECT rail open
the two windows this lives in: **Plugin store** is the catalogue — what each
module is, what it reaches for, what it costs to fit, and the updates the
catalogue offers — and **Plugins** opens **Fitted modules**, which is what is
already installed on this machine, which of them are being fed, which were
refused and why, and the keys that start, stop, remove and permit them. Each
is a real window you can drag as large as your screen. **Fitted modules**
comes back at the size and place you left it, because its rectangle is
written to the configuration file with everything else; the store's is not,
so it holds what you drag it to only until FoxSDR closes. Neither reopens by
itself: since 0.79.1 FoxSDR always starts on the main window alone, whatever
was showing when it was closed - no page, no map, no decoder output, and the
bench rather than the radar scope. Every one of them is a key on the rail
away, and nothing opens a window but your own hand. A plugin's own window -
a decoded picture, a plugin's panel - has a row of its own under DECODE,
with a chip saying what it holds (WAIT, RX or IMG for a picture, a row count
for a panel): press the row to open it, its key to close it. (Before 0.79.1
the pages you left open came back, the decoder output opened itself on a
decoder's first line, a map page opened itself on its first target, and a
plugin's window appeared whenever the plugin published one.) The catalogue is contacted only when you
press **CHECK NOW** inside the store window: nothing is fetched at startup, and
no plugin ever updates itself.

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
that can do this says so on its own plate before you fit it.

**Stop and start.** In the Fitted modules window every loaded module's row
carries a **STOP** key, and a stopped one carries **START**; the selected
module's plate carries the same as **STOP MODULE** / **START MODULE**.
Stopping destroys everything that plugin had
running — its decoders, its map targets and trails, its window, its basemap
tiles — while leaving the module loaded and the row where it was, so a stopped
plugin decodes nothing, draws nothing, and cannot move the receiver. The row
then reads **STOPPED BY YOU**, lettered in plain ivory rather than in anything
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
already says so on that module's row, in gold, and quotes the reason on its
plate, and taking the sound away on behalf of a decoder the
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
CONTROL** on that module's plate in the Fitted modules window. The key is
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

Security model, in one line: every download is https, sha256-verified against
the catalogue before it is allowed to become a file, size-capped, refused on a
cross-host redirect, and written under a sanitised bare filename inside the
plugins directory.

**Where that directory is** depends on whether the application can write to its
own: a portable copy keeps plugins in `plugins/` beside the executable, while an
installation under a directory the user does not own — `C:\Program Files\FoxSDR`
being the ordinary case — uses `%LOCALAPPDATA%\foxsdr\plugins` instead
(`$XDG_DATA_HOME/foxsdr/plugins`, or `~/.local/share/foxsdr/plugins`, on Linux).
The Fitted modules window prints the one in use, in full, under **MODULES ARE
READ FROM**, and each module's plate says which file it was **LOADED FROM**.
Nothing needs elevating either way.

Compatibility is ABI-exact. A plugin must be built against this host's
`src/core/plugin_abi.h` and declare exactly its ABI version — a near miss is
refused rather than loaded, because a struct-layout difference becomes memory
corruption days later. A plugin built for an older FoxSDR therefore needs a
new build from its author; no update can fix it.

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
| Save a screenshot | <kbd>Ctrl</kbd>+<kbd>W</kbd> | |
| Maximise / restore the window | <kbd>F11</kbd> | |
| Bank: SIGNAL PATH / DECODE / VIEW / EXTEND / SYSTEM | <kbd>F1</kbd>…<kbd>F5</kbd> | |
| Settings and key bindings | <kbd>F7</kbd> | |

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
  waterfall and the radar scope are all the same tube.
- **Rust is trouble**, and never a reading, so a fault can never be mistaken
  for a figure.

The controls live on the **FUNCTION SELECT** rail down the left, and the rail
is a bank selector, the way a bench instrument's is: five brass pushbuttons
under its title - **SIGNAL**, **DECODE**, **VIEW**, **EXTEND**, **SYSTEM** -
one of them pressed in with a phosphor strip lit beneath it, and the column
below showing that bank's sections and nothing else. SIGNAL is the receiver
itself (source, radio, audio filters, sinks, the recorder); DECODE is the
plugin store, the fitted modules, the decoders, target details and the
satellites map; VIEW is the display range, the radar scope, bookmarks and the
scanner; EXTEND is browser access and CAT control; SYSTEM is updates,
diagnostics and usage reporting. **F1 to F5** press the same five keys from
the keyboard. Each section still opens and closes with its own key and
keeps that state as you move between banks; a section unfolds rather than
appearing, a bank comes up like a lamp rather than switching in one frame, and
the rail opens on whichever bank you left it on — the only part of this the
configuration file records, so a section comes back at its usual state on
the next launch. Every chip on a row still reports what that section is
doing without opening it.

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
instrument and map window back to its default size and position. On Linux the
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
and which radio model, against a random identifier created on your machine.
Switching it off deletes that identifier.

**It never sends frequencies, anything decoded, your location, or your IP
address**, and hardware serial numbers are stripped before the radio model is
sent. The complete list of what is and is not transmitted is in
[PRIVACY.md](PRIVACY.md), the payload is held to it by an automated test, and
the receiving endpoint's source is in [telemetry-worker/](telemetry-worker/).

**Everything else that reaches the network, in full.** Apart from the usage
report above, these are the only things this application sends or fetches, and
each says what starts it:

- **The update check.** Once per launch, to foxsdr.com, sending the version you
  are running and nothing else — no identifier and no cookie. It is on by
  default and is the **Check for updates at startup** tick in **SYSTEM →
  Updates**; nothing is downloaded or installed unless you press the button.
  It is not the usage report and the two share nothing.
- **The plugin catalogue.** Fetched only when you press **CHECK NOW** in the
  Plugin store window. Nothing is fetched at startup — since 0.79.1 that
  window does not even reopen by itself, and opening it by hand fetches
  nothing either — and no plugin ever updates itself.
- **A plugin download**, when you press **FIT MODULE** or **UPDATE MODULE** for
  one — always https, sha256-verified against the catalogue, size-capped and
  refused on a cross-host redirect.
- **A crash or freeze report**, and only with Diagnostics on: the report's text
  goes out the *next* time you open the application, never from inside the
  fault. See [When it crashes or freezes](#when-it-crashes-or-freezes).

A plugin is native code loaded into this application's own process, so an
installed plugin may make requests of its own — a basemap plugin fetching map
tiles from the server you pointed it at is the ordinary case. That is the
plugin's traffic, not the host's, and it is why each module's plate says
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
[PRIVACY.md](PRIVACY.md) lists the payload field by field, and an automated test
holds the request, the code and that document to each other in both directions.

- A crash writes a report to `%LOCALAPPDATA%\FoxSDR\crashes\` naming the fault,
  the faulting module and offset, the loaded modules, and the last 256 log
  lines.
- A **freeze** does too, which matters more: every fault this product has
  actually shipped was a hang rather than a crash, and a crash handler catches
  none of those. If the window stops responding for five seconds, a watchdog
  captures the stack of **every** thread — a deadlock is only legible as a pair
  — and then lets the application carry on if it recovers.
- **Closing counts as well, on a longer clock.** Shutting down is where this
  product's worst freeze ever happened, so the watchdog stays armed right
  through it — but closing legitimately waits on the radio driver for a few
  seconds, so the teardown is given twenty rather than five. Until 0.75.0 it
  was judged by the same five, and a slow but perfectly healthy close could
  file a freeze report against an application that had already exited. The
  report says which clock it was measured against.
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
  system, loaded plugins with versions, source and device state and the recent
  log on your clipboard, so you can read it before you send it to anybody. Off
  means off — no directory, no file, and off from the first instruction the
  application runs rather than from its first frame: the switch is read before
  anything is armed or created.

What a report contains is listed field by field in [PRIVACY.md](PRIVACY.md) and
held to that list by an automated test. It never contains a frequency, anything
decoded, or your position. A full memory dump is off by default, written locally
if you switch it on, and never sent by the application.

The engineering side — how a handler writes a report from a broken process,
how the five-second threshold is derived and measured, and where the symbols
that make a report readable are kept — is in
[docs/DIAGNOSTICS.md](docs/DIAGNOSTICS.md).

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
speaks to the board's own daemon over the network. Any OTHER radio - a USRP, a
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
bound to WinUSB, which tuner answered, the gain ranges, and how many samples
arrived in three seconds.

**The hardware search runs in a separate short-lived process.** Looking for
radios means loading every SDR driver installed on the machine and letting each
one scan the USB bus, and a driver that falls over while doing that used to
take the whole application with it. It now takes only that small process, and
the session carries on with an empty device list. If a scan finds nothing, the
diagnostics log distinguishes "no devices" from "the search crashed" and from
"a device stopped answering and the search was cut off" — three answers that
used to look identical.

## Building the installer

A Windows installer (Inno Setup 6) lives under `installer/` — it packages
`cascade.exe`, `SoapySDR.dll`, the app-local MSVC runtime, the license, and
post-install hardware notes into
`installer\Output\foxsdr-setup-<version>.exe`. Build instructions:
[installer/README-installer.md](installer/README-installer.md). Radio
hardware support is installed separately by the user (PothosSDR or
radioconda) — see `installer/POSTINSTALL.txt`; the app runs with no hardware
at all (signal generator + IQ playback).

### Stable and nightly

Two channels are published. **Stable** is built from a release commit and is
the version the download page offers by default. **Nightly** is the same
product built from `master`, produced by `tools/build-nightly.ps1`, and stamped
`<next>-nightly.<date>.<sha>` — a pre-release version that sorts *before* the
release it is heading towards, so nothing can mistake one for the other.

The version is injected by the build (`CASCADE_VERSION_STRING`, defaulting to
the `project()` version) rather than written into `version.cpp`, so the file
name, the About line, the usage report and the bug-report form all carry the
same string. That is the point of the arrangement: a nightly whose binary
called itself by the release version would produce bug reports naming a build
that does not exist. `cascade --version` prints it, `ctest` pins the format,
and the nightly script refuses to package a build whose binary disagrees with
the name it is about to be given — or one whose tests fail.

### Symbols, and why the archive is not in this repository

A crash report from the field is a list of `module+offset` pairs. Turning one
back into a function and a line needs the PDB produced by **that link** — not a
rebuild of the same source, not the same version built on another machine. PDBs
are not shipped to users, so a PDB not kept at build time does not exist
anywhere afterwards, and every report ever filed against that build is
unreadable hex forever. There is no repairing that later.

So every build archives its own PDB, keyed by the **PE build id** (the CodeView
GUID and age the linker stamps into the binary — the only key that tells a
release from the nightly heading towards it, or one rebuild from the next).
That happens in a CMake `POST_BUILD` step, `tools/archive-symbols.ps1`, so it is
part of the build rather than something to remember.

**`symbols/` is gitignored**, because a PDB is tens of megabytes of binary per
link and committing one per build would make this repository unusable inside a
fortnight. **It is not local-only either**: `tools/build-nightly.ps1` mirrors
each shippable build's symbols to `nas:/volume1/foxsdr-symbols` over SSH, in the
*same step* that compiles the installer, and a mirror failure fails the nightly.
Local-only would mean one disk failure permanently destroying the ability to
read every crash report against every build already in users' hands, and that is
not a risk worth accepting for a product that is sold. `-SkipSymbolMirror`
exists for a deliberately offline build and says plainly what it is risking.

`tests/test_diagnostics.cpp` asserts that *this* build's PDB really is in the
archive under the build id a report would quote. Full detail:
[docs/DIAGNOSTICS.md](docs/DIAGNOSTICS.md).

## License

**Free for noncommercial use. Commercial use requires a paid licence.**

The application is licensed under the
[PolyForm Noncommercial License 1.0.0](LICENSE). Hobbyists, amateur radio
operators, students, charities, schools and public bodies may use, modify and
share it at no cost. Using it in a business, selling it, bundling it with
hardware, or building a product from it requires a commercial licence — see
[COMMERCIAL-LICENSE.md](COMMERCIAL-LICENSE.md) for the tiers and
[COMMERCIAL-AGREEMENT.md](COMMERCIAL-AGREEMENT.md) for the actual terms.

If you are a hobbyist, that free licence is permanent and unconditional. There
is no trial period, no registration, no licence key, no activation, and no
feature withheld to sell you later. It will not be withdrawn
from under you: this project is funded by companies paying for commercial
licences, which is precisely what keeps it free for everybody else.

Modification is expressly permitted under the noncommercial licence; a
modified version simply remains noncommercial-only, and must carry the same
terms and the `Required Notice:` line.

Two deliberate exceptions:

- **`src/core/plugin_abi.h` is MIT.** Every plugin must include it, so it is
  licensed permissively on purpose — anyone, including commercial vendors, can
  write plugins without needing a licence from us.
- **Bundled third-party components keep their own permissive licences**
  (Dear ImGui MIT, GLFW Zlib, PortAudio MIT-like, nlohmann/json MIT, pffft
  BSD-style, SoapySDR BSL-1.0) and are unaffected by the terms above; see
  `THIRD-PARTY-LICENSES.txt`. FFTW and librtlsdr are intentionally not used —
  keeping every dependency permissive is what makes this licensing choice
  possible at all.

FoxSDR is an independent project, not affiliated with or
endorsed by SDR++ or its authors.
