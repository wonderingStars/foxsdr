# SDR-+ AI Supreme Leader

A from-scratch, MIT-licensed SDR receiver for Windows and Linux: spectrum +
waterfall, multi-mode demodulation, and hardware support through SoapySDR.
Built clean-room — no GPL code, no GPL-linked dependencies — so the whole tree
stays MIT.

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

## License

MIT — see [LICENSE](LICENSE). Dependencies are deliberately permissive
(Dear ImGui MIT, GLFW Zlib, PortAudio MIT-like, nlohmann/json MIT, pffft
BSD-style, SoapySDR BSL-1.0); FFTW and librtlsdr are intentionally not used.

SDR-+ AI Supreme Leader is an independent project, not affiliated with or
endorsed by SDR++ or its authors.
