# Multi-radio engine, stages 5 and 6: saved radios, plugins per radio (record)

Branch `mr/engine-stage5-6`, cut from 5aaa49ac (the tested merge of stage 4 with the AGC and noise-blanker work), built 2026-11-11 in the worktree `C:\Users\steve\foxsdr-wt\mr-engine1`, build folder
`D:/foxsdr-build/mr-engine1`. Design: `docs/multi-radio-engine-design.md` of the engine-stage4 worktree (untracked there, not copied; its last section supersedes every "four": `kMaxReceivers` = 8, at most eight
open radios in total). What stages 1 to 4 built and left for this work: `engine-stages-1-3.md`, `engine-stage-4.md`, `agc-nb-wiring.md`. Nothing here is pushed or merged.

## Commits (oldest first)

| Commit | Subject |
|---|---|
| 41fd5265 | Engine stage 5: the saved settings of radios 2 to 8, the plugins' radio key, the usage report's radios_max (+ `tools/old_reader_probe`) |
| 89e972fd | Engine stage 6: plugins per radio, `PLUGIN_RADIO`, the per-radio lists, `FOXAPI_CAP_RADIOS`, scanner and transmitter facts (+ API.md, the mock, additions 9.22) |
| cb911951 | Gaps: golden snapshot records, fuzz seeds, the load tool with decoders, `PLUGIN_RADIO` in apply-command |
| (next) | Orchestrator additions (source kinds, solo read-back, focus after remove), `test_converter_call_sites`, and this record |

## What was built

### Stage 5: the saved settings of radios 2 to 8

