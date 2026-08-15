# FoxSDR application icon

![The FoxSDR mark at 256 px](foxsdr-256.png)

## The mark

An **angular fox head with a spectrum trace cut into its brow**, on a dark
slate tile.

The head is a single flat polygon: two triangular ears rising from a shallow
notch, straight outer edges running down into wide cheeks, and a taper to a
pointed muzzle at the bottom. No outline, no fur, no eyes drawn as such — a
literal fox is unreadable below 32 px and looks like clip art above it, so the
mark is a silhouette with exactly one piece of internal detail.

That detail is the radio half: four vertical bars, sharing a baseline and
varying in height like a signal peak, **cut out of the head in the tile colour**
so they read as negative space rather than as painted-on marks. Two of them
also sit where a fox's eyes would be, which is the point — the mark reads as a
fox at a glance and as a spectrum on a second look.

### Why 16 and 24 px are different

They deliberately use **two equal bars instead of four**. Four bars plus three
gaps across the ~7 px of face available at 16 px works out under one pixel per
bar; it renders as a grey smear and destroys the silhouette. At two bars the
minimum feature is 2.3 px, comfortably above the 2 px floor, and the mark still
reads — as a fox face with eyes. Every size from 32 px up carries the full
four-bar trace (minimum bar width 2.6 px at 32 px).

This per-size simplification is normal practice for a multi-resolution icon and
is implemented in `generate_icon.py` (`SMALL_VARIANT_MAX_PX`).

### Palette

| Role | Hex | Notes |
| --- | --- | --- |
| Fox orange | `#E8722C` | The mark. Warm, saturated, distinct from the amber the band-plan overlay already uses in-app. |
| Slate | `#1B1F26` | The tile, and the colour the spectrum bars are cut in. Matches the app's dark theme. |

Two flat fills only. No gradients (they average to mud at 16 px), no text, no
stroke thinner than 2 px at 32 px. Contrast between the two is ~4.5:1, which is
what keeps the silhouette crisp on both light and dark taskbars.

## Files

| File | What it is |
| --- | --- |
| `generate_icon.py` | The generator. The single source of truth for the geometry. |
| `foxsdr.ico` | Multi-resolution icon: 16, 24, 32, 48, 64, 128 (32-bit BGRA DIB) and 256 (PNG). Used by `foxsdr.rc` and by the installer. |
| `foxsdr-256.png` | 256 px preview, so the mark can be seen without opening a `.ico`. Not shipped. |
| `foxsdr_icon_rgba.hpp` | Generated raw RGBA pixel arrays (16/32/48) for `glfwSetWindowIcon`. Included by `src/gui/app_window.cpp`. |
| `foxsdr.rc` | Windows resource script declaring the icon as `IDI_ICON1`. |

## Regenerating

```
py -3.14 -m pip install pillow
py -3.14 resources\icon\generate_icon.py
```

It rewrites `foxsdr.ico`, `foxsdr-256.png` and `foxsdr_icon_rgba.hpp` in place
and prints their sizes. Nothing in the build invokes it — the three artefacts
are checked in, and Python is not a build dependency. The script exists so the
icon is reproducible and editable rather than an opaque binary.

## Where the icon is applied

Three independent places, all three needed:

1. **The executable's own icon** (Explorer, taskbar pin, Alt-Tab, shortcut
   targets) — `foxsdr.rc`, compiled into `cascade.exe`. Requires the CMake
   change below.
2. **The runtime window icon** (title bar and the live taskbar button) —
   `glfwSetWindowIcon` in `AppWindow::run`, fed from `foxsdr_icon_rgba.hpp`.
   This is a *separate* Win32 property from the resource icon; setting only the
   resource icon still leaves the running window showing GLFW's default.
3. **The installer** — `SetupIconFile` in `installer/cascade.iss` gives
   `foxsdr-setup-<version>.exe` the same mark. `UninstallDisplayIcon` already
   points at `{app}\cascade.exe`, so Add/Remove Programs picks the icon up from
   the executable automatically once (1) is in place.

`IDI_ICON1` is numeric and equal to `1` on purpose: Explorer and the taskbar
display the **lowest-numbered** `RT_GROUP_ICON` in an executable. If further
icon resources are ever added, the application icon must keep the lowest id.

## Required CMakeLists.txt change

The `.rc` is not yet compiled into the executable — this is the one change that
must be made by hand. In the top-level `CMakeLists.txt`, in the
`# Application` section, replace:

```cmake
add_executable(cascade src/main.cpp)
```

with:

```cmake
# Windows executable icon (Explorer, taskbar, Alt-Tab). The .rc must be a
# SOURCE of the executable target — MSVC compiles it with rc.exe and the
# linker embeds the RT_GROUP_ICON — and only of the executable: cascade_lib
# and the test binaries neither need it nor should carry it. The runtime
# window icon is set separately in AppWindow::run; see resources/icon/README.md.
if(WIN32)
    enable_language(RC)
endif()

add_executable(cascade
    src/main.cpp
    $<$<BOOL:${WIN32}>:${CMAKE_CURRENT_SOURCE_DIR}/resources/icon/foxsdr.rc>)
```

Nothing else changes: `target_link_libraries(cascade PRIVATE cascade_lib)` and
the `/DELAYLOAD` block below it stay exactly as they are.

Those lines were verified against CMake 3.20+ with the Visual Studio 17 2022
generator: the resulting executable carries `RT_GROUP_ICON` id 1 and
`RT_ICON` ids 1–7, and `Icon.ExtractAssociatedIcon` returns the mark. The
`enable_language(RC)` guard is what keeps the same lines working under the
Ninja and Makefile generators, where RC is not enabled implicitly.
