# Diagnostics — crashes, freezes, and reading a report six months later

This describes phase 1, which is complete: FoxSDR captures faults **locally**
and uploads **nothing**. Phase 2 (a Cloudflare Worker that receives a minimal
report) is deliberately not built yet — publishing noise from a capture nobody
has trusted yet would only make the numbers harder to believe later.

## Why a crash handler was not enough on its own

Every fault this product has actually shipped was a **hang**, not a crash:

- a 120-second freeze while a CAT client was shut down,
- a map window that could not be closed,
- an audio stream that had died while every number on screen still read
  healthy.

A crash handler would have caught none of them. The process was alive and, from
its own point of view, fine. So there are two capture paths, and the second one
is the one that pays: `src/core/crash_handler.cpp` for a process that dies, and
`src/core/hang_watchdog.cpp` for one that stops moving.

## What is captured

| | Crash | Hang |
|---|---|---|
| Trigger | SEH filter, `std::terminate`, `SIGABRT`, purecall, CRT invalid parameter | GUI thread has not pumped for 5 s |
| Stack | the faulting thread, unwound from the exception context | **every** thread in the process |
| Written | `%LOCALAPPDATA%\FoxSDR\crashes\crash-*.txt` | `…\crashes\hang-*.txt` |
| Afterwards | the process dies as it always did (Windows Error Reporting still runs) | the application **carries on**, and the watchdog re-arms |

Both carry: exact version **and** git commit, the stack as `module+offset`, the
loaded module list with each module's **build id**, the last 256 log lines from
the in-memory ring, the application context (mode, source, sample rate, radio
model with the serial stripped, the crystal correction, the sound path - output
state, volume, who muted it and the squelch, since 0.99.61 - the radios the
patch page has running, by driver kind, since 0.99.62, and the loaded plugins
with versions), and a **stable signature** for grouping. Since 0.99.62 that
context is rendered after every event that changes what it says, and on every
frame, rather than once a second: see *The context block follows the session*
below.

Since 0.99.56 the context has a `ppm:` line: `off`, `not applicable` (switched
on over the generator, a sound card or an I/Q file), or the value in force and
how it is applied - `+2.6 in the radio` (the radio's own correction; a native
RTL-SDR rounds it to whole ppm), `+2.6 in the radio (refused)`, or
`+2.6 by retuning` (the centre frequency only). It is support's first question
when a signal is "not where the band plan says", and it is a property of the
radio's crystal, never a frequency. The log says the same once per open
(`source: frequency correction +2.6 ppm applied in the radio (rtlsdr, whole
ppm: +3)`, or `... by retuning (hackrf, centre frequency only)`), and once per
change. The upload does not carry the line (crash_upload.cpp reads only the
fields listed under *The reader*).

Since 0.66.0 the upload also forwards the report's own `reason` and `code`
lines verbatim (empty for a hang, whose writer has no such lines). The reason
is what lets the dashboard tell an **absorbed** vendor fault — the guard filed
the evidence and the process carried on — from a process death at the same
address: without it the two were identical rows, and a survived teardown fault
measurably reopened a fixed signature. And the client signature is no longer
what the dashboard *groups* by when it can do better: it is hashed from the
faulting module + offset, and one release cycle of field data showed both
failure modes of that key — the same teardown fault split into two signatures
by a Windows update moving ntdll's offsets, and two unrelated faults from two
users merged into one because `RtlpWaitOnCriticalSection` sat at the same RVA
in both their ntdll builds. Offsets into code we did not ship identify the
victim's patch level, not the bug. The server therefore re-groups reports by
the nearest frame in **our own** code, by *name*, using the per-build
RVA→function maps `tools/export-symbol-map.py` emits into the symbol archive
(see *The reader* below for what is and is not uploaded). Having a map is what
marks a frame as ours — which is why the same step matters for **plugins**: a
plugin frame with a map is named by the plugin's own function, and without one
every distinct bug inside that plugin collapses into a single group, since
every plugin stack passes through the same host dispatch on its way down. The client signature
stays in the payload as transport identity and the client-side dedup key. For a
**freeze** (`hang` or `stall`) that signature is, since 0.99.62, hashed from the
first frame of the stalled thread that lies in our own executable rather than
from its top frame - see *What a freeze's signature is built from* below.

### What the native drivers write (0.91.0, 0.92.0, 0.93.0)

FoxSDR drives an RTL-SDR and a HackRF itself now, from 0.92.0 an Airspy
R2/Mini and an Airspy HF+, and from 0.93.0 a Mirics MSi2500 and an RX888 mk2,
over its own WinUSB
transport, with no SoapySDR module in the path. Two more radios are driven
natively without that transport: an SDRplay RSP through the vendor API the
user installed (`sdrplay`), and an ADALM-Pluto over TCP to the board's own
daemon (`pluto`). The log lines that produces
are deliberately the same shape as the SoapySDR ones, so one log reads the
same whichever way a radio was opened - the only difference is that these
describe code this product can be held responsible for.

- `source: opened RTL2838UHIDIR natively, tuner R820T, 2.4000 MS/s` - written
  by the driver itself at the end of a successful open (`RtlSdrSource::open`).
  The tuner matters: it is what decides whether the dongle can be opened
  natively at all, and it is the first thing a report about a dongle that
  behaves oddly needs to say.
- `source: opened RTL2838UHIDIR (rtlsdr) at 2400000 S/s` - written by the
  application when it installs the radio in the pipeline. The parenthesis is
  the DRIVER KIND (`soapy`, `rtlsdr`, `hackrf`, `airspy`, `airspyhf`,
  `sdrplay`, `mirisdr`, `rx888`, `pluto`, `aor`): two rows
  in the Source dropdown can name one physical radio, and a report has to say
  which of them was taken. Every one is its own key and none is a family
  name - `airspy` and `airspyhf` are different USB ids, different hardware and
  different bands, and `mirisdr` is not `sdrplay` even though an early RSP1 is
  a Mirics device, because one goes through the vendor service and the other
  over the bare MSi2500. Two of the keys name radios that are not on the USB
  bus at all: `sdrplay` is reached through `sdrplay_api.dll` and `pluto` over
  the network.
- `source: opening RTL2838UHIDIR natively (was SoapySDR rtlsdr)` - the
  prefer-native rule firing. A config saved before 0.91.0 says "SoapySDR,
  driver=rtlsdr" because that was the only way to reach the dongle; the
  application opens it with its own driver instead, without being asked, and
  this line is where that decision is recorded. See `gui::preferNativeFor`.
- `source: the native rtlsdr driver refused the saved radio (...); opening it
  through SoapySDR instead` - a warning, and the one case the rule above backs
  out of: an E4000 or FC0012/13 tuner, which the native driver does not
  support. The radio still opens, the way it always did.
- `source: the RTL-SDR reader thread did not exit within its bound; it is
  abandoned and the radio is condemned` - a warning, and the native
  counterpart of SoapySource's abandonment. The remedy is the same one and is
  told to the user in the same words: restart FoxSDR to use this radio again.
- The stream-health line below is written by the native drivers too, in the
  same format and on the same rules.
- The Airspy drivers write the same shapes under their own names - the open
  line carries the model the bus and the firmware named (`Airspy R2`,
  `Airspy Mini`, `Airspy HF+ Discovery`) and, for the HF+, the part id
  verbatim rather than a model guessed from it. There is no `--airspy-check`:
  the Airspy equivalent of `--rtlsdr-check` was not added, because there is no
  Airspy on the bench this was written on for it to have been proven against.
- The four added in 0.93.0 write the same shapes again, plus the lines only
  they can produce:
  - `source: SDRplay API - ...` once per process, from the loader, naming the
    path it came from or why it did not. On a machine with no RSP this is the
    line that says whether the API was even found, and the Source section
    shows the same sentence the user needs (install 3.x from sdrplay.com).
    `source: SDRplay enumeration skipped - ...` follows it whenever the list
    could not be read - no API, an API older than 3.07, or a service that did
    not answer.
  - From 0.96.1, three more lines that only a DEAD SDRplay service produces,
    and they are worth recognising because between them they replace a hang
    report and fifty seconds of a frozen receiver:
    - `source: SDRplay enumeration abandoned - the SDRplay service did not
      answer within 3 s - restart the SDRplay API service`, followed by
      `source: SDRplay scans are held off for 60 s`. The vendor's device-list
      call takes no timeout and cannot be cancelled, so the scan runs it on a
      worker and gives up on the worker rather than on the wait. The same
      sentence appears in the Source panel. Seeing this means the service, not
      the radio, is what has to be restarted.
    - `source: SDRplay <what> - the service stopped answering; the radio is
      released`, where `<what>` is the control that was refused (`retune`, `IF
      gain change`, `LNA state change`, and so on). The receiver is marked
      dead and stopped at that point. Before 0.96.1 a service that died with a
      radio open produced a run of `source: SDRplay <what> failed -
      ServiceNotResponding (14)` lines and a stream-health line counting
      thousands of timeouts, with the picture frozen and nothing saying why.
    - From 0.96.3, and this is the one to look for when a hang report blames
      the graphics driver: `source: SDRplay <what> abandoned - the service did
      not answer within 1000 ms; the radio is released`, followed by `source:
      SDRplay controls are refused for this device from here - a worker is
      still inside sdrplay_api_Update`. The line above it is the service
      REFUSING a control; this one is the service never answering it at all.
      The vendor's update call takes no timeout either, so it is run on a
      worker the same way a scan is and the worker is abandoned rather than
      waited for. Before 0.96.3 that wait was the GUI thread's, it lasted
      about five seconds on a dead service, and the hang watchdog filed a
      report whose captured stack was the next frame's `SwapBuffers` inside
      the graphics driver - because by the time the stack was walked the
      vendor call had returned. A freeze of about five seconds per click on an
      RSP, reported as `nvoglv64` or `atio6axx`, is this and not the GPU.
  - From 0.99.61, the open line names the tuner the live controls will address,
    and one more line can follow it. The line is `source: SDRplay opened
    RSPdx-R2, API 3.15, tuner 1`; the 0.99.59 RSPdx-R2 report read `tuner 0`.
    For every model except the RSPduo the driver set `Tuner_Neither` before it
    selected the device, and every `sdrplay_api_Update` it sent afterwards -
    each retune, antenna, gain and sample-rate change - carried that value as its
    `tuner` argument, so every live control asked the service to apply the change
    to **no tuner**. In that report the first control after the open was never
    answered and the service was then found stopped; RSP1A and RSP2 owners had
    sent matching reports earlier. The driver now keeps the tuner the service
    listed when it is Tuner A, and uses Tuner A when it is anything else, which
    for a radio with one tuner can only mean "the one there is". In that second
    case it says so: `source: SDRplay listed tuner N for a single-tuner RSP -
    using Tuner A, the only tuner it has`. That line is new information, not an
    error: it means the service's list differs from what SDRplay's own client
    code is handed. Its absence, with `tuner 1` in the open line, is the
    expected case.
    **Proven:** the old argument deviated from SDRplay's own clients and from
    their specification. The source comment in `SdrPlaySource::selectByArgsLocked`
    records that the example program in the API Specification 3.15 (section 4)
    assigns `tuner` and `rspDuoMode` only for an RSPduo and passes
    `chosenDevice->tuner` to every update, that SDRplay's RSPdxR2 ExtIO and
    SoapySDRPlay3 do the same, and that the specification calls the update's
    argument the tuner "to apply the update to", which `Tuner_Neither` is not.
    **Inferred, not shown:** that this is why the service stopped answering. No
    RSP was on the bench: `tests/test_sdrplay_source.cpp` pins, against a fake
    service, that every update for a single-tuner model names Tuner A with the
    exact reasons, that the open line reads `tuner 1`, and that an overload
    acknowledgement and the control calls can no longer name different tuners.
    If an RSP still stops answering with `tuner 1` in the open line, this was not
    the whole cause, and **Run SDRplay diagnostic** (below) is what to ask for.
  - `rx888: opened ... (firmware loaded by FoxSDR)` when the radio was a
    Cypress bootloader and FoxSDR uploaded the image to it. An open that takes
    about five seconds and this line in the log is the NORMAL first open after
    a power cycle, not a fault.
  - `aor: ...` lines come from the native AOR digital-I/Q driver (AR5700D;
    AR2300, AR5001D and AR6000 with the IQ5001). It was written from AOR's
    developer document and has NOT been tested on hardware, so a report from
    a real receiver is new information, not a regression. The lines, in open
    order: `aor: control port COMn: VR -> "..."` for every FTDI port asked,
    with what it answered (the AOR is the one naming a model; the rest were
    left alone after EX/VR); `aor: probe: armed yes/no, START
    accepted/refused, N transfers / N bytes, aligned - firmware is running`
    or `no alignment`, which is how the driver decides whether to load AOR's
    FX2 firmware, because AOR's document does not say how to tell; `aor:
    loaded N bytes of FX2 firmware` and `aor: after the firmware load the
    interface re-enumerated` when it did load it; `aor: opened AOR AR5700D -
    VR "...", control on COMn, I/Q at 1.125 MS/s`. `aor: open refused` or
    `aor: open abandoned` carries the same sentence the user saw: not bound
    to WinUSB (AOR's AorAlpha driver has the interface), firmware file not
    installed, no or several AOR receivers answering, or no stream even after
    the firmware load. `aor: stopped - N transfers, first 1 discarded, N
    alignment losses` closes each stream. Alignment losses above zero on a
    healthy USB link are worth reporting to the project.
  - The Pluto's open line names what the board reported about itself rather
    than a model from a table, including whether it is a stock AD9363 or one
    with the AD9364 unlock applied - the tuning range differs by a factor of
    ten between them and nothing else in a report would reveal which.

**`cascade.exe --rtlsdr-check`** is the native counterpart of `--soapy-check`
and answers the question the suite cannot. The register sequences are proven
against a fake transport in `tests/test_rtlsdr_source.cpp`; whether THIS
machine's dongle is bound to WinUSB, enumerates, opens, answers its tuner
probe and delivers samples that are neither silent nor saturated is a question
only a run against it can settle, and those four are what actually go wrong in
the field. It prints what is bound to WinUSB, the tuner, the tuning range,
every gain with its range and current value, the rate and centre it set, the
sample count against what three seconds should have produced, the mean and
peak magnitude, and the stream-health line. Exit 0 means samples arrived and
were plausible; exit 1 names which of the checks failed, including the
commonest cause of all - nothing bound to WinUSB, and what to do about it.

### One chip, one route: SoapySDR's SDRplay and Mirics modules (0.99.61)

An RSP1, RSP1A or RSP2 is a Mirics chip, and FoxSDR can reach one four ways: its
native SDRplay driver (through the SDRplay API), its native Mirics driver, and
SoapySDR's `sdrplay` and `miri` modules, which run inside the same process beside
both. The two native routes already obeyed the SDRplay rules — the native SDRplay
driver refuses a session it has declared lost, and the native Mirics row is
hidden for a radio the API manages. The two SoapySDR routes obeyed nothing, and
two crash reports from one Linux user (an RSP1A, the SDRplay API 3 installed,
0.99.59) are the two ways that went wrong:

- `crash libmiriSupport.so @ SoapySource::read` — a SIGSEGV inside SoapyMiri, after
  the log had said `1 native SDRplay row(s) hidden - the SDRplay API is
  installed`. The Source list had hidden the native Mirics row of the radio the
  API manages and still offered, and opened, SoapyMiri's row for the same radio.
- `crash libc.so.6 @ AppWindow::publishWebSnapshot()` — an `abort()` from glibc
  on the GUI thread, seconds after the patch page opened the radio through
  SoapySDR's `sdrplay` module, **after** the native driver had logged that the
  SDRplay session was lost (`no further SDRplay API calls are made until FoxSDR is
  restarted`). The SoapySDR module simply made them.

**Proven:** both opens were allowed by the code — the `miri` row was listed and
opened while the API managed the radio, and the patch page opened the `sdrplay`
module after the native session had been marked lost. **Inferred only:** that
the second open is what corrupted the heap. An audit made while preparing this
fix found no race in `publishWebSnapshot` itself (that audit is not recorded in
the code), and an `abort()` in glibc is where heap damage done earlier by any
code would surface, so the site of the abort says nothing about the cause; the
link rests on the order of events. Neither report is claimed fixed by anything
beyond closing the two routes.

The rule, now enforced in `SoapySource::open` and in the Source list and the
patch page's device list:

- **(a) Once the SDRplay API session is lost** — the latch the native driver's
  `no further SDRplay API calls` line announces, readable without the session
  mutex because an abandoned scan worker may be parked holding it — **or an
  enumeration worker was abandoned inside the API** and nothing since has shown
  the service answering, no SoapySDR `sdrplay`, `miri` or `mirisdr` device is
  opened for the rest of the process.
- **(b) A SoapySDR Mirics device (`miri`, `mirisdr`) is not offered or opened for a
  radio the API manages**: exactly the radios whose native Mirics row the
  duplicate rule already hides (on Windows, every native Mirics row this driver
  recognises as an SDRplay unit while the API is installed; on Linux, one the
  API's own enumeration lists, so a stopped service lists nothing and takes
  nothing away). A SoapySDR row is tied to such a radio by its serial, ignoring
  case; a row that names no serial, or a managed radio that names none, cannot be
  told apart and is treated as the managed radio.

**What is never refused**, so no working route is taken away: any other SoapySDR
driver; `driver=sdrplay` while the API is healthy (unchanged — the prefer-native
rule upgrades the row, and the module is still the way in where the native route
cannot take it); and `driver=miri` on a machine with no SDRplay API installed (a
genuine Mirics dongle), when the API lists no radio of this kind on Linux, or
when the scan holds no SDRplay-flavoured native row. The one stated cost: with an
RSP and a genuine Mirics stick plugged in together, a SoapySDR Mirics row that
names no serial is refused (the stick's own native row is listed and works).

The refusal sentences, which are what `open()` reports and what a patch node
shows:

- `FoxSDR will not open an SDRplay or Mirics radio through SoapySDR because the
  SDRplay API stopped answering earlier in this session - restart the SDRplay API
  service, then restart FoxSDR`
- `this radio is an SDRplay RSP that the SDRplay API manages, so FoxSDR does not
  also open it through SoapySDR's Mirics module - pick its SDRplay row instead`

and the two log lines, both of which name the driver or a count and never the
radio's args (which carry its serial):

