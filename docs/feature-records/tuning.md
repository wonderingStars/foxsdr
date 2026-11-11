# Engine lane `tuning` - record

Branch `feat/tuning`, created from engine/multi-radio d3630d78. New-file model lane: every model is a header under `src/core/` or
`src/dsp/` with its test under `tests/`; no existing source file is edited (the tests are globbed, so `tests/CMakeLists.txt` needed no edit
either). Nothing is wired; the wiring hand-off at the end says exactly where each model plugs in.

## 1. Step 0 - the table (reading only, written before any code)

Searched: the base (d3630d78) and the branches `feat/screen-c`, `feat/recording-models`, `feat/signal-blocks-3`, `feat/signal-blocks-4`,
`feat/screen-b`, `feat/web-bookmarks`, `feat/knob-input`. Those branches are not in this lane's base, so a model that exists only there is
"built elsewhere" (this lane does not copy it); a gap they only partly cover is built here as a new, separate file.

| # | Row | Verdict | Evidence / what this lane builds |
|---|-----|---------|----------------------------------|
| S1 | Scan trigger level separate from the demod squelch, dynamic noise floor | new file model | `feat/screen-c` has per-entry thresholds (C1) against a level, but a FIXED dB; no floor tracking. Built: `core/scan_trigger.hpp` (fixed or floor-relative, per slot, hysteresis). Base `noise_floor_tracker.hpp` is for the squelch's automatic threshold and is not per scan slot. |
| S2 | FFT scan across the captured span without retuning | new file model | Nothing in the base or branches. Built: `core/fft_scan.hpp` (one spectrum frame -> hits per scan channel, floor from the frame itself). |
| S3 | Several search ranges in one scan | new file model (hand-off for the form) | `ScannerParams` holds one range or one `list`. Built: `core/scan_plan.hpp` expands up to 16 ranges into the existing `list` mode. |
| S4 | Scanning backwards / stepping back | new file model | Scanner has no direction (`scanner.hpp` is forbidden). Built in `scan_plan.hpp`: direction, and a cursor with `stepBack`; the plan hands the scanner an already-reversed list. |
| S5 | Save a scan hit as a bookmark | new file model | Built in `scan_plan.hpp`: `bookmarkFromHit` (name, mode and bandwidth from the range the hit lies in; duplicate refused). |
| B1 | Notes / comment / tag field on a bookmark | new file model | `core/bookmark.hpp` is data shared with the engine API; not edited. Built: `core/bookmark_notes.hpp`, a sidecar keyed by frequency + mode. |
| B2 | Favourites that save the whole state | new file model | `Bookmark::favourite` is only a flag. Built: `core/favourite_state.hpp` (display range, spectrum range, waterfall contrast, DSP, versioned JSON). |
| B3 | Marker database edited and exported separately | new file model | `core/freq_markers.hpp` is the store (note edit exists); no export/import. Built: `core/marker_export.hpp` (CSV + JSON out, strict import, merge). |
| B4 | Custom CSV columns overlaid from a station database | new file model | `core/freq_import.hpp` imports CHIRP-like rows into bookmarks; no overlay of foreign columns. Built: `core/csv_overlay.hpp`. |
| T1 | Per-band memory: volume, AGC, attenuator, spectrum limits, zoom, antenna | new file model | `engine.cpp:6579` says nothing is stored per band. `feat/recording-models` `operating_aids.hpp` has band-stacking registers (frequency/mode/bandwidth, three per band) and a per-MODE memory (bandwidth, step, AGC): a different record. Built: `core/band_memory.hpp`. |
| T2 | Frequency back/forward history (16 entries) | new file model | Not in the base or any searched branch (`operating_aids.hpp` has VFO A/B, not a history). Built: `core/freq_history.hpp`. |
| T3 | RIT/XIT clarifier on the spectrum; CAT max 0 | new file model + hand-off | `src/net/cat_protocol.cpp:192-193` reports max RIT/XIT 0. Built: `core/rit_xit.hpp` (offsets, limits, spectrum marker positions, CAT numbers). The CAT change itself is a hand-off. |
| T4 | Numbered channel plans (PMR446, CB, FRS/GMRS, MURS, marine VHF) with a channel mode | new file model | Built: `core/channel_plans.hpp` + a channel cursor. Tables typed from the regulators' pages (section 6). |
| T5 | CW stereo placement by pitch | new file model | `feat/signal-blocks-4` `dsp/stereo_place.hpp` places by pan/azimuth, not by pitch. Built: `dsp/cw_surround.hpp` (pitch estimate -> pan, constant-power). |
| T6 | 1750 Hz tone-burst key | new file model (tone only) | Built: `dsp/tone_burst.hpp`, a tone generator with attack/release only. Keying is NOT built (owner decision). |
| T7 | Launching companion programs | new file model, OFF by default | Built: `core/companion_launcher.hpp` (validation, quoting, policy, injected spawner). Real spawn is a hand-off (owner decision). |
| C1 | Shared preset and bookmark packs | new file model (format only) | Built: `core/bookmark_pack.hpp`. Frequency-database DOWNLOADS are an outside service: NOT built, listed. |
| C2 | Keyboard actions (bandwidth up/down, cycle step, cycle mode, centre signal, next/previous bookmark) | new file model + hand-off | `gui/key_bindings.cpp` is lane C's. Built: `core/key_actions.hpp` (action list + handler functions). |
| - | N1MM/DXLog spot feeds | not this lane | plugin lane `dxspots2`. |

Rows that need a forbidden file only for the last step (wiring): S1-S5 (engine scanner/pipeline), T1 (engine band switch), T2, T3 (CAT,
engine NCO), T4-T7, C2: each has its standalone model here and a hand-off section below. No row is `blocked`; none was already built in
a form that makes the new file redundant.
