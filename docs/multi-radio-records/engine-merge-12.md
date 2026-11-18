# Engine merge 12 (`engine-merge-12`): the mouse-drive fixes, three groups, and the window guard, onto `engine/multi-radio` (2026-11-18)

Worktree `C:/Users/steve/foxsdr-wt/mr-dsp`, branch `engine/multi-radio`, start tip `17e9a61a` (engine merge 11, clean), build `D:/foxsdr-build/mr-dsp` (Slint ON, clip probe OFF, Release, `--parallel 4`), scratch `D:/foxsdr-build/m12-scratch/`, hidden desktop `FoxSDRMerge12`. Every test and drive ran through the hidden-desktop launcher (`FOXSDR_FRAME_CAP_HZ=60`). No radio opened, nothing transmitted, nothing pushed, downloaded or deployed, no outside request, no other worktree touched, no real scan run, `FOXSDR_ALLOW_VISIBLE_GUI_TESTS` and `FOXSDR_ALLOW_REAL_HARDWARE_TESTS` never set, nothing shown on the visible desktop, no `foxsdr-slint.exe` of anyone else touched. The machine's real `config.json` sha256 is checked at the end (hand-back message).

Because the stamp tests compare the binary's commit with HEAD, THIS RECORD IS THE LAST COMMIT BEFORE THE FINAL REBUILD: the results of the runs at the final build are in the hand-back message and in `D:/foxsdr-build/m12-scratch/` (`f*.log`).

## 1. The merges (tips checked first: `fix/drive-2` `284c92b4`, `fix/drive-3` `dd80786c`, exactly as briefed)

| Commit | Branch | Conflicts |
|---|---|---|
| `3309c7f7` | `fix/drive-2` (contains `fix/drive-1` `b7e2e43c`) | none |
| `bb094ce9` | `fix/drive-3` (contains `fix/outside-resize-2` `b25310f9`) | none: git merged the six files both lanes touched (`lanem_publish.cpp`, `main.cpp`, `main.slint`, `extend_bank.slint`, `signal_bank.slint`, `system_bank.slint`) without a marker; the interplay was read by hand (section 2) |

Later commits: `4a3a32b6` (release notes and the new drive `merge12-typed-select`), `7251ce7f` (four reds fixed, section 5), then this record, the open-rows refresh and two comment lines in `tests/test_engine_publish_alloc.cpp`.

## 2. The particular points

### The publishers: one rule, publisher by publisher
The rule the merged tree follows: **the model object the window holds is never replaced while only the rows' contents change; a changed row is written in place; a new model object is built only when the row SHAPE (count or kinds) changes.** Read, for every `*_publish.cpp` either lane touched:

| File | Lane | What it does now |
|---|---|---|
| `lanek_publish.cpp` | 1 | persistent `VectorModel` per section; same shape: `set_row_data` for the rows that differ (`sameRowShape`); changed shape: the SAME model emptied and refilled |
| `lanel_publish.cpp`, `lanem_publish.cpp`, `lanes2_publish.cpp`, `lanes3_publish.cpp`, `s4_publish.cpp` | 1 | same shape: `set_row_data` in place on the held `models[s]`; changed shape: a new `VectorModel` handed to the window (a shape change is not a press in progress; building a new one is what keeps a typed-in field from showing a stale text). Lane 2 touched `lanem_publish.cpp` only to add a refusal note row, in a different function (no overlap) |
| `signal_publish.cpp` | 1 | selected radio for the Source list and the centre refusal; its models were not changed |
| `kit_publish.cpp` (pages), `scope_publish.cpp` (radar contacts, flight lines, options) | 2 | `reuseModel` / `reuseModelFrom` (`model_reuse.hpp`): the held object kept, differing rows set in place, rows added or erased at the end |
| `kit_publish_mapj.cpp` | 1 | no model change (antenna sentences) |
| `system_publish.cpp`, `tools_publish.cpp` | 2 | one line each (the ledger line, `group_count`); no model change |

No publisher is half-applied; lane 2's `reuseModel` was not applied to lane 1's six publishers because their `sameRowShape` path already keeps the object when the rows keep their shape. Both lanes' press and focus tests ran on the merged tree: `drive2-playing-focus` Passed (51 s), `test_slint_scope_modes` Passed (the press-republish-release block).

