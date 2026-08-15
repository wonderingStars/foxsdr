# FoxSDR

A from-scratch software-defined radio receiver for Windows: spectrum and
waterfall, multi-mode demodulation (NFM/WFM/AM/DSB/USB/LSB/CW), stereo FM with
RDS, recording, bookmarks, a scanner, band plans, and hardware support for any
radio SoapySDR can reach.

Built clean-room — no GPL code and no GPL-linked dependencies anywhere in the
tree. That discipline is what leaves the licensing free to choose (see
[License](#license)); it is not itself a licence claim.

Internal project/binary name: `cascade`.

## Status

Early development. See [PLAN.md](PLAN.md) for the roadmap (P0–P6) and the
architecture. Currently: GUI shell, FFT/spectrum estimator, and the core DSP
primitives (SPSC ring, FIR design + decimation, NCO/mixer, quadrature
discriminator, AGC) — each with a unit-test suite verified by independent
adversarial review.

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

## Installer

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
[COMMERCIAL-LICENSE.md](COMMERCIAL-LICENSE.md).

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
