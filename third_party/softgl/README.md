# softgl — the software OpenGL renderer (Windows)

Two files of a prebuilt Mesa 3D, shipped in `softgl\` beside `cascade.exe` in
the Windows packages (installer and MSIX). They are a complete OpenGL 4.6 drawn
by the processor (Mesa's **llvmpipe**), used only when the graphics driver
cannot be: no OpenGL driver at all (the 0.99.89 field log, "WGL: The driver
does not appear to support OpenGL"), or one older than OpenGL 3.0, or
`FOXSDR_GL=software`. How and when: `src/gui/software_gl.hpp` and
`src/gui/window_platform.hpp`.

Unlike everything else in `third_party/`, the binaries are **not in the
repository** (62 MB). `PINNED.txt` pins them instead, and CMake
(`CASCADE_SOFTGL`, on by default on Windows) downloads the archive once per
build tree, checks it and each extracted file against those hashes, and stages
the two files in `build\Release\softgl\`. A mismatch is a configure error, so a
different download can never be staged, packaged or shipped.
`-DCASCADE_SOFTGL_ARCHIVE=<file>` uses a copy already on disk (no network).

## What is pinned

| | |
|---|---|
| Upstream | Mesa 3D, https://mesa3d.org, as built by **mesa-dist-win** (https://github.com/pal1000/mesa-dist-win) |
| Release | `26.2.4` (published 2026-10-04), MSVC build |
| Archive | `https://github.com/pal1000/mesa-dist-win/releases/download/26.2.4/mesa3d-26.2.4-release-msvc.7z`, 70,257,286 bytes, SHA256 `351fc8c8b695878ffb3eaa044b3ead08672a48b1a045e3c3e3975811df0f6695` |
| `x64/opengl32.dll` | 139,264 bytes, SHA256 `75b295fc80df0eaa8f1264c28663f6932c8b72d36513eb092556608858b7f5e0` — Mesa's WGL front end; imports `libgallium_wgl.dll`, `GDI32.dll`, `KERNEL32.dll` |
| `x64/libgallium_wgl.dll` | 61,868,032 bytes, SHA256 `53e49adf87590d5b0cab8553b1e98084812040cc1fca8c487695b17b7af3702e` — the Gallium megadriver (llvmpipe, LLVM 23.1.2 linked in); imports only Windows system DLLs, no C runtime |
| Renderer string | `llvmpipe (LLVM 23.1.2, 256 bits)`, `4.6 (Compatibility Profile) Mesa 26.2.4 (git-9a0169f2a7)` (read back by FoxSDR's `window: GL vendor` log line) |
| Signature | none: both files are unsigned (`Get-AuthenticodeSignature`: NotSigned) |

The rest of the archive (x86, Vulkan, OpenCL, video and D3D12 pieces,
deployment scripts) is not extracted or shipped. `GALLIUM_DRIVER=llvmpipe` is
set before the load unless the environment names a driver, because Mesa's
default where a Direct3D 12 driver exists is its D3D12 back end, which needs a
`dxil.dll` that is not shipped.

Measured on the release desktop (i9-14900K, 1,200 frames of the signal
generator at 1280x720, 0.99.89): about 85 frames a second on llvmpipe and 2.6 s
of processor over 14.4 s, against the display's 120 Hz and 0.7–1.5 s on the
GPU; 39 frames a second with `LP_NUM_THREADS=2`. No slow frames in any run.

## Licences

The archive carries **no licence or notice files** (only a `readme.txt` that
points at the online manual), so the texts are taken from the upstream
sources at the matching revisions and kept here verbatim in `licenses/`:

| File | Covers | Source |
|---|---|---|
| `Mesa-license.rst` | Mesa's own statement: core MIT, per-file SPDX identifiers | `docs/license.rst` at tag `mesa-26.2.4` (gitlab.freedesktop.org/mesa/mesa) |
| `Mesa-MIT.txt`, `Mesa-Apache-2.0.txt`, `Mesa-BSL-1.0.txt` | licences Mesa's tree uses | `licenses/` at tag `mesa-26.2.4` |
| `Mesa-SGI-B-2.0.txt`, `Mesa-GPL-2.0-only.txt`, `Mesa-GPL-1.0-or-later.txt` | the rest of `licenses/` at that tag; kept for the record, NOT shipped (see below) | same |
| `Mesa-SoftFloat-3e-LICENSE.txt` | Berkeley SoftFloat 3e (BSD-3-Clause), in `src/util/softfloat.c` | the file's own header, `mesa-26.2.4.tar.xz`, comment markers removed |
| `Mesa-xxHash-LICENSE.txt` | xxHash (BSD-2-Clause), `src/util/xxhash.h`; the DLL carries XXH code | the file's own header, same tarball |
| `LLVM-LICENSE.TXT` | LLVM, statically linked into `libgallium_wgl.dll`: Apache-2.0 WITH LLVM-exception (and the legacy UIUC terms) | `llvm/LICENSE.TXT` at tag `llvmorg-23.1.2` |
| `zlib-LICENSE.txt` | zlib 1.3.2, in mesa-dist-win's build environment; the DLL carries Mesa's "failed to compress data (zlib)" path | `LICENSE` at tag `v1.3.2` (github.com/madler/zlib) |
| `mesa-dist-win-LICENSE.txt` | the mesa-dist-win build (MIT, pal1000) | `LICENSE` at tag `26.2.4` |

`installer/THIRD-PARTY-LICENSES.txt`, the aggregate notice both packages ship,
reproduces the Mesa MIT, Apache-2.0 and BSL-1.0 texts, the SoftFloat and xxHash
notices, LLVM's licence, zlib's and mesa-dist-win's.

**Traced 2026-10-25** against the official `mesa-26.2.4.tar.xz`
(archive.mesa3d.org, SHA256
`bce5f7fbebb934373b86c999a064d52fb5065878dc57f287f95346648ec832e9`). Every file
whose SPDX tag names a GPL or SGI-B licence is outside a Windows WGL build:
`src/glx/` (SGI-B-2.0, the X11 GLX client), `include/drm-uapi/` (Linux kernel
UAPI headers, GPL WITH Linux-syscall-note; one Rockchip NPU header plain
GPL-2.0), and `src/freedreno/vulkan/msm_kgsl.h`. The VMware `svga` headers are
"GPL-2.0 OR MIT", taken here under MIT. In the directories such a build
compiles (`src/gallium/{drivers/llvmpipe,drivers/softpipe,drivers/d3d12,
drivers/zink,drivers/svga,auxiliary,frontends/wgl,targets/wgl,winsys/sw,include}`,
`src/mesa`, `src/util`, `src/compiler`, `src/c11`, `src/microsoft`) there is no
GPL or SGI text at all; the identifiers are MIT (336 files), Apache-2.0 (41,
Khronos headers), BSL-1.0 (2, `src/c11`), and the licence texts that are not
MIT are BSL-1.0 (`gallivm/f.cpp`, `src/c11`), public-domain dedications
(`fast_idiv_by_const.c`, `rand_xor.c`, `gpuvis_trace_utils.h`), Berkeley
SoftFloat's BSD-3-Clause and xxHash's BSD-2-Clause - the two that require their
notice with a binary, now shipped. The GPL and SGI-B texts in `licenses/` here
are therefore not reproduced in the shipped notice. zstd was looked for and is
not in the DLL (LLVM's own message says it was built without it).

## Updating

1. Pick a mesa-dist-win release; download its `-release-msvc.7z`.
2. Put the archive's SHA256 and the two extracted `x64/` files' SHA256s in
   `PINNED.txt` (and its URL and version).
3. Re-fetch the licence texts at the new Mesa and LLVM tags into `licenses/`
   and update `installer/THIRD-PARTY-LICENSES.txt` and the table above.
4. Reconfigure (CMake re-downloads on a hash change) and run
   `test_software_gl_app` and `test_window_failed_app`.
