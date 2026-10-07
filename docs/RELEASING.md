# Releasing FoxSDR

A release used to be ten manual steps and about seventy minutes of attention, with three
traps that each cost an hour the first time (they are listed at the end). It is now two
scripts that run one NAMED STEP at a time, check what they did, and stop at the first
failure:

| Script | What it does |
|---|---|
| `tools\release.ps1` | The application: build, installer, MSIX, install test, CI artifacts, symbols, GitHub release, Store submission. |
| `tools\site-release.ps1` | The website: bump, test, build, stage on the Pi, swap, verify through Cloudflare. |
| `tools\release-common.ps1` | What both share (output, process and ssh helpers). Dot-sourced, never run on its own. |

Both are Windows PowerShell 5.1 scripts (no `&&`, no ternary), both take `-Steps a,b,c` or
`-From <step>`, both take `-DryRun`, both are safe to run again, and both keep their state
in a release folder.

```
C:\Users\steve\foxsdr-build\release-<version>\
    state.json        what each step finished, for which commit, with which hashes
    site-state.json   the same for site-release.ps1
    dist\             the five release files, sha256.txt, gh-notes-<version>.md
    msix\             FoxSDR-<version>-x64.msix (and the Store upload zip)
    msix-in\          cascade.exe + SoapySDR.dll, the only payload the MSIX is built from
    ci\               one folder per GitHub Actions artifact
    verify\           install.log, uninstall.log, symbols.json, what the site served
    site\             the built foxsdr-site binary
    store\            the Partner Center fallback bundle (only if it was needed)
    log\<step>.log    every command and its output; site-<step>.log for the site
```

A release folder belongs to ONE commit. Running against a different HEAD stops with a
message (`-Force` starts over, or give a new `-ReleaseRoot`).

## What `-DryRun` is

It prints every command and every remote action (`ssh host 'cat > path' < file`, the `gh`
calls, the Store requests) instead of running it. A step that has already been done says so
and still prints its plan. What still RUNS, because a dry run that cannot see the machine can
only show a plan and not whether the plan would work: `git` and file reads, the tool
versions, `ssh <host> true`, `gh auth status`, the public Store catalogue query in
`store-status`, and `site_bump.py --check` (which writes nothing).

## Before the first release with the scripts

1. **Tools on the release machine**: Visual Studio 2022 with the C++ workload, CMake, vcpkg at
   `C:\vcpkg` with `soapysdr:x64-windows` installed, Inno Setup 6, the Windows SDK, `py -3.14`
   with Pillow (`py -3.14 -m pip install pillow`), `gh` logged in as the repository owner, an
   ssh key that works for the NAS and the Pi, and Go for the site. `release.ps1 -Steps preflight`
   checks every one of them and says which is missing.
2. **Hosts** are kept out of this (public) repository. Put them in
   `C:\Users\steve\foxsdr-build\deploy\release-config.json` (or pass `-PiHost`, or set
   `FOXSDR_PI_HOST`):
   `{ "piHost": "<user>@<address>", "nasHost": "nas", "nasSymbolRoot": "/volume1/foxsdr-symbols",
   "piSymbolDir": "/var/lib/foxsdr-site/symbols", "piSiteRoot": "/opt/foxsdr-site",
   "repo": "wonderingStars/foxsdr" }`.
3. **Store credentials** (optional: without them the Store is submitted by hand): see
   "The Microsoft Store" below.
4. **Telemetry Worker**: nothing to set up, but see step 1 of the procedure.

## The procedure

1. **Version bump commit.** `CMakeLists.txt` (`project(cascade VERSION x.y.z`), `README.md`
   ("The current release is **x.y.z**"), and `docs\release-notes-<x.y.z>.md` with its five
   `SHA-256 of ...` blocks committed as `` `TBD` ``. Push master. CI builds Linux x64 and
   arm64 (about 32 minutes) and Windows (see "CI"); a tag push triggers nothing.
2. **If `telemetry-worker\worker.js` changed since the previous release tag**, deploy the
   Worker BEFORE releasing: `cd telemetry-worker; npx --yes wrangler deploy` (the account
   token is the owner's; the machine's own `CLOUDFLARE_API_TOKEN` is read-only), check
   `POST not json` to the Worker's URL answers 400, and pass `-WorkerDeployed` to preflight.
   Without it preflight refuses, and says why. A Worker older than the application drops the
   fields the application sends.
