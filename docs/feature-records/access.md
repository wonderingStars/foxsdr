# Accessibility and comfort lane (`access`, branch `feat/access`)

Engine lane, NEW-FILE MODEL LANE: models, OS wrappers and tests in new files. No existing file was edited
(`tests/CMakeLists.txt` needed no line: tests are globbed). No `.slint`, `engine.cpp`, `pipeline.cpp`,
`receiver.hpp`, `demod.cpp`, `plugin_abi.h`, `fox_engine_api.cpp`, recorder or `map_view.cpp` file was touched.
Versions: none changed (old = new). Nothing pushed, nothing merged.

## 0. Step 0 table (written from reading, before any code)

| Row (REMAINING-GAPS "Accessibility and comfort") | Class | What I found | What this lane did |
|---|---|---|---|
| Screen reader: Slint kit has 490 `accessible-` lines; classic ImGui window has no keyboard navigation flag; waterfall, map, dial faces not exposed; no NVDA/JAWS test | **needs screen or engine file** (plus an unrunnable proof) | Recounted: 490 `accessible-` lines in `src/slint/ui/*.slint` (exact match). Classic window: `ConfigFlags` is set only for Viewports (`app_window.cpp:1177`) and Docking (`:1186`); `NavEnableKeyboard` appears nowhere. Spectrum and waterfall are `accessible-role: image` with a label only (`spectrum.slint:171`, `waterfall.slint:118`); the map is `image` "Map" with a `list` legend (`map.slint:134`, `:415`); the frequency counter is a `groupbox` "Tuned frequency" whose digits are `spinbox` with a value (`digits.slint:50-56`, `:262`); the Meter is `progress-indicator` with a value text and a 0..1 range (`meters.slint:76-81`); the Compass has no accessible properties (`instruments.slint:371`). No screen reader is installed and none may be downloaded. | Nothing in code (the brief says no code here). The exact list of what to expose, with roles and values, is section 7. NVDA/JAWS: **UNVERIFIED**, not run. |
| Spoken frequency, mode and S-meter on demand or on change | **new file model** (the audio hook is a hand-off) | No speech code in `src`. | BUILT: `access_speech.hpp`, `access_speech_sapi.hpp/.cpp`. |
| Sonification: signal strength or spectrum as a tone | **new file model** (device output is a hand-off) | None in `src`. | BUILT: `access_sonify.hpp`. |
| Colour-blind-safe palettes for the UI and waterfall | UI palette: **new file model**; waterfall map: **already built** | `feat/signal-blocks-3` exists (tip 59458f1e) and its record (`docs/feature-records/signal-blocks-3.md`) describes the colour maps ("Blue-yellow safe", viridis licence report) and uses the same Machado source. Not duplicated. Only the band plan was checked for colour vision (`band_plan.hpp`, Okabe-Ito). | BUILT: `access_palette.hpp` (five UI colour sets + verifier). Applying them needs `theme.slint` (lane K). |
| Right-to-left languages | **investigation, no `.slint` edit** (the screen half is lane K, the strings are i18n) | The 34 string tables carry no RTL language. | DONE: a stand-alone headless Slint probe, `tools/access_rtl_probe/`, measured what Slint 1.18.1 does. Section 6. |
| Imperial/metric unit choice | **new file model** (wiring needs `track_metrics.hpp`, `map_view.cpp`) | Per-object units are fixed in `track_metrics.hpp:1252-1280` and `:1376-1380` and `map_view.cpp:89-103`. | BUILT: `access_units.hpp`; the existing per-object defaults are `UnitSystem::Automatic` and are pinned by a test. |
| Locale-aware number output (only input accepts a decimal comma) | **new file model** (wiring needs `host_logic.cpp` and the readouts) | Output is `"%.6f MHz"`-style (`host_logic.cpp:111-120`, `:181-200`); input already accepts a comma (`host_logic.cpp:130`). | BUILT: `access_number_format.hpp/.cpp`. |

Nothing is **blocked**; two things cannot be PROVEN here (a person hearing the speech, a real screen reader), and
both are listed as UNVERIFIED, not as blocked.

