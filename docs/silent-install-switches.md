# Silent install, silent uninstall and the program's command line

This page is written for the manual to include. Every line says where it comes from. Three levels of proof are used and
never mixed:

* **RUN** - this project has run it and recorded the result (`docs/engine-stage4.md` 6.38.5, `tools/release.ps1` step
  `install-test`, `docs/release-notes-0.99.80.md`).
* **SCRIPT** - read from `installer/cascade.iss` (the setup's source), not run by this page's author.
* **INNO** - a switch that belongs to Inno Setup itself. The Inno Setup help (`jrsoftware.org/ishelp`) could not be opened
  when this page was written (the host refused the connection and its mirror refused too), so these are **not checked
  here**; check them in the help that ships with Inno Setup 6 (`ISetup.chm`) before the manual states them as fact.

The setup is `foxsdr-setup-<version>.exe`, built by Inno Setup 6 (`OutputBaseFilename=foxsdr-setup-{#AppVersion}`).
It is **not code-signed**, so Windows SmartScreen may stop an interactive double-click; a silent run from a script is
not asked.

## Install

| Switch | What it does | Proof |
|---|---|---|
| `/VERYSILENT` | No wizard, no progress window. | RUN: `tools/release.ps1` runs the setup with it; exit 0 |
| `/SILENT` | No wizard, a progress window. | RUN: `docs/engine-stage4.md` 6.38.5 ("exit 0, Installation process succeeded"); the setup itself uses it to remove the other-scope copy (SCRIPT) |
| `/SUPPRESSMSGBOXES` | Message boxes are answered with their default, so nothing waits for a click. Always give it with a silent switch in a script. | RUN: `tools/release.ps1` |
| `/NORESTART` | Never restart Windows. The setup needs no restart; this is the safe form. | RUN: `tools/release.ps1` |
| `/CURRENTUSER` | Install for the current user only, no elevation; `{autopf}` then resolves under `%LOCALAPPDATA%` (the script's own comment; the exact subfolder was not checked here). Allowed because the script sets `PrivilegesRequiredOverridesAllowed=commandline`. | RUN: `tools/release.ps1`, 6.38.5 |
| `/ALLUSERS` | Install for every user, with elevation (the script's default is `PrivilegesRequired=admin`, `DefaultDirName={autopf}\FoxSDR`, so a plain run asks for administrator rights and installs under Program Files). | RUN: 6.38.5 reinstall |
| `/DIR="<folder>"` | The install folder. **Quote it when it contains a space.** | RUN: 6.38.5 (an unquoted `C:\Program Files\FoxSDR` installed into `C:\Program\` with exit 0: check `InstallLocation`) |
| `/TASKS="desktopicon"` | Selects exactly the listed tasks. The script has two: `desktopicon` (the Desktop shortcut, on by default) and `cleaninstall` (off by default). | RUN: 6.38.5 reinstall used `/TASKS=desktopicon` |
| `/LOG="<file>"` | Writes Setup's log, which the release script reads to prove the install. | RUN: `tools/release.ps1` (`/LOG=<path>`) |
| `/NOICONS` | No Start Menu folder. | INNO (named in `installer/README-installer.md`, not run) |
| `/CLEANINSTALL=1` | **FoxSDR's own switch.** Deletes the existing FoxSDR settings, plugins, caches and logs (`%APPDATA%\foxsdr` and `%LOCALAPPDATA%\FoxSDR`) BEFORE the files go down. Recordings are never touched. Off unless given. Write it with `=1`: the script compares the value with `1`. | SCRIPT: `CurStepChanged`; `docs/MANUAL.md` says the same |
| `/DATAROOTS="<folder>"` | **A test hook, not for users.** Moves both data folders under `<folder>` (`Roaming\foxsdr`, `Local\FoxSDR`) so a clean install or uninstall can be proved without touching a real profile. | SCRIPT |

What a silent install does and does not do:

* The "Launch FoxSDR now" entry is `skipifsilent`, so a silent install never starts the program (SCRIPT).
* The setup silently runs the **other scope's** uninstaller first when it finds a FoxSDR there (a per-user install
  removes a machine-wide one and the reverse), with `/SILENT /NORESTART /SUPPRESSMSGBOXES`, and carries on if that fails
  (SCRIPT, `PrepareToInstall`). Settings are untouched by it. Do not run a test install on a machine whose real FoxSDR
  you want to keep without knowing this.
* A running FoxSDR can stop an install: the 0.99.80 release run's first attempt stopped because receiver processes
  could not be closed (RUN, `docs/release-notes-0.99.80.md`). Close it first.
* An upgrade over an existing install keeps the settings; only `/CLEANINSTALL=1` (or the ticked task) clears them.
* The AppId is `B3D4A7E2-6C51-4F8B-9A0D-2E7F5C8B1A64`, so the uninstall key is
  `Software\Microsoft\Windows\CurrentVersion\Uninstall\{B3D4A7E2-6C51-4F8B-9A0D-2E7F5C8B1A64}_is1` in `HKLM` (all users) or
  `HKCU` (current user); `UninstallDisplayName` is `FoxSDR <version>` (SCRIPT, RUN in 6.38.5's inventory).
* The `foxsdr:` link handler is registered under `HKA` (machine or user, matching the scope) and removed on uninstall
  (SCRIPT, RUN in 6.38.5).

Examples (PowerShell; give each argument as its own array item so a folder with a space survives):

```powershell
# for the current user, no elevation, into a folder of your choice
$p = Start-Process -FilePath .\foxsdr-setup-0.99.108.exe -Wait -PassThru -ArgumentList @(
    '/CURRENTUSER', '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/DIR="D:\Apps\FoxSDR"', '/LOG="D:\Apps\foxsdr-install.log"')
$p.ExitCode   # 0 is success (RUN); other codes are Inno's (see the Inno help)

# for every user (an elevation prompt appears unless the shell is already elevated)
Start-Process -FilePath .\foxsdr-setup-0.99.108.exe -Wait -Verb RunAs -ArgumentList @(
    '/ALLUSERS', '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART')
```

## Uninstall

The uninstaller is `unins000.exe` in the install folder (`UninstallString` in the registry key above is its quoted path).

| Switch | What it does | Proof |
|---|---|---|
| `/VERYSILENT`, `/SILENT`, `/SUPPRESSMSGBOXES`, `/NORESTART`, `/LOG="<file>"` | As for the setup. | RUN: `tools/release.ps1` uninstall; 6.38.5 |
| `/CLEANDATA=1` | **FoxSDR's own.** Also deletes `%APPDATA%\foxsdr` and `%LOCALAPPDATA%\FoxSDR` without asking. Write it with `=1`. | SCRIPT (`CurUninstallStepChanged`); `docs/MANUAL.md` |
| `/KEEPDATA=1` | **FoxSDR's own.** Keeps them without asking (checked before `/CLEANDATA`, so it wins if both are given). | SCRIPT |

* A **silent uninstall keeps the data** unless `/CLEANDATA=1` is given - on purpose, because an upgrade runs exactly that
  command (SCRIPT; RUN: "silent and no /CLEANDATA; the settings stay"). A user-installed plugins folder therefore
  survives a silent uninstall (`tools/release.ps1` reports it).
* An interactive uninstall asks "Also remove your FoxSDR settings and data?" and prints both folders; recordings
  (`Documents\SDR-recordings`) are never touched by setup or uninstall (SCRIPT).
* The data folders are only removed if their last path component is exactly `foxsdr` or `FoxSDR` and the path is at least
  8 characters (SCRIPT, `DeleteDataTree`).
* A per-user uninstall of a machine-wide install, or the reverse, is not what these switches do; use the uninstaller of the
  scope that was installed.

## Package managers

`packaging/winget`, `packaging/scoop` and `packaging/chocolatey` carry manifests for the same setup file;
`tools/make-package-manifests.ps1` fills them from a release setup. winget and Chocolatey run the setup silently with the
switches above (winget picks them itself for an Inno setup; Chocolatey's script passes `/VERYSILENT /SUPPRESSMSGBOXES
/NORESTART`). Scoop does not run Setup at all: it unpacks the files, so the `foxsdr:` handler is not registered.

## The program's own command line

Both programs (`cascade.exe`, the Legacy window, and `foxsdr-slint.exe`, the new window) and `foxsdr-engine` read their
arguments in `src/engine/process_boot.cpp`. This table is read from that file (SCRIPT-level proof only; meanings come from
the code's own comments). **Window** = the windowed programs, **engine** = `foxsdr-engine` only, **all** = both.

| Argument | Who | What it does |
|---|---|---|
| `--version` | all | Prints one line, `FoxSDR <version>`, and exits 0. `tools/release.ps1` reads it back (RUN). |
| `--selftest` | all | Runs the built-in self test. `tools/release.ps1` runs it after installing (RUN). |
| `--frames <N>` | window | Runs N frames and exits; N must be 0 to 1000000, anything else exits 1. Used by the install test (RUN). |
| `--seconds <N>` | engine | A bounded run of N seconds. |
| `--config <file>` | engine | Use this settings file; the engine reads, owns and saves it. A value that starts with `--` is refused as a forgotten path, and giving it twice is refused. |
| `--use-config` | engine | Use the per-user settings file the window application uses (one shared file behind a lock). With neither, the engine uses no settings file. |
| `--safe-plugins` | all | Start with no plugin loaded (`docs/PLUGIN-API.md`). |
| `--update-check [download]` | all | Asks the update endpoint what this build should be told, and with `download` also downloads and verifies the installer without running it. **Makes a network call.** |
| `--rtlsdr-check`, `--soapy-check`, `--record-check`, `--rds-check <MHz>`, `--tone-check` | all | Bench diagnostics; some need attached hardware. Hidden from the usage text on purpose. |
| `--web-enable`, `--web-bind <addr>`, `--web-port <n>`, `--web-user <name>`, `--web-password-file <file>`, `--cat-enable`, `--cat-bind-all` | engine | Provision the web and CAT servers for a headless run; the bind rules in `src/net/web_policy.hpp` still apply (a non-loopback bind without a password is refused). |
| anything else | all | A first argument starting with `foxsdr:` is a link activation, not a flag. An unknown argument prints `unknown argument '...'` with the usage line and exits 1 (`foxsdr-engine: ...` or `cascade: ...`). |

`--sentinel`, `--enumerate-json`, `--list-drivers`, `--sdrplay-service-state`, `--sdrplay-probe`, `--diag-stall`,
`--diag-toggle` and `--link-tester` are internal support tools; read their comments in `process_boot.cpp` before
documenting them.
