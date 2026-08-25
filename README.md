# FoxSDR

A from-scratch software-defined radio receiver for Windows: spectrum and
waterfall, multi-mode demodulation (NFM/WFM/AM/DSB/USB/LSB/CW), stereo FM with
RDS, recording, bookmarks, a scanner, band plans, and hardware support for any
radio SoapySDR can reach.

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
- **Hardware.** Anything SoapySDR reaches, with antenna, sample-rate and
  per-stage gain selection. Developed against an Ettus B200; the built-in
  signal generator and IQ-file playback mean it runs with no radio at all.
- **Working with signals.** Bookmarks, a band scanner, band plans, and
  recording of both audio and raw I/Q.
- **Map and decoders.** Aircraft, vessels and stations plotted together,
  coloured by altitude band, with optional map imagery from a basemap plugin.
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
  windows and saveable).
- **Browser access.** The full interface over HTTP on your LAN, at feature
  parity with the desktop, with password authentication and live audio. Off by
  default; see [Browser access](#browser-access) for the security posture.

**Verification, honestly stated.** The DSP core and every decoder carry unit
tests (`ctest` runs 51 suites), and the audio chain has been confirmed by ear
on broadcast FM. Of the decoders, **ADS-B is the one confirmed against real
off-air signals** — aircraft decoded live, with ICAO address blocks and
callsigns agreeing across independent message types. The others (AIS, APRS,
SSTV, Morse, RTTY, POCSAG) are verified against synthesised signals and
published constants, which is real evidence but not the same thing. The
Inmarsat-C plugin is published at 0.1.0 and explicitly marked EXPERIMENTAL:
roughly ten of its air-interface constants are reconstructed guesses, and it
will most likely decode nothing off air. Each plugin's catalogue entry says
where it stands.

See [PLAN.md](PLAN.md) for the roadmap and architecture.

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
anything else that speaks the protocol Hamlib's `rigctld` uses. Turn on **CAT
control** in the settings panel, then point the client at this machine and
choose a Hamlib **NET rigctl** radio. Port 4532 is the default because it is
the one those clients already expect.

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
catalogue ("Plugin store" -> "Get plugins"). Browsing and installing live in
that section; what is already installed, and what it is allowed to do, lives in
the "Plugins" section beneath it. The catalogue is contacted only when
you press Browse: nothing is fetched at startup, and no plugin ever updates
itself.

A plugin may declare several capabilities. Decoders are fed real samples —
either the tuned, demodulated audio or the raw receiver band — and produce
either text lines or **images** (slow-scan and weather-satellite pictures,
shown in their own window and saveable as BMP). A plugin may also put targets
and tracks on the map, and declare a window of its own.

**Stop and start.** Every loaded plugin's row has a **Stop** button, and a
stopped one has **Start**. Stopping destroys everything that plugin had
running — its decoders, its map targets and trails, its window, its basemap
tiles — while leaving the module loaded and the row where it was, so a stopped
plugin decodes nothing, draws nothing, and cannot move the receiver. The row
says "Stopped" in orange, because a plugin that produces nothing for a reason
you have forgotten choosing is exactly what this must not become. It is
remembered between sessions and across a rescan. Pressing one of the plugin's
own preset buttons starts it first: pressing "ADS-B 1090 MHz" is an unambiguous
request for that plugin, and tuning there with the decoder still switched off
would be worse than useless.

**Mute audio while running.** A decoder that consumes raw I/Q is handed the
whole receiver band and tunes inside it, so the channel your speakers are fed
is not the signal being decoded: on ADS-B at 1090 MHz it is hiss, at whatever
the volume happens to be. Every loaded plugin's row therefore has a **Mute
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
is not producing the sample rate it asked for — the Plugins panel already says
so on its row, in orange, and taking the sound away on behalf of a decoder the
program itself says is not being fed would be silence for no benefit.

**Tuning away.** Leave a running decoder's preset and FoxSDR asks once whether
to stop it so the sound comes back, with a button that does exactly that.
Decline and the plugin keeps running, the audio stays muted — sound returns
when the plugin stops, which is what the question said — and a small
**Sound muted by *plugin*** banner sits beside the frequency readout, with its
own Stop button, until the plugin is stopped or you tune back. It asks again
only after you have returned to a preset and left it again.

**Tune permission.** A plugin that can move the receiver can also take it away
from you, so a plugin may only retune the radio if you tick it under
"Plugins" -> "Receiver control". It is off by default and off for every newly
installed plugin; a plugin that asks and is refused says so on that panel, so a
satellite tracker that needs Doppler correction is one visible click away
rather than mysteriously idle. The grant is per plugin and is remembered
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
The panel prints the one in use above the catalogue. Nothing needs elevating
either way.

Compatibility is ABI-exact. A plugin must be built against this host's
`src/core/plugin_abi.h` and declare exactly its ABI version — a near miss is
refused rather than loaded, because a struct-layout difference becomes memory
corruption days later. A plugin built for an older FoxSDR therefore needs a
new build from its author; no update can fix it.

Retirement: the catalogue may publish a minimum supported version per plugin.
That floor is cached locally the moment a catalogue is seen, so it applies
offline and forever after, and an installed plugin below it is **disabled** —
renamed out of the scan, so it is never loaded into the process — and shown in
red with a one-click Update when the catalogue has a newer build. Plugins the
catalogue has never described (private or hand-installed builds) are left
alone and keep loading.

## Browser access

FoxSDR can serve its interface to a browser, so the receiver can live where the
antenna is. Open **Web access** in the settings panel, set a password, tick it
and press Apply; the panel then shows the address to open on another machine.

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

## Privacy

FoxSDR reports anonymous usage counts. **Settings → Usage reporting** is
**on by default** — untick it and reporting stops. It sends counts only:
version, operating system, session length, which modes and plugins get used,
and which radio model, against a random identifier created on your machine.
Switching it off deletes that identifier.

**It never sends frequencies, anything decoded, your location, or your IP
address**, and hardware serial numbers are stripped before the radio model is
sent. The complete list of what is and is not transmitted is in
[PRIVACY.md](PRIVACY.md), the payload is held to it by an automated test, and
the receiving endpoint's source is in [telemetry-worker/](telemetry-worker/).

Apart from that, the application makes exactly one network request: fetching
the plugin catalogue, and only when you press **Browse**.

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
- A rotating log lives in `%LOCALAPPDATA%\FoxSDR\logs\foxsdr.log`.
- Each report that has been sent — or could not be — carries a small `.upload`
  file beside it saying which, in plain words. At most five reports a day leave
  one machine, and the same fault only once a day, so a machine stuck in a crash
  loop stops sending on its own.
- **Settings → Diagnostics** shows both paths, has the on/off switch, and has
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

**The setup executable is not code-signed yet**, so Windows SmartScreen will
show "Windows protected your PC". Choose **More info → Run anyway** if you are
happy to proceed. Signing is planned.

Radio hardware support is a separate install (PothosSDR or radioconda) — see
`POSTINSTALL.txt` in the install folder. FoxSDR runs with no hardware at all
using the signal generator or I/Q playback.

FoxSDR locates either of those in its default location and adds it to the
SoapySDR module search path itself; `SOAPY_SDR_ROOT` overrides the guess and a
`SOAPY_SDR_PLUGIN_PATH` you set by hand is appended to, never replaced. **An
RTL-SDR dongle needs one further step on Windows**: it ships bound to the DVB-T
television driver, under which it is invisible to every SDR application, and
Zadig must be used to bind WinUSB to "Bulk-In, Interface (Interface 0)" instead.
`cascade.exe --soapy-check` prints the search paths, the loaded modules and
either the device it opened or the reason there was none.

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
