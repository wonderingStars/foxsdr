# The error-path gate (0.99.109 campaign, wave 7)

Branch `feat/errpath-gate`, cut from `e0d0c669`. New files plus the registration lines in `tests/CMakeLists.txt`; no product
source file is edited. Nothing was pushed. The real `%APPDATA%/foxsdr/config.json` still has sha256
`c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3`.

The owner's rule: no radio, sound device, plugin, update or output may fail in any install without him being told. The gate is a
test in the normal run that knows every place the code notices a failure, and goes red when someone adds one that nothing reports
and nobody has written down why it need not be.

## What a site is

`tests/errpath_scan.hpp` reads `src/` as text (comments and string contents blanked, raw strings handled, preprocessor lines
ignored for brace tracking). Tests (`test_*` files, any `tests`/`test`/`probe` folder) and vendored code (`third_party`) are not
scanned. Sites by kind:

| Kind | Pattern | Example |
|---|---|---|
| `warn` | a call of `diagWarnf(` | `diagWarnf("patch: radio '%s' did not stop within %d ms ...")` |
| `log` | a call of `diagLogf(` whose first literal carries failure wording (could not, failed, refused, did not, timed out, lost, abandon, fault, error, unable, cannot, dropped, stalled, dead, falling back, upper-case NOT) | `diagLogf("the radio failed to start")` |
| `seterr` | a call of `setError(` or `setErrorOn(` (declarations and definitions excluded) | `setError("bad host"); return false;` |
| `errassign` | an assignment `=` or `+=` (with text) to an error field: `error`, `err`, `errMsg`, `errorText`, `lastErr`, `lastError`, `faultMsg`, `win32Error`, anything `*Error`, each with an optional trailing `_` and an optional `[index]`, used inside a function body; not a clear (`""`, `{}`, `nullptr`, `false`, `true`, `0`, `'\0'`, a brace list), not a constant (`kXxx`), not a Hungarian Win32 field (`hStdError`, `fAbortOnError`) | `lastError_ = "the device went away";` |
| `swallow` | a `catch (...)` / `catch (T&)` whose body, once comments, `diagLogf`/`diagWarnf` statements and bare `return;` / `continue;` are removed, is empty | `catch (...) { }` |
| `ignored` | a bare statement call, or a `(void)` cast, of a vendor-library call: `Pa_*`, `libusb_*`, `hackrf_*`, `airspy_*`, `airspyhf_*`, `rtlsdr_*`, `sdrplay_api_*`, `iio_*`, `FT_*`, `WinUsb_*` (also through `a.`, `a->`, `::`) | `Pa_CloseStream(stream);` |
| `stderr` | a statement that writes to `stderr` (`fprintf`, `fputs`, `fwrite`, `fputc`, `vfprintf`) or `std::cerr <<` | `fprintf(stderr, "cascade: ...")` |
| `plwarn` (profile `plugins` only) | `CASCADE_LOG_WARN` / `CASCADE_LOG_ERROR` handed to the host log | `h->log(ctx, CASCADE_LOG_WARN, err)` |

## The three ways to pass

