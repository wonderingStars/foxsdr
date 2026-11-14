# Patch faces: the Spectrum part and the Map part on the PATCH page of the new window (2026-11-14)

Branch `feat/patch-faces`, cut from `07e717e6` in worktree `D:/foxsdr-wt/en-screen-k`, build `D:/foxsdr-build/en-screen-k` (Slint ON, Release), scratch `D:/foxsdr-build/pf-scratch/`. Nothing is pushed or merged. No radio was opened: every run used the signal generator, or the ADS-B decoder's fixture recording, on the hidden desktop `FoxSDRTests`.
Real `%APPDATA%/foxsdr/config.json` sha256 at the start: `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3`; at the end: see the last section.
Read first: `STANDARD-RULES.md`, `screen-lanes-common.md`, `screen-m.md` (the build folder, the gate commands, the hidden-desktop launcher, how a screen lane proves a control).

## Why

The owner placed the Blueprint "Listen to the tower" on the PATCH page of the new window and found its Spectrum part (a DISPLAY node wired from the Radio) an empty black box, the inspector saying "This node has nothing to set yet.", while the old window draws a live spectrum and waterfall there. `docs/slint-port-tick.md` rows 1924 (Spectrum face), 1925 (Map face) and 1940 (Map: Fit to targets / Whole world) were DEFERRED. The owner's decision: build both before the release.

## What the old window does (from the code)

`src/gui/app_window.cpp`, `drawPatchFaces`:

* **Display** (`case pc::NodeKind::Display`, line 11883): a live trace and a scrolling waterfall of whatever feeds the part, cut out of the same wideband frame the main waterfall draws. Fed from a RADIO: the whole capture (-rate/2 .. +rate/2), every planned channel of THAT radio marked with a cream line and named. Fed from a CHANNEL: that channel's slice (offset +- half its output rate), its centre marked. It is the spectrum of the radio the part hangs off (`pc::radioOf`; a patch can hold several), read by `patchSpectrum(radio)` = `read_node_spectrum` with the held sequence as the cursor, once a frame per radio. The columns are `pg::binsToColumns` (the loudest bin of each column, so a narrow carrier survives a small part), scaled by `pg::autoRange`; two pixels a column and a row, capped at 200 columns and 100 rows; the trace takes the upper 38 % of the face. Two sentences: "Wire the radio or a channel here to see its spectrum." (not wired: a Radio feeder with no running rate, a Channel feeder the plan lacks, nothing) and "No spectrum yet - its radio is not running." (wired, no frame). Also, in the old part and not here: the trace-mode hold and average overlay, the user's frequency markers on the picture, a tip per region, and two right-click menus (trace mode, marker).
* **Map** (`case pc::NodeKind::Map`, line 12074): the targets of every decoder wired in (up to `kMapInputs`, from any radio) on one chart drawn by the same `MapView` the map pages use; the sentence "Wire a decoder's map output here - ADS-B, AIS, APRS - up to %zu." above the chart until a decoder is wired, then the counts; a fresh part is fitted to the targets (`requestFitToTracks`) and centred on the receiver when its position is known; the basemap's attribution under the chart.
* **Inspector** (`app_window.cpp` 13190-13361, `app_window_patch_radios.cpp` `drawPatchMapInspector`): a Display node has NO setting - "This node has nothing to set yet." - so the new window keeps that sentence and **row 1945 is left alone**. A Map node shows "%zu of %zu inputs wired", the count per source, two full-width buttons `Fit to targets` (`requestFitToTracks`) and `Whole world` (`requestWholeWorld`: centre 0, 0 and the span the picture's shape needs, `wholeWorldSpanDeg`), then the help.

## What was built, and what it reuses

Nothing is a second renderer.

| Piece | File | Reuses |
|---|---|---|
| What a Spectrum part shows: `spectrumSliceOf` (the slice, the part's radio and its rate), `spectrumMarkersOf`, the two sentences by the window's rule (`PatchPage::setSpectrumHave`) | `src/slint/patch_logic.{hpp,cpp}` | `pc::radioOf`, `pc::findChannel`, `gui/patch_scope_math.hpp` `markerX` |
| The Spectrum well: the trace and the waterfall painted at the part's own size | `src/slint/patch_faces.{hpp,cpp}` (`SpectrumWell`, `wellSizeOf`) | `host_logic.hpp` `paintTrace` and `scrollWaterfallRow` (the receiver's own painters and colour map), `patch_scope_math.hpp` `binsToColumns` / `autoRange` |
| The Map face's view and targets: `wholeWorld`, `fitToTargets`, `panView`, `zoomView`, `tracksOfSources`, `projectTargets` | `src/slint/patch_faces.{hpp,cpp}` | `kit_logic.hpp` `fitView`, `map_compositor.hpp` `TileCompositor` (the map pages' compositor), `slintmap::targetIconPx`, the core's `trackPresentation` (passed in) |
| The reads, the pictures, the gestures: one frame per radio per tick, one compositor per Map part, `FaceLive` rows | `src/slint/patch_publish.cpp` | `read_node_spectrum`, `PluginKit::tracks()` / `basemap()`, `KitPublisher::mapLook()` |
| The faces: `SpectrumFace`, the kit's `MapPane` on the node, the two inspector keys | `src/slint/ui/patch_page.slint` | `MapPane` (`ui/map.slint`) unchanged |

