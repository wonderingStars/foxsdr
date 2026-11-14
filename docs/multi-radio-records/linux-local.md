# Linux builds on this PC (WSL), 2026-11-14

Branch `feat/linux-port`, cut from `07e717e6` in worktree `D:/foxsdr-wt/en-hamops`. Never pushed. GitHub's Linux builders are refused, so every Linux build runs under WSL on this machine.

## 1. What was installed

Owner approval 2026-11-14 ("Yes, install both"); nothing else was downloaded. The existing `Ubuntu` (26.04) system was not touched (not stopped, not shut down, no package changed there).

| System | Location | Size on disk after the builds | Purpose |
|---|---|---|---|
| `Ubuntu-22.04` (22.04.5, GCC 11.4, glibc 2.35, CMake 3.22.1) | `D:\wsl\Ubuntu-22.04\ext4.vhdx` | 25.4 GB (1.39 GB fresh) | the shippable x64 build |
| `Ubuntu-24.04` (24.04, GCC 13.3, glibc 2.39, CMake 3.28) | `D:\wsl\Ubuntu-24.04\ext4.vhdx` | 11.9 GB (1.42 GB fresh) | the aarch64 cross build, qemu-user |

Commands as run (WSL 3.0.5.0; `wsl --help` read first; no launch, so no console window and no first-run user or password prompt; all work as `root`):

    wsl --install Ubuntu-22.04 --no-launch --location D:\wsl\Ubuntu-22.04 --name Ubuntu-22.04
    wsl --install Ubuntu-24.04 --no-launch --location D:\wsl\Ubuntu-24.04 --name Ubuntu-24.04

22.04 packages (the workflow's `linux` job list, plus `git file binutils pkg-config ca-certificates`, all from Ubuntu's archive):

    apt-get install -y --no-install-recommends build-essential cmake ninja-build libgl1-mesa-dev libsoapysdr-dev libssl-dev libunwind-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libwayland-dev libwayland-bin wayland-protocols libxkbcommon-dev libasound2-dev libasound2-plugins xvfb libgl1-mesa-dri git file binutils pkg-config ca-certificates

24.04: the stock sources were restricted to `Architectures: amd64` and `/etc/apt/sources.list.d/ubuntu-ports-arm64.sources` added (ports.ubuntu.com, noble, noble-updates, noble-security, arm64), `dpkg --add-architecture arm64`, then

    apt-get install -y --no-install-recommends build-essential cmake ninja-build pkg-config git file binutils ca-certificates g++-13-aarch64-linux-gnu gcc-13-aarch64-linux-gnu qemu-user libwayland-bin wayland-protocols libgl1-mesa-dev:arm64 libsoapysdr-dev:arm64 libssl-dev:arm64 libunwind-dev:arm64 libx11-dev:arm64 libxrandr-dev:arm64 libxinerama-dev:arm64 libxcursor-dev:arm64 libxi-dev:arm64 libwayland-dev:arm64 libxkbcommon-dev:arm64 libasound2-dev:arm64
    apt-get install -y --no-install-recommends libc6-dev:arm64 libstdc++-13-dev:arm64

The arm64 sysroot `/opt/arm64-sysroot` (287 MB) is every installed `:arm64` package's files copied out (`/root/mksysroot.sh`) plus the architecture-independent `/usr/include/X11` (x11proto-dev, which has no arm64 build); `/opt/arm64-toolchain.cmake` points CMake at it with `aarch64-linux-gnu-g++-13`. Proven: a C++ hello built with it is an aarch64 ELF and prints under `qemu-aarch64 -L /opt/arm64-sysroot` (qemu-user 8.2.2).

The system `Ubuntu` showed as Stopped in `wsl -l -v` after the installs and Running at the end; WSL starts and idles systems on demand, nothing here stopped it or ran a command in it.

## 2. Source fixes (each proven on Linux and on Windows)

