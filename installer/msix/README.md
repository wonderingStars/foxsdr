# FoxSDR as an MSIX package

This directory packages FoxSDR as an MSIX so it can be submitted to the
Microsoft Store, where **Microsoft signs the package** and no code-signing
certificate has to be bought.

    installer\msix\
      AppxManifest.xml    the package manifest, as a template
      make-logos.py       renders every Store logo asset from the app's own icon
      build-msix.ps1      stages, indexes, packs, and (for local tests) signs
      README.md           this file
      Layout\             generated - the package contents before packing
      Output\             generated - FoxSDR-<ver>-x64.msix

Build it:

    powershell -ExecutionPolicy Bypass -File installer\msix\build-msix.ps1 -Force

Build it and sign it so it can be installed on this machine for testing:

    powershell -ExecutionPolicy Bypass -File installer\msix\build-msix.ps1 -TestSign -Force

Build the one you would actually submit, once the name is reserved:

    powershell -ExecutionPolicy Bypass -File installer\msix\build-msix.ps1 -Force `
        -IdentityName      "<Package/Identity/Name from Partner Center>" `
        -IdentityPublisher "<Package/Identity/Publisher from Partner Center>"

**Citation convention**, as in the research notes this work follows:
learn.microsoft.com pages carry `ms.date` (when written) and `updated_at`
(when last refreshed), and on these pages they differ by years. Both are
given. Anything not verified is labelled as such rather than answered from
memory.

---

## 1. Why MSIX, and what it costs instead

The EXE route needs an Authenticode certificate. The MSIX route does not:

> "Your MSIX and AppX packages don't have to be signed with a certificate
> rooted in a trusted certificate authority when submitting to the Microsoft
> Store. The Microsoft Store will automatically re-sign your MSIX/AppX packages
> with a Microsoft certificate during the publishing process after your app
> passes certification. This means:
> - You don't need to purchase a CA-trusted code signing certificate for
>   MSIX/AppX Store submissions
> - You don't need to provide a .pfx or .cer file from a certificate authority
>   to submit MSIX/AppX packages
> - USB tokens or hardware security modules (HSMs) are not required for
>   MSIX/AppX Store submissions"

— `learn.microsoft.com/windows/apps/publish/publish-your-app/msix/app-package-requirements`
(`ms.date` 2022-10-30, `updated_at` 2026-08-24).

So **the package you submit is unsigned.** `build-msix.ps1` produces exactly
that by default. `-TestSign` exists only so the package can be installed on a
development machine, and a self-signed certificate is worthless for any other
purpose — `.../publish/code-signing-options` (2026-08-29): "self-signed
certificates are not accepted".

What it costs instead is that the product installs, updates and finds its own
files differently from the Inno Setup build. Sections 3 to 6 are that
difference, written down.

---

## 2. What the package contains

Item for item what `installer\cascade.iss` ships, and `build-msix.ps1`
enforces the same payload contract the `.iss` enforces at compile time (only
`SoapySDR.dll` may appear as a runtime DLL; anything else aborts the build, so
a new DLL is added to both packages deliberately or to neither).

| In the package | From |
|---|---|
| `cascade.exe` | the Release build |
| `SoapySDR.dll` | the Release build — the hardware ABI boundary |
| `msvcp140.dll`, `vcruntime140.dll`, `vcruntime140_1.dll` | the VS Redist folder, never `C:\Windows` |
| `LICENSE.txt` | the repository `LICENSE` (PolyForm Noncommercial 1.0.0) |
| `THIRD-PARTY-LICENSES.txt` | required by the MIT, BSD-3 and Zlib notices |
| `POSTINSTALL.txt` | the hardware setup notes |
| `resources\bandplans\*.json` | nine plans — `BandPlan::defaultDir()` looks beside `cascade.exe` |
| `Assets\*.png` | 84 logo assets, section 7 |
| `AppxManifest.xml`, `resources.pri` | generated |

**No plugins are staged, and that is correct rather than an omission.** The
Inno installer ships none either: decoder modules come from the catalogue, on a
press, into a per-user directory (section 4).

Measured on the 0.96.2 build: 3,397,686 bytes, 107 files.

---

## 3. Where FoxSDR writes, and what a package does to that

### 3.1 The rule for the package directory, which is not in doubt

> | Write inside the package | Not allowed. The package is read-only. | Writes under `C:\Program Files\WindowsApps\<package_full_name>` aren't allowed. |

and

> "After deployment, package files are marked read-only, and are heavily
> locked down by the operating system (OS). Windows prevents apps from
> launching if those files are tampered with."

