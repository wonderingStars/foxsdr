# Memory-error and fuzz testing

Two things the ordinary build and the ordinary suite cannot do: notice a memory
error that does not (yet) change a result, and feed the parsers input nobody
wrote a test for. Both are optional builds; neither changes anything a user
receives.

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

## CI

`.github/workflows/sanitize.yml` (separate from `build.yml`, so it can neither slow
nor colour the release jobs) has two jobs: the whole suite under ASan + UBSan with
GCC, and the fuzz targets under Clang - the corpus replayed first, then each target
for a short fixed time (`fuzz_seconds`, default 60). Start it on a branch from the
Actions tab or with `gh workflow run sanitize.yml --ref <branch>`.
