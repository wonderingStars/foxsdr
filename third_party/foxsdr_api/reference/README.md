# FoxSDR engine API - reference material, vendored

Copied **unmodified** from the `foxsdr-api` repository, commit
`8fb6522027cb580725fc2707355c5541324fe41e` (API 0.2, draft) - the same commit
`../foxsdr_api.h` comes from. Vendored 2026-09-29 by the owner's decision, so
that stage 4 of the engine extraction (docs/engine-stage4.md) can be built and
checked against the specification and the reference behaviour, not guessed.

| Here | From foxsdr-api | What it is |
|---|---|---|
| `docs/API.md` | `docs/API.md` | The command reference, lists, streams, the Transmitter rules ("THE KEY"), the coverage table |
| `docs/TRANSPORTS.md` | `docs/TRANSPORTS.md` | The remote transport and session rules (stages 5-6) |
| `docs/ENGINE-EXTRACTION.md` | `docs/ENGINE-EXTRACTION.md` | The extraction plan: stages 1-7 and the performance gate |
| `mock_engine/` | `src/mock_engine/` | The reference engine: tickets, BUSY, merging, results in ticket order, the latch protocol |
| `tests/` | `tests/` (five files) | The mock engine's conformance tests (`test_mock_engine`, `test_transmit_safety`, `test_merge_exhaustive`) and their helpers |

Nothing here is built by this repository's CMake. It is reference material:
stage 4 ports the conformance tests to run against the real engine, and that
port lives under `tests/` like any other test.

Licence: PolyForm Noncommercial 1.0.0 (the files' own SPDX lines), the same
licence as this application. `../foxsdr_api.h` itself is MIT.

Do not edit these copies. A new API version is vendored by copying the files
again from the commit that defines it and updating the commit above.

ONE EXCEPTION, 2026-11-10 (multi-radio engine, stage 3; docs/multi-radio-records/engine-stages-1-3.md): the additive "several radios" vocabulary was
written into `../foxsdr_api.h`, `docs/API.md` (6.18), `mock_engine/mock_engine.cpp` and `tests/test_mock_engine.cpp` (`testRadios`) here, because this is
where the specification and its reference engine live. The mock includes three headers of the `foxsdr-api` repository that are not vendored
(`common/audio_ring.hpp`, `common/seqlock.hpp`, `common/struct_io.hpp`), so nothing here builds in this repository; the edit was compiled and run against
three small stand-ins for them (in the build folder, not committed): 393 checks, 2 failed - the two version-gate checks at test_mock_engine.cpp:30 and :33,
which expect API 0.2 and fail against the 0.3 header already vendored here, before this change.
