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
`cmake -S /root/fox -B /root/fox-build -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C /root/fox-build -j6 && FOXSDR_CI_NULL_AUDIO=1 XDG_SESSION_TYPE=x11 xvfb-run -a ctest --test-dir /root/fox-build -j3` (2026-11-16: `FOXSDR_GUARD_DESKTOP_NAME` is no longer needed under `xvfb-run`; unset `WAYLAND_DISPLAY`; section 9)
Plugins x64: `cmake -S /root/plugins -B /root/plugins/build -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C /root/plugins/build -k 0 && ctest --test-dir /root/plugins/build -j3 --timeout 1200`.
arm64 (`wsl -d Ubuntu-24.04 -u root`): `cmake -S /root/fox -B /root/fox-arm -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=/opt/arm64-toolchain.cmake && ninja -C /root/fox-arm cascade foxsdr-engine && qemu-aarch64 -L /opt/arm64-sysroot /root/fox-arm/foxsdr-engine --version`; plugins the same with the plugin tree and `ninja -k 0 $(cat /root/plug-targets.txt) probe_plugin`.
Gotcha: with `core.autocrlf=true`, `git archive` hands shell scripts CRLF and `tools/archive-symbols-linux.sh` then fails at link ("set: -: invalid option"); use `git -c core.autocrlf=false -c core.eol=lf archive`. Also under Git Bash use `MSYS_NO_PATHCONV=1` for `wsl` arguments.

## 8. Unverified / open

