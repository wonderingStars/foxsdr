# Drive fixes, group 5 (2026-11-18)

Branch `fix/drive-5`, cut from `3276b1bb` (drive fixes 4), worktree `D:/foxsdr-wt/en-screen-j`, build `D:/foxsdr-build/en-screen-j` (Release, `--parallel 3`), hidden desktop `FoxSDRDriveFix5`, scratch `D:/foxsdr-build/df5-scratch/`. Nothing pushed. Source: drive handovers 15 to 26, `plugins-drive-1.md`.

| # | Reproduced | Cause (file) | Verdict | RED before |
|---|---|---|---|---|
| 1 waking every decoder | by reading + test | `kit_logic.cpp pluginUseText` sent bit 8 (Decoder output page open) for every text decoder; `engine_plugin_run.cpp 403` woke every AUTO decoder for it. All four actions (Show decoder output, "+ Window", START on one text plugin whose preset opens the page, STOP ALL landing on a preset) end in `open.decoderOutput` | FIXED in the window: the bit is no longer sent. The Legacy window and the engine's handling of bit 8 are unchanged | `test_slint_host_kit` 2 checks red (aprs "8" / pocsag "9"; page-only report not empty) |
| 2 odd-rate file | by reading + test | 1083333 = 3 x 361111: no integer channel in 150..300 kHz, `Pipeline::setInputRateHz` refuses (a pinned contract, `test_rate_follow` 2000001, `test_pipeline_device_rate_audio` 900001) and stays at 2 MS/s; the raw band the plugins get is the source's own blocks, but `rebuildRunnerOf` and the host's `sampleRateHz` told them the chain's rate | FIXED: new `Pipeline::rawBandRateHz()` (source rate when the chain refused; chain rate otherwise) used by both. The Source panel still says the chain refused (audio and the spectrum's chain are unchanged) | `test_rate_follow` leg 8b; break-it (return the chain rate) RED `q.rawBandRateHz() == 1083333.0` |
| 3 kept offset | yes | `installOpenedIqFile` kept the previous source's VFO offset (a generator's +300 kHz on a 48 kS/s file); the typed dial then put the source centre outside the band | FIXED: `Engine::keepVfoInsideSpan` after `followInputRate`; outside the span the offset goes to 0 (dial = centre), inside it is kept | `test_engine_api_wire_a_files` F5b: RED (offset stayed 300 kHz), GREEN |
| 4 VOR 086 | by measurement (test) | the 2 Hz AM DC blocker leads 30 Hz by 3.81 degrees | FIXED without touching the listener path: `dsp::AmPhaseTrim` (inverse of the blocker above its corner) applied only to the decoder tap's copy in AM (`pipeline.cpp`). Effect on AM audio: none (speakers take the demodulator output as before). Other AM decoders see the same trim: +0.4 % at 30 Hz, 0.01 % at 1 kHz | `test_demod`: six radials, raw 3.81 degrees, trimmed 0.03 |
| 5 number field | yes (test) | the focus change's select-all runs after the press, and every pointer move while the button is down moves the caret again (anchor left at 0): a hand's one pixel emptied the selection. Group 4's test pressed, paused and released without moving | FIXED: while a number field has no keyboard a TouchArea layer takes the whole click and focuses and selects all on RELEASE; focused, the layer is off (second click places the caret). Seven copies (fields, extend, patch, signal, system, tools, transmit) | `test_slint_fields` 3 variants RED with the layer disabled ('77', '7877', '787877') |
| 6 required input | by reading | ABI declares only `requiredRateHz`; the runner already says "needs N Hz raw I/Q and the receiver is producing M Hz" (`plugin_runner.cpp rateSentence`), shown on the plugin's page (seen in part 23). There is no field for a required mode, so the application cannot say "this needs USB" | NOT DONE (nothing to build: no declaration to read). FOR THE PLUGINS LANE: SYNOP and ALE want a preset with KEEP_TUNED_ON_START, USB, 3000 Hz; C-QUAM stereo one with RAW; HF ARQ USB 3000 Hz; FMS and CLIP NFM 12.5 kHz, no frequency is verified in this repository so none is invented | |
| 7 words and small faults | see below | | | |
| 8 Legacy | see below | | | |

