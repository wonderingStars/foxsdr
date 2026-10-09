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
a single-file AppImage — the same payload the CI tarball carries (the
`cascade` binary and the `resources/bandplans` it reads next to itself at
runtime) plus `LICENSE`, `THIRD-PARTY-LICENSES.txt` and `POSTINSTALL.txt`. It
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
`build/cascade`, so the tarball and the AppImage carry none; CI uploads that
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