- `source: refused to open a SoapySDR <driver> device - <one of the sentences
  above>` — a warning.
- `source: N SoapySDR SDRplay/Mirics row(s) hidden - another route to the same
  chip owns it, or the SDRplay API session was lost` — written when a scan
  leaves rows out, beside the `native SDRplay row(s) hidden` line it mirrors.
  A row that vanishes without a line is the report this prevents from the other
  side. The row of a radio already open through SoapySDR is never dropped.

One pure decision (`miricsSoapyRouteRefusal`, `src/source/rsp_rows.hpp`) is asked
from `SoapySource::open` — the one place every receiver, patch, restore and
fallback open of a SoapySDR device passes through, so a new call site cannot
bypass it — and from the scan merge, after every SoapySDR scan and every native
scan. Whether the API manages a radio is read off the native scan, so a saved
patch that names a SoapySDR Mirics device before any native scan this session runs
that scan first (it opens nothing and costs one bounded question to the service).

**A hardening change beside it, not claimed as the cause of either crash:**
`SoapySource::read` now treats a module that returns more samples than it was
asked for as a broken contract. Such a module has already written past the end
of the caller's buffer, and the old code took its count at face value — the
sanitiser then scanned and rewrote elements past the end, and every caller
processed that many samples out of a buffer sized for fewer. Now none of it is
used, `read()` returns 0 and the device is marked faulted through the path a
driver that throws takes (the pipeline stops with the reason and the radio is not
read again), with `soapy: readStream returned N samples for a request of M - the
module wrote past the caller's buffer; the device is faulted and will not be read
again` as a warning. `tests/test_soapy_read_bound.cpp` drives it with a fake
SoapySDR module that answers exactly `n` (not a violation), fewer, one more and
four more, and requires the over-long answers to be refused without touching the
element past the end of the buffer.

**Not run on Linux, on an RSP, or against the SDRplay service.** What ran is the
pure decision (`tests/test_one_chip_one_route.cpp`), a real `AppWindow` driven
with fake SoapySDR modules registered under the real driver names and a test-supplied
native list (`tests/test_one_chip_app.cpp`: the Source list, the patch page's
list and worker, the native scan and the startup restore each ask the
question), and the read bound above. Whether either crash stops happening is
untested.

### What the log carries since 0.89.0 — the driver's own words

The 0.88.0 field crash from an RTL-SDR user reached the crash store with a
21-line log tail that said nothing about the radio. Not because the ring was
short — it holds 256 lines — but because nothing that mattered had been
written to it: the sample-rate change 45 s before the fault was a
`setSampleRate` call the log never saw, the minute of stalled USB delivery
before that showed only as the audio ring's starvation counter climbing, and
the driver's own account of it — librtlsdr's `rtlsdr_read_async: dev_lost`,
printed with `fprintf(stderr, …)`, and SoapyRTLSDR's warnings, sent through
SoapySDR's logger to its **default handler, which prints to stderr** — went to
a console nobody was reading. Three things changed, none of them the fault:

**The stream-health line.** Once a minute the radio's read loop writes what the driver answered: `source: stream health - reads 612, with samples 598, timeouts 14, overflows 0, errors 0, longest gap 38 ms, 1434880 samples in 60 s`. The first minute after a start is always written, so a healthy radio leaves one line proving it; after that a minute is written only when something was not nominal (any timeout, overflow or error, or a gap of 250 ms or more without samples), and as a warning when there was an error or a gap of a second or more. A minute of stalled USB delivery before a driver fault - what the 0.88.0 field crash had, visible then only through the sound path's starvation counter - now reads as a line in the report's tail.

**The transmitter's lines, all prefixed `tx:` (0.95.0).** A transmit path is
the one part of this product whose faults are somebody else's problem as well
as the user's, so every state change it has is in the log rather than only on
the panel:

- `tx: opened <board> at <host>:<port> for transmit - phy ad9361-phy (LO
  altvoltage1, gain voltage0), DAC cf-ad9361-dds-core-lpc (le:S16/16>>0), 4 DDS
  tone channel(s)`, followed by the board's own published transmit power range
  and tuning range, and then `tx: the board is quiet (attenuation at its
  maximum, TX LO down) and stays that way until the operator keys it`. That last
  line is the one to look for when somebody asks whether opening the page can
  transmit: it is written every time a board is opened, and it is written
  because the driver has just made it true.
- `tx: keyed - USB at 145.500000 MHz, 2500000 S/s` and `tx: started - ...,
  power -20.00 dB` when the key closes; `tx: unkeyed after N block(s)` and
  `tx: stopped - the board was told to go quiet (confirmed)` when it opens. The
  word in the brackets is `confirmed` when the board acknowledged both
  quietening writes and `NOT CONFIRMED - the board did not answer` when it did
  not, which is a different thing to act on: the first is a radio that is
  certainly silent and the second is one that may not be.
- `tx: fault while <what>: <detail>` for anything the board refused or stopped
  answering, as a warning.
- `tx: the transmit latch released itself after 60000 ms` and `tx: no frame in
  <n> ms - releasing the key` for the two failsafes. Either of these in a log
  means the key was opened by the application rather than by the operator, and
  the second one in particular means the frame loop had stopped - so it will be
  next to whatever else was wrong.
- **The web remote's key (0.95.1)** writes `tx: keyed by the web remote` once,
  when a browser's PTT first closes the key, and
  `tx: remote key released (<why>)` when it opens - so a transmission somebody
  at the desk did not make is identifiable as such, which is the first question
  anybody reading this section will have. The reasons are `the remote let go`
  (the page said so: a finger lifted, the tab hidden, the window blurred),
  `the hold expired after <n> ms` **as a warning** (the browser stopped asking
  and the two-second hold ran out - a closed tab, a dropped link, a phone that
  froze the page), `the transmitter stopped on its own` (a fault or the
  dead-man's handle got there first), `the radio would not key`, `the transmit
  page was closed`, `the web server is not running`, and `the transmitter was
  shut down`. The warning is the one to look for: it means the key was opened
  by a timer rather than by a person, so whatever else went wrong on that
  network is in the same log. A `keyed by the web remote` with no matching
  release before the log ends means the application died keyed - and then the
  driver's own lines above say whether the board was silenced on the way out.
- `tx: the writer thread did not return within 1500 ms and was abandoned; the
  board may still be transmitting` is the one line in this product that says
  the radio's state is unknown. It is written when the bounded join in
  `PlutoTx::stopWritingLocked` expires, which means the board stopped answering
  while keyed; the abandoned thread keeps trying to silence it on its own
  connection, and `~PlutoTx` tries again on the control connection.

- **SoapySDR's logger is bridged into ours.** `source/soapy_log_bridge.cpp`
  registers a `SoapySDR::registerLogHandler` handler; FATAL/CRITICAL/ERROR/
  WARNING become `warn` lines, NOTICE/INFO (and the `O`/`U`/`D` stream
  indicators) become `info` lines, all prefixed `soapy:`. DEBUG and TRACE are
  dropped unless `FOXSDR_SOAPY_DEBUG=1` is set, which also lowers SoapySDR's own
  threshold to TRACE. The registration is the only line that touches SoapySDR,
  and `cascade.exe` delay-loads `SoapySDR.dll`, so it must run only after
  `SoapySource::runtimeAvailable()` has answered yes; the classification, the
  prefix and the limit are pure and tested without a runtime
  (`tests/test_soapy_log_bridge.cpp`).
- **The process's stderr is captured** (`core::installStderrCapture`, Windows
  only). An anonymous pipe replaces fd 2 and `STD_ERROR_HANDLE`; a reader
  thread logs each line as `vendor: <line>`. It is installed only for an
  interactive session — never `--frames`, `--selftest`, `--soapy-check` or the
  other tools — and only when nobody is watching stderr. `cascade.exe` is a
  console-subsystem binary, so a Start Menu launch has a console too, one
  Windows created with nothing else attached; the test that separates that
  from a developer's terminal is `GetConsoleProcessList` reporting more than
  this one process. The write end is inheritable on purpose, so the
  device-enumeration child's stderr (which UHD's discovery errors go to) lands
  here as well. The diagnostic log itself never writes to stderr, so nothing
  it does can loop back; the crash handler's one stderr line goes to the
  handle the process had before the capture. Our own `cascade: …` stderr
  lines from the GUI (a GLFW error, a failed backend init) now reach the log
  too, prefixed `vendor: cascade:`, which is a gain: in a windowed session
  they used to go nowhere. Linux is unchanged (the call returns false).
  That capture is how `cascade: GLFW error 65543: WGL: Failed to create
  OpenGL context` reaches a report, and from 0.96.1 one of two lines of our
  own follows it and says what was done about it:
  `viewports: this display would not give a second OpenGL context (GLFW
  65543) - every page stays inside the main window`, written once at startup
  when the probe FoxSDR makes before enabling torn-off pages is refused; or
  `viewports: the display could not create a second OpenGL context (GLFW
  65543) - every page stays inside the main window for this session`, written
  the first time the driver refuses one while running. Either means torn-off
  pages are off and everything is drawn inside the main window — which is a
  restriction, not a fault, and is what 0.95.0 took an access violation
  instead of doing. `viewports: single (FOXSDR_SINGLE_VIEWPORT set)` is the
  developer switch and means the same thing on purpose.
- **Both are rate-limited and scrubbed.** Twenty lines a second per source,
  then one `N more lines suppressed` per second, because a driver that has
  lost its device says so on every failed read and would otherwise push every
  state-change line out of the ring in seconds. Before anything is kept,
  `core::scrubVendorLine` replaces the value after the word "serial" with
  `<stripped>` and masks every digit on a line that mentions a frequency,
  tuning or hertz — SoapyRTLSDR's `Setting center freq: N` is exactly the
  thing PRIVACY.md promises a report never carries.
- **Every line is scrubbed again on the way out (0.99.33).** Until then only
  driver lines were scrubbed, and FoxSDR's own `source: asked for 433.917000
  MHz ...`, `patch: radio 'Loft Airband' running ... MHz` and `tx: keyed -
  ... at ... MHz` went into uploaded reports verbatim. Now
  `core::scrubUploadLog` runs at the one point each upload is assembled
  (crash_upload.cpp's payload, diag_report.cpp's bundle) on EVERY line:
  serials (the word, `SN:`/`S/N:`, `AIRSPY_SN:`, SoapySDR's ` :: ` label,
  a USB instance id's last segment) become `<stripped>`, a path's account
  name `<user>`, single-quoted names `'<name>'`, and on a line that mentions
  a frequency every number becomes `#` unless a unit, a word or its shape
  says otherwise (S/s, ms, dB, `firmware 2.1`, `(-5)`, `0x...`, `R820T`).
  Since 0.99.44 the report's own plugin inventory is passed in too, and a
  listed plugin's name and version survive wherever they appear (0.99.43
  turned `plugin: loaded 406 MHz Beacons 1.0.0` into `plugin: loaded # MHz
  Beacons #`, GitHub issue 5); every other number on the line is judged as
  before. libusb info/debug lines that list the machine's USB devices are left out,
  one line counting them. diag_log.hpp has the rule in full and
  tests/test_upload_scrub.cpp holds each part against every upload path.

And the report itself carries two more facts, in a `--- process ---` block
after the stack (before the stacks, in a freeze report): `uptime-sec`, the
session's age at the fault, and — crash reports only — `fault-thread-own`,
whether any frame of the walked stack lies in the main executable. `no` means
a thread a vendor driver created and ran entirely in its own code, which is
where the 0.88.0 report could not be placed; `unknown` means the walk could
not be taken. The uploader forwards them as `uptimeSec` and `faultThreadOwn`
(`"true"`/`"false"`/`""`), and no rung of its trimming ladder that carries a
log now carries fewer than 80 lines (`kMinUsefulLogLines`, with static asserts
that the payload cap and the ring are at least that).

A report about a **fault in a child process** carries two more lines in that
same block, and no stack at all (0.96.4). `child-exit-code` is what the child
died of and `child-attempt` is which try it was (the uploader reads only
`uptime-sec` and `fault-thread-own` out of this block, so neither line is sent:
what reaches the server of a child death is the `reason` line, the `code`, the
`signature` and the log); the stack section says, in words, that the fault was
in another process and this one has nothing to show. Since 0.99.62 such a
report is written only for a death the child's own crash handler did not
report (see *One report per death*).
It is written that way because it had to be: the enumeration helper
(`cascade --enumerate-json`) is expected to die occasionally by design, and the
report filed at each death used to walk the stack of whichever worker thread
noticed — which killed the parent the containment had just saved, twice
(`B9D41A8D` on 0.64.0, and `crash cascade.exe @ captureFramesGuarded` on
0.96.3, a stack overflow on a spent worker stack that no `__try` can catch).
The frame capture now refuses to walk a thread with less than 64 KiB of stack
left, and refuses to substitute the calling thread's stack for a supplied
exception context that is null or zeroed. No minidump is written for a child
fault either: a dump of the process that survived cannot document it.

### `kind: stall` — the display stopped, not the application

A freeze report's `kind` is `hang` or, since 0.96.4, `stall`, and every one of
them carries a `note` line saying which in a sentence.

`stall` means the stalled thread was in a kernel wait with a **display driver,
OpenGL or Direct3D/DXGI module underneath it** — the application waiting for
the graphics stack to present, which is what a monitor being switched off, a
resolution change, a GPU reset or a remote session reconnecting does. The case
that forced it: `hang ntdll.dll @ cascade::gui::AppWindow::run` on 0.96.3,
1177 s into a session, with `GLFW error 65544: Win32: Failed to query display
settings` in the log and `atio6axx.dll` — AMD's display driver — under a
five-second `SwapBuffers`.

The classification is made from the frames and nothing else
(`HangWatchdog::isDisplayPresentationStall`), and it is deliberately not "a
stall with SwapBuffers on top": a 0.96.2 report had SwapBuffers on top too and
was a dead SDRplay service, a real fault that must keep being reported. A stall
also gets its own grouping signature (`STAL`, against `HANG`) so the two can
never share a group, and **the uploader keeps it on the machine**: the crash
store accepts `crash` and `hang`, and a display driver's behaviour is not this
product's fault to file. The sidecar beside it says so rather than implying the
file was unreadable.

**What does leave the machine is how many there were, and only that (since 0.99.62).** Keeping
the report local made the frequency invisible: a user whose window freezes every
few minutes and one whose never has looked identical from outside. So the usage
record (PRIVACY.md, *What is sent when usage reporting is enabled*) carries one
integer, `stalls`: how many freezes the watchdog classified `stall` and the
server has not yet been told about. No stack, no module, no driver name, no time
of day. It is counted by the same call that writes the `kind` line
(`HangWatchdog::recordFreezeKind`, on the watchdog's own thread, once the kind
is known); a `hang` is not counted at all. The classification needs the report
to be captured, so **a stall is counted only while Diagnostics is on** - with it
off nothing is detected, and nothing is counted.

Where the running number lives is dictated by the way a stall most often ends:
the user closes the frozen window from the taskbar, which is `TerminateProcess`,
and nothing on the GUI thread ever runs again - so neither memory nor
`config.json` can hold it. `core::StallLedger` keeps it in a file beside
`config.json`, `telemetry-stalls`, holding the install id it belongs to and the
number, and **the watchdog's thread writes it** the moment the freeze is
classified (temp file and rename, so a kill mid-write leaves the old number or
the new one). The GUI thread only ever reads an atomic. The next start reads it
back, sends it in the record for the session that just ended, and subtracts
exactly what it sent **only if the server answered 2xx** - the transport reads
the HTTP status and nothing else. A send that fails, or never happens, leaves it
to ride the next record. Reporting off: nothing is counted, nothing is kept, the
file is removed, and a file left by an earlier identity is ignored rather than
attributed to a new one. `tests/test_stall_count.cpp` holds all of this, including
the real capture on the real watchdog thread; the Worker side is held by
`telemetry-worker/worker.test.mjs`.

Before any of that, two things stop such a report being written at all
(`gui/present_grace.hpp`): the watchdog is paused for a bounded 10 s grace when
a display change is seen — `WM_DISPLAYCHANGE`, counted in the window procedure,
or GLFW's own 65544 display-settings error — and paused for as long as the
window is iconified or hidden, because a window nobody can see is not expected
to present. Both share one counted pause, and the log says
`display changed - presentation stalls for the next 10000 ms are not reported`
when the grace starts. Since 0.99.61 that pause is an application-paced one like
the others (rule 2b below) and excuses a stall for at most 30 s: the "bounded"
grace was only bounded by the frame loop noticing it had expired, and the
"as long as it is minimised" pause was not bounded at all, so a loop that
stopped while either was held was never reported. A minimised window whose loop
keeps turning is still never reported, however long it stays minimised.

### What a freeze's signature is built from (0.99.62)

Until 0.99.62 a freeze report's `signature` was hashed from the report's kind
tag (`HANG`, or `STAL` for a display stall) plus the **module and offset of frame
0** of the stalled thread. A frozen GUI thread is almost always parked in the same
few kernel wait stubs - `ntdll.dll`'s wait for a handle or a condition variable,
`win32u.dll`'s message wait - and frame 0 of every one of those waits is the same
instruction whatever the thread is waiting *for*. So freezes from completely
different causes (the audio open, a shell call, a lock a worker holds) shared one
signature. The upload de-duplicates by signature for 24 hours
(`decideUpload`, `kDedupSeconds`, `src/core/crash_upload.cpp`), so a user who met
two different freezes in a day sent the first, and the second was marked
`duplicate` in its `.upload` sidecar and never arrived. The server was not the
problem - it re-groups what it receives by the nearest named frame of our own code
- the loss was on the client, before the upload.

The signature now identifies **the code of ours that was waiting**
(`core::freezeSignature`, `src/core/diag_report.cpp`): the hash is the kind tag,
plus the module and the module-relative offset of the **first frame, nearest the
top, of the stalled thread that lies in the main executable** - the same test
`fault-thread-own` applies to a crash. Two properties are held by
`tests/test_hang_signature.cpp`, against a real watchdog and real blocked
threads: two stalls parked in the *same* kernel wait from two *different* functions
of the program get different signatures, and the same stall twice gets the same
one. Through the real `decideUpload` the first sends two reports and the second
sends one.

