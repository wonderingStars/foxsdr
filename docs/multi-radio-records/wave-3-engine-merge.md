# Wave 3: mr/engine-stage5-6 (61f6c9d1) merged into engine/multi-radio (fe2e5917)

Date 2026-11-11. Worktree foxsdr-wt/mr-dsp, build D:/foxsdr-build/mr-dsp. Nothing pushed. Merge commit 25b0ebe7 (`git merge --no-ff mr/engine-stage5-6`).

## What merged

Engine stages 5 and 6: the saved settings of radios 2 to 8 (`radios`, `pluginRadio`), a PluginRunner per radio and `PLUGIN_RADIO` (0x1206), the per-radio lists (`read_radio_list`, `read_list` of GAINS / ANTENNAS / SAMPLE_RATES / DECODERS following the session focus),
`FOXAPI_CAP_RADIOS` (capability bit 30), `fact.radio.<n>.plugins` per radio, `fact.radio.solo`, `fact.scanner.radio`, `fact.transmit.radio`, the file / Pluto / rtl_tcp ids accepted by `RADIO_ADD`, the focus move after `RADIO_REMOVE`, `tools/old_reader_probe`,
`tools/multi_radio_load` with decoders, API.md 6.20, additions 9.22 (after the screen lane's 9.21). 54 files changed, 4415 insertions. No textual conflict. `resources/lang` and `src/core/lang_assets.hpp` were touched by neither side (the diff against the screen tip is empty), so `tools/embed-lang.py` was not needed. `src/slint` is untouched by the merge.

## The joins, read

The screen side (written against the stage 4 engine) was checked by reading `src/slint/main.cpp` and its logic files against what the engine now does. Nothing needed a change.

* **Focus after `RADIO_REMOVE`.** The engine now moves a session that was focused on the removed radio to radio 1 itself (`ApiHost::radioClosed`). The host's workaround (`onTick`: a `NO_DEVICE` read refreshes the radios from the facts, the selection goes to the lowest radio left, and `RADIO_FOCUS` is sent for it; `refreshRadios` sends `RADIO_FOCUS` whenever the selection changed) stays correct: radio 1 is always there, so the lowest radio left is radio 1 and the engine's move and the host's `RADIO_FOCUS 1` agree; a removal of a radio the window was NOT on leaves the engine's focus untouched and the host sends nothing. The workaround is now only a second path to the same state (it still matters for a session the engine does not move, and for the test seam). `live-two`, `live-four` and `live-eight` (which remove the selected radio) pass on the merged build.
* **`fact.radio.<n>.plugins` per radio.** The screen reads `fact.radio.<n>.state`, `.device`, `.label`, `.audio`, `.refusal` (`readRadioFacts`) and nothing from `.plugins`; no join.
* **The four lists follow the session's focus.** Every list the window reads (`refreshLists`: GAINS, ANTENNAS, SAMPLE_RATES and DECODERS among them; the SIGNAL bank's gain / rate / antenna rows in `signal_logic.cpp`; the gain stage in `scope_publish.cpp` and `main.cpp`) goes through the window session's `read_list`, and the window's session focus IS the selected radio (`RADIO_FOCUS` on every selection). Before this merge those four lists always described radio 1 whichever radio was selected; now they describe the selected radio, which is what the SIGNAL bank wants ("the controls follow the selected radio"). The lists re-read when `listSeq` moves; the engine moves a session's seq groups on `RADIO_FOCUS`, so a selection change re-reads them. `live-agc` (selects radio 2, then 1, then 2 and reads the bank) passes. `decodersFed` (from DECODERS `FED`) now means "fed on the selected radio", which is the right meaning for a per-radio screen. No screen code assumed radio 1's lists while another radio is focused except through that single path, so no change.
* **`FOXAPI_CAP_RADIOS` (bit 30).** The screen does not test the capability word for the multi-radio set (it reads `fact.radio.count`, which is absent on an engine without the set); nothing to change. The word still ends at bit 30 (`test_engine_api_05a`).
* **The mock engine** (`third_party/foxsdr_api/reference/mock_engine`) is not built in this tree (it includes a `common/` directory that is not here); stage 6 edited it by reading only, as its record says. The screen's own test seam (`workspace_testseam.hpp`) answers the facts and `RADIO_FOCUS` for made-up radios and is untouched by the merge.

## Fixes

None. No file other than the merge was changed.

## Gates

* Build: `cmake --build D:/foxsdr-build/mr-dsp --config Release --parallel`, exit 0 (the Slint-generated code compiled once, about 40 minutes with the machine shared).
* Headless gate, `ctest -L "headless|slint" -j4 --timeout 300`, run in two halves (`-I 1,380` and `-I 381,757`; 757 tests, the screen lane's 753 plus the four new stage 5 and 6 tests): 751 passed, 6 failed, 729 s and 568 s.
  * `test_airband_data`, `test_band_plan`, `test_converter_call_sites`, `test_page_resize_id`, `test_png_decode`: the known false failures of a D: build (they find the tree from the working directory). Run with the worktree root as the working directory: 115, 5174, 5, 22 and 49 checks, 0 failed.
  * `test_engine_api_transmit`: the known timing test (`issued.size() > 1000`, line 1500). It failed again alone, three more times in a row (105 s each, 662 checks passed, 1 failed), and the same test built from the unmerged stage 5 and 6 tree (`D:/foxsdr-build/mr-engine1`) fails identically (802 new tickets) under the same machine load, so the merge did not cause it.
  * `test_diagnostics` and `test_diagnostics_free` PASSED (the tree was built after the merge commit, so the commit stamp is current).
* Window gate on the hidden desktop (`FoxSDRTests`, `FOXSDR_FRAME_CAP_HZ=60`, `-L gui -j3`, a .bat in the build folder): `100% tests passed, 0 tests failed out of 244`, 1612 s. That includes all four `slint_drive_live-*` scripts (`live-two` 24 s, `live-four` 31 s, `live-eight` 38 s, `live-agc` 28 s) on the merged build, and `test_diagnostics`.
* Ledger: `ledger: 2167 rows, 542 asserted, 92 driven, 1533 not drivable, 0 unclassified` (the `slint_drive_ledger` test passed in the headless half; the command run by hand gives the same line).
* `config.json` (`%APPDATA%\foxsdr\config.json`) sha256 c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3 before and after; no gate touched it.

## Not verified here

No real radio, no listening, no new window code: the merge brought no screen change, so the screen's live drives are the evidence that nothing broke.