Windows proof: `D:/foxsdr-build/en-hamops` (Visual Studio 2022 generator, Release), targets rebuilt and tests run through ctest, all headless: test_bits_* (8), test_clip_recorder, test_pipeline_zoom, test_iq_file_seek, test_waterfall_timing, test_webhook, test_rfexplorer_source, test_tci_server, test_engine_api_wire_a_{display,files,jobs,record,recordings,scan,tuning}, test_engine_api_wireb_history: 100 % passed (22 of 22 in one ctest run, test_iq_file_seek passed on its own), cascade_engine built.

| Commit | Kind | Fault (compiler error before) | After |
|---|---|---|---|
| `21d56ab2` 1/3 | source portability | `crc.hpp:440,441` `for (uint64_t i0 : {0ull, ones})` "unable to deduce std::initializer_list<auto>&& from {0, ones}" (12 targets incl. cascade_engine); `net_sock.cpp` getaddrinfo/addrinfo/freeaddrinfo undeclared (no `<netdb.h>`); `clip_recorder.hpp:246` `explicit ClipWriter(Config c = {})` "cannot convert {} to ClipWriter::Config" | `uint64_t{0}`; include added; two constructors. The engine compiles. |
| `8fe309cf` 2/3 | GCC 11 library gaps (found on the real shipping toolchain; the discovery on GCC 15 could not see them) | `std::atomic<std::shared_ptr<ZoomFft>>` at `pipeline.hpp:1267`: "std::atomic requires a trivially copyable type" (71 translation units); `std::chrono::clock_cast` at `file_start_time.hpp:95` "is not a member of std::chrono" | pipeline: a small slot using `std::atomic_load/store` where `__cpp_lib_atomic_shared_ptr` is absent and not MSVC; file_start_time: offset-from-now conversion where `__cpp_lib_chrono < 201907` and not MSVC. MSVC keeps the original code. |
| `b65f17d5` 3/3 | tests | see below | all test targets build |

Commit 3/3 in detail. Kept Windows-only off non-Windows builds (list `CASCADE_WINDOWS_ONLY_TESTS`, `if(NOT WIN32)`): test_extio (registered although its helper target exists only under WIN32: configure-time generator error), test_gui_desktop_guard, test_hardware_guard, test_vendor_lib, test_g33ddc_source, test_g39ddc_source, test_sdriq_d2xx, test_engine_api_wireb_ftdi, test_engine_api_wireb_winradio, test_engine_api_wireb_neededlibs (their subject is Windows DLL loading or Win32 desktop APIs: `windows.h` via `tests/eol_fixture.hpp`). Made portable: the seven `test_engine_api_wire_a_*` and `test_engine_api_wireb_history` (`::_getpid` mapped to `getpid` as test_engine_api_wire_a_airband.cpp already does), test_tci_server (httplib.h already defines the `INVALID_SOCKET` macro on POSIX), test_waterfall_timing and test_webhook (`std::max` of `long` and `long long`), and `RB_SRC_DIR` for test_rfexplorer_source (its definition was inside `if(WIN32)`). No test weakened, no expectation changed.

Not fixed because GCC 11 and 13 do not need it: missing `#include <algorithm>` in test_web_bookmarks.cpp:379 and test_web_clip.cpp:251 (the discovery saw these on GCC 15 only).

## 3. x64 application, Ubuntu 22.04 / GCC 11.4 (build as the workflow: `cmake -G Ninja -DCMAKE_BUILD_TYPE=Release`)

- `cascade` and `foxsdr-engine` link (1150 of 1150 steps after the fixes). Highest versions imported: GLIBC_2.35, GLIBCXX_3.4.30 (both programs). `cascade --version` and `foxsdr-engine --version` on aarch64 print "FoxSDR 0.99.108".
- ctest, headless under `xvfb-run`, `WAYLAND_DISPLAY` unset, `XDG_SESSION_TYPE=x11 FOXSDR_CI_NULL_AUDIO=1 LIBGL_ALWAYS_SOFTWARE=1`, null ALSA device: 1118 tests registered.
  Run 1: 1012 passed, 106 failed. 76 of the 106 were "Not Run" gui tests: on Linux `gui_desktop_guard` always refuses ("the desktop this process runs on could not be read"), because `tests/gui_desktop_guard.cpp` returns an empty desktop name off Windows. The workflow does not set the documented `FOXSDR_GUARD_DESKTOP_NAME` hook, so this is a CI finding: on a runner every gui test would be Not Run. Re-run with `FOXSDR_GUARD_DESKTOP_NAME=xvfb-virtual` (the Xvfb display is not a visible desktop): the 76 pass.
  Final: 1088 passed, 30 red (re-run alone and serially, the same 30 fail again, so none is load).
