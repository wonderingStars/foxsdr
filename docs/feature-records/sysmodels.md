# Lane `sysmodels` - setup, settings, install, reliability models

Branch `feat/sysmodels` (from engine/multi-radio d3630d78). A new-file model lane: models, OS wrappers and tests in new files; no forbidden file edited.
Every model is standalone and tested through an injected stand-in; nothing is wired into the application (wiring hand-offs below).

## Step 0 - the table (reading only, written before any code)

Classes: `new file model` (built here, standalone), `needs a forbidden file` (the WIRING needs one: model built, exact hand-off written), `already built`, `blocked`.

| # | Row | Class | What was opened and found |
|---|-----|-------|---------------------------|
| 1 | First-run wizard and starter presets | new file model + needs a forbidden file (the pages are `.slint`) | `src/core/radio_setup.hpp` is a one-card check plus `radioSetupDontShow` (config.hpp:243). No wizard, no starter data anywhere. |
| 2 | Portable mode | new file model + needs a forbidden file (`config.cpp` / `ConfigStore::defaultPath` read `%APPDATA%` directly, and `diag_log.cpp`, `plugin_host.cpp` read `%LOCALAPPDATA%`) | The resolver can hand back an environment override pair (APPDATA, LOCALAPPDATA), so the wiring is two `_putenv_s` calls at the top of `main()`; `main.cpp` is not on the forbidden list but is another lane's neighbour, so it is a hand-off. |
| 3 | One-file settings export / import / backup | new file model + UI hand-off | Only the automatic `<file>.<stamp>.bak` copies exist (`config.cpp:1862`). No `export` / `import` / `backup` in `src/slint`, `src/engine`, or the API header. |
| 4 | Whole-app reset | new file model + UI hand-off | Only key-binding reset exists. |
| 5 | Settings profiles per radio / use; multi-PC sync | profiles: new file model. Sync: folder-based planner is new file model; a cloud/service sync is **blocked on an owner decision** (it needs an account and a server, both forbidden to this lane) | Nothing exists. |
| 6 | Telemetry on/off switch in the UI | **already built** (the switch: `src/slint/system_logic.cpp:922` reads `fact.telemetry.enabled`; engine op at `engine.cpp:8442-8446`). The brief still asks for a standalone model and the "off sends nothing" test, so a small model is built. | `PRIVACY.md:234` text about a hand edit is stale; listed. |
| 7 | Signature on the update manifest | new file model (Ed25519 verifier from RFC 8032 + envelope + signing tool) + needs a forbidden file (`updater.cpp`) | `updater.cpp:235-262` checks version, sha256 field, host allow-list. Whether HTTPS alone is enough is the owner's decision. |
| 8 | Code-signed setup | new file model (dormant `installer/sign-setup.ps1`, never run) + needs a forbidden file (`cascade.iss` `SignTool=`); buying a certificate is an owner decision | `docs/MANUAL.md:1862` says "not code-signed yet". |
| 9 | USB re-plug recovery | new file model (device-arrival watcher) + needs a forbidden file (engine). FEATURE-GAPS R10 belongs to the engine serial lane A: standalone watcher built, not wired. | No `WM_DEVICECHANGE` / `RegisterDeviceNotification` anywhere in `src`. The interface classes the engine lists (`src/usb/winusb_device.cpp`): `GUID_DEVINTERFACE_USB_DEVICE` and `GUID_DEVINTERFACE_WINUSB`. |
| 10 | Sleep / resume handling | new file model + needs a forbidden file (engine) | No `PBT_APM*` / `RegisterSuspendResumeNotification` in `src`. |
| 11 | Keep the PC awake while recording / scheduled capture | new file model + needs a forbidden file (recorder, schedule runner) | No `SetThreadExecutionState` in `src`. |
| 12 | CPU / GPU / memory readout; low-end / reduced-effects mode | part **already built** (memory: `process_resources.cpp` reads GDI/USER/handles/private bytes and DXGI video memory for one log line; `engine_measure.cpp:149` reads working set inside a bounded measurement run only). New file model for the live meter (process CPU %, working set, system memory, GPU engine utilisation by counters) and the reduced-effects policy. | No reduced-effects / low-end switch anywhere in `src` (grep). The UI half is `.slint`: hand-off. |

Nothing in the table is `blocked` outright; row 5's cloud sync and the three owner decisions (HTTPS-vs-signature, signing the manifest, buying a certificate) are listed under "Owner decisions" at the end.