The settings file keeps radio 1 in its top-level keys, exactly as every build has written it, and gains two keys, **written only when there is something to say** (a one-radio user's file is the file 0.99.104 writes):

* `radios`: an array of up to seven objects (radios 2 to 8), in the order the user opened them. `schemaVersion` stays 1.
* `pluginRadio`: an object, plugin id (`pluginRunId`, version-stripped) -> radio number 1..8. The engine never writes a 1 (a plugin on radio 1 is the absence of an entry).

The exact shape of one entry of `radios` (`core::SavedRadio`, `src/core/config.hpp`; every key is optional on load and defaults as shown):

```json
{
  "number": 3,                      // 2..8; 0 or taken or out of range = the lowest free one
  "sourceKind": "rtlsdr",           // "siggen" (a test radio) or soapy rtlsdr hackrf airspy airspyhf sdrplay mirisdr rx888 aor hydrasdr; anything else = "siggen"
  "args": "serial=00000001",        // the device arguments ("" for siggen; > 512 bytes = dropped and the radio is "siggen")
  "sampleRateHz": 2400000.0,        // 0 = the driver's default
  "antenna": "",                    // the RX port ("" = the driver's)
  "toneHz": 0.0,                    // a test radio's tone from its centre (0 = 25 kHz x its number)
  "centerHz": 98500000.0,           // the AIR frequency at the centre; 0 = leave the radio where it opens
  "mode": "WFM",                    // a demodulator name ("" = leave)
  "bandwidthHz": 150000.0,          // 0 = the mode's
  "vfoOffsetHz": 0.0,
  "squelchDb": -50.0, "squelchAuto": false,
  "volume": 0.5, "muted": false,
  "deemphasisIndex": 0, "stereoEnabled": true,
  "nrEnabled": false, "nrStrength": 0.5,
  "agcMode": 2, "agcFixedGainDb": 0.0,          // 0 Auto 1 Fast 2 Medium 3 Slow 4 Off; -20..+40 dB
  "nbEnabled": true, "nbThresholdDb": 15.0,     // 6..30 dB
  "notchEnabled": false, "notchFreqHz": 1000.0, "notchQ": 30.0, "autoNotch": false,
  "audioRoute": 1,                  // 0 both 1 left 2 right 3 none
  "running": true                   // it was running when saved; restored running
}
```

An example of a whole two-radio file's new keys: `"radios": [ {entry above}, {"number": 5, "sourceKind": "siggen", "toneHz": 125000, "centerHz": 7100000, "mode": "USB", "bandwidthHz": 2800, "audioRoute": 3} ], "pluginRadio": {"pocsag-decoder": 2, "ads-b": 5}`.
**Gains are not in the entry**: they are per DEVICE (`autoGainByRadio`, `manualGainByRadio`, `airspy`, `ppm`, `biasTee`, `converters`, all keyed by the radio's key) and come back when that device opens, for any radio (the
design, section 7: "the per-device maps already work for any radio"; the brief listed gains per radio, the design wins).

**Load.** `Engine::applyConfig` restores radio 1 as before, then `restoreSavedRadios` (engine_radio_saved.cpp): for each entry, in order, `RADIO_ADD` (the number, the tone for a test radio, the device for a device
radio), then the ordinary ops aimed at the new radio (tune, mode, bandwidth, VFO offset, squelch, volume, mute, de-emphasis, stereo, NR, AGC mode and then its fixed gain, blanker, notch, auto notch, `RADIO_AUDIO`), then `RUN`
if it was running. A device radio opens with the first radio's own start-up open (`restoreExtraRadioDevice`: synchronous; the control thread is not running yet), under the radio's `RadioWork`. A radio whose device is missing or
busy stays, on the generator, with its settings applied to the generator, **named** in the log, in `fact.radio.<n>.refusal` ("RTL-SDR did not open: ...") and as a line "Radio 2: ..." of the new `fact.radio.restore`; the
saved entry still names the missing device on the next save (`restoreKeep_`, the first radio's own rule), a busy one is retried (`pollSavedRadioRetry` now also runs for added radios); and the radios after it are restored all the
same. The same device twice: the second is refused by the ledger ("Radio 3: in use by Radio 2") and the entries after it are restored. After the radios, `restorePluginRadios` takes the plugins' map (a radio that is not open reads as radio 1,
in the log) and rebuilds every runner.

**Save.** `fillConfig` assigns `cfg.radios = savedRadios()` and `cfg.pluginRadio = pluginRadio_`; `savedRadioOf(slot)` reads each radio's settings out of its `Receiver` and its `*_wide_` fields under that radio's `RadioWork`, as the
stage 4 record says (the source by the first radio's own rule, `sourceToSave` with the radio's `restoreKeep_`).

**Validation** (`sanitiseSavedRadios`, applied on load and on write, so the writer cannot write what the reader would refuse): at most 7 entries (the reader stops after 28 so a hostile file cannot make it work long); a number outside 2..8 or repeated
becomes the lowest free one in file order; a kind that cannot be a second radio (a file, a sound card, the Pluto and rtl_tcp panels, anything unknown) becomes the generator; negative or absurd rates, frequencies, bandwidths and tones become 0; every dial is
clamped to its range; arguments over 512 bytes are dropped. A damaged `radios` (wrong type at the key, entries that are not objects, fields of the wrong type) loses nothing of radio 1: fields keep their defaults, the rest of the file loads.

**What an old build does with a new file: proven by running it.** `tools/old_reader_probe` compiles the **sources of 0.99.104** (`git show v0.99.104:src/core/config.{hpp,cpp}`) beside this tree and does what the old application does with a settings file:
`ConfigStore::load`, then `ConfigStore::save`. Results at the stage 5 commit (the files were written by this tree's `ConfigStore::save`, one radio and eight radios plus a plugin's radio):

```
old reader: load ok ()            (one.json)    old reader: sourceKind=rtlsdr centerHz=98500000 mode=WFM volume=0.35 nativeArgs=serial=00000001 schemaVersion=1   write-back ok
old reader: load ok ()            (eight.json)  old reader: sourceKind=rtlsdr centerHz=98500000 mode=WFM volume=0.35 nativeArgs=serial=00000001 schemaVersion=1   write-back ok
one.json: 160 keys, no radios, no pluginRadio;  eight.json: 162 keys = the same 160 + radios + pluginRadio;  after the old build's write-back both have 154 keys: neither new key, radio 1 intact.
```

`schemaVersion` stays 1 (an old build rejects any other value outright), so the old build ignores the two unknown keys (config.hpp: "unknown fields -> ignored") and reads radio 1 as it always did; **it writes the file back without them, so radios 2 to 8 and the plugins' choices
are forgotten there; radio 1 is not** (the design's stated cost, section 7: the five dated copies still hold them). The one-radio file is the same file as before: the test `test_engine_api_config_radios` pins that the new file with
the two keys removed equals the document the same settings wrote without them, and that the keys added are exactly those two. This follows the design and the brief (the old top-level keys are kept for radio 1) with no disagreement.

**Telemetry.** The usage report gains one field, **`radios_max`** (JSON key; integer 1..8 sent clamped): the highest number of receivers running at once in the session (`Engine::telemetryNoteRadios`, once a pass; 1 for a one-radio user).
PRIVACY.md has its row ("Most radios running at once"), `tests/test_telemetry.cpp` lists it. The design's other two items (the diagnostic bundle per radio, issue words carrying a radio number) are not usage-report fields: the bundle now prints one
`radio N: source= rate= model= running= decoders=` line per added radio (no serial, no address); the issue words are not changed (they are Worker vocabulary and the brief says the usage report only). **The Cloudflare Worker is not changed here and must
learn `radios_max` (a number, 1 to 8, sent with every usage report of a build carrying this work) before any such build ships.** No new word, no new issue kind.

### Stage 6: plugins per radio

* **A `PluginRunner` per receiver.** `Engine::radioRunners_` (slots 1 to 7, declared after `pluginRunner_` so they are destroyed before the plugin host and API core) beside `pluginRunner_` (radio 1). `createRadioRunner` at `RADIO_ADD`,
  `destroyRadioRunner` at `RADIO_REMOVE`; `runnerOf(slot)`, `runner()` (the scoped radio's), `forEachRunner`. A runner is built from `pluginsOnRadio(slot)` against its own radio's rate and centre and attached to its own pipeline
  (`rebuildRunnerOf`); a source or rate change on an added radio rebuilds that radio's decoders alone (`refreshPluginRunner(false)`; no plugin UI directive, no other radio touched); the first radio's funnel behaves as before.
  While the patch runs every receiver's decoders stand down (radio 1's always did).
* **Where the choice is kept, how it is set, how it is read.** `Engine::pluginRadio_` (plugin id -> 2..8; no entry = radio 1), saved as `pluginRadio` and restored with the radios, survives a restart. Set by the new op
  **`FOXAPI_OP_PLUGIN_RADIO` = 0x1206** (grant SETTINGS; `text` the module file name, `ival[0]` the radio 1..N, `ival[1]` = 1 also starts it and clears a stop). Read by `fact.plugin.<module file>.radio`, `fact.radio.<n>.plugins` (now every
  radio's) and `value[0]` of `FOXAPI_LIST_DECODERS` (below). The design's way (DECODER_START with a target) is not used: `DECODER_START`/`STOP` stay global; the Runs-on picker sends `PLUGIN_RADIO` (with `ival[1]` = 1 to start).
  Answers: `NOT_FOUND`, `NO_DEVICE`, `OUT_OF_RANGE`, `UNSUPPORTED` (a plugin that transmits: below), `NO_CHANGE`.
* **Default radio 1**: a plugin that has not been placed is on radio 1; with one radio nothing changes (the whole one-radio test set, 753 tests, passed unchanged but for the five known cwd tests and the three apply-command variants that
  had to learn the new op).
* **When its radio is removed** the design is silent, so: the plugin's instances end cleanly with the runner (the pipeline is detached first), its choice goes back to radio 1 and it stays **STOPPED** (run mode `Stopped`, as if the user had stopped it:
  a decoder that quietly starts on another radio's signal would be a surprise); the user starts it again anywhere (`PLUGIN_RADIO` with `ival[1]` = 1). A choice in the file for a radio that is not open at start-up reads as radio 1 and is not stopped
  (it is the same plugin, restored by a build that has fewer radios).
* **Host-call attribution** (no ABI change, the 43 plugins are not rebuilt). The `PluginApiCore` now owns one receiver snapshot and one stream clock per radio (the receiver for slot n *adopts* the core's snapshot, so a plugin thread's `get_state` never
  meets a snapshot being freed; the snapshot is emptied when a radio is added). Each `PluginApiClient` has a `radio`; `get_state`, `get_gain`, `get_sample_rates`, `get_stream_info` and the device checks of a control request answer from it; the controls a plugin queues
  are applied with `target` = its radio; its legacy `request_tune` retunes its radio; a preset tunes the plugin's radio; and `PluginRunner` labels every call it makes into plugin code with its radio (`currentRadio()`), which the host services
  `centre_hz` and `sample_rate_hz` use (a call from a thread of the plugin's own, or from the window's poll, answers for radio 1 - the one place the design's "window session's focus" is not used; the plugin's own radio would be better and needs the
  service table to carry the client, which is an ABI-adjacent change left alone).
* **Per-radio fault isolation.** Containment is per plugin slot (`core/plugin_fault.hpp`), not per radio, and every guarded call into a plugin runs on its radio's own runner and DSP thread, so a plugin that faults or hangs on radio 2 trips ITS slot and is put down while radio 1's
  decoders and audio path never wait on it: tested (below). `process()` is the one entry point the ABI leaves unguarded (plugin_calls.hpp: "the hot signal path is not here"), as it was; the test fixture faults and hangs in `poll_text`, which the I/Q decoders' DSP thread calls.
* **What stays on radio 1, on purpose.** A plugin that transmits (`CASCADE_CAP_TX_AUDIO`): the transmitter has one audio ring and one writer (`PLUGIN_RADIO` for such a plugin to another radio is `UNSUPPORTED`). The audio mute by preset (one latch, one popup: a plugin on
  another radio never mutes). The patch page's own decoder instances.
* **Aggregates** (the 53 uses of `pluginRunner_`, each decided): the image feed's accumulator (`pollAllImages`: one accumulator over all runners, an unchanged picture moved over without a pixel copy; exactly the first runner's own with no other radio), decoder text, status
  rows and generations, the speakers' holder, stopping and starting one plugin, the stop and dormant sets, the lists' matched/fed keys, the diagnostics, the unload path (every runner detached and cleared before the modules are unmapped) all span the runners; the per-radio
  published state (`decodersRunning`, `decodersMatched`, the DEC lamp, `audioPlayingPlugin`, the plugin audio gaps) reads the scoped radio's runner.
* **Scanner and transmitter.** The one scanner stays bound to the radio it was started on (stage 4); `fact.scanner.radio` says where (the number while active, else 0). The transmitter follows radio 1's frequency unless a radio whose open device is a Pluto at the
  transmitter's own address exists (`transmitRadioFor`, a pure function in `engine/transmit_radio.hpp`, tested on its own); `fact.transmit.radio` says which; a radio's stop ends the key only when that radio is the transmitter's. **A Pluto cannot be opened as a second radio in this
  build** (its panel selects and does not connect; `RADIO_ADD` through the scanned list never opens it), so in practice the answer is radio 1 until the Quadrant decides which radio transmits - the hook is here; nothing about a real Pluto as a second radio is tested. An op that chooses another radio (the design's phrase) was not built: the brief asks only for the Pluto rule.

### Gap 1: API.md and the mock

`third_party/foxsdr_api/reference/docs/API.md` has a new section 6.20 (`RADIO_ADD`, `RADIO_REMOVE`, `RADIO_AUDIO`, `RADIO_SOLO`, `PLUGIN_RADIO`, the new facts, `read_radio_list`), rows in its op table, and 6.18's per-radio reads now name the capability bit; `docs/api-0.3-additions.md` 9.22.
The mock engine (`third_party/foxsdr_api/reference/mock_engine/mock_engine.cpp`, which does not build in this tree: it includes a `common/` directory that is not here) now knows the five ops and answers as a one-radio engine would (`RADIO_ADD` is `LIMIT`, `RADIO_REMOVE` of radio 1 `UNSUPPORTED`,
the audio and solo ops `NO_CHANGE` for radio 1 and `NO_DEVICE` for radio 2, `PLUGIN_RADIO` `NOT_FOUND`: it fits no plugin) and does not set the new capability bit. Checked by reading only; it was not compiled or run.

### Gap 2: the four lists per radio

`read_radio_list(session, radio, list, index, out)` (appended to `FoxEngineApi`, after `read_radio_spectrum`; `radio` 1..N or 0 = the session's focus; `out` NULL = the count) for `FOXAPI_LIST_GAINS`, `ANTENNAS`, `SAMPLE_RATES`, `DECODERS`; any other list is `BAD_ARGUMENT`, a
radio that is not there `NO_DEVICE`. **`read_list` of those four follows the session's focus** (radio 1 until `RADIO_FOCUS`), as the design's section 4 says; for every client that knows nothing of several radios it is exactly what it was (the brief said "the focused read unchanged": the
focused session with focus 1 reads unchanged, and a session that moved its focus to radio 2 now reads radio 2's gains instead of radio 1's, which is what "the controls follow the selected radio" needs). `DECODERS` lists every decoder plugin on every radio: `ACTIVE`/`FED` mean "on this radio", `value[0]` is the radio
the plugin runs on. The block (`ApiListBlock::radios`) is built with the rest of the list block, and the fingerprint hashes each added radio's rates, gains (name, range, current), antennas and antenna.
**The capability word is extended deliberately: `FOXAPI_CAP_RADIOS` = bit 30** (the engine serves the whole multi-radio set: the five ops, the three per-radio reads, `fact.radio.*`); `test_engine_api_05a`'s table has its row (the functions are still tested with `FOXAPI_HAS`), its loop runs to bit 30 and
the word ends there. The earlier "no capability bit" comments in the header are corrected.

### Gap 3: the capability word, the fuzz seeds and the golden snapshot

* **The capability word** had no bit for the multi-radio set and no row for it; it has both now (above).
* **The fuzz seeds.** `tests/fuzz/corpus/config/seed-defaults.json` already carries the AGC and blanker keys (lane C); the config corpus had nothing for `radios` or `pluginRadio`: two seeds added (`seed-radios.json`, `seed-radios-damaged.json`) and `fuzz_config` now also requires the
  properties of the new keys (at most seven radios, distinct numbers 2..8, every value in range, no kind that cannot be a second radio, plugin radios 1..8); `fuzz_plugin_hostapi` now drives `setClientRadio` with any number between its calls and requires the radio to stay 1..8. The control-request corpus
  is not affected (no ControlRequest field maps to the new ops; the web profile was left as lane C decided).
* **The golden snapshot** (`tests/golden/state_snapshot.golden`, the readers' text: web status, CAT, plugin ABI reads) is held by `test_state_snapshot_golden`, a window test. Its drive did not include the stage 4 and lane C ops. They are appended (the records of the capture are unchanged: the old file is a prefix of the new one, 144 lines are added: with the AGC set to Fast and the blanker on, and with a second radio open and tuned, every reader's text is exactly the text of "back to the generator" (these readers do not carry the new fields and read the first radio); with a plugin put on radio 2 exactly one line differs - the plugin state read, which is now radio 2's centre and mode).

### Additions from the orchestrator (the screen lane's five findings on live radios)

1. **Source kinds.** `RADIO_ADD` now takes `file:<path>`, `open-pluto:<address>` and `open-rtltcp:<address>` (they open through the same worker as on radio 1; a Pluto and an rtl_tcp server claim a ledger key, a file none). The design excludes no kind;
   the **sound card stays the first radio's** (its one list and open are the Source panel's shared state) and the refusal now says so (`UNSUPPORTED`, "the sound card is the first radio's ..."). A bare `row:` id is refused with a sentence naming what a second radio can open. **Not done: saving these kinds in `radios`** - a second radio on a
   file, a Pluto or an rtl_tcp server is written as a generator radio, so it does not come back after a restart (the restore path opens scanned devices only). Tested by acceptance only (nothing behind the file, the Pluto or the server: the radio is added and stays on the generator with its reason); an I/Q file really playing as radio 2 and a real Pluto were not run
   (D:/foxsdr-samples was not used). The transmitter's follow-the-Pluto rule therefore has no restart path for a Pluto radio 2 yet.
2. **Solo read-back**: `fact.radio.solo` (the radio heard alone, 0 = nobody; cleared when that radio is removed), in the list fingerprint.
3. **`fact.radio.<n>.plugins` per radio**: done in stage 6 (every radio's, tested: `pluginsOn(n)`).
4. **List sequence on add and remove**: verified first - the fingerprint already held the slot mask, and now also each added radio's lists and the solo, so `listSeq` moves on add and on remove (test E). Mutating away the mask alone left the test green because the per-radio lists move it too; both are in.
5. **Focus after `RADIO_REMOVE`**: a session focused on the removed radio is moved to radio 1 and its seq groups move (`ApiHost::radioClosed`); a session on another radio is untouched. `test_engine_api_multi_radio`'s stage-4 expectation (NO_DEVICE until the session sets another) was changed to the new rule. Break-it: the focus move, the solo fact and the file acceptance each turned `test_engine_api_radio_lists` red.

## Decisions and deviations from the design or the brief

1. **Gains are not in the `radios` entry** (design section 7: the per-device maps already work for any radio); the brief listed them.
2. **`DECODER_START` with a target is not the way to place a plugin**; `PLUGIN_RADIO` (the next free number of the 0x12xx block) is, because `DECODER_START`/`STOP` are global ops and a target on them would have changed `radio_scope.hpp`'s list and every client's reading of them.
3. **A removed radio's plugins go back to radio 1 and stay stopped** (the design is silent; the brief's fallback).
4. **`read_list` of the four lists follows the session's focus** (the design; the brief's "focused read unchanged" holds for focus 1).
5. **A plugin that transmits stays on radio 1** (one audio ring, one writer).
6. **The window session's focus is not what a plugin's host call is attributed to** (the design): it is the plugin's own radio (its client's), or the runner's label from inside a call. A plugin's page (the kit) reaches the same client.
7. **`running` is restored** (the design lists "running at start" per radio): a saved running radio 2 comes back running while radio 1 is stopped as ever. The screen lane may prefer stopped; it is one line in `restoreSavedRadios`.
8. **The test fake radio fails to open an argument containing `MISSING`** (`tests/engine_api_fake_radio.hpp`), for the missing-device section.
9. **Tests edited that were not mine**: `test_config_halves` (a perturbation for `std::map<std::string,int>`: a valid radio number), `test_engine_main_config_roundtrip` (the two keys the engine writes), `test_telemetry` (the new field and its PRIVACY.md row), `test_engine_api_05a` (the capability bit, deliberately), `test_apply_command` (`PLUGIN_RADIO` coverage: its own rule is that every implemented op is covered), `test_state_snapshot_golden` (appended records).

## Tests

New (each looped 20 times in a row, see Gates):

* `test_engine_api_config_radios` (the file's side, core only): A round trips with 0, 1 and 7 entries (every field, in order; a second pass is the same text); B the old file (a 0.99.104-shaped fixture built in the test, never the real one) loads as one radio and writes
  back without either key; C the new file read as an old build reads it (schemaVersion 1; the keys added are exactly `radios` and `pluginRadio`; with them removed the document equals the one the same settings wrote before; this tree's reader on the stripped document reads radio 1 identically);
  D a damaged array (the wrong type at the key, entries of the wrong type, fields of the wrong type, nine entries, four hundred entries, repeated and impossible numbers, negative and absurd values, kinds that cannot be a second radio, device arguments of a megabyte, duplicate devices kept for the ledger);
  E the plugins' radio (per-entry rules, an empty map writes no key).
* `test_engine_api_saved_radios` (the real engine, generator radios and the fake RTL-SDR): A the old file at the engine (one radio, no key written); B round trips of 2, 3 and 8 radios through the file format (centre, VFO, bandwidth, squelch, volume, mute, de-emphasis, NR, notch, AGC mode and fixed gain,
  blanker, auto notch, route, running; a second save is the file it was restored from); C a device radio comes back on its device; D a missing device is reported by number and name, the radio sits on the generator, the others come back, the next save still names the device; E the same device twice;
  F damaged arrays at the engine; G `radios_max` (the report's field clamped 1..8; the engine's counter counts running radios only and never goes down).
* `test_engine_api_plugin_radio` (the real engine and a real I/Q decoder module, `tests/fixtures/radio_probe_plugin.cpp`, copied under several names): A default radio 1 and the facts; B `PLUGIN_RADIO` moves it - each radio's plugin is fed ITS radio's tone, centre and `get_state`, the other runners hold
  nothing of it, the op's answers, the DECODERS list of every radio; C a plugin's `set_frequency` moves its own radio only (and again from radio 3); D 160 moves of two running plugins among three radios with a reader thread on the lists, facts and states: no failure, no instance left behind or doubled, the process's threads back to
  what they were; E removing a plugin's radio stops it cleanly (back on radio 1, stopped, nothing fed, the others untouched, a radio added in its place does not bring it back); F saved (and a plugin on radio 1 is no entry); F2 restored by plugin id, and a radio that is not open reads as radio 1; G a fault in `poll_text` on radio 2
  (contained, quarantined, put down; radios 1 and 3 and their decoders keep running, radio 2's spectrum goes on) and a 1.5 s hang there (radio 1's decoder is fed throughout); H the scanner stays on its radio and `fact.scanner.radio` says so, and the transmitter rule as a pure function.
* `test_engine_api_radio_lists`: A each radio's gains, rates and antennas (the fake device's stage with its range, six rates with the current one marked, the antenna; none on the generators; a gain and a rate set on radio 2 move radio 2's list alone); B `read_list` follows the session's focus for these four lists and no other, per session;
  C the edges (no such radio, radio 0, a list that is not one radio's, a removed radio, a radio added in its place); D the capability word.

Extended: `test_engine_api_05a` (the capability word: bit 30, the loop to 30, the word ends there), `test_apply_command` (`PLUGIN_RADIO` refusals), `test_config_halves`, `test_engine_main_config_roundtrip`, `test_telemetry`, `fuzz_config` and `fuzz_plugin_hostapi` with two new config seeds, `test_state_snapshot_golden` (appended records).
`engine_api_fake_radio.hpp`'s fake radio fails to open arguments containing `MISSING`. `tools/multi_radio_load/engine_main.cpp` takes an optional radio probe module and puts one decoder on each of radios 1 to 4. `tools/old_reader_probe` is the proof about old builds.

## Break-it checks

Each mutation was applied, the test built and run, red seen, and the file put back with `git checkout`.

| Mutation | What turned red |
|---|---|
| `sanitiseSavedRadios`: the cap of seven removed | `test_engine_api_config_radios`: nine entries kept 9, four hundred kept 28 |
| `sanitiseSavedRadios`: a repeated number is kept | the distinct-numbers check of the repeated-numbers section |
| `sanitiseSavedRadios`: the volume clamp removed | the range checks (-3 and 7 survived) and the engine-level damaged-array section |
| the writer writes `radios` even when empty | the old-file, new-file-as-old-build and one-radio sections (6 checks) and the engine's "no change for a one-radio user" |
| `fillConfig` does not assign `cfg.radios` | the round trips: no radio in the file, a restored engine has one radio |
| `applyConfig` does not call `restoreSavedRadios` | the same: every restored radio missing |
| a device that will not open is not remembered (`restoreKeep_`) | the next save no longer names the missing device; the "Radio 2:" note is also gone in the combined run |
| a radio that cannot be had stops the restore (`break` for `continue`) | the radios after the same-device-twice and after the missing one are not restored |
| the restore notes are not recorded | `fact.radio.restore` is empty |
| every pipeline is attached to the first radio's runner | stage 6 B: the plugin on radio 2 is never fed (tone, centre, state) |
| a plugin's reads answer from radio 1 whatever its radio | `state_centre` of the plugin is radio 1's on radios 2 and 3 |
| the controls a plugin queues have no target | `set_frequency` moves radio 1, not the plugin's radio (C) |
| a removed radio's plugins are not sent back and stopped | E: the plugin's run word is not "stopped" |
| `pluginRadio` is not saved | F: the file holds no entry for the plugin |
| `pluginRadio` is not restored | F2: the plugin is on radio 1 after the restart |
| `read_list` ignores the session's focus | the lists B section and the DECODERS focus check |
| `read_radio_list` answers radio 1's copy for every radio | `test_engine_api_radio_lists`: radio 2 lists nothing |
| a moved plugin is not taken off its old radio | B and D: two or three instances of one plugin |
| the scanner fact never says where it is | H: `fact.scanner.radio` is 0 |
| a Pluto that did not open counts as the transmitter's radio | the pure rule (after the test was strengthened: the first draft put the closed Pluto in slot 0, where the mutation could not show) |

## Gates

* Build: `cmake --build D:/foxsdr-build/mr-engine1 --config Release --parallel` exit 0 (`BUILD_DONE_0`) after every change; last at the commit before this record.
* `ctest -L "headless|slint" -j4 --timeout 300` at 89e972fd: 753 tests, 9 failed (1286 s): the five tests that find the tree from the working directory, three `test_apply_command` variants (the new op was uncovered: fixed, pass) and `test_diagnostics_free` (commit stamp).
  **At the tip (after the additions, 1296 s): 753 tests, 8 failed**, every one explained: `test_airband_data`, `test_band_plan`, `test_page_resize_id`, `test_png_decode` (known false failures on D: builds; they pass from the worktree root: 115, 5174, 22, 49 checks, 0 failed);
  `test_converter_call_sites` (known cwd failure AND a real one found in the first gate: the restore line of an added radio had to be classified; now 5 checks, 0 failed from the root); `test_engine_api_transmit` (the known coin-flip test, failed in the gate); `test_rtl_tcp_source` (load flake: 421 checks, 0 failed alone);
  `test_diagnostics` / `test_diagnostics_free` (the commit stamp: they fail while the tree is dirty and pass on a clean rebuilt tree).
* `ctest -L gui -j3 --timeout 300` on the hidden desktop (`FoxSDRTests-mr5`, `FOXSDR_FRAME_CAP_HZ=60`, 1458 s) at cb911951: 227 tests, 226 passed, the one failure `test_diagnostics` (commit stamp, as above). `test_state_snapshot_golden` was written and checked on the hidden desktop (0 of 1022 lines differ).
* New tests looped 20 times each (80 runs): `test_engine_api_config_radios`, `test_engine_api_saved_radios`, `test_engine_api_radio_lists`, `test_engine_api_plugin_radio` all 20 of 20 green (before the additions; the radio-lists test then gained section E and passed alone at the tip).
* `config.json` sha256 before and after: c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3 both times (every test used its own settings folder).

## Measured

`multi_radio_load_engine 8 300 - radio_probe_fixture.dll` (eight test radios on paced generators at 2 MS/s, WFM, volume zero, through the real engine - control thread, API host, per-radio publish and housekeeping, an audio stream a radio on the default
output - **with one real I/Q decoder module fed on each of radios 1 to 4**, each writing a report every 50 000 frames, about 12 150 reports apiece in the run): every radio read 603 780 000 samples (2.000 MS/s), **zero ring drops on every radio, zero audio underruns on every radio**,
no fault, no audio recovery, all four decoder instances live and fed; process CPU **0.52 core on average = 1.61% of this machine** (32 logical processors), working set **92 to 93 MiB**, 30 to 35 threads. Peak ten-second sample 2.00 core(s) at t=60 s (1.64 at 70 s, 1.18 at 50 s): cause not found
(a first 300-s run of the same tool, whose decoders were live and fed but wrote no reports because the tool had not yet set the environment the module reads, averaged 0.55 core with a peak of 0.82; both runs had zero drops and zero underruns). Stage 4's figures for the same load without
decoders: 0 drops, 0 underruns, 0.37 core, 92 MiB, peak 0.81. So drops and underruns are unchanged (zero), memory is unchanged, and the average CPU is about 0.15 core higher with four decoders fed and writing their reports. This machine, one fast desktop, generators, no slow machine.

## What is unverified

* **No real radio** (stage 7 is separate): the restore of a device radio is proven on the fake RTL-SDR only; two real radios at once (the B200 and the RTL-SDR), several RSPs on one SDRplay session and a Soapy device as a second radio were not run.
* **A Pluto as a second radio**: the transmitter's rule is tested as a pure function, and the engine's wiring of it is a read of the radios' kind and arguments; a Pluto cannot be opened as a second radio in this build, so the path with a real Pluto was never exercised. Which radio transmits remains the Quadrant's decision.
* **Sound** was never listened to. The load run's audio streams are at volume zero on the default output; route and solo are proven in the sink's arithmetic and the engine's state (stage 4), not through speakers.
* **The 43 real plugins** were not run on a second radio: the proof is the radio probe (a real module through the real loader, host table, runner and guard). Image decoders on another radio (`pollAllImages`) and the audio-out and in-chain processor plugins on another radio have no test. The window (classic and Slint) was not driven with plugins on other radios.
* **A plugin's own thread** calling `centre_hz` / `sample_rate_hz` is answered for radio 1 even when the plugin is on another radio; `get_state` and the other reads, the controls and `request_tune` are right by client.
* **`process()` is unguarded by the ABI's design**: a plugin that faults inside `process()` on any radio still takes the process down, as before; the fault and hang tests use `poll_text`. A hang inside `process()` on radio 2 would hold radio 2's DSP thread (radio 1's does not wait for it) and the pass whenever it takes radio 2's runner lock, as radio 1's would.
* **The mute-by-preset machine** serves radio 1 only (a plugin on another radio never mutes anything).
* **No sanitizer run** (ThreadSanitizer) of the new cross-thread paths (a runner removed while its DSP thread is running); the 160-move loop with a reader thread, the 20-run loops and the storm tests are what there is.
* **The golden snapshot's new records** were written by this code and checked against it; no break-it on them separately (the same mutation of the plugin's reads turns the plugin tests red).
* **The mock engine** and API.md edits were checked by reading only (the mock does not build in this tree).
* **The Cloudflare Worker is unchanged**: it does not yet know `radios_max`.

## What the screen lanes must know

* **The Runs-on picker.** Read: `fact.plugin.<module file>.radio` (a number as text; "1" for a plugin that has not been placed) per plugin, or `value[0]` of every item of `read_radio_list(session, n, FOXAPI_LIST_DECODERS, ...)` (the same on every radio's list; `ACTIVE`/`FED` of the item say it runs and is
  fed on radio n); the radios the picker offers are `fact.radio.count` / `fact.radio.<n>.state` for n up to `fact.radio.max`. Set: `FOXAPI_OP_PLUGIN_RADIO` with `text` = the module file name, `ival[0]` = the radio, `ival[1]` = 1 to start it there too (the DECODE bank's START on a selected radio sends exactly this).
  A plugin that transmits answers `UNSUPPORTED` (offer radio 1 only for it: its `CASCADE_CAP_TX_AUDIO` bit is in the PLUGINS item's capability word). The choice is saved and comes back; one for a radio that is not open comes back as radio 1 (say so on the first start). When the user closes a radio, its plugins are back on radio 1 and STOPPED.
* **A pane's decoder chip**: `fact.radio.<n>.plugins` (module files, comma-separated) is the list of decoders running on radio n.
* **The per-radio lists**: `read_radio_list` for GAINS / ANTENNAS / SAMPLE_RATES / DECODERS of any radio behind `FOXAPI_CAP_RADIOS`; `read_list` of the same four follows the session's focus. The block moves with the fingerprint, so a gain set on radio 2 changes radio 2's list; read the `listSeq` of `read_radio_state(n)` (which carries `(n-1) << 48`).
* **Saved layout**: nothing of the screen's layout is in the engine's settings (the screen lane saves its own half); the engine's `radios` array is the radios themselves and is restored before any pane exists.
* **Restore problems**: `fact.radio.restore` (newline-separated "Radio N: ..." sentences, absent when every saved radio came back) and `fact.radio.<n>.refusal`; show them on the pane of the radio named.
* `fact.scanner.radio` (0 when the scanner is idle), `fact.transmit.radio` (the radio the transmitter follows).