(2026-11-16: the undiagnosed differences are all diagnosed, section 9; what is still open is its table `OPEN ROWS FOR THE NEXT LANE`.) Earlier list: the 20-odd undiagnosed Linux differences above; the x64 suite on an LF clone; the Slint SDK facts beyond the docs page; no AppImage, tarball or symbol-archive step was run (the workflow's later steps), only build and test; fix 2's offset-from-now fallback is accurate to the gap between the two clock reads (milliseconds); on Linux test_iq_file_seek passes with it, on Windows MSVC takes the original branch; config.json sha256 checked unchanged at the end (`c21ce98d...04603c3`).

## 9. The 30 reds (2026-11-16, lane `linux-reds`, branch `feat/linux-port`, built and run on `Ubuntu-22.04`, GCC 11.4)

Every red of section 3 was run alone, its failing line read, and the cause found; none is left unexplained. Verdicts: PRODUCT (the program misbehaves on Linux; fixed portably, the test kept), TEST (the test assumes Windows; made portable, nothing weakened except the one expectation named below), WINDOWS-ONLY (a Windows feature; the test is not built off Windows and the table says what a Linux user does not get). Commits are one per cause (`git log fe4e8d26..HEAD`).

**Counts.** Of the 30: 6 are PRODUCT (the four SIGPIPE tests, the missing CAT-over-TCP service, the rig-cat serial refusal wording), 1 is WINDOWS-ONLY (hot-plug), 23 are TEST faults. After the lane 28 of the 30 run green on Linux, `test_engine_api_hotplug` is not built there (Windows-only) and `test_converter_call_sites` is still red, for a real finding of the product that is red on Windows for the same reason (open row 3). What the product faults were, in one line each: a TCP `send()` with flags 0 killed the whole process with SIGPIPE when a peer closed (fixed: `MSG_NOSIGNAL`); the CAT-over-TCP service did not exist off Windows (written); a bad serial setting was never named off Windows (fixed, small).

| # | Test | Cause (one sentence) | Verdict | Fix, or why not |
|---|---|---|---|---|
| 1 | test_aaronia_source | its stand-in server's `send()` and the product's control-socket `send()` had flags 0: a write to a closed socket raises SIGPIPE, which ends the process | PRODUCT (+ test helper) | `MSG_NOSIGNAL` through `net/send_flags.hpp` in the product's senders; `tests/src_test_net.hpp` the same |
| 2 | test_kiwisdr_client | the WebSocket close frame (`\210\202`) sent by cpp-httplib to a server that had gone: httplib sends with flags 0 and only `httplib::Server`'s constructor ignores SIGPIPE | PRODUCT | `CPPHTTPLIB_SEND_FLAGS=MSG_NOSIGNAL` on the `cascade_httplib` target (Linux) |
| 3 | test_sdrconnect_source | SIGPIPE as above, then the stand-in counted the client's close frame on its own thread after `closeDevice()` returned | PRODUCT + TEST | send flags (1, 2); the test waits (bounded) for the frame, then checks the same thing |
| 4 | test_spyserver_source | SIGPIPE as above, then the stand-in read the client's last commands after the test had stopped it (`sets.size() >= 10`) | PRODUCT + TEST | send flags; the test waits for the stream-off command before stopping the stand-in |
| 5 | test_adif_broadcast | 500 UDP datagrams queued with nobody reading: Linux's default receive buffer holds fewer than 500 small datagrams and the kernel dropped the rest | TEST | the receiver reads in a thread while the sender sends |
| 6 | test_decode_webhook | the POSIX `StubServer::requests()` kept only the Content-Type value where the Windows one keeps the request line and headers | TEST | `tests/http_post_stub.hpp` records the same shape on POSIX |
| 7 | test_webhook | `RawStandIn::stop()` closed a listening socket while a thread sat in `accept()` (does not wake it on Linux), the per-connection `SO_RCVTIMEO` was set on Windows only, and a `send()` had flags 0 | TEST | `shutdown()` before close, the timeout on both platforms, `MSG_NOSIGNAL` (`tests/netmap_stand_in.hpp`) |
| 8 | test_mqtt_publisher | the stress producer's 200 us pause is a 15 ms timer tick on Windows, so 449 of 30000 got through on Linux against the "> 1000" of the Windows pace; and three reads of the stand-in broker's list raced its parser (about one run in three) | TEST | 2 ms between bursts (about 3100 delivered); three bounded waits; 15 runs in a row pass. The product's TCP send also passed flags 0 (row 1's class) and is fixed |
| 9 | test_tcp_fanout | the same 200 us pause: the producer overflowed the GOOD client's 64 KB queue too, so "exactly the lazy one" was dropped with it | TEST | 5 ms between 64 KB bursts |
| 10 | test_power_log | the UDP stand-in's `SO_RCVTIMEO` was Windows-only (a blocking `recv()` for ever, the 120 s timeout) and the TCP stand-in's destructor closed a listener under a blocked `accept()`; the read-only-file case cannot hold for root | TEST | timeout on both, `shutdown()` before close, `SKIP (root)` for that one case (the directory-instead-of-file refusal still runs for root) |
| 11 | test_if_recorder | 38 sleeps of 3 ms are 0.57 s on Windows and 0.11 s on Linux, so a 100 ms checkpoint interval was crossed once, not three times | TEST | 12 ms |
| 12 | test_sigmeas_bandwidth | the 3 dB width is the OUTERMOST crossing of a noisy Welch spectrum and reads about 3 % wide, by an amount that depends on the noise draw; MSVC and libstdc++ draw different noise from one seed | TEST | **the one changed expectation**: the 3 dB bound is 8 % (was 3 %); sweep of 40 seeds in the test comment; 6 dB and 20 dB keep 3 % |
| 13 | test_engine_api_kiwisdr | a hardware tune is paced (one device call per interval); `pump(20)` is 300 ms on Windows and 40 ms on Linux, so the 5 MHz was not yet applied when the radio was saved | TEST | waits for the radio to report the centre |
| 14 | test_engine_api_wire_c_cat | `CatTcpService::start()` answered "built for Windows only so far" off Windows | PRODUCT (missing feature) | the POSIX half of the service written beside the unchanged Windows half (`net/cat_emulation.cpp`) |
| 15 | test_engine_api_hotplug | hot-plug detection is a hidden window receiving `WM_DEVICECHANGE`; off Windows `DeviceArrivalWatcher::start()` and `inject()` return false | WINDOWS-ONLY | test joins `CASCADE_WINDOWS_ONLY_TESTS`; a Linux user does not get the automatic re-detection of a saved radio when its USB interface arrives (row below) |
| 16 | test_engine_api_wire_c_sys | the test stages its import under `%APPDATA%`; off Windows the settings folder is `$XDG_CONFIG_HOME/foxsdr` | TEST | sets `XDG_CONFIG_HOME` too, off Windows |
| 17 | test_engine_main_args, 18 test_engine_main_provision, 19 test_engine_main_smoke | the engine's PortAudio enumeration loads ALSA's PulseAudio plug-in (installed by the CI job list) and libpulse writes `pulse/cookie` into the profile folder the test calls "nothing may be written here" | TEST (harness) | `PULSE_COOKIE` pointed beside the profile in `isolate()`; the program wrote nothing (the tree listings show it) |
| 20 | test_engine_api_source_kinds | `scope.wav`, `cw-decoder.wav`, `twotone-decoder.wav` are git-ignored generated fixtures and a fresh checkout has none | TEST (registration) | three setup tests run the tree's own `make_<name>.py` generators (python -I, standard library, no download); both tests require them |
| 21 | test_file_drop | `block()` built the UTF-16 list with `MultiByteToWideChar`, so off Windows every path was empty | TEST | a hand-written UTF-8 to UTF-16LE conversion off Windows |
| 22 | test_plugin_repo_helper | the fixture named the plugin file `.dll`; the product correctly requires the host's module extension | TEST | `.so` off Windows |
| 23 | test_rig_cat | the baud and stop-bit refusals sat inside the Windows branch of `openSerialByteStream`, so off Windows a bad setting said only "not built on this platform" | PRODUCT (small) | the two platform-neutral refusals now come first. The serial open itself on Linux is NOT written (row below) |
| 24 | test_sys_manifest_tool | `std::system()` returns the wait status on POSIX (exit 2 reads as 512) | TEST | `WEXITSTATUS` off Windows |
| 25 | test_analyser_project | the 40 large SHA-256 comparisons used the Windows CNG as the independent reference and got "" | TEST | OpenSSL's EVP SHA-256 off Windows; the 301 small buffers are compared there too |
| 26 | test_aircraft_out | `strcpy(m.hexIdent, "4840D6X")` copies 8 bytes into `char[7]` | TEST (see below) | `memcpy` of `sizeof(hexIdent)` and a `static_assert` on the size |
| 27-30 | test_airband_data, test_converter_call_sites, test_page_resize_id, test_png_decode | each walks UP from the working directory to find the source tree, and ctest runs it in `<build>/tests` (outside the tree for WSL, CI and `D:/foxsdr-build`; the same four are "known false" on Windows) | TEST (registration) | `WORKING_DIRECTORY` is the source root (two lines, both platforms). Three pass. test_converter_call_sites now finds the tree and reports a REAL finding (open row) |

**`test_aircraft_out`: the overrun is in the TEST.** `SbsMessage::hexIdent` is `char[7]`. The product never copies into it from a longer string: `sbsEncode` only reads it through `upperHex6` (six characters, then `h[6] == '\0'`), and the two places that fill it (`engine_wire_b.cpp:528` and `:539`) use `snprintf(..., sizeof(field), ...)`. Only the test's malformed input wrote 8 bytes (undefined behaviour on every platform; glibc's fortify aborts, the Windows build did not stop it). Nothing in the product needed changing; the check that would have caught it is the `static_assert(sizeof(m.hexIdent) == 7)` beside the `memcpy`, and the case now holds the intended input (seven characters, no terminator).

