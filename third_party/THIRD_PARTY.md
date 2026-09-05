# Vendored third-party libraries

Every library the application links is vendored here, pinned to the exact
revision the app was built and tested against, so an upstream change can never
break or alter this build. Sources are verbatim upstream copies — **no vendored
file has been modified**. Each subdirectory keeps its upstream license file.

Those license files, plus SoapySDR's, are reproduced verbatim in
`installer/THIRD-PARTY-LICENSES.txt`, the aggregate notice shipped with the
installer and referenced by `COMMERCIAL-LICENSE.md` and
`COMMERCIAL-AGREEMENT.md`. **Update it whenever a pinned revision changes** — a
commercial licence defines the third-party carve-out by reference to it, so it
must list what the build actually contains.

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
| Dear ImGui | https://github.com/ocornut/imgui | tag `v1.92.8-docking` | `https://github.com/ocornut/imgui/archive/v1.92.8-docking.tar.gz` | `ca0653454ed371b7a87e9b0bc29a5d15c9be7f7c0fbe778042fc48c71df1d3d8` | MIT (`imgui/LICENSE.txt`) | `third_party/imgui/` |
| GLFW | https://github.com/glfw/glfw | tag `3.4` | `https://github.com/glfw/glfw/archive/3.4.tar.gz` | `c038d34200234d071fae9345bc455e4a8f2f544ab60150765d7704e08f3dac01` | Zlib (`glfw/LICENSE.md`) | `third_party/glfw/` |
| PortAudio | https://github.com/PortAudio/portaudio | commit `147dd722548358763a8b649b3e4b41dfffbcfbb6` (v19.7 line — the exact commit the vcpkg `portaudio` 19.7#9 port pins, not the older `v19.7.0` tag) | `https://github.com/PortAudio/portaudio/archive/147dd722548358763a8b649b3e4b41dfffbcfbb6.tar.gz` | `95457b809ce60d4d4790f84bb692e271f644e59d8adf96feb988c89ab52a506a` | PortAudio license (MIT-like, `portaudio/LICENSE.txt`) | `third_party/portaudio/` |
| nlohmann/json | https://github.com/nlohmann/json | tag `v3.12.0` (release asset `json.hpp` = `single_include/nlohmann/json.hpp`) | `https://github.com/nlohmann/json/releases/download/v3.12.0/json.hpp` | `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63` | MIT (`nlohmann_json/LICENSE.MIT`) | `third_party/nlohmann_json/` |
| pffft | https://bitbucket.org/jpommier/pffft | tag `v1.0.0` (bitbucket changeset `02fe7715a5bf`) — the exact revision the vcpkg `pffft` 1.0.0 port pins | `https://bitbucket.org/jpommier/pffft/get/v1.0.0.tar.gz` | `9adeb18ac7bb52e9fb921c31c0c6a4e9ae150cc6fcb20a899d4b3a2275176ded` | FFTPACK/BSD-style (`pffft/LICENSE.txt`) | `third_party/pffft/` |
| cpp-httplib | https://github.com/yhirose/cpp-httplib | tag `v0.53.1` (single header `httplib.h`) | `https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.53.1/httplib.h` | `bc69d53636a8757cb24a1deb9880bf7e2fdae3a80bbc759e145b8c80913cbfa3` | MIT (`cpp_httplib/LICENSE`) | `third_party/cpp_httplib/` |
| Saira Condensed | https://github.com/google/fonts | commit `eda91bff215c766697fcbbcf836a6425e5c167ac` (`ofl/sairacondensed/`, Medium and SemiBold statics) | `https://raw.githubusercontent.com/google/fonts/eda91bff215c766697fcbbcf836a6425e5c167ac/ofl/sairacondensed/SairaCondensed-{Medium,SemiBold}.ttf` | Medium `a02d8fe45b8b7d952cb0dd341683b02ddc1b55dbd0ed89d9d438868be614b66f`, SemiBold `30f8ed4d078211003a9715c80c51ce031bab5c9a17e8771182e4c4599205634b` | SIL OFL 1.1, reserved name "Saira" (`fonts/OFL-SairaCondensed.txt`) | `third_party/fonts/` |
| Nova Mono | https://github.com/google/fonts | commit `90abd17b4f97671435798b6147b698aa9087612f` (`ofl/novamono/`) | `https://raw.githubusercontent.com/google/fonts/90abd17b4f97671435798b6147b698aa9087612f/ofl/novamono/NovaMono.ttf` | `648eadb6648c0801b186d3dcef60ee6aa84a791b1e09c726935c0712508b4807` | SIL OFL 1.1, reserved name "NovaMono" (`fonts/OFL-NovaMono.txt`) | `third_party/fonts/` |