— `learn.microsoft.com/windows/msix/desktop/desktop-to-uwp-behind-the-scenes`
(`ms.date` 2025-09-09, `updated_at` 2026-01-28).

FoxSDR never writes into its install directory at runtime, so nothing about
the product has to change for this. The one place it *would* have touched the
package directory is the plugin-directory write probe, and that is now skipped
— section 4.

### 3.2 AppData redirection — READ THIS BEFORE ASSUMING IT HAPPENS

It is widely repeated (and was the starting assumption of this work) that a
packaged desktop app's `%APPDATA%` and `%LOCALAPPDATA%` writes are virtualised
into a per-package private location. **The current Microsoft documentation says
that applies only to app-container apps, which FoxSDR is not.**

The same page divides packaged apps two ways. By runtime behaviour:

> - One type includes both WinUI 3 apps … and Desktop Bridge apps (Centennial).
>   Declared with `uap10:RuntimeBehavior="packagedClassicApp"`.

and by trust level:

> - A *full trust* app. Declared with `uap10:TrustLevel="mediumIL"`.
> - An *appContainer* app. Declared with `uap10:TrustLevel="appContainer"`.
>   Runs in a lightweight app container.

and then says what virtualising is and who gets it:

> "But a key goal of an *appContainer* app is to separate app state from system
> state as much as possible… Windows accomplishes that by detecting and
> redirecting certain changes that it makes to the file system and registry at
> runtime (known as *virtualizing*). **We'll call out when a section applies
> only to virtualized apps.**"

Every one of the following sections carries the line "This section applies only
to virtualized apps": *AppData operations on Windows 10, version 1903 and
later*, *AppData operations on OSes earlier than…*, *Working directory, and
application files*, the whole *Registry* section, and *Uninstallation*.

FoxSDR's manifest declares `uap10:TrustLevel="mediumIL"` — it must, because it
opens USB devices with `WinUsb_Initialize`, loads native plugin DLLs, opens
serial ports and `LoadLibrary`s the SDRplay API from outside the package. So
**on the current documentation, FoxSDR's AppData writes are NOT redirected**
and a Store copy reads and writes the same `%APPDATA%\foxsdr\config.json` and
`%LOCALAPPDATA%\FoxSDR` as the installer copy.

**This was not measured** — see section 9 — and it is the single most important
thing to measure before submitting, because two opposite designs follow from
it:

* **If AppData is NOT redirected** (what the documentation says): a user with
  both the Store copy and the website copy shares one config, one plugin
  directory and one log. That is arguably the better outcome — no surprise
  "where did my bookmarks go" — but it means **uninstalling the Store package
  does not remove the user's data**, and the two copies can fight over
  `config.json` if both are open.
* **If AppData IS redirected** (the older, widely-assumed behaviour): the Store
  copy starts with a blank profile, and uninstalling it cleans up. The user
  then has two FoxSDRs that cannot see each other's settings, which needs
  saying in the listing.

The test that settles it is three lines and is written out in section 9.

### 3.3 Where each thing goes

Verified by reading the code, not assumed:

| What | Where | Source | Under a package |
|---|---|---|---|
| Settings, bookmarks, receiver position, plugin permissions | `%APPDATA%\foxsdr\config.json` | `ConfigStore::defaultPath()`, `src/core/config.cpp:153` — `getenv("APPDATA")` | see 3.2 |
| Logs, crash dumps | `%LOCALAPPDATA%\FoxSDR\logs`, `…\crashes` | `diagBaseDir()`, `src/core/diag_log.cpp:63` — `getenv("LOCALAPPDATA")` | see 3.2 |
| Installed plugins and their caches | `%LOCALAPPDATA%\foxsdr\plugins` | `PluginHost::userPluginDir()`, `src/core/plugin_host.cpp:1012` | **always this, now** — section 4 |
| Band plans | `<install dir>\resources\bandplans` | `BandPlan::defaultDir()`, `src/core/band_plan.cpp:79` | read-only inside the package, which is all it needs |
| **Recordings** | `%USERPROFILE%\Documents\SDR-recordings` | `src/gui/app_window.cpp:763` | **not AppData, so not virtualised on any reading of the docs** |

**Documents is not virtualised**, and that is worth stating plainly because the
question was asked directly. The behind-the-scenes page's redirection table
covers `AppData` and the `VFS`-mapped well-known folders (`System32`,
`Program Files`, `Windows`, `ProgramData` and the `System32` sub-trees). The
user's Documents folder is in neither list, and the rule that does cover it is:

> | Write outside the package | Allowed if the user has permissions. |

So a recording written by a packaged FoxSDR lands in the user's real
`Documents\SDR-recordings`, where they expect it, and survives an uninstall.
That is the right behaviour and needs no change.

