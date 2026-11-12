# FoxSDR plugin SDK template

A minimal plugin project that builds against nothing but FoxSDR's `plugin_abi.h` (one MIT-licensed C header). It holds one
working decoder plugin, **Heartbeat Decoder**, which says `heartbeat N` once for every second of audio it is fed, a test
that drives it the way the host does, and the steps to build it, load it and ship it.

```
sdk/
  CMakeLists.txt             builds the plugin module and its test; finds plugin_abi.h (see below)
  src/heartbeat_decoder.cpp  the plugin: copy it, rename it, replace decoderProcess()
  tests/heartbeat_test.cpp   calls the exported entry point and the decoder table as the host does
  tests/scan_sinks.cmake     fails the build if the plugin writes to stdout/stderr or to the host's settings
  tests/host_load_check.ps1  loads the built module with FoxSDR's own plugin host (needs a FoxSDR build tree)
```

The contract is [`src/core/plugin_abi.h`](../src/core/plugin_abi.h) and its guide
[`docs/PLUGIN-API.md`](../docs/PLUGIN-API.md). Where this README and those disagree, they are right; please report the
README.

## 1. Build

You need CMake 3.16 or newer and a C++17 compiler (Visual Studio 2022 on Windows; GCC or Clang on Linux). Nothing is
downloaded.

```
cmake -S sdk -B build-sdk -A x64          # Windows; elsewhere: cmake -S sdk -B build-sdk
cmake --build build-sdk --config Release
ctest --test-dir build-sdk -C Release --output-on-failure
```

The module is `build-sdk/Release/heartbeat-decoder-1.0.0-abi3-win-x64.dll` (`.so` on Linux, with `linux-x64` or
`linux-arm64` in the name; `.dylib` on macOS). Only the Windows build has been run for this template; the Linux and macOS
name rules and flags are written from the same CMake file and are **unverified**.

`plugin_abi.h` is found, in this order, in `-DFOXSDR_PLUGIN_ABI_DIR=<folder>`, in `sdk/include/` (copy the header there when
your project lives outside the FoxSDR tree), then in `../src/core/` (this folder inside the FoxSDR source tree). A plugin
never links against FoxSDR and never includes anything else of its tree.

## 2. File name

The host reads a plugin's identity from its file name as well as from its descriptor:

`<id>-<version>-abi<N>-<os>-<arch>.<dll|so|dylib>`, for example `heartbeat-decoder-1.0.0-abi3-win-x64.dll`.

A file name that declares **another** plugin ABI than the host's is refused before any of its code runs. The plugin ABI is
**3** (`CASCADE_PLUGIN_ABI_VERSION` in the header; docs/PLUGIN-API.md: "The plugin ABI is still 3"). Two files with the same
`<id>` are one plugin to the host's stop list, saved presets and dormancy clock, whatever their descriptors say, so change
`PLUGIN_ID` in `CMakeLists.txt` and the descriptor's name when you copy this template.

## 3. Install and run

Copy the module into FoxSDR's `plugins` folder and restart FoxSDR (the host scans it at start). The host uses ONE of two
folders (`PluginHost::defaultPluginDir`, `choosePluginDir` in `src/core/plugin_host.cpp`):

* `<FoxSDR install folder>\plugins\`, when the program can write there (a portable copy, or a per-user install); both
  FoxSDR windows read the same folder, and the installer puts no plugin in it;
* otherwise `%LOCALAPPDATA%\foxsdr\plugins\`. A machine-wide install under Program Files is not writable for an ordinary
  user, and a Microsoft Store (MSIX) install always uses this one.

From the build folder, `cmake --install build-sdk --config Release --prefix "<that folder>"` copies the module there.

In FoxSDR the plugin appears in the plugin list as **Heartbeat Decoder 1.0.0**. A decoder with text output is shown in the
shared Decoder output window while the receiver runs; Heartbeat Decoder prints `heartbeat 1`, `heartbeat 2`, ... once per
second of audio. `cascade.exe --safe-plugins` starts with no plugin loaded, which is the first thing to try if a plugin
misbehaves.

To prove the module loads in FoxSDR's own plugin host without installing it, run `tests/host_load_check.ps1` against a FoxSDR
build folder that has the `test_plugin_abi3_compat` test built; it copies only your module into a scratch folder and runs that
test with `FOXSDR_PUBLISHED_PLUGINS_DIR` pointing at it (the test isolates `APPDATA`/`LOCALAPPDATA` itself).

## 4. Host API levels

The plugin ABI (the descriptor and the capability tables) is 3 and has not changed. What grows is the **host API**, a table
of functions the host hands a plugin that declares `CASCADE_CAP_HOST_CLIENT` (docs/PLUGIN-API.md, "Getting the table, and
asking what it offers"):

| Level | FoxSDR | What it added |
|---|---|---|
| 1 | 0.99.31 | The host table: read the whole receiver, ask to change it, mark the spectrum, keep settings, talk to the user, offer keys, process the audio the user hears. |
| 2 | 0.99.74 | One capability: hand the host **audio to transmit** (see "Transmitting (host API level 2)" in the guide before writing a transmitting plugin). `CASCADE_HOST_API_LEVEL` is 2 in this header. |

This template declares only `CASCADE_CAP_DECODER`, which needs no host table, so it runs on every host that speaks ABI 3. A
plugin that uses the table must, before calling any level-1 function:

```cpp
if (CASCADE_HOST_HAS(host, get_state)) { /* covered by the table's structSize AND non-NULL */ }
```

and say plainly what it cannot do on an older host instead of going quiet. Branch on `CASCADE_HOST_HAS` and `apiLevel`,
never on the version text. The rest of the rules, from the guide:

* `process()` and the audio-out and transmit-audio `pull()` calls run on the real-time thread and are not guarded: no
  blocking, no allocation, no I/O, no `settings_*` calls there. A fault there ends the process with a report that names the
  plugin. Every other call the host makes into your tables is guarded: a fault or an exception switches the plugin off for
  the session.
* Keep the host's table pointer in an atomic; `attach()` may run again and must be idempotent.
* A plugin that handles identifiers of people or devices never prints to stdout or stderr (the host captures stderr into its
  log and uploaded reports) and never puts an identifier into a setting (settings are written to the config file).
  `tests/scan_sinks.cmake` is the check this template ships for that.
* A request the host refuses with `CASCADE_API_DENIED` should tell the user which grant would help.

## 5. Making it yours

1. Copy `sdk/` out of the FoxSDR tree, put `plugin_abi.h` in `include/`.
2. Change `PLUGIN_ID` and `PROJECT_VERSION` in `CMakeLists.txt`, and the name, version, author and licence strings in the
   descriptor (the version string and `PROJECT_VERSION` must agree: the test checks it).
3. Replace `decoderProcess()` with your decoder; keep its callbacks free of allocation and I/O.
4. Change the test to your signals. Feed it noise, a frequency offset and a sample-rate error, as `heartbeat_test.cpp` does,
   and keep one test that fails when the behaviour is removed.
5. State your licence honestly in the descriptor. The host displays it verbatim.

## Licence

This template is MIT-licensed, as is `plugin_abi.h`.