1. **A reporter.** A call of a name in `tests/errpath_reporters.txt` (`noteRecovered`, `raiseRecovered`, `noteRadioFail`,
   `noteSoundFail`, `noteUpdateCheckFailed`, `noteUpdateDownloadFailed`, `noteUpdateVerifyFailed`, `noteUpdateRunFailed`,
   `notePluginCatalogueFailed`, `notePluginInstallFailed`, `notePluginLoadRefused`, `noteRecordFailed`, `fileIssueReport`)
   whose own innermost brace block ENCLOSES the site (the site's own block, or any block around it up to the function). A reporter
   in a sibling branch, or in a lambda the site is not in, does not count. A `swallow` site can never pass this way (its body is
   empty by definition). The test checks every name in the file is declared in `core/health_events.hpp` or `core/issue_report.hpp`.
2. **An allow line** in `tests/errpath_allow.tsv`: `key<TAB>ALLOW<TAB>reason<TAB>added`; the reason is a code (`info`, `user-action`,
   `test-seam`, `ui-text`, `protocol-parse`, `teardown`, `validation`, `reporter-itself`) or at least 12 characters. A
   `DETAIL-OF:<key>` verdict is accepted for a site whose failure is reported one level up; its target must exist and itself be
   reported or PENDING.
3. **`PENDING:<group>`**: a known unreported site that a later lane fixes. The count is pinned (`kPendingPin` in
   `tests/test_error_paths.cpp`) and the test fails when it rises, when it falls without the pin being lowered, and when a PENDING
   line's site now reports (delete the line, lower the pin).

A site that passes none of them fails with: the file, the function, the kind, the text, one sentence of what the author does,
and the exact line to paste. A line whose site is gone fails ("STALE ALLOW ENTRY"), with a move hint when the same function,
kind and text now exists under another file. Malformed lines, a duplicate key, a bad reason and an unknown verdict fail too.

## The key

`file::function::kind::stem`, no line number. `function` is the qualified enclosing function (class names kept, namespaces
dropped; `<file>` and `<scope X>` for code outside any function). `stem` is the first string literal of the site, lower-cased,
digits to `#`, printf conversions to `%`, punctuation dropped, cut at 60 characters; with no literal, the identifier skeleton. The
second and later identical keys in one file get `#2`, `#3` in file order. Moving code inside a file changes nothing; moving a
function to another file makes one NEW site and one STALE line, still failing, with the hint.

## Counts at e0d0c669 (this tip's seed)

3242 sites in 1220 files, scanned in about 1.5 s (MSVC Release):

| Kind | Sites | | Outcome | Sites |
|---|---|---|---|---|
| errassign | 1898 | | pass by a reporter | 69 (warn 43, errassign 14, stderr 5, log 4, seterr 3) |
| seterr | 779 | | allowed (ALLOW) | 2049 |
| warn | 271 | | PENDING | 1124 |
| stderr | 167 | | | |
| log | 75 | | | |
| swallow | 46 | | | |
| ignored | 6 | | | |

PENDING by group (the pin is **1124**): reports-not-in-function 610, G3-control-refused 138, unsure 88, O-outputs 55,
G2-stream-health 29, R-record 27, G1-device-dead 26, G10-gpu-fft 16, D-gps-serial 13, F-files 11, T-tx 11, S2-window 10,
G9-vendor-fault 10, unmatched 9, S1b-headless 7, A-audio 7, P-plugin 5, S5-driver-install 5, P-store 5, S3-plugin-ui 5,
S8-sdrplay-service 5, G8-sdrplay-lifecycle 5, S6-scan 4, G7-patch-radio 4, S9-report-upload 4, G6-route-fallback 3, S7-ui-engine 3,
S1-startup 3, G5-rtl-pll 3, S4-first-run-check 2, PL2-companion 1. (The survey's MUST REPORT groups are the survey's own names.)

## Where the scanner and the survey disagree (the CODE decides)

| Kind | Survey | Scanner | Why |
|---|---|---|---|
| `warn`, `seterr`, `stderr` | 271 / 779 / 167 | 271 / 779 / 167 | identical |
| `log` | 438 | 75 | the survey's own design lists only `diagLogf` with failure wording; 363 survey rows have none (318 ALLOW, 33 REPORTS, 10 MUST REPORT, 2 UNSURE). The 10 MUST REPORT rows are in the table below |
| `errassign` | 1994 | 1898 | 96 survey rows are not failures noticed: 41 clears (`= ""`, `{}`, `nullptr`), Win32 fields (`hStdError`, `fAbortOnError`), `kXxx` enum constants, class-scope default members, `bestErr`-style measurements; 0 sites the survey lacked |
| `swallow` | 43 | 46 | 6 survey rows are `catch (_) {}` inside a JavaScript string in `web_server.cpp` (not C++: correctly not sites); 9 scanner sites are `catch (...) { return; }` / `{ continue; }`, which the survey's own definition includes (bare return/continue removed) but its count did not |
| `ignored` | 9 | 6 | `Pa_OpenStream` (assigned, value used), `Pa_GetErrorText` (a text lookup) and `(void)boot.controlOut(...)` (a project call, not a vendor prefix) are not recognised |
| survey `REPORTS` | 696 rows | 69 pass | the survey meant "reaches a counter somewhere"; the gate wants a reporter in an enclosing block. 610 sites the survey called REPORTS have none in their function and are `PENDING:reports-not-in-function` (mostly open-path `setError` that reaches `noteRadioFail` in the caller, and plugin-report wiring in another function) |
| scanner reports, survey disagrees | | 20 | the scanner sees a reporter: survey said ALLOW 13, MUST REPORT 5, UNSURE 2 |
| scanner sites with no survey row | | 9 | all `swallow` (bare return/continue handlers): `PENDING:unmatched` |

Plugins (`8ab7f16`, `src/` without `third_party/` and `test/` folders): scanner 953 sites (errassign 522, swallow 386, plwarn 39,
stderr 6) against the survey's 999 (570, 386, 39, 4). The survey's 570 errassign rows included 48 the scanner does not count
(clears, vendored code, arithmetic) and 1 stderr row; the scanner finds 3 stderr statements in `sstv_decode.h` debug blocks that have no survey row (`PENDING:unmatched`).

## Self-test (`errpath_gate_selftest`, real output)

```
$ ctest -C Release -R "test_error_paths|errpath_gate_selftest"
1/2 Test #1133: test_error_paths .................   Passed    1.45 sec
2/2 Test #1134: errpath_gate_selftest ............   Passed    0.02 sec
100% tests passed out of 2
errpath_gate_selftest: 42 checks, 0 failed
```

Fixtures under `tests/errpath_fixtures/<case>/` (src/ plus allow.tsv, scanned as text). Each expectation is its own check with a
plain message: bare warning RED (names file, function, kind, text, action, key); the same with `noteRecovered` GREEN; with an allow
line GREEN; a stale allow entry RED; a new `lastError_` assignment RED; an empty `catch (...)` RED; a function moved to another
file RED with the "moved" hint; 40 added lines above a function with the same allow list GREEN (and the line number differs, the
key does not); a reporter in a sibling branch RED, in an enclosing block GREEN; comments / strings / raw strings / informational
`diagLogf` / clears / comparisons / class default members / handled catches / assigned `Pa_` results are not sites; four dropped
vendor results found and the assigned one not; `Class::function`, constructor with a brace initialiser, out-of-line names; `#2`
for identical sites; PENDING pin rising RED, falling RED, a PENDING site that now reports RED; bad reason, dangling `DETAIL-OF`,
duplicate key RED; `test_*.cpp` and `/tests/` not scanned.

## Break-it on the real tree (real runs, MSVC Release build)

1. A temporary `src/core/zz_errpath_breakit.cpp` with a bare `diagWarnf("radio %d would not open", id)`:
   `FAIL UNREPORTED ERROR PATH: src/core/zz_errpath_breakit.cpp, function zzBreakIt, kind warn: ...` with the key to paste,
   `test_error_paths: 22 checks, 1 failed`, exit 1. File removed: `22 checks, 0 failed`.
2. The `raiseRecovered(...)` call at `src/core/patch_radio.cpp:117` commented out (copy restored afterwards, `git status` clean):
   `FAIL UNREPORTED ERROR PATH: src/core/patch_radio.cpp, function PatchRadio::stop, kind warn: ...`, 68 report instead of 69,
   exit 1. Restored: green.
3. One allow line deleted (`src/core/macros.hpp::Player::fail::errassign::m`): `FAIL UNREPORTED ERROR PATH ... function Player::fail,
   kind errassign`. Restored: green.
4. 60 comment lines inserted at the top of `src/core/macros.hpp` (every line number below moves): green, 22 checks 0 failed.

## Re-seeding after a merge (one command)

```
python tools/errpath_seed.py --scanner <build>/tests/Release/test_error_paths.exe --write-pin
```

(run from the repository root; add `--survey D:/foxsdr-build/prep/error-paths.tsv` if the survey is elsewhere; `--fresh` starts
from the survey alone). The tool runs the scanner's `--dump`, keeps every existing allow line whose site still exists, re-keys a
line whose function moved to another file, drops lines whose site is gone, classifies only NEW unresolved sites from the survey
join (`PENDING:unmatched` when there is no survey row), prints what it could not match, and rewrites `kPendingPin`. It
refuses to RAISE the pin unless `--allow-raise` is given: a re-seed after other lanes merged can bring in their new unreported
sites, and raising the pin is a decision. Re-running it on an unchanged tree changes nothing (checked: byte-identical file).
Nothing in it uses a line number of the current tree; the survey's own lines only order the rows of one file.

Parallel lanes that each lower the pin will conflict on the one `kPendingPin` line: take the smaller number and re-run
`--write-pin`, which recounts.

## How a later lane removes a PENDING entry

In the same change that adds the report: delete that site's line from `tests/errpath_allow.tsv` (the key is printed by the
failing test, or `grep` the file for the function), then run the seed with `--write-pin` or lower `kPendingPin` by one by hand. If
the line is left behind the test says `STALE PENDING (the site now reports through <reporter>)`; if the pin is left behind it
says `PENDING count fell to N (pin M): lower the pin`. A site that is truly information may instead be turned into `ALLOW` with a
reason (the pin falls by one the same way).