### Typed text and select-all in one field
Lane 1 hands typed text over before a key acts (`flush()` / `edited`); lane 2 selects all when a number field takes the focus (`changed has-focus`). New drive `merge12-typed-select`: the clarifier's offset box holds 300; a click into it and the single key steps 1, 2, 0 (the `type` step does its own Ctrl+A and would hide the focus rule) replace the 300; "Set the clarifier" without Enter; the engine's `lm.fact.rit.1.hz` reads 120. Passed. Not break-it tested (a rebuild of the window costs about 30 minutes here); UNVERIFIED as a regression guard, the two lanes' own break-its stand.

### Layout pins, measured once on the merged window, both values
| Where | Before (merge 11) | Measured now | Result |
|---|---|---|---|
| `layout-section-overlap-624`: "Stop bypassing the selection" bottom / "Source"#1 top / "Read the pitch now" bottom / "Sinks"#1 top | 902 / 949 / 2361 / 2549 | 902 / 949 / 2361 / 2549 | pins 902 / 937 / 2361 / 2361 unchanged (read by probe scripts with impossible pins, the failure message prints the value) |
| `layout-section-overlap-1600` | 820 / 867 / 2279 / 2467 | 820 / 867 / 2279 / 2467 | unchanged |
| `test_slint_main_screen` 1440 sheet, `bar-min-624`, `window-strip-1024/1280/1366/1440/1480/1920`, `window-min-strip` | pins | all pass | unchanged (lane 1's caption rule and lane 2's overlay move none of them) |
So no layout pin changed in this merge. Lane 2's mute banner is an overlay and takes no layout room.

### Catalogues
Lane 1 added no key; lane 2 added 4 (all in its own tree). Union by key against the common ancestor `17e9a61a`: nothing to merge. `python tools/i18n_keys.py`: 4892 keys in each of the 33 (4888 + 4), 0 empty, 0 missing, 0 stale. `python tools/embed-lang.py` regenerated gives a `lang_assets.hpp` identical to the committed one apart from line endings. Korean word-spacing, CJK wrap and the developer-words test: inside `test_i18n`, `test_cjk_wrap`, `test_slint_host_mustfixa`, all Passed in the headless gate.

### The radio search change (UNVERIFIED on a real B200)
`engine.cpp` scanSoapy, `device_scan_plan.hpp`, `soapy_enum_proc.*` (USB-only drivers via `FOXSDR_SOAPY_USB_ONLY`), `soapy_source.cpp` (UHD hint `type=b200`): `test_device_scan_plan` Passed. No real scan was run (it can raise a Windows Firewall prompt). It is UNVERIFIED that UHD, given `type=b200`, still finds a USB B200 through SoapyUHD, and that no firewall prompt appears with "Look for network USRPs" unticked: row `DF3-uhd-b200`, to be tried under the owner's eye.

### Approved privacy wording
`git diff --word-diff 17e9a61a HEAD -- PRIVACY.md docs/DIAGNOSTICS.md`: 0 bytes.

### Release notes
Two plain lines in `docs/release-notes-0.99.109.md`: under Radios, "The search for network radios happens only when "Look for network USRPs" is ticked. So Windows does not ask about network access unless you asked for network radios."; under Also, "Removing a bookmark group now asks first." Nothing else changed. Version stays 0.99.109.

## 3. Error-path pin
410 on the base and on both branches. Union key by key against `17e9a61a`: lane 1 added one ALLOW line (`kit_publish_mapj.cpp::KitPublisher::mapJAct::errassign::e`, "UI result text"), lane 2 none; neither added a PENDING site. The gate on the merged tree: "3487 sites: 284 report, 2793 allowed, 410 PENDING" (3486 sites at merge 11 plus the one new allowed site). **Pin 410 before, 410 after: 410 + 0 + 0 = 410.** `test_error_paths` and `errpath_gate_selftest` Passed. (A re-seed run without the survey file lists hundreds of `PENDING:unmatched`; that run was only a probe into a scratch output and its file was discarded, nothing re-seeded.)

## 4. Pinned numbers and changed expectations (every one, both values)