Integrity cross-check: the SHA512 of each fetched tar.gz was compared against
the SHA512 recorded in the corresponding vcpkg portfile
(`C:/vcpkg/ports/<port>/portfile.cmake`) and matched byte-for-byte for
glfw3, portaudio, and pffft (imgui was checked this way while it was pinned to
the master-branch `v1.92.8`; see the note below on why the docking tag it now
carries has no vcpkg counterpart to compare against) — i.e. these are provably the same sources vcpkg
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

**The `-docking` tag, not plain `v1.92.8`.** The map window and each decoded
image are real operating system windows rather than panels penned inside the
application, and multi-viewport support is the only supported way to do that:
it exists solely on upstream's `docking` branch. The alternative considered and
rejected was creating extra GLFW windows by hand with one Dear ImGui context
each — the vendored GLFW backend says in its own source that multi-context
support "is not well tested and probably dysfunctional", so that route means
fighting the backend for the same result.

`v1.92.8-docking` is the docking-branch counterpart of the exact tag that was
already pinned, so this changes the branch and not the version: same upstream
repository, same MIT licence, same 1.92-era API, no change to which translation
units are compiled or which files are vendored. The integrity cross-check
against vcpkg's portfile no longer applies to this row — vcpkg packages the
master branch, so there is nothing on that side to compare a docking archive
against. The SHA256 above is of the archive actually fetched.

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

### cpp-httplib (`third_party/cpp_httplib/`)
Single header only, at `include/httplib.h`, exposed through the
`cascade_httplib` INTERFACE target (which also carries `ws2_32` on Windows,
since the header's socket layer needs it). It provides the HTTP/1.1 server
behind web server mode. The `0.53.1` version was confirmed from the
`CPPHTTPLIB_VERSION` macro inside the fetched header, not from the URL alone.

Fetched as the raw header at the tag rather than as a release archive, so — as
with nlohmann/json — there is no vcpkg portfile hash to cross-check it against;
the SHA256 in the table above is of the header actually vendored.

**TLS is deliberately not enabled.** `CPPHTTPLIB_OPENSSL_SUPPORT` is never
defined, so no OpenSSL is required, linked, or shipped. Two consequences that
belong with the pin rather than buried in the server code:

- the server speaks plain HTTP, so a password crossing a network it serves is
  in clear — which is why the bind policy distinguishes loopback from anything
  else, and why remote access is documented as terminating TLS in front of the
  application (a reverse proxy or a tunnel) rather than as opening its port;
- the application already reaches the network as a *client* through WinHTTP,
  which uses the operating system's trust store. Enabling OpenSSL here would
  put a second TLS stack, with its own certificate store and its own patch
  cadence, into a product that needs neither.

Verified by `tests/test_httplib_vendor.cpp`, which binds a server to loopback
on an OS-chosen port and makes real requests through it (11 checks). That test
exists because a vendored header can be present and correctly hashed while
still failing to compile under `/W4 /permissive-`, missing an import library,
or having no working socket layer on this platform — none of which is visible
from the file on disk.

### Saira Condensed and Nova Mono (`third_party/fonts/`)
The application's three typefaces — Saira Condensed Medium and SemiBold for
everything a hand operates, Nova Mono for counter digits — named by the design
handoff and compiled into the binary by `tools/embed-fonts.py`. See
`src/gui/fonts.hpp` for which role each one fills and why the monospaced face
is restricted to digits.

**These are unmodified upstream files and must stay that way.** Both families
carry a Reserved Font Name under the SIL Open Font License — "Saira" and
"NovaMono" — and OFL clause 3 forbids a *Modified Version* from using the
reserved name. Subsetting them to the ASCII this application actually draws
would be such a modification, so it would require renaming the font in its own
name table. Bundling the originals verbatim is explicitly permitted by the
licence, needs no rename, and costs about 490 KB in the executable. That trade
was made deliberately; anyone tempted to shrink it by subsetting must rename
the result first.

Google Fonts is the upstream of record rather than each family's own
repository, because that is where the OFL text shipped alongside these exact
binaries lives. The pinned commits are the most recent ones touching those
directories — both families' static instances have been unchanged there for
years — and the raw files served at those commits were confirmed
byte-identical to what is vendored here.