**The window tests (item 4): the guard recognises an xvfb-run display by itself; the workflow is not changed.** The guard's purpose is "never open a test window on a person's real desktop". An unconditional `FOXSDR_GUARD_DESKTOP_NAME` in the workflow would switch that protection off for anyone who copies the job to a machine with a screen. `linuxDesktopName` (tests/gui_desktop_guard.hpp) now allows only what `xvfb-run` makes (`XAUTHORITY` of the exact shape `<tmp>/xvfb-run.<random>/Xauthority`, which the CI jobs' `xvfb-run -a ctest` sets: checked on this system), refuses a Wayland session (`WAYLAND_DISPLAY`) whatever DISPLAY says, refuses any other X display, and refuses no display. A run on a virtual display nothing here started still names it with `FOXSDR_GUARD_DESKTOP_NAME`, as before. New `test_gui_desktop_name` judges all of that on every platform. Proven on this system: `gui_desktop_guard` allows under `xvfb-run` and refuses with `WAYLAND_DISPLAY` set, with the WSL desktop's `:0`, and with no display; the 76 window tests ran in the full run below with the variable NOT set.

**Break-it checks.** SIGPIPE: before the change four tests died of SIGPIPE (the strace is in the commit message); after, none. CAT: before, the test failed on "built for Windows only so far"; after, it passes. Guard: with `linuxDesktopName` changed to return an allowed name for every display, `test_gui_desktop_name` fails 14 of its 24 checks; restored, 24 of 24 pass. Fixtures and working directory: before, the tests failed on the missing file / the unfound tree (row 20, rows 27-30), after they pass. Load-only races found by the full run: the spyserver gain read-back failed 1 run in 12 under CPU saturation (24 busy loops) and 0 in 20 after the wait; the fan-out storm failed once in a full run and 0 in 15 runs under saturation after the 500 ms settle (its cause is UNVERIFIED: not reproduced before the change).

### Results

**Linux** (`Ubuntu-22.04`, GCC 11.4; headless under `xvfb-run`, `WAYLAND_DISPLAY` unset, `FOXSDR_CI_NULL_AUDIO=1`, null ALSA, `ctest -j3`, **`FOXSDR_GUARD_DESKTOP_NAME` not set**, running as root):

| Run | Registered | Passed | Skipped | Failed |
|---|---|---|---|---|
| start of the lane (section 3) | 1118 | 1088 | 0 | 30 |
| run 1, the 30-red fixes | 1121 (+3 fixture setups, +`test_gui_desktop_name`, -`test_engine_api_hotplug`) | 1111 | 4 | 6: `test_converter_call_sites` (real finding); `test_engine_api_multi_radio` (aborted); `test_pass_control` (on the known load-flake list: `gaps.front() >= 1.95`); `test_spyserver_source` and `test_tcp_fanout` (load races, fixed afterwards); `test_thread_inventory` (the CAT row, fixed afterwards) |
| run 2 | 1123 (+2 fixture setups) | 1116 | 4 | 3: `test_converter_call_sites`; `test_diag_hang` (collided with its `_free` twin in the shared working directory, fixed afterwards by a resource lock); `test_engine_api_multi_radio` (Timeout 120 s) |
| **run 3, the final tree** | 1123 | 1118 | 4 | 1: `test_converter_call_sites` (the real finding, open row 3) |

The four skipped are the same each time and are skips, not failures: `test_pipeline_audio_lead` (a null ALSA device cannot sustain playback), `test_plugin_abi3_compat` (nothing to prove without the published plugin binaries), `test_rtlsdr_live` (real hardware, guarded), `packaging_manifests` (no PowerShell 7 here). The 75 `gui`-labelled window tests plus the `gui_desktop_guard` fixture ran and passed in all three runs with the guard deciding by itself.

Each remaining red alone: `test_converter_call_sites` fails alone (the real finding, open row 3). `test_engine_api_multi_radio` passes alone (12 of 12), under saturation by 24 busy loops (10 of 10), as 6 copies at once (18 of 18) and next to the two tests it started with (4 of 4); it failed in the first minutes of two of three full runs (once aborted, once timed out at 120 s) with no output of its own kept: cause UNVERIFIED, open row 9.

**Windows** (VS 2022 Release, `D:/foxsdr-build/en-hamops`, every test run on the hidden desktop `FoxSDRTestsLR`, nothing on the visible desktop): built `cascade_engine`, `cascade`, `foxsdr-engine`, `foxsdr_manifest_sign`, `gui_desktop_guard` and 51 test targets (every test whose source or shared header I touched, and the engine process tests); build exit 0. ctest over those 54 tests: 52 passed. `test_converter_call_sites` fails for the same four findings as on Linux (it is on the known-false list; it now reaches the findings). `test_engine_api_decode_radio` timed out at 120 s for want of two fixtures (`pocsag-decoder.wav`, `rtty-decoder.wav`, which the first version of the fixture setup did not generate); with them it passes in 33.7 s and the setup now generates all five. After a fresh configure: `test_spyserver_source`, `test_engine_api_decode_radio`, `test_engine_api_source_kinds`, `test_gui_desktop_name` and the five fixture setups, 9 of 9; `test_thread_inventory` and `test_tcp_fanout` 2 of 2. On Windows `test_engine_api_hotplug` still runs and passes (2.8 s), `test_engine_api_wire_c_cat` and `test_cat_emulation` pass on the unchanged Windows half. The last commit (the `RESOURCE_LOCK` pairs, CMake only) was configured on Windows with exit 0; the pairs were not run there.

`%APPDATA%/foxsdr/config.json` sha256 at the end: `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` (unchanged).

### OPEN ROWS FOR THE NEXT LANE

