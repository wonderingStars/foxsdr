# Packaging lane: winget, Scoop and Chocolatey manifests, the plugin SDK template, the OpenAPI file, the silent-switch list

Date 2026-11-11/12. Worktree `D:\foxsdr-wt\en-packaging`, branch `feat/packaging` cut from engine/multi-radio d3630d78.
Build folder `D:\foxsdr-build\en-packaging` (configured `-DCASCADE_BUILD_SLINT=OFF -DCASCADE_SOFTGL=OFF`, the other arguments
from `D:/foxsdr-build/feat-web/CMakeCache.txt`). Nothing pushed, merged, submitted, published, signed, uploaded or installed.
No version changed (no versioned file was touched): versions old to new, none.

## What was asked, and what is done

| Row (REMAINING-GAPS.md, "Setup, settings and install") | State |
|---|---|
| winget, Scoop and Chocolatey packages | **BUILT**: templates under `packaging/`, one generator `tools/make-package-manifests.ps1`, tests, break-it. **Publishing is not done** (it needs an account on each service): owner steps with the exact commands are in `packaging/README.md`. |
| Third-party plugin SDK: template project, SDK package, OpenAPI file | **Template project BUILT** (`sdk/`, built and loaded by the host's own test). **OpenAPI BUILT** (`docs/api/openapi.yaml`, 3.0.3). **An SDK archive for download is NOT built**: how and where to ship it (release asset, plugin repository, website) is an owner decision below. |
| Code-signed setup | **Not this lane's** (the `sysmodels` lane owns the signing script). The manifests were written to work either way: nothing in them depends on a signature (winget's installer file, Scoop and Chocolatey carry no signature field). Nothing was signed, no signing tool was run. |
| Silent-install switches / user command line | **BUILT** as `docs/silent-install-switches.md`, a new file for the manual lane to include (each switch marked RUN, SCRIPT or INNO-not-checked). `docs/MANUAL.md` is not edited. |

## Step 0: what the official pages say (reading only)

Every page below was opened on 2026-11-11 (the fetch tool summarised each page; where a page had too little, that is said).
Licences: `microsoft/winget-pkgs` is MIT and `ScoopInstaller/Scoop` is Unlicense or MIT (both LICENSE files opened);
the Microsoft Learn, Chocolatey and Inno Setup pages are vendor documentation. No source code of any of them was read and
nothing was downloaded: the Scoop schema was read for its field names and a schema of my own was written from it
(`tests/packaging/scoop-manifest.schema.json`), not copied.

### winget (Windows Package Manager)

* https://learn.microsoft.com/en-us/windows/package-manager/package/manifest (ms.date 2026-09-14, updated 2026-09-15):
  the schema in use is **ManifestVersion 1.12.0** (the page links `manifest.singleton.1.12.0.json` and the 1.12.0 version,
  defaultLocale, installer and locale schemas). Singleton form only for one installer and one locale; otherwise a multiple-file
  manifest of **at least three YAML files**: version, default locale, installer (additional locale files optional). Required
  in the minimal schema: `PackageIdentifier` (Publisher.Package), `PackageVersion`, `PackageLocale` (BCP 47), `Publisher`,
  `PackageName`, `License`, `ShortDescription`, `Installers` with `Architecture`, `InstallerType`, `InstallerUrl`,
  `InstallerSha256`, and `ManifestType`, `ManifestVersion`. For an Inno or Nullsoft exe the page says the client sets the
  silent and silent-with-progress switches itself when `InstallerType` is `inno` or `nullsoft`. `PackageName` and `Publisher`
  should match Add/Remove Programs.