## The plugins repository

`tools/errpath-for-plugins/` (scanner header, `errpath_check.cpp`, reporters file, seed tool, CMake lines, README of the three
steps). Nothing was written into the plugins repository: `git archive 8ab7f16 src include tools` was unpacked into
`D:/foxsdr-build/eg-scratch/plug8ab7f16`. Over it, with the MSVC-built `errpath_check`:

- 953 sites (survey 999): errassign 522, swallow 386, plwarn 39, stderr 6.
- Seed: 597 ALLOW, 356 PENDING: PL3-abi-catch 221, PL4-plugin-output 71, PL1-plugin-log 39, PL2-companion 11, unsure 11,
  unmatched 3 (matches the survey's PL3 221, PL4 71, PL1 39; PL2 is 11 here against its 14).
- `errpath_check`: `error-path gate: 953 sites: 0 report, 597 allowed, 356 PENDING` ... `errpath_check: GREEN`.
- Break-it: a scratch file with a bare `CASCADE_LOG_WARN` and an empty catch: two `FAIL UNREPORTED ERROR PATH` lines,
  `errpath_check: RED`, exit 1; file removed: GREEN.
- Two plugin headers (`acars_out.h`, `cwskim_spot.h`) have `#if/#else` branches that both open a brace; the scanner notes
  them ("unbalanced braces") and carries on.

Until the host offers a plugin report call every plugin site stays PENDING or ALLOW (no reporter exists to call); the macros
`FOXPLUGIN_REPORT` / `FOXPLUGIN_CATCH_ALL` are named in the reporters file ready for it.

## Verification actually run, and what was not

Run: `test_error_paths` and `errpath_gate_selftest` (MSVC Release, built alone with `--target test_error_paths
errpath_gate_selftest errpath_check`; the engine was not built), green at the tip; the four break-its above; the seed twice
(fresh, then re-run: identical); the plugins run above. NOT run: the engine's other source-scan tests (`test_json_dump_policy`,
`test_converter_call_sites`, `test_hardware_guard`, ...). They link the whole engine and this lane builds only its own targets;
this change adds files under `tests/` and `tools/` only, edits no source under `src/`, and a grep of my test files for the
needles `test_hardware_guard` looks for (the only neighbour that reads `tests/*.cpp`) finds none. Linux and macOS compilation of
the new files: UNVERIFIED (standard C++20 and `std::filesystem` only; g++ 16 compiled `test_error_paths` with `-Wall -Wextra`
without a warning; the other programs without warning flags).

