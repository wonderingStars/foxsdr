# Look fixes: the faults the second look review left in the window (2026-11-17)

Branch `feat/look-fixes`, cut from `cdc1d783` (engine/multi-radio, engine merge 11 first phase), worktree `D:/foxsdr-wt/en-screen-j`, build `D:/foxsdr-build/en-screen-j`, hidden desktop `FoxSDRLookFix`, scratch `D:/foxsdr-build/lf-scratch/` (scripts in `scripts/`, pictures `before/`, `after/`, column crops `pics/`). Nothing pushed. Source of the work list: `D:/foxsdr-build/look-review/LOOK-REVIEW-2.md`.
Real `%APPDATA%/foxsdr/config.json` sha256 `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` (checked at the end: unchanged).

## The ranked ten

| # | Fault | Verdict | Before (picture of this tree) | After (picture) | Code |
|---|---|---|---|---|---|
| 1 | Replay buffer "Held now seconds=5;bytes=48..." | FIXED | `pics/before-CLIPS-2.png`: the row reads `seconds=5;bytes=480` / `000;max=11520000` | `pics/after-CLIPS-2.png`: "5 s of audio held (0.5 MB)" | `replayHeldText` (`lanes3_logic.cpp`), `lanes3_publish.cpp` |
| 2 | Needed libraries English in all 33 languages | FIXED | `pics/before-B4S22DE-1.png` (German window): steps, reason, name in English between German labels | `pics/after-B4S22DE-1.png`: name, reason, "Sucht nach", the four numbered steps and the licence line are German | table literals marked `FOX_TR_NOOP` (`source/needed_library.cpp`, `sdriq_d2xx.cpp`), `trs()` in `parseNeeded` (`lanel_logic.cpp`); 81 strings x 33 catalogues |
| 3 | Airband "squelch -" and "hold 2." with a stray "s" | FIXED | `pics/before-B2S4-1.png`: readout `squelch …`, Hold box `hold 2.` then `s` | `pics/after-B2S4-1.png`: `-55 dB`, Hold box `2.0` then `s` | `tools_logic.cpp` (figure and unit only; the box is 62 px) |
| 4 | Strip hints bypass `tr()` | FIXED (strings only) | `pics/before/STRIPDE.png` (German window): "no gain stage" English under GAIN | `after/STRIPDE.png`: "keine Verstärkungsstufe". "Manual threshold" sits under the squelch tooltip in the picture: its German text is not readable there (UNVERIFIED by eye); it is in all 33 catalogues | `main.cpp` (three hints through `tr()`; no layout, no `host_logic.hpp`, no strip section of `main.slint`) |
| 5 | "Actual actual 0.2..." | FIXED | `pics/before-s5b-1.png`: `Actual   actual 0.2…` | `pics/after-s5b-1.png`: `Actual   0.24 MS/s` | `signal_logic.cpp` |
| 6 | "Job idle" | FIXED | `pics/before-B4S17-1.png`: `Job   idle` | `pics/after-B4S17-1.png`: `Job   nothing running` | `jobStateWords` (`lanel_logic.cpp`) |
| 7 | DEVICE STOPPED card and chip "127.0.0.1:" | FIXED | `pics/before-s5b-1.png`: "Device stopped: source thread: the I/Q stream stopped: no samples arrived for 3.00 s" (the "s" alone on a line), chip `127.0.0.1:` | `pics/after-s5b-1.png`: "Device stopped: No samples arrived for 3 seconds: the sender may have stopped, or its address or port may be wrong"; the chip now reads OPEN (the address does not fit, so it is not shown) | `raw_iq_net_source.cpp` (the one source-file sentence, through `tr()`), prefix drop in `signal_logic.cpp`, chip rule in `sourceChipText` |
| 8 | Radar position: two unlabelled boxes | FIXED (labelled; the empty page below is left, nothing invented) | `pics/before-B2S1-1.png`: two empty boxes side by side | `pics/after-B2S1-1.png`: rows "Latitude" and "Longitude" | `ui/tools_bank.slint` |
| 9 | Accessibility sliders cut and alike | FIXED | review picture `w1024-b4s12` (not retaken on the old binary: the old build was replaced before this section was photographed) | `pics/after-B4S12-2.png`: "Lowest pitch", "Highest pitch", "Level that sounds lowest", "Level that sounds highest", each whole above its slider | `ui/system_bank.slint` (kind 7 rows of every lane panel: name above the slider) |
| 10 | Field error at the bottom of a long section | REAL, FIXED | `pics/before-s5err-1.png`: "Open the stream: Port: type a whole number" is under the rail of notes at the foot of the Source section, about 1000 px below the "Open the stream" key | `pics/after-s5err-1.png`: the line is directly under that key | `placeNote` (`lanek_logic.cpp`) used by every lane panel's section note (`lanel`, `lanem`, `lanes2`, `lanes3`, `s1`, `s4`, `s4b`, `s5_rows`); a note whose text starts "<row name>: " goes under that row, anything else at the end as before |

