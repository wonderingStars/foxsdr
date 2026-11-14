# Engine fixes 6 (`engine-fixes-6`): the airband file writer never waits on the disk, two controls drawn over a section header, the Legacy rename

Date 2026-11-13/14. Worktree `C:\Users\steve\foxsdr-wt\mr-dsp`, branch `engine/multi-radio`, build `D:/foxsdr-build/mr-dsp`, scratch `D:/foxsdr-build/ef6-scratch/`. Start tip `b07cd141` (clean, verified). Commits: 07e717e6 (item 1), 70885413 (item 2), 2bcc05ce (item 3, rename only), then the export-wf script fix; this record is the last commit. Test stamps: the build was made at 2bcc05ce; the later commits change only a drive script and this record. Nothing pushed, nothing downloaded, no radio opened, `FOXSDR_ALLOW_VISIBLE_GUI_TESTS` and `FOXSDR_ALLOW_REAL_HARDWARE_TESTS` never set; every window, drive script, screenshot and ctest ran through the hidden-desktop launcher (`FoxSDRTests`).

## Item 1. The airband per-channel WAV writer (commit 07e717e6)

* Cause in one sentence: `AsyncWavOutput` (`src/core/airband_gate.hpp`) joined its worker and closed its file in its destructor, which runs on the control thread when a patch plan is retired, so a stalled disk froze the thread that applies the person's commands.
* RED (stand-in writer whose `write` / `close` block; the class was first made a template on its writer with no change of behaviour): destroying the output with `write()` blocked returned after **1516 ms**; with `close()` blocked after **1511 ms** (the stand-in disk was stuck 1500 ms; with no release the call never returned and the test hit its timeout).
* Fix, with the mechanism the tree already has (the patch speaker's MP3 destination in `core/patch_audio.cpp` and the recorders' finish accounting in `core/record_finish.hpp`): the worker is detached and owns everything it touches (ring, writer, flags) through a `shared_ptr`; the destructor only sets `stop`, counts the retirement with `RecordFinisher::externalBegin()` and returns. The worker writes every block still queued, closes the file and ends, calling `externalEnd()`; quit therefore drains it against a deadline through the existing `drainRecordFinishes`. No queued data is thrown away on a healthy disk.
* Per-block allocation removed: `push` now copies into a ring of 2^18 samples with 256 block slots allocated when the output is built (control thread). Drop-and-count unchanged (busy lock, queue full, block larger than the ring all drop and count).
* GREEN (`tests/test_airband_gate_blocking.cpp`): `A: ... write() blocked returned after 0 ms`, `B: ... close() blocked returned after 0 ms` (bound 250 ms); `C: quit drain against a stuck disk: returned false after 156 ms` (bounded, counted in `RecordFinisher::inFlight`, collected when the disk answers); `D: take of 91680 accepted samples -> ch04_121500kHz_0001.wav, 183404 bytes` (valid 16-bit mono PCM header, data size == file size - 44, every accepted sample present and equal to its quantised value, in order, output destroyed straight after the last push); `E: 1000 pushes on a healthy disk: 0 allocations on the audio thread` and `E: 1000 pushes with the writer stuck (the drop path): 0 allocations` (counted with `dspmodels_alloc.hpp`, the tree's existing pattern; the old code allocates once per push); `F: 400 pushes into a stuck writer: 9216 samples dropped and counted, 0 ms`, and written + dropped == pushed exactly. `test_airband_gate_blocking: 41 checks, 0 failed`.
* Audio continuity across the change: not measured through a live engine (UNVERIFIED); the unit test proves in-order, complete delivery of every accepted block and exact drop accounting.
* Break-it: the destructor made to wait for the worker again: RED, `1514 ms` and `1513 ms` (`FAIL ... ms < kBoundMs`), restored, 41 checks 0 failed.
* Inventory row (`tests/thread_inventory_rows.inc`) rewritten: detached, never joined by the owner, counted for quit; the worker itself can wait on a stalled disk and registers no heartbeat, so a stuck disk is not reported by name: still "NOT COVERED by this batch (a known gap)", like the MP3 writer's row. `test_thread_inventory` passes (5.6 s).
* Related tests run: all 35 airband / wire-a / patch / thread-inventory / record-finish tests passed except the gui-labelled ones (refused by the guard, "Not Run") and the known `test_airband_data`.

## Item 2. Two controls drawn over the next section's header (commit 70885413)

* Reproduced on the hidden desktop, 1600x2400, sections 0,2,3,6 open: Patch note at y=828 height 40 (ends 868) but the Source header at y=854; CW surround's "Read the pitch now" key at y=1982 (ends 2002; the dump showed the section ending at 1976) while Sinks header at 1976 (before-fix screenshot `D:/foxsdr-build/ef6-scratch/shots/ov-before.png`; `m5` records the same).
* Cause: a `Section` takes its height from `body.preferred-height`; the `Note` of a `KPanel` row (`src/slint/ui/system_bank.slint`, a wrapped `Text` in a repeated nested `VerticalLayout`) was counted as ONE line in that measurement while the layout then gave it its real wrapped height, so each section was short by the lines its notes wrapped. Fix: `height: self.preferred-height` on that Note (one line).
* After: Source header y=880 (note ends 868 + 12 padding), Sinks header y=2042 (key ends 2028), identical at 1600 and 624 wide (624 is the inventory minimum; the side panel is 296 px at both). Screenshot `shots/ov-after.png`.
* Test: `src/slint/tests/drive/layout-section-overlap-1600.txt` and `-624.txt` assert `expect "Source"#1 top >= "868"` and `expect "Sinks"#1 top >= "2028"` (plus the controls above). Against the unfixed layout (rebuilt): `Source#1 top >= 868 -> FAILED: it reads "854.00"`, `Sinks#1 top >= 2028 -> FAILED: it reads "1976.00"`, both widths, `4 checks, 3 failed`; with the fix both `4 checks, 0 failed`.
* Not asserted generically (every control against every header): the drive verbs read one control at a time; the two measured places are the ones pinned.

## Item 3. The classic window is named Legacy (commit 2bcc05ce) - rename only

* Changed: `installer/cascade.iss` (Start-menu entry "FoxSDR (Legacy)", its tooltip, header comments), `packaging/scoop/foxsdr.json.template`, `tools/release.ps1` (checks and plan lines that name the shortcut file), `tests/test_release_install.ps1` (pins the name; updated, 55 cases 0 failed), `docs/MANUAL.md` (shortcut name, "the Legacy window", three "Legacy only" cells), `README.md`, `docs/silent-install-switches.md`.
* Upgrade: Inno only adds on an upgrade, so `[InstallDelete]` now removes `{group}\FoxSDR (classic window).lnk`; an upgrade from 0.99.104 leaves one Legacy entry. The installer was NOT compiled (ISCC not run): UNVERIFIED.
* No string-table or catalogue string said "classic window": no catalogue change, `test_i18n` green (356864+ checks, 0 failed).
* Deliberately left: `cascade.exe`, targets, settings and registry keys, test names, code identifiers, source comments, developer records and past release notes (history), and the "Classic" colour palette / meter face / "Bench Classic XL" skin (different things with the same word).
* NOT DONE, hand-off (owner's second message: Legacy as a choice inside the new window): (1) an entry "Open the Legacy window" in the Layouts menu (`MenuKind::Layouts`, `src/slint/main.cpp` ~line 1015, id 1992) with one plain line "the previous FoxSDR layout", new strings in the 33 catalogues; (2) its mechanics must follow the tree's rule for two processes (`src/main.cpp` ~150: the second process is started only after the first has released everything, the settings lock and the radios, as the software-GL relaunch does), i.e. the new window saves, closes its radios and the lock, exits, and a launcher starts `cascade.exe` after it is gone; the new window cannot start `cascade.exe` while it holds the lock; (3) the way back: a "Switch to the new window" item in the classic window's menu, same hand-over in the other direction; (4) a start-in-Legacy setting only with a loop guard (a flag the hand-over passes so the launched window does not hand back). Needs a drive-script seam that records the launch instead of starting `cascade.exe`, and a break-it. Not built: it is larger than a small addition and was not verifiable inside this lane's time.

## Gate (code tip 2bcc05ce, full rebuild exit 0)

Headless + slint, `ctest -C Release -L "headless|slint" -I a,b -j4 --timeout 300`, 1148 tests in four slices through the hidden-desktop launcher (`FOXSDR_FRAME_CAP_HZ=60`):

| Slice | Result |
|---|---|
| 1-290 | `99% tests passed, 4 tests failed out of 290` (593.07 s): test_airband_data, test_band_plan, test_converter_call_sites (known), test_engine_api_merge (load) |
| 291-580 | `99% tests passed, 3 tests failed out of 290` (552.97 s): test_page_resize_id (known), test_engine_main_clean_exit_marker, test_failure_files (load) |
| 581-870 | `99% tests passed, 1 tests failed out of 290` (408.03 s): test_png_decode (known); skipped on purpose: test_plugin_abi3_compat, test_rtlsdr_live |
| 871-1148 | `99% tests passed, 1 tests failed out of 278` (439.96 s): test_health_app_stepped (load) |

Re-run alone, the four non-known reds: test_engine_api_merge `Passed 20.44 sec`, test_engine_main_clean_exit_marker `Passed 14.49 sec`, test_failure_files `Passed 71.52 sec`, test_health_app_stepped `Passed 34.97 sec` (`100% tests passed out of 4`). GUI gate: see the line below.

GUI gate, `ctest -L gui -j4 --timeout 300` through the hidden-desktop launcher (`gateg.bat`): `99% tests passed, 4 tests failed out of 346`, `Total Test time (real) = 2152.95 sec`. Reds: `slint_drive_deck-skins` (load flake, `Passed 42.27 sec` alone), `test_health_app` (load flake, `Passed 36.94 sec` alone), `slint_drive_system-look` (load flake, `Passed 18.82 sec` alone), `slint_drive_export-wf` (REAL: it failed alone too, `no control labelled "Save to"` / `"Data length 15 minutes"`). Cause: the script scrolls the Tools panel by fixed amounts and the Export section is now as tall as its wrapped notes (item 2), so the box had scrolled out of view above; fixed in the SCRIPT only by scrolling back (`scroll "Save waterfall picture" -300`, a 12-tick settle before the type, `scroll "Save waterfall data" -300` before the length key), every assertion unchanged; run alone with the fixed script: `test_slint_drive_run: 4 checks, 0 failed`, exit 0 (that fixed script was run directly through the drive runner on the hidden desktop, not re-run in a full ctest gate). The two new `layout-section-overlap` scripts passed in the gate.

## Config hash

`%APPDATA%/foxsdr/config.json` sha256 `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` at the end (no test opened it).

## UNVERIFIED

* Audio continuity through a live engine across a channel-settings change.
* The installer script change (ISCC not run) and the upgrade removal of the old shortcut on a real upgrade.
* The Legacy entry inside the new window, the way back and the start-in-Legacy setting: not built (hand-off above).
* The writer's worker has no heartbeat: a stuck disk is not reported by name.
* Nothing was built or run for Linux.