## OPEN ROWS FOR THE NEXT LANE

Kinds of site left out of the scanner, by name:

| Left out | Why | Where it matters |
|---|---|---|
| error-code locals named `*Err` (`openErr`, `startErr`, `bestErr`, `haveErr`) | `bestErr` and `haveErr` are measurements; the name cannot tell a PortAudio result from a number | `sink/audio_out.cpp` open path (`openErr = Pa_OpenStream(...)`, `startErr = Pa_StartStream(...)`): lane A (audio) must read it by hand |
| `(void)` casts and bare calls of the project's own functions | no reliable way to know which return a failure | `source/rx888_source.cpp:435` `(void)boot.controlOut(...)` (survey item 18, G1/RX888) |
| `diagLogf` lines without failure wording | the design's word list | 10 survey MUST REPORT rows (below) |
| a reporter in a different branch of the same function; a message built in a helper; a macro that wraps `diagWarnf`; `.slint` and scripts; JavaScript inside C++ strings (`web_server.cpp` page code) | text scanning cannot see them | the stated blind spots |
| a failure path that only returns `false`/an error code with no message | nothing to find | the "unreported" paths with no text at all are invisible to every pattern |
| `catch` handlers that do something other than nothing and still do not report (`catch (...) { x = 0; }`) | not "empty" by the survey's definition | review by hand when a lane touches the function |

