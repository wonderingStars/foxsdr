# Building the Windows installer

Produces `installer\Output\foxsdr-setup-<version>.exe` — a
self-contained Inno Setup 6 installer for FoxSDR.

## Prerequisites

1. **A completed Release build** at `build\Release\` (see the repo
   [README](../README.md) for the CMake commands). The payload contract is
   `cascade.exe` + `SoapySDR.dll` and nothing else — the `.iss` enumerates
   `build\Release\*.dll` at compile time and **aborts** if any other DLL
   appears, so a new runtime dependency must be added to
   `installer\cascade.iss` deliberately.
2. **Inno Setup 6** — `ISCC.exe`, typically at
   `C:\Program Files (x86)\Inno Setup 6\ISCC.exe` or
   `%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe`.
3. **VS 2022 Build Tools** (already required to build the app): the installer
   stages the app-local MSVC runtime (`msvcp140.dll`, `vcruntime140.dll`,
   `vcruntime140_1.dll`) from the VC Redist folder
   (`...\VC\Redist\MSVC\<ver>\x64\Microsoft.VC143.CRT`) — never from
   `C:\Windows`. If your VS version directory differs from the default pinned
   in the `.iss`, override it on the command line (see below).

> **Before you ship anything built this way, read the symbols note below.** An
> installer compiled by hand carries a binary whose PDB exists on exactly one
> disk. `tools/build-nightly.ps1` mirrors symbols off-machine as part of the
> same step; `ISCC` on its own does not.

> **Check the tree is clean first.** `build-nightly.ps1` appends `.dirty` to
> the version string when the tree has uncommitted changes; a hand-run `ISCC`
> does not, so the version on the installer is indistinguishable from a
> released one. The binary's own `commit:` field does carry the marker — every
> build from a modified tree reports `<sha>-dirty` (see
> `docs/DIAGNOSTICS.md`), so a crash report from it can at least be recognised
> as coming from a tree nobody can check out. That is a way to notice the
> mistake afterwards, not a licence to ship one: commit first.

## Build

From the repo root:

```
"%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" installer\cascade.iss
```

With a different VC redist location:

```
ISCC.exe /DVcCrtDir="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Redist\MSVC\14.44.35112\x64\Microsoft.VC143.CRT" installer\cascade.iss
```

Output: `installer\Output\foxsdr-setup-0.8.1.exe`.

## Version bumps

The version is defined **once**, at the top of `installer\cascade.iss`
(`#define AppVersion`). Keep it in lockstep with `project(cascade VERSION ...)`
in `CMakeLists.txt`; the output filename follows automatically.

## Install behaviour

* Default: machine-wide to `{autopf}\FoxSDR` (elevated). The
  install-dir leaf avoids `+` on purpose; the display/app name keeps it.
* Per-user, no elevation:
  `setup.exe /CURRENTUSER` (add `/VERYSILENT /SUPPRESSMSGBOXES /DIR="..." /NOICONS`
  for scripted installs).
* Uninstall: `unins000.exe /VERYSILENT` from the install dir (or Apps &
  Features).

## Symbols — the step this file cannot do for you

The moment an installer exists, so does a binary that can reach a user, and
every crash report that binary will ever produce is unreadable without the PDB
from **that exact link**. A PDB cannot be regenerated: rebuilding the same
source produces a different CodeView GUID and different offsets.

The CMake build already archives each link into `symbols\`, keyed by build id
(`tools/archive-symbols.ps1`). That directory is gitignored and lives on one
disk. The durable copy is pushed to `nas:/volume1/foxsdr-symbols` by
`tools/build-nightly.ps1`, **in the same step that compiles the installer** —
which is exactly why a hand-run `ISCC` is not equivalent.

So: build shippable installers with `tools/build-nightly.ps1`, or mirror the
symbols yourself before the binary leaves the machine. Detail and the reading
procedure are in [../docs/DIAGNOSTICS.md](../docs/DIAGNOSTICS.md).

## What is deliberately NOT bundled

* **SoapySDR vendor modules / UHD** — hardware support is the user's choice of
  PothosSDR or radioconda, matching the system SoapySDR ABI story documented
  in `third_party/THIRD_PARTY.md`. `POSTINSTALL.txt` (shown by the wizard and
  installed next to the exe) explains this to end users.
* **A system-wide VC++ redistributable install** — the three runtime DLLs are
  app-local instead (mirrors the mayhem-b200 installer's approach), which
  keeps `/CURRENTUSER` installs elevation-free. The UCRT the binaries also
  need is an OS component on Windows 10+.
