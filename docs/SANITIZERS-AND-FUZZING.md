# Memory-error, thread, static-analysis and fuzz testing

Four things the ordinary build and the ordinary suite cannot do: notice a memory
error that does not (yet) change a result (AddressSanitizer), notice two threads
using one thing without agreeing how (ThreadSanitizer), read every path through
the code and not only the ones a test takes (the static analyser), and feed the
parsers input nobody wrote a test for (fuzzing). Every one is an optional build
with its own option and its own build directory; none changes anything a user
receives, and with its option off none changes a byte of what the normal build
generates (`CASCADE_SANITIZE`, `CASCADE_ANALYZE` and `CASCADE_WERROR` each return
before defining anything when they are not asked for).

## AddressSanitizer (and UBSan) over the whole suite

```
cmake -S . -B build-asan -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DCASCADE_SANITIZE=address
cmake --build build-asan --config Release --parallel 10
ctest --test-dir build-asan -C Release -j 4 --output-on-failure
```

On Linux, GCC or Clang:

```
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Release -DCASCADE_SANITIZE=address,undefined
cmake --build build-asan -j"$(nproc)"
xvfb-run -a ctest --test-dir build-asan --output-on-failure -j2
```

`CASCADE_SANITIZE` is empty by default and then does nothing at all: see
`cmake/sanitize.cmake`. `address` is AddressSanitizer (and, on Linux,
LeakSanitizer); `undefined` is UndefinedBehaviorSanitizer and is GCC/Clang only
(MSVC has none: asking for it there is a warning). It instruments the
application library, the application, the report reader, every test and the
vendored libraries linked into them.

- **A sanitizer build is a different build directory.** Keep `build/` for the
  shipping configuration.
- **Windows needs the runtime beside the program.** The build copies
  `clang_rt.asan_dynamic-x86_64.dll` next to every executable it makes, as it does
  for `SoapySDR.dll`; a program that cannot find it raises a "DLL not found" dialog
  on the desktop instead of failing. It needs the *MSVC AddressSanitizer* component
  of Visual Studio. `/RTC`, `/ZI` and incremental linking, which cannot be combined
  with it, are removed for a sanitizer build, and `/bigobj` is added.
- **It is slow.** Every test's time limit is multiplied by
  `CASCADE_SANITIZE_TIMEOUT_SCALE` (5). Runtime options (`ASAN_OPTIONS`,
  `UBSAN_OPTIONS`, `LSAN_OPTIONS`) are attached to each test by CMake, so a failure
  is the same on every machine and in CI.
- **Tests that cannot run under it are listed, not dropped.** They are registered
  with the label `sanitize-excluded` and disabled, so `ctest -N -L sanitize-excluded`
  prints them and a full run ends with "Not Run (Disabled)" lines. The reason for
  each is next to the list in `tests/CMakeLists.txt`. Three tests that fault a child
  process on purpose, to prove the crash handler writes its report, do run, with
  AddressSanitizer's own fault handlers switched off (otherwise it ends the process
  before the product's handler sees the fault). Memory-error checks stay on.
- **Timing-sensitive tests can fail for speed alone.** An instrumented DSP thread
  can fall behind a source paced by the wall clock; the symptom is a numeric
  failure with no sanitizer report. Read the output before believing either way.

## ThreadSanitizer

```
cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Release -DCASCADE_SANITIZE=thread
cmake --build build-tsan -j"$(nproc)" --target cascade_tsan_tests
xvfb-run -a ctest --test-dir build-tsan -L tsan --output-on-failure -j2
```

`thread` is GCC/Clang on Linux only (MSVC has no thread sanitizer: asking for it
there is a configure error). It cannot share a process with `address`
or `fuzzer`, so CMake refuses `address,thread` and `thread,fuzzer`; `thread,undefined`
is allowed. Runtime options (`TSAN_OPTIONS`: stop at the first report, show both
stacks of a lock-order inversion, a longer access history, the suppressions file)
are attached to each test by CMake, and every test's time limit is multiplied by
`CASCADE_SANITIZE_TIMEOUT_SCALE` (10: an instrumented program is 5-15 times slower).