## Expectations changed (both values)
| Test | Before | After |
|---|---|---|
| `test_slint_host_tools` airband model | `squelch_text == "squelch -55 dB"`, `hold_text == "hold 2.0 s"` | `"-55 dB"`, `"2.0"` |
| `test_slint_host_tools` (second case) | `"squelch -80 dB"`, `"hold 7.5 s"` | `"-80 dB"`, `"7.5"` |
| `test_slint_host_tools` Legacy-key list | contains `"squelch %.0f dB"` and `"hold %.1f s"` (keys that Legacy shares) | those two entries removed: the new window no longer says them |
| `test_slint_host_signal` | `actual_rate == "actual 2.4 MS/s"` | `"2.4 MS/s"` |

New tests: `replayHeldText` (3 cases, `test_slint_lanes3_host`), `jobStateWords` (`test_slint_lanel_host`), `placeNote` (4 cases, `test_slint_lanek_host`), source chip and the thread-name prefix (`test_slint_host_signal`).

## Catalogues
81 new English strings in all 33 catalogues, machine-made (`"machine": true`), unchecked by a speaker. `python tools/i18n_keys.py`: 4887 keys, 0 empty, 0 missing, 0 stale in every catalogue; `lang_assets.hpp` regenerated; `test_i18n`, `test_i18n_glyphs` (CJK and Korean spacing) pass. A first run of `test_i18n` was red: it refuses a translated format printed into a fixed buffer (`snprintf`) in two of my sites; both now use `formatText`.

## Tests run (Release, `ctest -j2`)
`test_slint_host_tools`, `test_slint_host_signal`, `test_slint_lanek_host`, `test_slint_lanel_host`, `test_slint_lanes3_host`, `test_slint_host_mustfixa`, `test_slint_host_extend`, `test_slint_host_system`, `test_slint_lanem_host`, `test_i18n`, `test_i18n_glyphs`, `test_error_paths` (PENDING pin 410 before and after, no allow-list line changed): all green after the two fixes above (see the commit). Drives (`ctest -R "slint_drive_(lanel|lanek|s3|s5|scope|tools|airband|layout-airband|lanex-panes|mapj|extend|s4|system|radar|signal|setup|window-min|layout-section)"`, hidden desktop): `100% tests passed out of 85`.

## Not fixed / open
- Needed-libraries check sentences that are built at run time and name a folder ("Not found: X is not in Y.") stay English: they are in no catalogue.
- Radar scope page is still mostly empty below the boxes (only the boxes are labelled).
- The wrapped slider names change the height of every lane-panel slider row (one extra line); a window at 1024 wide was not re-photographed.
- Item 9 before-picture is the review's, not mine.
- Other STILL THERE rows of the review's BROKEN and MISLEADING tables (scanner status wording, "Daily folders" duplicate heading, Plugin store GET EVERYTHING sentence, "Radio RADIO" on the Patch node, Web "listeners are signed in by the owner's say-so" wording): NOT STARTED, no time.

## NOT RUN
The full headless and GUI gates, the clip sweep, a real pointer pass, Legacy by eye.