---

## 4. Store-mode detection in the app

`src/core/package_identity.{hpp,cpp}` asks one question —
`GetCurrentPackageFullName` from `kernel32` (declared in the SDK's
`appmodel.h`; `APPMODEL_ERROR_NO_PACKAGE` is `15700`) — and two behaviours hang
off it.

The API is resolved with `GetProcAddress` rather than linked, so a system
without the app-model exports reports "unpackaged" instead of failing to start.
The mapping from the return code to a decision is a pure function
(`identityFromApiResult`), and an **unknown** code means unpackaged: that is the
answer that cannot break the ordinary installed build, which is every copy in
the world today.

### 4.1 The update check stands down

`AppWindow::startUpdateCheck()` consults
`updateCheckDisposition(packaged, userEnabled)` and, when packaged, never
starts. The log carries, once per launch:

    update check: running from a Store package - the Store delivers updates

and the Settings > Updates row shows a `STORE` chip, no checkbox (there is
nothing for the user to decide), the package full name, and:

> this copy came from the Microsoft Store, which delivers its updates — so
> FoxSDR does not check foxsdr.com and nothing is downloaded here

**Why it must stand down rather than merely be pointless.** The check downloads
`foxsdr-setup-<ver>.exe` and hands it to the shell. Run from inside a package
that would install a *second, unpackaged* FoxSDR beside the packaged one: two
products, two install directories, two update paths, and — given section 3.2 —
possibly one shared config between them.

The CLI `--update-check` is deliberately **not** suppressed. It is a support
tool the user invoked explicitly; the thing that must not happen on its own is
the startup check.

### 4.2 The plugin directory, and the probe that no longer runs

`PluginHost::defaultPluginDir()` normally chooses between the exe-adjacent
directory and `%LOCALAPPDATA%\foxsdr\plugins` by **writing a probe file**,
because an installed program directory looks perfectly writable until something
tries. Inside a package that probe would be a write into
`C:\Program Files\WindowsApps\<full name>`.

So under a package the probe is **skipped, not ignored**:

```cpp
if (cascade::core::runningInPackage() && !userDir.empty()) {
    return choosePluginDir(exeDir, userDir, false, true);
}
return choosePluginDir(exeDir, userDir, directoryIsWritable(exeDir), false);
```

The probe would have failed harmlessly, but it would still have been an
attempted write into a directory the OS protects, on every launch. The pure
`choosePluginDir` now takes a fourth `packaged` argument that defaults to
`false`, so the three rows pinned at `tests/test_plugin_host.cpp:909-914` keep
their exact meaning; `tests/test_package_identity.cpp` repeats them verbatim
and adds the packaged rows.

The directory is now logged at every scan, which it never was:

    plugins: <dir>                                  (unpackaged)
    plugins: <dir> (this is a Store package: <pfn>)  (packaged)

### 4.3 The test seam

`setPackageIdentityForTest()` for the unit tests, and the environment variable
`FOXSDR_FAKE_PACKAGE=<full name>` for a child process (`none` forces the
unpackaged answer). The environment hook is safe because both behaviours it can
reach are fail-safe: it can stop an update check (a loss, not a hazard) and it
can move the plugin directory to the per-user one, which the unpackaged build
already uses whenever it is installed to Program Files. It cannot grant
anything, load anything, or send anything. Contrast `CASCADE_CONFIG_TEST`,
which is honoured **only** under `--frames` precisely because redirecting a
real session's config file *would* be a hazard.

---

## 5. The manifest, decision by decision

`AppxManifest.xml` is a template; `build-msix.ps1` substitutes the
at-sign-delimited placeholders and **refuses to pack if any are left**. It also
parses the result as XML before handing it to `makepri`, because `makepri`'s
answer to a malformed manifest is `PRI191: Appx manifest not found or is
invalid` buried in a screenful of usage text, with the real reason printed
separately and UTF-16 encoded. (The reason, twice, while writing this: an XML
comment may not contain a double hyphen, and this manifest's comments wanted to
mention command-line switches.)

### 5.1 Identity — Partner Center assigns it

`Name` and `Publisher` are issued when the product name is reserved and are not
free text:

> "If you've reserved your application name in the Microsoft Store, you can
> obtain the Name and Publisher by using Partner Center."
> — `.../desktop-to-uwp-manual-conversion` (`ms.date` 2023-01-04, `updated_at`
> 2026-04-15)

> "Values in the manifest are case-sensitive. Spaces and other punctuation must
> also match."
> — `.../msix/app-package-requirements`

