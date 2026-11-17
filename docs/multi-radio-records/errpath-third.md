# Error-path third reading: the ALLOW lines the second review doubted (2026-11-17)

Branch `feat/errpath-third`, cut from `4229dbd3` (the tip of `engine/multi-radio` after merge 10), worktree `D:/foxsdr-wt/en-tuning`, build
folder `D:/foxsdr-build/en-tuning`. Nothing was pushed. No radio, sound device or window was opened (stand-ins only). The real
`%APPDATA%/foxsdr/config.json` still has sha256 `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` (checked at the last commit).
The transmit file `src/source/pluto_tx.cpp` and anything that keys a radio were not edited.

## The rule and why this reading

An ALLOW line says "this failure site need not report". A wrong one is a failure that reaches nobody. Review 2 estimated 12 to 16 wrong
lines among the 478 of sweep A. This reading follows the call chain of the lines it doubted and the reason classes it found weakest.

## The set, and how it was chosen

Not a re-read of the 478. Chosen in this order:

| Part | Lines | Why in the set |
|---|---|---|
| Open-path family (every `open()` failure of a radio) | 168 | the reviews' largest cluster; the fallback fault and the dropped-open fault were found here |
| Lines review 2 named (glfw x8, `closeDevice` keep x9, `Txn::stage` x16, `parseHelperFields`/catalogue parse x21, saved-radio x2) | 56 | named as doubted or inconsistent |
| Remaining sweep-A lines outside the store families | 113 | read one by one from the allow list; the reporter each names was grepped |
| "UI result text" (129 seed) and "information" classes | 134 | the weakest reason classes; each classified by whether its site PRODUCES a failure or copies one produced (and gated) elsewhere; the producers were read in code |
| Server-start "validation" (35) and the CAT link lines (17) | 52 | looked at because "validation" covers real faults there |
| Total looked at | about 520 | |

Depth, stated plainly: the 168 open-path lines and the store families were proved by enumerating every caller (grep over `src/`) and reading
the reporting call at the end of each; about 95 other lines were read in code, function by function; the rest were judged from their reason
text plus a grep that the reporter the reason names exists and encloses the path (line numbers in older reasons had drifted; the substance was
re-checked, and the reasons that quoted old line numbers were rewritten).

## Verdicts

| Verdict | Lines | Notes |
|---|---|---|
| ALLOW STANDS (reporter named and found, or shown not to be a failure) | about 495 | all families above except the rows below |
| MUST REPORT, fixed | 5 faults, 10 lines | below |
| CANNOT TELL, left ALLOW and listed under OPEN ROWS (the pin may not rise) | 14 lines | below |

### ALLOW STANDS, the ones that were doubted

* **Open path (168).** Every caller of a radio's `open()` files ONE radio_fail through `Engine::reportFailedOpen`
  (`engine_radio_reports.cpp:117`): the async worker's result in `finishDeviceOpen` (`engine.cpp:1772`; an open the person moved on from,
  `:1726`, `superseded`; a busy saved-radio retry skips only the anonymous count, `:1766`), the saved radio at start-up (`engine.cpp:10453`,
  `engine_radio_saved.cpp:219`), a patch-page radio (`engine_patch_radios.cpp:334`, start `:377`), a sound card (`engine_soundcard.cpp:203`).
  The two defects review 2 named are already fixed on this tip: the native-to-SoapySDR route fallback (worker `engine.cpp:2066-2086` stores the
  native failure in `g_fellBack`, `finishDeviceOpen :1703` files it; the saved-radio route files it at `:10375`) and the open dropped by
  `asyncOpenStillWanted` (`:1726`). The developer tools (`tool_modes.cpp:241`, `:433`, `rtlsdr_check.cpp:74`) print to their console, which is
  their whole output. The 164 reasons quoted stale line numbers and a retired `noteRadioFail` route; they now cite the routes above.
* **glfw x8.** `glfwInit`, `glfwCreateWindow`, the two ImGui backends, the old-driver check and `tellUserWindowFailed` all call
  `breadcrumb::noteWindowFailed()` before `return 1`, and the sentinel files it as signature `:window-failed` (`sentinel.cpp:398-413`; the
  existing sentinel tests cover the flag). Review 2's inconsistency is settled: all are ALLOW by that one path.
* **`closeDevice` keep x9.** They re-set the error the open path or the run time set; the original is a separately gated site (open: reported
  above). Copies, never new faults. Reason corrected.
