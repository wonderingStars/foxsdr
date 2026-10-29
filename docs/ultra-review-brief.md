# Ultra-review brief — branch `engine/stage4` (2026-10-29)

This brief is for the deep multi-agent review (`/code-review ultra`) of the engine-extraction
branch before it merges home to master. Run it from this worktree with `engine/stage4` checked
out. The branch is up to date with master's 0.99.100 release (merge `0c044e3c`); master has
since moved ~5 commits (the 0.99.101 pipeline-ring rewrite), which is NOT in this branch and
will be a final small catch-up at merge time — do not report its absence as a finding.

## What this branch is

The staged extraction of the FoxSDR engine from its ImGui window. 214 commits over the
0.99.100 base; 762 files changed; 715 registered tests (headless + a GUI gate that runs on a
hidden desktop). The end state:

- The GUI is a **client of the C API table** (`third_party/foxsdr_api/foxsdr_api.h`). The old
  residue surface `engine_shell.hpp` is **deleted**; a compile fence
  (`cmake/interface_fence.cmake`) denies every engine header to `src/gui`, and
  `tests/test_engine_shell_guard.cpp` pins the boundary at 0/0/0/0.
- **`foxsdr-engine`**, a headless binary sharing process boot with the app (`ProcessKind`
  gates), with config locking, orderly bounded shutdown (wedged = exit 3), and web/CAT
  provisioning flags.
- The engine owns the web and CAT servers as **internal sessions**; remote ops are
  capability-gated, and several ops are LOCAL-only by design (see Security surfaces).
- The **pass free-runs on the engine's control thread** (frame-kick from the window, timer
  fallback); the window drains answers once per frame through the uiState_/uiFact/uiSubmit
  ports.
- The plugin system is **fault-contained** (see the top review focus below).
- A **per-thread watchdog** (`src/core/thread_beats.*`): every long-lived worker heartbeats;
  a wedged thread files a hang report naming it.

The running record is `docs/engine-stage4.md` (§6.8–§6.30 cover everything on this branch,
each section with its mutant/RED table). The API spec deltas are `docs/api-0.3-additions.md`.
The four master catch-up merges have ledgers: `docs/engine-merge-0.99.84.md`, `-0.99.90.md`,
`-0.99.97.md`, `-0.99.100.md` (every master commit classified carried / re-expressed / n/a).

## Review focuses, in priority order

1. **Plugin containment** (`src/core/plugin_fault.*`, `plugin_calls.hpp`, `plugin_marker.*`,
   `plugin_module_check.cpp`; records §6.28–§6.29). In-process third-party native code is the
   riskiest surface in the product. Review: the SEH guard's foreign-image fault predicate
   (what it absorbs vs. what must still kill the process — host faults, stack overflow); the
   quarantine path (instances put down via the life directive, module left mapped, a held TX
   key released FIRST); the crash-loop breaker's memory-mapped marker and strike rules (the
   app's own crash must never cost a plugin a strike); call budgets riding the existing hang
   watchdog (lock-free table — check its memory ordering); guarded reads of plugin-lent
   memory after a call returns (the P2 "F1" fix — pixels, vertices, strings); load-time
   validation (PE/ELF, entry export, declared ABI, sha256). Adversarial evidence is
   `tests/fixtures/hostile_plugin.cpp` + `test_plugin_hostile*`, `test_plugin_misuse`,
   `test_plugin_fault*`, `test_plugin_crashloop`, `test_plugin_marker`,
   `test_plugin_validate`, `test_plugin_containment`. Known, recorded gaps (not findings): the
   hot signal path (process/pull/active/retune) is unguarded by design; POSIX runs guarded
   calls unguarded; panel/instrument titles and basemap attribution are re-read from plugin
   memory after load, unguarded; a thread that outlives destroy() silently is undetected; a
   DllMain-detach fault is contained by the loader, not the guard; freeze reports cap at
   eight wedges per session (counting continues).
2. **Transmit key / arming rules** (`src/engine/fox_engine_api.cpp`; §6.12-era 5d rules plus
   the B13 TX-plugin window and P1's quarantine-unkey). Anything that can leave RF keyed
   when its owner is gone is the worst defect class in the product. The owner has a
   post-campaign plan to model-check these rules (TLA+); the review should flag any rule that
   is ambiguous enough to need it sooner.
3. **Thread architecture and shutdown ordering.** The pass on the control thread, the
   frame-kick handshake, EngineService start/stop, bounded stops, the per-thread watchdog's
   beats registry, and static-destruction order (two exit-time UAFs were already found and
   fixed on master; P1 deliberately leaks the static bridge store to avoid a third — judge
   that call). TSan evidence exists for the plugin and thread-beats suites (WSL/GCC); the
   rest of the suite has had TSan passes at earlier stages but not after the final merge.
4. **The merge re-expressions.** Four ledgers map every master change into the branch's
   architecture. Spot-check the re-expressed halves against master's intent, especially the
   0.99.100 issue-report system (the branch files issue reports from its five plugin/thread
   counters through master's hook) and the 38-word health vocabulary union (Worker, app,
   PRIVACY, DIAGNOSTICS and tests must agree; `telemetry-worker/worker.test.mjs` is 60/60).
5. **Security surfaces.** LOCAL-only ops and their locks: `FOXAPP_OP_PLUGIN_REENABLE`
   (0x870E, four locks), `FOXAPP_OP_IMAGE_PRESS` (0x870F), the plugin-UI port
   (`get_local_extension`, capability bit 29, LOCAL sessions only). A remote web/CAT session
   must not be able to re-enable a quarantined plugin, press plugin UI, or widen its
   capabilities. Web password provisioning never rides argv.
6. **The C API table itself** (`foxsdr_api.h` + `docs/api-0.3-additions.md`): lifetime and
   threading contracts of list/fact/op entries, string ownership at the boundary, and whether
   any entry added late (stages 5–7, B-batches, P-batches) breaks the conventions the early
   entries set. The table is private (single consumer) — style findings matter less than
   lifetime/UB findings.

## Known issues — do not re-report

- Flakes, recorded: `test_look_order` (~13% solo, pre-existing), `test_usage_diag_window_free`
  (timing assumption, ~3/46), `test_plugin_audio` under TSan (race in its own fake).
- The Worker vocabulary additions are **not deployed**; deploy precedes any release of this
  branch (owner-gated). The site's reports API has not been shown to accept the new upload
  fields (`blamed-plugin`, `blamed-plugin-entry`).
- Linux evidence is WSL2 only; macOS is not built on this branch; real Pi/arm64 UNVERIFIED.
- Stage 6 (remote transport/DLL loader) is deliberately absent — the engine is private.
- The ImGui window remains the shipped GUI; the Slint port is deferred and out of scope.
- Four of the six relocated health-word counting sites are pinned by the counting-site table
  in `test_health_paths`, not by end-to-end scenarios (recorded in the 0.99.100 ledger).

## After the review

Findings land as fixes on `engine/stage4` with the house discipline (RED-first, mutants,
full suite, hidden-desktop GUI gate). Then: merge home to master → beta channel only →
crash-free + working-session metrics decide promotion out of beta.
