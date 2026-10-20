# Memory-error and thread testing (engine/stage4 branch)

**This is the sanitizer page of origin/master, extracted into the engine/stage4
branch ahead of the full master merge** (master is hundreds of commits ahead;
merging is its own project). What landed is the sanitizer plumbing and the
ThreadSanitizer job, as groundwork for the pump move's threaded tests. What did
not land is listed under "Master-only, not on this branch" at the end, and the
rest of this page says where master's instructions differ here. When the branch
merges, take master's page and keep its history.

Two optional builds, each with its own option and its own build directory;
neither changes anything a user receives, and with `CASCADE_SANITIZE` empty (the
default) `cmake/sanitize.cmake` returns before defining anything, so the generated
project files are what they were before it existed.

## ThreadSanitizer

```
cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Release -DCASCADE_SANITIZE=thread
cmake --build build-tsan -j"$(nproc)" --target cascade_tsan_tests
xvfb-run -a ctest --test-dir build-tsan -L tsan --output-on-failure -j2
```

`thread` is GCC/Clang on Linux only (MSVC has no thread sanitizer: asking for it
there is a configure error, which is the gate on Windows), and the CI job uses
Clang 18: GCC 13's libtsan has no interceptor for `pthread_mutex_clocklock`,
which libstdc++ calls for every `std::timed_mutex::try_lock_for`, so under it
every unlock of such a mutex is reported as an unlock of an unlocked mutex and
the happens-before edge of the lock is lost (LLVM added the interceptor in release
18). A newer GCC (15 was used for the local proof below) may have it; that is not
the CI compiler and not verified for the whole suite. It cannot share a process
with `address`, so CMake refuses `address,thread`; `thread,undefined` is allowed.
Runtime options (`TSAN_OPTIONS`: stop at the first report, show both stacks of a
lock-order inversion, a longer access history, the suppressions file) are attached
to each test by CMake, and every test's time limit is multiplied by
`CASCADE_SANITIZE_TIMEOUT_SCALE` (10: an instrumented program is 5-15 times
slower).

**On this branch the sanitizer reaches the code through `cascade_engine`.**
Master links `cascade_sanitize` into `cascade_lib`; the branch split that in two,
so the line is `target_link_libraries(cascade_engine PUBLIC cascade_sanitize)` in
`CMakeLists.txt`, and the window (`cascade_gui`), the application, the report
reader and every test inherit it. The vendored `imgui`, `glfw`, `portaudio_static`
and `cascade_pffft` are instrumented as on master. The test fixtures built as
plugin modules (`tests/fixture_*_plugin.cpp`) are not: they stand in for a third
party's plugin, which is not built with our flags either.

**It runs a selection, not the suite.** A thread sanitizer finds a race between
two threads and nothing else, so a test with one thread in it and no thread inside
the code it drives cannot fail under it and would only cost a build and a slow
run. `tests/tsan.cmake` is the list, and on this branch it is the engine's
threads, not master's list:

- the engine API through the C table against a real headless engine on its own
  control thread: `test_engine_api_conformance`, `test_engine_api_03`,
  `test_engine_api_merge`, `test_engine_api_transmit`;
- the control thread against the front end's thread: `test_command_queue_threads`,
  `test_engine_control_thread`, `test_engine_host_threads`, `test_engine_shutdown`;
- the one receiver snapshot, readers against a publishing writer:
  `test_receiver_snapshot`, `test_snapshot_stress`;
- every op the engine applies: `test_apply_command`;
- the transmit key across front end, control thread and TX thread:
  `test_transmit_key_race`, `test_transmit_liveness`, `test_transmit_dead_man`;
- the pump move's step P1 (the engine PULLS the web/CAT queues in its own pass; the key rules are the control thread's alone):
  `test_remote_control_pull` and `test_remote_key_release`.

That is 16 tests (label `tsan`; 14 until the pump move's step P1). Everything else is registered, disabled and labelled
`sanitize-excluded`, so `ctest -N -L sanitize-excluded` prints it, and every excluded
test that is named in the file carries its reason: it starts the real application as a
child, faults a process on purpose, measures against the wall clock, drives the whole
window under a software GL context, or is **not run yet**. The last is the large
group: the 46 tests of master's TSan list that this branch also has, and 7 of the
branch's own that are not engine-threading proper (`test_engine_headless`,
`test_engine_publish_alloc`, `test_snapshot_app`, [`test_remote_key_release`: IN since step P1],
`test_hang_capture_concurrent`, `test_pa_stream_list_guard`,
`test_patch_graph_draft_input`). Master's first run found its first races in the
signal generator and in tests and fixed them there; running master's list here
before the merge would mostly report what master already repaired, in a job whose
purpose is to be quiet enough to read when the pump move's own threaded tests go
in. A test joins the selection by moving its name from an exclusion list to
`CASCADE_TSAN_TESTS`; the typo check refuses a name that is not a test. The
remaining tests start no thread and drive no code that has one. Judge a test by
what it starts, not by its name, when you move one.

