# Engine merge 13 (`engine-merge-13`): drive fixes 4 and 5 onto `engine/multi-radio` (2026-11-18)

Worktree `C:/Users/steve/foxsdr-wt/mr-dsp`, branch `engine/multi-radio`, start tip `4205a401` (engine merge 12, clean), build `D:/foxsdr-build/mr-dsp` (Slint ON, clip probe OFF, Release), scratch `D:/foxsdr-build/m13-scratch/`, hidden desktop `FoxSDRMerge13`. Every test and drive ran through the hidden-desktop launcher. No radio opened, nothing pushed, downloaded or deployed, no outside request, no other worktree touched, nothing shown on the visible desktop, no `foxsdr-slint.exe` or `cascade.exe` of the drive touched. The mouse drive ended at 18:26; builds before that ran at `--parallel 4` and ctest at `-j1`/`-j3` as briefed, afterwards `-j3`.

Because the stamp tests compare the binary's commit with HEAD, THIS RECORD IS THE LAST COMMIT BEFORE THE FINAL REBUILD: the totals of the complete headless and GUI gates at the final commit are in the hand-back message and in `D:/foxsdr-build/m13-scratch/` (`f*.log`, `g*.log`).

## 1. The merge
`fix/drive-5` `20be194e` fast-forwarded onto `4205a401` (`git merge --ff-only`): six commits, `7e61b3a8`, `3276b1bb` (drive fixes 4), `4b149dcf`, `beca9eb8`, `c56c6639`, `20be194e` (drive fixes 5). No conflict is possible on one line of history. The two groups touched the same two files (`tools_bank.slint`, `test_slint_fields.cpp`): read, they compose (group 4's memory-block Note and the Remove key's `on:` state; group 5's release-time click layer in the same `LineInput`, whose tip timer now also listens to the layer's hover).

## 2. What relied on the old wake of every decoder
Searched: every drive script and test that mentions the Decoder output page, `decoderLines`, `decodersRunning` or a decoder's lines. Found: NONE depends on the page waking decoders. Every drive script that reads decoder lines or a decoder preset presses START (or ticks Keep running) first (79 scripts mention decoder lines or running decoders; none lacks START or a preset action). The Legacy-window tests (`test_plugin_inuse_app`, `test_plugin_run`, `test_decoder_window_app`) test the engine's bit 8, which group 5 did not change. Nothing was corrected by restoring the wake. The eight reds the merged tree DID show had other causes (section 4): the new scrolling box, the AM phase trim, a START that opens the instrument page, a line number.

## 3. The three text-field rules, together
Group 2's "typed text handed over before a key acts", group 3's select-all on focus (both on the base) and group 5's select-all on the RELEASE of a real click: drives `merge12-typed-select`, `drive1-clarifier`, `drive2-iq-typed` all Passed on the merged tree, together with `test_slint_fields` (the release-time variants, RED with the layer disabled in group 5's own break-it) and `keys-in-field`.

