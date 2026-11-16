# Screen lane S5c (wave 8): the Fobos SDR and Perseus bridges (2026-11-16)

Branch `feat/screen-s5c`, cut from `c3efc203` (lane S5b's tip) in `D:/foxsdr-wt/en-rfmon`; build folder `C:/Users/steve/foxsdr-build/s5`. Nothing pushed. No radio, no COM port, no window was opened; no outside request, download or web search was made. Only the two approved header files were read (`D:\foxsdr-build\vendor-headers\fobos\fobos.h`, `...\perseus\perseus-sdr.h`); neither is copied into the tree. **Every proof is stand-in proof only; real radio UNVERIFIED.**

## Rows

| Row | Verdict | Proof |
|---|---|---|
| G1-07i Fobos SDR | BUILT (stand-in proof only; real radio UNVERIFIED) | `src/source/fobos_{api.hpp,source.hpp,source.cpp}`, key `fobos`, wire-b table row, saved-kind list, one needed-library row (`libfobos`), the choice in "Network and vendor radios" only after the row was checked and found. `tests/test_fobos_source.cpp`, 423 checks against the stand-in library `tests/fixtures/fobos_standin.cpp` (built as `fobos.dll`): open, list, serial/index pick, tune (rounded value read back), rate (nearest, change while running), LNA/VGA (steps, clamped, refusal keeps the old value), stream tone and level, restart cycles, non-blocking library, a refused start, silent stream, unplug mid-stream, impossible lengths (too long, zero, odd, null), flood nobody reads (bounded, dropped counted), callbacks after stop, crash in start and in tuning, hung start and hung cancel (bounded close), missing/not-an-image/partial/needs-a-missing-dependency library, list/info/rate-table overruns, bad arguments. Break-it (the gain call removed) went RED (6 checks), restored GREEN. 3 consecutive full runs green. |
| G1-07b Perseus | NOT BUILT | Stopped on one point (below): the header does not say what the stream callback's buffer holds. Nothing was written for it. |

### Why Perseus stops
`perseus_input_callback` is `int (*)(void *buf, int buf_size, void *extra)` and `perseus_start_async_input` takes `buffersize` ("6*1024 suggested"). The header gives no sample format (bytes per sample, whether 24-bit packed, I/Q order, scaling, whether `buf_size` is bytes or samples) and no return-value meaning; `perseus_get_sampling_rates` takes `unsigned int size` without saying whether it counts elements or bytes. Those facts are in project files this lane was not given (`perseusfx2.h`, `perseus-in.h` are commented out of the header and are not approved). Decoding a guess would corrupt memory or the signal, so nothing was built. **Question for the owner:** may the lane read `perseus-in.h` / `perseusfx2.h` (or the maker's sample-format description), or is there a document that gives the callback layout?

What the header alone says for a bridge once that is known: calls `perseus_init`, `perseus_open`, `perseus_firmware_download` (mandatory after open: loads the FPGA bitstream embedded in the library, the only firmware-type call permitted by the brief), `perseus_get_product_id`, `perseus_set_ddc_center_freq(descr, double, int presel)`, `perseus_set_sampling_rate`, `perseus_get_sampling_rates`, `perseus_set_attenuator_n`/`_in_db`, `perseus_set_adc(dither, preamp)`, `perseus_start_async_input`/`stop`, `perseus_close`, `perseus_exit`, `perseus_errorstr`. Nothing in the header writes the EEPROM; `perseus_get_product_id` reads it.

**Windows (header alone):** it includes `<libusb-1.0/libusb.h>` unconditionally, includes `<pthread.h>` only when `_WIN32` is not defined (so its author expected a Windows build), and uses GCC `__attribute__((packed))` for `eeprom_prodid` (a 16-byte-or-less packed record: sn, prodcode, hwrel, hwver, signature[6]). So a Windows build of the library, if it exists, needs libusb-1.0's DLL beside it. **Whether a Windows build exists cannot be told from the header**: open question for the owner. A bridge would be written to work wherever the library is present, with the file name a setting (`lib=`) and a list of names tried; no DLL name is asserted here.

## Names taken from each header
**fobos.h (Fobos):** functions `fobos_rx_get_device_count`, `fobos_rx_list_devices`, `fobos_rx_open`, `fobos_rx_close`, `fobos_rx_get_board_info`, `fobos_rx_set_frequency`, `fobos_rx_get_samplerates`, `fobos_rx_set_samplerate`, `fobos_rx_read_async`, `fobos_rx_cancel_async` (required), `fobos_rx_set_lna_gain`, `fobos_rx_set_vga_gain`, `fobos_rx_set_direct_sampling`, `fobos_rx_set_clk_source`, `fobos_rx_error_name` (optional); the opaque device type, the callback type `(float*, uint32_t, void*)`, the error-code numbers 0, -1, -2, -3, -4, -8, -9 and the text-field size 64. Ranges used (LNA 0 to 3, VGA 0 to 31) are from the header's comments, restated in the code's own words. Not taken or used: everything else.
**perseus-sdr.h:** none taken into code (nothing built).

## Functions resolved by the Fobos bridge, and the forbidden-name test
Required (10) and optional (5) are the lists above (`fobos_api.hpp` `kRequired` / `kOptional`). Never resolved, listed once in `kForbidden`: `fobos_rx_write_firmware`, `fobos_rx_read_firmware`, `fobos_rx_reset`, `fobos_rx_set_user_gpo`. `test_fobos_source` `testReceiveOnly`: none is in either list; `fobos_source.cpp/.hpp` contain none of the names, nor "firmware_", "_gpo", "rx_reset"; the api header names each exactly once; and the stand-in exports the four only to count calls and the count stays 0 across the happy path and restarts. Also not resolved: the explicit synthesizer calls, the synchronous read calls, the library-version call.

## Design facts (and what is unproven)
- Library file names tried when no `lib=`: `fobos.dll`, `libfobos.dll` (neither is stated by the header: guesses; the needed-library row says so).
- The header does not say whether the streaming call blocks until cancelled, whether the callback length counts floats or complex samples, the tuning range, the gain units, or the thread rules. The bridge makes the streaming call on its own guarded thread (handles both), reads the length as floats by default (never reads more than the library gave; `count=pairs` for the other reading; a stream at about half rate sets `streamNote()`), reports no frequency range, and shows the two gains in steps. Control calls: one thread (the apartment), bounded; the streaming and control calls may overlap in the library (UNVERIFIED that this is safe).
- The sample values are passed through unscaled (the header does not give their scale).

## New engine commands / facts
None. `open-fobos:<arguments>` goes through the existing wire-b open path (`Engine::openWireBSourceAt`), so open failures use `sourceError_`.

## SHARED FILE EDITS
`src/source/vendor_apartment.{hpp,cpp}` (one method `StreamLink::deliverFloats`), `src/source/needed_library.cpp` (one check function, one row at the end of the table), `src/engine/wire_b_sources.cpp` (one include, one row), `src/core/wire_b_source_kinds.hpp` (one key, comment), `src/slint/lanel_logic.cpp` (key and name appended), `src/slint/lanel_publish.cpp` (the Fobos choice shown only when found, index mapping, one note), `tests/CMakeLists.txt` (one name in the Windows-only list, one block at the end), `tests/errpath_allow.tsv` (22 lines), `resources/lang/*.json` + `src/core/lang_assets.hpp` (1 key in 33 catalogues).

## New strings
One key (the Fobos note) in all 33 catalogues, MACHINE-MADE by a sub-agent, unchecked by a speaker; `tools/i18n_keys.py`: 3977 keys, 0 missing, 0 stale; `lang_assets.hpp` regenerated. (The source-name list entry `Fobos SDR (needs the maker's library)` is not a translated literal, like its neighbours.)

## MERGE NOTES (radio-reports lane's reporting calls; from `git show feat/radio-reports-app-2:src/core/radio_events.hpp`)
After that merge, `FobosSource` needs: `radioevents::registerSource(this, identity)` once the board is open (identity = model "Fobos SDR", masked serial); `noteControlRefused(this, "fobos", Control::Frequency|Rate|Gain|DirectSampling|Other, reason)` at the refusal sites in `setCenterFrequencyHz`, `setSampleRateHz`, `setGainDb` (the `setError(o->e)` after `!o->ok`); `noteVendorCallFault(this, "fobos", what, reason)` in `Impl::call` for TimedOut and Faulted and in the stream thread's guarded call (`st->faulted`); `noteDeviceFault(this, "fobos", what, detail)` in `Impl::markDead`; `noteStreamTrouble` when `bad` callbacks are counted and when the silence watchdog fires. On this branch the failures go through `lastError()` / `deviceDead()` and the engine's existing open-failure and refusal paths. The 22 allow lines in `tests/errpath_allow.tsv` (tag `s5c`) should be re-judged after the merge.

## Expectations changed
`tests/test_engine_api_wireb_neededlibs.cpp`: needed-library rows 9 to 10 (the kinds list gains a tenth "library-file"). `src/slint/tests/test_slint_lanel_host.cpp`: source kinds and names 12 to 13; `openRadioId(12, "x")` (was refused as out of range) now `openRadioId(13, "x")`. No assertion was weakened.

## Error-path gate
Pin unchanged (`kPendingPin` untouched); 22 new sites all carry ALLOW lines with reasons; `test_error_paths` 22 checks, 0 failed.

## Gates
Run (all green): `test_fobos_source` 423/0 (3 runs); `test_wire_b_saved_kinds` 268/0; `test_engine_api_wireb_neededlibs` 86/0; `test_engine_api_wireb_ftdi` 43/0; `test_engine_api_wireb_winradio` 62/0; `test_i18n` 394286/0 (with the repository root given); `test_error_paths` 22/0; `test_slint_lanel_host` 180/0. NOT run: the full headless gate in slices, the full GUI gate through the hidden-desktop launcher, the stamp tests, a drive script for the changed Sources page. The window code changed (`lanel_publish.cpp`), so the GUI gate owes a run.

## Config hash
Measured at the end: sha256 `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` (unchanged).

## UNVERIFIED
All of Fobos on a real radio and a real library: the file names, whether the streaming call blocks, the callback length unit, the sample scale, the tuning range, thread safety of overlapping calls, the gain meaning. The translation of the one new string. The Sources page with the Fobos choice in a real window (no drive script written).

## OPEN ROWS FOR THE NEXT LANE
| Row | What waits | Kind of lane |
|---|---|---|
| G1-07b Perseus | the callback's sample layout and `buf_size` unit, the rate-array size unit, return-value meaning (not in `perseus-sdr.h`); owner: may `perseus-in.h`/`perseusfx2.h` be read; owner: does a Windows build of libperseus-sdr exist (needs libusb-1.0's DLL per the header) | engine (sources), owner |
| Fobos on a real radio | real library file name, blocking behaviour, callback unit (`count=`), scale, range, gains in dB if any; a hardware lane with the user's installed library | owner / hardware |
| Fobos reporting calls | the MERGE NOTES list, then re-judge the 22 allow lines | engine (after the radio-reports merge) |
| Fobos engine-level test | an engine test opening `open-fobos:` on the stand-in through `Engine` and reading the facts back (the table row is covered only by the saved-kinds test and the bridge test) | engine test |
| Sources page | drive script for the filtered choice list and the Fobos note; the full GUI and headless gates; ledger rows | screen |
| Registry | `links-worker/registry/libraries.json` `libfobos` has `files: []` and a note saying Fobos is not built: update with the two names tried and republish (worker deploy is the owner's) | owner |