Until then the script substitutes `FoxSDR.Test.Unsubmittable` and a local test
publisher, and says so in yellow on every run.

### 5.2 Version — the package major is the product major plus one

> "For Windows 10 or Windows 11 (UWP) packages, the last (fourth) section of
> the version number is reserved for Store use and must be left as 0 when you
> build your package… The other sections must be set to an integer between 0
> and 65535 (**except for the first section, which cannot be 0**)."
> — `.../msix/app-package-requirements` (re-read 2026-09-15, `updated_at`
> 2026-08-24; the sentence is unchanged)

**FoxSDR is 0.x, so `0.96.4.0` cannot be submitted.** Decided 2026-09-15, for
the first submission: the package version is the product version with the
first section **plus one** — product `0.96.4` packs as `1.96.4.0`, product
`0.97.0` as `1.97.0.0`, and product `1.0.0` will pack as `2.0.0.0`. Two
properties made this the choice over bumping the product to 1.0.0 (which
would have been a release of its own, and the listing says "still pre-1.0")
or an ad-hoc number: it is **monotonic across the 0.x → 1.0 boundary**, which
the Store's "highest package version wins" rule requires, and it is
**mechanical**, so a package version reads back to exactly one product
version. The number the Store shows and the number in the About line therefore
differ by one in the first section, and only there. `build-msix.ps1` still
takes the version from `cascade.exe`'s own version output, so the package
cannot be stamped with a number the binary does not report.

### 5.3 Application element

    uap10:RuntimeBehavior="packagedClassicApp"
    uap10:TrustLevel="mediumIL"
    desktop4:SupportsMultipleInstances="true"