## 1. What was built (all new files)

| File | What |
|---|---|
| `src/core/access_units.hpp` | `UnitSystem` {Automatic, Metric, Imperial, Aviation, Nautical}; `unitFor`, `convert`, `quantify`, `text` for altitude, distance, speed, temperature, pressure; NIST SP 811 factors. |
| `src/core/access_number_format.hpp/.cpp` | `NumberSymbols`; pure `formatNumber` / `parseNumber`; `loadNumberSymbols(localeName, honourUserOverride, out)` over `GetLocaleInfoEx`. |
| `src/core/access_sonify.hpp` | `LevelCurve`, `Sonifier`, `findSpectrumPeak`, `sonifySpectrum`, `ToneRenderer`. |
| `src/core/access_palette.hpp` | Five colour sets as data, WCAG contrast, Machado simulation, CIELAB distance, `verifyPalette`. |
| `src/core/access_speech.hpp` | `PhraseTable`, phrases, S-units, `AnnouncePolicy`, `Announcer`, `Speaker`, `RecordingSpeaker`. |
| `src/core/access_speech_sapi.hpp/.cpp` | `SapiMemorySpeaker`: SAPI 5, output ALWAYS a memory stream. |
| `tools/access_rtl_probe/` | Stand-alone Slint RTL probe (not in the FoxSDR build). |
| `tests/test_access_{units,number_format,sonify,palette,speech}.cpp` | The five tests. |

Design choices a reader should know:

* **Units.** `Automatic` returns exactly what the code prints today (aircraft/vessel/station: ft, kt; satellite: km, km/s;
  every range: km). A test also scans `src/gui/track_metrics.hpp` for the literal format strings the table was copied
  from, so editing either side turns it red. Under an explicit system a satellite's altitude is in the system's distance
  unit and its speed in that unit per second (km/s, mi/s, nmi/s), never feet or knots. Aviation pressure is hPa (ICAO
  form); Imperial uses inHg.
* **Numbers.** Digit policy default is Latin (reasons in the header). `loadNumberSymbols` with
  `honourUserOverride=false` is used by every test, so results do not depend on this PC's Region settings. In the
  application it is called with `true`.
* **Sonification.** Pure; the only sound code is a tiny phase-continuous renderer used by tests and available to the audio hand-off.
* **Palettes.** The Normal set is the Workbench skin's values; it is **not** colour-blind safe and the test pins that fact.
* **Speech.** `SapiMemorySpeaker` attaches an in-memory stream, reads the voice's output back and compares it with what
  it attached; only then may `speak()` reach the engine. A source-scan test pins that no audio-device API is
  referenced. **Nothing was played. Nobody has listened. NVDA and JAWS were not run (none is installed or downloaded).**

## 2. Tests (real result lines)

**Own tests** (built with `-DCASCADE_BUILD_SLINT=OFF -DCASCADE_SOFTGL=OFF`, Release, build folder `D:/foxsdr-build/en-access`;
`ctest -C Release -R test_access_` -> `100% tests passed, 0 tests failed out of 5`):

```
test_access_units: 1785 checks, 0 failed
test_access_number_format: 21878 checks, 0 failed
test_access_sonify: 4470 checks, 0 failed
test_access_palette: 661 checks, 0 failed
test_access_speech: 125 checks, 0 failed
```

**Whole build**: `cmake --build ... --config Release --parallel 3` of every target, `done 0` (567 test executables).

**Headless set** (`ctest -C Release -L headless`, 708 tests), run in eight slices of at most 10 minutes
(`ctest -I a,b -j3 --timeout 150`; logs `D:/foxsdr-build/en-access/access-gate-s1..s8.log`). The fixtures
`src/slint/tests/drive/fixtures/*.wav` are gitignored and generated; a fresh worktree has none, so the 25 deterministic,
standard-library-only generators (`python -I make_*.py`, in that folder) were run once first (nothing tracked changed).
First pass: 36 distinct tests red. Each was re-run alone, and the ones that stayed red were compared with a **control
build of the base commit d3630d78** (a temporary worktree of my own, since removed):

