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

## What is deliberately NOT bundled

* **SoapySDR vendor modules / UHD** — hardware support is the user's choice of
  PothosSDR or radioconda, matching the system SoapySDR ABI story documented
  in `third_party/THIRD_PARTY.md`. `POSTINSTALL.txt` (shown by the wizard and
  installed next to the exe) explains this to end users.
* **A system-wide VC++ redistributable install** — the three runtime DLLs are
  app-local instead (mirrors the mayhem-b200 installer's approach), which
  keeps `/CURRENTUSER` installs elevation-free. The UCRT the binaries also
  need is an OS component on Windows 10+.
