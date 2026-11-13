# Engine fixes 5 (`engine-fixes-5`): a test that opened the owner's real radio, `test_i18n`, the intermittent wire-a display test, the silent ABI-3 test

Date 2026-11-13. Worktree `C:\Users\steve\foxsdr-wt\mr-dsp`, branch `engine/multi-radio`, build `D:/foxsdr-build/mr-dsp` (Slint ON, Release, `--parallel 3`), scratch `D:/foxsdr-build/ef5-scratch/`.
Start tip `2d8d4ee9` (clean, verified). Commits: bf01a3f0 (item 1), d0ce0fbf (item 2), dac03fb5 (item 3), edfa30e2 (item 4); this record is the last commit. Nothing pushed. No download, no outside request, no radio opened by me (the one test that would have, `test_rtlsdr_live`, was never run with the variable set). `src/core/plugin_abi.h` untouched. No gui-labelled test and no `.slint` file changed, so no GUI gate; every ctest and every single run went through the hidden desktop (`FoxSDRTests`) or ran headless with the real-hardware variable unset; `FOXSDR_ALLOW_VISIBLE_GUI_TESTS` was never set.

## Item 1. No test opens the person's own radio unless they ask (commit bf01a3f0)

* Red line (from engine-merge-4.md, section 3): `test_rtlsdr_live` opened the real RTL-SDR bound to WinUSB and failed at `tests/test_rtlsdr_live.cpp:206` (`period == 0`: "the first 32768 samples REPEAT EXACTLY every 1880 samples"). I did not re-run it (that is the thing being prevented).
* Cause in one sentence: the live test enumerated and opened whatever dongle was on the PC from the `headless` label, so every gate touched the owner's hardware.
* The variable: `FOXSDR_ALLOW_REAL_HARDWARE_TESTS`; exactly the value `1` allows it (unset, empty, `0`, `yes`, `true`, `11`, ` 1` do not). The guard is `tests/hardware_guard.hpp` (a pure decision plus the one skip line).
* Tests that could open or scan real hardware, and what now guards them:
  * `test_rtlsdr_live`: whole test (it enumerated the bus, then opened the dongle and streamed). Without the variable: `SKIP real hardware: test_rtlsdr_live opens the real RTL-SDR dongle - set FOXSDR_ALLOW_REAL_HARDWARE_TESTS=1 to run it on purpose (nothing was opened)`, exit 0, before any enumeration; ctest reports `***Skipped` (`SKIP_REGULAR_EXPRESSION "SKIP real hardware"`).
  * `test_usb_winusb`: section 4 (`openWinUsb` on a real RTL2832U, a second open, a vendor control transfer). The fake-transport and listing sections still run; the test passes and prints `hardware half SKIPPED (not allowed)`.
  * `test_soapy_source`: the two `SoapySource::enumerate()` scans (a child walks every vendor module's find function over the real bus) and the whole-bus `enumerateInProcess()` walk. The fake-driver blocks still run; the test passes and prints `real-bus scans SKIPPED (not allowed)`.
  * Looked at and NOT guarded, with the reason: `test_funcube` (`enumerateFcdHid` only lists HID interfaces with SetupAPI, opens nothing), `test_radio_setup` (`collectInventory`, a SetupAPI listing), `test_radio_setup_exit` (the probe with a collector that does not touch the bus), `test_usb_usbfs` (Linux sysfs listing, empty on this PC), `test_serial_port` (lists port names, opens `COM254`, a port that does not exist), `test_pluto_source` (the two addresses are offered, nothing contacted), `test_gpu_fft*` (GPU adapters, not a radio), `test_kiwisdr_live_probe` (a stand-in; the real connection needs `--live`).
* `tests/test_hardware_guard.cpp`: H1 the decision (both branches, nine refused values); H2 the environment read; H3 to H5 run the three guarded tests as children WITHOUT the variable and require the skip line, exit 0 and the absence of every line that follows a real enumeration or open (`live check:`, `opened `, `soapy devices:`); H6 a source scan of all 889 test files: a code line that calls a real-bus entry point (`enumerateRtlSdr()`, `openWinUsb(found`, `SoapySource::enumerate()`, `enumerateInProcess()`, `enumerateIsolated()`) in a file that does not include `hardware_guard.hpp` fails, so a new unguarded test cannot be added.
* Green: `test_hardware_guard: 52 checks, 0 failed`; `test_rtlsdr_live ***Skipped 0.01 sec`; `test_usb_winusb Passed 0.04 sec`; `test_soapy_source Passed 29.70 sec`.
* Both branches: the "not allowed" branch is proved on the real tests (children, above). The "allowed" branch (`=1`) is proved on the decision and on the environment read only: running a guarded test with it set would open the radio. That half is UNVERIFIED end to end by design.
* Break-it: an unguarded probe file (a string holding `SoapySource::enumerate()`, never built) made the scan fail (`UNGUARDED real-bus call ...`, `53 checks, 1 failed`), removed it again `52 checks, 0 failed`; the decision made to refuse `1` failed H1 and H2 (`52 checks, 2 failed`), restored. (The guarded tests themselves were never broken: a broken guard would have opened the dongle.)

## Item 2. `test_i18n` (commit d0ce0fbf)

* Red line: `test_i18n: 356666 checks, 33 failed` (`<lang>.json: 2 missing, 0 stale` in all 33 catalogues).
* Cause: two English sentences added by the second-pass lanes were in no catalogue.
* The two sentences: (wire-c, `SettingEditResult::NotAnOption`) `Not saved: that is not one of the choices.` and (wire-a, `Problem::NodeNotRun` in `src/core/patch_plan.hpp`) `this part is saved with the patch but this build does not run it yet`.
* Fix: both added to every `resources/lang/*.json` (66 added lines, none removed, the tool's own key order), `tools/embed-lang.py` re-run (`src/core/lang_assets.hpp`, 33 catalogues). **MACHINE TRANSLATIONS, UNCHECKED BY A SPEAKER, in all 33 languages** (bg ca cs da de el es et fi fr hr hu id it ja ko lt lv nb nl pl pt-BR pt-PT ro ru sk sl sv tr uk vi zh-CN zh-TW; the catalogues were already all `"machine": true`). The `Not saved:` prefix and the word for "patch" are the ones each catalogue already used (so Lithuanian says "komutacija", Latvian "shema", Vietnamese "bang noi", Chinese "peixian tu"/"jiexian tu" like their neighbours); a speaker should read them.
* Green: `test_i18n: 356864 checks, 0 failed`. Break-it: one new line removed from `de.json`: `de.json lacks: "this part is saved with the patch ..."`, `test_i18n: 356861 checks, 2 failed`; restored (the script is idempotent), 0 failed.

## Item 3. `test_engine_api_wire_a_display` (commit dac03fb5): found, fixed in the PRODUCT writer

* Measured under `ctest --repeat until-fail:N` alone: failed on run 1 of the first loop (`15.72 sec`, the 6 s wait timed out), run 5 of a second loop, run 28 of a third; the executable run directly: 8 of 8 passed. The mechanism is the same in both; the timing of two threads is what differs.
* Cause (proved, not assumed): a temporary print in `WireA::drainSaves` (removed again) showed the result that function discards: `DIAG save of display-average.json failed: markers: atomic replace of "...\user1\display-average.json" failed: Access is denied.` The test polls the target with `std::ifstream` every 5 ms while it waits for the save; on Windows the temp-file-then-rename in `FreqMarkers::writeFile` is refused with "Access is denied" while any other handle on the target is open (an ifstream, and also a handle opened WITH `FILE_SHARE_DELETE`: measured). A refused replace drops the save and nothing retries it, so the file keeps the older content (the failing D3 reads `persistence_medium` where `persistence_slow` was set).
* It is a product fault too, not only the test's: a virus scanner, the search indexer or a backup holds a fresh settings file open for a moment in the same way, and the person's bookmark, marker, trace mode, band memory or scan plan was lost silently.
* Fix: `FreqMarkers::writeFile` (`src/core/freq_markers.cpp`; the blocking half for every wire-a file and the bookmark and marker files, on the saver's worker thread) tries a refused replace again up to five times (10, 20, 40, 80, 160 ms; 310 ms at most), only for access denied / sharing violation / lock violation, Windows only. A handle held longer is still reported with the system's words, the old file intact, no temp file left. The wire-a test is unchanged (no weakening).
* Tests: `tests/test_freq_markers.cpp` `testTargetBusyForAMoment` (reader closes after 120 ms: the write lands; reader held beyond the window: refused with `atomic replace ... Access is denied` after 250 ms to under 4 s, old content intact, no `.tmp`; closed again: straight through; a directory in the target's place: refused, nothing hangs).
* RED (retries set to 0): `test_freq_markers: 313 checks, 5 failed`. GREEN: `test_freq_markers: 313 checks, 0 failed`; `test_engine_api_wire_a_display`: `ctest --repeat until-fail:30` = 30 of 30 Passed (`100% tests passed out of 1`); `test_failure_files`, `test_gui_disk_audit`, `test_bookmark_save_async` Passed. Break-it: retries 0 is the RED above, restored to 5.
* Not changed, noted: `WireA::drainSaves` still discards a finished write's result (a refused save that outlasts the retries is shown to nobody).

## Item 4. `test_plugin_abi3_compat` (commit edfa30e2)

* Before: no variable gave `SKIP: ... NOTHING WAS PROVED.` then `test_plugin_abi3_compat: 0 checks, 0 failed`, so ctest said `Test #885: test_plugin_abi3_compat ... Passed 0.02 sec`.
* Now: the line is `SKIPPED (nothing was proved): test_plugin_abi3_compat loads the published plugin binaries and none was given. Set FOXSDR_PUBLISHED_PLUGINS_DIR ... and run it again.` (no "0 failed" summary) and `SKIP_REGULAR_EXPRESSION "SKIPPED .nothing was proved"` makes ctest report `***Skipped` and list it under "did not run".
* Run the proper way on this tip (hidden desktop, `abi3.bat`, the 43 published Windows binaries of the plugin repository's master copied for engine-fixes-4 into `D:/foxsdr-build/ef4-scratch/published`, not refreshed): `43 of 43 published binaries loaded; 16 take the host table` and `test_plugin_abi3_compat: 147 checks, 0 failed`.

## Gate (code tip edfa30e2, full rebuild exit 0)

Headless + slint, `ctest -C Release -L "headless|slint" -I a,b -j4 --timeout 300`, 1146 tests in four slices through `ef5-gate.bat` started by the hidden-desktop launcher (`FoxSDRTests`, `FOXSDR_FRAME_CAP_HZ=60`, real-hardware and published-plugin variables unset):

| Slice | Result |
|---|---|
| 1-280 | `99% tests passed, 3 tests failed out of 280` (301.48 s): test_airband_data, test_band_plan, test_converter_call_sites |
| 281-560 | `99% tests passed, 1 tests failed out of 280` (520.04 s): test_page_resize_id |
| 561-840 | `99% tests passed, 1 tests failed out of 280` (410.96 s): test_png_decode; Skipped: test_plugin_abi3_compat, test_rtlsdr_live |
| 841-1250 | `100% tests passed out of 306` (351.06 s) |

1146 run, 5 red, every one of the five known false failures (red as in every earlier record; not investigated), 2 skipped on purpose (items 1 and 4), the rest passed, including `test_i18n` (3.80 s), `test_engine_api_wire_a_display` (8.35 s), `test_hardware_guard`, `test_freq_markers`, `test_engine_api_kiwisdr` and `test_pump_live_traffic` (merge 4's load flakes). No red needed a re-run alone: none was outside the known five. No GUI gate (no gui-labelled test changed).

## Config hash

`%APPDATA%/foxsdr/config.json` sha256 `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` at the start and again after the gate (`sha256sum` of the real file; no test opened it).

## UNVERIFIED

* The guarded tests with `FOXSDR_ALLOW_REAL_HARDWARE_TESTS=1` were not run (that is the hardware); the allowed branch is proved on the decision and the environment read only.
* The 33 new translations are machine output, unchecked by a speaker.
* Item 3's retry fixes the Windows "target open for a moment" case; a handle held longer than 310 ms still loses the save silently (see "Not changed").
* The 43 published plugin binaries used for item 4 are the copy made for engine-fixes-4, not a fresh copy of the plugin repository.
* Nothing was built or run for Linux.
