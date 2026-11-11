# Screen lane B: the Slint window's spectrum, waterfall and scope rows (2026-11-11)

Branch `feat/screen-b`, cut from `feat/screen-s1` at b4bdcd08. Worktree `C:\Users\steve\foxsdr-wt\mr-screen1`, build `D:/foxsdr-build/mr-screen1`. Nothing is pushed or merged.
Read first: `screen-s1.md` (waterfall export, persistence trace, layouts, stacked mark names), the lane briefs, `feature-gaps-survey.md` section 1.

## Step 0: the table of rows (reading only; before any code)

Checked: `git log` of `feat/screen-s1`, `feat/slint-gaps`, `feat/engine-wire`, `engine/multi-radio` and a grep of the base for each feature.

| Row | What | Class | Notes |
|---|---|---|---|
| S7 | Waterfall colour maps | screen only | The five maps exist as `core/colour_maps.hpp` (OKLab, "Deep sea", "Ember", "Slate", "Spectrum", "Blue-yellow safe") on `feat/engine-wire` (d0a06a41, two NEW files, nothing wired); cherry-picked here unchanged so the merge is mechanical. |
| S8 | Waterfall time stamps | screen only | Age ticks exist ("now", "5 s"); adds clock stamps (UTC / local) that scroll with the picture. |
| S10 | Waterfall speed, very slow | screen only | The waterfall scrolls one row per frame (`scrollWaterfallRow`); the screen can decimate (a row per interval with the strongest of the frames between). No engine change needed. |
| S5 | Min-hold | screen only | A trace mode beside peak hold and average. |
| S6 | Fill / histogram spectrum modes | screen only | Painted into the trace picture. |
| S11 | Audio waterfall | screen only | The scope's audio tap exists (`read_scope`, `scope_logic`); a waterfall of its spectrum is a new scope display. |
| S14 | SNR readout and history | screen only | From the spectrum frame (signal in the passband against the median floor); history shares S15. `core/noise_floor_tracker.hpp` is the S-meter's, not used. |
| S15 | Signal-history graph with CSV | screen only | The CSV writer of the power-log block (`b103a510` on `feat/engine-wire`) is not in this base; this lane writes its own small CSV (a DiskJob). |
| S17 | Autocorrelation / cepstrum | screen only | From the scope's audio tap; a new scope signal. |
| S18 | Constellation and eye diagram | screen only | The constellation is the scope's existing I/Q vector figure; the eye is new, from the I/Q tap, at a symbol rate the user sets. |
| S20 | Automatic peak labels | screen only | |
| S21 | A/B measurement markers with delta | screen only | |
| S22 | Reference / saved trace | screen only | |
| S23 | Click-to-tune on bookmark labels | screen only | spectrum.slint's bookmark names; lane C owns the bookmark PAGES, not these labels. |
| S25 | Band zoom buttons | screen only | |
| S26 | Compact mode | screen only | |
| S27 | Touch pan / pinch | screen only | No touch screen on the desk: scripted events only, real touch UNVERIFIED. |
| Q2 | Channel-power tool | screen only | Integrated from the spectrum frame; the accuracy is the frame's (stated). |
| Q3 | Frequency tracker / counter | screen only (limited) | A peak tracker with parabolic interpolation on the spectrum frame; the true counter accuracy needs the engine (hand-off written). |

Already built before this lane: none of the 19 (persistence, export and layouts are S4/S9/U7 of lane S1).
No row needs plugin_abi.h, engine.cpp, pipeline.cpp, receiver.hpp, demod.cpp, fox_engine_api.cpp or the core recorder to be edited.