| Result | Tests |
|---|---|
| Red, listed false failures of the standard rules | test_airband_data, test_band_plan, test_converter_call_sites, test_page_resize_id, test_png_decode, test_window_failed_app (the last is the no-Slint configuration) |
| Red, listed load flakes | test_health_app_stepped, test_pass_control, test_tx_audio |
| Passed on re-run alone (load) | test_cat_server, test_engine_api_05a, test_engine_api_decode_radio, test_engine_api_source_kinds (also needed the fixtures above), test_autogain_app_stepped, test_radio_setup, test_rtlsdr_live, test_failure_files, test_sdrplay_probe, test_recorder_bookmarks_scanner_free, test_scope_window_free, test_plugin_window_stepped, test_pipeline_device_rate_audio, test_pa_stream_list_guard, test_udp_audio, test_shutdown_budget_free, test_plugin_unload_handshake, engine_only_configure, release_tarball_check_selftest, telemetry_worker, interface_fence_control_net_web_server_hpp |
| Intermittent on BOTH trees (base fails too) | test_engine_api_schedule (mine 1 fail in 4 runs, base 2 in 4: the recorder wrote 44 bytes under 100% CPU), test_usb_bounded_walk (a teardown SegFault once under load; 6 of 6 passes on each tree afterwards) |
| Red on mine AND on the unmodified base d3630d78 | test_sentinel (crash-location lookups of `ucrtbase.dll`, a timing budget), test_excuse_cap, test_crash_upload_free, test_soapy_enum_proc (timeout) |

Nothing in the headless set that this lane could have affected went red: the lane adds files only, and the two new
library objects (`access_number_format.cpp`, `access_speech_sapi.cpp`) are referenced by no existing code. The machine
was at 100% CPU for the whole gate (`Win32_Processor.LoadPercentage` = 100). The headless set contains
`test_rtlsdr_live` and `test_sdrplay_probe`; they ran as part of the set the brief asked for (no radio was opened by me).
The GUI-labelled tests were not run (not asked, and the Slint window is not built in this configuration).

The `config.json` check at the end: sha256 `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3`, unchanged.

## 3. Break-it table (behaviour removed, red seen, restored, green)

| Feature | Mutation | Red | Restored |
|---|---|---|---|
| Units | `Automatic` altitude forced to metres | `test_access_units`: 6 checks failed (of 1785) | 0 failed |
| Numbers | lenient 3-digit group tolerance disabled | `test_access_number_format`: 5 failed (of 21917 run) | 0 failed (21878) |
| Sonification | the 5 ms fade removed (instant gain) | `test_access_sonify`: the two click checks failed. **First attempt passed vacuously**: the audible block was 4800 samples = exactly 100 cycles of 1 kHz, so it ended on a zero crossing and hid the click; the block is now 4813 samples and the mutant is caught | 0 failed (4470) |
| Palettes | colour-vision simulation replaced by the identity | `test_access_palette`: 7 failed (the Normal set stops failing under protanopia/deuteranopia, so the pinned finding trips) | 0 failed (661) |
| Speech policy | the dial-settle rule removed | `test_access_speech`: 4 failed (the knob-turn and the frequency+mode cases) | 0 failed (125) |
| Speech safety | the `memoryOutput` guard before the engine call removed | `test_access_speech`: the source scan failed | 0 failed |

## 4. Numbers worth knowing

* **Real locale data (explicit names, no user override), read on this PC:** en-GB decimal `.` group `,` grouping `3;0`;
  de-DE `,` `.` `3;0`; fr-FR decimal `,` group U+202F (narrow no-break space) `3;0`; ar-SA decimal `.`, group `,`, digit
  substitution **0 (context)**, native digits start U+0660 (Arabic-Indic); hi-IN decimal `.` group `,` grouping `3;2;0`
  (lakh), native digits start U+0966 (Devanagari), substitution 1 (none). The formatter's output for 1234567.891 matched
  the OS's own `GetNumberFormatEx` for en-GB, de-DE, fr-FR and hi-IN byte for byte (including `12,34,567.89`).
  ar-SA's own formatting was not compared (the OS call may apply digit substitution); the symbols are reported, not asserted.