* https://github.com/microsoft/winget-pkgs/blob/master/doc/manifest/schema/1.12.0/installer.md: required `PackageIdentifier`,
  `PackageVersion`, `Installers[].Architecture`, `InstallerType`, `InstallerUrl`, `InstallerSha256`, `ManifestType`
  (`installer`), `ManifestVersion` (`1.12.0`). `InstallerSwitches` has Silent, SilentWithProgress, Interactive,
  InstallLocation, Log, Upgrade, Custom, Repair. `Scope` is `user` or `machine`. `UpgradeBehavior` `install`,
  `uninstallPrevious`, `deny`. `ElevationRequirement` `elevationRequired`, `elevatesSelf`, `elevationProhibited`.
  `AppsAndFeaturesEntries` (DisplayName, DisplayVersion, Publisher, ProductCode, UpgradeCode, InstallerType).
* https://github.com/microsoft/winget-pkgs/blob/master/doc/manifest/schema/1.12.0/defaultLocale.md: required
  `PackageIdentifier`, `PackageVersion`, `PackageLocale`, `Publisher`, `PackageName`, `License`, `ShortDescription` (3 to
  256 characters), `ManifestType` `defaultLocale`, `ManifestVersion`; at most 16 tags.
* https://learn.microsoft.com/en-us/windows/package-manager/package/repository (ms.date 2024-11-21, updated 2026-07-14):
  the submission steps, the folder rule `manifests/<first letter of publisher, lower case>/<publisher>/<application>/<version>`,
  one manifest per submission, the validation labels, and the expectations: HTTPS URL straight from the publisher's release
  location, a silent installer, install and uninstall working "for both administrators and non-administrators".
* On this PC: `winget --version` is v1.29.380 and `winget validate --manifest <dir> --disable-interactivity` exists
  (`winget validate --help` read). It is the only winget command the lane runs.

### Scoop

* https://github.com/ScoopInstaller/Scoop/wiki/App-Manifests: required `version`, `homepage`, `license` (description is
  recommended); `architecture` may hold `32bit`, `64bit`, `arm64`; `url`, `hash` (64 hex digits, or `sha1:`/`sha256:`/
  `sha512:`/`md5:` prefixed; SHA-256 is the default), `innosetup` (boolean, "true if the installer is InnoSetup based",
  no further detail on the page), `shortcuts`, `persist`, `notes`, `checkver`, `autoupdate`, `bin`, `installer`.
  License is an SPDX identifier, or a URL, or one of Freeware, Proprietary, Public Domain, Shareware, Unknown.
* https://raw.githubusercontent.com/ScoopInstaller/Scoop/master/schema.json: `$schema` draft-07, `$id`
  `http://scoop.sh/draft/schema#`, `required` = `["version","homepage","license"]`, the top-level property list, `license`
  as a string or an object with a required `identifier` and optional `url`, `url` without `$` placeholders, `autoupdate`
  with its own closed property list.
* https://github.com/ScoopInstaller/Scoop/wiki/Buckets: a bucket is a Git repository of JSON manifests; `scoop bucket add
  <name> <location>`.

### Chocolatey

* https://docs.chocolatey.org/en-us/create/create-packages/: only the nuspec is required; `chocolateyInstall.ps1` and
  `chocolateyUninstall.ps1` live in `tools\`; `choco pack` builds, `choco apikey add --source ... --key ...` then
  `choco push <file>.nupkg --source "https://push.chocolatey.org/"` pushes.
* https://docs.chocolatey.org/en-us/create/functions/install-chocolateypackage/: only `-PackageName` is required; `-FileType`
  default `exe`; `-SilentArgs`, `-Url`, `-Url64bit` (alias `url64`), `-ValidExitCodes` default `@(0)`, `-Checksum`,
  `-ChecksumType`, `-Checksum64`, `-ChecksumType64`, `-File`, `-File64` and others (the test's stub declares exactly the
  documented list).
* https://docs.chocolatey.org/en-us/create/functions/uninstall-chocolateypackage/ (`-PackageName` required; `-FileType`,
  `-SilentArgs`, `-File`, `-ValidExitCodes`) and `.../get-uninstallregistrykey/` (`-SoftwareName` required, wildcards
  allowed, the example reads `$key.UninstallString`).
* https://docs.chocolatey.org/en-us/community-repository/moderation/package-validator/: the moderation rules (CPMR...)
  the nuspec is checked against: description 30 to 4000 characters, tags present and no commas, `licenseUrl` different from
  `projectUrl`, `title` different from the id, no raw-GitHub icon URL, no `Start-Process` in scripts, a checksum when
  downloading, among others. (Titles only; the page gives no detail.)
* https://docs.chocolatey.org/en-us/create/create-packages/nuspec-reference answered 404. The required elements come from
  https://learn.microsoft.com/en-us/nuget/reference/nuspec (ms.date 2019, updated 2026-06-11), which the Chocolatey page
  points to: `id`, `version`, `description`, `authors` inside `metadata`, root `package` in namespace
  `http://schemas.microsoft.com/packaging/2010/07/nuspec.xsd`, a `files` element with `src` and `target`. NuGet states it
  never published a schema file, so the nuspec check is structural, not a schema validation.