3. **Windows, while CI runs** (from a clean worktree at the release commit):
   `powershell -ExecutionPolicy Bypass -File tools\release.ps1 -Steps preflight,build,installer,msix,install-test`
4. **CI and the five files**: `... -Steps ci` waits for the run for HEAD (polling every
   60 s), downloads the artifacts, checks every file, copies the five release files into
   `dist\` and writes their SHA-256 into the notes file. That is the ONE edit the scripts make
   to the tree; it prints the diff. Commit it or `git checkout` it after the release, as you
   prefer (until then preflight accepts a tree whose only change is those five lines).
5. **Symbols and GitHub**: `... -Steps symbols,github`. Symbols are mirrored to the NAS (rows
   APPENDED to its index, never overwritten) and the maps put on the Pi before the site swap.
   The release is created as a draft, GitHub's digests compared with the local hashes, and only
   then published as latest.
6. **Store**: `... -Steps store` (checks everything and writes nothing), then
   `... -Steps store -SubmitStore`. Run `... -Steps store` again later for the certification
   status.
7. **Site**: with a clean worktree of the site repository at the commit to build on:
   `tools\site-release.ps1 -SiteDir <worktree> -AppNotes <this repo>\docs\release-notes-<x.y.z>.md -NewSiteVersion <next minor> -Steps bump,test,build`
   then commit and push the site, then `-Steps stage` and `-Steps swap,verify`. `-DryRun`
   shows all of it first.
8. **Afterwards**: `release.ps1 -Steps store-status` says which version the public Store
   catalogue serves and whether the site's `StoreAppVersion` is stale; if it is, the next site
   bump passes `-StoreVersion <that version>`.

## What each step checks

`release.ps1` prints one line per check, `PASS` or `FAIL` and the value that decided it; any
`FAIL` stops the run (exit 1). Exit 2 means something only the owner can do is next.

| Step | Checks |
|---|---|
| `preflight` | Tree clean (a modified tracked file stamps `-dirty` into the binary); HEAD tagged or taggable (an existing tag on another commit fails); CMakeLists.txt, README.md and the notes file agree on the version; the notes carry five `TBD` blocks or five real hashes, naming exactly the five release files; every tool present; `gh auth status`; `ssh <host> true` for the NAS and the Pi; a changed `worker.js` needs `-WorkerDeployed`. |
| `build` | Configure; the binary's version string is the project version; build twice (the second pass should change nothing); `cascade.exe --version` prints `FoxSDR <version>`; only `SoapySDR.dll` beside it; symbols archived for exactly that binary (PDB, exe, symbol map, index row); the copy in `msix-in\` equals the build. |
| `installer` | ISCC from PowerShell; `generated-version.iss` says the version; the installer exists, its size and sha256; it is not code-signed (stated). |
| `msix` | `build-msix.ps1` with the Store identity; the package's own `AppxManifest.xml` has the right Name, Publisher and Version (product major + 1); the `cascade.exe` inside has the sha256 of the built one. |
| `install-test` | Nothing installed before (the installer silently removes a copy in the other scope, so it refuses to run over one); silent per-user install exits 0; the installed `cascade.exe` and `SoapySDR.dll` equal the built ones; payload and registry entries present; `--version`, `--selftest` and `--frames 60` run (with scratch `APPDATA`/`LOCALAPPDATA`/`CASCADE_CONFIG_TEST`, usage reporting off and its URLs pointed at a discard port); silent uninstall; the install directory (polled: the uninstaller returns before it has finished), the registry keys and the `foxsdr:` handler gone. A failure still uninstalls. |
| `ci` | The run for HEAD exists; the required jobs (`linux`, `arm64`) succeeded; every artifact downloaded (three tries each); every tarball is gzip and every AppImage ELF, over 1 MB; the five hashes written into the notes. |
| `symbols` | Every file sent by `cat >` into a `.part` name, its sha256 read back on the far side, then renamed; a file already there with the same hash is skipped and one with a different hash stops the run; the NAS index must end in a newline and have no BOM before a row is appended, and grows by exactly one row; the symbol maps go to the Pi as `cascade-<build id>.json.gz` and `cascade-<id>.linux.json.gz`. |
| `github` | HEAD is on `origin/master`; the notes' hashes equal the files'; annotated tag `v<version>` on HEAD, pushed; draft release; every asset's name, size and digest equal the local file's; only then `--draft=false --latest`. |
| `store` | See below. |
| `store-status` | The public catalogue answers; the package version is decoded (four 16-bit numbers; product major = package major - 1); compared with `StoreAppVersion` in the site's `version.go`. |

`site-release.ps1`: `bump` (the five hash constants in `version.go` and on `web/index.html`
equal the files in `dist\`, no `PENDING-` token left, new versions in place), `test` (`go vet`,
`go test -count=1 ./...`), `build` (`CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go build -trimpath`,
then the binary's own build settings and ELF header), `stage` (room on the Pi; binary,
`swap-site.sh`, the five downloads and the symbol maps by `cat >` with sha256 read back; a
download already on the Pi with a different hash stops the run), `swap` (`swap-site.sh` ON the
Pi; waits at least 180 s; a rollback is a FAIL), `verify` (refuses to make any `/download`
request until all five files are read back on the Pi; then `/healthz` through Cloudflare,
`/api/update?v=<previous>`, and the five downloads and `/download/latest` by sha256).

## The Microsoft Store

**Mechanism.** The Partner Center submission REST API, called directly by `release.ps1`
(documentation:
`learn.microsoft.com/windows/uwp/monetize/create-and-manage-submissions-using-windows-store-services`,
`.../manage-app-submissions`, `.../create-an-app-submission`, `.../update-an-app-submission`,
`.../commit-an-app-submission`, `.../get-status-for-an-app-submission`, `.../get-app-data`;
the zip goes to the returned Azure Blob SAS URL with `x-ms-blob-type: BlockBlob`,
`learn.microsoft.com/rest/api/storageservices/put-blob`). The Microsoft Store Developer CLI
(`msstore`, `learn.microsoft.com/windows/apps/publish/msstore-dev-cli/commands`) calls the same
API; it was not used because it needs the .NET 9 desktop runtime and a credential store of its
own and takes the whole submission JSON as ONE command-line argument to change the What's new
text or the mandatory flag, while the API does each of those directly.

**What the step does:** create a submission from the last published one, mark the old package
`PendingDelete` and the new one `PendingUpload`, set the en-GB listing's `releaseNotes` from the
release notes, set `packageDeliveryOptions.isMandatoryUpdate` (the standing instruction: every
Store update is mandatory), upload the zip, commit, and poll until the commit is accepted. Run
`store` again for `status` and `statusDetails` (errors, warnings, certification reports). A
submission already in progress stops the step (it may be your own draft); `-ReplacePendingStoreSubmission`
deletes it first.

**The one thing that may stop all of it.** The documentation says "This API cannot be used with
apps or add-ons that use mandatory app updates ... the API will return a 409 error code", while
its own data model documents `isMandatoryUpdate`. Every FoxSDR submission ticks "Make this
update mandatory", so the first live call decides. If `create` answers 409 the step does not
fail: it writes `store\MANUAL-<version>.txt` and `store\whats-new-<version>.txt` and reports the
step as owner action. `msstore` would meet the same 409.

**Owner setup (once).** In Partner Center
(`learn.microsoft.com/windows/apps/publish/partner-center/manage-azure-ad-applications-in-partner-center`):
the account must be associated with a Microsoft Entra ID directory and you must be a Global
administrator of it; then Account settings > User management > the "Microsoft Entra
applications" tab > Add Microsoft Entra application > Create (or add an existing one), with the
MANAGER role; click the application's name for its Tenant ID and Client ID; "Add new key" and
copy the Key at once (it is shown once). Write
`C:\Users\steve\foxsdr-build\store-credentials.json`:
`{ "tenantId": "...", "clientId": "...", "clientSecret": "...", "applicationId": "9NBT95VMRVL2" }`.
Nothing the scripts print or log contains any of those values; the upload URL's signature is
redacted too.

**By hand (what the step automates).** Partner Center > FoxSDR > Start update (a draft from the
last published submission); Packages: upload the .msix, tick "Make this update mandatory"; Store
listings > English (United Kingdom) > "What's new in this version"; Save; Submit for
certification. Certification took a few hours both times on 2026-10-05.

## CI

`.github/workflows/build.yml` has three jobs: `linux` and `arm64` (build, test, package,
AppImage, symbols) and `windows`. The `windows` job is NEW and has not yet run on a runner:
read the first run, expect to correct it.

* It runs on `windows-2022`, not `windows-latest`: `windows-latest` is Windows Server 2025 with
  Visual Studio 2026 only (`actions/runner-images` README), and the project is configured with
  the "Visual Studio 17 2022" generator.
* The only package CMake needs from vcpkg is `soapysdr` (everything else is vendored under
  `third_party/`). vcpkg is checked out at the commit of the release machine's own `C:\vcpkg`
  (the `VCPKG_COMMIT` in the job) and the job stops unless soapysdr 0.8.1 is what it installed.
  Move the commit when the release machine's vcpkg moves. There must be no `vcpkg.json` at the
  repository root: with the toolchain file in use it would switch every local build to manifest
  mode.
* The vcpkg binary cache is restored from, and saved to, GitHub's cache
  (`VCPKG_DEFAULT_BINARY_CACHE`), saved right after the install so a later failure keeps it.
* It builds the `cascade` target twice, checks `--version`, builds the installer (ISCC is
  already on the image; `choco install innosetup` is the fallback) and the MSIX with the real
  Store identity, and uploads `foxsdr-windows-installer`, `foxsdr-windows-msix` and
  `foxsdr-windows-symbols`. It does not run the test suite.
* `release.ps1 -Steps ci` judges the run on `-RequiredCiJobs linux,arm64` only. Add `windows`
  once it has been seen green. The installer that ships is still the one built and
  install-tested on the release machine; the CI one is for comparison (the CRT redistributable
  and the toolset differ, so the bytes will not match).

## Traps

* **The Pi has no `scp` or `sftp`.** Files go `ssh <pi> 'cat > path' < local`, then `sha256sum`
  on the Pi; downloads go into place with `install -o foxsdr -g foxsdr -m 644`. The NAS needs
  `scp -O` for the same reason; the scripts use the `cat >` pipe for both.
* **A binary piped through a .NET `Process.StandardInput` gets a UTF-8 BOM** when the console
  code page is UTF-8 (Windows PowerShell 5.1), so every uploaded file arrived three bytes long.
  The scripts hand the file to `Start-Process -RedirectStandardInput` instead, and the sha256
  read back on the far side is what caught it.
* **`gh run download`** must run inside the checkout or carry `-R owner/repo`; a single
  `-n <artifact>` download extracts WITHOUT the artifact-name folder; the 76 MB arm64 symbols
  artifact has timed out once and worked on a retry (three tries are built in).
* **ISCC must be started from PowerShell**; from Git Bash MSYS rewrites its `/D` and `/O`
  switches into paths. Likewise write MSBuild's switch as `-m:4`, never `/m`.
* **`cascade.exe` is becoming a Windows-subsystem program.** From PowerShell `& cascade.exe
  --version` then returns at once, captures nothing and leaves `$LASTEXITCODE` unset (measured
  on a copy with its subsystem field patched). Every call in these scripts, and in
  `build-msix.ps1`, goes through redirected handles instead.
* **The site takes about 55 seconds to start.** A 40 second health wait once rolled back a
  healthy binary. `swap-site.sh` waits 180 s and `site-release.ps1` refuses less.
* **Never probe a `/download/<name>` URL before the file is on the Pi.** Cloudflare caches the
  404. `verify` reads every file back on the Pi first.
* **Append to the NAS symbol index, never overwrite it** (about 1,200 rows there; a fresh
  worktree's has one). `tools\build-nightly.ps1` still copies the whole file.
* **The installer silently uninstalls a FoxSDR in the other install scope.** The install test
  refuses to run over an existing install.
* **A modified tracked file stamps `-dirty`** into the commit the binary reports; the build
  step refuses unless `-AllowDirty` (rehearsal only).
* **Hosted runners fail now and then with nothing wrong in the code** ("job was not acquired",
  exit 143 in the compile step): `gh run rerun <id> --failed -R wonderingStars/foxsdr`.
* **A Store submission needs the owner's own yes.** `-SubmitStore` is that yes; the scripts
  never submit without it.

## Not yet verified

The `windows` CI job (not run on a runner); every call to Microsoft (the token endpoint, the
submission API, the blob upload; the flow was exercised only against a loopback fake shaped
from the documentation, and the 409 question above is open); `sha256sum` on the NAS (the NAS
helper falls back to `shasum` and `openssl`); the real `gh` digest field of a draft release;
the Windows-subsystem build of `cascade.exe` itself (only a patched copy of the console build
has been run).