* **Palettes** (smallest CIE76 Delta E inside a group; required floor 25):

  | set | normal | prot | deut | trit |
  |---|---|---|---|---|
  | Normal | 29.5 | **5.4** | **6.0** | **8.8** |
  | Deuteranopia-safe | 38.3 | 22.7 | **33.8** | 3.1 |
  | Protanopia-safe | 43.0 | **34.7** | 21.0 | 11.3 |
  | Tritanopia-safe | 41.1 | 6.3 | 8.8 | **36.6** |
  | High contrast | 42.4 | **26.2** | **26.3** | **26.0** |

  Bold = a vision the set is held to. The Normal set's `status-ok` against `status-bad` is 16.4 under deuteranopia and
  `radio-1` against `radio-4` is 6.2: **the current skin is not safe for protanopia or deuteranopia**; its lamps and
  radio colours rely on hue. Searched sets reach 34-57 for status under their own deficiency. Eight identity colours
  that stay 25 apart under all four visions AND clear 7:1 on black do not exist (the search topped out at 22.6), so the
  high-contrast radio colours are held to 4.5:1 as graphics and reach 26.0.
* **Speech through the real voice, to memory:** voice "Microsoft David Desktop - English (United States)"; "S 8" ->
  61,492 bytes (1.39 s at 22,050 Hz, 16-bit mono, RMS 1754); "7.074 megahertz, upper sideband, S 9 plus 20 decibels" ->
  313,496 bytes (7.11 s, RMS 3240). Not silence, longer text longer. That the words are intelligible is **UNVERIFIED**.
* **Sonification:** a peak in a noisy generated tone at seven fractional bin positions was found within 0.2 bin;
  2000 noise-only frames gave 0 false peaks at the 14 dB default (theory: about 1.4e-5 per frame).
* **RTL probe:** section 6.

## 5. UNVERIFIED list

1. Nobody listened to the speech; whether David/Zira read "S 9 plus 20 decibels" or "narrow F M" as intended is untested.
2. NVDA and JAWS were not run (none installed; downloading is forbidden). Whether Slint's accessible properties reach a
   screen reader at all on Windows, and whether `accessible-live-region` is honoured, is not tested. The Slint
   reference pages opened name no platform accessibility API.
3. The Machado matrices are applied in linear RGB. The authors' page does not state the space; this lane's choice (the
   lane that built the waterfall maps made the same one and says the same).
4. IARU R.1 values (S9 = -73 dBm HF, -93 dBm above 30 MHz, 6 dB per unit) come from the Wikipedia article quoting it,
   not from the IARU document. The 7:1 high-contrast text floor was not checked against SC 1.4.6 (not opened).
5. The Delta E floor of 25 is this lane's choice, not a standard's. Nobody with colour-vision deficiency saw the sets.
6. Digit shapes: whether the bundled UI fonts carry Arabic-Indic or Devanagari digit glyphs is untested; that is why the
   default digit policy is Latin.
7. Sonification was never played; levels, pitch range and comfort are numbers only. Its audio-device output does not exist yet.
8. `fr-FR`'s group character is U+202F on this OS build and U+00A0 on older ones; the test accepts either.
9. Whether Slint infers paragraph direction when an app language is selected through its bundled translations was not
   tested (the probe did not select a translation).
10. The SAPI test ran on the owner's logged-in desktop session, not the hidden desktop: it creates no window, plays
    nothing, and the output is a memory stream; the hidden-desktop launcher refused the probe's batch file (exit 1,
    no output), so that probe was run directly. No window appeared in either case (nothing creates one).

## 6. Right-to-left: what Slint 1.18.1 does (investigation, no `.slint` edit)

**Documents opened** (docs.slint.dev, the Slint documentation, read 2026-11-11; Slint's source is GPL, which this lane
may not read, so only documentation and the changelog were used):