- The window tests used no real hardware and no Windows desktop. Nothing was set on the Windows side.

The 30 reds, classified (none is a compile fault):

| Class | Tests | Cause |
|---|---|---|
| Build folder outside the source tree (the same four the standard rules list as false on Windows) | test_airband_data, test_converter_call_sites, test_page_resize_id, test_png_decode | `findRepoRoot` walks up from the executable; the build is in `/root/fox-build`, the source in `/root/fox` |
| Generated fixture absent | test_engine_api_source_kinds | `scope.wav` is a git-ignored generated file |
| Test written with Windows data | test_file_drop (`C:\a\one.wav`), test_plugin_repo_helper (`.exe`/`.dll` companions), test_rig_cat (`COM3`), test_sys_manifest_tool, test_analyser_project:114 (sha256 compared with CNG) | Windows paths and names baked into the expectation |
| Test bug the Windows runtime hides | test_aircraft_out | `strcpy(m.hexIdent, "4840D6X")` writes 8 bytes into `char[7]` (test line 355); glibc's fortify aborts "buffer overflow detected". Not changed: the intended input (seven characters, no terminator) is already line 359. |
| Real difference, NOT diagnosed (failing check lines recorded in `/root/ctest-x64-rerun3.log` on the 22.04 system) | test_aaronia_source, test_kiwisdr_client, test_sdrconnect_source, test_spyserver_source (SIGPIPE in the test's stand-in server or client; product sends use MSG_NOSIGNAL), test_adif_broadcast:130 (UDP broadcast not received), test_decode_webhook:183/186 and test_webhook (timeout), test_mqtt_publisher:672/734/735, test_tcp_fanout:344/352, test_engine_api_hotplug (hotplug monitor not running: 20 checks), test_engine_api_kiwisdr:386, test_engine_api_wire_c_cat (CAT emulation does not start), test_engine_api_wire_c_sys:202-208, test_engine_main_args:145, test_engine_main_provision:320, test_engine_main_smoke:129, test_if_recorder:379 (1 checkpoint, expected 3), test_power_log (120 s timeout), test_sigmeas_bandwidth:75 (3697.7 vs 3526.2 Hz) | each is a behaviour or timing difference on Linux, listed and not papered over |

UNVERIFIED: whether any of the "real difference" rows is a product bug on Linux; the suite was run from a synced working tree (CRLF files from the Windows checkout, shell scripts converted), not from an LF clone; the Slint tests (label `slint`) are not part of this build.

## 4. Plugins, commit `b889d40` (cloned from `C:/Users/steve/foxsdr-plugins-dev`, a clone, nothing written there)

x64 (22.04, as `plugins-linux.yml`): all targets built (2149 of 2149, no FAILED). 95 modules plus `voacap_probe.so` (96 `.so` files). Highest `GLIBC_` imported by any of them (`objdump -T`): 2.35 (the ceiling, met by 3+ modules, e.g. demod-analyzer, dtmf-decoder, ils-meter). ctest `-j3 --timeout 1200`: 295 registered, 294 run, 292 passed; test_drm and test_vdl2_depth failed under the three-way load and both passed alone (28.3 s and 9.8 s). vdl2 `o7` ("a file that cannot be opened") is permission based and the system runs as root, so it can also fail there on its own account; it passed in the single run.

## 5. arm64 (Ubuntu 24.04, GCC 13.3 cross, sysroot)

- Application: `cascade` and `foxsdr-engine` are ELF aarch64, highest GLIBC_2.38 (ceiling 2.39), GLIBCXX_3.4.32. Under `qemu-aarch64 -L /opt/arm64-sysroot`: `--version` prints "FoxSDR 0.99.108" for both. The suite was not run under emulation.
- Plugins: 751 of 751 steps, 0 failed; 95 modules `*-abi3-linux-arm64.so`, highest GLIBC_2.38. `probe_plugin` (aarch64) under qemu-user loaded 94 of 95; `uk-cb-channels` answers "declares only capabilities this probe does not know", and the x64 probe says the same, so this is the probe, not the arch.

## 6. The Slint window on Linux (reported, not built)

`CASCADE_BUILD_SLINT` is OFF by default and `src/slint/CMakeLists.txt` does nothing without `find_package(Slint)`. Nothing in `src/slint` is Windows-only in the CMake (the `WIN32`/`.rc` and `slint_cpp.dll` copy branches are guarded; the non-Windows branch links `OpenGL::GL` when found). It would need: (1) the Slint C++ SDK for Linux on `CMAKE_PREFIX_PATH`, which per docs.slint.dev/latest/docs/cpp/cmake is the prebuilt `slint-cpp-<version>-Linux-x86_64.tar.gz` from Slint's GitHub releases (x86-64 only; no Rust needed; CMake >= 3.21, so 22.04's 3.22 is enough) and `LD_LIBRARY_PATH` set to its `lib` at run time; the version this tree used on Windows is 1.18.1; (2) for arm64 there is no binary package, so it needs Slint built from source, which needs a Rust toolchain (the tree's notes say Rust >= 1.92) and cargo's crate downloads: neither is approved here; (3) Python 3 for the translation step (otherwise English only); (4) unverified: the glibc ceiling of the prebuilt Linux SDK (not downloaded, so not measured) and whether the window compiles on GCC 11; Slint's own runtime libraries (fontconfig, X11/Wayland) were not checked.

## 7. Repeating each build in one command

On the x64 system (`wsl -d Ubuntu-22.04 -u root`), from an LF checkout of the tip in `/root/fox`:
`cmake -S /root/fox -B /root/fox-build -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C /root/fox-build -j6 && FOXSDR_CI_NULL_AUDIO=1 XDG_SESSION_TYPE=x11 FOXSDR_GUARD_DESKTOP_NAME=xvfb-virtual xvfb-run -a ctest --test-dir /root/fox-build -j4`
Plugins x64: `cmake -S /root/plugins -B /root/plugins/build -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C /root/plugins/build -k 0 && ctest --test-dir /root/plugins/build -j3 --timeout 1200`.
arm64 (`wsl -d Ubuntu-24.04 -u root`): `cmake -S /root/fox -B /root/fox-arm -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=/opt/arm64-toolchain.cmake && ninja -C /root/fox-arm cascade foxsdr-engine && qemu-aarch64 -L /opt/arm64-sysroot /root/fox-arm/foxsdr-engine --version`; plugins the same with the plugin tree and `ninja -k 0 $(cat /root/plug-targets.txt) probe_plugin`.
Gotcha: with `core.autocrlf=true`, `git archive` hands shell scripts CRLF and `tools/archive-symbols-linux.sh` then fails at link ("set: -: invalid option"); use `git -c core.autocrlf=false -c core.eol=lf archive`. Also under Git Bash use `MSYS_NO_PATHCONV=1` for `wsl` arguments.

## 8. Unverified / open

The 20-odd undiagnosed Linux differences above; the x64 suite on an LF clone; the Slint SDK facts beyond the docs page; no AppImage, tarball or symbol-archive step was run (the workflow's later steps), only build and test; fix 2's offset-from-now fallback is accurate to the gap between the two clock reads (milliseconds); on Linux test_iq_file_seek passes with it, on Windows MSVC takes the original branch; config.json sha256 checked unchanged at the end (`c21ce98d...04603c3`).