### The installer type and the release names

* `installer\cascade.iss` (read-only) is an **Inno Setup 6** script: `OutputBaseFilename=foxsdr-setup-{#AppVersion}`,
  `AppId` `B3D4A7E2-6C51-4F8B-9A0D-2E7F5C8B1A64`, `PrivilegesRequired=admin` with `PrivilegesRequiredOverridesAllowed=commandline`,
  `UninstallDisplayName={#AppName} {#AppVersion}`, the application exe `foxsdr-slint.exe` (default `SlintIsApp`) and
  `cascade.exe` as the classic window.
* `tools/release.ps1` (read-only): the setup is `foxsdr-setup-<version>.exe` (`Get-ReleaseFileNames`), the tag is
  `v<version>`, the repository `wonderingStars/foxsdr` (`docs/RELEASING.md` config; README's download link). The hash source
  of a release is `dist\sha256.txt` and the five `SHA-256 of ...` blocks in `docs\release-notes-<version>.md`
  (`docs/RELEASING.md`, step `ci`); the generator does NOT use either: it hashes the file it is given. Compare its printed
  hash with `dist\sha256.txt` before publishing.
* Inno Setup's own command-line help (`jrsoftware.org/ishelp`) **could not be read**: the host refused every connection,
  `documentation.help` answered 403, the archive host is blocked for the fetch tool, and decompiling the locally installed
  `ISetup.chm` with `hh.exe -decompile` produced nothing. So the Inno switch semantics are marked INNO-not-checked in
  `docs/silent-install-switches.md`; what is stated as run is what `tools/release.ps1` and `docs/engine-stage4.md` 6.38.5
  actually ran.

## What was built

* `packaging/winget/templates/` three files (version, default locale `en-GB`, installer), schema 1.12.0,
  `PackageIdentifier StevenFox.FoxSDR` (publisher "Steven Fox" as in `AppPublisher`), `InstallerType inno`,
  `Scope machine`, `ElevationRequirement elevatesSelf`, `UpgradeBehavior install`, x64.
* `packaging/scoop/foxsdr.json.template`: `innosetup: true` (Scoop unpacks the setup, so the `foxsdr:` handler is not
  registered; the manifest's notes say so), shortcuts for the application exe and the classic window, `persist: plugins`,
  `checkver` from GitHub releases, `autoupdate` to the same release naming rule.
* `packaging/chocolatey/`: nuspec (id `foxsdr`), `chocolateyInstall.ps1` (`Install-ChocolateyPackage`, `url64bit`,
  `checksum64`, `/VERYSILENT /SUPPRESSMSGBOXES /NORESTART`), `chocolateyUninstall.ps1` (finds `FoxSDR*` through
  `Get-UninstallRegistryKey`, uninstalls only on exactly one match, no `/CLEANDATA`).
* `tools/make-package-manifests.ps1` (Windows PowerShell 5.1 and 7): described in `packaging/README.md`.
* `sdk/`: CMake project, `heartbeat_decoder.cpp`, test, sink scan, host-load script, break-it, README.
* `docs/api/openapi.yaml` (OpenAPI **3.0.3**: the widest tool support, and nothing here needs 3.1).
* `docs/silent-install-switches.md`.
* `tests/CMakeLists.txt`: two `add_test` entries, `packaging_manifests` (needs `pwsh`) and `packaging_openapi_routes` (needs
  Python), registered just before the gui/headless labelling pass so both carry `headless`.

## Tests (real result lines)

| What | Command and result |
|---|---|
| Generator, templates, schema checks | `pwsh -NoProfile -File tests\packaging\test_package_manifests.ps1` -> `215 checks, 0 failed` (about 30 s on an idle machine; 215 s under ctest while three other jobs ran) |
| Same through CTest | `ctest -C Release -R ^packaging_` -> `2/2 Test #778: packaging_manifests Passed 215.56 sec`, `Test #779: packaging_openapi_routes Passed 0.46 sec`, `100% tests passed out of 2` |
| `winget validate` on the generated set | inside the suite: `Manifest validation succeeded` (winget v1.29.380); the same suite shows winget refusing a copy without `InstallerSha256` |
| OpenAPI route scan | `python -I tests\packaging\test_openapi_routes.py` -> `580 checks, 0 failed` |
| SDK template | configured and built in `D:\foxsdr-build\en-packaging\sdk` (MSVC, /W4), `ctest -C Release` -> `100% tests passed out of 3` (heartbeat_test, heartbeat_sinks, heartbeat_sinks_scan_refuses_a_bad_file), `heartbeat_test.exe` -> `3689 checks, 0 failed`, no compiler warning at /W4 |
| SDK module in FoxSDR's own host | the existing `test_plugin_abi3_compat` (built in my build folder) run with `FOXSDR_PUBLISHED_PLUGINS_DIR` naming a folder that holds only `heartbeat-decoder-1.0.0-abi3-win-x64.dll`: `LOADED   Heartbeat Decoder      v1.0.0    caps=0x001`, `1 of 1 published binaries loaded`, `runner: 1 running, 0 idle ... 240000 audio + 0 I/Q frames fed, 5 text lines` (40000 samples at 8 kHz / 8000 = 5), `test_plugin_abi3_compat: 61 checks, 0 failed`; repeatable with `sdk\tests\host_load_check.ps1` |

Pinned independent value: the fake setup file (bytes 1 to 200) has SHA-256
`a48742653620d8aeee06f9aef53889980ead41a44aeb11c5b0996dfc4fbc127b`, computed with Python's `hashlib` before the test
was written.

### Break-it (remove one behaviour from a copy, the suite must go red, then the real files must be green)

**Manifests: generator and templates** (`tests\packagingreakit_package_manifests.ps1`, final run on the final files: `26 mutants, 0 survived`; unmodified: `215 checks, 0 failed`)

| Mutant | Result | First failing check |
|---|---|---|
| generator hashes the file NAME, not the file | RED | winget installer: SHA-256 is the file's (upper case, matched case-sensitively) |
| generator builds the URL with latest instead of the version tag | RED | winget installer: URL by the release naming rule |
| generator does not upper-case the winget hash | RED | winget installer: SHA-256 is the file's (upper case, matched case-sensitively) |
| generator does not check that -Version matches the file name | RED | refused: version that disagrees with the file name (exit 1) exit 0 |
| generator accepts any file name | RED | refused: file name setup.exe (says why) make-package-manifests: the version must be x.y.z, got: |
| generator accepts an empty setup file | RED | refused: empty setup file (exit 1) exit 0 |
| generator does not refuse a non-empty output folder | RED | refused: output folder that already has files (exit 1) exit 0 |
| generator does not refuse leftover placeholders | RED | stray brace pair in a template: exit 1 and nothing written |
| generator writes each file as soon as it is rendered (no all-or-nothing) | RED | generator (pwsh 7) exits 0 make-package-manifests: the output folder is not empty (use -Force to write into it |
| generator contacts a service (a web call is added) | RED | generator (pwsh 7) exits 0 Invoke-WebRequest: |
| generator runs winget install | RED | generator runs winget only as validate --manifest |
| winget installer template: wrong ManifestVersion | RED | winget installer file: ManifestVersion 1.12.0 |
| winget installer template: InstallerSha256 dropped | RED | winget installer: SHA-256 is the file's (upper case, matched case-sensitively) |
| winget locale template: unknown InstallerType value | RED | winget installer: InstallerType inno |
| scoop template: homepage dropped | RED | scoop manifest passes the schema |
| scoop template: hash key misspelt | RED | scoop: hash is the file's (lower case) |
| scoop template: hash replaced by a constant | RED | scoop: hash is the file's (lower case) |
| nuspec template: <id> dropped | RED | nuspec: required element <id> present and not empty |
| nuspec template: unclosed element (not well-formed) | RED | nuspec is well-formed XML Cannot convert value "<?xml version="1.0" encoding="utf-8"?> |
| nuspec template: wrong namespace | RED | nuspec: root is package in the nuspec namespace with a metadata element |
| chocolateyInstall template: checksum dropped | RED | install: checksum64 is the SHA-256 of the file |
| chocolateyInstall template: silent switch /S that release.ps1 never uses | RED | install: silent switch /S is one tools\release.ps1 runs the setup with |
| chocolateyUninstall template: UninstallString left quoted | RED | uninstall (one match): file is the unquoted unins000.exe path |
| chocolateyUninstall template: uninstalls when two matches are found | RED | uninstall (two matches): nothing is uninstalled, both are named in warnings |
| chocolateyUninstall template: asks for /CLEANDATA | RED | uninstall (one match): silent args, no /CLEANDATA |
| chocolateyInstall template: parameter name misspelt (url64bits instead of url64bit) | RED | install: url64bit is the release URL |

**OpenAPI file and the sources it is read against** (`python -I tests\packagingreakit_openapi.py`: `26 mutants, 0 survived`; unmodified: `580 checks, 0 failed`). The path-parameter mutant went red on its removed `security` line first, because the cut took the neighbouring block with it; the undeclared-parameter check is the same run's next failure.

| Mutant | Result | First failing check |
|---|---|---|
| a documented route is removed (GET /api/spectrum) | RED | route GET /api/spectrum is documented |
| a route that is not in the source is added (GET /api/nothing) | RED | documented operation GET /api/nothing is a real route |
| a route is documented under the wrong method (POST /api/status) | RED | route GET /api/status is documented |
| a status the handler answers is not documented (409 on /api/control) | RED | POST /api/control: status 409 that the handler can answer is documented documented: 202,400,401,413,415,503 |
| a status the handler never answers is documented (404 on /api/status) | RED | GET /api/status: documented status 404 can really be answered handler answers: 200,401 |
| a protected route is documented as public (/api/status security: []) | RED | GET /api/status: security matches the handler (the handler refuses an unauthorised request) |
| a public route is documented as protected (/api/session) | RED | GET /api/session: security matches the handler (the handler never refuses one) |
| a path parameter is not declared (/api/image/{index}) | RED | GET /api/image/{index}: states its sign-in rule (security) explicitly |
| a $ref that does not resolve | RED | $ref #/components/schemas/Spectrumm resolves |
| a control key is dropped (pluginFetch) | RED | ControlRequest documents the key pluginFetch |
| a control key is invented (centreHz) | RED | ControlRequest key centreHz is one web_control.cpp accepts |
| a control range is wrong (centerHz maximum) | RED | centerHz maximum is kMaxCenterHz from web_control.hpp 7000000000 vs 6000000000.0 |
| a status field is dropped (scannerState) | RED | Status documents the field scannerState |
| a status field is invented (rdsStation) | RED | Status field rdsStation is one statusJson writes |
| a to-add-at-merge route loses its marker (GET /api/events) | RED | documented operation GET /api/events is a real route |
| the session schema loses a key (reachableOffMachine) | RED | GET /api/session: the schema SessionState has exactly the keys the handler writes ['authRequired', 'authentic |
| the file leaves the YAML subset (a tab) | RED | openapi.yaml parses inside the YAML subset line 3: tab character |
| the file leaves the YAML subset (a duplicate key) | RED | openapi.yaml parses inside the YAML subset line 2: duplicate key 'openapi' |
| a new route appears in the source (GET /api/newthing) | RED | route GET /api/newthing is documented |
| a handler answers a new status (418 from /api/audio) | RED | GET /api/audio: status 418 that the handler can answer is documented documented: 200,401,503 |
| a handler stops requiring a sign-in (/api/status) | RED | GET /api/status: security matches the handler (the handler never refuses one) |
| statusJson writes a new field | RED | Status documents the field newField |
| the session answer gains a key | RED | GET /api/session: the schema SessionState has exactly the keys the handler writes ['authRequired', 'authentic |
| the audio listener cap changes in the header it is read from | RED | the audio listener cap named in the file (4) is the header's kMaxAudioListeners |
| web_control.cpp accepts a new key | RED | ControlRequest key muted is marked "to add at merge" and is not in web_control.cpp yet |
| web_control.hpp widens the centre-frequency range | RED | centerHz maximum is kMaxCenterHz from web_control.hpp 6000000000 vs 7000000000.0 |

**SDK template** (`sdk	estsreakit_sdk.ps1`: `9 mutants, 0 survived`; unmodified copy GREEN)

| Mutant | Result | First failing check |
|---|---|---|
| a second is one sample late (counted <= rate) | RED | heartbeat_test ............................***Failed 0.68 sec |
| no back-pressure: a new line overwrites the one not yet collected | RED | heartbeat_test ............................***Failed 1.44 sec |
| poll_text writes more than the room it was given | RED | heartbeat_test ............................***Failed 0.80 sec |
| the capability entry lies about its table size | RED | heartbeat_test ............................***Failed 1.02 sec |
| the entry point answers a host with another ABI | RED | heartbeat_test ............................***Failed 1.03 sec |
| poll_text does not guard a NULL handle | RED | heartbeat_test ............................***Exception: SegFault 5.02 sec |
| the descriptor version no longer matches CMakeLists.txt | RED | heartbeat_test ............................***Failed 1.32 sec |
| the plugin prints to stderr (the sink scan must refuse it) | RED | heartbeat_sinks ...........................***Failed 0.48 sec |
| the line number is off by one | RED | heartbeat_test ............................***Failed 0.96 sec |


### What the break-it found, and what was fixed

The first full run of the manifest break-it left two mutants alive. (1) Not upper-casing the winget hash: the test's
`-match` is case-insensitive in PowerShell, so it could not see the case; now `-cmatch`. (2) `url64` for `url64bit`: not a
fault (PowerShell accepts a unique prefix of a parameter name, and the real Chocolatey function has `url64` as an alias),
so the mutant was equivalent; the mutant is now `url64bits`, which the stub refuses. Both re-ran red, and the full
break-it was run again on the final files.

## Gates

**Own tests first**: `packaging_manifests` and `packaging_openapi_routes` pass through CTest (above); the SDK project builds
with no warnings and its three CTest entries pass; `test_plugin_abi3_compat` with the SDK module: 61 checks, 0 failed.

**Headless set** (`ctest -C Release -L headless`, 705 tests, run in 12 slices of 60 with `-j 3 --timeout 300`; the whole
product was built first, 560 test programs, 0 compile errors): the product tree is byte-identical to d3630d78 outside
`tests/CMakeLists.txt` (`git diff --name-status d3630d78 HEAD` shows only that one `M`; everything else is `A`), so these
results describe the tree the lane started from, plus my two tests.

* First pass: **660 passed, 45 failed** (the machine was at high load; the slices took 91 to 861 s each; the slice that took
  861 s ran past 10 minutes because of load, the others were under).
* The 45 re-run one at a time with `-j 1 --timeout 180`: **27 passed alone** (load flakes), **18 still red**.
  The final count is therefore **687 of 705 pass, 18 red**.
* The 18, sorted:
  * On the rules' known false-failure lists (9): `test_airband_data`, `test_band_plan`, `test_converter_call_sites`,
    `test_page_resize_id`, `test_png_decode`; `test_diagnostics_free` (`gitCommit() == expect`: the build was made before
    this lane's commits, so its stamped commit is stale); `test_pass_control` and `test_tx_audio` (the listed load
    flakes; they failed again alone, with `n >= 80u`/median-lag checks and `tx.preemphasis()`); `test_window_failed_app`
    (configuration: configured with `CASCADE_SOFTGL=OFF`, so `softgl\opengl32.dll` is absent).
  * **Not on any list (9)**, all in code this lane did not touch: `test_engine_api_decode_radio` (timeout at its 120 s
    limit), `test_engine_api_merge` (SEGFAULT in the first pass, a 600 s timeout alone), `test_engine_api_source_kinds`
    (fails at 0.19 s), `test_engine_api_transmit` (178 s; failures at `test_engine_api_transmit.cpp:571,573,702,703,737,738`,
    depth and ticket-order checks), `test_failure_files` (74 s), `test_sentinel` (`test_sentinel.cpp:2132-2136`, the
    own-module report lines) and `test_sentinel_beat_source` (`headlessFrames >= 10u`), `test_soapy_enum_proc` (timeout),
    `test_crash_upload_free` (`srv.connections() == 1`). They are reported, not fixed (they are other lanes' code), and
    **not baselined against a second build**: the argument that they are not caused by this lane is that the binaries are
    built from sources equal to the base commit. Whether they also fail on the engine/multi-radio tip in a quiet
    machine is UNVERIFIED.


## UNVERIFIED (not run, and why)

* **Chocolatey**: Chocolatey is not installed and nothing was downloaded, so `choco pack`, the install script and the
  uninstall script were never run in Chocolatey. They were parsed and run against stubs declared from the documented
  parameter lists. `<iconUrl>` is absent (no stable non-raw-GitHub icon URL exists); the community moderation rules
  (CPMR0033 asks for an icon) may ask for one.
* **Scoop**: Scoop is not installed. The manifest passes a schema written from Scoop's published one; it was never
  installed, never run through `checkver` or `checkhashes`, and what `innosetup: true` extracts from this setup (the `{app}`
  files, no `[Code]`, no registry) is Scoop's documented-by-name behaviour, not observed. Whether `checkver` on GitHub
  skips pre-releases (so Scoop would offer 0.99.104, not a beta) was not checked.
* **winget**: `winget validate` passes; **no `winget install` was run** (forbidden). Whether the community repository accepts
  this manifest is the moderators' decision: its policies page was not opened; the noncommercial licence string
  `PolyForm Noncommercial 1.0.0`, the unsigned setup, a machine-scope installer that elevates itself ("install and
  uninstall working for both administrators and non-administrators"), and `Validation-Domain` (the URL is the project's own
  GitHub release) are the points likely to be questioned. `ProductCode` is omitted (the Inno uninstall key name is
  `{AppId}_is1`, but whether winget wants that value there was not verified).
* **Inno Setup switches** marked INNO in `docs/silent-install-switches.md` and the bare-switch forms (`/CLEANDATA` without
  `=1`).
* **SDK**: only the Windows MSVC build was run. The Linux and macOS file-name rules and compiler flags in
  `sdk/CMakeLists.txt` are unverified. The decoder was never run against a real receiver's audio, only the host's
  isolated test with noise.
* **OpenAPI**: read from source and checked against it by scan; the live server was not started, so the `413` answer, the
  throttle, cookie attributes and the streaming routes are as the source says, not observed. Item types of the nested lists
  in Status (tracks, plugins, catalogue, images, basemap) are given as `object`; read `statusJson()` for their fields.
* The generator and templates were run on a fake setup file, never on a real release setup.

## Owner decisions

1. **Publish?** Each index needs an account: winget-pkgs (a GitHub fork and a pull request, one version each), a Scoop
   bucket (a GitHub repository of your own; the main bucket is a separate matter), Chocolatey community (an account and
   API key). Commands: `packaging/README.md`.
2. **Identity strings**: `PackageIdentifier StevenFox.FoxSDR`, Chocolatey id `foxsdr`, moniker `foxsdr`. They are hard to
   change once published.
3. **Which releases**: GitHub pre-releases (the 0.99.108 beta) versus the stable channel (0.99.104). winget and Chocolatey
   would carry whatever manifests are submitted; Scoop's `checkver` follows GitHub's latest release.
4. **winget scope**: the manifest is machine-scope only. winget-pkgs validates installs for non-administrators too; a
   second installer entry with `Scope: user` and `InstallerSwitches: Custom: /CURRENTUSER` is possible (the setup supports
   `/CURRENTUSER`) but was not added, because winget's own handling of that switch for Inno was not verified.
5. **Icon URL** for the Chocolatey package and the moderation rules (a hosted icon on foxsdr.com would do).
6. **SDK distribution**: ship `sdk/` as a zip beside the release, in the plugin repository, or on the website; and whether
   `docs/api/openapi.yaml` is linked from the site.
7. **Hand-offs**: add the generator as a step to `tools/release.ps1` (not edited here); the manual lane includes
   `docs/silent-install-switches.md`.

## Wiring and screen hand-off

There is no screen and no control. For the release script: after the `github` step, run
`tools\make-package-manifests.ps1 -SetupFile <release>\dist\foxsdr-setup-<v>.exe -OutDir <release>\packages -RunWingetValidate`
and compare the printed SHA-256 with `dist\sha256.txt`.

English strings (package metadata, not translated): the winget `ShortDescription` ("A free software-defined radio
receiver for Windows with a spectrum and waterfall, a scanner, recording, a map and 43 plugins.") and `Description`,
the Chocolatey `summary` and `description`, the Scoop `description` and `notes`, the SDK's descriptor name "Heartbeat
Decoder" and its `heartbeat N` text line. "43 plugins" is the README's count; update it when the catalogue changes.

## Merge notes

* `tests/CMakeLists.txt`: one block of two `add_test`s was added just before the "Label APPENDED" gui/headless pass, which
  other lanes' list edits also touch; keep the block above the labelling loop.
* `docs/api/openapi.yaml`: `GET /api/events` and the `muted` field of the control request and the status are marked
  `x-to-add-at-merge` for the feat/web-secure lane (branch feat/web-bookmarks, `docs/feature-records/web-bookmarks.md`).
  When it merges, remove the markers: `test_openapi_routes.py` then requires them in the source and says which marker is
  stale. Any other route or control key that lane (or another) adds makes the test name the missing item.
* `packaging/` also holds the platform lane's `flatpak/`, `docker/` and `linux/` on `feat/platform`; this lane only adds
  `packaging/winget`, `packaging/scoop`, `packaging/chocolatey` and `packaging/README.md` (the platform branch has no
  README there at the time of writing).

## Environment

The real `%APPDATA%\foxsdr\config.json` was only hashed, never opened for writing: sha256
`c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` at the start and at the end of the lane (the expected
value). No radio was opened, no connection was made to any package service, no account was created, no certificate store,
signing tool, `certutil`, `ssh` or credential prompt was used, and no window other than hidden test processes was shown.
