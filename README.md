# FoxSDR

A from-scratch software-defined radio receiver for Windows and Linux: spectrum and
waterfall, multi-mode demodulation (NFM/WFM/AM/DSB/USB/LSB/CW), stereo FM with
RDS, recording, bookmarks, a scanner, band plans, and hardware support for any
radio SoapySDR can reach.

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
- **Map and decoders.** Aircraft, vessels and stations plotted together, with
  optional map imagery from a basemap plugin. Decoder plugins produce text or
  pictures (slow-scan and weather-satellite images, shown in their own windows
  and saveable).
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

## Building (Linux)

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

**Plugins are currently built for Windows only.** The Linux application runs
and the plugin loader handles `.so` modules, but the published catalogue
contains no Linux builds yet, so the decoders (ADS-B, AIS, APRS, POCSAG, SSTV)
are unavailable there for now.

## Plugins

Decoders can be installed as separate native plugins, from an in-app
catalogue ("Plugins" -> "Get plugins"). The catalogue is contacted only when
you press Browse: nothing is fetched at startup, and no plugin ever updates
itself.

A plugin may declare several capabilities. Decoders are fed real samples —
either the tuned, demodulated audio or the raw receiver band — and produce
either text lines or **images** (slow-scan and weather-satellite pictures,
shown in their own window and saveable as BMP). A plugin may also put targets
and tracks on the map, and declare a window of its own.

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

## Building the installer

A Windows installer (Inno Setup 6) lives under `installer/` — it packages
`cascade.exe`, `SoapySDR.dll`, the app-local MSVC runtime, the license, and
post-install hardware notes into
`installer\Output\foxsdr-setup-<version>.exe`. Build instructions:
[installer/README-installer.md](installer/README-installer.md). Radio
hardware support is installed separately by the user (PothosSDR or
radioconda) — see `installer/POSTINSTALL.txt`; the app runs with no hardware
at all (signal generator + IQ playback).

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