- **When no frame of the stalled thread is ours** (a thread idling in the thread
  pool, a stack of vendor code only, a Linux non-PIE executable whose load bias of
  0 cannot be told from "unknown") it falls back to the old key, frame 0, and still
  gets a signature.
- **A display stall keeps its own tag.** `STAL` and `HANG` are different hash
  inputs, so a monitor being switched off can never share a group with a deadlock
  even when both are keyed by the same frame. Stalls are still kept on the machine
  and never uploaded.
- **The form is unchanged**: 16 uppercase hex digits, the shape the dedup memory,
  the wire contract and PRIVACY.md already describe. What leaves the machine does
  not change, and neither do the payload's `module` and `offset`, which still name
  frame 0 of the stalled thread; the server's re-grouping reads the stacks.
- **The offset is build-specific.** One freeze in two builds has two signatures.
  That has always been true of a crash signature and is correct - the offsets
  differ, and so do the symbols needed to read them. (A frame-0 key in `ntdll.dll`
  did survive across builds, but it grouped everything.)
- **The nearest frame of ours can still be a shared wrapper.** If two different
  causes both reach their wait through one function of ours - a helper every call
  site goes through - they share that frame and so a signature. The full stacks in
  the payload are what tells them apart; this change removes the kernel stub from
  the key, not the possibility of a common helper.
- **The stalled thread is walked before the header** whenever its frame 0 is not
  already in our image (it used to be only when frame 0 was a kernel wait). The
  signature needs the frames under the wait, and the `kind` line, which is the first
  line of the file, is decided from the same walk. A thread whose frame 0 is already
  ours is keyed by that frame exactly as before, with no earlier walk. The cost is the
  one phase 1b already carried and documents in `hang_watchdog.cpp`: a wedge in that
  one walk costs the whole report.
- **Migration.** The uploader reads the signature from the report file's own
  `signature:` line and never recomputes it, so a freeze report written by the
  previous build and swept by this one is judged by its old-style signature exactly
  as before: nothing is lost and nothing is sent twice because of it. The dedup memory
  in the config is read as it always was (a signature and a time). After the upgrade a
  freeze that the old build had already sent in the last 24 hours is a *new*
  signature, so it can be sent once more - at most once per new signature, inside the
  existing five-a-day cap - and the server, which groups by named frame (above),
  is not asked to tell the two apart by their signatures.
- **On Linux** the same function is used: the stack is already walked in full
  before the header there, and the main image is module 0 of the `dl_iterate_phdr`
  snapshot. This path has not been compiled or run by the author of this change.

**Crash signatures are not changed**, and they have a narrower form of the same
loss: the key is the code, the faulting module and the offset of the *faulting
instruction*, so two different faults reached from different callers of ours share a
signature only when they fault at the very same instruction of one module - the
shared system-DLL case (an unrelated fault in the same `ntdll.dll` or `ucrtbase.dll`
routine) - and a crash ends the process, so a second one in 24 hours also needs a
restart. An absorbed vendor fault is the opposite constraint: it recurs
indefinitely, and one upload a day is the behaviour that keeps it from filling the
five-a-day allowance, so folding the caller of ours into *its* key would multiply it
by the number of distinct call sites that reach it.

### Shell calls are made under a watchdog pause

`ShellExecute` blocks the GUI thread for as long as the shell takes, and for an
elevation or SmartScreen prompt that is as long as the **user** takes.
`hang ntdll.dll @ cascade::gui::AppWindow::launchInstaller` (0.96.2, 28 s
uptime) is the watchdog reporting a consent dialog the user was reading. Every
shell call in the application — the update installer, the reports folder in
both places it is offered, the privacy-policy link — now goes through
`gui::runShellOpen`, which brackets it in a watchdog pause, exactly as
`core/hang_watchdog.hpp`'s false-positive rule 2b has always said a native
modal dialog must. Since 0.99.61 it is the one **user-paced** pause
(`pauseUser`/`resumeUser`, via `AppWindow::watchdogShellHooks`) and the one pause
that is not capped, because what it waits for is a person; see rule 2b in *The
5-second threshold* below. `cascade --frames N` prints the number of pauses a
run took, so these are visible as a count and not only as a sentence.

**On Linux** (this port, 2026-09-15) `AppWindow::shellOpen()` no longer just
returns `false`: it hands the target to `xdg-open` via `fork`+`execvp` — never
a shell, so a target string is never reinterpreted — double-forked so the call
does not wait for whatever `xdg-open` starts (which can outlive it by as long
as a freshly launched browser stays open) and so nothing is left as a zombie
regardless. `cascade::gui::posixShellOpen()` (`gui/shell_open.hpp`) is the free
function this runs through, chosen so a test can drive it without constructing
an `AppWindow`; `tests/test_shell_open_posix.cpp` points
`FOXSDR_SHELL_OPEN_EXE` at a recording stand-in rather than a real `xdg-open`.
There is no installer to launch on Linux (`AppWindow::launchInstaller` still
returns `false` there, unchanged), so the update banner's action is instead an
"Open foxsdr.com" button through this same path — see the "Building (Linux)"
section of the top-level README.
### The audio device is opened off the frame loop

`hang ntdll.dll @ InitializeWaveHandles` (0.96.4, Windows 11 26200, an RTL-SDR
Blog V4, 350 s uptime) is the Sinks panel's device combo: `Pa_OpenStream` is
`waveOutOpen` on the WMME host API, it has no timeout, and it held the GUI
thread from `AppWindow::drawSinksSection` down through `wdmaud.drv` into
`ntdll` — the watchdog filed at 5 s and the application's own log records the
frame loop returning 57 seconds later.

A pause alone would have deleted the report and kept the freeze, so the open
moved instead. `gui::AudioOpen` runs it on a worker and the requesting frame
waits `kOpenBound` (1500 ms) for the answer under a `WatchdogPause` — a device
that is merely slow still opens inside the click that asked for it — and then
goes back to rendering, with the Sinks panel reading **OPENING** and "audio
device busy — still opening" until the driver replies. Both callers go through
it: the combo, and the once-a-second reopen the audio watchdog performs when a
stream has died, which is the worse of the two because a device that has gone
away is exactly the one whose open blocks. Two opens are never in flight at
once; a click made during one is queued, and only the last of them runs.
`tests/test_audio_open.cpp` drives a real `HangWatchdog` against an opener that
takes 2.5 s and requires no report and an unbroken heartbeat.

What none of this proves: that a real librtlsdr or UHD line arrives. The
bridge and the capture are exercised with synthetic lines
(`tests/test_vendor_lines.cpp` writes through both the CRT and the Win32
handle in-process); a driver with its own statically linked CRT reads
`STD_ERROR_HANDLE` when it is loaded, which is after the capture is installed,
and that reasoning is what puts its lines in the pipe — a bench with an
RTL-SDR is the check.

### The settings-folder poll is answered off the frame loop (0.99.61)

`hang ntdll.dll @ __std_fs_get_stats` (0.99.59, Windows 10.0.19045, an RTL-SDR
open and streaming, 880 s uptime). Resolved against the symbol archive, the GUI
thread's stack was `main` → `AppWindow::run` → `AppWindow::drawUi` →
`AppWindow::testerLinkPoll` → `core::claimLinkRequestFile` →
`__std_fs_get_stats` → KERNELBASE → ntdll, and it stayed there for five seconds
or more. `testerLinkPoll()` runs once a second in **every** session, tester or
not, because it has to be listening before a beta-tester link click can arrive,
and what it asked was whether `%APPDATA%\foxsdr\link-request` exists: a
synchronous `std::filesystem::exists()` on the GUI thread, justified in a
comment as "a stat() is cheap". It is cheap until the directory it names is slow
to answer, and that directory is also where `config.json` is written — the
reason 0.97.2 had already moved the config write off this thread.

**What is not established is why that user's disk was slow.** The code comments
list a redirected or network profile, a cloud-synced folder, an antivirus
holding the directory and a spun-down disk; none of them is known for this
user. The class was reproduced with a config path on a network share that never
answers (an unreachable address): one frame in three seconds, a 22.5 s gap in
the frame loop, and one hang report. Those figures are from that reproduction
run; the test below does not assert them.

A watchdog pause was not the answer, for the reason the audio open gave: a pause
deletes the report and keeps the freeze, and a poll that fires every second
would have the watchdog paused for the whole session. So the call moved.
`gui::LinkRequestPoll` (`src/gui/link_request_poll.hpp`) runs it on one
background worker and **nothing waits for it**, not even for a bound
(`AudioOpen` holds the requesting frame for up to 1.5 s because a user is
waiting on a device; nobody is waiting on this):

- A frame that finds the poll due calls `request()`, which starts the worker and
  returns; **every** frame calls `poll()`, which collects a finished answer and
  returns, so a token is acted on the frame it lands rather than up to a second
  later. The question is still asked at about 1 Hz.
- **One worker at a time.** `request()` while a check is still out does nothing,
  so a directory that never answers costs one parked thread, not one a second.
- The worker owns what it touches by value and never sees the window, and a
  throw inside it is turned into "nothing found" so that `future::get()` can
  never rethrow on the GUI thread.
- At quit a check still blocked in the filesystem is given 250 ms and then
  abandoned, not joined — a join would be the same hang under another name.
- The token is a credential: it is never logged, and neither is the directory.

Two log lines are its whole record, because a poll nobody sees has no panel:

- `tester link: the configuration directory has not answered a status check for
  N s - the check is waiting on a worker thread, the window is not` — a
  warning, written once per check, when one has been out for five seconds (N is
  the seconds it had been out, rounded to a whole number, so normally 5).
- `tester link: the configuration directory answered after N s` — written once,
  when a check that was reported stuck finally comes back, with how long the
  whole check took, rounded the same way.

A report whose log carries the first line and never the second was written while
the folder was still not answering. Seeing either line at all means the window
kept drawing, which is the point of the change; it is also now the only evidence
that the folder was slow.

`tests/test_link_request_poll.cpp` holds it three ways. On Windows it points a
real `AppWindow` at a config path on a share whose server never answers (an
address in TEST-NET-1, a different one every run, because the SMB client
remembers an unreachable server and fails the next attempt quickly), under a real
`HangWatchdog`, and requires no report; if the machine answers the dead share
quickly the block says so and SKIPS, and never passes by not having been slow.
A source scan requires that no file under `src/gui` calls
`claimLinkRequestFile` except through `link_request_poll.hpp`. And on any
platform the same loop runs with a claimer that sleeps standing in for the slow
disk, against `LinkRequestPoll` alone and against a real window's
`testerLinkPoll()`, with the synchronous call of the same claimer as the control
that proves the harness can see the fault; around them it requires that the
answer still arrives, that only one worker is ever out, that quit does not wait
for a wedged one, and that a slow check is said once and not once a frame.

**Two open items this fix did not cover**, found while reading for it, and the
same class of fault:

- `AppWindow::drawFittedModulesWindow` called `std::filesystem::file_size` on
  every plugin's file, every frame, while the Fitted Modules window was open (it
  was the only way the plate got a size). **Closed after 0.99.62**: see *The Fitted
  modules window asks the disk for nothing*, below.
- The filesystem work inside `AppWindow::rescanPlugins` — the inventory read, the
  directory scan and the load of every module — also runs on the GUI thread. The
  watchdog pause around it hides a slow disk from the watchdog (rule 2b, and
  capped at 30 s since 0.99.61), but the window still freezes for as long as the
  disk takes, and a rescan that outlasts the cap is now reported. **Still open**:
  see *A plugin rescan runs on the GUI thread*, below.

Neither is a claim that no other synchronous filesystem call remains on that
thread. The audit made for that change covered `src/gui` (every `std::filesystem`
call, every stream and `fopen`): the calls that remain there are made on a user's
press, at start-up or at exit, not per frame. Nothing outside `src/gui` was
audited for what it does when the *GUI thread* calls it.

### The Fitted modules window asks the disk for nothing (after 0.99.62)

The call named in the open item above ran **sixty times a second for as long as
the window was open**, once for every loaded plugin: twenty modules is twenty
synchronous stats per frame, on the thread that draws the window, in the plugin
folder — which for a Store package is a redirected profile — and the window is
the one the plugin **Rescan** key lives on, so a session that rescanned plugins
had the window open. Where the folder was slow, every frame was as slow as the
slowest of the twenty answers; one stat of a path on an unreachable share was
timed at 26.7 s on the machine this was written on (that is the extreme, not the
expected figure).

The size is now a property of the record. `PluginHost::scan()` takes it from the
directory listing it is already reading (`LoadedPlugin::fileBytes`; on Windows the
listing returns the size with the name, so no further call is made), refused
records included, and `makeFittedModule` carries it to the plate. 0 still means
"not measured" and is never drawn as a size. A figure that may be as old as the
last scan is the whole fix: an installed or updated plugin rescans, and a file
replaced by hand shows its old size until the next rescan, as it already showed
its old version.

`tests/test_plugin_host.cpp` pins the scan (two files of different sizes get their
own, a refused file has one, a record nobody scanned says 0);
`tests/test_plugins_view.cpp` pins the carry, with a path that names no file;
`tests/test_fitted_modules_no_disk.cpp` is the source scan that holds the draw
function to no `std::filesystem`, stream, `fopen` or Win32 file call at all — and
runs against the old code first, so a scan that cannot see the call it forbids
would fail instead of passing blind. It is a scan of the source because no test
can run that window against a slow disk: the call site is in `AppWindow`, which
needs a GL context. What no test shows is the freeze itself on a real slow
folder; the cause of it was established by reading, and the figure above by
timing one call.

### A plugin rescan runs on the GUI thread, and says where its time went (after 0.99.62)

A 0.99.58 session (a Store package, twenty plugins) logged the line that opens a
rescan and, **119.976 s later**, the line for the first plugin it had loaded. The
rescan holds a watchdog pause, which in 0.99.58 had no limit, so the freeze was
not reported; the only trace was those two timestamps. This is what is established
about it, by reading, and what is not.

**What `rescanPlugins()` does, all on the GUI thread, in this order:**

1. `decoders` — `PluginRunner::clear()`: every decoder instance's `destroy()`
   (third-party code).
2. `patch` — `Runner::flushNow()` on the patch page's runner and on every patch
   radio's: waits, with a **spin that has no bound**, for the DSP thread (or a
   radio's reader) to leave its current block, then destroys the decoders the
   patch holds.
3. `panels and map` — `PluginUi::clear()` (track-source, panel and instrument
   `destroy()`), `BasemapCache::detach()` and `TrackInfoCache::detach()`
   (`destroy()` of the basemap and the aircraft lookup).
4. `unload` — `FreeLibrary` of every module: each one's `DllMain` and static
   destructors, under the loader lock.
5. `inventory` — the quarantined files renamed back, and **every installed file
   re-hashed** against the record made at install, then the retired ones moved
   aside.
6. `load` — the folder listed and every module mapped (`LoadLibrary`, its
   `DllMain`, the descriptor query).
7. `restart` — the log lines, and the decoders created again.