## 4. Reds on the merged tree, each explained from its output
| Test | Cause | Fix |
|---|---|---|
| `slint_drive_plugin-cw-decoder`, `-dmr-monitor`, `-eas-decoder`, `-ft8-decoder`, `-rtty-decoder`, `-selcal-decoder` | "no control labelled" for the plugin's preset keys: group 5's 132 px scrolling box on the Decoder output page hides every bar but the first three (15 bars here, in fitted-module order, outer heights 75, 45, 75, 45, 75, 75, 135, 135, 105, 45, 75, 75, 105, 45, 45); the drive finds only controls that are on screen | each script wheels the box to its own bar (`scroll "Presets"#1` by the bar's offset, 1 px per unit: CW 120, DMR 195, EAS 240, FT8 390, RTTY 885, SELCAL 960). Six Passed |
| `slint_drive_plugin-vor-decoder` | `click "VOR course indicator"#1` hits the rail row, which closes the instrument page START already opened; the tab is #2 | click #2. Passed |
| `slint_drive_fixture-vor-decoder` | the radial reads `VOR radial 090 ...`; the script expected `VOR radial 08` (086..090 before the AM phase trim, group 5 item 4) | expectation `VOR radial 090` (old `08`, new `090`). Passed |
| `test_slint_clip_probe` | `tools/slint-clip-explained.tsv` names `sliders.slint:220`; group 5 added 21 lines above it | line 241. Passed |
| `test_slint_globe_body` Not Run | the full build ran its CMake configure LAST (the glob saw the new files only then), so the target was never built | built by name; Passed in the gates after the rebuild |

Expectations changed by these fixes, both values: VOR radial `08` to `090`; `sliders.slint:220` to `:241`; the six scripts gain one scroll step each. Group 4 and 5's own changed expectations (see their records): `test_aor_source` firmware path "resources" to "firmware"; `test_slint_tools` Remove group second press y 273 (merge 12) to 250; `test_slint_host_kit` use table aprs "8" and pocsag "9" to pocsag "1"; `test_slint_forms` idle tooltip lost "or the Decoder output window"; drive `tools-bookmarks-rows` clicks "Remove this group" again.

Group 5's decision to show only the first three bars is a usability point for the owner (not changed here): a plugin the user has just started can sit below other decoders' bars and needs the wheel (row `DF5-bars-order` in the open rows is not added: the order is the fitted-module order, an owner call).

## 5. Slider wheel
The sixteen distinct labels the drive scripts use with the `scroll` verb ("1 MHz digit", the globe and orbit views, "Above the noise", "Airport code", "Audio filters", "Copy diagnostics", "Mute audio while running", "Picture", "Presets", "Radar tube", the diagnostics tick, "Save to", the two Save waterfall keys, "Signal strength", "Theme") were checked against the window's files: none is a `Slider` (the only slider accessible roles are `sliders.slint`, `instruments.slint`, `scope_modes.slint`, `tabs.slint`; "Signal strength" is a read-only `ReadBar`). So no drive needed to rest first, and none was changed for the slider rule. Drives that move a slider use the `slider` verb (a drag). The wheel rule's own checks are in `test_slint_sliders` (drives B and F hold the keyboard; specimen C reports); that test is in the gate totals in the hand-back message.

## 6. Layout pins, measured once on the final window (both values)
`layout-section-overlap-624`: "Stop bypassing the selection" bottom / "Source"#1 top / "Read the pitch now" bottom / "Sinks"#1 top: read by probe scripts with impossible pins: 902 / 949 / 2361 / 2549 (merge 12: 902 / 949 / 2361 / 2549); pins 902 / 937 / 2361 / 2361 unchanged. `layout-section-overlap-1600`: 820 / 867 / 2279 / 2467 (merge 12: the same). The 1440 sheet, `bar-min-624`, `window-strip-1024/1280/1366/1440/1480/1920`, `window-min-strip`: pass unchanged. `test_slint_tools` Remove group second press: y 250 (merge 12 had 273). `lanec-memory-reserve`: both readings 699 (reserved line; 679 without it). The Decoder output page's box takes no room outside its page. So no layout-section pin changes in this merge.

## 7. Catalogues, privacy wording, error-path pin, allocation bounds
- `python tools/i18n_keys.py`: 4904 keys in each of the 33 catalogues, 0 empty, 0 missing, 0 stale (merge 12: 4892; group 5 added 12). `python tools/embed-lang.py` gives a `lang_assets.hpp` that differs from the committed one in line endings only. Korean word-spacing, CJK wrap and the developer-words test (now also refusing "later" where it promises a feature, `test_slint_host_mustfixa`): Passed.
- Privacy wording: `git diff --word-diff 4205a401 HEAD -- PRIVACY.md docs/DIAGNOSTICS.md`: 0 bytes.
- Error-path pin: "3487 sites: 284 report, 2793 allowed, 410 PENDING". 410 before (merge 12), 410 after (groups 4 and 5 added no site). Does not exceed 410.
- `test_engine_publish_alloc` measured again: F3 rebuilds 2183 to 2546 (max 2546), F12 2174; bounds 2800 and 2400 stand; comments updated.

## 8. Release notes
`docs/release-notes-0.99.109.md`, four plain lines in sections that already held such things (Radios: AOR firmware folder; Plugins and companions: the Decoder output page no longer starts every decoder, VOR bearings corrected; Also: the slider wheel). Version stays 0.99.109.

## 9. Worker validator, open rows
Validator result in the hand-back message. `open-rows-2026-11-17.md` refreshed in place: 368 open, 72 closed, 440 in all (screen 69 / 28, engine 135 / 36, plugins 18 / 0, Worker/site 21 / 6, owner decision 59 / 1, mouse drive 66 / 1). Closed: `DF3-confirm-jump` (group 4), `DF3-wake-decoders` for the new window (group 5). Added: `DF5-legacy-wake`, `DF5-required-input`, `DF4-solar-sentence`, `DF4-open-phase`, `DF5-real-pointer`, `DF5-real-files`, `DF4-real-pointer`, `DF4-beat-https`, `DF4-demod-track-s29`.

## 10. UNVERIFIED
Everything real-pointer (the release-time select-all, the slider wheel's rest rule, the globe options under a held press, the wake fix, the Decoder output page's box), the odd-rate and preset-on-a-file fixes on real files, the https beat on a real Worker, the twelve machine-made translations by a speaker, the instrument page opened by START (read from the drive, the cause in `openWindowsFor` not changed), and the stamp and repeat results at the final build (hand-back message).

## 11. Later commits and the final merge (added before the final build)
Order of the tree after the first record: `92752495` fix/drive-6 merged; `89296aed` clip-explained lines, release notes and open rows brought up to drive fixes 6; `7c5726f7` `keys.slint:436` carries the not-translated mark on the StopKey probe word (`test_slint_house_rules` was red alone: "untranslated literal keys.slint:436: stop"; a comment, the only src/ edit of mine); `abce63d0` `test_window_full_session` B1 follows the Legacy Decoder output no longer being a plugin use (the test used bit 8 as its plugin-use table; it now uses the plugin's own window, "file TAB 1"; red alone at lines 335 and 337, both variants); `237d05d6` `test_plugin_inuse_app`'s "output" block now requires that the open Decoder output window wakes nothing (the same stale expectation, red in the GUI gate; turned round, five checks for four, nothing weakened); then the merge of `fix/drive-7` `d4fbfa23` as `0dfd2703` (the window edge follows the pointer: `drive-fixes-7.md`), and this record with the open-rows row `DF7-edge-linux` (372 open, 72 closed, 444 in all).
Also: `test_s4b_sec_peers` waited for the server to count a dropped client out (`99e2407d`: red alone in `--repeat until-fail:8`, 30 of 30 afterwards). The scrolling-box drive offsets are in section 4.

The gates were run at the commit after this one is made and BUILT; nothing is committed after them. Their totals, the validator, the config hash and the stamp result are in the hand-back message and in `D:/foxsdr-build/m13-scratch/` (`h*.log`, `g*.log`).