* *Text element* page: `horizontal-alignment` takes `TextHorizontalAlignment` with values left, center, right, **start**
  and **end**; "start" and "end" are described as "left or right depending on the direction of the text". The page does
  not mention bidirectional text, Arabic or shaping.
* *Common properties* page: lists the 21 `accessible-*` properties and 30 `AccessibleRole` values; no mention of RTL.
* *CHANGELOG* (github.com/slint-ui/slint, master): "[1.19.0] Unreleased ... Fixed `overflow: elide` with right-to-left
  and mixed-direction text." This shows RTL and mixed-direction text reach the text engine.

The documents do not say whether Arabic is shaped or Hebrew reordered, so a **measurement** was made. The Slint SDK in
`C:/Users/steve/slint-cpp-1.18.1` (slint-compiler, `slint_cpp.dll`, headers) compiles a stand-alone program headlessly:
`tools/access_rtl_probe/` renders strings with the software renderer into memory (the same technique as
`src/slint/tests/render_harness.cpp`), through a window adapter that owns only a renderer, so no window exists. Font
"Segoe UI", 40 px. Output of the run:

```
HEBREW  alef 20x24  gimel 15x24  three-letter ink x 1..62
  leftmost glyph:  matches alef 0.22, gimel 0.00
  rightmost glyph: matches alef 0.92, gimel 0.00
  => the first logical letter is drawn at the RIGHT: bidirectional ordering IS applied
ARABIC  one beh: ink width 30, 2 connected parts
  four beh: ink width 70, 5 connected parts
  isolated forms would be at least 120 wide with 8 parts; => letters are JOINED (shaping applied)
ALIGN left   hebrew ink x   1.. 62   latin ink x   2.. 60
ALIGN center hebrew ink x 268..329   latin ink x 270..329
ALIGN right  hebrew ink x 535..596   latin ink x 539..598
ALIGN start  hebrew ink x   1.. 62   latin ink x   2.. 60
ALIGN end    hebrew ink x 535..596   latin ink x 539..598
```

**Findings.** (1) Hebrew is reordered right to left (alef, the first logical letter, is drawn at the right). (2) Arabic
letters are joined: four BEH are 70 px wide in 5 connected parts, against at least 120 px in 8 parts for unjoined
isolated forms. So Slint 1.18.1 does bidirectional ordering and Arabic joining in the software renderer with this
font. (3) **`start` alignment did NOT follow the text's direction**: a Hebrew line aligned `start` sat at the left,
exactly like `left`; `end` sat at the right. In this setup `start` means left-to-right start whatever the script.
(4) Layout mirroring (a row of buttons running right to left, a tab order, a scrollbar on the left) is not documented
on any page opened and was not probed: **UNVERIFIED**. Slint's other renderers (femtovg, Skia) were not probed; the
shipped window's renderer should be probed the same way before relying on this.

**Hand-off for screen lane K and the i18n tables.**
1. Do not assume `start`/`end` mirror: for an RTL language set alignment explicitly from the host
   (`horizontal-alignment: end` on text that is `start` today), or have the host publish a `rtl` flag that the kit's
   `Theme` reads and uses to pick the side. Every `x:` / `alignment: start` in the kit that means "leading edge" needs this.
2. The 34 string tables (`resources/lang/*.json`, 33 languages + English) carry no RTL language. Adding Arabic (`ar`),
   Hebrew (`he`) and Persian (`fa`) means: new tables; a font that covers the script (Segoe UI did in the probe; the
   bundled fonts must be checked); numerals policy (digit policy in `access_number_format.hpp`); and a pass over the
   kit for hard-coded left/right.
3. Mixed text (a Latin frequency inside an Arabic sentence, "7.074 MHz") needs direction marks (U+200E/U+200F, or
   U+2066...U+2069 isolates) around the Latin part; the probe did not test mixed runs beyond the changelog's mention.
4. `SPAN %s`-style `formatText` templates put figures at fixed positions; in RTL the figure order is decided by the
   bidi algorithm, so the templates should be proofread in each RTL language by a speaker.

## 7. Screen-reader exposure of the canvases: what to expose (for screen lane K; no code here)

