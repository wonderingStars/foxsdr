# Radios and drivers

Every radio FoxSDR drives, what each driver does, and how far it has been verified. Moved here from the front page; the text is the original.

Back to the [README](../README.md).

> ### Linux status
>
> **Linux is actively supported.** Releases ship a Linux AppImage and a plain
> tarball beside the Windows installer, and the same CI run that builds and
> tests Windows on every push builds, tests and smoke-runs Linux; Linux bugs get fixed the same way Windows
> ones do, and a Linux crash writes and can upload the same report a Windows
> one does (`src/core/crash_handler_posix.cpp`, `src/core/crash_upload.cpp`).
> What is genuinely still catching up to Windows:
>
> - **The native USB drivers reach their radios on Linux through their own
>   usbfs transport, not yet confirmed against real hardware.** Since 0.97.0
>   the RTL-SDR, HackRF, Airspy R2/Mini, Airspy HF+, RX888 mk2 and Mirics
>   drivers (and, from 0.99.66, the HydraSDR RFOne's) talk to their radios on Linux through `src/usb/usbfs_device.cpp`,
>   the kernel's own usbfs interface — no libusb, no SoapySDR module; the udev
>   rule in `installer/linux/` grants a desktop user access to the device
>   nodes. That transport is written and unit-tested on a machine with no
>   radio attached, so the hardware path is unproven — see
>   `installer/linux/README.md` for exactly what is and is not verified. Any
>   radio SoapySDR itself can reach already works the same as on Windows.
> - **SDRplay on Linux is the same driver over a different transport, also
>   not yet confirmed against real hardware.** It `dlopen()`s the vendor's own
>   `libsdrplay_api.so.3` (from SDRplay's `.run` installer) instead of talking
>   to a Windows service, and that path is likewise unexercised against a real
>   RSP.
> - **21 of the 28 catalogued plugins ship a Linux build today** and install
>   from the in-app catalogue, including the aircraft registry lookup and the
>   map basemap. The seven still Windows-only are the newest instrument
>   decoders — ACARS, FLEX, 406 MHz beacons, ERT utility meters, 433 MHz
>   weather sensors and WEFAX — and the example plugin.
> - **Not yet run on a real Linux desktop with a real sound card** by anyone
>   on the project — it has been exercised under WSL and under a virtual
>   display in CI. If you run it for real, a report either way is useful.
>
> None of that is a reason to wait rather than install it: point a radio at
> it and tell us what breaks. It will be announced as fully proven once the
> hardware items above have a real report behind them.

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
parts per million (the **PPM frequency correction** switch since 0.99.56, in
whole ppm); and the bias tee as its own control. A rate change on a live
stream is made with the stream stopped and restarted, because the resampler is
reset as part of the change and a stream running across that reset delivers
half a buffer of each rate.

**The bias tee is off on every open** unless the dongle's EEPROM says it is
wired permanently on — and the EEPROM's own header is checked before that byte
is believed, because a dongle with no EEPROM at all reads as zeroes and zero
means "force it on". Switching 4.5 V onto somebody's antenna because their
dongle had no configuration memory is not a default worth having.

**Turning it on** is the same **Bias tee** checkbox the other radios have, below
the gain sliders in the Source section, and it appears for every RTL-SDR opened
natively. Tick it to power a mast-head LNA from an RTL-SDR Blog V3 or V4, a
NooElec stick with a bias tee, or any other dongle whose maker wired one to the
RTL2832U's GPIO 0 (on a dongle with nothing on that pin the box does nothing).
The box always shows what the dongle actually did, so a switch it refused leaves
the box where it was. FoxSDR remembers the setting against **that dongle, by its
serial**, and puts it back the next time that dongle opens — but never on
another dongle, never on one listed by position rather than serial, and never on
a dongle with no EEPROM: such a dongle has no serial of its own to be recognised
by, so its bias tee comes on only when you tick the box in that session. A
dongle whose EEPROM forces the bias tee on opens with the box ticked, as it
always has; untick it and FoxSDR switches it off and remembers that for that
dongle too. The HackRF, Airspy and other radios keep their own separate setting,
so ticking the box for one does not put power on the other. Two dongles
programmed with the same serial (many ship as `00000001`) cannot be told apart;
give each its own with `rtl_eeprom -s` if you use more than one.

**The RTL-SDR Blog V4** is supported as a V4 rather than as a generic R828D:
below 28.8 MHz the tuner is asked for the upconverted frequency, the dongle's
own GPIO throws the upconverter switch, the tracking filter is bypassed on that
path, and the notch filters open inside the bands they notch. Without that a V4
hears nothing at all below 24 MHz. On any other R82xx dongle, tuning below
24 MHz switches the demodulator to direct sampling instead, which is what the
common HF modification wires an antenna to.

**Native on Windows (WinUSB) and Linux (usbfs) alike**, like the HackRF driver,
through the same `src/usb/usb_device.hpp` transport contract — see "Linux
status" at the top of this file for what is and is not yet confirmed against
real Linux hardware. **On Windows a dongle must be bound to WinUSB** (with
Zadig) to be opened natively; one still running the DVB-T driver is not
listed, because it cannot be opened. **On Linux nothing needs unbinding** —
`installer/linux/README.md` explains why.

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

**Native on Windows (WinUSB) and Linux (usbfs) alike**, through the same
`src/usb/usb_device.hpp` transport contract — see "Linux status" at the top of
this file for what is and is not yet confirmed against real Linux hardware.
The protocol layer itself is plain C++20 and builds everywhere.

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

**Three gain modes, one at a time** (0.99.41), as Airspy's own software offers them:
**Sensitive**, **Linear** and **Free**. Sensitive and Linear are one **Gain** slider each,
0 to 21, over libairspy's two curated walks up all three of the R820T's stages at once —
linearity trades sensitivity for headroom against a strong neighbouring signal,
sensitivity does the opposite. **Free** is the three stages by hand — **LNA**, **MIXER**
and **VGA** — with the LNA's and the mixer's own automatic gain control each switched on
or off (**LNA AGC**, **Mixer AGC**); a stage its AGC is driving is greyed, and goes back
to its own number when the AGC is switched off. Only the chosen mode's controls are shown,
and each mode keeps its own values while another is in use. The numbers are the
hardware's own steps rather than decibels — libairspy publishes no decibel mapping for
them and FoxSDR does not invent one — so everywhere a gain is shown (the Source sliders,
the RECEIVER card, the scope deck's GAIN knob, the browser interface) it is a bare step
number. The browser and the plugin API see only the chosen mode's gains too; naming a
gain from another mode switches to it in the browser, but the plugin API refuses any
gain name outside the current mode's published list (CASCADE_API_UNSUPPORTED) rather
than switching modes on a plugin's behalf. FoxSDR's generic auto-gain switch is Free
mode with both AGCs on.

**Decimation** (0.99.41): none, 2, 4, 8, 16 or 32 on an R2 and up to 64 on a Mini, as in
Airspy's own software. It divides what the radio delivers by that factor before anything
else sees it — half-band filters, flat and alias-free over the inner 80% of the new band
and more than 90 dB down on everything that would fold into it — so the span narrows, the
whole receiver does proportionally less work, and each halving leaves about 3 dB less
noise per sample. The Rate list then shows the delivered rates (an R2 at 10 MS/s under 8
is 1.25 MS/s, and the line under the Decimation box says so), and the spectrum, the
channel, recordings and every decoder run at that rate. An R2 stops at 32 because its
2.5 MS/s divided by 64 is not a whole number of samples a second, which the receiver's
resampler needs. Radios on the patch page run undecimated, at the rate the patch asks for.

The gain mode, every mode's values, the two AGC switches and the decimation are
remembered **per radio** (by its serial) and put back when that radio opens, before its
sample rate is set. The bias tee is a separate control —
a **Bias tee** checkbox below the gain sliders — and it is switched off every time FoxSDR
opens or closes the radio, so a previous application cannot leave 4.5 V on your antenna
port without anything on screen saying so. FoxSDR remembers the setting across restarts
and puts it back after the open, because a mast-head amplifier does not stop needing
power because the application was closed.

Tuning range 24 MHz to 1.75 GHz.

## The native HydraSDR RFOne driver (0.99.66, NOT YET TESTED ON HARDWARE)

**The HydraSDR RFOne** (USB 38AF:0001) is opened by FoxSDR's own driver, over the
same transport and with the same panel as the Airspy R2 - it appears in the Source
list as "HydraSDR RFOne" once it is bound to WinUSB with Zadig. It is written from
the vendor's published sources only (the `rfone_host` host library and the
`rfone_fw` firmware on github.com/hydrasdr) and checked against a fake built from
that firmware's source; **nobody who wrote it has held an RFOne**, so every
statement here is "the vendor's source says", not "it was seen to work". If you have
one, a report of what happened on first open (the log's `hydrasdr:` lines) is the
most useful thing you can send.

What it is: the RFOne is an Airspy R2 on the wire - the same twelve-bit ADC and
packed stream, the same twenty-eight vendor requests, the same LNA / mixer / VGA
registers and the same Linear / Sensitive / Free gain modes with decimation - so the
driver is the Airspy driver given the RFOne's own constants rather than a second
copy. What differs, and is handled: the USB id; a 64-bit frequency payload; the bias
tee, which the HydraSDR host switches with its own request; a tuning range of 24 MHz
to 1.8 GHz; the three sample rates its firmware lists (10, 5 and 2.5 MS/s, read from
the radio like the Airspy's); and **three receive ports - ANT, CABLE1 and CABLE2 -**
chosen from the Antenna list. The bias tee powers the ANT connector only, and
is switched off every time FoxSDR opens or closes the radio, as the Airspy's is.

What it deliberately does not do: it does not claim the RFOne's prototype USB id
(1D50:60A1, which is an Airspy's own and cannot be told from one without opening
it), so a prototype board shows up as an Airspy and is not supported; and it does not
send the five requests the host library added in 1.1.0 (capabilities, bandwidth list
and setting, temperature, unified gain), which the published firmware does not
implement. A firmware newer than the public source that offers sample rates in other
than the 12-bit packed form would not be understood.

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

**Native on Windows (WinUSB) and Linux (usbfs) alike**, like the other two,
through the same `src/usb/usb_device.hpp` transport contract — see "Linux
status" at the top of this file for what is and is not yet confirmed against
real Linux hardware. The protocol and DSP layer is plain C++20 and builds
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

## The SDRplay driver

An RSP is the one radio FoxSDR cannot reach the way it reaches the others.
SDRplay publish no device protocol, and the tuner is programmed by a Windows
service that owns the USB handle — so the only door is `sdrplay_api.dll`, and
the driver goes through it directly: FoxSDR loads the user's own SDRplay API
3.x at run time, resolves the entry points it needs, and drives the API itself.
No SoapySDR module in the path, nothing of SDRplay's linked at build time, and
nothing of theirs in the installer. If the API is not installed, no RSP is
listed and the Source panel says exactly what to do about it: install the
SDRplay API 3.x from sdrplay.com and restart FoxSDR. An API that IS installed
but is older than 3.07 - the version whose interface this driver is written
against - reaches the same place, naming the version that is there and asking
for an update; before 0.94.1 that sentence existed only in the log, because
the panel composed its advice from a version number the refused session never
recorded.

**What it does.** RSP1, RSP1A, RSP1B, RSP2, RSPduo, RSPdx and RSPdx-R2, 1 kHz
to 2 GHz, 62.5 kS/s to 10 MS/s. Everything below 2 MS/s is produced the way the
hardware actually produces it — the binary fractions of 2 MS/s by decimating a
2 MS/s zero-IF front end, the audio rates (96/192/384/768 kS/s) by decimating a
faster (3.072 MS/s) zero-IF one — because the RSP's front end does not run below
2 MS/s. Every rate is zero-IF: through 0.99.43 the binary fractions were taken
from a 6 MHz front end at a 1.62 MHz low IF, on the assumption that the service
down-converts and divides by three, and two RSP2 field logs showed it delivering
the 6 MS/s ADC rate instead, so 0.99.44 replaced it. The IF
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

**A Windows service on Windows; a dynamically loaded library on Linux.** On
Windows the SDRplay API runs as a background service this driver talks to
through `sdrplay_api.dll`; on Linux there is no service, only
`libsdrplay_api.so.3` from SDRplay's own `.run` installer, `dlopen()`d the
same way. Either platform without the API installed gets an empty enumeration
and a reason in the log, and the Linux path has not yet been confirmed against
a real RSP (see "Linux status" at the top of this file).

**When the service itself stops answering** (0.96.1). Everything above depends
on a Windows service that FoxSDR neither owns nor can restart, and none of the
vendor's calls take a timeout or can be cancelled — so when that service dies
with a radio open, every call answers `ServiceNotResponding` and anything
waiting on one waits forever. Two field reports were exactly that: an RSP1A on
API 3.15 whose retune, gain, AGC and shutdown calls all came back 14, and a
scan from the Source panel that went into the vendor's device list and never
returned, freezing the window. FoxSDR now treats both as what they are. A
control call answered `ServiceNotResponding` releases the radio the way an
unplugged one is released, with a message saying to restart the SDRplay API
service — rather than retrying into a service that is gone. And a scan gives
the vendor three seconds on a worker thread, then ABANDONS it: the panel says
*the SDRplay service did not answer within 3 s — restart the SDRplay API
service*, and further scans skip the SDRplay step for a minute rather than
spend another three seconds each time the source list is opened.

A third report, the same RSP1A on API 3.09, showed what neither of those
covered (0.96.3). Marking the radio dead on the first `ServiceNotResponding`
worked exactly as written — but the vendor's update call took about five
seconds to SAY `ServiceNotResponding`, and it was being made on the thread
that draws the window. So one click on a dead service froze FoxSDR for five
seconds, which is the whole of the hang watchdog's threshold; the watchdog
duly filed a freeze report, and by the time it walked the stack the vendor
call had returned and the thread was back in the graphics driver, so the
report blamed the GPU. Every live control — retune, gain, AGC, antenna, bias
tee, notches, sample rate — now runs its vendor call on a worker and gives it
one second, then abandons it, releases the radio and refuses further controls
for that device. A healthy service answers in milliseconds and behaves exactly
as before.

A fourth report, an RSP1B on API 3.15, was the freeze that came next — this
time in FoxSDR's own teardown. Abandoning the retune releases the radio, which
stops the pipeline, which stops the source, which called the vendor's
uninitialise on the thread that draws the window — into a device the abandoned
worker was still holding, because the API lets one call at a time near a
radio. So the window froze on the step that was meant to be the recovery.
FoxSDR now treats an abandoned call, and a service that has said it is gone,
as a door that stays shut: no uninitialise, no release, no close, the ring the
service writes into is kept alive rather than freed under it, and stopping and
closing the radio return immediately. Recovery is restarting the SDRplay API
service (Windows Services, **SDRplay API Service**) and then restarting
FoxSDR — the radio stays claimed inside the service until the service comes
back, and this is the one case where choosing it again in the same session is
not enough. Every other SDRplay fault, including an unplugged RSP, still
releases the radio properly and can be re-opened without restarting anything.

**The tuner every live control names** (0.99.61). An RSPdx-R2 on 0.99.59 opened,
and the first control after that was never answered and the service was then
found stopped; RSP1A and RSP2 owners had sent matching reports. For every model
but the RSPduo, FoxSDR had sent each frequency, antenna, gain and sample-rate
change to the service naming *no* tuner (`Tuner_Neither`, which the open line
printed as `tuner 0`), where SDRplay's own example program, its RSPdx-R2 ExtIO
and SoapySDRPlay3 name the one the radio has. FoxSDR now names Tuner A and the
open line reads `tuner 1`. That the old argument deviated from SDRplay's own
clients is checked against their published interface and source; that it is why
the service stopped answering is an inference, and none of this has met a real
RSP. If yours still stops, send the SDRplay diagnostic from the Diagnostics
section (its **Run SDRplay diagnostic** button).

**The RESTART SDRPLAY SERVICE key** (0.99.55, Windows). When the SDRplay API
refuses to open, a scan gives up on it, or the session is lost as above, the
Source panel now says what Windows reports the **SDRplay API Service** is doing
— stopped, disabled, still starting, running but not answering, or not
installed — and shows a **RESTART SDRPLAY SERVICE** key. Pressing it asks for
administrator rights through the ordinary Windows prompt and restarts the
service (`net stop`, falling back to ending `sdrplay_apiService.exe` if the
stop hangs, then `net start`); cancelling the prompt changes nothing. If this
FoxSDR session had never reached the SDRplay API, the radios are listed again
and the saved radio is reopened straight away. If the session had been lost,
the service is restarted but FoxSDR still has to be restarted, for the reason
given above. A disabled service has to be set back to **Automatic** in Windows
Services first — the key cannot start a disabled service. The service name
`SDRplayAPIService` comes from third-party listings rather than SDRplay's own
documentation, so FoxSDR also looks the service up by its display name and by
its program file, and uses whatever name Windows actually has for it.

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
way the vendor's header says the service does. More than 1,200 checks cover the exact call
sequence at open, the exact parameters the radio is started with, every update
reason against the change that caused it, the sample conversion, the per-model
antenna, notch and bias-tee routing, the rate-and-decimation plan for every
published rate, the refusals (no API, an API too old, a frequency out of range,
an RSPduo another application already holds), the fault path for a removed
device, the teardown bound, and - from 0.96.1 - a service that never answers
at all, which the fake can now imitate because the real one did. Each was
confirmed to go red against a deliberately broken driver before being
believed.

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
in FoxSDR, the device has to be bound to WinUSB first (see [Installing](MANUAL.md#installing));
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
firmware) and `04B4:00F1` (the radio, after it). See [Installing](MANUAL.md#installing).

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

## AOR digital-I/Q receivers (AR5700D; AR2300, AR5001D, AR6000 with IQ5001)

**Read this first: this support is written from AOR's documentation and has
not been tested on hardware.** Nobody on the project has an AOR receiver.
Every part of the driver has been tested against fake USB and serial devices
that behave the way AOR's document says the receiver does, and against
nothing else. Please report what happens on a real receiver.

The driver was written from AOR's *Digital I/Q USB Interface Developer
Information* (Revision 1.1, September 2026). Thanks to AOR, Ltd. for
publishing it. It is FoxSDR's own code, written without reading SoapyAOR or
AOR-GQRX-RPi, and it does not use AOR's Windows driver. AOR's document
describes the AR5700D as validated. The AR2300, AR5001D and AR6000 with the
IQ5001 board belong to the same family but are **unverified**. FoxSDR guesses
their VR replies by analogy with the AR5700D's and does not send them `@21`.

The receiver uses two USB cables, and FoxSDR needs both:

- **I/Q** (`08D0:A001`): samples at a fixed 1.125 MS/s. That is the only rate
  offered.
- **Control**: an FTDI serial port. FoxSDR tunes the receiver through it with
  the `RF` command. FTDI chips are in many unrelated devices, so FoxSDR does
  not pick the port by its USB ID. It asks each FTDI port `VR` and uses the
  one that answers as an AOR receiver. If none answers, or more than one
  does, the open is refused with a sentence saying why. Connect one AOR
  receiver at a time.

**On Windows the I/Q interface has to be bound to WinUSB.** AOR's own driver
(AorAlpha) and WinUSB cannot both own it, so with AOR's driver installed
FoxSDR cannot open the receiver. The Source section says so under the list.
To switch it, run Zadig, tick Options → List All Devices, select the AOR I/Q
interface (`08D0:A001`), choose WinUSB and click Replace Driver. AOR's own
software will then need its driver back. The control port stays on FTDI's
normal serial (VCP) driver. On Linux nothing needs rebinding: the udev rule
in `installer/linux/` grants access to the I/Q interface. The control port is
an ordinary `/dev/ttyUSB*` node, so your user must be in the `dialout` group.

**The FX2 firmware is not included yet.** The I/Q interface forgets its
firmware whenever it loses power. The firmware (`fx2fw.hex`) is AOR's, and
AOR will supply it with a redistribution notice. Until then, FoxSDR looks for
it at `resources/firmware/aor/fx2fw.hex` beside the executable. If the
interface has no firmware running and that file is missing, FoxSDR says so in
plain words and does not open the receiver. If other software has already
loaded the firmware since power-on, the file is not needed.

When the file is present, FoxSDR loads it only after checking that the
interface is not already streaming. AOR's document does not say how to tell
an unprogrammed interface from a running one, so FoxSDR asks it to stream
first. This question has been put to AOR.

Not supported yet: gain and antenna control (AOR's document does not
describe them), and **Android**. usbfs would work there, but the FTDI control
port needs a userspace FTDI driver on Android. That is a follow-up and has not
been built.

## The native ADALM-Pluto driver

An **ADALM-Pluto** is opened by FoxSDR's own driver, over the network — no
libiio, no libad9361, no SoapySDR module. It is the first radio FoxSDR reaches
that is not on the USB bus at all: a Pluto presents a USB Ethernet gadget and
runs an `iiod` daemon on TCP port 30431, so where the other native drivers send
control transfers this one sends one-line text commands, and where they read a
bulk endpoint this one reads a socket.

**It transmits too, from 0.95.0** — see [Transmitting](TRANSMITTING.md), which is a
section of its own because the rules about when a radio may be keyed matter more
than anything else on this page. The receive driver stays exactly as it was: the
transmit half is a separate file, a separate pair of connections and a separate
object, so nothing about keying a Pluto can change how it listens.

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

## The rtl_tcp network source

**rtl_tcp** is the small server that ships with the RTL-SDR software: it holds
a dongle on one machine and streams its samples over TCP to a client on
another, which is how an RTL-SDR on a Raspberry Pi in the loft, or on another
PC, is listened to from here. Several other programs imitate its protocol
(SDR++ Server has an rtl_tcp-compatible mode), so "an rtl_tcp server" is
whatever answers that protocol at an address. FoxSDR connects to one with its
own client - nothing is installed on this machine, and no SoapySDR module is
involved.

**To use it,** choose **rtl_tcp server (network)** at the end of the Source
dropdown, type the server's address into the box - `host` or `host:port`, and
the port is 1234 unless the server was started with another (`127.0.0.1:1234`
is what the box holds until you change it) - and press **Open**. Like the
Pluto's row it is an address and not a discovery: selecting the row contacts
nothing, a failed Open leaves whatever was running still running with the reason
beneath the box, and what you typed is remembered for next time whether or not
the server answered. A bracketed IPv6 address with a port, `[fe80::1]:1234`, is
accepted.

**What it does.** The sample rate menu is the RTL-SDR's own. Gain for an R820T
or R828D tuner is in real decibels on the same ladder as the native RTL-SDR
driver; for any other tuner (an E4000, say) the server publishes only how many
gain steps it has, so the slider shows bare step numbers and no decibel figure
is invented. Automatic gain, the bias tee switch and the crystal correction
(whole ppm) are sent to the remote dongle. **The server never acknowledges a
command**, so what the panel shows for a gain, rate or bias tee is what FoxSDR
asked for - the server rounds a gain to the nearest step its tuner has and does
not say which. The tuning range shown is the tuner's usual one, for
information only: a dongle with direct sampling or an up-converter can reach
below it, and FoxSDR sends whatever frequency you ask for.

**Samples are 8-bit**, because that is what rtl_tcp sends: the dynamic range is
the RTL-SDR's own, whatever the network.

**Stop pauses; closing the source lets go of the server.** The connection is
made when you press Open and kept until the source is closed or another is
chosen. Stop and Start only pause and resume: Stop returns at once, and while it
is stopped FoxSDR goes on receiving the server's stream and throws it away, so
the server is never held up, and Start has no connection to make and throws away
anything that arrived in between. Two consequences. rtl_tcp serves one client at
a time, so the server stays busy for every other program until you close the
source (a stopped source is still a connected one). And a stopped source still
costs the network the whole stream - several megabytes a second at the top
sample rate - so close it rather than leave it stopped on a metered link. On
open, FoxSDR restates every setting to the server (its dongle keeps what the last
client left: the crystal correction, the bias tee), so nothing is taken on
trust; changes made while stopped are sent at once.

**If the connection is lost** - the server stopped, the network dropped, or it
went silent for two seconds - the receiver stops with "Device stopped" and the
reason, and nothing reconnects by itself: open it again when the server is back.

**Every wait is bounded.** A connect gives up after three seconds, the header
read and every later send and receive after two, and closing the source wakes
the reader thread and joins it within two and a half. Stop and Start wait for
nothing. The diagnostic log carries the tuner and the port, never the address,
and the source's own name (which the log quotes) has no address in it
(`docs/DIAGNOSTICS.md`).

**The SoapySDR device scan is not held back** by an open rtl_tcp source: the
scan waits for a local radio because its probe would reset the dongle, and a
network connection has no dongle here to protect.

**How it is verified.** The suite runs without an rtl_tcp server: the proof
there is the protocol rather than a spectrum.
`tests/test_rtl_tcp_source.cpp` runs a fake server on the loopback interface - a
real socket, real threads - and checks the header parse, the exact bytes of
every command (the big-endian value, the two's-complement ppm), the decoding of
the 8-bit stream across receives that split a sample in half (on a scripted
transport that hands over exactly the chunk sizes it is given, because a real
socket may merge small sends), what a stop and a start deliver, the fault when
the server goes away or goes silent, the wording of each way an open can fail,
a stop that returns at once, and a reader that never comes back.
**It was also run against the real server, by hand, on 2026-10-06**: the
`rtl_tcp.exe` that ships with radioconda, serving an RTL2838 with an R820T tuner
on the same computer. FoxSDR connected, read the header (the tuner and its 29
gain steps), took the stream at 2.4 million samples a second, and when the
server was stopped part way through it reported the lost connection, with no
address in the log. A Stop followed by a Start, sent through the browser remote
(which runs the same code as the window's keys), was tried against it on
2026-10-07: the spectrum froze, kept the connection, and ran again after the
Start. **What is not verified**: pressing the Stop and Start keys in the
window itself, a server on another computer, a server that only imitates
rtl_tcp, and Linux. The
suite itself still checks only against the fake, which was written from the
protocol's public description, as the driver was; librtlsdr's GPL source was
not used for any code.

## A sound card as the receiver

**Below about 100 kHz a sound card is the receiver**, which is how SAQrx listens
to SAQ Grimeton on 17.2 kHz, and a stereo card is also the classic input of an
I/Q receiver such as a SoftRock. The Source list has a **Sound card** row for
both. Choosing it shows the controls and opens nothing; **Open** opens the card
on a worker thread, so a slow audio device never holds the window.

- **Real (mono)** takes the left or right channel as real audio from 0 to half
  the card's rate, removes the mirror image and shows the band on its true air
  frequency: a card at 192 kHz shows 0 - 96 kHz, and a 17.2 kHz transmitter
  sits at 17.2 kHz. A card has no tuner, so typing a frequency moves the
  receiver inside that span instead of retuning anything. The receive filter
  has to fit in the span too, so the reach is the span less half the filter at
  each end (1.2 - 94.8 kHz for a 2.4 kHz filter on a 192 kHz card), and a
  frequency outside that reach is refused with a sentence giving it - never
  moved to the nearest edge. A filter wider than the whole span (WFM on a
  96 kHz card) centres the receiver and refuses every tune until it is
  narrowed. In
  `tests/test_soundcard_source.cpp` a 17.2 kHz tone through a simulated card at
  192 kHz lands at 17.2 kHz on the spectrum and USB on 16.4 kHz turns it into
  an 800 Hz audio tone, measured at the end of the real pipeline; USB on
  17.6 kHz, where the tone is on the lower sideband, does not hear it.
- **I/Q (stereo)** takes left as I and right as Q, with a **Swap I/Q** switch for
  hardware wired the other way round (the symptom is a mirrored spectrum), and
  a **Centre (MHz)** box for what the external receiver is tuned to. The box
  moves the running receiver only while the card is running in I/Q mode;
  otherwise it is kept for the next **Open**.

The card is remembered by its **name and host API**, never by its position in
the list, and a saved card that is not there at startup is named on screen as
not connected while the settings are kept - no other input is ever opened in
its place. **After unplugging or plugging in a card, restart FoxSDR**: cards
are listed when FoxSDR starts, because PortAudio builds its device list once
and keeps it for the session, and every message about a missing or stopped
card says the same. A card pulled out while it runs stops the receiver with
the reason on screen: at once when the audio system reports it, and after two
seconds of silence when it does not. Closing a card never holds the window
up: the close runs on a thread of its own, a healthy card's close is waited
for at most one second (so the same card can be reopened straight away), a
card that has already failed is not waited for at all, and on exit nothing is.

**On Windows the list holds WASAPI inputs only.** Its list for each card holds
the rate the Windows mixer is set to, plus - where the card allows exclusive
mode - the rates its hardware accepts there, marked "(exclusive)": those open
the card in exclusive mode, so FoxSDR has it to itself while it runs, and are
meant to reach the card's own rates without anybody changing a Windows sound
setting (not yet seen on a 192 kHz card; the bench headset offers no
exclusive-mode rate). MME and DirectSound entries are not offered at all. MME
keeps each input as a number Windows gives out again whenever a card is
plugged in or pulled out, so a card saved by name could reopen as a different
one; it also accepts every rate and resamples, so its rates say nothing about
the card; and its "Sound Mapper" (like DirectSound's "Primary Sound Capture
Driver") follows whatever the Windows default input is. A card saved from an
MME entry by an earlier development build is shown as not connected. On Linux
the inputs are PortAudio's ALSA devices.

The patch page offers every listed card to its radios as well; the card opens
as the Source section has it set up when it is the same card, and as real mono
on the left channel otherwise. While the patch runs it takes the receiver's
own card the way it takes the receiver's radio - the receiver runs on the
signal generator and gets the card back when the patch stops - so a patch
radio on that card never opens a second stream on it. An I/Q recording of a
sound card captures what the spectrum shows (the complex stream, at half the
card's rate in real mode).

Not yet verified on real hardware at 192 kHz or on a VLF antenna: the bench has
a headset microphone at 48 kHz, which opened, streamed and drew its (very
quiet) spectrum from 0 to 24 kHz in an isolated run.