The first two are the documented form for a packaged Win32 app and force
`MinVersion="10.0.19041.0"` ("uap10 was introduced in Windows 10, version 2004
(10.0; Build 19041)"). The third is **required by the schema** for the console
execution alias below — `makeappx` refuses the package without it — and is also
true of the product: FoxSDR has never been single-instance.

### 5.4 Capabilities, and the honest reason for two of them

`runFullTrust` is **required**:

> "a Medium IL app *needs* to declare the **runFullTrust** restricted
> capability… A *full trust* app is one that sets `uap10:TrustLevel` to
> *mediumIL*"
> — `learn.microsoft.com/windows/apps/package-and-deploy/app-capability-declarations`
> (`ms.date` 2026-09-08, `updated_at` 2026-09-08)

`internetClient` and `privateNetworkClientServer` are **not** required, and
saying otherwise would be wrong. The same page:

> "The **internetClient** and **enterpriseAuthentication** capabilities grant an
> application the ability to perform certain operations that the user can
> already do. So those are examples of capabilities that apply only to
> AppContainer apps. Conversely, a Medium IL app is already running as the
> user; so an app like that can already perform those operations without
> requiring those capabilities."

They are declared as **disclosure**: the Store shows a product's declared
capabilities to the customer on the listing page, and a radio receiver that
talks to the internet and to the local network should say so there rather than
leave it to be discovered. What each covers in FoxSDR:

**`internetClient`** — "apps can receive incoming data from the Internet.
Cannot act as a server. No local network access."
* the usage report and crash reports (`telemetry.foxsdr.com`)
* the plugin catalogue and plugin downloads (github.com)
* the update check — which stands down inside the package, but the code path
  is in the same binary
* satellite element sets from CelesTrak
* basemap tiles for the map view

**`privateNetworkClientServer`** — "provides inbound and outbound access to
home and work networks through the firewall… On Windows, this capability does
not provide access to the Internet."
* the ADALM-Pluto, a USB-attached network device at 192.168.2.1 driven over
  IIOD
* the web remote (`src/net/web_server.cpp`) and the CAT server
  (`src/net/cat_server.cpp`) — both **off** by default and bound to 127.0.0.1
  when switched on; this capability covers a user who deliberately widens the
  binding to the LAN
* USRP/UHD devices reached over Ethernet

**Not declared, deliberately: no location capability.** FoxSDR's receiver
position comes from a serial NMEA port the user names and opens by hand
(`src/core/gps_reader.hpp`), never from the Windows Location Service, and it is
never transmitted. Declaring a location capability would tell the customer
something untrue about where the position comes from. (Store Policy 10.5.7 is
about location collected *other than* through the Location Service API; the
privacy page and the certification notes are where that belongs — see the
Store-submission research notes.)

### 5.5 File types and protocols: none, checked

**FoxSDR registers no file type association and no URI scheme**, so the
manifest declares none. Checked rather than assumed: `installer\cascade.iss`
has no `[Registry]` section at all, and nothing in `src/` writes an
`HKCR`/`HKCU\Software\Classes` key or registers a protocol. Recordings
(`.wav`/`.iq`) and band plans (`.json`) are opened through the app's own file
dialogs. Declaring an association the unpackaged product does not have would
make the Store copy behave differently from every other copy.

### 5.6 The one extension: an execution alias

A packaged app is normally launched through the shell
(`shell:AppsFolder\<family>!FoxSDR`), which needs nothing declared. But FoxSDR
has a real command-line surface that support and testing depend on (`--version`,
`--frames N`, `--selftest`, `--soapy-check`, `--rtlsdr-check`,
`--update-check`), and a shell activation gives it neither arguments nor a
console.

    <uap5:Extension Category="windows.appExecutionAlias" …>
      <uap5:AppExecutionAlias desktop4:Subsystem="console">
        <uap5:ExecutionAlias Alias="foxsdr.exe" />

`desktop4:Subsystem="console"` matches the binary: `cascade.exe` is linked
`/SUBSYSTEM:CONSOLE` (PE Subsystem field = 3, read from the header), so it
writes its diagnostics to an inherited console rather than to nowhere. This is
also the mechanism by which a packaged run can be driven and measured at all —
see section 9.

---

## 6. Logo assets

`make-logos.py` imports `render(size)` from
`resources/icon/generate_icon.py` and draws every asset **at its own size**.
It does not resample the checked-in 256 px PNG, for a concrete reason: the
generator deliberately simplifies the mark below 32 px (four spectrum bars
become two, because four bars plus gaps across ~7 usable pixels is under a
pixel each), and a resample cannot know that — it would ship an unreadable
16 px icon.

It is the **same rendition** as the `.ico` the installer ships, the GLFW window
icon and the site's mark: padding, tile and all. Cropping the mark to its
bounding box to make it read larger at small sizes was tried on the website
favicon and rejected; the icon a user recognises has to be one icon
everywhere.

Sizes come from
`learn.microsoft.com/windows/apps/design/iconography/app-icon-construction`
(`ms.date` 2026-07-21, `updated_at` 2026-08-05):

| Manifest entry | Base | 100% | 125% | 150% | 200% | 400% |
|---|---|---|---|---|---|---|
| `Square44x44Logo` | 44 | 44 | 55 | 66 | 88 | 176 |
| `Square71x71Logo` | 71 | 71 | 89 | 107 | 142 | 284 |
| `Square150x150Logo` | 150 | 150 | 188 | 225 | 300 | 600 |
| `Square310x310Logo` | 310 | 310 | 388 | 465 | 620 | 1240 |
| `Wide310x150Logo` | 310x150 | 310x150 | 388x188 | 465x225 | 620x300 | 1240x600 |
| `StoreLogo` | 50 | 50 | 63 | 75 | 100 | 200 |

What that page marks **required to publish to the Store**: the `StoreLogo`
scale set ("Package logo (Microsoft Store logo) … These assets are required to
publish to the Microsoft Store"); the medium tile at 100% ("Windows 11 does not
use the tile assets, but currently at minimum the Medium tile assets at 100%
are required to publish to the Microsoft Store"); and the app-list target sizes
16/20/24/30/32/36/40/48/60/64/72/80/96/256 with their `_altform-unplated`
copies —

> "If you do not include the targetsize-\*-altform-unplated assets above your
> icon will scale to a smaller size and will get an undesirable backplate
> behind the icon on Taskbar and Start."

84 PNGs in total. The unplated copies are byte-identical to the plated ones,
deliberately: FoxSDR's mark is a filled slate tile with the fox cut into it, so
it is already its own plate, and supplying them is what stops Windows adding a
second one. The script reopens every file it wrote and asserts its dimensions
match the name it was given, because a silently mis-sized asset is the kind of
thing certification rejects days later.

`makepri` is **not optional**: the manifest names `Assets\Square44x44Logo.png`,
and the eighty-odd qualified variants beside it are only ever selected through
`resources.pri`.

`BackgroundColor="#1B1F26"` is `SLATE` from `generate_icon.py` — the same tile
colour the `.ico` and the window icon are drawn on.

---

## 7. Installing it locally to test — and the one thing that needs an
administrator

Sideloading a signed MSIX is allowed on this machine by default. What is not
allowed is trusting a self-signed certificate, and **that needs an elevated
shell**, because the package deployment service reads the *machine* trust
store, not the user's.

Measured on this machine (Windows 11 Pro 22631, standard non-elevated session):

* `Import-Certificate … -CertStoreLocation Cert:\CurrentUser\TrustedPeople`
  succeeds, and then `Add-AppxPackage` still fails with
  **`0x800B0109 — A certificate chain processed, but terminated in a root
  certificate which is not trusted by the trust provider`**. A per-user
  TrustedPeople entry is not enough.
* `Add-AppxPackage -Register …\Layout\AppxManifest.xml` (the documented
  "test before packaging" route) fails with **`0x80073CFF — To install this
  application you need either a Windows developer license or a
  sideloading-enabled system`**, because Developer Mode is off
  (`HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock` does not
  exist) and turning it on is also an administrator action.

Note which error came first: the signed package got past the *policy* check and
failed only on *trust*. So **one elevated command is the whole unlock**:

    # In an ADMINISTRATOR PowerShell. This adds a self-signed code-signing
    # certificate to the machine's Trusted People store. It is a trust-store
    # change: do it knowingly, and undo it below.
    Import-Certificate -FilePath "installer\msix\Output\FoxSDR-LOCAL-TEST.cer" `
                       -CertStoreLocation Cert:\LocalMachine\TrustedPeople

Then, back in an ordinary shell:

    Add-AppxPackage -Path "installer\msix\Output\FoxSDR-0.96.2-x64.msix"
    Get-AppxPackage FoxSDR.Test.Unsubmittable | Select-Object PackageFullName, InstallLocation

### Undo all of it

    Get-AppxPackage FoxSDR.Test.Unsubmittable | Remove-AppxPackage

    # elevated:
    Get-ChildItem Cert:\LocalMachine\TrustedPeople |
        Where-Object { $_.Subject -like "*FoxSDR MSIX Test Signing*" } | Remove-Item

    # ordinary - AND NOTE THE THIRD STORE. signtool prints "Done Adding
    # Additional Store" and quietly drops a copy of the signing certificate
    # into CurrentUser\CA as well, which a My + TrustedPeople sweep misses.
    # Search the whole hive rather than naming stores:
    Get-ChildItem Cert:\CurrentUser -Recurse |
        Where-Object { $_.Subject -like "*FoxSDR MSIX Test Signing*" } |
        ForEach-Object { Remove-Item -Path $_.PSPath -Force }

The test certificate expires 30 days after it is made regardless.

**Current state of this machine (2026-09-15):** no FoxSDR certificate exists in
any CurrentUser or LocalMachine store, and no FoxSDR appx package is
registered. The signed package and its `.cer` are on disk in `Output\`, and the
signature is self-contained (the chain is embedded in `AppxSignature.p7x`), so
the elevated `Import-Certificate` of that `.cer` plus `Add-AppxPackage` is all
that is still needed — the private key is gone, but nothing needs it unless the
package is rebuilt, and `build-msix.ps1 -TestSign` makes a fresh one if so.

---

## 8. Driving the packaged app once it is installed

**Close it through its window, never `Stop-Process`.** FoxSDR counts an
unclean exit as a crash (`telemetryCrashes`), so every forced kill inflates the
owner's own figures — that has happened before, and the rows cannot be deleted.

**Black-hole the endpoints before any launch.** A development build posting to
the live endpoint pollutes the product analytics, and Analytics Engine is
append-only:

    $env:FOXSDR_TELEMETRY_URL = "http://127.0.0.1:9"
    $env:FOXSDR_CRASH_URL     = "http://127.0.0.1:9"

Both are read with `GetEnvironmentVariableA` (`src/core/telemetry.cpp:221`,
`src/core/crash_upload.cpp:682`), i.e. from the OS copy, so a process that
inherits the environment gets them. **Whether a packaged app launched from
PowerShell inherits the environment at all is the thing to check first**, and
the execution alias is the reason there is a way to check:

    foxsdr --version          # does the alias work, and is there a console?
    foxsdr --frames 60        # hermetic: no config is loaded or saved

`--frames` runs are hermetic by design (`src/main.cpp:1142` — a bounded run
uses no config unless `CASCADE_CONFIG_TEST` is set), so they can be run before
anything is known about where the packaged app writes.

---

## 9. What is proven, and what is not

### Proven on this machine, 2026-09-15, FoxSDR 0.96.2 (commit e188ac4)

* The package **builds**: `makepri` indexes 84 assets, `makeappx` packs 107
  files into `FoxSDR-0.96.2-x64.msix` (3,397,686 bytes) and its **schema
  validation passes** — `makeappx` rejected two earlier manifests with named
  errors, so this is a check that can fail.
* The package **signs and verifies as signed**: `Get-AuthenticodeSignature`
  names the signer and reports the only defect as the untrusted self-signed
  root, which is the expected and correct state for a test certificate.
* The identity is **well formed**: `Add-AppxPackage` computed the full name
  `FoxSDR.Test.Unsubmittable_0.96.2.0_x64__7vfa77ky0r4tt` before it reached
  the trust check.
* Every logo asset is **the size its filename claims** (84/84).
* The **packaged code paths run**, exercised through the `FOXSDR_FAKE_PACKAGE`
  seam in a child process with `APPDATA`, `LOCALAPPDATA` and `FOXSDR_DIAG_DIR`
  redirected to a scratch tree and both endpoints black-holed:
  * `update check: running from a Store package - the Store delivers updates`
    in the log of an interactive launch, and no request to foxsdr.com;
  * `plugins: …\Local\foxsdr\plugins (this is a Store package: …)` — the
    per-user directory, chosen without a write probe;
  * `web: serving on port 8074`, and `GET http://127.0.0.1:8074/` answered
    HTTP 200 (8,640 bytes of HTML) and `/api/status` answered HTTP 200 with the
    receiver state;
  * the instance was closed through its own window, addressed by the PID that
    was started (never by process name — another FoxSDR was running on this
    desk at the time, and a name-based harness would have closed theirs).
* The suite: **123 tests, 122 pass.** The one failure is `test_soapy_enum_proc`
  and it is a **pre-existing flake unrelated to any of this**: on an md5-pinned
  binary with identical input it fails roughly three runs in five, always on
  the single wall-clock assertion `CHECK(r.elapsedMs >= 1500u)` against a
  1500 ms budget (`tests/test_soapy_enum_proc.cpp:845`), and it behaves the
  same way on the 0.96.0 build and the 0.96.2 one. That test references nothing
  changed here.
* The **RTL-SDR opens from an ordinary process** on this build: RTL2838UHIDIR
  serial 00000001 bound to WinUSB, R820T tuner, 7,225,344 samples in 3.0 s,
  0 timeouts, `PASS`. That is the control the packaged run has to be compared
  against.

### NOT proven — the package was never installed

**An administrator is needed and was not available** (section 7). Everything
below is therefore open, and none of it should be described as working:

1. **WinUSB from inside the package.** The whole product turns on this. A
   full-trust `mediumIL` app runs outside an app container, so
   `SetupDiGetClassDevsW` → `CreateFileW` → `WinUsb_Initialize` should behave
   exactly as unpackaged — **but that is reasoning, not a measurement.** The
   test, once installed, is one line: `foxsdr --rtlsdr-check`, compared against
   the control above.
2. **Whether AppData is redirected** (section 3.2). The test:
   `foxsdr --frames 3` interactively once, then look for
   `%LOCALAPPDATA%\Packages\<family>\LocalCache\Roaming\foxsdr\config.json`.
   If it is there, writes are virtualised; if instead the real
   `%APPDATA%\foxsdr\config.json` changed its timestamp, they are not. **Record
   the real file's size and mtime before doing this**, and be ready to restore
   it.
3. **Whether environment variables reach a packaged app** launched from
   PowerShell through the execution alias. If they do not, there is no way to
   black-hole telemetry for an interactive packaged launch, and the only safe
   interactive test is `--frames`, which arms no heartbeat at all
   (`src/gui/app_window.cpp:1246` — heartbeats are interactive-only).
4. **The plugin catalogue over the network** from inside the package.
5. **Band plans loading from the read-only package directory.** They are
   readable by every reading of the docs; it has not been observed.
6. **The web remote from inside the package**, including whether Windows
   prompts for a firewall exception differently for a packaged app.
7. **The SDRplay API**, loaded by `LoadLibrary` from outside the package.
8. **Uninstall behaviour** — what `Remove-AppxPackage` does and does not take
   with it, which is item 2 restated.

---

## 10. Partner Center: the exact steps for the MSIX route

Only the owner can do these; they involve identity, money or a legal
commitment.

1. **Open a developer account, starting at
   `https://storedeveloper.microsoft.com` and nowhere else.** The registration
   fee is currently waived in that flow for both account types, and only in
   that flow: "You must begin your journey at
   https://storedeveloper.microsoft.com. **This is the only supported entry
   point for the new flow.** Other paths … will show the **legacy flow**"
   (`.../publish/partner-center/open-a-developer-account`, `ms.date`
   2025-09-17, `updated_at` 2026-07-17). **Individual or Company cannot be
   changed later**; Store Policy 10.14 and the commercial licence point at
   Company.
2. **Reserve the product name "FoxSDR"**, in *Create a new app*. This must
   happen **before** the package is built: the identity is baked into the
   manifest.
3. **Copy the identity into the build.** Partner Center → your app →
   *Product management* → **Product identity**. Take
   `Package/Identity/Name` and `Package/Identity/Publisher` and pass them to
   the script:

       .\installer\msix\build-msix.ps1 -Force `
           -IdentityName      "<Package/Identity/Name>" `
           -IdentityPublisher "<Package/Identity/Publisher>"

   `Package/Properties/PublisherDisplayName` from the same page goes in
   `-PublisherDisplayName` if it differs from "Steven Fox".
4. **Settle the version question** (section 5.2) — `0.96.2.0` is not a
   submittable version because the first section cannot be 0.
5. **Create the submission and upload the `.msix`** on the *Packages* page.
   **Do not sign it.** Microsoft re-signs it after certification.
6. **Declare the non-Microsoft driver dependency in the certification notes.**
   This is the most likely cause of a certification failure for this app and it
   applies to both routes. Store Policy 10.2.4: "If your product has a
   dependency on non-Microsoft provided driver(s) or NT service(s), **you must
   disclose that dependency to Microsoft in the certification notes in
   Microsoft Partner Center**", and the dependency must also be disclosed "at
   the beginning of the description in metadata". FoxSDR's three: WinUSB
   binding via Zadig (WinUSB itself is Microsoft's in-box driver, which helps),
   the SDRplay API service, and UHD's driver for a USRP.
7. **Describe the plugin catalogue in the Store description itself**, not only
   in the notes. Store Policy 10.2.2 forbids dynamic code inclusion "not
   consistent with the described functionality" — the test is consistency with
   what is *described*, so the catalogue must be described. And **do not use
   the word "store" for it in the listing metadata**: if a certifier reads it
   as a storefront, Policy 11.13 attaches and brings an obligations list nobody
   wants. The in-app window can keep its name.
8. **Put the PolyForm Noncommercial text (or a URL) in *Additional license
   terms*.** App Developer Agreement v8.11 §4(i): without it, Microsoft's
   Standard Application License Terms apply instead.
9. **Privacy policy URL** — mandatory. Policy 10.5.1: "Product types that
   inherently have access to Personal Information must always have privacy
   policies. These include, but are not limited to, Desktop Bridge and Win32
   products." A packaged FoxSDR *is* a Desktop Bridge product, so this is now
   true on the face of the policy, not by inference.

   The **in-app** half of this is already done as of 0.96.1: App Developer
   Agreement v8.11 §4(h) wants "a prominent link to your privacy policy in a
   reasonable location, such as within your App (in addition to a link on the
   Store product detail page)", and `src/core/telemetry.hpp` now carries
   `kPrivacyPolicyUrl = "https://foxsdr.com/privacy.html"` as one constant so
   the panel, the listing and the site cannot drift apart. **Enter that exact
   URL in Partner Center**, and make sure the page is current before
   submitting, because the submitted URL is checked.
10. **IARC age rating questionnaire**, mandatory under Policy 11.11.
11. **Submit**, and expect up to three business days. Once publishing begins it
    cannot be cancelled.

### What changes for the user, and should be said in the listing

* The Store delivers updates; the in-app update check is off in this build
  (section 4.1). That is the opposite of the EXE route, where the Store updates
  nobody and the in-app check is the only channel.
* Plugins install into `%LOCALAPPDATA%\foxsdr\plugins`.
* Depending on the answer to section 3.2, a user who also has the website build
  either shares their settings with it or does not. Say which, once it is
  known.

---

## 11. Files, and what the app build needed

New, in `src/core`:

* `package_identity.hpp` / `package_identity.cpp` — the query, the pure
  mapping, the two decisions, the seams.

Changed:

* `src/core/plugin_host.hpp` / `.cpp` — `choosePluginDir` gains a fourth
  `packaged` argument defaulting to `false`; `defaultPluginDir()` skips the
  write probe under a package.
* `src/gui/app_window.cpp` — the update check stands down and logs why; the
  Settings > Updates row gains a `STORE` state with no checkbox; the plugin
  directory is logged at every scan.

Tests:

* `tests/test_package_identity.cpp` — 42 checks. The pure decisions are
  covered in every branch; the live query is asserted in the only direction an
  unpackaged ctest run can prove (it must answer "not packaged"); the seams are
  exercised; and the three pre-existing `choosePluginDir` rows are repeated
  verbatim, because "the new default did not change the old answers" is the
  property that makes adding the argument safe.

No change to `CMakeLists.txt` was needed — both `src/core/*.cpp` and
`tests/test_*.cpp` are globbed with `CONFIGURE_DEPENDS`. (A file added after a
configure is registered but not built in the same pass with the Visual Studio
generator, so re-run the configure, or build twice.)