Several Spectrum parts at once work, each on its own feeder and its own radio: a radio's frame is read once a tick however many parts hang off it. A part follows its node's resize (the pictures are painted at the node's size, capped at 640 x 120 + 200, and start again on a reshape). Nothing is read, painted or composed while the PATCH view is not showing (`publish()` does not call into the faces then); the engine stops the patch when the page goes (as before), so a drive step starts it again.

## Proof

The signal generator is the source (`FOXSDR_SLINT_PATCH_RUN=1` is a bounded-run-only seam that lets START and ALL OFF through: a scratch profile has no device but the generator; the patch document is the drive header `patchdoc=`, `FOXSDR_SLINT_PATCH_DOC`). The generator patch radio draws two tones, +300 kHz at -30 dB and -500 kHz at -45 dB of a 2 MS/s capture centred on 100 MHz (`engine_patch_radios.cpp` `makePatchGenerator`). The seeded patch is a Radio, a Spectrum part off it, a Channel at +300 kHz and a Spectrum part off the channel, a Spectrum part wired to nothing, and a Map.

### RED (the base build, patch running on the generator, shots 2 s apart; `pf-red.txt`)

The window shows "RUNNING - 1 of 1 radio open", the channel's level reads -48 dB, and both Spectrum parts are empty wells.
`pf_png.py region` on the Radio spectrum well (445..700 x 505..645): `distinct colours 5; top [((13, 15, 18), 34677) ...]` (one flat ground colour and the border); `diff` between the two shots over the well: `differing pixels 0 bbox None`. The same for the Channel spectrum well: `distinct colours 6 ... differing pixels 0`.

### GREEN (`patch-faces.txt`, 62 steps, 0 failed; `pf-probe.txt` prints the figures)

| Figure | Radio-fed part (s0) | Channel-fed part (s1) |
|---|---|---|
| span / slice | 2,000,000 Hz (-1 MHz .. +1 MHz) | 47,619 Hz (276,190 .. 323,809 Hz): 42 times narrower |
| the tone's column | 180 of 278 = 0.6493 (expected +300 kHz: 0.65) | 138 of 278 = 0.4982 (the middle of the slice) |
| peak over floor | 88.2 dB (-39.0 over -127.2 dBFS) | 90.1 dB |
| marks | 1, "Channel" | 1 (the centre) |
| waterfall | 88 rows lit, a row per frame; `whash` / `thash` differ between two reads 30 ticks apart | the same |

The unwired part says "Wire the radio or a channel here to see its spectrum." and has no picture; before START the Radio-fed part says the same (a Radio feeder with no running rate) and the Channel-fed one says "No spectrum yet - its radio is not running."; after START both sentences are gone.

On screen (`pf-green-1.png` and `pf-green-2.png`, 30 ticks apart): Radio spectrum well `distinct colours 326`, `brightest column x=610 (165 of 255 wide)` = 0.647; `differing pixels 19709`; Channel spectrum well `distinct colours 305`, `brightest column x=884 (128 of 256 wide)` = 0.50, `differing pixels 18477`; the unwired part `differing pixels 0`. The inspector of a selected Spectrum part still reads "This node has nothing to set yet." (`expect ... label equals`). `Larger` (x 1.25) takes the part's pictures from 278 columns to more than 340 and the picture starts again with the tone at 0.65. Frames stop on the receiver view (`framesdelta == 0` over 40 and 30 ticks) and come back after START.

The Map (`patch-faces-map.txt`, 39 steps, 0 failed): the ADS-B decoder plugin decodes its fixture recording (`adsb-decoder.wav`: one aircraft, ICAO 4D2025, callsign FOXSDR1, near 51.5 N 0.1 W, the target `fixture-adsb-decoder.txt` reads back); the seeded patch wires the ADS-B part into a Map part. `patchface.m0.sources == 1`, `targets >= 1`, `ids contains 4D2025`, `fitted == 1`, the sentence "1 aircraft  0 vessels  0 stations", latitude 51..52; the aircraft is drawn on the chart (`pf-map-1.png`); the inspector's `Whole world`: `span >= 360`, lat 0, lon 0; `Fit to targets`: span < 20 and lat > 51 again; `Zoom out` takes the fitted span 0.15 to 0.30 and `Zoom in` back; `drag "Map"#2 80 0` moves the centre west (longitude below -0.11 from -0.10). The first view of an unwired Map part shows the coastline of the world with the scale bar, the keys and the credit "Coastline: Natural Earth (public domain)" (`pf-green-1.png`).

### Break-it

Unit level (apply the mutation, rebuild the two logic tests, run, restore): every mutation went red, and the restored build is green (`test_slint_host_patchfaces: 242 checks, 0 failed`, `test_slint_host_patch: 1350 checks, 0 failed`).

| Mutation | Red |
|---|---|
| the slice ignored (`binsToColumns` over -rate/2 .. +rate/2 always) | `test_slint_host_patchfaces` 4 failed (a Channel-fed tone not in the middle, the other tone's level, the quiet slice) |
| no waterfall row painted | 4 failed (lit rows, frames, hash, a full picture) |
| every radio's channels marked on every part | `test_slint_host_patch` 4 failed (the second radio's marks) |
| the Channel slice not narrower | `test_slint_host_patch` 2 failed (slice bounds, narrower than the capture) |
| the pan's sign flipped | 2 failed (west, north) |
| Whole world span 90 | 2 failed (600 and 360) |
| stale targets drawn | 1 failed (the targets of the Map part) |
| every module's tracks, not only those wired in | 4 failed |

Drive level (`read_node_spectrum` never called, and no tracks given to the Map part; one build): `patch-faces: 62 steps, 62 ran, 20 failed` - the first at `state patchface.s0.live == 1 -> FAILED: it is 0`, the sentence staying "No spectrum yet - its radio is not running."; `patch-faces-map: 39 steps, 39 ran, 12 failed` - the first at `state patchface.m0.targets >= 1 -> FAILED: it is 0`. Restored and rebuilt: 62 and 39 steps, 0 failed. **Own mistake, caught:** the first restore script restored the second mutation's backup over the first, leaving the no-feed mutation in `patch_publish.cpp`; a re-run of the green script failed the same way (20 failed), `git diff` showed it, the line was put back and the rebuilt scripts passed. Every mutation text was then searched for in the three files: none remains.

## Tests added

* `test_slint_host_patchfaces` (new, `src/slint/tests/test_slint_host_patchfaces.cpp`, 242 checks): the pictures' size, the tone's column on a Radio-fed and a Channel-fed slice, the floor, the scroll, reshape, the unusable inputs; Whole world, Fit to targets, pan, wheel zoom and the limits; the targets projected, filtered, capped.
* `test_slint_host_patch` (1350 checks): `theSpectrumSliceAndItsMarks` (two radios, their slices and marks, the sentences per part with one radio's frame and not the other's); the display test now says what changed (a radio that is open with no frame has the sentence).
* Drive: `patch-faces`, `patch-faces-map` (+ `fixtures/patch-faces.patch`, `patch-faces-map.patch`, the header `patchdoc=` of `test_slint_drive_run`, `.gitattributes` keeping the documents LF).
* Ledger: `docs/slint-drive-coverage.md`: 719 asserted (716 before), 117 driven, 1543 not drivable, 0 unclassified.

## Ledger rows changed (`docs/slint-port-tick.md`)

1924, 1925, 1940: DEFERRED -> PRESENT, each naming what the face leaves out of the old part; 1941 (the Map help names keys that now exist); the block's subtotal (PRESENT 142 -> 145, DEFERRED 11 -> 8); the paragraph that named the three DEFERRED rows. Row 1945 untouched.

## New strings for the 34-language pass

None. `@tr("Spectrum")` (the face's accessible name), `@tr("Fit to targets")`, `@tr("Whole world")` and the compositor's "Coastline: Natural Earth (public domain)" are keys all 33 catalogues already hold; the sentences are the old window's own.

## Finding: the PATCH pill running while the top strip says stopped (read from code, not changed)

In the owner's session the PATCH pill said "PATCH RUNNING - 1 of 1 radio open" on the B200 while the top strip said "SIGNAL PATH STOPPED / RECEIVER STOPPED Signal generator" with an advisory that the signal generator stands in for the saved B200.

1. Intended in principle. A running patch holds the radio: START lends the receiver's radio to the patch (`engine_patch_radios.cpp` ~270-285: `selectSource(0)`, `patchMainKeep_`, the device ledger records a loan, the diagnostic log says "the receiver's radio ... is handed to the patch page; the receiver runs on the signal generator until the patch is stopped"), and the START key's tooltip says the same ("The receiver hands its radio over and runs on the signal generator."). The receiver strip therefore reads the generator, and "stopped" is the receiver, not the patch.
2. The words do not explain the state when the advisory is the other kind. `signal_logic.cpp` shows "%s is still the saved radio. The signal generator is running in its place for this session only - FoxSDR will try the radio again next time it starts." whenever the engine's `fact.status.restoreKeepLabel` is set (a saved radio that failed to reopen at start-up) and the generator is active (`keepPreview`, line 569); that condition does not look at the patch. A patch that then opens the very radio the receiver could not (it is free) puts both statements on the screen: the pill says the radio is open and running, the advisory says it is "not open" and will be tried "next time". The Source label then reads "(saved, not open)" and not the patch-held text the section would otherwise show (`o.patch_held`, line 594-597), though the chip does say "ON PATCH".
3. Which of the two was the owner's case is UNVERIFIED (his sequence was not reproduced: it needs the B200). If it was the second, the fix is a sentence: when `patch_held` names the saved radio, say "held by the patch" instead of the restore advisory. Not done here.

## Gates

STOPPED EARLY on the orchestrator's message (usage limit); to be resumed after Monday 02:05. State at the stop, build `D:/foxsdr-build/en-screen-k` fully built (ALL_BUILD, exit 0) at the code tip (the record is the only commit after it), hidden desktop `FoxSDRTests`, `FOXSDR_FRAME_CAP_HZ=60`, logs in `D:/foxsdr-build/pf-scratch/gate-*.log`:

* `ctest -R patch` (56 tests, includes slint_drive_patch-*, patch-faces, patch-faces-map): `100% tests passed out of 56`.
* Headless gate `-L "headless|slint"` (1149 tests) in three slices, 11 reds: the five known false failures (test_airband_data, test_band_plan, test_converter_call_sites, test_page_resize_id, test_png_decode) and six load flakes, each re-run alone and PASSED: test_failure_files, test_engine_service_keep, test_engine_api_merge (failed twice in a batch re-run, passed alone in 32 s), test_spyserver_source, test_pass_control, test_patch_dest_async_stepped. Nothing of mine red.
* GUI gate `-L gui` (346 tests) in slices: slice 1 (121 tests) 100 % passed; slice 2 (121) 100 % passed; slice 3 (106 tests) was RUNNING when stopped: 51 done, one red so far, `slint_drive_tools-airband` (not yet classified: re-run it alone; it passed in lane M's gate; this lane touched neither the Tools bank nor the airband), the rest unknown. Its log is `gate-g3.log`; the process may have finished by the time this is read.
* NEXT: read `gate-g3.log` (or re-run `gui` tests 241-400), re-run each red alone and classify; then stamp tests (refresh the build after the last commit, so test_diagnostics / test_diagnostics_free are not stale) and re-run those two; re-prove the config hash; fill the final report. Not done: the "Listen to the tower" Blueprint run itself (needs a receiver position in a config seed; the seeded patch has the same shape).
* Real `%APPDATA%/foxsdr/config.json` sha256 at the stop: `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` (unchanged). No process of the owner's was touched; nothing visible; nothing pushed.

## Not built, UNVERIFIED, decisions

Not built (the old part's): the trace-mode hold and average overlay, the user's frequency markers on the Spectrum part, the parts' tips and their right-click menus; on the Map part the trails and paths of its sources, the altitude colours, a target's details on a click, the map pages' layer and replay keys. The Spectrum part scales by the automatic range of its own columns (the old part's default); the user's manual range is not read.
UNVERIFIED: a real radio (none was opened); the faces under a real pointer and a screen reader; the tower Blueprint itself (the seeded patch has the same shape - a Radio, a Spectrum off it, channels - but the Blueprint needs a receiver position or a bookmark group, not in a scratch profile); the clip sweep and the translated languages (no new string, the sweep was not re-run); a basemap-tiles Map part (the scratch profile has no basemap plugin: the coastline path is what ran); more than one Map part; the Spectrum part at a zoom below 0.6 (the face is off there, as for every part). Frame cost: the base run's last 300 ticks `mean 1.32 ms, p95 2.39 ms, worst 4.04 ms`; the same script on the new build, three runs on a busy machine: `mean 1.58 / 1.40 / 1.37 ms, p95 3.64 / 3.09 / 3.34 ms, worst 12.9 / 9.6 / 10.8 ms` - about 0.1 to 0.3 ms more a tick on the mean with two live Spectrum parts and a Map composing.
Decisions for the owner to confirm: (1) a fresh Map part opens on the receiver's position at 20 degrees when it is known, else on the whole world, and is fitted to its targets the first time there are any (the old part's `setHome` + `requestFitToTracks`); (2) the bounded-run seams (`FOXSDR_SLINT_PATCH_RUN`, `FOXSDR_SLINT_PATCH_DOC`, `state patchface.*`) exist only in a bounded run and are never set by a release.
