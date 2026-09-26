# Engine extraction, stage 1: one control path for everything

Status: **implemented on branch `engine/stage1`** (from `engine/step1`,
5bdcdab). The plan is foxsdr-api `docs/ENGINE-EXTRACTION.md` section 4,
"Stage 1"; the vocabulary is the engine API, `third_party/foxsdr_api/foxsdr_api.h`
(API 0.2, foxsdr-api commit 8fb6522, vendored unmodified).

## 1. What changed

- **`AppWindow::applyCommand(const FoxCommand&, longText)` is the one place the
  receiver's state changes.** It returns a `FoxCommandResult` (status,
  `REFUSED`/`CLAMPED`, `applied[]`, a message) exactly as the API specifies.
- **Every desktop widget, key binding and gesture that changes the receiver
  submits a command** to a local queue (`submitCommand`), drained at the top
  of every frame (`drainLocalCommands`, section 3).
- **`applyControlRequest` is a pure translation**: `net::controlRequestToCommands`
  (src/net/control_ops.cpp) turns each `ControlRequest` field into its
  command(s), in the order the fields were always applied, and each goes to
  `applyCommand`. Plugin host API controls go through
  `net::pluginControlToCommand`. CAT reaches the same code through
  `executeCatLine` -> `ControlRequest`.
- **App-internal extension ops** (`FOXAPP_OP_*`, 0x8000-0x8FFF,
  src/core/app_commands.hpp) carry desktop behaviour API 0.2 has no op for
  (section 5). Nothing in the foxsdr-api repository was edited.
- **Bookmarks have ids.** `Bookmark::id`, assigned by `FreqManager`,
  stable for the entry's lifetime in the session, kept across `updateAt`,
  never saved. `BOOKMARK_*` name an entry by id, as the API requires.
- **The long-text channel.** `FoxCommand::text` is 255 bytes (API gap 10). A
  command applied in-process may carry its whole text beside the struct
  (`QueuedCommand::longText`, e.g. an I/Q file path); an op that takes text
  reads the long text when there is one.
- **Test seams** (null in every build of the application):
  `testHooks_.pluginDir` and `testHooks_.soapyScan`.

## 2. The op table

Op -> the desktop controls converted to it -> the `ControlRequest` field(s)
that translate to it (web remote; CAT uses `centerHz`, `vfoOffsetHz`,
`mode`, `bandwidthHz`) -> plugin host API kind.