* **`Txn::stage` x16, catalogue parse x21.** `install()` counts `notePluginInstallFailed` for the official catalogue (`plugin_repo.cpp:3145`).
  The parse is reached by `fetchIndex` (counts the catalogue failure), by the kept copy (counts `Recovered::CatalogueBad`,
  `engine_store.cpp:144`, `:192`) and by `fetchSiteIndex`. **Site-sourced entries are not counted: by the 0.99.74 rule written in
  `install()`'s own comment ("the counter is the official catalogue's, and a failure on someone else's server says nothing about it").** I
  could not find the owner's decision for it; it is an owner question below.
* **Information, 9 lines.** A rate the driver coerced on a call that succeeded (`applyPluginPreset`, `applyRetuneNow`, the sound card's nearest
  rate), a successful reopen after a counted fault, ring drops (`closeDspMinute` files the `ringdrop` report at `engine.cpp:1062`, behind its
  severity floor, and the stream journal), 'saved' trace lines. Not failures.
* **UI result text, 129 + 4 producers.** The text copied into a page (status, install error, picture, update error) is produced at a site that
  is itself gated; the one-click driver installer's two `GetLastError` lines are reported by `winusb::noteInstallOutcome`
  (`app_window_radio_setup.cpp:303`, `signal_publish.cpp:246`); `shell_open` rethrows; `crashUploadFinish` is the reporting machinery itself;
  user-file exports and imports are shown to the person who asked.

### MUST REPORT, fixed (each through a helper that already exists; no new message kind)

