# Building from source

Windows, Linux, the installer, the stable and nightly channels, and symbols.

Back to the [README](../README.md).

## Building (Windows)

Requires Visual Studio 2022 Build Tools and CMake. All GUI/DSP/audio/JSON
dependencies (Dear ImGui, GLFW, PortAudio, nlohmann/json, pffft) are vendored
at pinned revisions under `third_party/` and built from source — see
[third_party/THIRD_PARTY.md](../third_party/THIRD_PARTY.md). The only external
dependency is SoapySDR, consumed from vcpkg at `C:\vcpkg` (`soapysdr`
installed): it is the hardware ABI boundary — runtime vendor modules
(SoapyUHD etc.) must match the system SoapySDR ABI, so it is deliberately
not vendored.

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

**Running the suite in two passes.** Every test carries exactly one of the
labels `headless` or `gui` (set and checked in `tests/CMakeLists.txt`): `gui`
is the minority that put a real window on the desktop, which is what kept the
whole suite serial. The bulk runs in parallel and the window-openers follow,
serial, exactly as before:

```
ctest --test-dir build -C Release -L headless -j8 --output-on-failure
ctest --test-dir build -C Release -L gui --output-on-failure
```

The two passes together are the whole suite — the partition's totality is
asserted at configure time, so a test cannot fall between them. The `gui` pass
wants the desktop to itself: its tests open windows, steal focus and drive the
pointer through input scripts, so do not type into anything while it runs, and
never run it `-j`. A new test is `headless` unless it is added to
`CASCADE_GUI_TESTS` in `tests/CMakeLists.txt`. (On Linux, drop `-C Release`;
under `xvfb-run` the `gui` pass opens its windows on the virtual display, as
CI does for the whole suite.)

**Memory-error, thread, static-analysis and fuzz testing.**
`-DCASCADE_SANITIZE=address` builds everything (application, tests, the libraries
inside them) under AddressSanitizer in a separate build directory,
`-DCASCADE_SANITIZE=thread` does the same under ThreadSanitizer (Linux, GCC or
Clang) for the tests that put threads against each other,
`-DCASCADE_ANALYZE=ON` runs MSVC's `/analyze` over our own code, and the parsers
and rate-dependent DSP have fuzz targets whose regression corpus runs in the
ordinary suite. All are off unless asked for and change nothing in a normal build;
how to run them, what they found and what the warning counts are is in
[docs/SANITIZERS-AND-FUZZING.md](SANITIZERS-AND-FUZZING.md).

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