**It runs a selection, not the suite.** A thread sanitizer finds a race between
two threads and nothing else, so a test with one thread in it and no thread inside
the code it drives cannot fail under it and would only cost a build and a slow
run. `tests/tsan.cmake` is the list: 70 tests run (label `tsan`) - the pipeline
and its source and DSP threads, the patch radios, the plugin runner and host API,
the config/bookmark/marker savers and the disk jobs, the health ledger's writer,
the frame timer, the watchdog, the web and CAT servers, the telemetry sender, the
sound sink's callback thread, and the real web server over a loopback socket.
Everything else is registered, disabled and labelled `sanitize-excluded`, like the
ASan exclusions, so `ctest -N -L sanitize-excluded` prints it. Of those, 41 are
excluded one by one with a reason in the file - they start the real application as
a child process (a separate process whose reports would not reach this one's),
fault a process on purpose, measure against the wall clock (an instrumented DSP
thread falls behind a paced source and fails with no race report), or drive the
whole window under a software GL context - and the other 202 start no thread and
drive no code that has one. Judge a test by what it starts, not by its name, when
you move one.

**Suppressions** are in `tools/sanitize/tsan.supp` and name third-party libraries
only (`called_from_lib:` the sound server, Mesa, X11/Wayland), each with the
reason. Nothing of this repository is in it and nothing may be added for it: a
race in our code is fixed. The vendored libraries are compiled here with the
sanitizer, so a report in them is a finding too. PortAudio's ring buffer orders
its two threads with full memory barriers, which ThreadSanitizer does not model;
if the first log reports it, the line to add is named in the file, with that
report as the reason.

**The list, the suppressions and the job were written without a Linux run.** The
first log shows which of the 70 are too slow or too noisy and which exclusions are
worth lifting; do not read the absence of a report from a job that has never run
as a result.

### Shared state across threads in 0.99.64 (read, not run)

What each piece is, which threads touch it, what guards it, and whether it is
believed correct. Read from the source on the tree this was written against; the
test that would show a regression is a TSan run of the tests above.

| State | Threads | Guard | Believed correct |
|---|---|---|---|
| Breadcrumb block (`core/breadcrumb.hpp`, one 64-byte page shared with the watcher process) | Writers: the GUI thread only (phase, heartbeat, frame scope, the user-paced bit from the window procedure, the radio-opening bit mirrored each frame, the plugin-reload bit). Reader: another process, after the application has ended | Every field through `std::atomic_ref`, relaxed except the magic (release/acquire) and the `g_block` pointer; bit words changed with `fetch_or`/`fetch_and` | Yes. `phase`/`phaseMs` and `frames` are load-then-store, which is sound only because one thread writes them: every call site was read (`main.cpp`, `app_window.cpp` run/teardown, `hang_watchdog.cpp` heartbeat, `win_frame.cpp`) and all are the GUI thread. A second writer would lose updates (not a data race, but wrong) |
| `FrameTimer::current_`, `table_` (`core/frame_timing.hpp`) | Writer: the GUI thread. Readers: the freeze watchdog's thread (`frame-scope:` line) and the usage record | `std::atomic`, relaxed stores and `fetch_add`; `counts()` loads each cell relaxed | Yes. Each cell is exact; the cells are not read at one instant, which a usage record does not need. The rest of the timer is plain memory owned by the GUI thread, and all 11 `FrameScopeGuard` sites in `app_window.cpp` are in functions run only from `run()`/`drawUi()` (checked); the test hooks (`setStalls`, clocks) are called only before the loop |
| `HealthLedger` (`core/health_events.cpp`) | Any thread calls `note()`; one short-lived detached writer thread; `disarm()`'s removal thread; the Settings switch on the GUI thread | One mutex over all state; a second mutex (`io`) serialising file writes and removals; a generation counter that makes a stale write skip itself; the writer owns a `shared_ptr` to the state so the ledger may be gone first | Yes for memory safety: every field is read and written under the mutex, lock order is `io` then the state mutex everywhere (nothing takes them the other way), `flush()` waits on a condition variable under the state mutex. One ORDERING hazard, not a data race: `disarm()` then `arm()` of the same path within the scheduling latency of a thread start can let the removal thread (started first, `health_events.cpp:698`) take `io` after the new writer has written, deleting a file the ledger believes it has written; the next change rewrites it. Needs two Settings toggles within microseconds; not fixed, no deterministic test possible |
| `DiskJob<Result>` (`gui/disk_job.hpp`) | The GUI thread requests and polls; one `std::async` worker; at quit a detached thread holds an abandoned future | The result crosses through the `std::future` (its own synchronisation); the work owns its inputs by value; the throw flag is a `shared_ptr<std::atomic<bool>>`; one worker at a time, so no two jobs race for a file | Yes. All other members are GUI-thread only. `label_` is a `const char*` the caller keeps alive (every use is a literal) |
| `ConfigWriter` and `BackgroundSaver` (`gui/config_writer.hpp`, `gui/background_saver.hpp`) | The GUI thread; one `std::async` worker per saver; an abandoned worker at quit | Same shape: text and path by value into the worker, `Outcome` back through the future, queue and counters touched only on the GUI thread; one write out per saver, so a newer write never lands before an older one | Yes. The temporary file is named with the process id, not the thread, so two simultaneous writes to ONE target would share a temporary - impossible here (one write out per target) and not reachable at quit (nothing new is requested after the drain) |
| Sentinel start/stop state (`core/sentinel_host_win.cpp`, `sentinel_host_posix.cpp`: `Host g`, the pid, pipe and mapping) | The GUI thread: `main()` configures and enables it, `AppWindow` polls it once in about 300 frames and toggles it from the Diagnostics switch | None, by design ("GUI thread only: nothing here is locked" in `sentinel.hpp`) | Yes, as long as that stays true: the only callers are `main.cpp:1385-1386`, `app_window.cpp:1828` and `app_window.cpp:27060`, all on the main thread. The shared page is never unmapped while the process lives, so a stray writer would never touch freed memory |

No race was found. Three conditions the code relies on and does not enforce (a
second thread calling the heartbeat or `setPhase`; a frame-scope guard opened
off the GUI thread; the sentinel API called from a worker) are the first things
to check if a TSan run reports in these files.

## Fuzzing

`tests/fuzz/` has one small file per target, each an
`LLVMFuzzerTestOneInput(const uint8_t*, size_t)`. `fuzz_<target>.cpp` and its
inputs in `tests/fuzz/corpus/<target>/` are the whole of a target. They cover what
reaches the program from outside - the plugin catalogue and manifest, the update
answer, `config.json`, bookmark, marker and band-plan files, frequency-list imports,
I/Q file headers, CAT commands, web-remote requests (the control body, and the real
HTTP server over a loopback socket), crash reports, the report feed, NMEA, patch
documents - and the rate-dependent DSP: the resampler, the channel filter, the
demodulators and the stereo, RDS and noise-reduction blocks at arbitrary rates and
block sizes, and the patch page's rate arithmetic.

**The regression corpus is part of the normal suite.** Every target is also run as
`fuzz_corpus_<target>` in CTest: the program `fuzz_corpus_replay` feeds each corpus
file to the target once and fails on any fault or violated property. It needs no
fuzzing engine, so it runs on every platform, under AddressSanitizer in a sanitizer
build, and in `build.yml`. An empty corpus directory is a failure.

**Real fuzzing** needs the libraries to carry libFuzzer's coverage counters, so it
is a third build directory:

```
cmake -S . -B build-fuzz -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DCASCADE_SANITIZE=address,fuzzer -DCASCADE_FUZZ=ON
cmake --build build-fuzz --config Release --target fuzz_config fuzz_cat_command   # any of the fuzz_<target>
mkdir work
build-fuzz/tests/Release/fuzz_cat_command.exe -max_total_time=300 work tests/fuzz/corpus/cat_command
```

On Linux the same with Clang (`CC=clang CXX=clang++`, `-DCASCADE_SANITIZE=address,undefined,fuzzer`).
MSVC's `/fsanitize=fuzzer` is used on Windows (Visual Studio 2022 17.14 here). The
first directory given is where libFuzzer writes what it finds; the second is the
regression corpus, read as seeds.

**A finding becomes a test.** Minimise it (`-minimize_crash=1`), put the file in
`tests/fuzz/corpus/<target>/` with a name that says what it was, and write the
ordinary unit test that fails without the fix, in the existing test file for that
code, before fixing it. The corpus file then keeps it fixed on every platform; the
unit test says why.

A new target: add `tests/fuzz/fuzz_<name>.cpp`, add `<name>` to the list in
`tests/fuzz/fuzz.cmake`, and put at least one input in `tests/fuzz/corpus/<name>/`.
Corpus files are committed to a public repository: nothing in them may name a
person, a machine, an address or a path.

## Static analysis (MSVC `/analyze`)

```
cmake -S . -B build-analyze -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DCASCADE_ANALYZE=ON
cmake --build build-analyze --config Release --parallel 10 --target cascade_lib cascade
```

`CASCADE_ANALYZE` is off by default and then does nothing (`cmake/analyze.cmake`
returns before defining anything; the generated project files of a normal build are
identical with and without the file). On, it adds to every target that links
`cascade_flags` (the application library, the application, the report reader, the
tests) - and so not to the vendored libraries - these switches, each tried on a small
program first:

- `/analyze`: the C6xxx (PREfast) and C28xxx families - C6001 uninitialised memory,
  C6011 null dereference, C6385/C6386 buffer over-read and overrun, C28182.
- `/analyze:external-`, `/external:anglebrackets`, `/external:W0`: code from headers
  included with `<angle brackets>` is not analysed and its warnings are silenced.
  That is the standard library, the Windows SDK, and every third-party header
  (`<imgui.h>`, `<nlohmann/json.hpp>`, `<httplib.h>`, `<SoapySDR/...>`). This tree
  includes its own headers with quotes everywhere, so none of ours is hidden by it.
  CMake already marks the vendored and vcpkg include directories as `/external:I`;
  the angle-bracket rule covers what that cannot, the compiler's and the SDK's own
  directories.
- `/analyze:plugin <EspXEngine.dll>` and `Esp_Extensions=ConcurrencyCheck.dll`: the
  extension host and the one checker run by default. Without the host `/analyze`
  runs no concurrency checks at all (C26110 "caller failing to hold lock", C26115,
  C26117, C26135 come only from it). Left alone the host also loads CppCoreCheck,
  whose C264xx rules add about 9,800 style warnings (`noexcept`, `gsl::at`, raw
  pointers): a different review, available with
  `-DCASCADE_ANALYZE_EXTENSIONS=ALL`. With the Visual Studio generators a
  `Directory.Build.props` in the build directory sets the variable for cl.exe; with
  another generator set `Esp_Extensions` yourself.

`CASCADE_ANALYZE` is MSVC-only: elsewhere it is a warning and nothing else (GCC's
`-fanalyzer` has no usable C++ support).

**What it found on the 0.99.63 tree** (MSVC 19.44, `cascade_lib` and the
application): 26 analyzer warnings and the 10 compiler warnings that `/W4` already
gave (below), in our code only. None of the priority families (C6001, C6385,
C6386, C28182) fired; C6011 fired once, and the lock-checking family four times
(C26110 x2, C26117, C26135). Every one was read against the code, and none is a
defect:

| Warning | Where | Verdict |
|---|---|---|
| C6320 x4, C6322 x2 | `crash_handler.cpp:468`, `:710`; `hang_watchdog.cpp:81`; `diag_report.cpp:289` | Intended. An `__except(EXCEPTION_EXECUTE_HANDLER)` around a hand-rolled stack walk or a read of a module header that may be unmapped: swallowing the fault is the design (what was collected before the bad frame is still worth having) |
| C6011 | `crash_handler.cpp:1488` | False positive. The null store is the deliberate access violation of `raiseTestFault`, there to provoke the handler |
| C6387 | `crash_handler.cpp:1022` | Real, harmless as far as the code reads. `sehFilter` tolerates a null `ep` for its own use and then forwards it unchanged to the previous filter; its only caller is the operating system, through `SetUnhandledExceptionFilter` (`:1176`), and no caller in the tree passes null |
| C26110 x2, C26117 | `patch_audio.cpp:115`, `:123`; `plugin_runner.cpp:545` | False positives. The first two are exception edges after and inside a `lock_guard` taken three lines earlier, which the checker loses through `sh->m` (a `shared_ptr`); the third is `destroyInstances(std::unique_lock&)`, whose contract is that the caller holds the lock, and both callers (`:122`, `:455`) do |
| C26135 | `diag_log.hpp:119` | Real, harmless. A test hook that returns a held lock has no `_Acquires_lock_` annotation; the application never calls it |
| C6031 x3 | `sdrplay_service.cpp:230`, `:367`; `soapy_enum_proc.cpp:1585` | The first two are false positives: the documented size-probe call of `QueryServiceConfigW`, which is expected to fail and fill `needed`, and the result is checked on the next line. The third is real, harmless: `_setmode` on the child's stdout, which cannot fail for a valid descriptor |
| C6262 x4 | `plugin_api.cpp:209` (16 KB, a scratch copy of the control queue), `main.cpp:890` (36 KB, `main`), `web_server.cpp:3534` (19 KB, a streaming-response lambda), `airspyhf_protocol.hpp:509` (32 KB, the one-time initialiser of a static table) | Real, harmless. Large locals in functions that run on the main, GUI or web-worker thread; none is near a 1 MB stack, and none of the four is recursive |
| C6326 x2 | `diag_log.cpp:289`, `aor_source.cpp:499` | Real, harmless. A deliberate comparison of two constants (`kKeptFiles >= 2`; a log label that turns to `?` if the sample-rate constant ever changes) |
| C6246 x5 | `app_window.cpp:16937`, `:22677`, `:23060`; `demod_scope_face.cpp:604`; `sdrplay_source.cpp:2673` | Real, harmless. An inner variable of the same name as an outer one in a different role (a colour channel and a rectangle; a text buffer and a number); each was read and the outer value is not used by mistake. The same five sites are the C4456 warnings below |

The 26 stay in the output on purpose: no `#pragma warning` or cast was added to any
of them, so a new one is visible against this list. Nothing in product code was
changed for this work.

## Warnings

Measured on the same tree, a clean Release build at the existing level (`/W4`;
`-Wall -Wextra` on GCC and Clang) - which this change does not raise:

- **MSVC: 112 warnings, not zero.** 10 in 6 product files: C4456 x5 and C4458 x2
  (a name shadowing another, the same sites as C6246 above plus `crash_handler.cpp:341`
  and `:354`), C4100 x2 (`scope_view.cpp:412`, two parameters the function never
  reads - the caller computes the top altitude and nothing draws it), C4127 x1
  (`diag_log.cpp:289`). And 102 in tests: C4127 x91 (a `CHECK` whose condition is
  a compile-time constant, from `tests/test_check.hpp`'s macro), C4456 x9 (`src` and
  `gui` redeclared inside test bodies), C4005 x2 (`WIN32_LEAN_AND_MEAN` defined
  twice). None is a defect. What stands between the tree and zero is ten small edits
  in product code and, in the tests, one change at the `CHECK` macro or ninety-one
  at its call sites.
- **GCC, a proxy:** no Linux machine was available, so MinGW GCC 16 with
  `-fsyntax-only` over the sources it can parse (141 of 152 under `src/`, 279 of 284
  tests) gave 28 warnings in product code and 22 in tests at `-Wall -Wextra`: 25 of
  the 28 are in `soapy_source.cpp` (17 `-Wmissing-field-initializers` on partly
  initialised result structs and 8 `-Wswitch` for `Pending` and `NotRun`, which the
  function it switches on documents as never returned), 2 are the unread
  parameters above, and 1 is a `memcpy` of `std::complex` (`dsp/fft.cpp:95`,
  `-Wclass-memaccess`). The 11 source files and 5 tests it could not parse are
  unmeasured. Real GCC on the CI runner may differ either way.

`CASCADE_WERROR=ON` makes a warning in our own targets an error (`/WX`, `-Werror`),
at that same level. It is off everywhere, in `build.yml` and `sanitize.yml` alike,
because the count above is not zero; the sanitizer workflow is where it goes on
first, when it is.

**Extra warnings worth enabling, with what one build measured** (counts are unique
`file:line:number` sites; `src/` through MSVC with the flags raised by the `CL`
environment variable, and through the GCC proxy; the tests through the proxy only):

| Warning | `src/` | tests | Worth it? |
|---|---|---|---|
| MSVC C4062 (enumerator not handled, no `default`) | 4 | n/m | Yes: the four are all in `soapy_source.cpp` and are the same switches GCC's `-Wswitch` already flags |
| MSVC C4061 (enumerator not handled explicitly, `default` present) | 14 | n/m | Later: one switch, `plugin_host.cpp:962`, accounts for 53 of the messages (it names a few rejection reasons and defaults the rest) |
| MSVC C4242, C4254, C4263, C4264, C4265, C4287, C4296, C4302, C4311, C4355, C4545-C4547, C4549, C4555, C4619, C4640, C4826, C4905, C4906, C4928, C4946, C4165, C4191 | 0 each | n/m | Yes, and free: nothing fires today, so turning them on only guards the future (C4191 and C4242 do fire in the vendored libraries, which do not link `cascade_flags`) |
| MSVC C4365 (signed/unsigned) | 60 | n/m | No: indexing `std::array` and `std::vector` with an `int`, and `char` to `unsigned char`; GCC's `-Wsign-conversion` says the same thing (70 in `src/`, 39 in tests) |
| GCC/Clang `-Wshadow` | 10 | 12 | Yes: already an MSVC `/W4` warning (C4456/C4458), so the Linux jobs are the ones that cannot see it |
| GCC/Clang `-Wconversion` | 1 | 6 | Yes after the 7 are fixed: `sdrplay_probe.cpp:483` and, in tests, five `int` to `float` and one `size_t` to `double` |
| GCC/Clang `-Wold-style-cast` | 0 | 14 | Only with `core/plugin_abi.h` excluded: it is a C header for plugin authors and its two hits are casts inside its macros, which the tests expand. The other twelve are casts in tests |
| GCC/Clang `-Wsign-conversion` | 70 | 39 | No, as above |

n/m: not measured (the MSVC extra-warning build covered `cascade_lib` and the
application, not the tests).

## CI

`.github/workflows/sanitize.yml` (separate from `build.yml`, so it can neither slow
nor colour the release jobs) has three jobs, in parallel on separate runners: the
whole suite under ASan + UBSan with GCC; the thread-sanitizer selection of
`tests/tsan.cmake` under ThreadSanitizer with GCC (`tsan`); and the fuzz targets
under Clang - the corpus replayed first, then each target for a short fixed time
(`fuzz_seconds`, default 60). Start it on a branch from the Actions tab or with
`gh workflow run sanitize.yml --ref <branch>`. None of it has run on Linux yet; the
first run of each job will show what needs correcting, and `tsan` the most.
`CASCADE_ANALYZE` has no CI job: it needs MSVC, and `build.yml`'s Windows job is
not edited by this work.
