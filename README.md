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
is a real window you can drag as large as your screen, and each reopens if you
left it open; the Fitted modules window comes back at the size and place you
left it too. The catalogue is contacted only when you
press **CHECK NOW** inside the store window: nothing is fetched at startup, and
no plugin ever updates itself.

A plugin may declare several capabilities. Decoders are fed real samples —
either the tuned, demodulated audio or the raw receiver band — and produce
either text lines or **images** (slow-scan and weather-satellite pictures,
shown in their own window and saveable as BMP). A plugin may also put targets
and tracks on the map, and declare a window of its own.

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
remembers its state across banks and restarts; a section unfolds rather than
appearing, a bank comes up like a lamp rather than switching in one frame, and
the rail opens on whichever bank you left it on. Every chip on a row still
reports what that section is doing without opening it.

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
both to the desktop, with a taskbar button to come back from. On Linux the
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

The lettering is part of the same rule. FoxSDR carries its own typefaces rather
than borrowing the system's, so the panel looks the same on every machine:
**Saira Condensed** for anything a hand operates and for all prose, and **Nova
Mono** for counter digits — monospaced so the frequency stops shuffling
sideways as it changes. Both are SIL Open Font License faces, embedded in the
executable unmodified; `third_party/THIRD_PARTY.md` records exactly which
release each one is. Nova Mono is used for figures and not for words, which is
measured rather than fussy: its capital M is three close stems and rasterises
as a solid block below about 20 pixels.

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
target is followed. It remembers whether it was open and the rectangle it sat
in, and a window you shut stays shut across a restart — a propagator reports a
full sky on the first frame of every launch, with no radio and no action from
you, and that must never be allowed to reopen a window you closed. Within a
session a page does open itself on the arrival edge, when a plugin that had
nothing plotted starts reporting again; closing it holds until that happens
afresh.

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
  Plugin store window. Nothing is fetched at startup, restoring that window
  open fetches nothing, and no plugin ever updates itself.
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
- A rotating log lives in `%LOCALAPPDATA%\FoxSDR\logs\foxsdr.log`.
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