**No wait of 120 s was found on that path, in the application or in the plugins.**
A search of both for a 120-second constant, in the ways it would be spelled, finds
only the old CAT-shutdown story in the watchdog's comments and, in the plugin
repository, the ADS-B decoder's 120 s eviction of a silent aircraft track (a timer
on data, not a wait). The bounded waits that exist near the path belong to other
paths (`PatchRadio::kStopWaitMs` 3 s per radio, on the patch's close;
`GpsReader::kOpenAbandonWait` 1 s). On the path itself the host bounds **nothing**
it calls: the `destroy()` calls (a plugin's own join), the `flushNow()` spin,
`FreeLibrary`, and the file system are each as long as their owner takes. The figure is
also *under* 120 s, by 24 ms, so it is not a single 120-second timeout begun after
the first line was written: such a wait would have ended after 120 s plus the
rescan's own work (3.6 s on the next, warm rescan of the same plugins).

**What could have held it, in the order the code supports** (all inferred, none
shown in that session):

- *A plugin's `destroy()` joining a worker that is itself waiting.* The OpenMapTiles
  basemap's `bm_destroy` joins four tile workers, and each worker does a network
  fetch (bounded by the plugin to 20 s plus one 15 s read), **and file I/O on the tile
  cache that has no bound** (`readCache` / `writeCache`, in the per-user cache
  folder). A stalled file system — which the same session shows: the Record button
  blocked in file creation — holds a worker, and so holds `bm_destroy`, and so holds
  the GUI thread. The plugin's HTTP helper also opens its session with Windows'
  automatic proxy detection, which the per-phase timeouts it sets are not documented
  to cover; that is stated as unverified. The instance and its four workers exist
  for as long as the plugin is loaded (the host attaches it at every plugin
  rebuild), and the workers have work only while a map is drawing tiles; the AIS
  plugin's window opened four minutes earlier is one that feeds the map, and the
  log does not say whether a map was on screen.
- *The disk, in `inventory` or `load`*: every installed file is read and hashed, and
  every module mapped, from a folder a scanner or a synchronised profile can hold.
- *The loader lock*, taken by `unload` and `load`, held by another thread inside a
  `DllMain`.
- The `flushNow()` spin, only if the DSP thread or a radio's reader sat inside a
  decoder's `process()` for two minutes; a patch had been running and was closed
  ninety seconds before.

**Is the window closeable while it runs? No.** The frame loop's `glfwPollEvents()`
is its only message pump and the rescan is one call inside a frame; Windows marks
the window "not responding" after five seconds and a click on its close button
cannot reach the application.

**What this change adds** is the evidence, not a cure. Every rescan writes one line:

```
plugins: reload took 3.6 s (decoders 0.0, patch 0.0, panels and map 0.3, unload 0.4, inventory 1.9, load 0.9, restart 0.1)
```

— seven laps, in that order, with the step's own seconds — and a rescan of five
seconds or more is a **warning** that says the window did not draw for that long and
names the slowest step. Written on every way out of the function, the early return
included; it contains no plugin name, file or path. `tests/test_phase_clock.cpp` holds
the clock (laps in order, a repeated name adding to its lap, the sentence turning at
the threshold, the log level); `tests/test_plugin_rescan_log.cpp` holds the line
through the real `AppWindow::rescanPlugins`, and that only the rescan, not the exit
or a removal, writes it. Since 0.99.61 a rescan past `kExcuseCapMs` (30 s) is also
**reported with every thread's stack**, which names the plugin whose worker the GUI
thread is waiting on; the two together say which of the above it was.

**What this does not do is stop the window freezing**, and that is a design decision
rather than a repair:

- *A. Destroy on a helper, with a bound, and abandon what will not stop.* The GUI
  thread takes each handle out of its container (cheap; the runner already moves
  its instances out before it destroys them), hands them to one helper thread that
  calls `destroy()` in the same order, and waits a few seconds. If the helper is
  not done, the rescan **does not unmap anything** — a module with a thread inside
  it cannot be unmapped — marks it "did not stop", and carries on from the frame
  loop when the helper finishes; a module whose `destroy()` never returns stays
  mapped for the life of the process (the satellites plugin does the same to its own
  worker) and is **not loaded again until the program is restarted**, with a notice
  on the Fitted modules plate saying so. The window freezes for at most the wait,
  under the five seconds Windows takes to grey it. The cost: `destroy()` stops being
  called on the thread that called `create()` (the ABI promises "the host's control
  thread", not the same one, and `patch` code comments say the GUI thread is where a
  handle may die); `BasemapCache::detach()` deletes GL textures and must keep that
  part on the GUI thread; and `rescanPlugins()` stops being synchronous for its
  nine callers, three of which read the new list straight after.
- *B. A modal reload.* The same work on a worker while the GUI thread runs a small
  loop of its own that pumps events and draws "Reloading plugins — stopping
  <step>…". Callers stay synchronous. It keeps the window drawing and closeable
  but not usable, it has no answer for a plugin that never stops except to wait,
  and the work that touches ImGui, GL or window state (the map and basemap
  detach, `syncMapPagesToSaved`, `refreshPluginRunner`) has to be split back out
  onto the GUI thread.
- *C. Fewer rescans.* The catalogue fetch rescans every plugin whether or not
  anything changed, because the retirement floors it caches only take effect on
  a scan; skipping the teardown when the inventory and the blocked set are
  unchanged removes most rescans a session sees (the log of that session shows two,
  three seconds apart).

Recommended: **A with C.** C is small and removes the exposure in the common case,
A bounds the case that remains and is the only one that gives the conservative answer
for a plugin that never stops — leave it mapped, say so, do not load it again — and B
trades a smaller change for a window the user still cannot use.

### The recording's file is opened off the frame loop

A freeze report from 0.99.58 (Windows 11, 151 s into the session). Resolved
against the symbol archive, the GUI thread's stack was `main` → `AppWindow::run`
→ `AppWindow::drawUi` → `AppWindow::drawMenuColumn` →
`AppWindow::drawRecorderSection` → `core::Recorder::start` → the C runtime's
file open → KERNELBASE → ntdll, and it stayed there for more than the
watchdog's five seconds. The Record button's handler called `Recorder::start`
on the frame that drew the button, and `start()` creates the recordings
directory and opens the file before it returns: two synchronous filesystem
calls that last as long as the disk takes to answer. The Record audio button,
the Record key and the web remote's and plugins' record controls made the same
call, so all of them froze the same way.

**What is not established is why that user's disk was slow.** The usual
suspects are a synchronised or network folder, a drive that has spun down and a
scanner holding the path; none is known for this user. Nothing here has been
run against a genuinely slow disk: the tests below stand a sleeping opener in
for one.

A watchdog pause was not the answer, for the reason the audio open gave: it
deletes the report and keeps the freeze. So the open moved.

- **`Recorder::start` is three steps, and only the middle one waits.**
  `Recorder::prepare()` (the rate check, the file name, the 44 header bytes: no
  disk), then an *opener* (`create_directories`, `fopen`, the header's write and
  flush: all the waiting), then `Recorder::begin()` (arms the recorder: no
  disk). `start()` is the three in a row, and every caller that can afford to
  wait - the patch page's speaker files, the `--record-check` bench - still
  uses it. The opener is a `std::function` the recorder carries
  (`Recorder::bindOpener`), `Recorder::openFile` by default, which is the seam
  a test stages a slow disk through. It reads its request and touches nothing
  else.
- **`gui::RecordStart` (`src/gui/record_start.hpp`) runs the opener on a
  worker and nothing waits for it**, the shape of `LinkRequestPoll` and
  `ConfigWriter`: a Record press calls `prepare()`, hands the request to
  `request()` and returns; every frame calls `poll()` (after every widget, in
  `drawUi`), which collects a finished open and returns. The window keeps
  drawing for as long as the disk takes; the only cost of a slow disk is that
  the recording starts late.
- **The order contract in `pipeline.hpp` is unchanged.** The tap is still
  installed after the start, never before: the frame that collects the open
  calls `begin()` and only then `Pipeline::setIqRecorder` /
  `setAudioRecorder`, in one function (`AppWindow::finishRecordStart`). The
  worker never sees a recorder, so nothing can be written to a take that has not
  been armed. The take's elapsed clock starts when the file is open, not when
  the button was pressed, and a take opened late is byte for byte the take an
  inline `start()` makes (the test compares both kinds).
- **The user still learns the outcome, in the same words.** A start that
  fails reports the opener's own error - the same text `Recorder::start` gave -
  a frame or more later, in the Recorder section's red line and the web page's
  `recordError`; a refusal that needs no disk (an unrepresentable rate) is
  still immediate. While the file is opening the section shows **Starting IQ -
  click to cancel** (or **Starting audio - ...**) in place of Record, and after a
  second a line under it, `Waiting for the disk to open the file: N s`. The REC
  lamp, the elapsed clock and the web page's `iqRecording` / `audioRecording`
  stay off until the file is open, so they never overclaim.

The edges, and the choices made where the right behaviour was a decision rather
than an obvious fact. Each is tested, in `tests/test_record_start.cpp`:

- **A second Record while one is opening changes nothing** (the button is a
  cancel by then; the Record key and the browser's Record are ignored). One
  worker is ever out, so a disk that never answers costs one parked thread, and
  two opens can never race for one file name.
- **A Stop while it is opening withdraws it.** The "click to cancel" button,
  the toolbar Stop on a running receiver, the web remote's `recordIq` /
  `recordAudio` `false` and the teardown all end up in `stopIqRecording()` /
  `stopAudioRecording()`, which now cancel a start that is still opening. The
  worker cannot be interrupted - it is inside the filesystem - so it is let
  finish, its file is **closed unused** and no take starts. The panel reads
  "Cancelling ... start" until it comes back, and a Record pressed in that gap
  is refused rather than started alongside it. **The file the withdrawn open
  leaves behind is the zero-sample WAV that Record followed at once by Stop has
  always left; nothing is deleted** - the conservative choice, since the open
  had already truncated whatever shared its name.
- **A take whose input rate changed while its file was opening is not
  started.** The header on that file is for the old rate, and a take whose
  header disagrees with its samples replays detuned (the reason an accepted
  rate change ends an I/Q take that is already running). The panel says so as a
  notice, not an error.
- **Quit with an open still pending does not wait for it.** `~AppWindow` gives
  each wedged open 250 ms (`RecordStart::kQuitGrace`, after the watchdog has
  stopped, so outside the shutdown budget) and abandons it: the future is
  handed to a detached thread, which lets the file go when the open finally
  returns. The worker owns everything it touches by value, which is what makes
  that safe. The abandoned thread cannot be stopped from outside; it ends when
  the filesystem call does, or with the process.

Two log lines are the whole record a report will carry of a slow disk, written
by `AppWindow::pollRecordStarts`; neither names the directory or the file:

- `recorder: the I/Q file has not opened for N s - the open is waiting on a
  worker thread, the window is not` (or `audio`) - a warning, once per open,
  when one has been out for five seconds.
- `recorder: the I/Q file opened after N s` - once, when an open that was
  reported stuck comes back.

A report whose log carries the first line and never the second was written
while the folder was still not answering.

`tests/test_record_start.cpp` holds it. A source scan requires that nothing
under `src/gui` calls a recorder's blocking `start()`, that both takes ask
through `RecordStart`, and that the two taps are installed in one function and
only after `begin()`. A real `HangWatchdog` (800 ms threshold) sees a 2.5 s
open called on the heartbeating thread (the control that proves the harness
can see the fault), and then must see nothing - no report, no gap in the frame
loop - against `RecordStart` alone and against a real `AppWindow` driven
through the handlers the buttons, the key and the browser use. Around those:
the late take equals the inline take byte for byte, a late failure has the
inline failure's words, a second Record starts nothing, Stop by each route
leaves no take and closes the file, an exit with two opens wedged takes a
fraction of the 2.5 s they need and both files are closed afterwards, and a
slow open is logged once and not once a frame.

What this does not show: the panel's drawing is not read by any test (only the
state it draws from is), the web page does not show a start in progress (its
button reads Record until the file is open, and a Record sent in the meantime
is ignored), and no real slow disk was involved.

### What a plugin playing sound writes (0.93.0)

A plugin holding `CASCADE_CAP_AUDIO_OUT` **replaces** the demodulated audio
while it decodes, which makes "the radio went quiet" and "a decoder took the
speakers and then starved" the same complaint from the outside. `PluginRunner`
writes two kinds of line so a report can tell them apart, both from
`pollAudioDiagnostics()` — called by the GUI's existing `drainText()` poll, on
the GUI thread, so nothing here runs on the real-time path. The audio path
itself only records fixed-size events under its lock; the formatting, the
clock and the log write all happen on the side that is allowed to do them.

- **The transition line**, one per changeover, naming the plugin and what it
  is handing over: `audio: DAB+ is playing (48000 Hz stereo, no resampling
  needed)`, or `… (32000 Hz mono, resampled to 48000)` when the plugin's clock
  is not the sink's, and `audio: DAB+ stopped; demodulated audio returns` at
  the other end. A third form appears only when two plugins want the speakers
  at once — `audio: APT also wants the speakers; DAB+ has them and keeps them`
  — and is written **once per contention**, not once per block, because the
  losing plugin asks again every time the sink does.
- **The gap digest**, at most once a minute and silent unless something
  actually broke up: `audio: plugin audio (DAB+) came up short in 37 blocks in
  the last minute (154 ms of silence)`. This is the sibling of the sink's own
  starvation line and has to be read beside it: a plugin that cannot hand over
  a full block is charged a gap and the shortfall is filled with silence,
  which sounds exactly like the audio ring running dry and is repaired
  somewhere else entirely. The clock starts at the first call rather than at
  construction, so a runner nobody drains never reports a minute that did not
  happen. The same two counters are on the **AUDIO - UNDERRUNS** card and in
  `/api/status` (`audioPluginGaps`, `audioPluginGapFrames`), beside
  `audioSource` — the name of whatever is holding the speakers.

### What a "no audio" report carries (0.99.61)

0.99.58, an NESDR SMArt v5, "no audio from my speakers": five minutes of log and
not one line said whether a sound output had been opened at all. The only audio
line the application wrote was the starvation digest from
`AppWindow::pollAudioHealth` — `audio: N starved callbacks in the last minute (N
priming), ring low water N ms of N ms` — which is silent unless a callback
starved, and which is not even evaluated when no device has ever opened. Nothing
starves a stream that is open and being fed zeros, or one whose callback never
runs. So four different situations — never opened, opened and died, open and
silent by choice, open and silent by accident — left one identical log, and the
bundle held the source, the rate and the radio and nothing about the speakers.
**What this change establishes is what the next report can say. It does not
establish why that user's output was silent.**

**The absence of an `audio: N starved callbacks` line does not mean the output
was healthy.** A closed squelch, the Mute key, a decoder plugin holding the audio
down and a volume of zero all feed the sink silence at the full rate; nothing
starves and nothing is written. Read the bundle's four lines below, not the
absence of a warning.

`AudioOut::open` now writes one line per attempt that has something new to say,
after it has released the stream lock (a log write is a disk write, and that
lock is the one every query `try_lock`s on):

- `audio: output opened - <host API>, <1|2> channel(s), <rate> S/s, <system
  default | a chosen device>[, latency <N> ms]` — information. The latency is the
  figure PortAudio actually granted (the hint, rounded up to what the host API can
  honour); it is left off if PortAudio does not report one.
- `audio: output could not be opened - <reason>` — a warning. The reasons the
  code gives are `PortAudio did not start`; `the request itself was unusable (a
  rate or a channel count)`; `no default output device`; `no such output device`;
  `<host API> device has N output channel(s) and N are needed`; and `<host API>
  refused the stream: <PortAudio's own error text>`, with the operating system's
  sentence appended in brackets when the host API reported an unanticipated
  error. The wording avoids "range" and "offset" on purpose: the log scrub masks
  the digits of any line that sounds like tuning, and the counts here are what a
  reader needs.
- **A repeated refusal is written once, then at most once a minute.** The audio
  watchdog retries a dead stream every second and the ring holds 256 lines, so a
  line per retry would push the whole session out of a report in about four
  minutes. A refusal for a different reason is always written, and so is the
  first refusal after a success.
- **The device's name is never written** — only the host API, which names a
  driver model (`MME`, `Windows WASAPI`, ...) and not a person's device.
  Operating-system labels are often a person's name (`Headset (Alice's AirPods
  Pro)`), and this log goes into bundles that are pasted into public bug
  reports; it is the same rule as `source::loggableSoundCardDescription`.

The bundle (and the context block of a crash or freeze report, which carries the
same bytes) has four new header fields, listed in `PRIVACY.md` and held to that
list by `tests/test_diagnostics.cpp`:

- `audio-output:` — `open, <host API>, <1 channel|2 channels>` with `, restarted
  N time(s)` once the watchdog has had to reopen it; `opening` while a worker is
  inside the driver's open; `stopped - the stream died and is being reopened`;
  `none - no output device has opened`; or `(unknown)` when nothing filled it in
  (the enumeration child's report, a headless run), so an unfilled context cannot
  read as a healthy one.
- `volume:` — the volume control, `N%`.
- `audio-muted:` — `no`, or who: `you` (the Mute key), `a decoder plugin` (an I/Q
  decoder holds the audio down while the receiver is on one of its presets),
  `transmit key`, or several joined with ` + `. A plugin is described and **not
  named**, because its name would say which band the receiver was tuned to.
- `squelch:` — the threshold, the gate's own state, and, when the receiver has
  measured one, the level the gate is judging: `-50 dB, closed (signal -63 dB)`.
  Whole decibels: a level, not a frequency. It is the gate's own state
  (`Pipeline::squelchOpen`, published once per DSP block) and not a comparison of
  the signal meter with the threshold, because the gate opens above the threshold
  but closes only 3 dB below it and holds for 100 ms, so a meter just under the
  threshold can still be passing audio.

None of the four is uploaded: `crash_upload.cpp` does not read them, as it does
not read `ppm:` either.

**The bundle's `ppm:` line was wrong until this release.**
`currentDiagnosticsBundle` left the context's value at its default, `off`, and
the bundle re-renders the context from that struct, so every bundle said `ppm:
off` whatever the setting was, over the correct line the once-a-second refresh
had rendered. It now carries the real text.

What the tests establish: `tests/test_audio_open_log.cpp` that a request for a
device that cannot exist writes the refusal line with a reason, that twenty
repeats of it write nothing more, that a different refusal is written, that the
default device writes whichever line matches what this machine did, that no
output device's name appears in the line, and that a success after a refusal is
written; `tests/test_diagnostics.cpp` that the bundle prints every state of the
four fields and that its header labels, the declared field list and `PRIVACY.md`'s
table name the same set; `tests/test_diag_audio_app.cpp` that a real window hands
the bundle its own volume, mute and squelch and that the once-a-second refresh and
the bundle cannot describe the sound differently. What they do not stage: the
once-a-minute repeat of a standing refusal, a device that opens and then dies, or
a host API that reports an unanticipated error.

### The context block follows the session, not the last second (0.99.62)

Three crash reports from 0.99.59 carried a context block that read `source:
siggen`, `device-open: no`, `sample-rate: 2000000` and no radio model, while the
log tail of the same report said a radio had just opened: `source: opened miri
(soapy) at 2000000 S/s` one second before the fault; `source: opened ADALM-Pluto
(network) (pluto) at 30720000 S/s` immediately before one, the rate that was the
whole cause of that crash and nowhere in its context; and `patch: radio node 4
running SDRplay RSP1A (SoapySDR) at 2000000 S/s`. A fourth report, from a session
whose radio had been restored from the saved configuration at start-up, had a
correct block.

**What the code did (proven, by reading it and by the test below).** A fault
handler cannot compute anything, so the block is rendered on the healthy path and
the handler writes out the bytes. Through 0.99.61 that render, `AppWindow::
refreshDiagContext`, was called from exactly three places: once in `run()` before
the first frame, by `currentDiagnosticsBundle` (Copy diagnostics and the report
page), and once per 60 rendered frames in the frame loop - about a second at
60 Hz. No source change, rate change, mode change, plugin rescan or patch radio
called it. So the block was never more than about a second old, **and exactly that
old after a radio opened**. The premise "it is refreshed at start-up but not after
a radio is opened from the Source list" was half right: there was a refresh, a slow
one, which is why the fourth report - fault long after a restore - was correct.
`tests/test_diag_context_app.cpp` drives a real `AppWindow` through an interactive
open with no frame drawn, as a fault one millisecond after the open would see it:
on the unmodified tree the block read `source: siggen`, `device-open: no`,
`sample-rate: 2000000` against the window's own `rtlsdr`, open, 2048000.

**What is inferred, not proven.** That each of the three faults came inside that
second: reports (a) and (b) put the open one second and "immediately" before the
fault, which fits, and nothing else in the code produces a stale block for longer.
Report (c) was **not stale**: `source: siggen` was true of the receiver, and the
radio was a second signal path the block did not describe at all.

**The fix.** One builder, `AppWindow::currentDiagContext`, is the only place the
context is assembled from the window's state (the Copy diagnostics bundle used to
carry a second copy of it, which re-renders into the same buffer and had left `ppm:`
at its default in every bundle until 0.99.61). `refreshDiagContext` renders it and
is now called:

- on **every frame** (the net under everything below: a writer that forgets the
  call is wrong for a frame, not for a second - and the volume, mute, squelch gate
  and sound output, which change by other routes, are never more than a frame old);
- at the end of `applyConverterForSource`, the step every install, swap and close of
  the source ends with (the Source list's asynchronous open, its failure and its
  fall-back to the generator, the swap back to the generator, the patch page
  handing a radio back, the I/Q file, the sound card, the driver-fault reopen, and
  the startup restore) - after `installSource` and the `sourceKind_`, `device_` and
  `deviceModel_` assignments around it, so the block is rendered from a state that
  agrees with itself;
- on every way out of `followInputRate` (the Rate combo, a plugin preset and the
  browser's rate request all end there, and so does every source change), with the
  rate the source READS BACK, not the one asked for;
- in `commitModeIndex`, the one place `modeIndex_` is assigned (the mode buttons
  and keys, a plugin preset, a bookmark from the desktop and from the browser, the
  browser's mode request, and the config restore);
- on every way out of `rescanPlugins` (the early return after the quarantine
  failed included, which is taken after every plugin has been unloaded);
- on every way out of `patchReconcile` and `patchStopAll`, where the patch page's
  radios start, are switched off or retired, and stop.

It is cheap enough for that: the version, commit, OS and architecture strings are
read once, and `setDiagContext` writes **nothing** when the block it renders is
byte-for-byte what the buffer already holds, so the buffer a fault handler may be
reading is not touched sixty times a second. The structural half of the test holds
that `modeIndex_` is assigned nowhere but `commitModeIndex`, that every
`installSource` is followed by `applyConverterForSource`, and that the frame loop
no longer refreshes one frame in sixty.

**The new line.** `patch-radios:` is `none`, or the number of radios the patch page
has running and their **driver kinds** - `1 (rtlsdr)`, `2 (rtlsdr, soapy)` - sorted,
one per running radio. Kinds only: it is built from the part of each patch node's
device key before its first vertical bar (`siggen` for the generator, `iqfile` for a
recording, whose path is never read), and the renderer keeps only the leading run of
lower-case letters and digits of that, so a serial or a path cannot reach it. A fault
on a patch radio's thread otherwise reads as a fault with no radio at all.
`crash_upload.cpp` reads a fixed list of context lines (`mode`, `source`,
`sample-rate`, `device-open`, `sdr-model`, `plugin`) and **does not carry this one**:
the upload's `context` object is the wire contract PRIVACY.md lists, a new key in it
would need the receiving end changed first (it has not been, and whether it would
tolerate an unknown key is not something this repository can show) - so the line is
in the report file on the machine and in the bundle, and not on the dashboard.

**What this does not close.** The block is rendered at the end of `applyConverterForSource`,
not before `pipeline_.setSource` starts the new source's thread, so a fault in the
first read of a new stream can still land in the few microseconds between the thread
starting and the render. The mid-render write of the fixed buffer is still not atomic
against a handler on another thread (it now happens only when the text changes). The
plugin-set hook is held structurally and by a rescan with no plugins installed, not by
loading a real plugin into the test. And whether the three field faults are fixed by
any of this is untested: the context describes them, it does not cause or cure them.

### What the bundle says about the sessions before this one (0.99.62)

A user's window froze after twenty minutes; they ended it from the taskbar,
restarted, and sent the bundle five minutes into the next session. The bundle
described a healthy five-minute session: the in-memory log ring holds the CURRENT
session only, and nothing in the bundle said whether a freeze report had been
written or uploaded. The bundle (Copy diagnostics, and the attachment of the REPORT
A BUG page) now carries two more sections after the log, each under its own heading
(`src/core/diag_history.{hpp,cpp}`, assembled by `buildDiagnosticsBundle`):

- **`--- previous session (the end of its log) ---`**: the last lines of the session
  before this one, read from the rotating log files (`foxsdr.2.log`, `foxsdr.1.log`,
  `foxsdr.log`, oldest first, concatenated). Sessions are cut at the line `main()`
  writes first, `FoxSDR <version> (<commit>) starting`, and **the newest such line is
  this session**; the session before it is the one shown. At most **80 lines and
  12 KiB**, the newest kept, under a `session: FoxSDR 0.99.59 (<commit>), N lines in
  the log files, the last M follow` line. Every line goes through
  `core::scrubUploadLog` with the report's plugin inventory, the same call the
  current log goes through. When there is none the section says why in a sentence:
  `none - the log files hold no session before this one`, `none - no session start
  line is in the log files`, `none - the log folder holds no log file`, and so on.
- **`--- reports on this machine ---`**: for the newest **ten** `crash-*.txt` and
  `hang-*.txt` files (by the time they were written), one line:
  `hang, 1 min ago, version 0.99.59, uptime 1200 s, stalled 7213 ms, signature
  7C04AAAABBBBCCCC, upload sent`, or for a crash `crash, 2 h ago, version 0.99.59,
  uptime 3600 s, access violation (0xC0000005), signature 1B2C3D4E5F607182, upload
  none`. The kind is `crash`, `hang` or `stall`; the upload word is the sidecar's
  `status:` - `sent`, `duplicate`, `local-only` (a stall, which is never sent),
  `backoff`, `rate-limited`, `failed`, `abandoned`, `too-large`, `expired`,
  `refused` - `other` for a word the uploader does not write, and `none` when there is
  no sidecar (nothing has swept the report, or sending is off). Nothing else is read:
  no stack, no log, no module list, no context block, no file name or path; a `.dmp`
  or a saved `diagnostics.txt` in the same folder is never opened. Each line passes
  through `scrubUploadLine`. This is what answers "did the watchdog fire, and did the
  report reach us". (The uploader writes ten statuses, `rate-limited` and `refused`
  among them, and all ten are carried.)

**Off means off.** With Diagnostics off, or in a run that may not touch the disk (a
bounded `--frames` run has no reports folder), the bundle adds neither section, not
even a heading, and nothing is read: the only read path is `DiagHistoryCache`, whose
`readsStarted()` stays at zero (`tests/test_diag_history.cpp`, through a real window).

**It is read off the frame loop.** The reports folder is in the same tree as the
settings folder whose synchronous `exists()` froze a window for five seconds (*The
settings-folder poll is answered off the frame loop*), and the problem-report page
rebuilds its attachment about once a second while its box is ticked. So
`DiagHistoryCache` owns one worker thread at a time and the window never reads a file
itself: the previous session is read **once**, at the start of `run()` (and when
Diagnostics is switched on), before this session's own lines can rotate it away; the
report list is read again when the answer is older than five seconds, and a person's
own click on Copy diagnostics asks for a fresh one and waits for it for at most 750 ms.
At exit a read still blocked in the filesystem is abandoned, not joined. A bundle made
before the first read has finished says `(still being read - copy the diagnostics
again in a moment)`.

**Not in the automatic upload.** `crash_upload.cpp` reads a fixed list of fields from
a report and none of this is in it; the bundle is only ever sent by the user's own
action.

**Known limits, stated.**

- The previous session is "whatever precedes the newest start line", which is this
  session's only if this session's start line is in the files. A session that started
  with Diagnostics off and had it switched on part-way through has no start line there,
  so the section says so (`none - diagnostics were switched on part-way through this
  session ...`) rather than showing the wrong session; so does a log file that is not
  being written.
- Two FoxSDR windows appending to the same log interleave their lines and their start
  lines, and the section then shows whatever the newest start line implies.
- The log files keep three files of 1 MiB; a previous session older than that is gone,
  and the section says there is none.
- Not exercised against a real freeze followed by a real restart: the tests build the
  files and reports by hand in the shapes the writers produce (`DiagLog`'s
  `HH:MM:SS.mmm level text` lines, `crash_handler.cpp` and `hang_watchdog.cpp`'s
  headers, `crash_upload.cpp`'s sidecar).

### The bandwidth line (0.99.61)

`bandwidth: N Hz in MODE (how)` — written when the receiver's width changes,
with `MODE` one of `NFM`, `WFM`, `AM`, `DSB`, `USB`, `CW`, `LSB`, `RAW` and `how`
one of `bandwidth list`, `dragged on the spectrum`, `decoder preset`, `bookmark`
or `remote request`. It is support's answer to "the bandwidth control does
nothing": three 0.99.59 users said so (an RTL-SDR Blog V4 at 2,048,000 and
2,400,000 S/s: "changing the bandwidth changes nothing", "on AM I can listen to a
station +/- 30 kHz away"), and the log could not say whether the control had ever
been touched, because a mode button wrote `mode: AM, bandwidth 10000` and a
bandwidth change wrote nothing. A pick of the step
already in force writes nothing; a drag on the spectrum writes one line when the
mouse is released, with the width it ended on, not one a frame; a preset, a
bookmark or a remote request writes one only if the width actually moved. It
never carries a frequency.

It is not exhaustive. A bookmark recalled from the browser, the default width a
decoder preset's mode sets, and the restore of the saved configuration at startup
move the width without writing it (a mode change already reports its default
width on the `mode:` line), so a missing `bandwidth:` line shows that the routes
listed above were not used, not that the width never changed.

The cause the line cannot show, and the repair: the channel filter's length was
sized from the distance between the passband edge and the channel's Nyquist
frequency, about 100 kHz at every setting, so at 2.4 MS/s a 3 kHz request got 103
taps and a 10 kHz request 107 — and a windowed sinc that short is its window,
whatever its cutoff says. Measured through the real classes
(`tests/test_channel_bandwidth.cpp`, on generated tones): the 3 kHz filter read
-0.1 dB at +5 kHz and -5.1 dB at +30 kHz, the 10 kHz filter -0.2 and -5.9, so the
control moved the result by a few tenths of a decibel. A bandwidth below a
quarter of the channel rate now gets one more stage at the channel rate whose
transition is half the bandwidth wide (flat to half the bandwidth, -6 dB at 0.75
times it, about -32 dB at the bandwidth and below -90 dB from 1.25 times it); at
or above a quarter nothing changed, tap for tap. In USB, LSB and CW the control
now sets the width of the wanted sideband (it was a fixed 3 kHz), with the
channel filter four times that wide. Two consequences are worth knowing when
reading a report: the signal meter and the squelch sit after the channel filter,
so they now measure the narrower channel and a saved squelch level may need
adjusting, and the outline drawn on the spectrum is still symmetric in the
sideband modes. This is measured on generated signals
(`tests/test_bandwidth_app.cpp` drives the real window's mode and bandwidth
calls and reads the S-meter), **not on a radio**.

### What ADD ALL PLUGINS writes (0.96.0)

The plugin store's bulk key runs N transfers through the single install path,
one after another, over as many seconds as the network takes — and the panel
that reports it is a window the user may have closed by the time it finishes.
So the run is also written to the log, and the three lines below are the whole
of it. They are `info`, because a bulk install is a thing the user asked for
and not a fault.

- **The start line**, once per run, with the shape of the plan before anything
  moves: `plugin store: add all starting - 24 to fetch, 0 to update, 0 passed
  over`. A run that fetched fewer than the catalogue holds is the normal case
  and the count says so without anybody having to work it out from what
  followed.
- **One line per module passed over**, naming it and the reason the store's
  own gate gave: `plugin store: add all - passing over Inmarsat-C / EGC
  Decoder (EXPERIMENTAL) - the legal notice must be acknowledged first`. This
  is the line that answers "why did it not install everything", which is the
  only question a bulk key reliably produces.
- **One line per failure**, with whatever refused it, word for word:
  `plugin store: add all - GOES Weather Satellites (HRIT / LRIT) FAILED:
  sha256 mismatch …`. A module that fails does not stop the rest, so without
  this line a single failure in the middle of twenty-four is invisible once
  the summary scrolls away.
- **The finish line**, once: `plugin store: add all finished - 24 installed,
  0 failed` — the same sentence the panel shows, so the log and the window
  cannot give two accounts of one run.

There is deliberately no per-module *success* line: the host already writes
`plugin: loaded <name> <version>` for every module it accepts on the rescan
that follows each install, and a second "installed" line beside it would be
two records of one event that can disagree.

### The device-enumeration reports

The "Afterwards" row above says the process dies, and for a fatal fault it
does. The device search is the exception, because it is the one place where the
alternative is that the most reproducible fault this product has ever had stays
invisible. It produces three kinds of report, all of `kind: crash` — and since
0.99.62 **one death produces one of them, not several** (see *One report per
death* below):

| Reason line begins | Raised by | Written by | Stack |
|---|---|---|---|
| `fault in a third-party SDR module, absorbed…` | a vendor driver faulting on our own calling thread | `src/source/vendor_guard.cpp`, from its `__except` **filter** — `EXCEPTION_POINTERS` are dead by the time the handler body runs | the fault's |
| `access violation` (or any ordinary fatal reason), ending ` - enumeration child, <walk>, attempt N (contained)` | the helper process faulting on **any thread**, in whatever module: in cascade's own code, which the guard deliberately refuses to absorb, **or in a vendor DLL** — a thread a driver created, or an ASIO driver a vendor module maps itself in the middle of a probe | the helper's own crash handler, installed by `armEnumerateHelperProcess` into the directory the parent passed down; since 0.99.59 the report carries `version`, `commit`, `os` and `arch`, which the helper had no way to render before; since 0.99.62 the reason ends with the walk, the driver and the attempt | the fault's |
| `SDR device enumeration child process died…` | the helper process dying by a route **its own handler never sees** — a heap corruption (`0xC0000374`), a vendor `TerminateProcess`, a death before it armed — and so one the parent can only see from outside | `src/source/soapy_enum_proc.cpp`, in the parent, with the child's exit code as `code`, **and only when the child left no report of its own for that death** | none — the stack section says the fault was in another process |

**Which driver (0.99.34).** SoapySDR runs every driver's find function at once,
on a thread each, so a whole-bus helper that dies has no single driver to
blame. The helper therefore writes a probe log to the parent as it goes
(`cascade-probe: begin <driver>` / `end <driver>`), and the parent's whole-bus
report's reason ends with the drivers **still probing when it died** — a
shortlist, not a verdict, since a heap corruption is detected at a later
allocation, possibly on another driver's thread. (Since 0.99.62 the same
begin/end lines are also written to the helper's own log ring, so the helper's
own report carries the shortlist in its log; the parent's report, which exists
only for the deaths that report cannot cover, still carries it in its reason.)
The per-driver sweep that follows two whole-bus deaths is what names a culprit:
the report of its death reads `…died probing driver=<name>` (the parent's) or
ends `…, driver=<name>, attempt 1 (contained)` (the helper's own), and that
driver is then left out of every scan for the rest of the session (the Source
panel says so in one line). The **parent's** reports hash
`enumerate-child:whole-bus` or `enumerate-child:driver=<name>` in place of the
faulting module; before 0.99.34 it hashed `"?"`, so every contained death of one
exit code was one crash group (`650B88A1735695DB` = `0xC0000005`,
`91965660116CF497` = `0xC0000374`), and the per-driver report was dropped by
the uploader's 24-hour de-duplication as a repeat of the whole-bus one. The
helper's own report hashes the faulting module and offset, as every crash
report does — see *One report per death* for why that is the right grouping.

They are filed as `kind: crash` rather than a new kind on purpose:
`src/core/crash_upload.cpp` forwards `crash` and `hang` and **refuses anything
else**, so a new spelling would be a report nobody ever receives.

**A death is reported at every death, not only when the whole scan fails.**
The common shape of the libusb fault is "first helper died, the retry
worked": on this bench, at roughly one child in forty, some forty of every
forty-one occurrences end that way. Filing only when *every* attempt died would
report about one in forty-one of them, and the other forty would exist as a
line in `foxsdr.log` — which is never uploaded on its own. The success of the
containment is exactly what would have made the fault invisible. (The report is
the helper's own when its handler ran, and the parent's third kind above when it
did not.)

**One report per death (0.99.62).** Before this, one death could leave
*three* reports: the helper's own (the stack and the module list), the parent's
whole-bus report of it (no stack; the drivers still probing) and — after a
second whole-bus death — the parent's per-driver report from the sweep (no
stack; the driver). One machine's deterministic ASIO-driver fault produced six
report files in one scan and, after the client's 24-hour de-duplication, three
uploads: three of the five a machine may send in a day, for a fault the
application survived, so that a genuine crash later the same day could be
rate-limited away; and on the dashboard it was three unrelated groups. Now:

- **The helper's report is the report, whenever the helper wrote one.** The
  parent starts the helper itself, so it knows its process id
  (`EnumResult::childPid`), and the helper's report is named with that id
  (`crash-<stamp>-<pid>-<seq>.txt`) and is on disk before the parent sees the
  exit, because the handler writes and only then terminates. The parent asks the
  crash directory (`core::crashReportWrittenByProcess`) and files its own report
  only when the answer is no. That answer is yes only for a file that is named
  for that process id **whole** (not 14242 for 4242), was written **no earlier
  than the spawn** (process ids are reused and the folder keeps reports for
  weeks), has a whole header — `kind: crash` and a sixteen-digit `signature` —
  because a file the handler created and never wrote to is not a report, and is
  **not an absorbed vendor fault**, because a helper that absorbed a fault and
  carried on has a report with its own process id too, and if it then dies by a
  route its handler never sees, that earlier file is not the report of the death.
  `vendor_guard.cpp` `static_assert`s its reason still begins with the prefix that
  distinction rests on.
- **What the helper's handler never sees keeps the parent's report exactly as it
  was:** a heap corruption (`0xC0000374`), a vendor `TerminateProcess`, a helper
  that died before it armed, an absorbed-then-unseen death. The timeout kill
  never produced a parent report — only a `ChildDied` outcome is reported — and
  still does not. With diagnostics off neither process writes a file: the helper
  is handed no directory, so it dies quietly, and the parent's writer is
  disabled.
- **The facts only the parent's reports carried are in the helper's.** The
  helper knows its own arguments, so on the healthy path, before it arms, it
  renders the tail of its `reason:` line — ` - enumeration child, whole bus,
  attempt 2 (contained)`, ` - enumeration child, driver=uhd, attempt 1
  (contained)`, or `driver list` — and hands it to its crash handler to copy out
  at fault time (`CrashHandlerConfig::reasonSuffix`: fixed storage, nothing
  formatted on the fault path). The attempt is handed down on the command line
  (`--attempt=N`), because only the parent knows it. This rides in the `reason`
  line, which is already uploaded verbatim: **no new upload field**, and nothing
  new leaves the machine. The driver is cut to 16 characters; the site keeps 200
  of a reason and the tail has to fit behind the longest reason this product
  writes (an absorbed fault's, 134) — pinned by a test against a real absorbed
  fault in a real helper.
- **The "still probing when it died" shortlist is kept in the helper's log, not
  in its reason.** It is known only as the fault happens, and the parent reads it
  off the pipe. The options were the parent appending a line to the file the
  helper wrote (a second writer on a report the next start reads, with a new way
  to leave it half-written if the parent itself dies mid-append), accepting its
  loss, or keeping the parent's report and so the second upload. Instead the
  helper writes each `cascade-probe: begin/end <driver>` line to its own log ring
  as well as to the parent, and the ring is in the report and in the uploaded
  `log`: a probe that began and never ended is the shortlist, and the stack names
  the faulting module besides. What is lost is that the shortlist is no longer on
  the dashboard's reason line for a death that has a stack.
- **The signature is the fault's, so one fault is one upload.** The helper's
  report hashes the faulting module and offset like every crash report, so the
  same fault met by two whole-bus helpers and then by the per-driver helper is
  one signature and one upload inside the 24-hour window (`decideUpload`,
  `kDedupSeconds`), where it was three. Two *different* drivers faulting fault in
  two different modules and are two signatures. A fault in **no module** (private
  memory, a jump through a freed pointer) has no module to hash and used to share
  one `"?"` signature per exception code — which, now that the parent's tagged
  report is not filed, would have made two drivers' such faults one group. The
  helper therefore hashes `enumerate-child:whole-bus`,
  `enumerate-child:driver=<name>` or `enumerate-child:driver-list` in that case
  only (`CrashHandlerConfig::unresolvedSignatureTag`), so those stay apart per
  walk and per driver.

`tests/test_soapy_enum_proc.cpp` pins all of it against the real binary and real
vendor modules (`tests/fixtures/soapy_fault_module.cpp`, whose `thread` stage
faults on a thread the find function spawns — the libusb fault's shape, and the
one fault the vendor guard cannot absorb, so the per-driver helper dies too): one
file for one death with a stack, the driver and the attempt; three deaths and
three files with one signature and one upload for the deterministic case; two
faulting drivers' four files, two signatures, two uploads; diagnostics off, no
file; and the deaths the helper's handler never sees, one stackless parent
report each. **What is not shown:** the POSIX spawn path and the POSIX crash
handler's tail and tag, which cannot be built on the machine this was written on.

The parent's report (the third row) has an `address` of `0`: the fault was in
another process and this one has no address to offer, so it groups by its tag
and code rather than by module and offset, and the useful half is the `code`. The
middle row exists so that a
fault on that path is not reduced to an exit code — the helper runs above
`installCrashHandlers` in `main()` and would otherwise have no handler at all —
and it is not limited to our own code: the handler reports whatever reaches it,
on any thread, whichever module the address is in. When diagnostics are switched
off the helper installs nothing and dies in microseconds, writing nowhere: off
means off in the child too.

**A DLL mapped after the module table was taken is now named (0.99.61).** The
field case (2026-10-01, reported on 0.99.57): SoapyAudio lists sound cards through
RtAudio, whose ASIO back end maps every ASIO driver on the machine in the middle
of its probe, and one of them — a Native Instruments driver — killed every
helper that asked. The helper refreshes the crash handler's module table once,
after the vendor modules load and before any probe runs, so a DLL a probe maps
for itself was in no table: the report's `address:` was a bare number, nine
frames were named `-`, the module list did not mention the DLL, and the report
hashed to the signature every unresolved access violation shares. The
`cascade: fatal exception 0xC0000005 at 0x00007FFF52E84E94` line the helper writes
to the parent's pipe was a bare address for the same reason. The crash handler
now asks, at fault time, which mapped image contains the faulting address and
each frame of the walked stack (`adoptModuleContaining`, called from
`writeReport` and, for the pipe line, from `enterFatal`), and adds that image to
the table: its file name, base and size. It uses `VirtualQuery` and ntdll's
`NtQueryVirtualMemory` (the entry point is resolved at install time, because
`GetProcAddress` in a handler is a call into the loader), takes no loader lock,
allocates nothing and formats nothing, and reads the module's own headers behind
a `__try`. Three limits are stated rather than hidden:

- **It names the file, not the build.** The entry has no PDB name and no build
  id, because reading the CodeView record means formatting it; a module nobody
  archived symbols for — the vendor's, which is what arrives late — needs its
  name, base and size, and gets exactly those. It lasts until the next
  `refreshModuleTable()`.
- **Only image memory is named.** Heap, a JIT page or a stack stays a bare
  address, and nothing is invented for it (the file name is `unknown-image` if
  the name query fails, with the base, the size and the offset still given).
- **Windows only.** The POSIX handler is unchanged; its table is refreshed after
  `dlopen` only.

`tests/test_crash_late_module.cpp` arms the handlers, refreshes the table, *then*
loads a DLL no table has heard of (`tests/fixtures/late_fault_dll.cpp`) and
faults inside it, and requires the DLL's name in the `address:` line, in the
stack, in the module list and in a signature that is no longer the shared
unresolved one; its negative control faults in executable private memory and
requires a bare address, no invented module, and a whole report. The same stage
is run through the real enumeration child in `tests/test_soapy_enum_proc.cpp`. **What
is not shown:** the Native Instruments driver itself, which was not reproduced —
the fixture is an ordinary DLL — and that no lock is taken, which rests on the
code's own reasoning about what `VirtualQuery` and `NtQueryVirtualMemory` read
(the address space's bookkeeping, not loader or heap state) rather than on a
test.

**A freeze report names such a DLL too (after 0.99.62).** The hang watchdog
resolved each frame of the stalled thread against the same snapshot and was never
given the crash handler's answer, so a GUI thread stuck *inside* code mapped
after the last refresh — a display driver a GPU reset reloads, a vendor DLL a
probe maps, a shell extension a dialog injects — printed those frames as bare
addresses. Two things were lost with the name. The report could not say where
the thread was. And the display-stall rule, which reads module names off the top
frames to tell a monitor being switched off from a deadlock in this program,
could not see a graphics driver the table had never heard of, so the same
freeze was filed as a **hang** rather than kept on the machine as a **stall**.
`describeModuleContaining` (`core/diag_report.cpp`) answers with the same
`VirtualQuery` / `NtQueryVirtualMemory` lookup, and it is deliberately **not**
`adoptModuleContaining`: that one appends to the table, whose single writer is the
fault path, while the watchdog thread runs beside a GUI thread that rebuilds the
table at the end of every plugin rescan, and a racing append could drop modules
the next crash report needs. The new function writes nothing shared — it fills a
local entry (file name, base, size; no PDB, no build id, as above) and the
frame line and the classification both use it, so the `kind:` the header states
and the frames under it cannot disagree. `HangWatchdog::start` resolves the one
ntdll entry point it needs, for the reason the crash handler does at install:
`GetProcAddress` from the capture would be a call into the loader on a process
that may be wedged inside it. The module list in the report is still the
snapshot's; a late module is named on the frame line (name and offset are all a
vendor DLL needs) and is not listed with its base. `tests/test_crash_late_module.cpp`
holds it with a real `HangWatchdog` and a real thread parked inside the late
fixture — the report's stalled stack must carry `late_fault_fixture.dll+0x…` — and
a second time with the fixture copied under an NVIDIA user-mode driver's file name,
where the report must come out `kind: stall` and not `kind: hang`. Windows only:
the POSIX table is refreshed after `dlopen` and nothing here changes it.

**The audio driver is no longer asked at all.** Since 0.99.59 the scan never asks
SoapySDR's `audio` driver — in the whole-bus walk, the per-driver sweep, the walk
beside an open radio or the in-process fallback — because nothing it lists is
ever offered (a sound card is the Sound card source, which has been FoxSDR's own
since 0.99.38), and the helper now ends with `TerminateProcess` rather than
`ExitProcess`, which would run every loaded module's `DllMain` with
`DLL_PROCESS_DETACH`, the place the 2026-10-01 field child died after it had
already answered. From 0.99.61 the scan says so once per scan, whichever walk
ran, in the voice of the `uhd` skip line:
`soapy: not asking audio - sound cards have their own source here, and nothing it
lists is offered`. A log tail from a machine with SoapyAudio installed used to
carry no trace of why a sound card never appeared. `SoapySource::open` now also
refuses `driver=audio` outright — a saved patch names its radio by args and
opens through that call directly, and `Device::make` would run the driver's find
function, the ASIO walk, inside the application itself — with `lastError`:
`SoapySDR's audio driver lists sound cards, not radios, and is never opened here -
use the Sound card source for a sound card.` The key and value are matched
ignoring case and surrounding spaces; `driver=audiofoo` is another driver.

### The device CONTROL-path reports (0.62.3)

Enumeration is no longer the only guarded crossing. Five reports uploaded from
the shipped 0.62.0 in 24 hours — at least two people, two RTL-SDR models, two
Windows 11 builds — grouped into three signatures that are all the same fault
family, and all of them are ordinary things to do with a radio:

| Signature | The user did | Faulted in |
|---|---|---|
| `B63B14A9BA45C175` | pressed **Play** | `SoapySource::start` → `activateStream` |
| `235E46B5D39DED8D` (×3) | **changed frequency** | `SoapySource::setCenterFrequencyHz` → `setFrequency` |
| `7496A711C2D58EE2` | **switched source** | `SoapySource::teardown` → `closeStream` |

Above our frame in every one: `SoapySDR.dll` → `rtlsdrSupport.dll` →
`rtlsdr.dll` → `libusb-1.0.dll` → `ntdll.dll`.

**These are catchable, and the B200 enumeration fault is not, for one reason:**
they are raised on cascade's *own call frame*. A structured exception is
delivered on the thread that raised it, so `__try`/`__except` around the call
sees them. The B200 fault is on a thread UHD spawns for itself, which is why
that one needed a whole child process instead.

So **every call that crosses into SoapySDR now runs inside
`callGuardingVendorFaults`** — `Device::make` and the whole device
interrogation in `open()`, `start`, `stop`, `teardown`, the sample rate, the
centre frequency, the gains, the gain mode, the antenna list/set/read, and the
module/search-path queries. The one deliberate exception is **`readStream`**:
`vendor_guard.hpp` states the limit of the trade in terms — absorbing is right
for a control call made on the user's behalf and is *not* a licence to wrap the
streaming path — and that path already has a real fault route, runs ten times a
second, and would turn one faulting stream into a storm of reports.

Each absorbed control fault produces the same `fault in a third-party SDR
module, absorbed…` report as the enumeration guard, and then:

- `SoapySource::faulted()` goes true and `lastError()` carries the code, so the
  Source panel shows **“Device stopped: …”** — the path a user already gets for
  an unplugged radio — instead of the application vanishing;
- the failing call returns **false**, and no cached readback is advanced: a
  retune that faulted leaves `centerFrequencyHz()` on the last value the
  hardware actually confirmed, so the readout never advertises a frequency the
  radio is not on;
- **the device is marked dead and is never called again.** `teardown()` then
  releases it *without* `closeStream` or `unmake`. That leaks the handle and
  the USB claim for the life of the process — the message says to restart —
  and it is the deliberate choice, because a module that has just raised an
  access violation may still hold its own locks, and a second call into it can
  **block forever**. The guard can absorb a second fault; it cannot absorb a
  hang, and a hung GUI thread is worse than a crash.

**Two lines added in 0.90.1**, from the first absorbed report the 0.89.0
logging caught whole (2026-09-09, a NESDR SMArt v5 on Windows 10.0.26200,
grouped `crash ntdll.dll @ SoapySource::setSampleRateHz`, uptime 176 s). The
radio had streamed at 2.4 MS/s for two minutes; then the tester opened the
Source section for the first time that session, which ran the device scan
(UHD's banner in the `vendor:` lines is the child process loading every
module), and twelve seconds later the next rate change died in
`RtlEnterCriticalSection` on a lock libusb had already freed. SoapyRTLSDR's
find routine probes every dongle with `rtlsdr_open` + `rtlsdr_close` — a full
demodulator reset — and the in-process gate (`enumerateInProcess`, 0.62.1)
never saw it because the scan runs in a child.

- `soapy: device scan deferred while <device> is open - the vendor probe opens
  and resets every dongle it finds (close the radio to look for other devices)`
  — written once per open radio by `AppWindow::scanSoapy()` when the combo's
  lazy first scan, Refresh or the web interface's `scanDevices` asks for a scan
  with a device open (or resolving, or abandoned by the dead-device policy and
  so still held by its module). The Refresh key is disabled with the same
  sentence; the decision is `gui::deviceScanAllowed`.
- `source: the driver faulted while <what>; reopening <device> at <rate> S/s`
  — written by `AppWindow::pollSoapyRecovery()` when the open device is dead
  by an **absorbed fault** (`SoapySource::deadReason() == VendorFault`; a
  wedged driver, `Abandoned`, still has a thread of ours inside it and is
  never reopened) and no attempt was made in the last 60 s
  (`gui::autoReopenDue`). It is followed by either `source: reopened <device>
  at <rate> S/s after the driver fault; receiver restarted` or `source: the
  reopen of <device> failed (<reason>) - restart FoxSDR to use this radio
  again`. The reopen reaches the driver's own object: SoapySDR's factory hands
  back a device still in its table for the same args (`lib/Factory.cpp`), so
  no vendor find runs and nothing on the bus is probed.

A hang report captures every thread because *a deadlock is only legible as a
pair.* "The GUI thread is blocked" names no bug; "the GUI thread waits on the
CAT server's mutex while the CAT thread waits inside a socket close" is the bug,
and it is only visible if both stacks are in the same file.

## The 5-second threshold, and why it is not a guess

The GUI paces itself off vsync, so the normal interval between heartbeats is one
display frame — 16.7 ms at 60 Hz. Five seconds is 300 consecutive missed frames.
The real hangs above were 120 s and permanent.

That number is **measured, not asserted**. `cascade --frames N` prints the worst
heartbeat gap it observed, and `tests/test_diag_hang.cpp` runs exactly that and
requires the measured worst gap to be under **half** the threshold. If a future
change makes a frame legitimately slow, that test goes red before a user ever
gets a false report.

Several separate things would otherwise cry wolf, and each is suppressed for its
own reason. Two of the excuses — a pause the application takes, and a stall inside
`win32u.dll` — are **time-limited** (30 s, see below); the others are not:

1. **A debugger at a breakpoint.** `IsDebuggerPresent()` — a break is not a hang.
2. **A nested Windows modal loop.** Dragging or resizing the window, or holding a
   system menu open, stops the application's own loop turning over at all, and
   can legitimately last minutes. `GetGUIThreadInfo` reports exactly this
   (`GUI_INMOVESIZE` / `GUI_INMENUMODE` / `GUI_POPUPMENUMODE`). The flags are
   exact — a person is holding the window — so this excuse has no limit.
2c. **A stalled thread whose instruction pointer is inside `win32u.dll`.** Some
   nested loops set none of those flags (holding a caption button, or resting
   the pointer on the maximise button until Windows 11 offers its snap layouts,
   parks the thread in the window manager's own message wait), and the
   application's frame loop only ever calls the non-blocking `PeekMessage`, so a
   thread parked in `win32u` was taken to be in a loop Windows owns. That premise
   is true of message waits and false of the rest of `win32u.dll`, which is the
   user-mode end of every win32k and graphics-kernel system call: a GPU
   driver's present and fence waits, a `SendMessage` to another thread's window
   that nobody answers, a clipboard owner that never replies. Until 0.99.61 the
   excuse lasted as long as the thread stayed there, so a real freeze in any of
   those was excused for ever and never reported. It is now granted for at most
   `HangWatchdog::kExcuseCapMs` = **30 s**, counted from the first poll that
   granted it in that stall; past it the ordinary threshold applies and the
   report follows about five seconds later, with the frames that show where the
   thread really is.
2b. **Blocking work the application enters knowingly** — a watchdog pause. Five
   call sites take one today, and they are named rather than left as a general
   principle, because a mitigation with no call sites protects nothing. There
   are **two kinds**, by who sets the pace of what is waited for:
   - *Application-paced* (`HangWatchdog::pause`/`resume`, `WatchdogPause`): code
     that is expected to finish. `AppWindow::rescanPlugins()`, which unloads and
     re-`LoadLibrary`s every installed plugin on the GUI thread; `gui::AudioOpen`'s
     bounded wait for an output device, and the same for the microphone
     (`micOpen_`); and the display-change and not-being-shown pause in
     `gui/present_grace.hpp`. The audio one is the shape this rule always
     anticipated — "a synchronous device open" — and it is bracketed AND bounded,
     because a pause held for an open that never returns would be the watchdog
     switched off. Since 0.99.61 **each continuous pause excuses a stall for at
     most 30 s**: the clock starts when the pause count goes from 0 to 1, and a
     pause that is released and taken again gets a fresh one. The reason is that
     the code that ends such a pause runs on the GUI thread, so a freeze inside it
     is a freeze in the thing that would have released it — the plugin rescan
     holds one across every plugin's `destroy()`, which for a plugin with a worker
     thread is a join, and the "bounded" ten-second display grace is measured by
     the very frame loop that has stopped. The field report that prompted the cap
     is the shape this closes (0.99.59, Radar Sweep running: the application
     stopped responding, was closed from the taskbar, and nothing from that
     session reached the crash store); which excuse, if any, was in force in that
     session is **not established**. The cap bounds what is
     *excused*, never what is *beating*: a window minimised for an hour whose
     loop keeps turning is still never reported. A pause that is still counted
     past its cap is not leaked or double-released; it simply stops excusing.
   - *User-paced* (`pauseUser`/`resumeUser`): a **person** sets the pace, and
     there is exactly one such place — the shell-open bracket
     (`AppWindow::watchdogShellHooks`, through `gui::runShellOpen`), where the
     wait is somebody reading an elevation or SmartScreen prompt. It is **not
     capped**: a cap would file again the false report this bracket was built to
     stop (`hang ntdll.dll @ AppWindow::launchInstaller`, below) for any prompt
     left open past it. While one is held a stall is excused whatever
     application-paced pauses are held with it; when the last is released, an
     application-paced pause still held starts a fresh 30 s from that moment, so
     a rescan nested inside a long shell call is not reported the instant the
     prompt closes. **What this leaves unreported, by design:** a `ShellExecute`
     that never returns and shows no dialog is indistinguishable from a prompt
     nobody has answered yet, and the only watch on it is the person who sees
     the window not responding.

   `cascade --frames N` prints how many pauses the run took (both kinds count)
   and `tests/test_diag_hang.cpp` requires at least one, so a rescan that stops
   pausing goes red instead of silently arming a false report.
   `tests/test_excuse_cap.cpp` holds the cap itself — with a real `HangWatchdog`
   on an 800 ms threshold and a 1.5 s cap so the suite is seconds long — against
   a stall inside the real display grace, a plain pause, a pause shorter than the
   cap (still excused), a long pause with a beating GUI thread (never reported),
   a pause released and retaken, the `win32u` excuse with the module injected, a
   user-paced pause far past the cap (not reported, and still counted), an
   application-paced pause nested inside a user-paced one, and the stall that
   continues after the user-paced pause is released; and, by scanning `src/`,
   that the uncapped kind is reached from the shell-open bracket and nowhere
   else. On Windows it also holds a real deadlock — a thread that has sent a
   message to a window whose owner never pumps, read by the watchdog's real
   register peek — to being reported. The 30 s figure is a judgement, not a
   measurement: it covers a plugin rescan off a cold disk many times over, and a
   capped stall that outlives it was not a short blocking call.
3. **The whole machine stopping.** Sleep, hibernate, or a paused VM freezes the
   watchdog thread too. If its own poll overshot by more than the threshold, it
   cannot tell the two apart, so it re-arms and says nothing.

The watchdog is stopped **last**, after the GL teardown — so a shutdown that
wedges is reported like any other hang. That is not an oversight: the worst
freeze this product ever shipped was inside a shutdown path.

### Where the clean-exit marker is written (0.99.61)

`telemetryCleanExit` is the one fact that makes the next start say "the last run
did not end normally": it is the crash counter, the trigger for offering a
report, and the `last-run-unclean` line of the bundle. It is `false` on disk for
the whole life of the process and is rewritten to `true` late in
`AppWindow::run`, so the question is only how late. The order of the steps that
matter, as the code now has it:

1. `pipeline_.stop()` — the DSP threads, the CAT server, the USB device stack.
2. The final-state save is queued and the crash upload's in-flight request is
   cancelled.
3. `patchStopAll()` and `detachAndUnloadPlugins()` — every plugin's `destroy()`.
4. **The marker**, and the bounded wait for its save (`config: final save
   abandoned - the disk did not answer within N ms` if the disk does not answer).
5. The GL and GLFW teardown, then `watchdog_.stop()`.

Through 0.99.60 the marker sat between steps 1 and 3. A `destroy()` is
third-party code — a worker join, a vendor close, a flush — and one that never
returns leaves the window on screen and not responding. The user ends it from
the taskbar (End task is `TerminateProcess`; nothing runs), and the file on disk,
written before the stage began, already said "clean". The field report, 0.99.59 with
Radar Sweep running: killed from the taskbar, the next start recorded no crash,
and the bundle read `last-run-unclean: no` and `crashes: 0` over 79 launches.
`tests/test_clean_exit_marker.cpp` (Windows only; it SKIPs by name elsewhere)
starts the real binary with a hook, `CASCADE_DIAG_UNLOAD_STALL_MS` — bounded
`--frames` runs only — that wedges the process at the end of the plugin unload,
waits for the line that proves it is inside that stage, reads the config as the
next start would, then `TerminateProcess`es it: the file must still say `false`.
A control run lets the same stall finish and requires `true`, so a change that
simply stopped writing the marker fails as well.

The marker still precedes the GL and GLFW teardown, deliberately, and **a death
in step 5 still counts as a clean exit**: the code says so in as many words and
leaves it to the watchdog, stopped last as ever, to cover that stretch. (The
marker is written by rewriting the snapshot the final-state save queued, with one
field changed, rather than by deriving the configuration again at a moment when
the session is half torn down — a second derivation would read a zero session
length once GLFW is terminated.) What this leaves unproven is a real plugin's
`destroy()` wedging: the test stands a sleep in for it.

## What a fault handler is allowed to do

A handler that allocates, takes a lock, or calls back into normal logging can
deadlock or double-fault a process that has already failed once. So the crash
path:

- **allocates nothing** — no `new`, no `malloc`, no `std::string`, no iostream;
- **takes none of this application's locks** — in particular the log ring's
  crash-path reader (`DiagLog::copyRingRaw`) deliberately does *not* take the
  log mutex, because a crash can happen while another thread holds it and a
  handler that blocked there would turn a crash into a hang with no report at
  all. The bounded cost is one possibly torn line, and it is the newest one;
- **calls no CRT formatting** — `snprintf` can take a locale lock, and a locale
  lock is a lock. Integers are rendered by hand;
- **writes with `CreateFileA`/`WriteFile`**, not buffered CRT streams that would
  need a flush the process may not live long enough to perform;
- keeps its large buffers (the 48 KiB ring copy, the `CONTEXT` it unwinds)
  **static, not on the stack**, because the fault it most needs to survive is a
  stack overflow, where roughly one page of stack is left.

Everything the fault path needs is prepared while the process is healthy: the
directory is created at install time, the module table is snapshotted in
advance, the context block is pre-rendered. The one thing a snapshot cannot
know is a module mapped *after* it was taken, and refreshing it from a handler
means the loader lock, so on Windows (since 0.99.61) the fault path names such a
module itself: the faulting address and each walked frame are looked up with
`VirtualQuery` and `NtQueryVirtualMemory` — reads of the address space's own
bookkeeping, resolved at install time because `GetProcAddress` is a loader call
— and an image found there is appended to the table with its file name, base and
size, using static scratch and no allocation, lock or CRT formatting. Anything
that is not mapped image memory stays a bare address. The POSIX handler is
unchanged and still has only what the last refresh saw. See *The
device-enumeration reports*.

**The one honest caveat.** Unwinding on x64 means asking ntdll where a
function's unwind data is, and that reads loader data under a lock another
thread could hold. There is no unwind without it. So the report is written
**incrementally**, most valuable first: the fault kind, the faulting address as
`module+offset`, and the application context all reach the disk *before* the
walk is attempted. If the walk ever did deadlock, the file already on disk still
names the bug.

**The hang watchdog obeys the same caveat, and one more.** It runs in a process
that has not faulted, so it may allocate — but it walks *other* threads, and the
lock above is the one `LoadLibrary` holds exclusively while inserting a module.
This application calls `LoadLibrary` from the GUI thread (a plugin rescan) and
from a worker (the Soapy enumerate), so unwinding a thread *while it is
suspended* could block forever with that thread never resumed: the diagnostic
becomes a permanent hang. It therefore captures in two phases — the suspend
window contains `GetThreadContext` and nothing else, no user-mode lock and no
allocation, and every thread is running again before a single frame is unwound.
The price is that a thread which really is running can move under its own walk
and produce a garbled tail; frame 0 comes from the register and is exact, and
the threads that matter in a hang are not going anywhere. The identifying half
of the report is written and flushed before phase 2 starts, and
`tests/test_diag_hang.cpp` stops a capture there and reads the file back.

`__fastfail` (the `/GS` stack-cookie failure, `0xC0000409`) transfers straight
to the kernel and no user-mode handler runs. Those appear as an unclean exit
with no report — which `telemetryCleanExit` still counts, so they are visible as
a number even when they are invisible as a report.

### A finding worth keeping

MSVC's `set_terminate` installs a **per-thread** handler. The one installed on
the main thread does not apply to a thread this application never created —
which is precisely the case the terminate registration exists for, a vendor SDR
driver throwing out of its own stream-read thread. Measured, not assumed: with
only `set_terminate` installed, an exception escaping a `std::thread` killed the
process with `0xC0000409` and left **no report at all**. `abort()` is where all
of those paths converge on whatever thread they happen on, and the UCRT raises
`SIGABRT` before it fast-fails — so a `SIGABRT` handler is the net underneath,
and it is load-bearing rather than belt-and-braces.

## Symbols — where the archive lives, and why that was a decision

A captured stack is `module+offset`. Turning an offset back into a function and
a line needs the PDB produced by **that link** — not a rebuild of the same
source, not the same version number built on another machine. PDBs are not
shipped to users, so if the one matching a shipped binary is not kept at build
time it does not exist anywhere afterwards, and every report ever filed against
that build is unreadable hex forever. Nothing else in this feature matters if
that step is missing.

**The key is the PE build id, never the file name or the version.** The linker
stamps a CodeView `RSDS` record into every PE: a GUID plus an age counter,
regenerated on every link and recorded identically in the binary and in its PDB.
"0.61.0" is the release, the nightly heading towards it, and every rebuild in
between; only the build id tells them apart.

**The archive is not in git.** `symbols/` is gitignored. A PDB is tens of
megabytes of binary per link and committing one per build would make the
repository unusable inside a fortnight.

**It is also not local-only.** Local-only means one disk failure permanently
destroys the ability to read every crash report ever filed against every build
already in users' hands. That is not a risk worth accepting for a product that
is sold. So there are two copies, with different jobs:

| Copy | Written by | When | Job |
|---|---|---|---|
| `symbols\` in the working tree | `tools/archive-symbols.ps1` from a CMake `POST_BUILD` step | **every** build | offline, instant, impossible to forget because it *is* part of the build |
| `nas:/volume1/foxsdr-symbols` | `tools/build-nightly.ps1` | in the **same step** that compiles the installer | the durable copy, on the box that already holds this project's git mirror, over the same SSH alias and key |

Only builds that can reach a user are mirrored, and they are mirrored
automatically — a network copy on every incremental relink would be unbearable,
and a manual "remember to upload the symbols" step would eventually be skipped.
A mirror failure is **fatal** to the nightly; `-SkipSymbolMirror` exists for a
deliberately offline build and prints exactly what is being risked.

**`symbols\` grows, and that is expected.** Every relink adds a PDB — around
23 MB for `cascade.exe` at the time of writing — because a rebuild produces a
new build id and therefore a new archive entry. It is gitignored, so it costs
disk and nothing else. Prune it by hand when it gets uncomfortable, but **only**
entries older than the newest build that has been mirrored: anything shippable
that has *not* reached the NAS exists in one place. `symbols\index.txt` lists
every entry with its date, version and commit, which is how to tell them apart.

**A manual `ISCC` run bypasses the mirror.** The mirror is wired into
`tools/build-nightly.ps1`, so an installer compiled by hand from
`installer/cascade.iss` produces a shippable binary whose symbols were archived
locally and nowhere else. If you build an installer by hand, run the mirror step
yourself, or build through the script.

Both copies use the standard symbol-server layout, so the archive can be handed
to WinDbg or `dotnet-symbol` as a symbol path with no conversion step:

```
<archive>\<pdb name>\<GUID><AGE>\<pdb name>
<archive>\<exe name>\<TIMESTAMP><SIZEOFIMAGE>\<exe name>
```

`tests/test_diagnostics.cpp` asserts that **this** build's PDB really is in the
archive under the build id a report would quote, and that the build id read from
the running image is byte-identical to the one read from the file on disk — a
mismatch there would index the archive by something no report ever mentions,
while every individual piece still looked correct.

### Plugin PDBs are not in this archive, and that is the module class that matters most

**Read this before hunting for a plugin's symbols.** The `POST_BUILD` step is
attached to the `cascade` target, and no plugin is built in this tree — they
live in the separate plugin repository — so `symbols\` contains
`cascade.exe\` and `cascade.pdb\` and nothing else. Every plugin build id a
report quotes will be missing from it.

That is the wrong way round, and it is worth being blunt about why: plugins are
third-party code running in-process, "which plugin was loaded" has already been
the answer to real faults in this product, and a report's `--- modules ---`
block dutifully records a build id for each one. Left as it is, the modules
**most** likely to be the faulting ones are the ones that can never be
symbolised.

`tools/archive-symbols.ps1` is not specific to the application: `-Binary` takes
any PE, the build id comes out of that file's own CodeView record, and the
layout is the same symbol-server one. So the plugin repository archives into
**this** archive root from its own `POST_BUILD` step:

```cmake
add_custom_command(TARGET <plugin> POST_BUILD
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File
            "<path to cascade>/tools/archive-symbols.ps1"
            -Binary "$<TARGET_FILE:<plugin>>"
            -ArchiveRoot "<path to cascade>/symbols"
            -Version "<plugin version>"
            -Commit "<plugin repo's short SHA, -dirty if modified>"
    VERBATIM)
