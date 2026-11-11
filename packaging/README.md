# Windows package-manager manifests (winget, Scoop, Chocolatey)

These are TEMPLATES plus a generator. Nothing here is submitted, published, signed or uploaded, and the generator never
contacts a service. Publishing to a package index needs an account and is the owner's step (below).

```
packaging/winget/templates/    three winget files (version, default locale, installer), schema 1.12.0
packaging/scoop/foxsdr.json.template
packaging/chocolatey/foxsdr.nuspec.template  and  tools/chocolateyInstall.ps1.template, chocolateyUninstall.ps1.template
tools/make-package-manifests.ps1             fills them from a release setup file
```

(`packaging/flatpak`, `packaging/docker`, `packaging/linux` belong to the platform lane and are not touched here.)

## Make the manifests for a release

After `tools\release.ps1` has produced `foxsdr-setup-<version>.exe` (it is in the release folder's `dist\`), and BEFORE or
AFTER the GitHub release (the file is only read; the URL is built from the release naming rule):

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools\make-package-manifests.ps1 `
    -SetupFile C:\Users\steve\foxsdr-build\release-<version>\dist\foxsdr-setup-<version>.exe `
    -OutDir C:\Users\steve\foxsdr-build\release-<version>\packages -RunWingetValidate
```

It computes the SHA-256 of the file on this PC, takes the version from the file name, and writes:

```
packages\winget\StevenFox.FoxSDR.yaml, StevenFox.FoxSDR.locale.en-GB.yaml, StevenFox.FoxSDR.installer.yaml
packages\scoop\foxsdr.json
packages\chocolatey\foxsdr\foxsdr.nuspec and tools\chocolateyInstall.ps1, chocolateyUninstall.ps1
```

It refuses (exit 1, nothing written) when the file is missing or empty, is not named `foxsdr-setup-<x.y.z>.exe`, when
`-Version` disagrees with the name, when a template names a placeholder it does not know or leaves one unfilled, or when
the output folder already has files (`-Force` writes into it anyway). `-AppExe` (default `foxsdr-slint.exe`, the program
`installer\cascade.iss` makes the application unless built with `/DSlintIsApp=0`) and `-RepoUrl` (default
`https://github.com/wonderingStars/foxsdr`) can be changed. `-RunWingetValidate` runs `winget validate --manifest
<winget folder> --disable-interactivity` (read-only; it installs nothing).

Whether the setup is code-signed changes nothing in the manifests. winget and Chocolatey run the setup silently with the
switches in `docs/silent-install-switches.md`; Scoop unpacks it without running Setup.

## Tests

```
pwsh -NoProfile -File tests\packaging\test_package_manifests.ps1      generator + templates + schema checks
pwsh -NoProfile -File tests\packaging\breakit_package_manifests.ps1   removes one behaviour at a time; the suite must go red
python -I tests\packaging\test_openapi_routes.py                       docs\api\openapi.yaml against the web server's source
python -I tests\packaging\breakit_openapi.py
```

## OWNER STEPS (not done: each needs an account on an outside service)

None of these has been run. The commands come from the opened official pages (see `docs/feature-records/packaging.md`).

### winget (Windows Package Manager community repository)

Needs a GitHub account and a fork of `microsoft/winget-pkgs`; one version per pull request.

```
winget validate packages\winget
git clone --filter=blob:none --no-checkout <your fork of https://github.com/microsoft/winget-pkgs>
cd winget-pkgs
git sparse-checkout set manifests\s\StevenFox
git checkout -b foxsdr-<version>
# copy the three .yaml files into manifests\s\StevenFox\FoxSDR\<version>\
git commit -m "Submitting FoxSDR version <version>" --all
git push
# then open the pull request on GitHub; the bot validates and a moderator reviews it
```

The folder is `manifests` / first letter of the publisher in lower case / publisher / application / version, and the
`PackageIdentifier` and `PackageVersion` must match it (`StevenFox.FoxSDR` -> `manifests\s\StevenFox\FoxSDR\<version>`).
`Tools\SandboxTest.ps1` in that repository tests the manifest in Windows Sandbox before the pull request.

### Scoop (a bucket of your own)

Needs a GitHub account and a repository for the bucket (the bucket template at ScoopInstaller/BucketTemplate adds the
update workflows).

```
git clone https://github.com/<you>/<bucket>
# copy packages\scoop\foxsdr.json to the bucket (the Scoop page's example puts the manifest at the repository root)
git add . ; git commit -m "foxsdr <version>" ; git push
scoop bucket add <bucket> https://github.com/<you>/<bucket>      # what a user then types
scoop install foxsdr
```

### Chocolatey (community repository)

Needs a Chocolatey account and its API key.

```
cd packages\chocolatey\foxsdr
choco pack
choco apikey add --source "'https://push.chocolatey.org/'" --key "'<API_KEY>'"
choco push foxsdr.<version>.nupkg --source "'https://push.chocolatey.org/'"
```

`choco pack` needs Chocolatey installed, which this PC does not have; the package has been checked structurally only (XML
well-formed, required nuspec elements, the scripts parsed and run against stubbed helpers).
