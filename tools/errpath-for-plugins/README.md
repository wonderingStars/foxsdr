# The error-path gate for the plugins repository

The owner's rule: nothing may fail in any install without him being told. The gate is a build test that
reads `src/` as text, finds every warning, error assignment and swallowed failure, and fails when one
is neither reported, nor allowed with a reason, nor on the pinned PENDING list. This folder is the engine
repository's scanner, vendored so the plugins repository can run the same gate. Nothing in here writes
into that repository until you copy it; nothing needs the engine, the host or a network.

Files (all under `tools/errpath-for-plugins/` in the engine repository; copy them to `tools/` in the plugins repository):

| File | What it is |
|---|---|
| `errpath_scan.hpp` | the scanner (one header, standard library only). Byte for byte `tests/errpath_scan.hpp` of the engine: the engine's `test_error_paths` fails if the two differ |
| `errpath_check.cpp` | a small program around it: `errpath_check <root> [--allow f] [--reporters f] [--pin-file f] [--dump out]`; exit 0 green, 1 red |
| `errpath_reporters.txt` | the calls that count as a report: `FOXPLUGIN_REPORT`, `FOXPLUGIN_CATCH_ALL`, `report_fault` (the host call for companion programs). Edit when the host offers its report call |
| `errpath_seed.py` | builds the allow list from the scanner's output and the survey's verdicts; re-runnable (no line numbers) |
| `errpath.cmake` | the CMake lines that build and register the gate |

## The three steps to adopt it

1. **Copy and build.** Copy the five files above into `tools/` of the plugins repository, add `include(tools/errpath.cmake)` to
   its top-level `CMakeLists.txt` (after `enable_testing()`), and build `errpath_check` (a few seconds; no plugin is compiled).
2. **Seed.** From the plugins repository root:
   `python tools/errpath_seed.py --scanner <build>/errpath_check.exe --repo . --survey <error-paths.tsv> --survey-repo plugins --out tools/errpath_allow.tsv --write-pin --pin-file tools/errpath_pin.txt --scanner-arg=--reporters --scanner-arg=tools/errpath_reporters.txt`
   Every site that no reporter covers becomes an `ALLOW` line (with the survey's reason) or `PENDING:<group>`
   (PL1 host-log warnings, PL2 companion programs, PL3 ABI-boundary catch-alls, PL4 network outputs and uploads). The tool prints
   what it could not match; classify those by hand. Without `--survey` file every unresolved site is `PENDING:unmatched`.
3. **Register.** `ctest -R errpath_check` now runs the gate: GREEN at the seeded state, RED as soon as a new warning, error
   assignment or empty `catch` appears with no report and no allow line. Add it to the per-plugin GitHub workflow with the
   same command. A lane that adds a report deletes the site's PENDING line and lowers `tools/errpath_pin.txt` by one
   (`errpath_seed.py --write-pin` does both after a build; it refuses to raise the pin without `--allow-raise`).

Until the host offers a plugin report call (the engine's `report_fault`, extended with a plugin-exception kind, declared in the
plugins repository's `plugin_abi.h`), no plugin site can pass by a reporter: the gate runs in PENDING-only mode. Define
`FOXPLUGIN_REPORT(kind, code)` and `FOXPLUGIN_CATCH_ALL(id)` over that call, and sites that use them pass at once.

## What a site is (plugins profile)

`plwarn` a `CASCADE_LOG_WARN` / `CASCADE_LOG_ERROR` handed to the host log (the host shows it, does not collect it);
`swallow` a `catch` whose body is empty once comments, log calls and bare `return;` / `continue;` are removed
(the ABI-boundary catch-alls); `errassign` an assignment to an error field (`err`, `error`, `lastErr`, `*Error_` ...) that is
not a clear (`""`, `{}`, `'\0'`, `0`); `stderr` a write to stderr. Vendored code under any `third_party/` and any `test/` or
`tests/` folder is not scanned.