```

`-Commit` and not `-CommitHeader`: the plugin repository has its own `HEAD`, and
stamping a plugin with the application's commit is the same "sends the reader to
a tree that exists and is wrong" failure the dirty marker below exists to
prevent. The index row names the DLL, so two repositories sharing one archive
stay distinguishable. `tests/test_diagnostics.cpp` runs the script against a
module that is **not** `cascade.exe` into a scratch archive root, so this path
is exercised rather than merely described.

Until a plugin's build has that step, say so in the report triage rather than
searching: the symbols do not exist anywhere, and no amount of looking in
`symbols\` will produce them.

Release builds are compiled with `/Zi` and linked with `/DEBUG /OPT:REF
/OPT:ICF`. `/Zi` is a debug-*information* switch, not an optimisation switch:
the generated code is byte-for-byte what `/O2` produced without it.

### Linux symbols: what the archive holds, and where it comes from

A Linux frame is `module offset build-id`, and what it can be turned into
depends on what is in `symbols/cascade.debug/<build id>/cascade.debug`. For a
long time that was assumed, from the file's size alone, to be too small to hold
line tables. It was then opened.

**What it held, measured (October 2026).** The 0.99.59 file
(`ec687d3420afc88116b06e356f0311ed13cda489`, 1,463,664 bytes) has 34 sections
and none of them is `.debug_*`: it is a symbol table (`.symtab`, `.strtab`) with
every code section marked NOBITS, and nothing else. A search of all 60
`cascade.debug` files in the mirror for `.debug_`/`.zdebug_` section names found
none; the larger ones (up to 10 MB, built by GCC 15.2 where 0.99.59 was built by
GCC 13.3) show the same section list with different padding, not DWARF. The
plugins' `.so.debug` files are the opposite: all 29 contain `.debug_*` section
names, and the two opened carry `.debug_info` and `.debug_line`, because the
plugin repository compiles with `-g`. So a Linux frame in the application could be
named — "52 bytes before the end of `_glfwTerminateWayland`" — and never placed
on a line, where the same frame on Windows resolves to a file and line from the
PDB.

**The step that lost it was the compiler.** CMake's Release flags for GCC and
Clang are `-O3 -DNDEBUG`; the application's `CMakeLists.txt` added `/Zi` for
Windows and nothing for Linux, so the binary never had DWARF to keep.
`tools/archive-symbols-linux.sh` split whatever the binary carried, and its
header said a normal binary "already carries its DWARF debug info inline",
which was true of nothing it had been run on. Nothing stripped the binary
afterwards either: the CI tarball and the AppImage copy `build/cascade` as
linked.

**What happens now.**

- The Release configuration on Linux compiles C and C++ with `-g`
  (`CMakeLists.txt`). `-g` is a debug-information switch and does not change the
  code: GCC's manual says `-g0` negates `-g` and provides `-fcompare-debug` to
  check that debug information changes nothing, and 12 of this repository's own
  translation units compiled at `-O3` with and without `-g` (GCC 16, x86-64)
  had byte-identical code and data sections. (`-fcompare-debug` itself flagged
  one of four files tried, `src/dsp/rds.cpp`, although that file's emitted
  sections were identical either way. Only GCC was measured; Clang is not used
  for a shipped build.)
- After the link, `tools/archive-symbols-linux.sh` runs
  `objcopy --only-keep-debug --compress-debug-sections=zlib` to write
  `cascade.debug` (plain, with a warning, if that `objcopy` refuses the
  option), then `objcopy --strip-debug --add-gnu-debuglink=<that file>` on the
  binary in the build tree, and archives that stripped binary as
  `cascade/<build id>/cascade`. The order is the one binutils documents, and
  the strip comes only after the split file exists. The build id is in a note
  section neither step touches, so it is the same before and after. A `Debug`
  or `RelWithDebInfo` build keeps its DWARF in the binary.
- Only DWARF is removed (`--strip-debug`, not `--strip-all`): the function
  symbol table stays, and `symmap.json.gz` is unchanged — regenerated from a
  real 0.99.59 binary before and after the strip it is identical, and the
  website's format is untouched.
- The test executables (`tests/CMakeLists.txt`) compile with `-g0` and link with
  `--strip-debug`. They are not shipped, there are 265 of them, and one
  translation unit that includes nlohmann::json grew from 0.4 MB to 6.4 MB of
  object with `-g`.
- The script prints `(line tables: yes)` or `(line tables: NO)` and warns when
  the archived file has no `.debug_line`; it never fails the build, like the
  rest of it.

**Size.** Nothing has been built with this change at the time of writing, so
there is no measured figure. The plugin builds give the ratio — a plugin's
`.so.debug` is 3 to 13 times its shipped module, and zlib halved the small one
tried — and the PDB for the same program is about 44 MB; expect tens of
megabytes per Linux build, two builds (x86-64 and arm64) per release. Replace
this paragraph with the real number from the first CI artifact.

**Where the Linux release symbols come from.** The release notes name the
files `foxsdr-<version>-linux-x64.tar.gz` and `FoxSDR-<version>-x86_64.AppImage`
(and the arm64 pair), which are exactly what the CI workflow produces, so the
released Linux files are taken to be CI's, built on a runner that is thrown
away. Before this change the workflow uploaded those files and nothing else, so the
`symbols/` the build wrote on the runner was lost, and `tools/build-nightly.ps1`
mirrors Windows PDBs only. Any Linux entry in the mirror was therefore copied
there by hand from a build made elsewhere. A build id in a user's report finds
its symbols only if the build the user was given is the build that was archived,
and that must be checked rather than assumed:

    readelf -n cascade | grep "Build ID"     # on the cascade inside the released tarball

and compare it with the directory name under `cascade.debug/` in the mirror.

Each CI job now uploads its `symbols/` as an artifact —
`foxsdr-linux-x64-symbols` and `foxsdr-linux-arm64-symbols` — and then checks
it (below). Mirroring is still by hand, and it must use the artifacts of the
**same run** that produced the release files: a rebuild of the same commit is
not guaranteed to produce the same build id. Artifacts expire, so do it when the
release is made.

1. Download both symbol artifacts and unzip them.
2. Copy `cascade.debug/<build id>/` and `cascade/<build id>/` from each into the
   same-named directories of the symbol mirror. Entries are keyed by build id,
   so the two architectures and every earlier build sit side by side.
3. Append the artifact's `index.txt` rows that the mirror's `index.txt` does not
   already hold. Each row is terminated `\r\n` and the file has no byte-order
   mark, as the script writes it; do not replace the mirror's file with the
   artifact's.
4. Check: `readelf -S -W <mirror>/cascade.debug/<build id>/cascade.debug`
   lists `.debug_line`.

**What the first CI run must show.** The "Check the symbol archive" step at the
end of each Linux job prints the archived file's `.debug_info`/`.debug_line`
sections, fails if `.debug_line` is missing, fails if the shipped
`build/cascade` still has `.debug_info`, and resolves `main` through
`addr2line -f -C -e` to a line in `main.cpp`. The same three can be run by hand
against any archived file:

    readelf -S -W symbols/cascade.debug/<id>/cascade.debug | grep debug_
    addr2line -f -C -i -e symbols/cascade.debug/<id>/cascade.debug 0x<offset>
    readelf -S -W <shipped cascade> | grep -c debug_       # 0

`addr2line` reads the compressed sections (the binutils 2.46 build on the
development machine did). The CI image's own binutils is the one that matters
for the archive step: Ubuntu 24.04 ships binutils 2.42, the arm64 job runs on
`ubuntu-24.04-arm`, and the 0.99.59 archive's compiler string is Ubuntu 24.04's
GCC 13.3 — and the CI step above is what proves it, rather than this sentence.

**Reading a Linux report.** `foxsdr-reports --archive symbols\` resolves ELF
frames (a 40-digit lowercase build id) with `addr2line`, found through
`FOXSDR_ADDR2LINE`, then `addr2line` on `PATH`, then `wsl addr2line`. A
Windows machine with MSYS2's binutils works: set `FOXSDR_ADDR2LINE` to its
`addr2line.exe` if it is not on `PATH`. A frame prints one of two ways:

    cascade!<function>  <file>:<line>
    cascade!<function>  (function only: no source line in the archived symbols)

The second means the archived file names the function and has no line for that
offset: a build made before the Linux build compiled with `-g`, or an address
with no line entry (a PLT stub, a compiler thunk). `-i` on a hand-run
`addr2line` also lists the functions inlined at that address. With no usable
`addr2line` the reader says so and names the archived file, and never prints a
half-answer.

**Frames in system libraries.** glibc, libwayland-client, Mesa, libstdc++ and
the rest can never be in our archive. They do not need to be: the report's
`--- modules ---` block carries a GNU build id for every module the process had
loaded, system libraries included (`diag_report.cpp` walks them with
`dl_iterate_phdr`), and a distribution's debuginfod server indexes its debug
packages by exactly that id. As a manual step for the person reading a report —
nothing in FoxSDR, and not `foxsdr-reports`, contacts these servers:

    export DEBUGINFOD_URLS="https://debuginfod.elfutils.org/"
    debuginfod-find debuginfo <build id of the module, from the report>
    addr2line -f -C -e <the path debuginfod-find printed> 0x<the frame's offset>

`debuginfod-find` (elfutils) saves the file into a local cache and prints its
path. Without elfutils the same file is
`curl -o module.debug https://debuginfod.elfutils.org/buildid/<build id>/debuginfo`.
`https://debuginfod.elfutils.org/` federates selected servers; the
distributions run their own, to be chosen from the report's `systems:` line:
`https://debuginfod.ubuntu.com/`, `https://debuginfod.debian.net/`,
`https://debuginfod.fedoraproject.org/`, `https://debuginfod.archlinux.org/` and
`https://debuginfod.opensuse.org/` (`DEBUGINFOD_URLS` takes several,
space-separated). A build id no server knows is a library version that
distribution no longer serves, or another distribution; there is nothing to fix
on our side. The request sends a build id, which identifies a library and not a
user, to a third party — which is why it is a person's decision and not a
feature.

