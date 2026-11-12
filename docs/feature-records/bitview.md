# Bit view library (engine lane `bitview`)

Branch `feat/bitview` (worktree `D:\foxsdr-wt\en-bitview`, cut at `d3630d78`, engine/multi-radio). Nothing pushed, nothing merged.
Written 2026-11-12. NEW-FILE MODEL LANE: a host-side, plugin-independent, header-only library in `src/core/bits/` and eight test programs
in `tests/`. No existing file was edited (`engine.cpp`, `pipeline.cpp`, `receiver.hpp`, `demod.cpp`, `plugin_abi.h`, `fox_engine_api.cpp`,
the recorder, every `.slint` file and `tests/CMakeLists.txt` are untouched: the test list is a glob, so no list entry was needed).
Versions: none changed (no versioned file touched).

## Step 0: the rows, checked against what exists (reading only)

| Row (REMAINING-GAPS / DEPTH-GAPS Part B) | What already exists | Verdict |
|---|---|---|
| Bit view with line-code decoders (NRZ, Manchester, differential, whitening) plus preamble and sync search | The Demod Analyzer plugin on the (unmerged) branch `depth/winlink-wb` has a BIT VIEW (`bitview.cpp`/`.h`, plugin-bound): NRZ, inverted, NRZI, Manchester, differential Manchester, each with and without PN9; sync search; CRC finder; frame diff (read from `D:/foxsdr-wt/pd-winlink-wb/docs/feature-records/depth-winlink-wb.md` and the header's first lines). `feat/plugins-p3` has `src/hfid` = MIL-STD-188-110 serial-tone preamble identification: not a bit view. | **new file model** (the host-side, plugin-independent library: ten line codes incl. Miller and bi-phase, polarity x clock-phase search, run-length path with jitter/clock/bias, generic LFSR and self-synchronising scramblers, tolerance search, three framings). The plugin's own view is its lane's; nothing here edits it. |
| Protocol field labelling, automatic field inference, CRC/checksum finder, message diffing | The plugin has a catalogue-based CRC finder (12 entries) and a diff. No field inference. | **new file model** (inference; an algebraic CRC finder that derives unknown polynomials; additive/XOR/Fletcher/Internet checksums; alignment diff). Labelling = the field list of the decoder file. |
| User-described decoder for unknown OOK/FSK devices | Plugin lane: "not built" (the pulse-width measurements exist). | **new file model** (own spec format, parser with line numbers, interpreter, OOK slicer). |
| Re-decode from a raw bit buffer; binary editor with code-table remapping (Code300-32) | Nothing. | **new file model** (bounded ring + re-decode chain; code tables with up to four shift planes and a user table file; editor model with undo). Code300-32 is a commercial HF decoder (HOKA); only its table-building feature (user alphabets on ASCII/ITA5, ITA2 with 2/3/4 shifts) is matched, from a search-result description; it was not run or opened. |
| Transmit a recorded I/Q file through the Pluto; fuzzing and protocol simulation | - | **owner decision, NOT built** / "probably out of scope", not built. |

## What was built

All in namespace `cascade::bits`, header-only, pure functions (no global state, no I/O, no prints, no identifiers kept).

| File | Contents |
|---|---|
| `bits.hpp` | `Bits` (one byte per bit), string/byte conversion, field read, slice, Hamming, byte-wise reversal. |
| `linecode.hpp` | NRZ-L/M/S(NRZI), Manchester (IEEE, Thomas), differential Manchester (both senses), bi-phase mark/space, Miller: decoders with violation counting, encoders; `searchLineCodes` (polarity x clock phase, ranked); `estimateChipPeriod`/`fitChipPeriod` (grid search with a largest-period preference and least-squares refinement); `runsToChips`; pulse-width / pulse-position decoding with two-means width classes; `searchLineCodesFromRuns` (T and T/2 readings, because Miller has no one-chip runs). |
| `whiten.hpp` | Generic Fibonacci LFSR additive whitening (bit and byte forms; PN9 preset), self-synchronising scramblers (G3RUH x^12+x^17, V.27ter, V.29 tap pairs), `applyWhitening` for the chain. |
| `sync.hpp` | Preamble/sync-word search with Hamming tolerance and inverted polarity; preamble measurement; framing by fixed size, by length field and by flag with HDLC bit stuffing; encoder for the tests. |
| `crc.hpp` | CRC engine (width 1..64), 18-entry catalogue as data, GF(2) polynomial arithmetic, `findCrc` (catalogue match; algebraic derivation of unknown polynomials; init/xorout by a GF(2) linear system with the honest number of undetermined dimensions), `findChecksums` (sum8, two's-complement, XOR, XOR from 0xFF, 16-bit byte sum, nibble sum, Fletcher-16 mod 255/256, Internet checksum). |
| `msgdiff.hpp` | Bit alignment diff (Needleman-Wunsch), aligned rows with markup, difference runs, per-column statistics, hex byte diff. |
| `fields.hpp` | Field inference: constants, varying bytes, counters (with missed-frame tolerance), length fields, few-valued flags, checksum position. |
| `ookslicer.hpp` | Envelope to level runs in microseconds; `balanceRuns` (mark/space bias compensation); `despeckle`; `iqMagnitude`. |
| `bitspec.hpp` | The user-described decoder: format `foxsdr-bitspec 1`, `parseSpec` (refusals with line numbers, limits), `runSpec`. |
| `rawbuf.hpp` | `RawRing` (bounded, thread safe), `redecode` chain (balance, chip clock, up to three stages, invert, whitening, sync, framing), `redecodeAll`, `sweepSettings`. |
| `bitedit.hpp` | Code tables (ASCII-7/8, ITA5, ITA2, ITA2-US, SIXBIT, AIS 6-bit) with shift planes, user table file `foxsdr-bittable 1` (`parseTable`), `remapSymbols`, `BitEditor` (range-replacement edits, exact undo/redo, bounded history, dirty tracking, byte view). |

### How the CRC finder works (written from the CRC textbook, no other tool consulted)
For frames of equal length the CRC is affine over GF(2), so `E(x) = (a^b)(x)*x^w + (ca^cb)(x)` is divisible by the generator for every pair. The gcd
over all differences is the generator when its degree is exactly `w` (after dividing out factors of x: a generator is odd). A bigger gcd is
resolved by removing odd cofactors up to degree 16 and reporting EVERY degree-`w` divisor with the count (`candidates`). Init and final XOR come
from a linear system over GF(2), one block per frame: frames of one length leave exactly `w` dimensions undetermined (init and final XOR are
interchangeable at that length; the hit says so and gives a catalogue name when one reproduces all frames), frames of two lengths usually
settle it. Deriving needs at least two independent differences (three frames of one length).

### The decoder file (user-described decoder), example that decodes the real Oregon recording
```
foxsdr-bitspec 1
name oregon-v21-thgr122n
coding chips manchester
stage manchester            # every bit is sent twice (as itself and inverted): Manchester inside Manchester
bit_us 976
tolerance 30
reset_us 5000
invert auto
preamble 1111 1111 1111 0101
preamble_errors 1
min_bits 68
max_bits 240
field sensor_type at 0 16 hex nibble_lsb
field channel at 16 4 uint nibble_lsb
field rolling_id at 20 8 uint nibble_lsb digits_lsd
field temperature at 32 12 bcd nibble_lsb digits_lsd scale 0.1 unit C
field humidity at 48 8 bcd nibble_lsb digits_lsd unit %
check nibblesum from 0 to 60 at 60 8 nibble_lsb digits_lsd
```
The full statement list is in the header comment of `bitspec.hpp`.

## Tests, with real result lines

Own tests, official MSVC Release build in `D:/foxsdr-build/en-bitview` through ctest (`ctest -R "^test_bits_"`):

```
test_bits_crc: 879 checks, 0 failed
test_bits_fields_diff: 86 checks, 0 failed
test_bits_fuzz: 405091 checks, 0 failed
test_bits_linecode: 98 checks, 0 failed
test_bits_rawbuf_edit: 127 checks, 0 failed
test_bits_real: 31 checks, 0 failed
test_bits_spec: 810 checks, 0 failed
test_bits_whiten_sync: 81 checks, 0 failed
100% tests passed, 0 tests failed out of 8   (about 13 s in all)
```
(The programs also compile and pass standalone with MinGW g++ -std=c++20 -Wall -Wextra without warnings; the MSVC build shows no warning in `src/core/bits`.)

Address sanitizer (MSVC `/fsanitize=address`, the asan runtime folder on PATH, all eight programs compiled standalone, run without a dialog):
```
test_bits_linecode: 98 checks, 0 failed
test_bits_whiten_sync: 81 checks, 0 failed
test_bits_crc: 879 checks, 0 failed
test_bits_fields_diff: 86 checks, 0 failed
test_bits_spec: 810 checks, 0 failed
test_bits_rawbuf_edit: 127 checks, 0 failed
test_bits_fuzz: 405091 checks, 0 failed
test_bits_real: 31 checks, 0 failed
(no AddressSanitizer report in any run log)
```

Key pinned numbers (printed by the tests):
* Line codes through jitter / clock error (200 frames of 64 bits each): jitter +-0.20 T and clock +-3 %: NRZ-L >= 97 % (long runs; measured 200/200), Manchester, differential Manchester, bi-phase mark, Miller 200/200. Manchester at jitter 0.24 T: 300/300 frames exact; 0.35 T: 3/300 (bit error rate 0.49: one slipped run garbles the rest); 0.60 T: 0/300.
* CRC finder: the catalogue definitions recovered from 3, 4 and 5 frames in both byte orders: **66/66** (11 definitions: SMBUS, MAXIM-DOW, ROHC, ARC, MODBUS, KERMIT, IBM-3740, X-25, ISO-HDLC, BZIP2, OPENPGP; widths 8, 16, 24, 32). Random definitions NOT in the catalogue (width 8/16/32, random odd polynomial, init, xorout, reflection; five frames of one length): polynomial found **36/36**, a held-out frame of the same length reproduced 36/36, 48 unique claims all right. Two lengths: init and xorout determined exactly in 6/20 (it needs (1 + x^d) coprime to the generator; the rest report their free dimensions). Random frames: 0 false hits in 40 sets (widths 16 and 32) and in 100 sets (width 8). Two frames: nothing is derived.
* Spec decoder, pulse-width device (CRC-16, 150 sets x 3 repeats): clock +8 %, width jitter +-15 %: 150/150 complete, 0 wrong; clock -10 %, jitter +-25 %: 16/150 complete (beyond the 30 % class tolerance), **0 wrong** (the check refuses what the widths damaged); jitter +-90 %: 0 accepted. Manchester-inside-Manchester device with a 110 us mark/space bias, +-12 % edge jitter, +-2 % clock: 80/80, 0 wrong.
* Real data, `D:/foxsdr-samples/weather-decoder` (local, not committed; 433.92 MHz OOK, 250 kS/s assumed by the folder): `os21-thgr122n-188-54`: the spec above decodes **both** packets the independent reference lists (rolling id 248, channel 1, 18.8 C, 54 %, type 0x1D20, nibble-sum check good) at 57.2 ms and 297.5 ms (reference 0.057164 s and 0.297504 s). A bare spec without the field layout finds the reference message count (2) in `os21-bthr968`, `os21-thgr122n-m550-30`, `os21-thn132n-202` and `os3-bthgn129`, each with a nibble-sum check closing every frame; **it finds 0 of 2 in `os3-rtgn318` and 0 of 1 in `os3-wgr800x`** (different v3 frames the v2.1 preamble does not match; not decoded). The decoded count equals the reference count wherever it decodes; the field values were compared with the reference only for the first file. The existing plugin's decode count was not run here (the plugin lives in the plugins repo; the reference decode printed beside each file is the comparison).
* Fuzz (`test_bits_fuzz`, 405,091 checks, 4.5 s): 8000 mutated or random spec texts (91 accepted, 7909 refused, never more than 32 errors, same text same verdict), 6000 table texts, accepted specs over hostile run lists (NaN, infinity, 1e15, negative, level values over 1), every line code with random phase and start level, whitening with random LFSR specs, pattern search and framing with random parameters, the slicer, the re-decode chain with random settings, CRC and checksum finders with hostile options, field inference, diff, hostile CRC models, 4000 steps of the editor against a plain model.

## Break-it table (mutation, first red check; each restored and re-run green)

| # | Component | Mutation | First red check |
|---|---|---|---|
| 0 | Manchester | IEEE reads the first chip | `dec.bits == d0` |
| 1 | Miller | boundary rule inverted | `dec.violations == 0` |
| 2 | Bi-phase mark | reads the wrong half | `dec.bits == d0` |
| 3 | Period fit | smallest period instead of the largest within tolerance | `ok24 == 300` |
| 4 | Period fit | no least-squares refinement | NRZ-L jitter floor |
| 5 | PN9 | tap moved to stage 4 | first sixteen bytes `got == want` |
| 6 | G3RUH | delay 17 -> 16 | the defining relation `rel` |
| 7 | Descrambler | uses its own output history | `descramble(sc) == data` |
| 8 | Sync search | tolerance off by one | `found` |
| 9 | Framing | stuffed bit not removed | `exact >= bodies.size() - 1` |
| 10 | Framing | length past the stream not refused | `rej2 >= 1` |
| 11 | CRC engine | input reflection removed | catalogue check value |
| 12 | CRC engine | output reflection removed | catalogue check value |
| 13 | CRC finder | factors of x not stripped | `polyOk == tried` (found only after the fuzz of the derivation showed 6 % misses) |
| 14 | CRC finder | derivation from one difference allowed | `!h.derived` |
| 15 | CRC finder | catalogue hit if ANY frame matches | `reproduces(...)` |
| 16 | Fletcher | mod 255 -> 256 | `0xC8F0` |
| 17 | Fields | counter share check removed | the counter field |
| 18 | Fields | counters over constant bytes | the constant field |
| 19 | Diff | mismatch scores +1 | `d.gaps == 1 && d.mismatches == 0` |
| 20 | Spec | first preamble hit instead of the best | **survived the first round**; killed by a near-miss-then-real-preamble test |
| 21 | Spec | bad check accepted | `wrong == 0` |
| 22 | Spec | duplicate field names allowed | `!p.ok` |
| 23 | Spec | pwm balance default left on | pulse-width decode rate (found by the test, then made a rule) |
| 24 | Spec | invert auto never tries the inverted stream | real recording `msgs.size()` |
| 25 | Slicer | balanceRuns bias sign flipped | real recording `msgs.size()` |
| 26 | Ring | eviction by count off by one | `ring.size() == 4` |
| 27 | Re-decode | invert ignored | `redecode(si, inv)` |
| 28 | Re-decode | a frame runs to the end of the stream | fuzz frame bound |
| 29 | Editor | redo branch kept after a new edit | `!e.canRedo()` |
| 30 | Editor | undo restores the wrong range | **survived the first round** (a later equal-size edit repaired it); killed by single length-changing undo/redo checks (rc 3221226356: the mutant corrupts the heap) |
| 31 | Tables | shift symbols printed | `"RY34E"` |

Defects found by the work itself and fixed in the library (not mutations): the period estimator drifted low under jitter (replaced by a grid search; Miller needs both T and T/2 readings); the CRC derivation missed a generator whenever every sample difference shared low zero bits (strip factors of x); `redecode` with a sync pattern and no framing copied the rest of the stream for every sync hit (a hostile 262k-bit slice took up to a quarter second per call: frames now end at the next hit or 4096 bits, at most 1024 frames, slices cut at 2^18 chips); a NaN run duration was cast to `size_t` (undefined behaviour).

## Gates

Whole tree built (`cmake --build D:/foxsdr-build/en-bitview --config Release --parallel 3`, 614 targets, no error; configured with `-DCASCADE_BUILD_SLINT=OFF -DCASCADE_SOFTGL=OFF`), then the headless set `ctest -C Release -L headless` in four slices
(`-I 1,110`, `111,330`, `331,560`, `561,800`; `-j 3 --timeout 240`; 711 headless tests; the machine ran at full load from the other lanes the whole time).
`git diff d3630d78 --stat`: 20 files changed, 5829 insertions, no deletion: every change is a new file, none is linked into any other test.

Failures, each re-run alone once (the brief's rule), and what they are:

| Test | Result alone | What it is |
|---|---|---|
| `test_airband_data`, `test_band_plan`, `test_converter_call_sites`, `test_page_resize_id`, `test_png_decode` | red | the known false failures of a D: build (named in the standard rules) |
| `test_window_failed_app` | red (needs the software-OpenGL DLLs: `opengl32.dll`, `libgallium_wgl.dll`) | configuration: this build is `-DCASCADE_SOFTGL=OFF` (named in the standard rules) |
| `test_pass_control` | red in the slice | named load flake |
| `test_engine_control_thread`, `test_look_order`, `test_pump_live_traffic`, `test_soapy_enum_proc`, `test_soapy_source`, `test_fitted_modules_window_stepped`, `test_health_app_stepped`, `test_window_threaded_look` | PASSED alone | load flakes |
| `test_engine_api_decode_radio` (timeout), `test_engine_api_merge` (`unclean.load() == 0 (198 vs 0)`), `test_engine_api_transmit` (keyed-frame timing checks, 231 s) | red alone, under load | timing tests that judge time by 900 ms while the machine ran at 100 % (the merge test prints `a run outlasted the 900 ms the rules judge time by` hundreds of times); engine-features lane A's record shows them passing on a quiet machine; NOT re-run on a quiet machine here: UNVERIFIED |
| `test_engine_api_source_kinds` | red (0.07 s) | environment: the fixture `scope.wav` is not in `src/slint/tests/drive/fixtures` of this worktree |
| `test_usb_bounded_walk` | SEGFAULT, repeatable | a `src/usb` test with no file of this lane linked in; the same executable also segfaults in another lane's build (`D:/foxsdr-build/en-access`) and passes in `en-android`: not caused by this lane; not diagnosed |
| `test_crash_upload`, `test_crash_upload_free` | red alone (`srv.connections() == 1`, `bodies.size() == 1`) | a loopback stand-in server test; fixed-port collisions with the ~20 lanes running the same test are the likely cause; NOT verified on the base: UNVERIFIED |

Everything else passed (the four slices ran 110 + 220 + 230 + 151 = 711 headless tests). No failure touches a file of this lane.

Real `%APPDATA%/foxsdr/config.json` sha256 after the work: `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` (unchanged; the tests read no config and write no file).

## Wiring and screen hand-off (nothing here is wired; every file named below is forbidden to this lane)

The library has no thread of its own and no output. Wiring needs the engine (a slicer tap on a radio's magnitude stream that fills a `RawRing`, one ring per radio or per receiver) and a Slint page; both are other lanes' files. Proposed shape, for the wiring lane:

Engine facts/ops (names are proposals): `bits.rawslices` (fact: count, newest id), `bits.redecode` (op: settings below, answer: frames as hex plus bit text, violation count, chip period), `bits.sweep` (op: slice id), `bits.findcrc` / `bits.findsum` / `bits.infer` (ops over frame ids), `bits.diff` (op: two frame ids), `bits.spec.check` (op: text, answer: errors with line numbers), `bits.spec.run` (op: text + slice ids), `bits.table.check` (op: text).

Bit view page controls (English labels; range; default):
* "Line code": Auto / NRZ-L / NRZ-M / NRZ-S (NRZI) / Manchester (IEEE 802.3) / Manchester (G.E. Thomas) / Differential Manchester / Differential Manchester (inverted) / Bi-phase mark / Bi-phase space / Miller; default Auto.
* "Second stage" and "Third stage": None or any line code; default None. "Invert bits": off. "Clock phase": Auto / 0 / 1; Auto.
* "Chip period (us)": Auto or 1..1,000,000; Auto. "Balance mark and space": on.
* "Whitening": None / PN9 / G3RUH scrambler / V.27ter / V.29 / Custom LFSR; None. Custom LFSR: "Degree" 1..63, "Taps (hex)", "Seed (hex)", nonzero; Custom scrambler: "Delay A", "Delay B" 1..64, A < B.
* "Sync word (bits)": up to 64 bits, empty; "Allowed bit errors": 0..16, 0; "Also look for inverted": off.
* "Framing": None / Fixed size / Length field / Flag; "Frame bits": 1..8192; "Length field position", "Length field bits" 1..32, "Length unit (bits)", "Length adjust (bits)"; "Flag (bits)" 01111110; "Remove stuffed bits": on.
* Buttons: "Try all line codes", "Find CRC", "Find checksum", "Infer fields", "Compare two frames", "Re-decode", "Keep burst".
* "Kept bursts": 1..256, 64 (ring limits 256 bursts, about a million runs in all).
* Decoder file: "Load decoder file...", "Check file" (list: line number and message), "Run on kept bursts", "Save decoder text".
* Editor: "Show bits" / "Show bytes", "Code table" (ASCII-7, ASCII-8, ITA5, ITA2, ITA2 (US), SIXBIT, AIS 6-bit, "Load table file..."), "Undo", "Redo", "Flip bit", "Insert bits", "Delete bits", "Invert range", "Reverse range", "XOR range", "Remap symbols".

English strings the library returns (listed, not translated): every parser message in `bitspec.hpp` and `bitedit.hpp` (e.g. "the first statement must be 'foxsdr-bitspec 1'", "field 'x' is already defined on line 4", "a glyph is at most 8 characters"), the CRC hit notes ("reproduces the check value of every frame", "init and final XOR cannot be told apart from these frames (N free dimensions): ...", "N polynomials divide every difference: the frames do not pin the polynomial down."), the field notes ("every consecutive pair steps by 1", "N of M consecutive pairs step by 1 (frames missed?)", "equals the frame length in bytes"), and the re-decode problems ("the slice is empty", "between one and three decoding stages are needed", "the chip period could not be estimated (too few runs)").

## Sources opened (2026-11-11/12) and their status

* reveng.sourceforge.io/crc-catalogue/all.htm (three pages of text): parameters and check values only; the page shows no licence statement; the program and its source were NOT opened.
* amsat.org/articles/g3ruh/109.html: G3RUH scrambler 1 + X^12 + X^17, 17-bit register, each bit XOR the bits sent 12 and 17 earlier; (c) 1988 James Miller G3RUH, no licence stated.
* en.wikipedia.org/wiki/Baudot_code: the ITA2 table and the US-TTY differences (text truncated before the licence statement).
* sigrok.org/wiki/Protocol_decoder:Ook_oregon (wiki text; no code): v2.1 preamble 1010, "two copies of all the bits".
* espruino.com ... Oregon Scientific RF Protocol Description (PDF): fetched twice; the text is in a custom font encoding that neither the page reader nor my own extractor could read: NOT read.
* Seen only as search-result text (pages not opened): TI application note SWRA322 and the CC1100/CC1150 datasheet wording (PN9 x^9 + x^5 + 1, all ones, 8 LSBs per byte, one clock per bit); two US patents quoted for the V.27ter and V.29 scrambler polynomials; HOKA's Code300-32 product text (user alphabets); winlink-wb's feature record and `bitview.h` header lines (own repository, MIT).
* Not opened, by rule: reveng, rtl_433, Universal Radio Hacker, any other GPL/LGPL program or port.
* The Oregon fields were found with this library from the recording itself and checked against the reference decode (`expected.json`, printed by an independent decoder whose source was not read; licence of that test repository: none stated).

## UNVERIFIED

* The PN9 sequence FF E1 1D 9A ED 85 33 24 EA 7A D2 39 70 97 57 0A was recalled and matched by the implementation (a different tap, x^9 + x^4 + 1, gives another sequence, asserted); it was not read from a page.
* V.27ter (taps 6, 7) and V.29 (taps 18, 23): patent text via a search result only; offered as plain tap pairs.
* AIS 6-bit table (0..31 = '@'..'_', 32..63 = ' '..'?') from memory of ITU-R M.1371; ITA5 is the 7-bit ASCII layout, the national-use positions were not checked; CCIR 476 is not a built-in table (a user table file can carry it; no source opened).
* The Oregon v2.1 structure (Manchester inside Manchester, nibbles least significant bit first, sync nibble 0101, nibble-sum check covering the nibbles before the check byte) is what this recording shows; the official document was not read.
* The slicer's 250 kS/s and 433.92 MHz centre are the sample folder's assumptions.
* The coincidence figures of the CRC finder (`coincidence`) are rough estimates (layouts tried x 2^-width).
* Nothing is wired: no live signal, no CPU measurement in the receive path, no screen.
* Only the first sample file's field values were compared with the reference; the other files' message counts were.

## Owner decisions

1. The `foxsdr-bitspec` and `foxsdr-bittable` formats are this program's own; publishing them as documented formats is a product decision.
2. Keep the V.27ter / V.29 / AIS 6-bit presets until their sources are opened, or drop them? (They are labelled in the header comments.)
3. CCIR 476 as a built-in table needs a source the lane could open (patent-free alternative: the user table file, supported today).
4. Where the Bit view lives (plugin-first rule): this lane's library is host-side; the Demod Analyzer plugin keeps its own BIT VIEW.