**How the engine's pass is driven in a bounded run.** The engine's pass runs on its own thread (the receiver's control thread), at its own
period and woken by every command. A bounded run (`--frames N`, which is what the tests and the screenshots use) STEPS it instead: the
window's frame asks for the three parts of the pass at the places the pass always ran in the frame, so a run of N frames is the same N frames
every time and a picture taken at a frame is reproducible. `FOXSDR_PASS=free` makes a bounded run free-running like the product (for a test of
an invariant, not of a frame; the run's `FOXSDR_SOURCE_DEVICE`, `FOXSDR_PRESS_PRESET` and similar seams reach the engine only through the pass
and are fine in both); an interactive run is always free-running. The log says which on its first line after "frame loop starting".

## Building (Linux)

**Actively supported, with the hardware path still unconfirmed.** It builds,
the tests pass, and 21 of the 28 catalogued plugins install from the in-app
catalogue (see [Plugins](MANUAL.md#plugins)) — see [Linux status](HARDWARE.md#linux-status) for exactly what has and has not yet been run against real hardware and
a real desktop session.

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
GitHub's Linux jobs instead route ALSA to `null`. That device can open and
briefly prime while failing to sustain playback, so the pipeline audio-lead
integration test is explicitly reported as skipped there. It runs on a machine
with real audio output; the null-device jobs still run the sink's callback and
buffer tests.

**Hardware.** The RTL-SDR, HackRF, Airspy R2/Mini, Airspy HF+, HydraSDR RFOne,
RX888 mk2 and Mirics native drivers talk to their radios on Linux through
`src/usb/usbfs_device.cpp` — the kernel's usbfs ioctls on
`/dev/bus/usb/BBB/DDD`, not libusb — behind the same `src/usb/usb_device.hpp`
contract the Windows WinUSB transport keeps. Install the udev rule from
`installer/linux/` first (`installer/linux/README.md` has the three commands
and the troubleshooting list); without it every open fails with a permissions
error that says so. An RTL-SDR needs no unbinding from the kernel's DVB
driver — the transport detaches it itself. None of this has been driven with
a real radio yet. The SDRplay driver
does work on Linux as of this port: it `dlopen()`s `libsdrplay_api.so.3`, the
SONAME SDRplay's own `.run` installer registers with `ldconfig` (get the API
from [sdrplay.com](https://www.sdrplay.com), version 3.x), falling back to the
bare `libsdrplay_api.so` for a dev machine with only the unversioned symlink —
but like everything else under [Linux status](HARDWARE.md#linux-status), that
path has not been exercised against a real RSP. The ADALM-Pluto's driver
(`src/source/iiod_client.*`) talks IIOD over a plain TCP socket rather than
USB, so it already builds and runs identically on both platforms — it is
likewise unverified against a real Pluto here. The rtl_tcp client
(`src/source/rtl_tcp_source.*`) shares that socket code and is written to the
same portable calls, but it has only been built and tested on Windows so far;
the retry of a receive or connect that a signal interrupts (EINTR), which only
Linux produces, has not been run at all.
Any radio SoapySDR itself can
reach (`libsoapysdr-dev` above) already works the same as on Windows. Opening
the reports folder, the update banner's "Open foxsdr.com" and the privacy
policy link all go through `xdg-open`, forked and exec'd directly (never
through a shell), so a desktop environment with no `xdg-open` on `PATH` will
see those buttons fail rather than silently do nothing.

**Packaging.** `installer/linux/build-appimage.sh` packages a Release build as
a single-file AppImage — the window application's payload as the CI tarball
carries it (the `cascade` binary and the `resources/bandplans` it reads next to
itself at runtime) plus `LICENSE`, `THIRD-PARTY-LICENSES.txt` and
`POSTINSTALL.txt`. It stays a launcher for the window application: the tarball
also carries `foxsdr-engine` (below), the AppImage does not. It
does not bundle SoapySDR, OpenSSL, ALSA or OpenGL, so a machine running the
AppImage needs the same libraries the tarball already needs on the host. Build
it after the `cmake --build` step above:

```
installer/linux/build-appimage.sh build
```

This downloads `appimagetool` (pinned by version and verified against a fixed
sha256) and writes `dist/FoxSDR-<version>-x86_64.AppImage`. FUSE is not
required to build it (the script runs `appimagetool` itself with
`APPIMAGE_EXTRACT_AND_RUN=1`) nor to run the result on a machine that lacks
FUSE — set the same environment variable before launching it:

```
APPIMAGE_EXTRACT_AND_RUN=1 dist/FoxSDR-<version>-x86_64.AppImage
```

CI builds and smoke-tests this AppImage (`--version`, then `--frames 60` under
Xvfb) on every push and publishes it as a separate build artifact alongside
the tarball.

**Debug information.** A Release build on Linux compiles with `-g` so that a
crash report can be placed on a source line, not just a function. After the
link, `tools/archive-symbols-linux.sh` writes the DWARF (compressed) to
`symbols/cascade.debug/<build id>/cascade.debug` and strips it from
`build/cascade`, so the tarball and the AppImage carry none (the same is done
for `build/foxsdr-engine`, under its own build id, to
`symbols/foxsdr-engine.debug/<build id>/foxsdr-engine.debug`); CI uploads that
`symbols/` directory as a `foxsdr-linux-*-symbols` artifact. A `Debug` or
`RelWithDebInfo` build keeps its DWARF in the binary, which is the one to use
under a debugger. To put an offset from a report on a line, run
`addr2line -f -C -e symbols/cascade.debug/<build id>/cascade.debug 0x<offset>`;
`docs/DIAGNOSTICS.md` ("Linux symbols") has the rest, including frames in
system libraries.

**Plugins: one catalogue, both platforms.** The catalogue lists every build of
a plugin and each installation picks the one matching its own os and
architecture, so a Windows and a Linux machine read the identical file and
install different binaries from it. 21 of the 28 plugins ship for both; the
seven newest instrument decoders (ACARS, FLEX, 406 MHz beacons, ERT meters,
433 MHz weather sensors, WEFAX) are Windows-only so far, as is the example
plugin, which is built from this repository rather than the plugin
repository.

## Building the engine without the window (`CASCADE_BUILD_INTERFACE`)

`foxsdr-engine` is the receiver with no window: the same engine as `cascade`,
started from a command line, for a service or a machine with no desktop
([headless-service.md](headless-service.md)). It is built with everything else
by default. The CMake option `CASCADE_BUILD_INTERFACE` (default **ON**) can turn
the window half off:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCASCADE_BUILD_INTERFACE=OFF
cmake --build build -j"$(nproc)"
build/foxsdr-engine --version
```

With it **OFF** the configure never reaches OpenGL, GLFW, Dear ImGui, the shell,
the interface or the `cascade` executable, so none of the OpenGL, X11, Wayland
and xkbcommon packages above is needed. What is still needed on Debian or
Ubuntu is the engine's own: `build-essential cmake ninja-build libsoapysdr-dev
libssl-dev libunwind-dev libasound2-dev` (PortAudio's ALSA backend). What it
builds: the engine library, `foxsdr-engine`, `foxsdr-reports` (the crash-report
reader), the engine link check, and the staging of `resources/bandplans` and
`resources/firmware` beside `foxsdr-engine` (the engine reads both from beside
itself, as `cascade` does). **It builds no tests**: nearly every test links the
window or spawns `cascade`. The OFF configure is itself tested, from a normal
build: `engine_only_configure` (`tests/engine_only_configure.cmake`) configures
the project again in a scratch directory with the option OFF and OpenGL
disabled, which must succeed, and with the option ON, which must fail on
OpenGL (so the first result means something), and checks that the staged
resources land.

`foxsdr-engine`'s symbols are archived by a POST_BUILD step, exactly as
`cascade`'s are (see Symbols, below), under the module name `foxsdr-engine`.

**Not yet verified:** that a bare Raspberry Pi image (no OpenGL, X11 or Wayland
packages installed at all) configures and builds with the package list above.
The OFF configure and build have been run on Windows (MSVC; the engine built
and ran from that tree) and on Ubuntu under WSL2, which has those
packages installed (OpenGL was disabled with
`-DCMAKE_DISABLE_FIND_PACKAGE_OpenGL=ON`, CMake's own "pretend it is not
installed" switch, so a `find_package(OpenGL)` that was reached would fail the
configure); GitHub's `linux-engine-only` job (`.github/workflows/build.yml`)
builds it with exactly the package list above and no others, and has not run yet.

## Building the installer

A Windows installer (Inno Setup 6) lives under `installer/` — it packages
`cascade.exe`, `SoapySDR.dll`, the app-local MSVC runtime, the license, and
post-install hardware notes into
`installer\Output\foxsdr-setup-<version>.exe`. Build instructions:
[installer/README-installer.md](../installer/README-installer.md). Radio
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
[docs/DIAGNOSTICS.md](DIAGNOSTICS.md).

**`foxsdr-engine` is archived the same way, as its own module with its own build
id** (`foxsdr-engine.pdb` on Windows, `foxsdr-engine.debug` on Linux), by a
second `POST_BUILD` step into the same `symbols/` and the same `index.txt`;
`tests/test_engine_packaging.cpp` asserts it, as `test_diagnostics` does for
`cascade`. The nightly and the Windows release mirror `cascade.exe`'s symbols
only, because `foxsdr-engine.exe` is not a Windows release file; the Linux
tarballs carry `foxsdr-engine`, and their CI symbol artifacts hold both modules
(`docs/RELEASING.md`, the `symbols` step).