The 10 survey MUST REPORT `diagLogf` rows the gate does not see (their groups' lanes must include them): `core/gps_reader.cpp`
"gps: fix after %llu sentences" (D-gps-serial), `core/patch_audio.cpp` "a speaker's WAV file opened after %.0f s" (R-record),
`engine/engine.cpp` "recorder: %s ended because %s" (R-record), `gui/app_window_radio_setup.cpp` and `slint/signal_publish.cpp`
"radio setup: one-click driver install requested" and "driver install %s - %s" (S5-driver-install, 4 rows), `source/pluto_tx.cpp`
"tx: stopped - the board was told to go quiet" and "tx: %s - the board was told to go quiet" (T-tx, 2 rows),
`source/sdrplay_service.cpp` "source: SDRplay API Service - %s" (S8-sdrplay-service). Two UNSURE log rows: `soapy_enum_proc.cpp`
"enumeration helper: whole bus" and "the pipe to the parent has no reader".

PENDING groups with their sizes (the reporting lanes' work lists; 1124 in all):

| Group | Lines | Group | Lines |
|---|---|---|---|
| reports-not-in-function | 610 | S5-driver-install | 5 |
| G3-control-refused | 138 | P-store | 5 |
| unsure (every UNSURE row) | 88 | S3-plugin-ui | 5 |
| O-outputs | 55 | S8-sdrplay-service | 5 |
| G2-stream-health | 29 | G8-sdrplay-lifecycle | 5 |
| R-record | 27 | S6-scan | 4 |
| G1-device-dead | 26 | G7-patch-radio | 4 |
| G10-gpu-fft | 16 | S9-report-upload | 4 |
| D-gps-serial | 13 | G6-route-fallback | 3 |
| F-files | 11 | S7-ui-engine | 3 |
| T-tx | 11 | S1-startup | 3 |
| S2-window | 10 | G5-rtl-pll | 3 |
| G9-vendor-fault | 10 | S4-first-run-check | 2 |
| unmatched (9 swallow handlers with bare return/continue, no survey row) | 9 | PL2-companion | 1 |
| S1b-headless | 7 | | |
| A-audio | 7 | | |
| P-plugin | 5 | | |

`reports-not-in-function` (610) is the survey's REPORTS verdict that the scanner cannot confirm: each needs one look. Either the
reporter really is reached from this site (add an `ALLOW` with `DETAIL-OF:<key of the reporting site>` once that site is
reported, or call the reporter in the function) or the site is not reported at all. `unsure` (88) are the survey's UNSURE rows,
the survey's listed questions among them: engine.cpp plugin-UI "is not framing" (:3360, :4337), the control-thread watchdog at
stop (:11350), the patch and settings lines (engine.cpp:8082, wire_c.cpp:167) and `plugin_marker.cpp` `plugins: %s` (:804, :887).
