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