| Op | Desktop controls now submitting it | ControlRequest field | Plugin control |
|---|---|---|---|
| `RUN` | STOP/START dome; Start/Stop key; radar scope POWER; measurement hook | `running` | `Running` |
| `SET_CENTRE` | - | `centerHz` | - |
| `SET_FREQUENCY` | counter's typed entry; `FOXSDR_TUNE_HZ` seam | - | `Frequency` |
| `SET_VFO_OFFSET` | spectrum VFO drag (applied at once, section 3) | `vfoOffsetHz` | `VfoOffset` |
| `STEP_TUNE` | counter wheel and toggle switches; TUNE step keys; TUNE span keys | - | - |
| `SET_MODE` | mode buttons; eight mode keys; measurement hook | `mode` | `Mode` |
| `SET_BANDWIDTH` | - | `bandwidthHz` | `Bandwidth` |
| `SET_SQUELCH` | squelch slider; squelch keys | `squelchDb` | `Squelch` |
| `SET_VOLUME` | VOLUME dial; volume keys | `volume` | `Volume` |
| `SET_MUTED` | Mute key | - | `Muted` |
| `SET_DEEMPHASIS` | De-emph combo | `deemphasisIndex` | - |
| `SET_STEREO` | Stereo checkbox | `stereoEnabled` | - |
| `SET_NR` | Noise reduction checkbox | `nrEnabled` (+`nrStrength`) | - |
| `SET_NOTCH` | Notch checkbox | `notchEnabled` (+`notchFreqHz`+`notchQ`) | - |
| `SET_AUTO_NOTCH` | Auto notch checkbox | `autoNotch` | - |
| `SET_DISPLAY_RANGE` | - | `dbMin`+`dbMax` | - |
| `SET_BAND_PLAN` | Display > Region; SYSTEM > Country | - | - |
| `SCAN_DEVICES` | Source Refresh; patch inspector "Look for radios" | `scanDevices` | - |
| `SELECT_SOURCE` | Source list rows; IQ file Open; Pluto Open; sound card Open; `FOXSDR_SOURCE_DEVICE` seam | `sourceKind`(+`soapyArgs`) | - |
| `SET_SAMPLE_RATE` | Rate combo | `sampleRateHz` | `SampleRate` |
| `SET_GAIN` | gain sliders | `gainName`+`gainDb` | `Gain` |
| `SET_DEVICE_AGC` | Auto gain checkbox | `agc` | `DeviceAgc` |
| `SET_ANTENNA` | Antenna combo | `antenna` | - |
| `SET_BIAS_TEE` | Bias tee checkbox; deck BIAS TEE key (after its question) | - | - |
| `SET_DEVICE_OPTION` | FM notch, DAB notch, HDR, ADC dither, ADC randomiser | - | - |
| `RECORD_IQ` | Record IQ / Stop IQ | `recordIq` | - |
| `RECORD_AUDIO` | Record audio / Stop audio; Record key | `recordAudio` | - |
| `BOOKMARK_ADD` | Add current | `bookmarkAdd` | - |
| `BOOKMARK_TUNE` | bookmark row click | `bookmarkTune` (row -> id) | - |
| `BOOKMARK_REMOVE` | row x | `bookmarkRemove` (row -> id) | - |
| `BOOKMARK_FAVOURITE` | row star | - | - |
| `BOOKMARK_REMOVE_GROUP` | Remove this group | - | - |
| `SCANNER_RUN` | Start scan; Stop scan | `scannerActive` | - |
| `SCANNER_SKIP` | Skip | `scannerSkip` | - |
| `SCANNER_CONFIG` | - (the panel's fields are a form, section 4) | - | - |
| `DECODER_START` / `DECODER_STOP` | per-decoder START/STOP (rail); Fitted modules Start/Stop | - | - |
| `DECODER_STOP_ALL` | - (STOP ALL sends the keys it drew: `APP_DECODER_STOP_LIST`) | - | - |
| `PLUGIN_PRESET` | rail preset buttons; preset bars; `FOXSDR_PRESS_PRESET` seam | `pluginPresetName`+`pluginPresetIndex` | - |
| `PLUGIN_GRANT` | Fitted modules grant/revoke keys; stale-grant rows | `pluginTuneName`+`pluginTuneAllowed` | - |
| `PLUGIN_COMMAND` | Fitted modules command keys | - | - |
| `PLUGIN_MUTE` | Mute audio while running | - | - |
| `PLUGIN_RESCAN` | Fitted modules SCAN AGAIN | - | - |
| `USER_PRESET_SAVE` | "+ Save ... as a preset" (rail, preset bars) | - | - |
| `STORE_FETCH` | CHECK NOW; the store's first open | `pluginFetch` | - |
| `STORE_INSTALL` | FIT | `pluginInstall`(+`acknowledgeNotice`) | - |
| `STORE_REMOVE` | Fitted modules REMOVE | `pluginRemove` | - |
| `STORE_CANCEL` | CANCEL | - | - |
| `STORE_UPDATE_ALL` | ADD ALL / UPDATE n | - | - |
| `PATCH_RUN` | patch page START/STOP dome; `FOXSDR_PATCH_RUN_AT` seam | - | - |
| `PATCH_ALL_OFF` | ALL OFF; `FOXSDR_PATCH_ALLOFF_AT` seam | - | - |
| `TX_OPEN` / `TX_CLOSE` | Transmit page Open / Close | - | - |
| `TX_PTT` | - (the web remote's hold only; see OPEN 1) | `transmitPtt` | - |
| `TX_SET_MODE` | CW/AM/NFM/USB/LSB keys | - | - |
| `TX_SET_FREQUENCY` | MHz field (split) | - | - |
| `TX_SET_SPLIT` | SPLIT | - | - |
| `TX_SET_POWER` | TX POWER slider | - | - |
| `TX_SET_INPUT` | Microphone / Tone | - | - |
| `TX_SET_TONE` | Tone Hz | - | - |
| `TX_SET_MONITOR` | Listen while transmitting | - | - |
| `AUDIO_DEVICE` | Sinks device combo (ival0 = the PortAudio index) | - | - |
| `SET_POSITION` | Set RX here; the two offers; the satellites map's set-from-click | - | - |
| `GPS` | Read position from GPS / Stop | - | - |

Extension ops and the controls on them are in section 5. API ops the app
does not implement answer `UNSUPPORTED`: `BOOKMARK_IMPORT` (content - the
desktop imports a file, `APP_BOOKMARK_IMPORT_FILE`), `USER_PRESET_FORGET`
(the app has no preset id: `APP_USER_PRESET_FORGET_AT`), `PATCH_LOAD`,
`TX_LATCH`, `TX_REMOTE_ARM` (OPEN 1), and the settings ops `SETTING_SET`,
`PROBLEM_REPORT`, `FEATURE_REQUEST`, `TELEMETRY_ENABLE`, `SERVER_CONFIG`,
`UPDATE_CHECK` (not receiver state: section 4).

**Ids SELECT_SOURCE takes in the app:** `siggen`; `<kind>:<args>` for a
scanned device (`soapy`, or a native driver key such as `rtlsdr`), matched
against the enumerated lists and never handed to a driver verbatim;
`row:iqfile` and `row:soundcard` (the two rows that only show their panel);
`file:<path>` (the IQ file panel's Open; the path may ride in the long text);
`open-pluto:<args>` (the Pluto panel's Open - distinct from `pluto:<args>`,
which selects the Pluto ROW as the web remote always has); and
`soundcard:open` (OPEN 3).

## 3. The rule: queued, or applied at once

**Default: queued.** A widget calls `submitCommand`; the queue is drained at
the top of `drawUi`, twice:

1. first thing, before the keyboard is read - what the widgets asked for
   during the previous frame;
2. straight after `dispatchKeyBindings` - so a key still changes the receiver
   in the frame it is pressed, before anything is drawn (its documented
   behaviour).

Then the web/CAT requests and the plugin controls are applied (each
translated and applied at once, as before), then the scanner's frame driver
runs (moved here from the end of the frame: its "user wins" test must see a
widget's tune, which now lands at the top of the frame), and only then is
anything drawn. So **everything drawn in a frame reads the state every
command submitted up to the previous frame left**, and no command is ever
applied while a list the frame is walking can be rebuilt under it (this
replaces the old mid-frame `pendingPresetRequest_` / `pendingUserPresetEdit_`
safe point).

A widget edits a **copy** of the receiver's figure (a slider's float, a
checkbox's bool) and submits it; the receiver's member changes only when the
command is applied. Where the same widget greys out a neighbour from its own
value (NR/notch strength sliders, the gain sliders under Auto gain, the
network-USRP hint) it reads the copy, so the greying still changes in the
frame it is clicked.

**Exception: applied at once through `applyCommand`** - still the one path,
with no delay - when the gesture's own drawing in the same frame reads what
it sets:

- **The spectrum's VFO drag and band-edge drag** (`SET_VFO_OFFSET`,
  `APP_SET_BANDWIDTH_DRAG`): the passband the panel measures PEAK IN PASSBAND
  over and paints that frame is the one the drag just set.
- **The Source list opening** (`APP_SCAN_DEVICES_ON_OPEN`): the rows drawn
  under it are the list the scan refreshes.
- **A file dropped on the window** (`APP_BOOKMARK_IMPORT_FILE`), applied where
  the drop has always been taken (top of `drawUi`).

**Timing that did change:** a widget's command lands at the top of the next
frame instead of inside its own draw - at most one frame (16.7 ms at 60 Hz)
later. Keys and the spectrum drag are unchanged. The engine measurement's
hooks submit commands too, so its tune-to-audio latency now includes this
wait (after the frame's swap, the next frame's drain is a few ms away).

## 4. The line: receiver state, view state, forms, settings

**Receiver state** - converted: everything the engine API models as engine
state: run state, tuning, VFO, mode, bandwidth, squelch, volume, mute, audio
DSP, the display range and band plan (engine state in the API), the source
and every device setting, the converter, recording, bookmarks, the scanner's
run state, decoders, plugin grants/mutes/presets, the store, the patch
page's run state, the transmitter's settings, the audio output device, the
receiver position and the GPS read.

**View state** - stays where it is, written by widgets directly: window
positions, which page/panel/bank is open, zoom and pan of the spectrum and
waterfall and maps, splitter ratios, the theme, the counter's face, the
bookmark filter/group/favourites view, map trails and sort, the demod scope's
settings, the radar scope mode, selected targets, key-binding capture,
language.

**Forms** - fields that change nothing until a command carries or applies
them, and therefore stay as widget state: the IQ file path, the Pluto
address, the bookmark name and import path, the sound card panel's settings,
the converter's LO text, the transmitter's address, the GPS port and baud,
the typed receiver position, and **the scanner's parameter fields** (start,
stop, step, dwell, hold, resume, listen), which the Start key (`SCANNER_RUN`
carries the range) or the commit-on-deactivate (`APP_SCANNER_RANGE` with only
its reconfigure bit) applies.

**Application settings** - not receiver state, not converted in stage 1: web
and CAT listeners, telemetry, updates, diagnostics, language/country
(country submits `SET_BAND_PLAN` for the plan it implies), key bindings,
reports and feature requests, the "Keep it running" answer to the mute
dialog. Their API ops (`SERVER_CONFIG`, `TELEMETRY_ENABLE`, `UPDATE_CHECK`,
`SETTING_SET`, `PROBLEM_REPORT`, `FEATURE_REQUEST`) are for a later stage.

**Engine machinery** - not a control, stays direct: the config restore
(`applyConfig`), the scanner's own retunes (`scannerFrame`), the coalescer's
paced retunes, device-open completion and fault recovery, the audio
watchdog's reopen, the GPS fix (`pollGpsReader`), the patch page's radio
hand-over, the mute policy (`updateAudioMute`), the transmitter's per-frame
key (OPEN 1), and the frame loop's verification seams where noted.

`tests/test_command_path_guard.cpp` holds the line: rule A forbids every
state-changing token (the setters, the helpers, assignments to receiver
fields, addresses handed to ImGui) in any widget member; rule B allows the
low-level setters only in a reviewed list of engine members.

## 5. Extension ops (`FOXAPP_OP_*`), each with why API 0.2 lacks it

| Op | Used by | Why not an API 0.2 op |
|---|---|---|
| `APP_VFO_TO_ABSOLUTE` (0x8101) | click-to-tune on spectrum and waterfall; measurement hook | the snap to the mode's raster and the subtraction of the centre happen when applied; `SET_VFO_OFFSET` would need the interface to compute both from a snapshot one frame old |
| `APP_SET_VFO_OFFSET_FREE` (0x8102) | rail VFO slider | the slider is not clamped into the band; `SET_VFO_OFFSET` is (divergence kept, OPEN 4) |
| `APP_SET_BANDWIDTH_STEP` (0x8201) | Bandwidth combo | one of the steps exactly, never clamped, the combo's index set (divergence kept, OPEN 4) |
| `APP_SET_BANDWIDTH_DRAG` (0x8202) | spectrum band-edge drag | clamped, but the combo's step left alone (divergence kept, OPEN 4) |
| `APP_SET_NR_STRENGTH` (0x8203) | Strength slider; web `nrStrength` alone | `SET_NR` must also set the switch |
| `APP_SET_NOTCH_FREQUENCY` (0x8204) / `APP_SET_NOTCH_Q` (0x8205) | Freq / Q sliders; web fields alone | `SET_NOTCH` sets the switch and both values together |
| `APP_SET_DISPLAY_MIN` (0x8301) / `APP_SET_DISPLAY_MAX` (0x8302) | Min dB / Max dB sliders; web `dbMin` or `dbMax` alone | the end that MOVED yields to the minimum span; `SET_DISPLAY_RANGE` has both ends and no way to say which moved |
| `APP_SCAN_DEVICES_ON_OPEN` (0x8401) | the Source list opening | scans SoapySDR only when never (wholly) scanned; `SCAN_DEVICES` always asks |
| `APP_SET_NETWORK_USRP_SCAN` (0x8402) | "Look for network USRPs" | a scan option (with a rescan); no API op |
| `APP_SET_GAIN_NO_READBACK` (0x8403) | radar scope GAIN knob | the knob keeps its requested figure; `SET_GAIN` reads back (divergence kept, OPEN 4) |
| `APP_SET_CONVERTER` (0x8404) | converter mode, LO, quick LO keys, inversion | the API has no converter |
| `APP_SOUNDCARD_IQ_CENTRE` (0x8405) | sound card panel's I/Q centre, while an I/Q card runs | the API has no sound card source |
| `APP_BOOKMARK_IMPORT_FILE` (0x8601) | Import; a file dropped on the window | the desktop imports a PATH; `BOOKMARK_IMPORT` takes content (and content exceeds 255 bytes, gap 10) |
| `APP_SCANNER_RANGE` (0x8602) | the panel's commit while scanning (bit 8 only); web `scanStartHz`/`scanStopHz`/`scanStepHz` | stores the range without starting; `SCANNER_RUN` starts |
| `APP_USER_PRESET_APPLY` (0x8701) | user preset buttons and bar keys | the API can save and forget user presets but not press one |
| `APP_USER_PRESET_FORGET_AT` (0x8702) | x / right-click forget | the app names a user preset by (plugin, ordinal), not an id |
| `APP_PLUGIN_AUTO_PRESET` (0x8703) | opening a plugin's window from the DECODE rail | "apply the first preset unless already on one" is a decision taken when applied |
| `APP_DECODER_STOP_LIST` (0x8704) | STOP ALL (the keys it drew); the mute dialog and banner's Stop (and the mute they caused ends) | `DECODER_STOP_ALL` stops whatever runs WHEN applied, which is not what the user was shown |
| `APP_STORE_UPDATE` (0x8705) | store window UPDATE; blocked row Update | no single-plugin update op |
| `APP_STORE_REMOVE_BLOCKED` (0x8706) | blocked row Remove -> Confirm | a quarantined file is not an installed plugin |

## 6. Divergences fixed on the way

- **RUN 0 from the web, CAT or a plugin is the dome's stop** (verified): the
  only caller of `stopReceiver` is `applyCommand`'s `RUN`, which every stop
  path sends; `tests/test_stop_ends_recordings` pins that and still proves the
  web stop ends both takes end to end.
- **The mode branch** now goes through `setModeIndex` (the keys' helper - the
  bandwidth moves to the mode's default, and the diag line is logged).
- **The bookmark-tune branch** goes through `tuneToBookmark`, the desktop's
  order (frequency, then mode, then bandwidth; the web did mode, bandwidth,
  frequency).
- **The record branch** goes through `startIqRecording` (shared with the
  Record IQ key) and `startAudioRecording`.
- A web request carrying `bookmarkAdd` and `bookmarkTune`/`bookmarkRemove`
  resolved the row to an index AFTER the add had shifted the list; rows are
  now resolved to ids before anything in the request is applied.

## 7. OPEN

1. **The local transmit key is not a command.** The page's PTT (mouse or
   Space) and LATCH are rebuilt from nothing every frame and applied just
   before `Transmitter::tick()` (`drawUi`, `txPageKey`): a page not drawn
   asks for nothing, which releases both - that per-frame rebuild IS the
   dead-man's handle. The API's `TX_PTT 1` is a 2 s HOLD and `TX_LATCH` has
   a marking/re-arm protocol the app's `Transmitter` does not implement;
   expressing the local key as those ops would change what the key does
   (a release would take up to 2 s). Stage 3 owns this (the keep-alive).
   `TX_PTT` in stage 1 is the web remote's hold (`keyRemote`) only;
   `TX_LATCH` and `TX_REMOTE_ARM` answer `UNSUPPORTED`.
2. **Closing the patch page stops the patch** (view state with an engine
   consequence, `drawPatchPage`), and closing the Transmit page releases the
   remote key (`applyWebControls`). Both are engine reactions to a view
   change; an interface would have to send `PATCH_RUN 0` / withdraw consent
   itself. Left as they are.
3. **`SELECT_SOURCE "soundcard:open"` carries no settings.** It opens the card
   the Source panel's form describes. The args form (`soundCardArgs`) is lossy
   (rate `%.0f`, centre `%.3f`, `pickedFromList` not carried) and a device
   name can make it exceed 255 bytes, so it was not used; an API sound card
   id needs a lossless form.
4. **Divergences kept, not unified** (unifying any of them changes what a
   control does): the rail VFO slider is unclamped where `SET_VFO_OFFSET`
   clamps; the Bandwidth combo never clamps where `SET_BANDWIDTH` does; the
   band-edge drag leaves the combo's step where `SET_BANDWIDTH` moves it; the
   radar scope's GAIN knob keeps the requested figure where `SET_GAIN` reads
   back. Each has its own extension op so the owner can choose.
5. **Verification seams in the frame loop that stay direct**:
   `FOXSDR_PATCH_START` sets the patch running without the START key's
   switch-everything-on; `FOXSDR_PRESS_PRESET` on a plugin with NO preset
   records it started and opens its windows; the measurement's generator
   install (`installSource`). The rest of the seams submit commands.
6. **SET_DEVICE_OPTION is tested by its refusals only**: the switches exist
   on an SDRplay RSP or an RX888, and neither is built on a fake transport in
   `test_apply_command`. The switch bodies are the Source panel's own, moved.
7. **Ranges are not re-checked in `applyCommand`.** NaN/inf is refused in
   every slot; the ranges stay where they were enforced (web and CAT
   parsers, the plugin host API, the widgets' spans), so a desktop control
   keeps exactly its old reach. The engine's own range refusals belong with
   sessions (stage 5).
8. **Tiny differences accepted** (none visible in normal use): a typed
   frequency's `sourceError_` is cleared when the command is submitted rather
   than after the tune, so an RX888 rate-move sentence caused by that tune is
   no longer wiped; the scanner's Start stores its range through Hz (a
   last-ulp round trip of the MHz field); a desktop source row whose device
   vanished between the click and the next frame says "no scanned device
   matches" instead of doing nothing; two Source rows with IDENTICAL kind and
   args (the same radio listed twice) both open the first, because a row is
   named by its id rather than its position; the mute dialog's Stop ends the
   mute one frame after the dialog closes (the banner can show for that
   frame).

## 8. Tests

- `tests/test_apply_command.cpp`: one case per op `applyCommand` implements
  (every `FOXAPI_OP_*` it implements and every extension), against the real
  `AppWindow` with a fake radio, sound card, iiod transmitter daemon and a
  real fixture plugin (`tests/fixture_stage1_plugin.cpp`); a coverage check
  fails an implemented op with no case or a case for an op not implemented.
- `tests/test_control_ops.cpp`: every `ControlRequest` field translates to its
  commands (a header scan fails a field added without a row), the historical
  order, the long text, every plugin control kind, the mode numbering.
- `tests/test_command_path_guard.cpp`: rules A and B of section 4.
- `tests/test_stop_ends_recordings.cpp`: `applyCommand` is the only caller of
  `stopReceiver`; the dome, key and POWER send `RUN`.

Every one was seen red (the behaviour it names broken, the exe deleted and
relinked with a changed hash, where the test compiles what it tests) and
then green. The source-scanning tests read the tree at run time, so their
red came from the scanned source, with the exe unchanged by design.