| # | Row | What a Linux user sees / what is not done | Kind of lane |
|---|---|---|---|
| 1 | Hot-plug detection on Linux | `DeviceArrivalWatcher` is Windows-only: a saved radio is not re-opened when its USB interface arrives; the user re-opens it by hand. A Linux watcher listens to udev or netlink uevents and maps USB ids to the radio families; cannot be verified without USB events (none in WSL) | hardware / engine lane with a real radio on Linux |
| 2 | Rig-follower serial port on Linux | `openSerialByteStream` still answers "serial ports are not built on this platform" for a valid setting, so the frequency-following of a rig over a serial cable does not work on Linux (over TCP it does). `core/serial_port.hpp` can open a Linux node; the follower also needs 2 stop bits, its read timeout, and a pty-based test | small build lane (portable code plus a pty test) |
| 3 | `test_converter_call_sites` real finding | four places read the pipeline's raw I/Q file source around the converter: `src/engine/engine.cpp:4952`, `:7415`, `:7427` and `src/engine/wire_a_transport.cpp:16`. Reached now that the test can find the tree; whether it also fails on Windows for this reason is recorded in the Windows results above | engine lane (route through the converter, or add them to the test's allow-list with the reason) |
| 4 | Windows-only features with their tests kept off Linux | ExtIO DLL hosting (`test_extio`), vendor-DLL radios loaded by path (`test_vendor_lib`, `test_g33ddc_source`, `test_g39ddc_source`, `test_sdriq_d2xx`, `test_engine_api_wireb_ftdi`, `test_engine_api_wireb_winradio`, `test_engine_api_wireb_neededlibs`), the Win32 desktop guard (`test_gui_desktop_guard`; the Linux half is `test_gui_desktop_name`), `test_hardware_guard`. A Linux user does not get ExtIO, the D2XX / G33DDC / G39DDC / SDR-IQ / WinRadio bridges, or the "needed library" help for them | hardware lane (Linux `.so` loaders for the vendor libraries the user installs), after the owner decides which matter |
| 5 | The Slint window tests (label `slint`) | not part of this build (section 6) | Slint / build lane |
| 6 | Tests that pass on Windows by the 15 ms timer tick | eight were found here (rows 8, 9, 11, 13 and three more races); others may pass on Linux only by luck. Run the suite 3 times on Linux in CI before trusting one green run | CI lane |
| 7 | `test_power_log` read-only case | skipped when the suite runs as root (this WSL system; a GitHub runner is not root, so it runs there) | none, noted |
| 8 | arm64 | the x64 results above are not claimed for the aarch64 build; the suite was not run under qemu | build lane |
| 9 | `test_engine_api_multi_radio`, intermittent in a full suite | aborted (run 1, 14.9 s) and timed out (run 2, 120 s) in the first minutes of a three-at-a-time run, with a heavy window test beside it; not reproduced in 44 runs alone, under saturation, in parallel copies or beside its start-up neighbours. It has threaded sections (`adder`, `commander`) and both failures look like shutdown; its own output is lost when ctest kills it. A Linux user would see nothing unless the engine itself can abort or hang at shutdown, which is NOT shown | engine / test lane: run it in a loop inside a full `-j3` run with `ctest --output-on-failure` kept, or under TSan |
| 10 | `test_pass_control` | `gaps.front() >= 1.95` failed once in run 1 under load; it is on the standing load-flake list and passed in runs 2 and 3 | none, noted |
| 11 | Parallel runs and shared working directories | a pair of tests that make files by name in `<build>/tests` collide when run together (found: `test_diag_hang` vs `test_diag_hang_free`; the pairs now carry a resource lock). Other tests that write fixed names there and run in parallel have not been looked for | CI lane |
| 12 | A different audio stack under the engine process tests | the PulseAudio cookie fix points `PULSE_COOKIE` beside the profile; a machine whose audio stack writes other files into `$HOME`/`$XDG_*` would still fail the "nothing written" check (not met here) | none, noted |

## 10. The new window on Linux (2026-11-16, lane `linux-slint`, branch `feat/linux-slint` cut from `2edb12c7`, never pushed)

Owner decision 68 (2026-11-16): the Slint toolkit for Linux may be downloaded, and Rust for the Raspberry Pi build. Nothing else was fetched. The tree's Slint version is 1.18.1 (`.github/workflows/build.yml` "The Slint C++ SDK", `SLINT_SDK_URL` .../v1.18.1/Slint-cpp-1.18.1-win64-MSVC-AMD64.exe; `docs/engine-stage4.md` "THE SDK AND ITS LICENCE"; the Windows SDK sits at `C:\Users\steve\slint-cpp-1.18.1`).

### 10.1 Downloads (all inside WSL, into their own folders on `D:\foxsdr-build\linux-slint\`)