The list was written without a Linux run on this branch; read the first log before
believing it. `test_engine_api_transmit` and `test_engine_api_merge` run at real
timers (serial, 600 s before the scale): a probe's heartbeat starving on a slow
runner is the rule working and reads as a failure with no TSan report.

**Suppressions** are in `tools/sanitize/tsan.supp`, ported from master unchanged
in its rules: third-party libraries only (`called_from_lib:` the sound server,
PulseAudio's client library, libunwind, Mesa, X11/Wayland) and two named upstream
PortAudio races, each with the reason. Nothing of this repository is in it and
nothing may be added for it: a race in our code is fixed. The reasons cite master's
first run (37355910809); this branch has not had one, so a line it never needs is
not evidence the line is wrong.

**`halt_on_error` ends a test at its FIRST report**, so a test that stopped on a
library's noise has not yet shown what comes after it. Do not read the absence of a
report from a test that has not yet got past its first one as a result.

## AddressSanitizer (and UBSan)

The option is in place (`cmake/sanitize.cmake`, `-DCASCADE_SANITIZE=address`, and
`address,undefined` on GCC/Clang) and the build wiring is the same as for `thread`,
but **no ASan job runs on this branch and the option has not been exercised beyond
configure** (see "What was verified"). Master's job needs two source files the branch
lacks, so do not read a failure under `address,undefined` here as a finding before
checking them:

- `core/ubsan_exempt.hpp` (`CASCADE_UBSAN_EXEMPT`): the functions whose job is the
  fault (`raiseTestFault`, the null store of `test_soapy_enum_proc`'s helper) are
  exempt from UBSan by name, or its null check ends the child before the SIGSEGV the
  crash handler is being tested on;
- `core/leak_on_purpose.hpp`: a leak that is the design (a driver giving up on a
  thread that will not return) is marked where it is made so LeakSanitizer ignores it.

`tests/CMakeLists.txt` applies master's ASan exclusions (the wall-clock audio tests,
`test_crash_second_fault`) to the tests the branch has; `tools/sanitize/lsan.supp`
(libraries only) came with it. On Windows the MSVC runtime DLL is staged beside every
executable, as `SoapySDR.dll` is; it needs the *MSVC AddressSanitizer* component of
Visual Studio.

## CI

`.github/workflows/sanitize.yml` is a separate workflow from `build.yml`, so it can
neither slow nor colour the release jobs. **It has one job, `tsan`** (Ubuntu 24.04,
Clang 18, `CASCADE_SANITIZE=thread`, builds only `cascade_tsan_tests`, runs
`ctest -L tsan -j2` under xvfb with the null audio device, uploads the log as
`tsan-test-log`). It runs on **every push to `engine/stage4`**, on a pull request that
changes the sanitizer files, and by "Run workflow" (GitHub offers that only for a file
on the default branch, so until the merge the push is the way). Master's other two
jobs are not here; see below.

## What was verified (and what was not)

Windows (MSVC, Visual Studio 17 2022, vcpkg), 2026-10-20: the normal configure and build
are unchanged by this work and the suite passed 262 of 262 at `ctest -C Release -j 8`
(`ctest -N -L tsan` lists nothing there). `-DCASCADE_SANITIZE=thread` stops the configure
with the message "MSVC has no ThreadSanitizer"; `undefined` warns and is ignored;
`address` configures cleanly, with `/fsanitize=address` on `cascade_engine`,
`cascade_gui`, the tests, `imgui` and `portaudio_static`, and the runtime DLL staged.
The ASan build itself was not built or run.

Linux, locally, in WSL (Ubuntu 26.04, GCC 15.2, libtsan from the compiler package; not
the CI compiler): `-DCASCADE_SANITIZE=thread` configures to "14 tests run, 84 excluded one
by one, 164 excluded as single-threaded" (14 + 84 + 164 = the 262 tests of the suite);
`test_snapshot_stress` (10 s) and `test_receiver_snapshot` (4.5 s) PASS under
ThreadSanitizer through `ctest -L tsan`, with the options CMake attaches
(`TSAN_OPTIONS=halt_on_error=1:...:suppressions=tools/sanitize/tsan.supp`); and
`test_command_queue_threads` runs its checks under it (6 checks, 0 failed) and then stops
on a report in the vendored PortAudio ALSA backend, which is the next layer of the noise
and is not suppressed (below). To run it the way CI would on a machine whose ALSA default is
a sound server (WSLg's is), point ALSA at a null device from a user file, and for a test that
touches the display use X11 rather than Wayland:

```
cat > ~/asound-null.conf <<'EOF'
</usr/share/alsa/alsa.conf>
pcm.!default { type null }
ctl.!default { type null }
EOF
export ALSA_CONFIG_PATH=~/asound-null.conf XDG_SESSION_TYPE=x11; unset WAYLAND_DISPLAY
```

**What the local run showed, for the first CI log to be read against:**

- GCC warns `-Wtsan` on every inlined `std::atomic_thread_fence` (20 sites: `SeqlockBox`,
  from `receiver_snapshot`, `plugin_api`, `pipeline`, `fox_engine_api`). ThreadSanitizer does
  not model a stand-alone fence as synchronisation, so a seqlock's reader/writer ordering
  that rests on one can read as a race. Neither snapshot test reported one here; if CI's
  Clang run does, check the fence first.
- `libpulse.so.0` (`pa_threaded_mainloop_wait`, "unlock of an unlocked mutex") appeared
  under PortAudio's device enumeration when a sound server was present: added to
  `tsan.supp` with that report as its reason (the one line in this port that master does
  not have).
- A data race in the vendored PortAudio ALSA backend: `callbackAbort` (a `volatile
  sig_atomic_t`) written by `RealStop` on the closing thread and read by `OnExit` on the
  callback thread (`pa_linux_alsa.c:3099` and `:3397`), reported at the destruction of
  an `AudioOut` that had a stream open. It is upstream's flag-by-volatile idiom and benign
  by construction: the store is made before the thread is told to stop and the read is made by
  the thread's cleanup handler after `pthread_cancel`, an ordering ThreadSanitizer does not
  model. **DECIDED (pump move step P1, 2026-10-20): suppressed, by the name of the function that
  makes the write** - `race:RealStop` in `tools/sanitize/tsan.supp`, with the report quoted there
  as its reason. Not `race:OnExit` (it calls the stream's finished callback, which is our code),
  and the vendored file is not patched (it is not compiled on Windows, where this is developed).
  With it in place `test_command_queue_threads` passes under ThreadSanitizer and the next layer
  shows (below).

**The first full run of the selection (pump move step P1, 2026-10-20, WSL, GCC 15.2; the Clang 18 CI leg has still not run).**
16 tests are in the selection (the 14 above, plus `test_remote_control_pull` and `test_remote_key_release`, which came in with
the pump move). With the `callbackAbort` line in place: **12 pass** (`test_transmit_key_race` 44 s, `test_remote_control_pull`,
`test_remote_key_release`, `test_transmit_dead_man`, `test_transmit_liveness`, `test_engine_control_thread`,
`test_command_queue_threads`, `test_engine_host_threads`, `test_engine_shutdown`, `test_receiver_snapshot`,
`test_snapshot_stress`, `test_apply_command`) and **4 fail on
races that are not the pump move's** (none has a frame of the code P1 or P2 changed), each the next layer behind the one that was
suppressed. Investigated, NOT suppressed, and not fixed here; the owner's call:
1. **`pa_front.c:312 AddOpenStream`** (`test_engine_api_transmit`, `test_engine_api_merge`): PortAudio's list of open streams
   (`firstOpenStream_`) written by two threads at once, each holding a DIFFERENT mutex (its own `AudioOut::apiMutex_`). Both tests
   build several engines on several threads at the same instant, which the application never does. It is also the documented
   escape hatch of the product's own `PaStreamListGuard::NoWait` (`sink/pa_init.hpp`): an open that finds the guard busy goes ahead
   without it so the GUI thread never waits. A test-only serialisation of the engines' construction and destruction removes it from
   `test_engine_api_transmit` but not the next one (2); it was tried and reverted. After the pump move the GUI no longer opens audio,
   and the engine's opens can take the guard with `Wait` - P3/P5's decision.
2. **`pa_front.c:363 Pa_Initialize` / `:1357 PaUtil_ValidateStreamPointer`** (`test_engine_api_conformance`, and behind 1 in
   `test_engine_api_transmit`): PortAudio's `initializationCount_`, a plain int, written by `Pa_Initialize`/`Pa_Terminate` (every
   `AudioOut`/`AudioIn`/sound card construction goes through `paInitializeShared`, under a lock of its own) and read, unlocked, by
   `Pa_IsStreamActive` - which the pass calls through `AudioOut::streamAlive()` from `fillPublishedState` on every publish. The value
   never moves below 1 while an engine lives, so it is benign in effect; it is a real race in kind, today (a worker's sound card
   beside the GUI-thread pump) and more after the pump move. The remedy that fits the file's own rule is for `streamAlive()` to take
   the pa_init lock (or keep a cached answer the audio thread updates), not a suppression.
3. **`scope_tap.hpp:119/151` `ScopeTap<float>::push` against `snapshot`** (`test_engine_api_03`): a sequence-lock whose payload is
   copied with `memcpy`, written by the DSP thread and read by the API thread. The reader retries when the sequence moved, so the
   torn copy is discarded - the usual seqlock, which ThreadSanitizer reports because the payload is not atomic. Master's list has
   `test_scope_tap` under "not run yet" for the same family. The fix is word-wise relaxed atomics in the tap (or an annotated
   reader), not a suppression.
A deliberate stray call also proved the leg catches the defect P1 fixes: `Engine::applyControlRequest` made from a test thread beside
the running control thread is reported at once (`TxKeyRules::key` writing `changes_` while the pass's publish reads `changes()`),
which is why `test_remote_control_pull` D0 uses the one stray call that only reads the rules.

**Not run:** the Clang 18 CI job itself, the whole `cascade_tsan_tests` set under
ThreadSanitizer (only the three tests above were built and run), and any ASan or UBSan job.

## Master-only, not on this branch

Referenced by master's page or files and deliberately not ported; each comes with the
merge:

- **The `sanitize` job** (ASan + UBSan over the whole suite): needs `ubsan_exempt.hpp`
  and `leak_on_purpose.hpp` above, and master's exclusion list for the tests this branch
  has not got.
- **Fuzzing**: `tests/fuzz/` (23 targets and their corpora), `tests/fuzz/fuzz.cmake`,
  `CASCADE_FUZZ`, the `fuzz_corpus_*` tests and the `fuzz` job. `cmake/sanitize.cmake`
  accepts `fuzzer` but there is nothing to build with it here. `fuzz_corpus_web_http`
  is on master's TSan list for the same reason it is absent from this one.
- **Static analysis and warnings**: `cmake/analyze.cmake`, `CASCADE_ANALYZE` (MSVC
  `/analyze`) and `CASCADE_WERROR`, with master's measured findings and warning counts.
- **Master's shared-state review of 0.99.64** (breadcrumb block, frame timer, window
  state, health ledger, `DiskJob`, savers, sentinel): written against code the branch
  has since changed around (stage 3 moved the receiver's ownership into the engine); it
  would have to be redone against this tree rather than copied.
- **Tests on master's TSan list that the branch does not have** (they come with the
  merge and are then judged): `test_audio_clip`, `test_audio_lead_app`,
  `test_audio_open_log`, `test_autogain_app`, `test_aor_source`, `test_bookmark_save_async`,
  `test_dsp_priority`, `test_excuse_cap`, `test_frame_timing`, `test_gui_disk_audit`,
  `test_gui_file_jobs`, `test_hang_signature`, `test_health_events`, `test_health_paths`,
  `test_iq_open_async`, `test_link_request_poll`, `test_one_chip_one_route`,
  `test_patch_dest_async`, `test_patch_presets_app`, `test_ppm_app`, `test_record_start`,
  `test_sdrplay_service`, `test_soapy_read_bound`, `test_source_overreport`,
  `test_tester_link_app`, `test_watchdog_startup`, `fuzz_corpus_web_http`; and, among
  master's exclusions, the child-process tests `test_tester_link`, `test_tester_usage`,
  `test_sentinel*`, `test_stall_count`, `test_health_app`, `test_clean_exit_marker`,
  `test_slow_frames_app`, the crash test `test_crash_late_module`, the wall-clock tests
  `test_channel_bandwidth`, `test_pipeline_device_rate_audio`, `test_airband_app`,
  `test_pipeline_audio_lead`, and the GL tests `test_diag_context_app`,
  `test_one_chip_app`, `test_bandwidth_app`.
- Master's `-g0 --strip-debug` for Linux test executables (outside sanitizer builds),
  `CASCADE_GUI_TESTS` and `CASCADE_REALTIME_AUDIO_TESTS` label handling in
  `tests/CMakeLists.txt`, and its Linux `-g` Release flag.