Today (verified by reading, `src/slint/ui`): spectrum and waterfall are `image` with a label only; the map is `image`
"Map" with a `list` "Legend"; the Meter is a `progress-indicator` with `accessible-value-text`; the counter is a
`groupbox` of `spinbox` digits; the Compass, the radar and demod scope tubes carry no value. A screen-reader user gets a
name and nothing that changes. Proposed, per canvas (Slint properties from the reference page opened:
`accessible-role`, `-label`, `-description`, `-value`, `-value-minimum/-maximum/-step`, `-live-region`, `-action-*`):

| Canvas | Role | Label | Value / description (updated at most once a second, only when it changes enough to speak) | Actions |
|---|---|---|---|---|
| Spectrum (per radio) | keep `image` | "Radio 2 spectrum" | description: `"Strongest signal 7.074 megahertz, 32 decibels over the noise"` from `findSpectrumPeak` (plus span and centre) | default action: tune to the peak; a key: speak now |
| Waterfall (per radio) | keep `image` | "Radio 2 waterfall" | description: span, seconds of history, strongest recent signal and its age | none |
| Frequency counter | keep `groupbox` | "Tuned frequency" | `accessible-value` on the group = `frequencyPhrase()`; mark the readout `accessible-live-region` so a change is announced by the screen reader itself | digits already have increment/decrement |
| Meter (S-meter) | keep `progress-indicator` | caption | value text = `levelPhrase()` ("S 7"); `value-minimum 0`, `-maximum 1` already set; live region **off** (a meter that talks every update is unusable) | speak-now key |
| Mode readout | `text` | "Mode" | `modePhrase()` | none |
| Map | keep `image` | "Map" | description: `"12 aircraft, nearest 34 kilometres bearing 245"` using the unit system | list of targets as `list-item`s with label `"UAL123 35000 feet heading 090"`; item action selects |
| Map legend | keep `list` | "Legend" | one `list-item` per swatch with its words | none |
| Compass / bearing rose | give it `image` | "Bearing" | value `"Bearing 245 degrees"` | none |
| Demod scope, radar scope | keep `image` | existing | description: one sentence of what the picture shows (modulation depth, sweep range) | none |
| Classic ImGui window | not a Slint canvas | n/a | needs `io.ConfigFlags \|= ImGuiConfigFlags_NavEnableKeyboard` where `app_window.cpp:1186` sets Docking; keyboard navigation then reaches ImGui widgets, but ImGui exposes no accessibility tree, so a screen reader still sees one window | n/a |

Not claimed: that any of these reaches NVDA/JAWS. Prove with a screen reader once one is installed on a machine where
installing is allowed; until then it is a design.

## 8. Wiring and screen hand-off

### 8.1 Settings and controls (screen lane K; English labels; setting keys are proposals)

| Control (English label) | Setting / op | Range / values | Default |
|---|---|---|---|
| Units | `access.units` | Automatic, Metric, Imperial, Aviation, Nautical (`unitSystemName`) | Automatic |
| Number format | `access.number_format` | "Follow Windows regional settings", "Always use a dot" | Follow Windows regional settings |
| Digits | `access.digits` | Latin (0-9), Native, As the language prefers (`DigitPolicy`) | Latin (0-9) |
| Colour set | `access.palette` | Normal, "Colour-blind safe: green-weak (deuteranopia)", "Colour-blind safe: red-weak (protanopia)", "Colour-blind safe: blue-weak (tritanopia)", High contrast (`paletteIdFromKey`) | Normal |
| Speak on request | key binding (host decides) | one chord | unset |
| Speak changes automatically | `access.speech.on_change` | on/off | off |
| Speak frequency / Speak mode / Speak signal level | `access.speech.frequency/mode/level` | on/off each | on |
| Signal level as | `access.speech.level_style` | S-units, Decibels | S-units |
| Wait between announcements | `minIntervalMs` | 0-60 s | 3 s |
| Wait for the dial to settle | `settleMs` | 0.2-5 s | 0.8 s |
| Smallest frequency change to announce | `minFreqChangeHz` | 1 Hz - 100 kHz | 100 Hz |
| Level change to announce | `levelStepDb` | 1-20 dB | 6 dB |
| Interrupt the previous announcement | `interrupt` | on/off | on |
| Speak first state on start | `announceOnStart` | on/off | off |
| Signal tone | `access.sonify.enabled` | on/off | off |
| Tone follows | `access.sonify.source` | Signal level, Strongest signal in the span | Signal level |
| Lowest / highest pitch | `minHz`, `maxHz` | 100-4000 Hz | 220 / 1760 Hz |
| Pitch scale | `logPitch` | Musical (logarithmic), Linear | Musical |
| Beeps per second (weakest / strongest) | `minRateHz`, `maxRateHz` | 0-20 | 0 / 0 (continuous) |
| Tone attack / release | `attackMs`, `releaseMs` | 0-1000 / 0-5000 ms | 30 / 250 ms |
| Tone floor / ceiling level | `LevelCurve::setLinear` | -160 to 0 dB, floor < ceiling | -120 / -20 dB |
| Pan with tuning offset | `panWidth` | 0-1 | 1 |
| Silence when the squelch is closed | `muteWhenSquelchClosed` | on/off | on |
| Maximum tone volume | `maxAmplitude` | 0-0.5 | 0.3 |