## Reading a report

1. Take the `build=` value for the faulting module out of the `--- modules ---`
   block.
2. Find it under `symbols\<pdb>\<build id>\<pdb>`, or on the NAS mirror, or grep
   `symbols\index.txt` for it to learn which version and commit it belongs to.
   **If the faulting module is a plugin, it will not be there unless the plugin
   repository archived it — see the section above.**
3. Point a debugger at the archive as a symbol path and resolve the
   `module+offset` frames — or, for reports that came in over the network, run
   `foxsdr-reports --archive symbols\`, which does exactly this for a whole feed
   at once and groups it by signature. See *Phase 2* below. A Linux report's
   build id is the 40-digit lowercase GNU build id and its file is
   `symbols/cascade.debug/<build id>/cascade.debug`; *Linux symbols* above
   covers what it can and cannot place on a line, and how to read a frame that
   is in a system library.
4. `commit:` in the report names the tree to check out — exactly, unless it ends
   in `-dirty`, which says the build was made from a tree with uncommitted
   changes and that commit is only the nearest one.

That last line is only worth having if it is **this** build's commit. It is
generated by `cmake/git-commit.cmake`, which runs on **every build** and
rewrites the header only when the SHA changes. It used to be an
`execute_process` at configure time, which is silently wrong: CMake
re-configures when `CMakeLists.txt` changes, not when `HEAD` moves, so every
build after the next commit stamped the *previous* one — and a wrong SHA is
worse than "unknown", because it sends the reader to a tree that exists and is
not the one the offsets came from. (`symbols\index.txt` still shows the
evidence: nine different build ids all stamped `5ba13f6d0c86`.) The build id in
the modules block never had this problem, so the archive stayed correct
throughout; only the "check out this tree" line was wrong.
`tests/test_diagnostics.cpp` holds `gitCommit()` to what `git rev-parse` says
`HEAD` is right now, and runs the generator twice across a commit in a scratch
repository to prove it re-reads.

**And a fresh SHA still is not the tree.** Measured on this project: the built
binary reported `5ba13f6d0c86` while `git status` listed 32 modified or
untracked entries, the whole diagnostics feature among them — so following step
4 above would have produced a tree that does **not** contain the code those
offsets came from. Same failure as a stale SHA, and worse than "unknown" for the
same reason. A build from a modified tree is therefore stamped
`<sha>-dirty`, and the marker is on the **commit**, not the version:
`tools/build-nightly.ps1` appends `.dirty` to the version string, but an
installer compiled by hand from `installer/cascade.iss` — which
`installer/README-installer.md` explicitly contemplates — never goes near that
script and was covered by nothing.

"Dirty" is `git status --porcelain`, so an **untracked** source file counts. It
is exactly as absent from the checked-out tree as a modified one, and in the
measurement above most of the feature was untracked; `.gitignore` still applies,
so build outputs and `symbols\` do not mark a tree dirty. Both arms are
asserted: `tests/test_diagnostics.cpp` derives the expected string from the
tree's real state, so the marker appearing when the tree is clean fails just as
loudly as it missing when the tree is not, and drives a scratch repository
through clean → untracked → committed-then-edited.

`symbols\index.txt` carries the same string, because the archiver reads the
commit out of the header the generator writes rather than from a CMake variable.
It is written **without a BOM**: `Add-Content -Encoding utf8` on PowerShell 5.1
stamps one when it creates the file, which put `EF BB BF` in front of the first
row's date and made every anchored parse (`grep "^2026"`, a `^\d{4}` regex, a
split on the first tab) silently skip the oldest entry — the one row that can
never be re-derived from a later build.

## Where the user finds things

- **Log:** `%LOCALAPPDATA%\FoxSDR\logs\foxsdr.log`, rotating at 1 MiB, three
  files kept — `foxsdr.log`, `foxsdr.1.log`, `foxsdr.2.log`, so at most ~3 MiB
  and never a `foxsdr.3.log`. The count is asserted after three rotations in
  `tests/test_diagnostics.cpp`; it used to keep four, which nothing noticed
  because the first two rotations look identical either way. Since 0.89.0 it
  also carries the radio driver's messages (`soapy:` lines) and whatever the
  driver's libraries printed to stderr (`vendor:` lines) — see *What the log
  carries since 0.89.0* above.
- **Reports:** `%LOCALAPPDATA%\FoxSDR\crashes\`.
- **Settings → Diagnostics** shows both paths, has the on/off switch and the
  minidump switch, and has **Copy diagnostics** — one click that puts the whole
  bundle on the clipboard *and* saves it as `crashes\diagnostics.txt`, because a
  clipboard does not survive the next copy and a support thread can take days.
  Since 0.99.62 the bundle also carries the end of the previous session's log and
  one line per crash or freeze report on the machine, under their own headings after
  the log - *What the bundle says about the sessions before this one*, above.
- After a run that did not exit cleanly (detected by the same
  `telemetryCleanExit` marker the crash counter already uses), the next start
  offers the same thing in a dialog. A crash handler can write a report but it
  cannot ask anything — by the time it runs there is no user interface left.
  What "cleanly" covers is set by where the marker is written, and since 0.99.61
  that is *after the plugins have been unloaded* — see *Where the clean-exit
  marker is written* below.
- **SDRplay diagnostic (0.99.50):** Settings → Diagnostics → **Run SDRplay
  diagnostic**, or `cascade --sdrplay-probe <file>
  [--sdrplay-probe-bias-tee]`. A child process drives the RSP straight
  through the SDRplay API table (`src/source/sdrplay_probe.cpp`): open and
  device list, every offered sample rate (3 s each, delivered against set),
  frequency steps, every LNA state, the antennas and HDR, the bias tee (ON
  only after a second confirmation), the six low-IF modes the specification
  lists, then Uninit and close. Every `sdrplay_api_*` call is logged with its
  UTC entry and exit time and waited for up to 30 s — SLOW over 1000 ms (the
  receiver's own give-up point), HUNG at 30 s, after which no further call is
  made. The file lands in `crashes\sdrplay-diagnostic-<date>-<time>.txt`,
  carries a hash of the serial and never the serial, and can be attached on
  the REPORT A BUG page inside the existing `diagnostics` field. The GUI never
  waits for the child, in a frame or at shutdown.

Nothing on that list is re-derived. The version, commit, plugin list with
versions, source and device state all come from state the application already
holds.

## Turning it off

`Settings → Diagnostics`. Off means off: no directory, no log file, no report —
not an empty folder, not a zero-byte file. `tests/test_diagnostics.cpp` and
`tests/test_crash_capture.cpp` both assert the directory does not even exist.

**And off from the first instruction of `main()`, not from the first frame.**
The switch is read out of the config *before* the crash handlers are armed and
before the log is configured, because the alternative is not a cosmetic
ordering detail: arming the on-disk half first means a user who opted out gets
a log file, a `crashes\` directory, and a live crash handler across the config
load, the GL context and the `LoadLibrary` of every third-party plugin — the
most fault-prone part of start-up. That is exactly what the shipped code did
until this was found by running it: a config saying `"diagnosticsEnabled":
false` produced a 14-line log. `tests/test_diagnostics.cpp` now launches the
real binary with that config and requires the tree not to exist — the two
older "off means off" tests could not see it, because they call
`configure()`/`installCrashHandlers()` themselves and so say nothing about
*when* `main()` calls them.

**Off means off for the bundle's reach into earlier sessions too (0.99.62).** With the
switch off the bundle gets neither the previous session's log nor the report list, not
even a heading, and no file is read for them.

The directory path is still handed to the crash handler while capture is off,
so that switching diagnostics **on** mid-session has somewhere to write; the
directory itself is created at that moment, not before.

**And off from the moment the box is unticked, not from the next launch.** The
switch governs three components — the crash handler, the log, and the hang
watchdog — and for a while it governed only the first two. `start()` is a no-op
once the watchdog is running and is called once, before the frame loop, so the
directory handed to it there was the only thing that ever decided whether a
freeze report reached the disk: a user who started with diagnostics on and
unticked the box kept a live watchdog writing `hang-<pid>-N.txt` on the next
stall, which is precisely what the paragraph above promises does not happen.
The mirror image was as bad and quieter — switching diagnostics **on** after a
freeze armed the crash handler and the log and left the watchdog disarmed, so
the next freeze, the fault the user had just turned it on for, still wrote
nothing. All three now go through one function
(`AppWindow::applyDiagnosticsEnabled`), and `tests/test_diag_hang.cpp` drives
the **real** application through it in both directions with the hidden
`--diag-toggle on|off` hook — off then stall leaves nothing, on then stall
leaves a report — because there is no way to click a checkbox from ctest and the
defect lived exactly in the gap between what the checkbox governed and what it
did not.

Diagnostics also stay off for every flagged invocation (`--frames`,
`--selftest`, `--version`, the bench checks): those are tools and tests, and
they must leave nothing on the machine that ran them. `FOXSDR_DIAG_DIR`
redirects the whole tree into a caller-owned directory *and* turns capture on,
which is how the tests exercise the real application without going near a user's
`%LOCALAPPDATA%\FoxSDR`.

## Phase 2 — sending a report, and reading it back (0.62.0)

### The upload

`src/core/crash_upload.{hpp,cpp}`. Four rules outrank the feature, and each of
them is why the code is shaped the way it is:

1. **Never from inside the fault handler.** That process has already failed
   once; it cannot safely allocate, lock, or open a socket, and `WinHttpOpen`
   does all three. Phase 1 writes to disk from the handler *on purpose*. The
   send happens on the **next start**, from a healthy process — which is also
   the only moment there is a user to ask.
2. **Never block or delay the application, including shutdown.** The sweep runs
   on a background thread armed with the frame loop; the live WinHTTP request
   handle is published to an atomic, and `AppWindow::crashUploadFinish()` closes
   it before the clean-exit save, so a blocked `WinHttpReceiveResponse` returns
   with `ERROR_WINHTTP_OPERATION_CANCELLED` instead of sitting out its timeout.
   Measured against the real binary in `tests/test_crash_upload.cpp`: a server
   that accepts and never answers costs **nothing** (1.9 s against a 1.9 s
   control), and with the cancel deliberately removed the same run took **9.5 s**.
   That 7.6-second gap is the whole reason the handle is published at all.
3. **Rate-limit and deduplicate on the client.** The same signature goes at most
   once per 24 h, and at most 5 reports per 24 h whatever their signatures. (For a
   freeze "the same signature" means the same first frame of our own code on the
   stalled thread, not the same kernel wait - *What a freeze's signature is built
   from*, above.) Both
   counters live in the config, because a crash loop *is* a sequence of runs and
   a limit held in memory would reset on every restart. A 429 that arrives
   anyway is honoured, `Retry-After` clamped into a sane range.
4. **Nothing silently lost, nothing retried for ever.** Every swept report gets a
   `<report>.upload` sidecar in plain words — `sent`, `duplicate`, `backoff`,
   `failed`, `abandoned`, `too-large`, `expired` — with an attempt count. Three
   failures, or fourteen days, and it is abandoned with the report itself
   untouched, so "Open reports folder" and "Copy diagnostics" still work.

The wire contract is in `crash_upload.hpp` and in PRIVACY.md, and the payload is
asserted against **both** the code inventory and PRIVACY.md's own table, in both
directions. `POST https://foxsdr.com/api/crash`, no authentication: reports are
anonymous by design, so the defence is size caps and rate limiting rather than a
secret compiled into every shipped binary, which is not a secret.