| # | Fault that reached nobody | Where | Report | Test |
|---|---|---|---|---|
| 1 | A rate the radio refuses, wherever `applySourceRate` asks (open worker, saved radio, plugin preset, API, patch page). Drivers never wired for refusals (SDRconnect, SpyServer) were silent at open. | `core/rate_follow_status.hpp` | a `RefusalWatch` inside `applySourceRate`: `noteRefusalIfSilent` (once, never twice when the driver said it) | `test_errpath_third` 1 |
| 2 | The patch page's radio that refuses a frequency or a frequency correction (main thread, and the open worker's correction call). Before: the node's text only. | `core/patch_radio.cpp` `setCentreHz`, `setRadioPpm`; `engine_patch_radios.cpp` worker | the same watch | `test_errpath_third` 2 |
| 3 | The signal tone's sound output that would not open or died while it played (Windows waveOut, Linux ALSA). | `slint/lanek_audio.cpp` records `lastFailure()`; `slint/lanek_publish.cpp` `countToneFailure` at both observation points | `health::noteSoundFail` (host API and one of five reasons, never the text) | Linux branch of `test_slint_host_toneout` (not run here) |
| 4 | A saved patch that lost connections on loading, in the NEW window (the old window counted it; the new page logged it). | `slint/patch_logic.cpp` via `PatchConfig::patchLostConnections`, `slint/patch_publish.cpp` | `Recovered::PatchLoad` | `test_slint_host_patch` `aSavedPatchThatLostConnectionsIsCounted` |
| 5 | The SDRplay low-IF fall-back to zero-IF (the person switched it on, the radio's first stream did not carry the plan). The old line said "OPEN ROW: send the fallback as an issue report". | `source/sdrplay_source.cpp` `lowIfFallBack` | `noteControlRefused` (control `other`) | `test_sdrplay_lowif` C2b |

The ALLOW lines of faults 3 and 4 and the engine's patch lines stay, with a reason naming the new route, because the reporting call is in a callee
and the gate reads one block; fault 5's line is removed (the reporter is in the same block). The PENDING pin is unchanged because no PENDING line was
touched.

Every new event carries no frequency, file name, path, position, decoded text, user, host or address: the reason goes through the reporter's own
scrub. `test_radio_reports_never_sent` now runs a hostile sentence (frequency, path, address, host, MAC, serial) through the rate route and
the patch radio's two routes and asserts the new events are on the wire; break-it (the scrub in `scrubText` made a pass-through) turns it red with
LEAK lines and `test_errpath_third` red; restored, green.

## CANNOT TELL: OPEN ROWS (ALLOW lines left unchanged; the pin may not rise)

| Row | Lines | What it needs |
|---|---|---|
| A software-renderer second process that could not be started (`software_gl.cpp` `relaunchAndWait` x2 `GetLastError`, `tellUserRelaunchFailed` x2; `main.cpp:184` ends with the sentinel already disabled for this process, so nothing files it) | 4 | a window-lane reporter: the sentinel is off at that point, and no existing word fits (a new word is not allowed here) |
| A CJK font that would not register (`slint/cjk_register.hpp`, stderr only; CJK text then draws blank) | 1 | a word for "a drawing resource did not load"; none exists |
| The engine's graph text that could not be read back by the patch page (`patch_logic.cpp` `readLook`) | 1 | an internal-inconsistency word |
| Server start failures filed as "validation" by lane s4b: MCP "could not listen on that port", API and rtl_tcp "could not initialise the network stack", the rotator's serial port and rotctld address, the listening log "could not be read / damaged" | 8 | an outputs word (the O-outputs group is PENDING for the same reason) |
| The Pluto transmit open (`engine.cpp` diagWarnf only) | verdict only | the transmit owner (not edited) |

## Owner questions

1. A plugin installed from a **third-party site** that fails to install or whose index cannot be read is not reported (by design, `install()`'s
   comment, 0.99.74). Is that the owner's decision? If every handled fault is to be reported, a site failure needs its own word.
2. The CAT rig link (17 lines, lane mustfix-b and 2026-11-16) is filed as "an accessory the person connects on purpose, not a radio": its failure
   is shown on the page and nowhere else. A rig on a serial port is arguably an "output"; decide whether it reports.

## Tests (real results, foreground, this tree)

* `test_error_paths` passed (pin 410, the gate also built over the edited sources); `errpath_gate_selftest` passed.
* `test_errpath_third` 24 checks, 0 failed (new). Break-it (both watches disabled): 4 checks red; restored: 0 failed.
* `test_radio_reports_never_sent` passed with the new block; break-it above.
* `test_sdrplay_lowif` passed on 5 of 6 consecutive runs; the 6th failed in the EXISTING case C1 (`lowIfChosen` at line 239, a first-stream pace
  check) under load, as it did once before the report was added; C2b (the new case) is written so that a loaded machine's fall-back is a report
  (one report exactly when it fell back). Break-it (the `noteControlRefused` call removed): C2b red; restored, green.
* The touched suites, run together (`-j2`), all green except the GUI-labelled ones that were not built or refuse the visible desktop:
  `test_rate_coercion`, `test_rate_follow`, `test_engine_shell_guard` (35 s alone), `test_patch_*` (headless: iqfile, radio, runner, graph, io, ...),
  `test_radio_*`, `test_mustfix_b`, `test_errpath_sweep_a`, `test_health_paths`, `test_sdrplay_*`, `test_slint_host_patch`,
  `test_slint_host_patchfaces`, `test_slint_host_toneout`, `test_slint_patch`, `test_slint_host_keydispatch`. Three tests (`test_patch_iqfile`,
  `test_patch_radio`, `test_patch_runner`) failed in the combined run and passed alone (1.5, 1.7, 0.8 s); the cause was not established (another build in the
  same folder was running at the time).
* `foxsdr-slint` (the new window) built, which compiles `lanek_audio.cpp`, `lanek_publish.cpp`, `patch_publish.cpp`, `patch_logic.cpp`.

### Expectations changed

* `tests/test_health_paths.cpp`: the table of where each `Recovered` word is counted gains `{"PatchLoad", "src/slint/patch_publish.cpp", 1}`;
  the existing row `{"PatchLoad", "src/gui/app_window.cpp", 1}` is unchanged (the test failed with "which the table does not list for it" until
  the new site was listed: the count of files for the word is 1 before, 2 after).
* `tests/errpath_allow.tsv`: reasons rewritten for 164 open-path, 9 keep, 21 parse, 2 saved-radio, 5 patch/refusal lines and 1 low-IF caller line
  (verdicts unchanged); 1 line removed (the low-IF fall-back warn, now reporting); the four tone lines were removed and put back with a new reason.
  No line was added, none re-sorted, the pin is 410 before and 410 after (no PENDING line touched).
* `tests/test_sdrplay_lowif.cpp` C1 is unchanged.

## NOT RUN

The full headless gate, the GUI gate, the stamp tests, the Slint drive scripts, the Linux ALSA test, the GUI-labelled patch tests
(`test_patch_canvas_nav`, `test_patch_info_pane`, `test_patch_map_credit` and the `_free`/`_stepped` variants) and `gui_desktop_guard` (which
correctly refuses the visible desktop).

## UNVERIFIED

* `ToneOut` on Windows (waveOut) was never opened by a test, and the Linux `lastFailure()` checks were not run on this machine; the window's
  `countToneFailure` and `patch_publish.cpp`'s `patchLostConnections` override compile in `foxsdr-slint` but no window was driven.
* `noteSoundFail` for the tone mapping (waveOut result codes to reasons) is written from the Windows documentation, not exercised.
* The 95 lines read in code are proved; the rest of the set rests on the reason text plus a grep for the named reporter.
