# Wave 2: mr/engine-stage4 (ee054680) merged into engine/multi-radio (78667639)

Date 2026-11-11. Worktree foxsdr-wt/mr-dsp, build D:/foxsdr-build/mr-dsp. Nothing pushed.

## Conflicts
* docs/api-0.3-additions.md: both sides appended a section 9.20. Both kept; the AGC/blanker section stays 9.20, the stage 4 section (several radios, RADIO_ADD/REMOVE/AUDIO/SOLO) is renumbered 9.21.
* src/engine/engine.cpp, four hunks (state fill, applyConfig, saveConfig-side config fill, the startup mirror push): stage 4's rx().name structure kept, the AGC/blanker lines re-expressed against rx() (they were written against the old slot-0 aliases and receivers_[0]). The state fill now reads the SELECTED radio's seven fields, including the live figures from that radio's own pipeline (agcGainDb, nbBlankedPct, nbStandingDown). Config load/save and the startup push act on rx(), which is slot 0 there: saved settings stay radio 1 only (per-radio saving is stage 5).

## Changes beyond the conflicts
* The SET_AGC and SET_NB handlers picked their receiver with pickRadio(op, target, focus = 1): a command with no target but a session focus on radio 2 would have landed on radio 1. applyCommand already selects the right slot (RadioWork, using the sender's focus), so both handlers now use rx().
* tools/multi_radio_slot_rewrite.py re-run on engine.cpp, engine_*.cpp, engine.hpp: 0 changes (the merged code uses no slot-0 alias). Remaining receivers_[0] uses outside declarations: two sourceGen_ reads (a target-0 fallback), unrelated to AGC.
* tests/test_engine_api_multi_radio.cpp: testAgcAndBlankerArePerRadio.

## Per-radio proof
Two generator radios. SET_AGC/SET_NB aimed at radio 2 change radio 2's agcMode, nbEnabled, nbThresholdDb and modeSeq and leave radio 1 at the defaults with its modeSeq unchanged; the reverse; a target-less command follows the session focus; read_radio_state(1) agrees with the focused read; a removed and re-added radio 2 starts at Auto / 0 dB / off / 15 dB. Break-it: the state fill reading receivers_[0] gave 11 failures (radio 2 reported radio 1's values); SET_AGC ignoring the target gave 8 failures (radio 2 stayed Auto, radio 1 changed). Both restored.

## Shared surfaces read
* foxsdr_api.h: SET_AGC 0x0111, SET_NB 0x0112, RADIO_ADD..SOLO 0x1202-0x1205, no op number defined twice; the seven FoxReceiverState fields are appended after rxLonDeg, FOXAPI_MIN_RECEIVER_STATE unchanged.
* app_commands.cpp op-name table has all six ops; radio_scope.hpp lists SET_AGC/SET_NB as receiver-scoped.
* lang_assets.hpp: tools/embed-lang.py output equals the committed file apart from line endings; the committed file kept.
* Gap not closed: third_party/foxsdr_api/reference/docs/API.md describes SET_AGC/SET_NB (lane C) but has no entry for RADIO_ADD/REMOVE/AUDIO/SOLO (stage 4 never added one); docs/api-0.3-additions.md 9.21 is their only spec.

## Gates
See the commit message of the merge and the report: build exit 0; headless|slint 742 of 749 on the first run, the 7 being 5 known D: false failures (pass run from the worktree) and test_tx_audio and test_usage_diag_window_free, which pass alone; gui on the hidden desktop 227 of 227; ledger 2148 rows, 0 unclassified.
