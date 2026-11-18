# Drive fixes, group 3 (2026-11-18)

Branch `fix/drive-3`, cut from `fix/outside-resize-2` (b25310f9), worktree `D:/foxsdr-wt/en-screen-k`, build `D:/foxsdr-build/en-screen-k` (Release, incremental, `--parallel 3`), hidden desktop `FoxSDRDriveFix3`, scratch `D:/foxsdr-build/df3-scratch/`. Nothing pushed. Source: `D:/foxsdr-build/drive-final/handover-part-07.txt` to `-12.txt`.

## THE THEME: a list handed over as a new model rebuilds its rows, and a press made on a row just before is lost

The host published every list as a NEW model object (the radar scope every 33 ms, the map and plugin pages on every change of a page's signature). A `for` row in a .slint file is rebuilt when its list is a different model object; a row rebuilt between a real press and its release never hears the click. A scripted click presses and releases inside one tick; a hand takes 50 to 150 ms.

Proof (`test_slint_scope_modes`, new block): press on a radar SYS row, publish the model again while the button is down, release. A fresh model: 0 clicks (what the host did). `reuseModel` (model_reuse.hpp): 1 click. Both lines are printed by the test.

Fix: `model_reuse.hpp` keeps the model object and changes only the rows that differ, in place. Used by `scope_publish.cpp` (radar contacts, flight lines, options) and `kit_publish.cpp` `keepPageModels` (every list of a page tab, the map's targets, legend, list rows, layers, grid labels).

Covered: radar SYS PHOSPHOR / SWEEP / TRAILS / FILTER (L1608-1611), the map Layers switches (L4901), Satellites map list rows (L1728). NOT covered, own cause needed: the "Stop plugin" key on the mute banner (not a list row; the banner now also leaves the layout, item 10, which removes a shift but is not shown to cure the lost click), the command palette first click, and "a click straight after another control is swallowed" (not reproduced). The in-process drive cannot show the fix on the real window: UNVERIFIED on a real pointer.

## The thirteen

| # | Reproduced | Cause (file) | Verdict |
|---|---|---|---|
| 1 | by reading; the probe was NOT run (it raises the prompt) | `engine.cpp` scanSoapy asked UHD with no hint whenever a USRP was on USB (the tick only decided the no-USRP case) and always asked remote / netsdr / redpitaya; UHD find broadcasts on the network. The Patch inspector's Device list goes through the same scanSoapy, so it passed the same switches as the SIGNAL bank; the switches were not enough. | FIXED: `device_scan_plan.hpp` soapyDriversToLeaveOut / soapyUhdUsbOnly, `soapy_enum_proc.*` usbOnlyDrivers + env `FOXSDR_SOAPY_USB_ONLY`, `soapy_source.cpp` findHintFor (UHD hint type=b200). Installer: `cascade.iss` has no firewall rule, so an installed copy would show the prompt the first time a scan asks UHD on a machine with a USB USRP, or SoapyRemote; now it does not unless network radios are asked for. UNVERIFIED: that UHD honours type=b200 through SoapyUHD and still finds the B200 (read-only reasoning; running it can raise the prompt). |
| 2 | YES (host test) | `patch_logic.cpp` infoModel took the engine's fact, which stays runnable beside a refused plugin (API 0.3 8.12) | FIXED in the sentence only; RED/GREEN `test_slint_host_patch` |
| 3 | sentence: yes; panel scroll: NO | the refusal appeared only after the apply key; `lanem_publish.cpp` now says it beside the fields as typed. The panel stopping at Output is probably `Flickable viewport-height: list.preferred-height` (main.slint) under-counting wrapped text at the panel's width: not shown | sentence FIXED; scroll NOT DONE |
| 4 | by reading | Remove group: `tools_bank.slint` asks first ("Yes, delete this group and its N bookmark(s)"); the Legacy window deletes at once with no question, so this adds the question. Presets: `kit_publish.cpp` forget needs two presses (tooltip still says right-click forgets: still true) | FIXED; drive `tools-bookmarks-rows` |
| 5 | NO | the code path (committed then accepted, host draft read synchronously) looks right; the drive's synthetic Enter works. Enter now hands over the typed text itself | NOT REPRODUCED, defensive change |
| 6 | YES by reading | `system_publish.cpp` cleared the host copy only, never the strip's `PointerLedger.line` | FIXED; drive `wg2-diagnostics` now reads the strip's line |
| 7 | YES (test) | `tooltip.slint` TipTimer kept `hovered` true when the item moved under a resting pointer | FIXED; `test_slint_elided` RED (tip STAYS) with the old file, GREEN now |
| 8 | by reading | engine takes any text; `kit_publish.cpp` now refuses, with a sentence under the field, and the field goes back (`pages.slint` callback returns bool) | FIXED; drive `decode-store-catalogue` |
| 9 | by reading | `presetNote_` was one string for every entry; now keyed by file | FIXED |
| 10 | by reading | banner sat in the layout; now an overlay above the status bar (`main.slint`) | FIXED |
| 11 | by reading | module page used the install record's ABI only; a loaded module has the build's | FIXED; `test_slint_host_kit` |
| 12 | by reading | LineInput / FieldInput did not select all on focus; they do now for number (mono) fields | FIXED, not shown on a real click (UNVERIFIED that the select survives the click that gives the focus) |
| 13 | NO | the drag code (`on_resize_drag`) uses relative deltas with no position or work-area clamp. The pointer cannot leave the screen, and the 12 px grip may have been missed on the second monitor | NOT REPRODUCED |

## Owner reports (nothing changed)
- Decoders woken unasked: `kit_logic.cpp` pluginUseText marks EVERY track source "radar scope" while the scope shows (line 536; 11 woke) and EVERY decoder "decoder output" while an Output page is open (537 with `kit_publish.cpp` 207-209; 65 woke); `engine_plugin_run.cpp` 395-405 turns those marks into wakes, and a manual tune offers audio-output decoders their first play (`playingAudio`, 3 woke). Smallest changes: mark only plugins already awake or pinned to Always for the Output page; mark only the track sources the user chose for the scope; give the tune offer only to the plugin whose preset matches.
- "See PRIVACY.md for the complete list and what is excluded." and the raw field list ("device open  sdr model (serial stripped)", "faulting stack as module+offset") are the Legacy window's own text, copied unchanged into `system_logic.cpp` (lines 931-934 and 1012); they date from 0.49.0 / 0.61.0. PRIVACY.md does not contain them; `config.hpp` calls the in-app Usage text approved. Whether the file name should be named to a user is the owner's call.

## Changed expectations
| Test | Old | New |
|---|---|---|
| drive `tools-bookmarks-rows` | one click on "Remove this group" removes the group | first click asks (group count 1), click "Yes, delete this group and its 1 bookmark(s)" removes it |
| drive `wg2-diagnostics` | `gaps.ledgerline` read the host's copy | reads the strip's line (same expected values) |

## Catalogues
4 new sentences x 33 machine-made; `i18n_keys.py`: 0 missing, 0 stale; `lang_assets.hpp` regenerated. Error-path pin 410 (not raised).

## Run / NOT RUN
At the final code, -j1: test_slint_scope_modes (248 checks), test_slint_elided (19), test_slint_main_screen (70), test_slint_host_patch, test_slint_host_kit, test_slint_host_tools, test_slint_lanem_host, test_device_scan_plan, test_i18n, test_cjk_wrap, test_error_paths: all passed. Drives (hidden desktop): wg2-diagnostics, tools-bookmarks-rows, decode-store-catalogue, lanem-tools, tools-airband: passed. Break-its: item 2, item 11, item 1 RED with the behaviour removed, GREEN restored; item 7 RED with the old tooltip.slint; the theme RED (fresh list 0 clicks). NOT RUN: full gates, the other drives, the clip sweep; RED on the base for the drive changes of items 4, 6, 8 (not built on the old code). config.json sha256 `c21ce98d...03c3` unchanged.

## OPEN ROWS
Panel scroll past Output (item 3), item 5 and 13 need the mouse drive; the real-pointer proof of the theme and item 12; the UHD hint on the B200; the mute banner's "Stop plugin" and palette first click.
