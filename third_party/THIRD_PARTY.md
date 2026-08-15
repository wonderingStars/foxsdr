# Vendored third-party libraries

Every library the application links is vendored here, pinned to the exact
revision the app was built and tested against, so an upstream change can never
break or alter this build. Sources are verbatim upstream copies — **no vendored
file has been modified**. Each subdirectory keeps its upstream license file.

The one deliberate exception is **SoapySDR**, which is *not* vendored and is
still consumed via `find_package(SoapySDR CONFIG REQUIRED)` from vcpkg:
SoapySDR is the hardware ABI boundary. Vendor device modules (SoapyUHD for the
B200, SoapyRTLSDR, …) are separate binaries loaded by SoapySDR at runtime, and
they must match the ABI of the *system* SoapySDR they were built against.
Vendoring our own SoapySDR would produce a second, private ABI that the
installed vendor modules were never built for — breaking exactly the hardware
support SoapySDR exists to provide. It therefore stays external by design.

## Pinned versions

| Library | Upstream | Tag / commit | Archive fetched | Archive SHA256 | License | Local dir |
|---|---|---|---|---|---|---|
| Dear ImGui | https://github.com/ocornut/imgui | tag `v1.92.8` | `https://github.com/ocornut/imgui/archive/v1.92.8.tar.gz` | `fecb33d33930e12ff53a34064e9d3a06c8f7c3e04408f14cd36c80e3faac863b` | MIT (`imgui/LICENSE.txt`) | `third_party/imgui/` |
| GLFW | https://github.com/glfw/glfw | tag `3.4` | `https://github.com/glfw/glfw/archive/3.4.tar.gz` | `c038d34200234d071fae9345bc455e4a8f2f544ab60150765d7704e08f3dac01` | Zlib (`glfw/LICENSE.md`) | `third_party/glfw/` |
| PortAudio | https://github.com/PortAudio/portaudio | commit `147dd722548358763a8b649b3e4b41dfffbcfbb6` (v19.7 line — the exact commit the vcpkg `portaudio` 19.7#9 port pins, not the older `v19.7.0` tag) | `https://github.com/PortAudio/portaudio/archive/147dd722548358763a8b649b3e4b41dfffbcfbb6.tar.gz` | `95457b809ce60d4d4790f84bb692e271f644e59d8adf96feb988c89ab52a506a` | PortAudio license (MIT-like, `portaudio/LICENSE.txt`) | `third_party/portaudio/` |
| nlohmann/json | https://github.com/nlohmann/json | tag `v3.12.0` (release asset `json.hpp` = `single_include/nlohmann/json.hpp`) | `https://github.com/nlohmann/json/releases/download/v3.12.0/json.hpp` | `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63` | MIT (`nlohmann_json/LICENSE.MIT`) | `third_party/nlohmann_json/` |
| pffft | https://bitbucket.org/jpommier/pffft | tag `v1.0.0` (bitbucket changeset `02fe7715a5bf`) — the exact revision the vcpkg `pffft` 1.0.0 port pins | `https://bitbucket.org/jpommier/pffft/get/v1.0.0.tar.gz` | `9adeb18ac7bb52e9fb921c31c0c6a4e9ae150cc6fcb20a899d4b3a2275176ded` | FFTPACK/BSD-style (`pffft/LICENSE.txt`) | `third_party/pffft/` |

Integrity cross-check: the SHA512 of each fetched tar.gz was compared against
the SHA512 recorded in the corresponding vcpkg portfile
(`C:/vcpkg/ports/<port>/portfile.cmake`) and matched byte-for-byte for imgui,
glfw3, portaudio, and pffft — i.e. these are provably the same sources vcpkg
built the previous binaries from. (nlohmann/json was fetched as the upstream
release asset, whose 3.12.0 version macros were verified inside the header;
vcpkg fetches the full repo archive instead, so its hash is not comparable.)

## Per-library notes

### Dear ImGui (`third_party/imgui/`)
Vendored subset per spec: the core sources (`imgui*.cpp/h`, `imconfig.h`,
`imstb_*.h`), the two backends the app uses
(`backends/imgui_impl_glfw.*`, `backends/imgui_impl_opengl3.*` +
`imgui_impl_opengl3_loader.h`), `misc/cpp/imgui_stdlib.*`, and `LICENSE.txt`.
`examples/`, `docs/`, fonts and the other backends are not vendored.
Built as the static target `imgui` (root `CMakeLists.txt`), compiling the same
translation units the vcpkg port compiled (core five + `imgui_stdlib.cpp` +
the two backends). The app uses the 1.92-era API (two-argument `PushFont`),
which pins the 1.92.8 tag.

### GLFW (`third_party/glfw/`)
Full pristine 3.4 tree. Built via `add_subdirectory` with
`GLFW_BUILD_DOCS/TESTS/EXAMPLES=OFF`, `GLFW_INSTALL=OFF`. Static library
(vcpkg's x64-windows build was a DLL; static linkage is a deliberate
deployment simplification — same code, same version).

### PortAudio (`third_party/portaudio/`)
Full pristine tree at the pinned commit. Built via `add_subdirectory`, static
(`portaudio_static` target). **Host APIs compiled on Windows: WASAPI and WMME
only.** ASIO is OFF (requires the proprietary Steinberg SDK), DirectSound is
OFF, WDM/KS is OFF (including `PA_USE_WDMKS_DEVICE_INFO`). Note: the vcpkg
port carried a patch (`fix-guid-linker-errors.patch`) that only touches
`src/hostapi/wdmks/pa_win_wdmks.c`; with WDM/KS disabled that file is not
compiled, so the unpatched upstream source builds identically. Its
`cmake_minimum_required(VERSION 2.8)` predates CMake 4's policy floor, so the
root `CMakeLists.txt` sets `CMAKE_POLICY_VERSION_MINIMUM=3.5` around the
`add_subdirectory` (build-system accommodation, not a source change).

### nlohmann/json (`third_party/nlohmann_json/`)
Single header only, at `include/nlohmann/json.hpp`, exposed through the
`cascade_json` INTERFACE target. The vcpkg 3.12.0#2 port additionally carried
two post-release patches (`fix-4736_char8_t.patch`,
`fix-4742_std_optional.patch`) affecting only json⇄`std::filesystem::path`
and json⇄`std::optional` conversions; cascade converts neither type to or
from json, so the pristine upstream 3.12.0 header is behaviorally identical
for this codebase (verified by the full test suite).

### pffft (`third_party/pffft/`)
Full pristine tree (6 files) from the bitbucket `v1.0.0` tag. `LICENSE.txt`
is the one added file: upstream ships no standalone license file, so it
reproduces the `pffft.h` header license verbatim (see its preamble). Built as
the static target `cascade_pffft` compiling `pffft.c` only, mirroring the
vcpkg port's own CMakeLists (which also compiled just `pffft.c`, with
`_USE_MATH_DEFINES` on MSVC and no `/arch` flag — on MSVC x64, pffft
auto-selects its SSE path via `_M_X64` and SSE2 is implicit in x64). The vcpkg
port's `fix-invalid-command.patch` alters only the `#warning` directive inside
the scalar-fallback preprocessor branch, which is a skipped group on x64
MSVC — the unpatched source compiles to identical code. The app includes
`<pffft/pffft.h>`, which resolves because the target exports `third_party/`
itself as the include root.
