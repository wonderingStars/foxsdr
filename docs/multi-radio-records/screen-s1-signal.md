# Screen lane S1 (wave 8): the SIGNAL page's receive controls for engine commands that had none (2026-11-16)

Branch `feat/screen-s1-signal` (the name `feat/screen-s1` is taken by an older, merged lane and its record `screen-s1.md`), cut from the merged engine tip `d4f855c5`, worktree `D:/foxsdr-wt/en-screen-j`, build `D:/foxsdr-build/en-screen-j`, scratch `D:/foxsdr-build/s1-scratch/`, hidden desktop `FoxSDRS1`. Nothing pushed.
The base was NOT fully built before the first change: the full `ALL_BUILD` of about 1100 test programs was started, found impractical on the busy machine and stopped by exact PID; only the targets the lane touches were built (foxsdr-slint, the tests named below). UNVERIFIED: the rest of the tree at this tip.

Architecture: `ui/s1_signal.slint` (global `LaneS1`, three row lists) mounted by three `KPanel` lines in `main.slint`; `s1_logic` (pure), `s1_meter.hpp` (the meter's words, quoted by `signal_logic.cpp`), `s1_publish` (rows built from the SELECTED radio's published state and facts about six times a second and after each action; a command goes out through the same submit path as lanes L and M). Every group is folded at the start. A slider drag updates its row in place; a figure sent a moment ago stays shown until the state catches up. The last row of each list is always a note, so a first answer does not change the list's shape.

## Rows

