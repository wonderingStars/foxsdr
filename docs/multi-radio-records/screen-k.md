# Screen lane K: the remaining rows of the Slint window (2026-11-12)

Branch `feat/screen-k`, started from `engine/multi-radio` 090e7039 (`git merge --ff-only 090e7039`). Worktree `D:\foxsdr-wt\en-screen-k`, build `D:/foxsdr-build/en-screen-k` (Slint ON, clip probe ON, Release). Nothing is pushed or merged.
Read first: `screen-lanes-common.md`, `STANDARD-RULES.md`, the records of lanes B and C, the feature records of `sysmodels`, `access`, `rfmon`, `dspmodels`, `sigmeas`, `gpufft`.

## Step 0: the table of rows (reading only, before code)

| Row (list, text) | Class | Notes |
|---|---|---|
| REMAINING-GAPS Setup: first-run wizard and starter presets | screen only | `core/sys_first_run.hpp` (the model) is used as it is; the wizard does NOT select a source (no radio may be opened; see hand-offs) |
| Setup: one-file settings export, import, backup | screen only (the file work), pending until exit | `sys_bundle` over the three settings files; the engine rewrites the settings file when it stops, so an import cannot be applied under a running engine: pending, done after the engine's last save |
| Setup: whole-app reset to defaults | screen only, pending until exit | `sys_reset` (moves files aside, undo exists) |
| Setup: settings profiles (and sync folder) | screen only, pending until exit | `sys_profiles` |
| Setup: telemetry on/off switch | already built | SYSTEM > Usage reporting ("Send anonymous usage reports", `SystemEvents.usage-send`, op `FOXAPP_OP_TELEMETRY_CONSENT`): the row text in REMAINING-GAPS is out of date; the wizard offers the same switch |
| Setup: CPU/GPU/memory readout, low-end / reduced-effects mode | screen only | the meter and the policy (`sys_resource_meter`, `sys_effects_policy`) sampled in the window's process; applied to the waterfall pacer and the skin |
| Accessibility: spoken frequency / mode / S-meter | screen only (plays through SAPI, nobody has listened) | `access_speech` announcer, `SapiDeviceSpeaker` in `lanek_audio.cpp` |
| Accessibility: sonification | screen only (plays through waveOut, nobody has listened) | `access_sonify` Sonifier + ToneRenderer, `ToneOut` in `lanek_audio.cpp` |
| Accessibility: colour-blind-safe UI palette picker | screen only | `access_palette` roles written over the skin (`Theme.skin`) |
| Accessibility: locale-aware number display | screen only | `access_number_format` through `host_logic setNumberLocale` (four readouts) |
| Accessibility: unit choice (imperial, metric, aviation, nautical) | NOT BUILT | the window has no altitude or speed readout of its own; the wiring sites are `track_metrics.hpp` and `map_view.cpp` (the map is lane J's) |
| Accessibility: screen-reader exposure of waterfall, dial faces, map | partly built | spectrum and waterfall sentences built (`SrText`); the counter's sentence is published but its `.slint` binding is not (digits.slint, a recompile); the map is lane J's |
| Accessibility: right-to-left layout | NOT BUILT | the access lane measured that Slint 1.18.1 DOES order Hebrew and join Arabic, so the brief's "build nothing" case does not apply; but no RTL catalogue exists and mirroring the layout is a pass over every `.slint` (hand-off 7) |
| DEPTH-GAPS A2 Multi-radio: Matrix overview with hover-to-listen | screen only | `radio_matrix_model` (`RadioMatrix`, `HoverListen`); the audio goes through `FOXAPI_OP_RADIO_SOLO` |
| DEPTH-GAPS A2 Display: averaging-mode choice | screen only | `dsp/trace_average` per radio, applied to the trace picture only |
| A2 Display: waterfall gradient editor | screen only | `core/gradient_model` (`GradientEditor`, `toTable`); typed colours and sliders, not a pointer editor |
| A2 Display: zoom to a dragged selection | screen only | `core/zoom_select` through `SpectrumEvents.zoom-selected`; Ctrl+drag, or a tick that makes a plain drag select |
| DEPTH-GAPS Part B: eight-cursor tuning tool (Wavecom) | screen only | `dsp/sigmeas_cursors` `CursorSet8` (free, fixed spacing, harmonics); drawn into the trace; levels read from the displayed frame |
| Spectrum limit lines / mask | screen only | `core/spectrum_mask` (parse, evaluate); drawn over the trace |
| Spectrum RBW readout and detector choice | screen only | `dsp/rbw_detector` definitions, applied per display column to the displayed bins; RBW = the Blackman-Harris ENBW the pipeline runs |
| REMAINING-GAPS Community: forum and Discord links | screen only | App menu entries + a section; hidden while the address is empty |
| Survey S19: GPU/CPU FFT switch | NOT BUILT | `gpufft` is "wired to nothing" (its record): no fact `gpu_fft.mode` exists in the engine; a control would do nothing |
| Survey S24: record-window start/stop | NOT BUILT | the screen recorder is the plugin `vsthost`'s, shown through its own panel; the ABI offers no key route from the host's side |

No row needed `engine.cpp`, `pipeline.cpp`, `receiver.hpp`, `demod.cpp`, `plugin_abi.h`, `fox_engine_api.cpp`, `config.cpp` or the recorder to be edited.

## Architecture (what a merger needs to know)

* ONE generic row panel (`ui/system_bank.slint`: `KRow`, `KPanel`, the global `LaneK`) carries every body and sheet. The host (`lanek_publish.cpp`) builds the rows (`lanek_logic.cpp` has the pure parts) and answers `LaneK.act(id, text)`; a row therefore costs C++ and no `.slint` recompile (14 to 45 minutes). Row kinds: heading, note, key, tick, choice list, text field, readout, slider, bar.
* Five sections after About in the SYSTEM bank, logical rows 10 to 14 (the window's `system-extra-base` says where they start: 10, or 9 while the Beta tester row is absent): Settings and backup, Performance, Accessibility, Community, Spectrum tools. The Display body gets `rows-display` at its foot.
* The saved choices are lane K's own file `lanek.json` beside the settings file (the arrangement of lane C's `lanec.json`): NO key was added to `AppConfig` or to either half of `config_halves.hpp` (decision 2).
* Shared files edited, all by appending: `main.slint` (imports, exports, five body lines, two sheets, `system-extra-base`), `main.cpp` (one block constructing the publisher, a menu pair, a tick line, a state hook, shutdown), `spectrum.slint` (the selection drag, `SpectrumEvents.select-mode` / `zoom-selected`, the screen-reader description), `waterfall.slint`, `radio_pane.slint`, `radio_strip.slint` (the Matrix key and global), `theme.slint` (`SrText`), `host_logic.cpp/.hpp` (`setNumberLocale`), `input_script.cpp` (the test-only verb `radio zoomsel`), `src/slint/CMakeLists.txt`, `src/slint/tests/CMakeLists.txt`, `tools/slint_clip_sweep.py` (a `lanek` view), `docs/slint-port-tick.md` (rows 5200 to 5236), `docs/slint-drive-coverage.md` (regenerated), the 33 `resources/lang/*.json` and `src/core/lang_assets.hpp` (regenerated: a merge hot spot, regenerate with `py -3.14 tools/embed-lang.py` after merging the catalogues).

## What was built, and the proof of each

(Real result lines. The drive scripts are `src/slint/tests/drive/lanek-*.txt`; "restart" = the same scratch profile started a second time.)

| Row | What the user gets | Proof |
|---|---|---|
| Export, backup | SYSTEM > Settings and backup: "Export settings" writes `foxsdr-settings-<stamp>.foxsdr-bundle` into the profile's exports folder (passwords, tokens and the install id left out; the note says how many); "Back up settings now" / "Back up automatically when FoxSDR closes" write into `backups/`, pruned to the newest 5, 10, 20 or 50 (only files of that exact name shape are ever removed) | `test_slint_lanek_host`: the bundle holds no secret, 5 backups pruned to 3 with a foreign file untouched, keep 0 clamped to 1; drive `lanek-backup` (`lk.status contains Exported`, `lk.exports == 1`, `Backed up`) |
| Import | "File to import" (a list of the exports folder and a path field), "Import settings..." opens a sheet that says how many files it replaces (the old ones are kept as `.before-import` copies); "Import and close FoxSDR" sets the work pending, which runs after the engine's last save. The apply plans the import AGAIN at that moment (the engine's last save changed the files' bytes, which invalidates the model's confirmation token) | drive `lanek-backup`: sheet 2, Cancel changes nothing (`lk.pending == 0`), Import sets `lk.pending == 1`, log line `lane K at exit: pending import applied`, and the SECOND START reads `cfg.uiTheme == night` (the theme in the exported file) after the run had set it back to Today's bench; unit test: the config changed between plan and apply still imports, junk / empty / wrong-format files refused |
| Profiles, sync | name a profile and save it (never carries the install id), pick one and "Use this profile" (a pending switch that keeps this computer's own network switches and passwords as the last save left them), delete; a shared folder syncs profile files (push, pull, a conflict is reported and left as it is) | `test_slint_lanek_host`: save/overwrite refusal, a path-like name refused, switch keeps `KEEPME-AFTER-SAVE`, delete, sync push/pull/same/conflict (both files byte-identical afterwards); drive `lanek-backup` (`lk.profiles == 1`, `Saved the profile`) |
| Reset, undo | "Reset everything to defaults..." lists the files that will be set aside (settings, bookmarks, markers; plugins and logs only if ticked), "Reset and close"; "Undo the reset" brings them back | `test_slint_lanek_host`: 2 items (3 with plugins), a foreign file and `lanek.json` left alone, the files move into `reset-<stamp>`, the undo restores them, a settings folder not named foxsdr is refused. The drive run is on a scratch folder that is not named foxsdr: the sheet shows the refusal and its key is dead (`expect "Reset and close FoxSDR" enabled equals "false"`) |
| First-run setup | a sheet of four pages (welcome, which radio, starting stations, anonymous usage reports); Next is dead until a radio is chosen; finishing adds the chosen stations as bookmarks (tune, mode, add; the tuning is put back) and sends the existing consent op; opens by itself on a NEW profile (decided before the engine's start writes the settings file; never in a bounded run) and from "Run the first-time setup again" | drive `lanek-wizard` (`FOXSDR_SLINT_FIRSTRUN=1`): steps 0, 1, 2, 3, `Next` disabled then enabled, `lk.action == wizard-done`, `lk.status contains starting station` |
| Performance | CPU (this program), memory (this program and the computer), graphics readouts once a second (a figure that cannot be read says n/a); "Reduced effects": Automatic / On / Off; when on: waterfall at most 15 lines a second, shadows and lamp glow off the skin | drive `lanek-perf` (`lk.samples >= 2`, `lk.reduced` 1 after On, 0 after Off, `lk.perfmode`); one sample costs: see frame cost |
| Colour-blind palettes | Colour set: Normal, green-weak, red-weak, blue-weak: the status, trace and radio colours, the foreground and accent are taken from `access_palette` and written over the skin in force (the high-contrast skin stays as it is) | drive `lanek-access` (`lk.palette` protanopia, deuteranopia, tritanopia) and screenshots; the contrast and colour-vision numbers of the palettes are `test_access_palette` (access lane) |
| Number format | "Follow the Windows regional settings" (default; a bounded run keeps the dot unless `FOXSDR_SLINT_LOCALE`) or "Always use a dot"; digits Latin / native / as the language prefers; applies to the frequency, span, kHz and cursor readouts | `test_slint_host_numbers` (19 checks): default text byte for byte, comma, a negative, Arabic-Indic digits and decimal sign, back off |
| Spoken readout | "Speak the frequency, mode and level now", "Speak changes automatically" and the announcer's policy (what, wait times, smallest change, level step, interrupt); Windows voice, default output; in a bounded run a recording stand-in | drive `lanek-access` (`lk.spokencount`, `lk.spoken contains megahertz`); the announcer's rules are `test_access_speech`. NOBODY HAS LISTENED |
| Signal tone | a tone whose pitch follows the signal level (parameters, curve and volume, mute while the squelch is shut) through waveOut, 50 ms blocks refilled from the tick, no thread; off until ticked; never opened in a bounded run | drive `lanek-access` (`lk.toneon == 1`, `lk.tonehz > 100`, `lk.toneon == 0`); NOBODY HAS LISTENED, no device was opened in any test |
| Screen reader | the accessible description of the spectrum, the waterfall (and each pane) is a sentence refreshed at most twice a second: tuned frequency and mode, the strongest signal and its height over the noise, the span | drive `lanek-access` (`lk.sr.spectrum contains Spectrum, tuned to`, `megahertz`; `lk.sr.waterfall`; `lk.sr.counter`). NOT run through a screen reader |
| Community | App menu "Forum" / "Discord" and a section with the same keys, hidden while their addresses (`community.forum`, `community.discord` in `lanek.json`, https only) are empty | drive `lanek-community` (`lk.forumurl == ""`, `lk.discordurl == ""`); no address was invented: see decision 5 |
| Averaging | Display body: Off / Linear / RMS / Log / Exponential, frames or a time constant; applied to the trace of every radio, not to the peaks, the measurements or the waterfall | drive `lanek-spectrum` (`lk.avgmode` 2 then 0); the averages' closed forms are `test_trace_average` |
| Zoom to a selection | tick "Zoom to a dragged selection" (or hold Ctrl) and drag over the plot: a rubber band, release zooms to it; a drag is not a click (no tune) | drive `lanek-spectrum`: tick (`lk.zoomselect == 1`), then `radio zoomsel 0.3 0.4` (the plot's own call) narrows the view (`lk.zoomspan` between 1 and 2 MHz); the real pointer drag is UNVERIFIED |
| Gradient editor | "Gradient editor..." sheet: use-my-gradient tick, blend, a colour and a position per stop, add, remove, reverse, reset; the waterfall paints through the 256-step table | drive `lanek-tools` (`lk.sheet == 7`, `lk.gradon`, `lk.gradtop == 16711680` after the last stop is set to FF0000, `lk.gradstops == 3`) |
| Detector, RBW | Spectrum tools: As drawn / Peak / Negative peak / RMS / Voltage average / Log average / Sample, applied per column; "Resolution bandwidth" readout | `test_slint_lanek_spectrum_host` (12054 checks): the closed forms on 200000 noise bins (voltage average -1.05 dB and log average -2.51 dB against RMS, within 0.06), a tone reads the same in every detector, edges and nulls; drive `lanek-spectrum` (`lk.detector`, `lk.rbw > 0`) |
| Limit mask | a mask file (`foxmask 1`, `upper` / `lower` lines), "Write an example mask file" fills in a path in the exports folder, "Load the mask", "Show the limit lines": red upper / cyan lower lines over the trace, worst margin and frames in breach | `test_slint_lanek_spectrum_host`: lowest upper line wins, highest lower line wins, interpolation, parse refusals, the painted pixels; drive `lanek-tools` (`lk.maskloaded == 1`, `lk.maskpass`) |
| Eight cursors | "Eight-cursor tool..." sheet: free / fixed spacing / harmonics of cursor 1, a frequency field per cursor with offset, ratio and level, "place the next cursor by a click", on the tuned frequency, snap to peaks; drawn into the trace (dotted line and a tab) | `test_slint_lanek_spectrum_host` (the tab and line pixels, the level under a cursor); drive `lanek-tools` (`lk.cursormode == 2`, `lk.cursor.0 == 100100000`, `lk.cursor.3 == 400400000`) |
| Matrix | "Matrix" key at the head of the Audio mix block: a sheet with a row per open radio (frequency, mode, level, state); a pointer resting on a row previews that radio's sound through `RADIO_SOLO` after the model's dwell (0.15 s), leaving restores the baseline after its grace (0.3 s); a click keeps that radio (a second click lets it go); a double click selects it; closing the sheet puts the sound back | drive `lanek-matrix` (`testradios=3`): `lk.tiles == 3`, click keeps (`lk.baseline == 2`, `lk.matrixsolo == 2`), again lets go, another, Esc closes and `lk.matrixsolo == 3`. The pointer's own hover is UNVERIFIED (a scripted click moves the pointer onto the row) |

## Break-it checks (mutation applied, test rebuilt and run, red seen, source restored and test green again)

| Mutation | Red |
|---|---|
| record: `getInt` does not clamp | `test_slint_lanek_host` 125 checks, 1 failed |
| keeper: the apply uses the stale confirmation token of the plan | `test_slint_lanek_host` 3 failed |
| keeper: a sync conflict is decided by pushing the local file | `test_slint_lanek_host` 1 failed |
| keeper: the backup is not pruned | `test_slint_lanek_host` 2 failed |
| detector: the voltage average taken over the powers | `test_slint_lanek_spectrum_host` 2 failed |
| detector: negative peak reads the strongest bin | `test_slint_lanek_spectrum_host` 1 failed |
| mask: the lowest upper line does not win | `test_slint_lanek_spectrum_host` 2 failed |
| cursors: the tab is not drawn | first GREEN (the test read a pixel the dotted line also covers); test strengthened (a pixel beside the line in the tab's rows); then `test_slint_lanek_spectrum_host` 1 failed |
| cursors: the level is the weakest bin in the search width | `test_slint_lanek_spectrum_host` 1 failed |
| numbers: the locale is ignored | `test_slint_host_numbers` 19 checks, 9 failed |

NOT break-it-checked: the window halves (publisher rows and acts, main.cpp wiring, `.slint`): a mutation of `lanek_publish.cpp` is a five-minute build and a 40 to 60 second drive each; the drive scripts read the effect back from the host's state (the file work through a second start), which is what a broken wiring would change.
A finding while writing the drive: a first version of the import failed ("the files here changed since the plan was made, or the token is wrong"): the engine's final save changes the settings file's bytes after the user's confirmation, so a token made at confirmation time is stale at apply time. The apply now plans again at exit (`SettingsKeeper::apply`), and a unit test pins it.

## Frame cost (the method of lane B: `FOXSDR_SLINT_FRAME_LOG=1`, hidden desktop, one radio, 900 ticks, `drive/measure/lanek-off.txt` vs `lanek-on.txt`, `run_lanek_cost.bat`; THE MACHINE WAS SHARED)

"off" = `FOXSDR_SLINT_LANEK_OFF=1` (lane K's tick, the trace hooks and the screen-reader sentences left out). "on" = RMS averaging, the Peak detector, a loaded mask drawn and evaluated, the cursors drawn with their levels, the resource meter once a second, the Matrix closed, the screen-reader sentences.

| Run (1600 x 2600 window, the tone's step failed to find its control, so the tone's parameters were NOT computed in this set) | tick interval mean / p95 / max | the tick's own work mean / p95 / max |
|---|---|---|
| off, three runs | 33.30 / 34.00 / 34.5, 33.29 / 34.04 / 34.5, 33.33 / 34.05 / 39.4 ms | 0.71 / 1.39 / 1.8, 0.71 / 1.25 / 4.6, 0.80 / 1.55 / 5.5 ms |
| on, three runs | 33.34 / 34.10 / 49.9, 33.30 / 34.00 / 43.4, 33.43 / 34.08 / 116.2 ms | 0.92 / 1.59 / 8.7, 1.04 / 1.83 / 13.1, 1.03 / 1.84 / 13.8 ms |

So lane K's per-frame elements cost about 0.2 to 0.3 ms a tick on the mean and nothing at p95 of the interval; the maxima are the script's clicks and the once-a-second resource sample. A second set at 1600 x 5200 (the window tall enough to show both sections AND the tone key) was too heavy for this shared machine (interval means 37 to 55 ms in off AND on): its work means were 1.23 to 1.54 ms off and 1.95 to 2.84 ms on, i.e. the window's own size dominates; kept in `D:/foxsdr-build/sk-cost-5200` (not committed). The resource meter's own cost (one `sample()`, PDH counters included, once a second) is exposed as the state `lk.samplems` (a moving mean) but its value was NOT recorded in a run (UNVERIFIED: the once-a-second tick maxima of 8.7 to 13.8 ms in the "on" runs include it).

## New strings

238 + 12 new keys went through the string tables in all 33 catalogues; `tools/i18n_keys.py`: 2911 keys, 0 empty, 0 missing, 0 stale in every catalogue. The translations are MACHINE-MADE (five Sonnet agents, 33 languages, placeholders checked by script) and UNCHECKED by a speaker. The English keys are the `tr("...")` literals of `lanek_publish.cpp`, `lanek_logic.cpp` and the six `@tr` words of the new `.slint` rows ("Matrix", "Radio matrix", "heard", "Close", the sheet's own words). The setup's radio and starter names come from `core/sys_first_run.cpp` as plain text; `wizardStrings()` in `lanek_logic.cpp` lists them for the extraction tool. Not translated: the phrase table of the spoken readout (`access_speech`, English words such as "megahertz") and the 5x7 face's words.

## Not built, with the reason (and the exact hand-off)

1. UNITS (imperial / metric / aviation / nautical): the Slint window shows no altitude or speed of its own. Wiring: `track_metrics.hpp:1252, 1273, 1276, 1378-1379` and `map_view.cpp:89-103` call `access_units.hpp quantify()` with the chosen system (setting `access.units`); the map's files are lane J's.
2. RIGHT-TO-LEFT: Slint 1.18.1 orders Hebrew and joins Arabic (access.md section 6), so "if Slint cannot shape RTL, build nothing" does not apply, but nothing could be shown: there is no Arabic, Hebrew or Persian catalogue and `start` alignment does not mirror (measured by the access lane). Needs: the three catalogues, a font check, a `Theme.rtl` flag and a pass over every `x:` / `alignment: start` in `ui/`.
3. COUNTER AND MAP SCREEN-READER TEXT: `SrText.counter` is published every half second (`lk.sr.counter`) but nothing in `digits.slint` reads it (adding `accessible-description: SrText.counter` to the counter's group is one line and one `.slint` recompile). The map's sentences belong to lane J.
4. GPU FFT SWITCH (S19): the engine has no `gpu_fft.mode` fact and the pipeline does not call `GpuSpectrumEngine` (gpufft.md: "wired to nothing"). Once `core/pipeline.cpp` owns it, the row is: choice "Spectrum FFT on" CPU / Auto / GPU, fact `gpu_fft.mode` (0 Cpu, 1 Auto, 2 Gpu), read-only `gpu_fft.backend`. A control now would do nothing.
5. SCREEN RECORDING (S24): the plugin `vsthost` owns its recorder and shows its own panel; no host key route exists. Nothing was added.
6. The first-run setup does NOT select the chosen radio as the source (no radio may be opened here, and the engine's device list is the only honest way to name one): it uses the choice for the starting stations and says nothing more. Wiring: `FOXAPI_OP_SELECT_SOURCE` with the first listed device of that family.
7. FOLLOW A MASTER (the rest of `radio_matrix_model.hpp`) and the Matrix's cell grid: the sheet is a table of rows.
8. Tuning accuracy of the cursors' levels is the displayed frame's (a bin), and the gradient editor has no pointer editing of stops.
9. The strength key under the waterfall still shows the chosen colour map, not the user's gradient (the waterfall itself uses the gradient).
10. Automatic relaunch after an import, a reset or a profile switch: the window closes and says so; the user starts FoxSDR again. (A relaunch needs the instance claim released first: a release-lane decision.)

## Engine hand-offs (none was made into an engine file)

* A "settings replaced" op (or a reload) would let the import and the reset apply without closing the program: today they are pending until exit because the engine's last save would overwrite them.
* The settings file reflects the interface half a few seconds after a change (the drive had to wait): "Export settings" reads the file, so it exports what was last SAVED. An op that forces a save first would make it exact.
* `fact.perf.*` and `perf.mode` (sysmodels hand-off step 5): not needed; the window samples in-process. If the engine should also reduce its own work (pipeline frame rate), it needs them.
* `lanek.json` keys (`backup.auto`, `backup.keep`, `perf.mode`, `access.*`, `speech.*`, `sonify.*`, `spec.*`, `community.*`, `sync.folder`): to move them into `AppConfig` / the interface half, append rows to `FOXSDR_CONFIG_FIELDS` (config_halves.hpp) and read them from `duties.interfaceHalf()`.
* Portable mode, device-arrival and resume watchers, keep-awake, manifest signature, code signing (sysmodels hand-off 1 to 4, 6, 7): engine / `main.cpp` / updater work, not screen rows.

## Decisions for the owner to confirm

1. The five new sections sit AFTER About in the SYSTEM bank (append-only for a mechanical merge): moving About to the end is a three-line change in `main.cpp` and `main.slint`.
2. The lane's choices are in `lanek.json`, not in `config.json` (no change to `AppConfig` or the config halves).
3. Import, reset, profile switch and undo take effect when the program closes, and the user starts it again.
4. "Number format" defaults to following the Windows regional settings (the access lane's default). On this PC (a dot) the text is unchanged; a comma locale will see commas in four readouts.
5. Forum and Discord: no address is invented. The owner supplies them as `community.forum` / `community.discord` (https only) in `lanek.json`, or a build constant. QUESTION: what are the two addresses?
6. The wizard offers the telemetry switch defaulted ON (as the sysmodels record decided and as the Usage reporting section does today).
7. Averaging, the detector, the mask and the cursors act on the displayed trace only (not on peaks, measurements or the waterfall).
8. The Matrix's hover dwell (0.15 s) and grace (0.3 s) are the model's defaults, UNVERIFIED as to feel.

## UNVERIFIED

* The spoken readout and the tone were never heard and no sound device was opened in any test; SAPI voice availability and the waveOut format are from the Windows documentation. No screen reader (NVDA, JAWS, Narrator) was run.
* A real pointer: the Matrix's hover-to-listen, the plot's rubber band drag and Ctrl+drag (the scripted drag starts on a control; the host calls were driven instead).
* A real first run on a clean profile, a real shared folder for the sync, a comma locale on a real machine (the number test uses made-up symbols).
* Reduced effects "Automatic" under real load (the thresholds are the model's; On / Off were driven); GPU counters on other machines.
* The effect of the palettes on every page (looked at on the screenshots of the access drive; no sweep of all pages).
* The clip sweep: see Gates.

## Gates

Run at the tip (Release, `D:/foxsdr-build/en-screen-k`, a shared machine; about a dozen other lanes were building). The tree was rebuilt after the last source change (a `.slint` tooltip, comments on action ids, two Chinese strings) and the lane's tests re-run on that build; the full headless and GUI gates were run on the build before those three small edits (stated, not hidden).

* `ctest -L "headless|slint" -j4 --timeout 300` (1025 tests): `98% tests passed, 22 tests failed out of 1025`. Sorted: the five known false failures (`test_airband_data`, `test_band_plan`, `test_converter_call_sites`, `test_page_resize_id`, `test_png_decode`); `test_engine_api_decode_radio` and `test_engine_api_source_kinds` (red in the base tree too, sysmodels record); load flakes that PASS alone (`test_discord_presence` 8.1 s, `test_engine_api_transmit` 107.6 s, `test_gui_file_jobs`, `test_link_request_poll`, `test_pa_stream_list_guard`, `test_patch_dest_async(_stepped)`, `test_plugin_unload_handshake`, `test_record_start`, `test_health_app_stepped`, `test_usage_diag_window_free`, `test_stop_ends_recordings_free`). THREE WERE MINE and are fixed: `test_slint_house_rules` (12 untranslated literals: the host's action ids in `main.slint`, now commented), `test_i18n` (a fixed buffer in `lanek_publish.cpp`, and two Chinese translations that reordered `%d` and `%s`), `test_json_dump_policy` (a `dump()` in my test). Re-run after the fixes: `test_i18n`, `test_i18n_glyphs`, `test_slint_house_rules`, `test_json_dump_policy`, `test_slint_lanek_host`, `test_slint_lanek_spectrum_host`, `test_slint_host_numbers`, `slint_drive_ledger` (2306 rows, 0 unclassified), `test_slint_system`, `test_slint_constraints`, `test_config_halves`, `test_slint_clip_probe`, `test_slint_elided`: all Passed (11 of 11 and 4 of 4 in two batches).
* GUI gate, hidden desktop, `FOXSDR_FRAME_CAP_HZ=60`, `-L gui -j3`: `99% tests passed, 2 tests failed out of 327`: `slint_drive_add-kinds` (a load flake: Passed alone, 69.7 s) and `test_health_app` (the known intermittent; it failed again alone). The eight `slint_drive_lanek-*` scripts all Passed in the gate and again on the final build (`slint_drive_system-look` failed once in that batch under load and Passed alone, 24 s).
* Clip sweep (probe build at the tip, hidden desktop, `--jobs 3`, the views `system` and the new `lanek`, 34 languages): the first run found 94 UNEXPLAINED items at 2 sites; 86 were a new site of this lane (`system_bank.slint:331`, the words of a choice elided in the 254 px column: the colour sets and the number format in many languages). Fixed by passing the option's own words as the tooltip and explaining the site in `tools/slint-clip-explained.tsv` (the same rule as every other elided label). Second run: `SWEEP: 1639 items (EDGE 8, ELIDE 1427, OUT 204); 1631 explained, 8 UNEXPLAINED at 1 sites in 4 languages`: the 8 are `keys.slint:285` "Full span" (the toolbar's zoom key, lane B's, el, et, ru, vi) - 4 in each of the two views, the same figure as the baseline's "4 per bank view": no growth. The other bank views and the multi-radio views were NOT re-swept (no lane K element is in them; UNVERIFIED by a sweep).
* Frame cost: see above.
* Real `%APPDATA%/foxsdr/config.json` sha256 before and after: `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` (read at the end; every run used a scratch profile). No outside connection, no download, no account, nothing pushed. One of my own long commands was moved to the background by the tool's timeout (a build, twice), never a ctest; no test process of mine was left running and no dialog was seen.