## Item 7 in detail
* Measurement results labels: `lanel_logic.cpp humaniseKey` now speaks the engine's short names (obw, xdb, snr, thd, thdn, sinad, sfdr, enob, fm, am, crc, ccdf, rms, dbfs, dbc, db, [n] indexes). FIXED, `test_slint_lanel_host` RED 2 checks before. The frame log is said in words (held, held with errors, good seen, errors seen, keeps errors) instead of raw JSON (`lanel_publish.cpp`).
* "later": the one welcome-sheet sentence uses it as an ordinary word. `test_slint_host_mustfixa` now refuses "later" in anything the window shows except three listed ordinary sentences (welcome, macro, "later end than start").
* Alarm on with no mask: the section note says "Alarm on: Load a mask first." at once (drive `lanel-measure` line added), not only after Apply.
* Decoder output page showed only chips: one bar per fitted decoder with presets filled the page (61 of them); bars now sit in a 132 px scrolling box above the log. `test_slint_main_screen` (61 bars, the log's ground must be in the lower page).
* Wheel over a slider: now only with the keyboard on it, or after the pointer rested 600 ms and 450 ms after the last notch it declined. The Legacy window's (ImGui) sliders take no wheel at all. `test_slint_sliders` (specimen C now reports changes); the old wheel checks drive B and F, which hold the keyboard.
* EPIRB / GOES preset on an I/Q file: `applyPluginPreset` says "This is an I/Q file: it cannot be retuned to %.4f MHz for %s. Open a recording made there." when the preset did not land (`tune_control.hpp fileTuneMissed`, `test_tune_control`). Whether the file's centre label moves for such a preset was NOT reproduced here (no plugin harness): the sentence is decided on the outcome, so it is silent when the retune did land. UNVERIFIED in the real application.
* Packet TNC "RESTART SERVER": it exists as a plate command the plugin registers (`tnc_engine.cpp:526`, with DROP TX QUEUE), so it is a key on the plugin's key plate under Fitted modules (where the drive found the other plate keys), not on the KISS page. Not a fault; nothing built.

## Item 8
* LG7 About sheet: the released 0.99.104 Legacy window has no About string at all (`git grep` of `v0.99.104:src/gui`); nothing went missing. The new window's App menu has "About FoxSDR...". NOT A REGRESSION; not built.
* S34 handover question: by design the question is asked only when a recording take or a patch runs (`handover_host.hpp choose()`, `window_handover.hpp Busy`), not for an I/Q FILE playing; the drive's row meant a record take. PLAN WRONG; playback carries on across the switch.
* LG4 "AM stereo": that is the C-QUAM plugin's own text (`cquam_plugin.cpp`), kept running by the drive's profile seed; not the mode word. PLAN WRONG.

## Changed expectations
| Test | Old | New |
|---|---|---|
| `test_slint_host_kit` use table | aprs "8" and pocsag "9" | pocsag "1" only |
| `test_slint_forms` idle tooltip | "...when you open its window or the Decoder output window. START keeps..." | "...when you open its window. START keeps..." |

## Strings
Twelve new, 33 catalogues each (machine-made by three Sonnet agents, unchecked by a speaker; ja and ko reworded to keep `%.4f` before `%s`): occupied bandwidth, bandwidth at X dB, Level (dB), five frame-log words, the I/Q-file preset sentence, and three sentences changed to stop saying the Decoder output window starts decoders (the Idle sentence, the Idle tooltip, the Keep running tooltip). 4904 keys x 33, 0 missing, 0 stale. Error-path pin 410, not touched.

## OPEN ROWS
Real-pointer proof of items 5, 7 (slider wheel, Decoder output page, alarm) and 1 (no wake); the Legacy window still wakes every decoder on its Decoder output window (not changed: Legacy is outside item 1's four actions); item 9 on a real file; item 2 on GSM broadcast in the application.