| Row | Verdict | Where | Proof |
|---|---|---|---|
| G1-24 CW pitch | BUILT | Radio, "CW pitch and peak filter" (slider 300 to 1200 Hz) | s1-signal-radio reads `cwPitchHz`; break-it B red |
| G1-35 independent filter edges | BUILT, NEW ENGINE COMMAND `FOXAPP_OP_SET_PASSBAND` | Radio, "Passband edges and shift" (low cut, high cut in Hz; USB, LSB, CW, AM) | test_pipeline_passband (far tone -10.6 to -47.8 dB), test_engine_api_passband, test_apply_command, drive reads `fact.radio.1.passband`; break-its M1, M3, UI red |
| G1-37 bandwidth presets per mode | BUILT | Radio, "Filter width and shape" (per-mode lists; the engine holds a width at 3 kHz or more, so SSB and CW offer 3, 4, 6 kHz only: narrower ones are the edges row's job) | drive reads `bandwidthHz` for AM, USB; break-it M red |
| G1-39 several notches | BUILT | Audio filters, "Notch bank" (ten slots, frequency, width, on, clear) | drive reads `notchBankMask/Hz/WidthHz`; break-it E red |
| G1-41 audio equaliser | BUILT | Audio filters, "Audio equaliser" (8 sliders, flatten key) | drive reads `eqDb.n`; break-it F red |
| G1-42 audio high-pass, low-pass | BUILT | Audio filters, "Audio high-pass and low-pass" | drive reads both, one keeps the other; break-its G, S red |
| G1-44a CTCSS | BUILT | Radio, "Noise and tone squelch" (tone field, previous / next tone, tone remove) | drive reads `toneKind/toneHz`; break-it D red; a tone not among the 50 is refused on the screen |
| G1-44b DCS | BUILT | same (code field, previous / next, polarity) | drive reads `toneCode/tonePolarity`; break-it Q red |
| G1-45 noise squelch | BUILT | same (on, threshold, tail) | drive reads the three; break-it C red |
| G2-13 S-units or dBm | BUILT | Radio, "Signal meter units" and the Signal strength figure itself (`signal_logic.cpp`) | test_slint_host_signal, drive reads `meterUnits` and the figure's text; break-it A red |
| G1-19 synchronous AM, sideband choice | BUILT, NEW ENGINE COMMAND `FOXAPP_OP_SET_AM_DETECTOR` | Radio, "AM detector" | test_pipeline_sam (lock at 60 Hz; upper keeps the tone, lower puts it 17 dB down), test_engine_api_passband, drive reads `fact.radio.1.amdet`; break-its M2, M4, UI red |
| G1-25 FM width presets | BUILT | Radio, "Filter width and shape", NFM and WFM show the engine's five widths | drive reads `bandwidthHz`; break-it J red |
| G1-33 noise-reduction methods | BUILT | Audio filters, "Noise-reduction method" | drive reads `nrMethod`; break-it H red |
| G1-34 IF shift | BUILT with G1-35 (shift field and two 100 Hz keys) | Radio, "Passband edges and shift" | as G1-35 |
| G1-36 filter shape | BUILT | Radio, "Filter width and shape" | drive reads `filterShape`; break-it N red |
| G1-38 CW peak filter | BUILT | Radio, CW group (on, width, boost) | drive reads the three; break-it P red |
| G1-43a AFC, G1-43b CW auto-tune | BUILT (one switch; its label is "CW auto-tune" in CW) | Radio, "Frequency tracking and multiplex" | drive reads `afcOn`, `afcPullInHz`; break-it K red |
| G1-46a I/Q balance, b DC removal, c swap | BUILT | Source, "I/Q correction" | drive reads `iqCorrectMask`; break-it I red |
| G1-50 volume and pan per channel | ALREADY BUILT (volume per radio: `main.cpp:839`, `:2034`, `radio_strip.slint:223` AudioMixBlock); pan BUILT as "Stereo placement" | Audio filters | drive reads `stereoPlaceMode/Value`; break-it L red. Per DECODED channel volume does not exist in the engine: NOT BUILT, see open rows |
| G1-22 MPX output | BUILT (record key; the engine writes a 192 kHz WAV) | Radio, "Frequency tracking and multiplex" | drive reads `fact.radio.1.mpx` start and stop; break-it O red |
| G1-23 pseudo-stereo | BUILT | Audio filters, "Stereo placement" | drive reads mode 2 and its default |

Every control shows the engine's current value at start-up: `s1-signal-start` seeds a settings file with a figure per setting and reads each control on screen (and the engine) without touching anything (it passes).

## New engine commands
`FOXAPP_OP_SET_PASSBAND` 0x8630 and `FOXAPP_OP_SET_AM_DETECTOR` 0x8631, receiver-scoped, documented in `app_commands.hpp` and `docs/api-0.3-additions.md` 9.49a, facts `fact.radio.<n>.passband` and `.amdet` (and in the facts fingerprint). Not saved: off at every start (like the clarifier). CPU: the passband is a fourth-order Butterworth pair in rotate form at the channel rate; off it returns at once.

## SHARED FILE EDITS
`src/slint/ui/main.slint` (import, export, three mount lines), `src/slint/main.cpp` (include, member, one block, tick, two answer lines, one state name), `src/slint/CMakeLists.txt`, `src/slint/tests/CMakeLists.txt` (one family), `src/slint/signal_logic.cpp` (the signal strength text), `tests/errpath_allow.tsv` (seven UI-text lines, pin not raised), `src/core/pipeline.hpp/.cpp`, `app_commands.hpp/.cpp`, `src/engine/engine.cpp`, `engine_api_lists.cpp`, `receiver.hpp`, `radio_scope.hpp`, `tests/test_apply_command.cpp`, `tests/test_radio_scope.cpp`, `src/slint/tests/test_slint_host_signal.cpp`, 33 catalogues and `lang_assets.hpp`.

## Break-it table (each mutation made, the run seen RED, restored)
Screen group (19 mutations in one build, drive scripts audio, radio, source): meter unit, CW pitch (+50 Hz), noise threshold/tail swapped, CTCSS next goes back, notch slot always 0, EQ band off by one, high-pass always off, NR method always standard, I/Q bit never cleared, FM width 0 sends 1, AFC pull +1, placement mode 2 sends 1, width preset always the first, filter shape always default, MPX stop never sent, CW peak width/boost swapped, DCS polarity always any, tone remove never on, low-pass always off: every one RED with the figures named (for example `iqCorrectMask == 6 -> it is 7`). Engine group: passband process call removed (test_pipeline_passband 3 failed), SAM bypassed (test_pipeline_sam 4 failed), engine stores the low cut as the high cut (test_apply_command 5, test_engine_api_passband failed), AM detector kind 3 sent to the pipeline as 0 (test_apply_command), passband low and high swapped on the screen, AM detector upper/lower confused on the screen (drive RED). All restored and green again.

## New strings
181 keys in all 33 catalogues, MACHINE-MADE by Sonnet sub-agents and UNCHECKED by a speaker (a few kept "S units" as a translated word); `lang_assets.hpp` regenerated.

## Gates (Release, `D:/foxsdr-build/en-screen-j`, hidden desktop `FoxSDRS1`, `FOXSDR_FRAME_CAP_HZ=60`, a machine so loaded that single tests took 100 to 340 s)
* The FULL gate was started (`ctest -R .`, 1558 tests, `-j3`) and STOPPED by exact PID after 13 tests in 7 minutes: at that speed it needed about a day. NOT RUN: the rest of the headless and GUI gates. UNVERIFIED for everything outside the slice below.
* The full tree builds: `cmake --build` of every target, `rc=0` (1100 programs).
* The SLICE that covers the changed code (148 tests: `test_error_paths`, `test_i18n*`, every `test_slint_*host`, forms/fields/cards/constraints/house_rules/main_screen/elided/theme, every `test_pipeline_*`, `test_apply_command` (+ stepped and free), `test_radio_scope`, `test_engine_api_passband|fmwidth|mpx`, `test_passband*`, `test_sam`, `test_if_filter`, `test_config*`, and the drive scripts `s1-*`, `lanem-*`, `lanec-lockout`, `transmit-page`, `signal-*`, `tools-airband`, `main-*`, `gaps-deemph`): `99% tests passed, 2 tests failed out of 148`, `Total Test time (real) = 1119.76 sec`. The two reds were MINE and are fixed: `test_slint_house_rules` (`s1_signal.slint` did not import the Theme: import added, green) and `slint_drive_transmit-page` (the SIGNAL list is taller: the Audio filters header starts below the window; see the changed expectation below; green after). The s1 drive scripts and `test_slint_house_rules` were re-run after the fix: all green. The stamp tests were not re-run after the last rebuild.
* Error-path gate: PENDING 1126 before and after (the pin is not raised); seven allow lines for typed-text refusals.

## Changed expectations (the one existing script touched)
`src/slint/tests/drive/transmit-page.txt`: the step `scroll "Audio filters" 400` found the header while it was inside the window; the Radio and Audio filters sections now hold folded groups, so the header starts lower and is outside the window (a control outside the window is not in the element tree). The script now wheels the list from "Signal strength" by 300 first and waits 6 ticks, and the closing click wheels from "Audio filters" again. No assertion changed; the Transmit row is still reached by the list's scroll (owner answer 76: it is not forced into view).

## UNVERIFIED / OPEN ROWS FOR THE NEXT LANE
| Row | What | Kind of lane |
|---|---|---|
| G1-50 per decoded channel | volume per decoded CHANNEL (not per radio) has no engine command | engine lane |
| Passband / AM detector memory | neither is saved with the radio (off at every start) | engine lane (config field and restore) |
| Width presets below 3 kHz | the engine holds SET_BANDWIDTH at 3 kHz or more; narrower SSB or CW widths are reachable only through the edges | engine lane if wanted |
| Real pointer, real radio | the new rows are driven by label, never by a real pointer; the clip sweep was not run | mouse drive |
| Translations | machine-made, unchecked | owner |
| Edge sharpness | 24 dB an octave per edge (fourth-order Butterworth); the demodulator's own filter still shapes SSB | engine lane if steeper wanted |

Config hash re-proved at the end: c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3 (unchanged).
