# Radio sources, stand-alone parts (branch feat/radio-sources)

Date 2026-11-11. Worktree foxsdr-wt/feat-src, branch feat/radio-sources cut from engine/multi-radio b9208fba, build D:/foxsdr-build/feat-src (`-DCASCADE_BUILD_SLINT=OFF -DCASCADE_SOFTGL=OFF`). Nothing pushed or merged. Every part is a stand-alone class with tests; NOTHING is registered with the engine, the Source menu or the settings: that is the wiring lane. Survey sources: `D:/foxsdr-build/prep/blocked-recheck-other.md` (R4, R7, R9, R13, R17, W4a, W4c) and `blocked-recheck-spec-a.md` (R3).

Clean-room rule kept: no GPL/LGPL/AGPL source or port was read; no software, SDK or archive was downloaded; no outside server or receiver was contacted; the radios on the desk were not opened.

## Part 1: R13 SDRplay low-IF choice, W4a diagnostic report model, W4c service restart over an injected SCM

Files: `src/source/sdrplay_if_mode.{hpp,cpp}`, `sdrplay_diag_report.{hpp,cpp}`, `sdrplay_scm_restart.{hpp,cpp}`, test `tests/test_sdrplay_ifmode_diag.cpp`.

**What existed (verified in this tree).** `sdrPlayRatePlan()` (sdrplay_source.cpp) is zero-IF only since 0.99.44: two RSP2 field logs showed a low-IF 6 MHz plan delivering 6 MS/s where 2 MS/s was expected (the log lines are quoted there). The SDRplay probe (sdrplay_probe.cpp, stepIfModes) already holds the low-IF combinations transcribed from the API specification section 3.15. The tree also already has an elevated cmd.exe service restart (`SdrPlayServiceRestart`) and a probe report; W4a and W4c here are the two things asked for that it does not have: a plain report model + formatter, and a direct SCM restart with an injected interface.

**What was built.**
* `SdrPlayIfMode {ZeroIf, LowIf}`, `sdrPlayRatePlanForMode(rate, mode, plan)`: ZeroIf forwards to `sdrPlayRatePlan` unchanged (the default; `sdrplay_source.cpp` is untouched). LowIf offers 62.5k, 125k, 250k (fs 2 MHz, IF 450, BW 0.2), 500k, 1M, 2M (fs 2 MHz, IF 450, BW 0.6), 3M (fs 6 MHz, IF 1620, BW 1.536), 4M (fs 8 MHz, IF 2048, BW 1.536), 8M (fs 8 MHz, IF 2048, BW 5.0); every other rate is refused, never guessed. Output = fs / decimation.
* `sdrPlayLowIfRateVerdict(plan, measuredHz)`: classifies a measured stream rate as AsPlanned, DeliversAdcRate (the 0.99.43 field behaviour: within 2% of fsHz while decM != 1) or Other. The wiring lane must run it on the first real low-IF stream and refuse low-IF on a mismatch.
* `SdrPlayDiagReport` + `formatSdrPlayDiagReport`: API version/path, service summary, device list (model from hwVer, serial masked to the last four characters, tuner, RSPduo mode, in-use flag), last error and step.
* `restartSdrPlayServiceVia(ScmInterface&, name, limits)`: query, stop if not stopped, wait for Stopped, start, wait for Running; results Restarted, AccessDenied (Windows error 5, sentence says administrator rights), NotInstalled (1060), StopTimedOut, StartTimedOut, OtherError(number). `WindowsScm` wraps OpenSCManagerW / OpenServiceW / ControlService / StartServiceW / QueryServiceStatusEx (CONNECT, QUERY_STATUS, START, STOP rights only). `restartSdrPlayServiceNow()` is called by nothing and by no test.

**UNPROVEN / not verified.** The SDRplay API specification PDF was not reachable in this lane: the low-IF combination table is the project's earlier transcription, not re-read. Whether low-IF takes effect on a real RSP, and what rate it delivers, is unproven (no RSP). The real SCM wrapper compiled but was never run; the service was never touched.

HAND-OFF (wiring lane): add a persisted setting `sdrplayIfMode` ("zero-IF" default | "low-IF"); in SdrPlaySource replace the `sdrPlayRatePlan` calls with `sdrPlayRatePlanForMode(rate, mode, plan)` and use `sdrPlaySupportedRatesForMode` for the rate list; after start in low-IF measure the delivered rate and call `sdrPlayLowIfRateVerdict`, falling back to zero-IF on a non-AsPlanned verdict. Threads started: none. Strings: the two mode names, the three verdict sentences, the `SdrPlayScmOutcome::sentence` texts, the diagnostic report headings.

Tests: see Results. Break-it: see Results.