### 8.2 Wiring points (exact places)

* **Units**: replace the literals at `track_metrics.hpp:1252`, `:1273`, `:1276`, `:1378-1379` and the legend/ladder text in
  `map_view.cpp:89-103` by `quantify(units, kind, quantity, valueInBaseUnits)` + `text()`. Altitude ladder boundaries stay in
  feet internally (a band edge is a band edge); only the printed unit and number change.
* **Numbers**: `host_logic.cpp:111-120` (`fmtMHz`, `fmtKHz`, `spanText`, `cursorText`) and `:181-200` (`fmt` with a dynamic
  precision) call `formatNumber(value, decimals, symbols, policy)` with the symbols from
  `loadNumberSymbols(name, /*honourUserOverride=*/true, ...)` for "Follow Windows regional settings" or the built-in
  `NumberSymbols{}` for "Always use a dot". Input: `parseFrequencyHz` (`host_logic.cpp:130`) can switch to `parseNumber`
  (lenient); note the rule "1,234 in a locale whose group is comma is 1234" differs from its present "7,074 is 7.074":
  keep `parseFrequencyHz`'s own frequency-specific rule, use `parseNumber` for other fields.
* **Speech**: the host owns one `Announcer`. Poll it with the receiver's `SpokenState` (frequency, mode string from
  `kDemodNames`, level in dBm if calibrated else the meter's dB, squelch flag) from the UI tick with `speakIfDue`; bind
  the "speak on request" chord to `speakNow`. The real-audio `Speaker` (SAPI to the default device, or hand the
  `SapiMemorySpeaker::pcm()` to the app's own audio path) is NOT in this lane on purpose: a speaker that plays is the audio
  lane's decision. The phrase table (below) is the string list to translate.
* **Sonification**: `Sonifier::update(levelDb, squelchOpen, tuningOffset, dtMs)` once per meter tick gives `ToneParams`;
  `ToneRenderer::render` (or the audio lane's own oscillator driven by `ToneParams`) writes the stereo block. The spectrum
  variant is `sonifySpectrum(bins, n, settings)` once per spectrum frame.
* **Palettes**: lane K maps `Role` to the `Theme`/`Skin` fields (`lamp-green/-amber/-red/-yellow` <- status ok/warn/bad/info;
  `chip-*-fg`; `meter-led-low/-mid/-high`; `trace-live/-peak/-average`; `radio-1..4`, and 5..8 which the Skin lacks;
  `fg`, `muted`, `accent`, `focus`) as a new filling next to `Workbench` and `NightWatch` in `theme.slint`.
  `verifyPalette` should run on whatever table the screen lane ends up with.

### 8.3 English strings (not translated here)

Phrase table (`PhraseTable`, `{n}` is the number): `{n} hertz`, `{n} kilohertz`, `{n} megahertz`, `{n} gigahertz`, `S {n}`,
`S 9 plus {n} decibels`, `{n} decibels`, `{n} decibel milliwatts`, `level {n} decibels`, `minus`, `signal`, `no signal`,
joiner `, `; modes `narrow F M`, `wide F M`, `A M`, `double sideband`, `upper sideband`, `lower sideband`, `C W`, `raw`.
Palette labels: `Normal`, `Colour-blind safe: green-weak (deuteranopia)`, `Colour-blind safe: red-weak (protanopia)`,
`Colour-blind safe: blue-weak (tritanopia)`, `High contrast`. Unit symbols are not translated (`m km ft mi nmi m/s km/h mph kt
km/s mi/s nmi/s °C °F K hPa inHg kPa`). Control labels: the table in 8.1.

## 9. Sources opened (all read-only, 2026-11-11; nothing downloaded)

| URL | Used for | Licence / status |
|---|---|---|
| https://www.w3.org/TR/WCAG22/#dfn-relative-luminance (glossary, read through the page's own definitions) and `#contrast-minimum` | relative luminance formula and constants (0.2126/0.7152/0.0722; threshold 0.04045, noted as 0.03928 before May 2021); SC 1.4.3 4.5:1 and 3:1; SC 1.4.11 3:1 | W3C document licence (reading only; formulas are standard definitions, not copied text) |
| https://www.inf.ufrgs.br/~oliveira/pubs_files/CVD_Simulation/CVD_Simulation.html | Machado/Oliveira/Fernandes 2009 matrices, severity 1.0; page does not state the colour space | authors' page; numbers are published constants |
| https://www.nist.gov/pml/special-publication-811/nist-guide-si-appendix-b-conversion-factors/nist-guide-si-appendix-b8 | conversion factors (foot, mile, nautical mile, knot, mph, inHg, mbar, Fahrenheit) | US government work, public domain |
| https://learn.microsoft.com/en-us/windows/win32/intl/locale-sgrouping | grouping string semantics (`3;0`, `3;2;0`, `3`, `3;2`) | Microsoft Learn documentation |
| https://learn.microsoft.com/en-us/windows/win32/intl/locale-information-constants, `.../locale-nouseroverride`, `.../locale-idigitsubstitution` | the `LOCALE_*` constants used; no-user-override (Microsoft says its use is "strongly discouraged" and gives no stability guarantee: used for tests only); digit substitution values 0/1/2 | Microsoft Learn documentation |
| https://en.wikipedia.org/wiki/S_meter | IARU Region 1 R.1 values (SECONDARY source) | CC BY-SA, facts quoted not copied |
| https://docs.slint.dev/latest/docs/slint/reference/elements/text/, `.../reference/common/` (and the primitive-types redirect page) | Text alignment values, accessible properties and roles, no RTL statement | Slint documentation |
| https://github.com/slint-ui/slint/blob/master/CHANGELOG.md | the 1.19.0 RTL `elide` fix line, 1.18.0 mixed-script line | changelog text only; Slint source (GPL) was NOT read |
| `sapi.h` (Windows SDK 10.0.26100.0, local) | SAPI interfaces (`ISpVoice`, `ISpStream`, `SPF_*`) | Windows SDK header, local |

## 10. Owner decisions

1. **Which unit system is the default for a new install?** The code keeps Automatic (today's behaviour). A US install could
   default to Imperial from the OS region (`GetLocaleInfoEx LOCALE_IMEASURE`), not wired.
2. **Aviation pressure unit**: hPa (ICAO) or inHg (US)? Currently hPa; Imperial has inHg.
3. **Speak the S-meter as S-units or decibels by default?** S-units, but only for a calibrated level; the S-meter
   calibration feature is another lane's.
4. **Should the Normal skin's lamps and radio colours change?** They are unsafe under protanopia and deuteranopia; the
   safe sets exist as options only.
5. **RTL languages**: Arabic, Hebrew, Persian are possible in the text engine (section 6), but cost a font check,
   mirrored layout work in the kit and translators; say whether to start.
6. **Install a screen reader on a test machine** to prove section 7 (this lane may not).