Off means off for sending too. With the Settings switch off the sweep is never
started, no sidecar is written and no socket is opened — asserted against the
shipped binary with a socket listening that must never be connected to.

### The reader

`tools/report-reader` (built as `foxsdr-reports`) and
`src/core/report_reader.{hpp,cpp}`:

    foxsdr-reports --archive <path to symbols\> [--file feed.json] [--json]

It pulls from `GET https://foxsdr.com/api/crash/reports` with
`Authorization: Bearer $FOXSDR_REPORTS_TOKEN` — never a token compiled in, never
a token in a query string — or reads a saved feed, groups by signature with
counts, first/last seen and affected versions, and **symbolises here, not on the
server**. The PDBs stay in `symbols\` and on the NAS; uploading them to save a
step would put the complete private debug information for every shipped build on
a reachable machine.

One precisely-bounded exception, since 0.66.0: the server holds the
**function-name table alone** — `symmap.json.gz`, the `[rva, size, name]`
triples for our own code, ~90 KB per build — because symbolic grouping cannot
work without names. That is the same information a linker `.map` file shows.
What stays off the server is everything else a PDB carries and the reader
depends on it for: types, locals, line numbers, source paths, and every
module we did not build. Full symbolisation still happens here.

What it refuses to do is as important as what it does: a report whose build id
is not in the archive prints exactly that, **naming the id**, and the human
output never prints a raw offset at all. `cascade.exe+0x1A2B` looks like
information and is not — the same offset in a different link is a different
function, and printing it invites somebody to look it up in whatever build they
have to hand and believe the answer. Offsets are in the `--json` output, where a
machine can use them and nobody can misread them. The same applies frame by
frame: a stack that crosses into a plugin whose symbols were never archived
shows the application's frames resolved and names the build id it would need for
the rest.

Verified end to end against the real archive: an offset in the shipped
`cascade.exe` resolved to `cascade::net::CatServer::serveClient` at
`src/net/cat_server.cpp:446`, with the plugin frame beside it named as
unarchived.

ELF frames (a GNU build id of 40 or 32 hex digits) go to `addr2line` against the
archived `.debug`, on any platform, rather than to a DWARF parser of our own. A
frame whose archived file has the function and no line for the offset prints
`(function only: no source line in the archived symbols)` rather than looking
like a complete answer; the JSON has an empty `file` and a `line` of 0.
`tests/test_report_reader.cpp` runs this through the real `addr2line` against
two small ELF files built by `tests/elf_line_fixture.hpp` — one with DWARF line
tables, one with a symbol table only — whose answers are known in advance, and
checks the tool-absent message with `PATH` really emptied. On Linux a missing
`addr2line` fails that test; on Windows it is a stated skip.

### A freeze report now carries the module table

`hang_watchdog.cpp` wrote thread stacks and no `--- modules ---` block, so every
freeze report this product had ever written was permanently unreadable: the
stacks named modules and nothing said which *build* of them. PRIVACY.md and this
document both described the block as present. It is written now, before the
unwind and byte-identically to `crash_handler.cpp`'s, and
`tests/test_diag_hang.cpp` asserts it against a report from a real stall. Since
every fault this product has actually shipped was a hang rather than a crash,
this was the readable half missing from the half that matters most.