| # | What | From | Size (bytes) | Checksum | Licence as its file states it |
|---|---|---|---|---|---|
| 1 | `Slint-cpp-1.18.1-Linux-x86_64.tar.gz` (`dl-x64\`), the x64 toolkit; also supplies the build-time `slint-compiler` for the arm64 build | `https://github.com/slint-ui/slint/releases/download/v1.18.1/` (the project's release page) | 21,621,300 | sha256 `f0b24ea601b80afe241677d1179bf5215dceae529f2b275b678a0af9a144b255` = the digest GitHub's API publishes for that asset: CHECKED, equal | `licenses/LICENSE.md` (3,759 bytes) and `licenses/THIRDPARTY.md` (98,334 bytes), read. The Slint framework is triple-licensed, the user picks: (1) Royalty-free License (proprietary DESKTOP, mobile and web applications at no cost, "as long as you disclose that you use Slint (for example with the `AboutSlint` widget or the Slint badge)"; EMBEDDED systems excluded), (2) GPLv3, (3) a Commercial license (needed for embedded systems regardless of disclosure) |
| 2 | `rustup-init` (`dl-rustup\`), the Rust project's own installer; run once as the build tool it is: `rustup-init -y --profile minimal --default-toolchain stable -t aarch64-unknown-linux-gnu --no-modify-path` in `Ubuntu-24.04` | `https://static.rust-lang.org/rustup/dist/x86_64-unknown-linux-gnu/` | 21,347,088 | sha256 `b1c8cb14e30889e7e2999e6b69e62d9e9b9ffc0f4df8180564d306d9680e6c55` = the `rustup-init.sha256` published beside it: CHECKED, equal. The toolchain components it then fetched (rustc and cargo 1.100.0, 762 MB installed) are verified by rustup itself against the channel manifest; I did not check them separately | `rustup-init` carries no licence file. The installed toolchain's own `share/doc/rust/COPYRIGHT.html` and `licenses/` (Apache-2.0, MIT, BSD-2-Clause, ISC, NCSA, LLVM-exception, GCC-exception-3.1, GPL-2.0-only, GPL-3.0-or-later, CC-BY-SA-4.0 texts) are present; I listed them, I did not read them through |
| 3 | `slint-1.18.1-src.tar.gz` (`dl-slint-src\`), Slint's source at tag v1.18.1, to build the toolkit for aarch64 | `https://github.com/slint-ui/slint/archive/refs/tags/v1.18.1.tar.gz` | 11,529,065 | sha256 `fe485305ed303215e76c04918ee9aefbffbe229f18f979098ec36c7fa1dab28b`. NONE PUBLISHED (a GitHub-generated tag archive): recorded, not checked | `LICENSE.md` inside it has sha256 `d767912198851eaeaabb8390515ae4eb7e2954f8b39e9cbc4711a5b057b26be0`, the same bytes as item 1's |
| 4 | the crates that Slint's own `Cargo.lock` names, from crates.io, by `cargo build --locked -p slint-cpp` (993 `.crate` files, 165 MB in the cargo cache; cargo checks each against the checksum in the lock file) | crates.io | 165 MB | the lock file's per-crate sha256, checked by cargo | per crate. `cargo metadata` over the aarch64 build graph: 717 registry crates, 37 distinct declared licence strings, most common "MIT OR Apache-2.0" 306, "MIT" 175, "Apache-2.0 OR MIT" 68, "Unicode-3.0" 27, "Apache-2.0" 23; no crate declares a GPL-family licence without a permissive alternative |

Same terms as the Windows build relies on? The Windows SDK's `licenses/LICENSE.md` is the same text (identical sha256 `d7679121...` once the CRLF line endings of the Windows copy are removed) and its `THIRDPARTY.md` is byte-identical (sha256 `12b9af0ee309537336882019a2ea4df4608c04237aee135054600cbb6a76e334`). The tree records its choice in `docs/engine-stage4.md` ("THE SDK AND ITS LICENCE", and the `AboutSlint` widget in the brand menu's About dialog, `src/slint/ui/main.slint`), which builds into every build of this window. NOT DECIDED HERE, for the owner: the Royalty-free text covers programs "installed and executed locally" on "a general-purpose computer" and excludes "embedded systems — computer systems that perform a specific task within a larger mechanical or electrical system". Whether a Raspberry Pi that runs FoxSDR as a dedicated receiver counts as one is a licence question; the Windows and x64 desktop cases are the ones the text names.

NOT fetched, because not on the approved list: the same release page also offers `Slint-cpp-1.18.1-Linux-arm64.tar.gz` (15,962,725 bytes, GitHub's digest sha256 `810cfed786c3f63aea9c1b73371a02ed591129d18e077094afa9e024e2d714fd`), a ready-made arm64 toolkit (the section 6 statement "x86-64 only" above was wrong). Using it would skip Rust, the crates and the 2.5 minutes of cargo; it needs the owner's word. Also NOT installed, because they are Ubuntu packages and not on the list: `libxkbcommon-x11-0` (section 10.4: it blocks every window run) and `libinput10` (a stand-in was used, 10.2).

### 10.2 x64, `Ubuntu-22.04` (GCC 11.4, glibc 2.35)

Configure: `cmake -S /root/foxs -B /root/fox-slint-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCASCADE_BUILD_SLINT=ON -DCMAKE_PREFIX_PATH=<sdk>`; the SDK is extracted to `/opt/slint-cpp-1.18.1/Slint-cpp-1.18.1-Linux-x86_64`. `find_package(Slint)` found 1.18.1. The CMake needed no change: the Windows-only branches are guarded. The window's own build is 436 steps; the whole tree (every test) 2484.

Stand-in used to link: the toolkit's `libslint_cpp.so` has a hard `DT_NEEDED` on `libinput.so.10` (its LinuxKMS input), which is not installed and is an Ubuntu package. `/opt/libinput-stub/libinput.so.10` is a stub made by `D:\foxsdr-build\linux-slint\scripts\mkstub.sh` (36 versioned symbols read from the toolkit, each one `abort()`), used ONLY so that `ld` resolves the entry (`-Wl,-rpath-link`) and the program loads. It is not shipped, the window's windowing path (winit on X11 or Wayland) never calls it, and the real package `libinput10` is what a user has.

Portability faults, each proved on both platforms (Windows: MSVC, Release, Ninja, Slint 1.18.1 SDK, `D:\foxsdr-build\linux-slint\win-slint`):

| Commit | Error before (Linux) | Fix | Proof |
|---|---|---|---|
| `ffd86afe` | `src/slint/main.cpp:2260`: `'_get_timezone' was not declared`, `'_get_dstbias' was not declared`, `'localtime_s' was not declared` (the only compile error in the product's 436 steps) | the Windows line is unchanged under `#ifdef _WIN32`; elsewhere `localtime_r` and `tm_gmtoff` (seconds east of UTC, daylight saving included) | Linux: `foxsdr-slint` links, `--version` prints `FoxSDR 0.99.108`. Windows: `main.cpp.obj` of `foxsdr-slint` builds (Windows branch untouched) |
| `034e95e4` | three TEST sources: `test_slint_globe_scene.cpp:277` "reference to 'daylight' is ambiguous" (glibc's global `long daylight`); `test_slint_host_mapj.cpp:29` "namespace access ... redeclared as different kind of entity" (POSIX `access()`); `test_slint_host_transmit.cpp:75` "call of overloaded show(int64_t) is ambiguous" (`int64_t` is `long`) | the call qualified `foxsdr::globe::daylight`; the alias renamed `accessns`; one `show(long)` overload | Linux: all three build; Windows: build, `ctest -L slint -R` the three: 3 of 3 passed |
| `5ac76662` | three tests with Windows paths in their data: `test_slint_host_patch.cpp:3176` got "Recording: C:\rec\a.cu8"; `test_slint_host_tools.cpp:981,983` a `C:/...` import path has no root off Windows; `test_slint_lanem_host.cpp` aborted (`json.exception.parse_error.101`: `sweepHistoryJson` correctly refuses a non-absolute path and the test parsed its empty text) | the path of the host's own form off Windows (`#ifdef _WIN32` keeps the original lines; `FOXTEST_ROOT`); same expectation | Linux: the three pass alone; Windows: build and 3 of 3 passed |

The product's own path rules are the host's (`std::filesystem`): they are right, the test data was Windows data. Expectations changed: none. Checks weakened: none.

glibc ceiling (highest `GLIBC_` / `GLIBCXX_` imported, `objdump -T`; script `scripts/ceil.sh`): `foxsdr-slint` 2.35 / 3.4.30; the toolkit's `libslint_cpp.so` (the downloaded ready-made one) **2.35** / 3.4.26; the downloaded `slint-compiler` 2.35 (build tool, not shipped); `cascade` and `foxsdr-engine` 2.35 / 3.4.30 as before. So the ready-made toolkit meets the 2.35 ceiling and the toolkit was NOT built from source for x64. `foxsdr-slint`'s other `DT_NEEDED`: `libslint_cpp.so`, `libasound.so.2`, `libSoapySDR.so.0.8`, `libssl.so.3`, `libcrypto.so.3`, `libunwind.so.8`, `libOpenGL.so.0` and the C/C++ runtime. The toolkit's: `libxkbcommon`, `libudev`, `libinput.so.10`, `libgbm`, `libfontconfig`, `libfreetype`, `libdrm`, `libwayland-server`, `libxcb-randr` and others, all in the system's own library directory. RUNPATH of `foxsdr-slint` is the absolute SDK folder (`/opt/slint-cpp-1.18.1/.../lib`), which a copied tarball does not have (10.7).

Sizes: the linked `foxsdr-slint` is 1.16 GB because Release carries `-g` and nothing splits it (10.7); `strip` leaves 65,156,304 bytes. `libslint_cpp.so`: 40,097,512 bytes, 34,807,224 stripped.

Tests, headless under `xvfb-run` (the tree's own `gui_desktop_guard` recognises it), `WAYLAND_DISPLAY` unset, `XDG_SESSION_TYPE=x11`, `FOXSDR_CI_NULL_AUDIO=1`, `LIBGL_ALWAYS_SOFTWARE=1` (Mesa llvmpipe; the tree's own softgl is Windows-only), null ALSA, `ctest -j3` (script `scripts/x64-ctest.sh`):

| Run | Registered | Passed | Failed |
|---|---|---|---|
| `-L slint`, before the test fixes | 93 | 90 | 3: test_slint_host_patch, test_slint_host_tools, test_slint_lanem_host (the path data above) |
| `-L slint`, after | 93 | **93** | 0 (57.8 s) |
| `-L drive` (267 scripts of `src/slint/tests/drive/*.txt`, each `slint_drive_<name>` through `test_slint_drive_run`, plus the `gui_desktop_guard` fixture) | 268 | 1 (the guard) | **267, all the same cause: BLOCKED** (10.4) |

### 10.3 What a Linux user of the new window does NOT get, compared with Windows

From the `#if defined(_WIN32)` branches of `src/slint` (each read) and section 9:
- Links never open: every "open this address" (About, help, plugin store docs, the radio-setup help) is a no-op off Windows (`gaps_win.cpp` openUrl returns false; `signal_publish.cpp:404`, `system_publish.cpp:170`, `lanek_publish.cpp:95`).
- "Open the folder" (recordings, logs, the markers folder) does nothing (`system_publish.cpp:186`, `tools_publish.cpp:254`, `lanec_publish.cpp:253,270`: the system's own player or viewer for a recording).
- Copy to the clipboard of the markers does nothing and says "the clipboard is Windows-only here; nothing copied" (`tools_publish.cpp`); the file drop, the clipboard text and the window lookup are `gaps_win.cpp` (Win32 only): dragging a file onto the window opens nothing.
- The test tone has no sound output ("no sound output on this platform", `lanek_audio.cpp`).
- No update installer: the update card never offers one (`host_duties.cpp:662`); a Linux user updates by the tarball or AppImage.
- Section 9's open rows apply to this window too: no automatic re-detection of a saved radio when its USB interface arrives; the rig follower over a serial cable; ExtIO and the vendor-DLL radio bridges.
- Rendering: Windows runs the SDK's default, which `screen-s0.md` infers to be Skia. The aarch64 toolkit built here has FemtoVG and the software renderer, no Skia (10.5). The x64 toolkit has Skia, FemtoVG and the software renderer.
- NOT verified (blocked, 10.4): what the window looks like and how it responds to a pointer and a keyboard on Linux at all.

### 10.4 The blocker: `libxkbcommon-x11.so` (an Ubuntu package that is not on the approved list)

Every window run aborts in the toolkit's winit X11 backend, on x64 and on aarch64 alike, before the first frame:

    thread '<unnamed>' panicked at .../xkbcommon-dl-0.4.2/src/x11.rs:59:28:
    Library libxkbcommon-x11.so could not be loaded.
    Aborted (core dumped)        (exit 134)

`libxkbcommon-x11.so.0` is in the Ubuntu package `libxkbcommon-x11-0` (22.04: 1.4.0-1), which neither system has (the workflow's package list does not name it either: `libxkbcommon-dev` does not pull it in). `SLINT_BACKEND=testing` answers "Could not load rendering backend testing, fallback to default", and no Wayland compositor is installed, so there is no second route. Not installed here (outside decision 68), and no stand-in written for it (a stub that makes the X11 setup succeed would only hide the keyboard path under test). With the package the 267 drive scripts and a bounded `--frames N` run are the next step; the same package is a RUNTIME dependency of any machine that runs the window under X11. Whether a Wayland session needs it is UNVERIFIED (winit's Wayland backend uses plain `libxkbcommon`, which is installed).

### 10.5 arm64, `Ubuntu-24.04` (GCC 13.3 cross, sysroot `/opt/arm64-sysroot`, qemu-user 8.2.2)

The toolkit was built from the source tarball at the same version with cargo (Rust 1.100.0; Slint needs 1.92 or later), not through Slint's CMake: its `api/cpp/CMakeLists.txt` fetches Corrosion from GitHub (`FetchContent`), which is not on the list. `scripts/arm-slint-cargo.sh` runs `cargo build --release --locked --target aarch64-unknown-linux-gnu -p slint-cpp -p i-slint-common --no-default-features --features "std,interpreter,backend-winit,renderer-femtovg,renderer-software,accessibility,system-tray,testing,i-slint-common/fontconfig-dlopen"` (2 m 26 s; `fontconfig-dlopen` because the sysroot has no `fontconfig.pc`). Features differ from the x64 toolkit's on purpose: no Skia (its build script would fetch prebuilt Skia binaries), no live-preview, no LinuxKMS libinput. `scripts/arm-sdk.sh` assembles the toolkit layout `/opt/slint-arm64-sdk` (the x64 SDK's CMake files and headers, the generated headers and the aarch64 `libslint_cpp.so` of this build, and the x64 `slint-compiler` as `bin/slint-compiler-host-x86_64`, which runs on the build machine). The window: `cmake -S /root/foxs -B /root/fox-slint-arm -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=/opt/arm64-toolchain.cmake -DCMAKE_FIND_ROOT_PATH="/opt/arm64-sysroot;/opt/slint-arm64-sdk" -DCASCADE_BUILD_SLINT=ON -DSlint_DIR=/opt/slint-arm64-sdk/lib/cmake/Slint -DSLINT_COMPILER=/opt/slint-arm64-sdk/bin/slint-compiler-host-x86_64` (`-DCMAKE_PREFIX_PATH` did not find the toolkit under a find-root-path; `Slint_DIR` does). 436 steps, no failure, no source change needed beyond the three commits.

| Output | Type | Highest import | Ceiling 2.39 |
|---|---|---|---|
| `foxsdr-slint` | ELF aarch64 PIE, 1,059,224,712 bytes unstripped | GLIBC_2.38 (`__isoc23_strtol`, `fmod` ...), GLIBCXX_3.4.32 | met |
| `libslint_cpp.so` | ELF aarch64 shared, from source | GLIBC_2.39 (`pidfd_spawnp`, `pidfd_getpid`), no libstdc++ | met, exactly at the ceiling |

Its `DT_NEEDED`: `libslint_cpp.so`, `libasound`, `libSoapySDR`, `libssl`, `libcrypto`, `libunwind`, `libOpenGL`, C/C++ runtime; the toolkit needs only `libgcc_s`, `libm`, `libc` (everything else, X11, Wayland, fontconfig, xkbcommon, it opens at run time).
Smoke under `qemu-aarch64 -L /opt/arm64-sysroot` with `LD_LIBRARY_PATH` at the toolkit: `foxsdr-slint --version` prints `FoxSDR 0.99.108`, exit 0. `--frames 0` boots the engine side ("program: slint (foxsdr-slint 0.99.108)", settings, diagnostics lines) and then aborts in the windowing backend with the same `libxkbcommon-x11.so` panic as 10.4; `Ubuntu-24.04` has no `xvfb`, so a virtual display was not attempted. The suite was not run under emulation. What a real Pi still has to prove: that the window opens and draws (FemtoVG or software renderer, no Skia), the frame rate, pointer and keyboard input, that the glibc ceiling of 2.39 is a floor Raspberry Pi OS meets (a Debian 12 base is glibc 2.36: UNVERIFIED from memory, the previous lane's arm64 `cascade` already needed 2.38), and the licence question of 10.1.

### 10.6 Repeating each build in one command

Sources: an LF export of the tip (`git -c core.autocrlf=false -c core.eol=lf archive --format=tar -o <tar> HEAD`, extracted to `/root/foxs`); all scripts are in `D:\foxsdr-build\linux-slint\scripts\` (run with `wsl -d <system> -u root -- bash /mnt/d/foxsdr-build/linux-slint/scripts/<name>`; a `$` inside a `bash -c '...'` string is expanded by the first shell, so use script files):
- x64 build: `x64-build.sh foxsdr-slint` (after `mkstub.sh` and with `-DCMAKE_EXE_LINKER_FLAGS=-Wl,-rpath-link,/opt/libinput-stub`); tests: `x64-ctest.sh <log> -L slint` (or `-L drive`); one test: `x64-one.sh <name>`; ceilings: `ceil.sh <file>...`.
- arm64: `arm-slint-cargo.sh`, `arm-sdk.sh`, `arm-build.sh foxsdr-slint`, `arm-smoke.sh`, `arm-ceil.sh <file>...`.
- Windows check: `win-obj.bat <ninja target>...` (Ninja under `vcvars64`, `D:\foxsdr-build\linux-slint\win-slint`, `CASCADE_SOFTGL=OFF`).
A WSL gotcha found: a process started in the background from a `wsl.exe` call dies when that call ends (`nohup` and `setsid` do not help), so a long build is run as the foreground command of its own `wsl.exe`.

### 10.7 Packaging hand-off for the release lane (written, NOT built)

`.github/workflows/build.yml` job `linux` (x64, runner `ubuntu-latest`) and job `arm64` (runner `ubuntu-24.04-arm`) build `cascade` and `foxsdr-engine` only; the tarball step copies those two plus `resources/`. To carry the new window:
1. Toolkit in CI. x64: a step that downloads `Slint-cpp-1.18.1-Linux-x86_64.tar.gz` from the URL of 10.1 and checks it against the pinned sha256 `f0b24ea6...b255` (the Windows job pins its installer the same way), extracts it, and configures with `-DCASCADE_BUILD_SLINT=ON -DCMAKE_PREFIX_PATH=<extract dir>`. arm64: either the owner approves the ready-made `Slint-cpp-1.18.1-Linux-arm64.tar.gz` (sha256 `810cfed7...b08`, 10.1) or the job builds from source with cargo as in 10.5 (CMake route needs Corrosion, which the job could install from the distro or pin).
2. The glibc ceiling: the `linux` job runs on `ubuntu-latest` (24.04 now: glibc 2.39). The 2.35 ceiling holds only on a 22.04 builder (`runs-on: ubuntu-22.04`). Add a check to `tools/check-linux-tarball.sh`: highest `GLIBC_` imported by every ELF in the tarball is at most 2.35 (x64) or 2.39 (arm64).
3. Build-time packages: nothing more to compile against (`libxkbcommon-dev`, `libwayland-dev`, `libx11-dev` are already in the list), but `libinput.so.10`, `libudev`, `libgbm`, `libfontconfig`, `libfreetype`, `libdrm` must be present to LINK (the toolkit's `DT_NEEDED`): `libinput-dev` or `libinput10`; whether the runner image already has them is UNVERIFIED.
4. Tarball (`Package` step, `tools/check-linux-tarball.sh`): add `build/src/slint/foxsdr-slint` and the toolkit's `lib/libslint_cpp.so` (x64: `<sdk>/lib/libslint_cpp.so`; arm64: the cargo output) to `foxsdr-<ver>-linux-<arch>/`, with `licenses/LICENSE.md` and `licenses/THIRDPARTY.md` of the toolkit (for example under `licenses/slint/`: the Royalty-free licence requires the disclosure the window already shows, and the third-party notices travel with the library). The check script's member list and its `--version` run need `foxsdr-slint` and `libslint_cpp.so` resolving from beside it.
5. RUNPATH: the linked `foxsdr-slint` carries the absolute toolkit folder. The tarball needs `$ORIGIN` (or `$ORIGIN/lib`): link with `-Wl,-rpath,'$ORIGIN'` (a `BUILD_RPATH` or link option on `foxsdr-slint` in `src/slint/CMakeLists.txt`, a one-line source change this lane did not make) or run `patchelf` in the step. Until then the tarball's launcher must set `LD_LIBRARY_PATH` to its own folder.
6. Symbols and size: `foxsdr-slint` is 1.16 GB (x64) with `-g` and no step splits it; `cascade` and `foxsdr-engine` have a POST_BUILD `tools/archive-symbols-linux.sh` in the root `CMakeLists.txt` (their symbols go to `symbols/` and the shipped binary is stripped). Add the same step for `foxsdr-slint` (module name `foxsdr-slint`, a line in the symbol archive check `tools/check-symbol-archive-linux.sh`) or the tarball carries 1.16 GB (65 MB stripped).
7. AppImage (`installer/linux/build-appimage.sh`, `AppRun`, `foxsdr.desktop`): `AppRun` today is `cd usr/bin && exec ./cascade "$@"`. Copy `foxsdr-slint` to `usr/bin/`, `libslint_cpp.so` to `usr/lib/` (RUNPATH `$ORIGIN/../lib`), the licences to `usr/share/licenses/foxsdr/slint/`. THE LAUNCHER'S CHOICE (mirror of the Windows installer's `SlintIsApp`, default the new window, `cascade.exe` kept as the classic one): `AppRun` runs `foxsdr-slint` unless `FOXSDR_WINDOW=legacy` or the first argument is `--legacy` (removed before the exec), or there is neither `DISPLAY` nor `WAYLAND_DISPLAY`, or `foxsdr-slint --version` fails (a missing library), in which case it says why on stderr and runs `./cascade`. Every other argument passes through. The `.desktop` file can stay `Exec=cascade` only if the AppImage's own `AppRun` is what `Exec` reaches; two entries ("FoxSDR" and "FoxSDR (Legacy)") are the clear alternative. The tarball gets a small `foxsdr` launcher script with the same rule.
8. `Verify AppImage runs` runs `xvfb-run -a "$appimage" --frames 60` for the old window; the new window needs `libxkbcommon-x11-0` on the runner (10.4) for the same step and for `ctest -L drive`. Add `ctest -L slint` and `ctest -L drive` to the `Test` step once that package is present.
9. Runtime dependencies to state in the release notes (user machines): `libxkbcommon-x11-0` (X11 sessions), `libinput10`, `libudev1`, `libgbm1`, `libfontconfig1`, `libfreetype6`, `libdrm2`, a working OpenGL (Mesa) or `SLINT_BACKEND=winit-software`.

### OPEN ROWS FOR THE NEXT LANE

| # | Row | What is not done / what a Linux user sees | Kind of lane |
|---|---|---|---|
| 1 | Install `libxkbcommon-x11-0` (and `libinput10`) in `Ubuntu-22.04` (and the arm64 equivalent) | needs the owner's word (Ubuntu packages, outside decision 68). Without it no window run is possible | owner decision, then a one-command install |
| 2 | Run the 267 drive scripts under `xvfb-run` and classify every red | blocked by row 1; none has run | Slint / build lane |
| 3 | Real-pointer and keyboard pass of the new window on Linux (X11 and Wayland) | not started; whether Wayland needs the X11 library is UNVERIFIED | Slint lane with a desktop session |
| 4 | Linux gaps of the new window (10.3): open a link, open a folder, copy to the clipboard, file drop, test tone | each is a no-op on Linux | Slint lane (portable code: `xdg-open`, a clipboard, a drop target, an ALSA tone) |
| 5 | Owner: the arm64 toolkit route (ready-made `Slint-cpp-1.18.1-Linux-arm64.tar.gz` or cargo from source in CI) | only the cargo route was done | owner decision |
| 6 | Owner: Royalty-free licence on a Raspberry Pi used as a dedicated receiver | not decided here (10.1) | owner / legal |
| 7 | Packaging hand-off (10.7): `$ORIGIN` RUNPATH, `foxsdr-slint` symbols and strip, tarball and AppImage steps, launcher, ceiling check | written, not built | release lane |
| 8 | Raspberry Pi OS glibc: the arm64 toolkit imports GLIBC_2.39 and the application 2.38 | whether the Pi OS releases the owner means have them is UNVERIFIED | owner / build lane (a build on an older base) |
| 9 | Real Pi: window opens, draws with FemtoVG or software, frame rate, input | only `--version` ran, under emulation | hardware lane |
| 10 | Slint features on arm64 differ from Windows (no Skia): pictures and speed may differ | not compared | Slint lane |