| Where | Was | Now | Why |
|---|---|---|---|
| `test_engine_publish_alloc` F3 bound | 2800 | 2800 | MEASURED rebuilds 2183 to 2546 (max 2546; 2545 at merge 11); 2546 x 1.10 = 2800.6. Comment only |
| same, F12 bound | 2400 | 2400 | MEASURED 2174 to 2175 (2174 at merge 11); 2175 x 1.10 = 2392.5. Comment only |
| `tools/slint-clip-explained.tsv` | `tooltip.slint:118` | `tooltip.slint:124` | lane 2 added six lines to the tip timer above the explained Text |
| `test_slint_tools` "Remove this group" | one press at y 250 sends `bookmark-remove-group` | first press at y 250 sends nothing; second press at y 273 sends it | lane 2: the group removal asks first; the asked words take a line of their own so the key moves down 23 px until the second press |
| `test_slint_patch` line 815, centre box figure | more than 30 plain-ink pixels | more than 10 accent-ink pixels, and more than 30 accent-ink plus plain-ink | the box still holds the focus from the Delete check above, and lane 2 selects all on focus: the figure is drawn selected |
| `tests/thread_inventory_rows.inc` | no row for `test_slint_window_guard.cpp` | one row (finite helper thread, joined) | lane 2's new test starts one `std::thread` |
| layout drive pins | | | unchanged, section 2 |

## 5. Red tests the merge produced (each fixed at its cause, commit `7251ce7f`)
`test_slint_clip_probe`, `test_slint_tools`, `test_slint_patch`, `test_thread_inventory`: all REAL, each fails alone, causes in section 4. Nothing weakened: each expectation follows a changed sentence, line or layout and the old behaviour test now asserts the new one (the patch test would fail again without the select-all).

## 6. Gates (before the fixes, at build `bb094ce9` plus the drive and notes commit; full `cmake --build` exit 0, 29 min)

| Gate | Slots | Reds | Class |
|---|---|---|---|
| headless, `ctest -L "headless\|slint" -j3`, slice 1 (1 to 622) | 627 run, 1112 s | `test_api_server` | LOAD: Passed alone (13.09 s) |
| | | `test_slint_clip_probe`, `test_slint_patch`, `test_slint_tools` | REAL, section 5 |
| headless slice 2 (623 to 1244) | 622 run, 934 s | `test_thread_inventory` | REAL, section 5 |
| | | `test_diagnostics_free` | STAMP (binary older than HEAD); rerun at the final build |
| | | `test_plugin_abi3_compat`, `test_rtlsdr_live` skipped by their own rule | |
| GUI, `ctest -L gui -j3`, slice 1 (1 to 220) | 221 run, 1720 s | none | 100 % passed |
| GUI slice 2 (221 to 439) | 219 run, 1730 s | `test_diagnostics` | STAMP |
| | | `test_clean_exit_marker` | LOAD: Passed alone (2.61 s) |
The `-j3` runs stayed at `-j3`: the orchestrator did not report the real-mouse drive resumed during this run.
Own tests and drives of both lanes (45 tests, `-j3`): all passed except my new drive's first version (it quoted the key names; fixed, then Passed alone).

## 7. Worker validator
`FOXSDR_DUMP_APP_MESSAGES` over `test_radio_events`, `test_radio_reporter`, `test_radio_reports_engine` (all Passed): `node tests/radio_reports_validate.mjs` gave 146 messages, 146 OK, 0 refused (146 at merge 10). `telemetry_worker` Passed in the headless gate.

## 8. Open rows
`open-rows-2026-11-17.md` refreshed in place: 361 open, 70 closed, 431 in all (screen 70 / 27, engine 134 / 36, plugins 16 / 0, Worker/site 20 / 6, owner decision 60 / 0, mouse drive 61 / 1). The seven the brief names are in it (panel past Output, "Stop plugin" key on the mute banner, the palette's first click, Radio setup closing after CHECK AGAIN, the one to three second lag, the Source section's other per-radio facts, the sound card refusal for radios 2 to 8), plus the confirming key's jump found here.

## 9. UNVERIFIED
UHD with `type=b200` on a real B200 and the firewall prompt (never scanned here); the real-pointer behaviour of everything the lanes fixed by scripted pointers (the theme, select-all on a real click, the right-click submenu); a break-it of the combined typed-text and select-all drive; the four new machine-made translations by a speaker; the stamp tests and the repeat runs at the final build (in the hand-back message).
