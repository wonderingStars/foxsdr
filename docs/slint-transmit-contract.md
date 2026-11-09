# Slint TRANSMIT page - the contract (read from the classic ImGui page at HEAD ab30fe6a)

Scope: behaviour, strings, ops and interlocks of the classic Transmit page, as the Slint page must reproduce them. No code changes.
Citations are file:line in this worktree. UNVERIFIED = not established from the files. "Classic" = the ImGui window.

Abbreviations: APP = src/gui/app_window.cpp; TX = src/gui/app_window_transmit.cpp; TP = src/gui/transmit_page.hpp; API = third_party/foxsdr_api/foxsdr_api.h;
APPC = src/core/app_commands.hpp; FEA = src/engine/fox_engine_api.cpp; ENG = src/engine/engine.cpp; ETP = src/engine/engine_tx_plugin.cpp; KR = src/core/tx_key_rules.cpp.

Colour tokens (src/gui/theme.hpp:330-361, "today" values): kInkFaint #7D7360, kInkMuted #9C9078, kIvory #EFE7D2, kAmber #F0A840, kPhosphor #8FD9A0, kBrassDark #4A4234,
kAlarm #B8552F, kAlarmHot #E07A4E. `legible(fg,bg)` = the readable text colour on that fill (used for every lit key's text).

---------------------------------------------------------------------------------------------------------------------------------------
## 0. Orientation: what exists at HEAD on the Slint side

* HEAD has NO transmit page in Slint. The only Slint transmit material: the Transmit rail row (bank 0, row 5) as a Section that only folds, its chip, and a red fault line
  (`TransmitRowState`, src/slint/ui/signal_bank.slint:1192; main.slint:603 draws `transmit-model.fault`).
* `SignalEvents.transmit-clicked()` is declared (signal_bank.slint:139) and wired in the publisher (signal_publish.cpp:902: toggles `txPageOpen_`, sends `opTransmitPage` =
  FOXAPP_OP_SET_TRANSMIT_PAGE_OPEN) but main.slint never calls it (docs/slint-port-tick.md row 1338). The page's open flag lives in the publisher (`txPageOpen_`), nothing draws it.
* `TransmitModel {chip,tone,available,armed,keyed,fault}` (signal_logic.hpp:251) is filled by `readTransmit` (signal_logic.cpp:1115): chip words NO RADIO/READY/ON AIR/FAULT via
  `transmitChip` (signal_logic.cpp:403); `armed` = REMOTE_ARMED flag; `fault` = `st.txError`. Row face for row 5 comes from it (signal_publish.cpp:812). main.cpp:609 carries a
  different default chip ("NO TX" / "READY" Good / "ON AIR" Err) - UNVERIFIED which paints when; the classic words are NO RADIO/READY/ON AIR/FAULT (TP:111-119), READY is amber (Warn), not Good.
* Inventory rows the page must satisfy: docs/slint-port-tick.md 1338-1364 (DEFERRED all, except 1339 chip+lamp PRESENT, 1356 State word RELOCATED - its note says "row 129" which is
  a mistake, the chip is row 1339); row 138 Space = "Transmit (hold)" (transmitPtt) DEFERRED ("companion screen").
* The key table already lists the binding for Settings: `{"transmitPtt","Transmit (hold, on the TRANSMIT page)","Transmit","Space"}` (src/slint/system_logic.cpp:620). The Slint root
  FocusScope (main.slint:332-366) has NO Space handling, NO key-released handler, NO PTT.
* The Slint host's session is `FOXAPI_SESSION_LOCAL | FOXAPI_SESSION_FRAME`, grants VIEW|TUNE|SETTINGS|TRANSMIT|ADMIN (main.cpp:223-225); one heartbeat per 33 ms tick then read_state
  (main.cpp:1001); there is NO FOXSDR_OPEN_TRANSMIT-style env in src/slint (grep: none).
* Results are polled in TWO places on the one session: main.cpp:1262 (8 at a time, routes to toolsBank/extendBank/patchBank, logs refusals to stderr) and PluginKit::pollResults
  (plugin_kit.cpp:339, 32 at a time, which pushes refusals to a notice list and stashes EVERYTHING in `unclaimed_`; main.cpp forwards `kit.takeUnclaimedResults()` to patchBank only).
  A TX answer (LATCH / REMOTE_ARM / PLUGIN_ARM) can therefore be consumed by either poller: the new page's answer handler must be hooked into BOTH paths or it will miss refusals.
* `PluginKit::submit` (plugin_kit.cpp:388) submits ONE command per call (`submit(session,&c,1)`), the classic submits the key as ONE batch (APP:17767). Use the table's batch submit for the key.
* WORKING TREE ONLY, NOT IN HEAD: untracked `src/slint/transmit_logic.hpp/.cpp` (mtime 2026-11-09 17:56-17:58) already exist, written by another session; this contract was NOT
  derived from them and they were not reviewed here.

---------------------------------------------------------------------------------------------------------------------------------------
## H. How the page is opened (do first: everything else assumes it)

| Item | Classic fact | Cite |
|---|---|---|
| Rail row | `benchSwitchRow(trId("Transmit###transmit"), transmitPageOpen_, tr(lampText), lampColour, lampLit=onAir, enabled=true, tooltip)`; press -> `setTransmitPageOpen(!transmitPageOpen_)`. Row lives in SIGNAL PATH bank after Sinks, before Recorder. Chip = `tr(txLampText(lamp))` (NO RADIO/READY/ON AIR/FAULT), chip/lamp colour = lamp colour (section A, "lamp"), lamp lit only when on air. | APP:16839-16872, 6425-6438 |
| Row tooltip (exact) | "Opens the transmitter: frequency, mode, power, input and the key.\nNothing here transmits until the key is held or latched, and the\npage is the only place the key exists." (tr) | APP:16865-16867 |
| Open flag | `transmitPageOpen_` (window-owned); every change sends `FOXAPP_OP_SET_TRANSMIT_PAGE_OPEN` ival[0]=0/1 via `syncTransmitPageOpen`, re-offered every frame until the session TAKES it (submit != BUSY): only a difference is sent. Sent flag `transmitPageOpenSent_` starts false. | APP:16874-16886, APP:3355, hpp:3912-3918 |
| Engine side | `transmitOpen_ = on`; if `on` and `transmitArgs_` empty -> `transmitArgs_ = transmitArgsSeed()` = deviceArgs_ when source is "pluto" and non-empty else "uri=ip:192.168.2.1". Gates NO key (stage 4). | ENG:8019-8036, 9944-9946 |
| Saved? | Saved as `transmitOpen` in config but CLEARED on every launch (`startupState`), so the page is always closed at launch. | config.cpp:1682, config.hpp:743-755, ENG:8811, 9320 |
| Stale comment | APPC:188 says "closing releases a remote key held, at once"; the engine does not (ENG:8019-8024); the release is the window's own TX_REMOTE_ARM 0 on page close (section D). | APPC:188 vs ENG:8019 |
| Env hook | `FOXSDR_OPEN_TRANSMIT` (non-empty), honoured ONLY in a bounded `--frames` run (`frames >= 0`), once (`transmitOpenedByEnv_`): calls `setTransmitPageOpen(true)`; opens a page, keys nothing. | APP:1795-1805, hpp:3590 |
| Page window | Floating cabinet page, id "Transmit###transmitwindow" (not translated), visible title `tr("TRANSMIT")`, default 560 x 640 px (FirstUseEver; was 520 until 0.99.74), default position = main viewport right edge - 560 - 36, y = 64. Close box (clears the open flag -> setTransmitPageOpen(false)), roll-up, maximise from the cabinet chrome. Drawn from `drawPluginWindows` area, INDEPENDENT of the rail bank (not hidden by switching banks). | APP:17861-17888, 15067, 10435-10686 |
| Space binding | `KeyAction::TransmitPtt`, config id "transmitPtt", default "Space"; label "Transmit (hold, on the TRANSMIT page)" group "Transmit". Skipped by the general dispatcher on purpose; the PAGE reads the chord itself. | key_bindings.cpp:73, 418; APP:19182; key_bindings.hpp:95-104 |
| Space rule | Held only if ALL: radio open (`have`), chord bound, `!io.WantTextInput` (a text field has the keyboard), the page window (or a child) is the focused ImGui window (`IsWindowFocused(RootAndChildWindows)`), `IsKeyDown(chord.key)` and Ctrl/Shift/Alt EXACTLY equal the chord's. | APP:18170-18182 |

---------------------------------------------------------------------------------------------------------------------------------------
## A. Every control, in draw order

Page-wide: all of rows 4-31 (from MHz to the notes) sit inside ONE `BeginDisabled(!have)` umbrella (APP:17974 .. 18269) except where noted. `have` = state.flags & FOXAPI_RX_TX_AVAILABLE;
`onAir` = FOXAPI_RX_TX_KEYED. ImGui shows a tooltip on a DISABLED item only when the code asked for `AllowWhenDisabled` (marked "AWD"); otherwise the tooltip is NOT shown while disabled.
Widths in px are the code's literals; "default" = ImGui's default button size (height = frame height; px UNVERIFIED). Hard `\n` are part of the tr() key - keep them.
tr()/trId(): `tr("x")` translated text; `trId("x##id")` = translated text with a language-stable id, id is the ORIGINAL label (i18n.hpp:85-92). "raw" = NOT translated.

| # | Control | Label (exact) / wrapper / id | Size | Enabled when | Look when on | Tooltip (exact) | Press sends |
|---|---|---|---|---|---|---|---|
| 1 | RADIO label | "RADIO" tr() | text | - | - | - | - |
| 2 | Address box | no label, id "##txargs" raw | width 260 | `!have` (disabled while a board is open); re-seeded each frame from fact `transmit.args` unless the field is the active item | - | none | each edit: `FOXAPP_OP_SET_TRANSMIT_ARGS` text=draft (buffer `char[128]`, cut at 127), committed as typed, every keystroke |
| 3 | Open | `trId("Open##txopen")`, shown only when `!have` | default | `!noAddress` (noAddress = draft is only spaces/tabs or empty) | - | AWD. noAddress: "Type the board's address first." else: "Connects to the board's iiod daemon and leaves it QUIET:\nmaximum attenuation, transmit oscillator down. Nothing goes\nout until the key below is held." (both tr) | `FOXAPI_OP_TX_OPEN` text=draft |
| 4 | Close | `trId("Close##txclose")`, shown only when `have` | default | `!onAir` (refused while keyed rather than silently unkeying) | - | none | `FOXAPI_OP_TX_CLOSE` (no args) |
| 5 | Radio line | have: `st.txDeviceName` raw; else if status line `transmitError` non-empty: that text, TextWrapped, kAlarmHot, through `tr()` (so the engine's "no microphone could be opened" / "audio device busy - still opening" translate, a driver error stays English); else "No transmitter is open." tr(), kInkFaint | text | - | see left | - | - |
| 6 | (separator) | | | | | | |
| 7 | Frequency field | "MHz##txfreq" raw; ImGui InputDouble, format "%.6f", step 0.001, fast step 0.1; value = `st.txFrequencyHz/1e6` (where it WOULD/DOES transmit; held at key-down while keyed) | width 180 | `have` AND `split` | - | none | each accepted edit: `FOXAPI_OP_TX_SET_FREQUENCY` num[0]=mhz*1e6 |
| 8 | SPLIT | `trId("SPLIT##txsplit")` | 80 x default | `have` | `split`: Button = kAlarm, text = legible(kIvory,kAlarm) | plain hover (NOT shown disabled): "Off: the transmitter follows the receiver's dial, so a reply\ngoes out where the call came in.\nOn: the two are unlinked - what a repeater, a pile-up and a\nsatellite each need, and the state in which you are not\nlistening where you are transmitting." | `FOXAPI_OP_TX_SET_SPLIT` num[0]=max(0, st.tunedHz), ival[0]= split ? 0 : 1 (NEW state; num carries the receiver dial so entering split never jumps) |
| 9 | Follow note | "following the receiver" tr(), kInkFaint, same line after SPLIT, shown only when `!split` | text | (inside umbrella: greyed with no radio) | - | - | - |
| 10 | Mode keys x5 | `txModeName(m)` raw: "CW","AM","NFM","USB","LSB" in that order (dsp::TxMode, kTxModeCount=5); same line | 64 x default each | `have` | `st.txMode == i`: Button kBrassDark, text legible(kPhosphor,kBrassDark) | plain hover: `tr(txModeCaption)` - CW: "A keyed carrier. The tone you hear is the receiver's, not this one's - what\ngoes out is the oscillator, switched on and off with a shaped edge so it\ndoes not click across the band." AM: "Carrier and both sidebands. The oldest mode there is, and the widest of\nthese for the voice it carries." NFM: "Narrow-band FM at 2.5 kHz deviation, with the pre-emphasis a receiver's\nde-emphasis expects. What a handheld on a repeater uses." USB: "Upper sideband: no carrier, one sideband, everything above the dial\nfrequency. The efficient way to put a voice on HF, and what is used above\n10 MHz by convention." LSB: "Lower sideband: the same thing mirrored below the dial frequency, which is\nthe convention below 10 MHz." (src/dsp/modulator.cpp:26-46) | `FOXAPI_OP_TX_SET_MODE` ival[0]=i (0 CW,1 AM,2 NFM,3 USB,4 LSB = FOXAPI_TX_MODE_*) |
| 11 | TX POWER label | "TX POWER" tr() (not "POWER": different word in most languages) | text | | | | |
| 12 | Power slider | id "##txpower" raw, SliderFloat 0..1, empty format; position t = `txPowerFraction(st.txPowerDb, quiet, loud)` (RIGHT = LOUDER; 0 dB attenuation = full output) | width 240 | `have` | - | none | on change: `FOXAPI_OP_TX_SET_POWER` num[0]=`txPowerFromFraction(t,quiet,loud)` dB (board's attenuation, <=0) |
| 13 | Power readout | `formatTxPower(power, 32, st.txPowerDb, quiet)`: "QUIET" (raw literal) when db is NaN or <= quiet+0.25, else "%.1f dB" raw | text | | | | |
| 14 | No-range note | "(the board published no range)" tr(), kAlarm, shown when `have && !(txFlags & POWER_RANGE)` | text | | | | |
|  | quiet/loud | quiet = st.txPowerMinDb, loud = st.txPowerMaxDb (engine default -89.75 / 0.0 when no sink or no range); `haveRange = have && POWER_RANGE` | | | | | |
| 15 | INPUT label | "INPUT" tr() | text | | | | |
| 16 | Input keys x2 | `trId(txInputName)`: "MIC" (index 0), "TONE" (index 1) (kTxHandInputCount=2; PLUGIN is never offered); same line as the label | 70 x default | `have` AND `!micOpening` (txFlags & INPUT_OPENING) | `st.txInput == i`: kBrassDark, text legible(kPhosphor,kBrassDark) | none | `FOXAPI_OP_TX_SET_INPUT` ival[0]=i |
| 17 | Tone Hz | "Hz##txtone" raw, InputDouble format "%.0f", step 50, fast 500, value `st.txToneHz`; shown only when `st.txInput == 1` (TONE), same line | width 110 | `have` | - | none | `FOXAPI_OP_TX_SET_TONE` num[0]=value (engine reads back, clamps) |
| 18 | AUDIO meter | label "AUDIO" tr(); ProgressBar fraction = `txMeterFraction(peak)` (dB scale over -50 dB floor: 0 for <=0 or <=-50 dB, else 1 - dB/-50); `transmitPeak_ = max(st.txInputPeak, transmitPeak_*0.90)` once per DRAWN frame (fast rise, slow fall); empty overlay | width 240 | read-only | - | none | - |
| 19 | (separator) | | | | | | |
| 20 | PTT | "PTT##txptt" RAW (not tr) | 160 x 56 | `have` | `onAir`: Button kAlarmHot, text legible(kIvory,kAlarmHot) (lit by ANY holder: hand, latch, remote, plugin) | AWD: "Hold to transmit. Let go and it stops.\nThe spacebar does the same while this page has focus, and can\nbe rebound in SYSTEM > Settings." | HOLD, not click: held = pointer pressed on the key (IsItemActive: stays true if the pointer leaves the key while still down; ends on release anywhere) OR the Space rule (row H). Sets `transmitPttHeld_` -> section D |
| 21 | LATCH | `trId("LATCH##txlatch")`, same line | 100 x 56 | `have` | `LATCHED` flag (state.flags & FOXAPI_RX_TX_LATCHED, ANY holder): Button kAlarm, text legible(kIvory,kAlarm) | AWD: "Holds the key down with no hand on it. It releases itself\nafter a minute, and any fault releases it at once." | a CLICK sets `transmitLatchPressed_` -> section D (edge protocol) |
| 22 | State word | `tr(txLampText(lamp))` in lamp colour, same line | text | | lamp: NoRadio kInkFaint; Ready kAmber; Transmitting kAlarmHot; Fault kAlarm. `txLampState(have, have && txFlags&FAULTED, onAir)`: FAULT outranks ON AIR, then NO RADIO, then ON AIR vs READY | | |
| 23 | Allow remote transmit | `trId("Allow remote transmit")` checkbox, own line; checked = `uiState_.flags & REMOTE_ARMED` | checkbox | `have` | - | AWD: "Lets the web remote's PTT key this transmitter while you are here.\nIt never latches, holds the key two seconds per press, and stops\nthe moment you untick this, close this page or the window stops." | a click sets `transmitArmRequest_ = checked ? 1 : 0` -> section D (`TX_REMOTE_ARM`) |
| 24 | Listen while transmitting | `trId("Listen while transmitting")` checkbox; checked = `txFlags & MONITOR` | checkbox | `have` | - | plain hover (not shown disabled): "The Pluto is full duplex, so the receiver keeps running while\nyou transmit. This is whether you HEAR it - off by default,\nbecause a receiver on the frequency it is transmitting on is\na howl." | click: `FOXAPI_OP_TX_SET_MONITOR` ival[0]=1/0 (NEW state) |
| 25 | Key note | `transmitKeyNote_` (engine message of the last refused LATCH/REMOTE_ARM, raw English), TextWrapped, kAmber, shown when non-empty | | | | | - (rules in D) |
| 26 | Auto-unkey reason | `st.txUnkeyReason` (why the key last opened on its own; "" when the operator opened it), raw, TextWrapped, kAmber | | | | | - |
| 27 | Transmitter error | `st.txError` raw, TextWrapped, kAlarmHot, shown when non-empty (independent of `have`) | | | | | - |
| 28 | (EndDisabled; end of the umbrella) | | | | | | |
| 29 | ARM FOR PLUGINS block | see rows 30-40; starts with a separator; NOT inside the umbrella: its keys are enabled with no radio ON PURPOSE (pressing one is how the page says why it cannot) | | | | | |
| 30 | ARM FOR PLUGINS label | "ARM FOR PLUGINS" tr() | text | | | plain hover: "Lets a plugin you have granted TRANSMIT key this radio, for\nthe time you choose. It chooses nothing else: the\nfrequency, the power and the mode are the ones on this page,\nand any fault, closing this page or the time running out\nstops it. Nothing here is remembered next time." | - |
| 31 | 5 / 15 / 60 MIN | `trId("5 MIN##txarm5")`, `"15 MIN##txarm15"`, `"60 MIN##txarm60"`; minutes from `TxArming::kChoicesMinutes = {5,15,60}`; same line as the label | 70 x default each | always | `fact tx.plugin.armed=="1" && minutes==this`: kBrassDark, text legible(kPhosphor,kBrassDark) | none | a click sets `transmitPluginArmRequest_ = minutes` -> section E: `FOXAPP_OP_TX_PLUGIN_ARM` ival[0]=minutes |
| 32 | OFF | `trId("OFF##txarmoff")`, same line | 70 x default | always | - | none | `transmitPluginArmRequest_ = 0` -> `TX_PLUGIN_ARM` ival[0]=0 ival[1]=0 |
| 33 | Armed line | armed: "ARMED: %s may key this transmitter, %s left." tr() with (fact `tx.plugin.name`, `formatTxArmCountdown(secondsLeft)` = "%d:%02d", seconds clamped >=0), TextWrapped; colour kAlarmHot if fact `tx.plugin.keyed` non-empty else kAmber. Not armed: "Not armed: no plugin can key this transmitter." tr(), kInkFaint | | | | | |
| 34 | On-air line | when armed and `tx.plugin.keyed` non-empty: "ON AIR for %s." tr() with the fact's value, kAlarmHot | | | | | |
| 35 | Refusal | fact `tx.plugin.refusal` int != 0: `tr(txArmRefusalText(n))`, kAlarmHot, TextWrapped (texts in section F) | | | | | |
| 36 | Close reason | when NOT armed and fact `tx.plugin.reason` non-empty: the text raw, kAmber, TextWrapped | | | | | |
| 37 | DATA (flat) | `trId("DATA (flat)")` checkbox; checked = fact `tx.plugin.dataFlat != "0"` (default on when the fact is missing) | checkbox | always | - | plain hover: "A plugin's audio goes out as NFM with the pre-emphasis off, whatever\nmode is selected above: a data modem's tones must reach the far\nreceiver flat. Untick it to send the plugin's audio in the mode\nselected above instead. The microphone and the tone are never\naffected." | click: `FOXAPP_OP_TX_DATA_FLAT` ival[0]=1/0 (NEW state) |
| 38 | (separator) | | | | | | |
| 39 | Licence notice | `tr(kTxLicenceNotice)` TextWrapped, kInkMuted: "This transmits. An ADALM-Pluto puts out about +7 dBm - roughly five milliwatts - and whether you may radiate it on the frequency you have set is your responsibility and your country's licensing authority's, not this software's." | | | | | |

Cites: rows 1-27 APP:17900-18268; 29-37 TX:46-140; 39 APP:18280-18283, TP:44-47; formatTxPower transmit_page.cpp:12-28; txMeterFraction transmit_page.cpp:30-38; txPowerFraction/FromFraction TP:67-84.
Not read by the classic page (do not invent controls for them): `txSplitHz`, `txHoldRemainingMs`, `txLatchRemainingMs`, `FOXAPI_RX_TX_LATCH_RELEASE_FIRST` (the API says "show it on the latch key"; the classic does not).
Census names (bounded-run test seam) the classic notes: `transmit:arm5|arm15|arm60` (rects), `transmit:armoff`, `transmit:armed|notarmed`, `transmit:refusal` (+n), `transmit:dataflat:on|off` (+rect) (TX:74-85, 110, 128-132).

---------------------------------------------------------------------------------------------------------------------------------------
## B. Every fact / state field the page reads

| Source | Field | Used for |
|---|---|---|
| state.flags | FOXAPI_RX_TX_AVAILABLE 0x10000 (`have`), TX_KEYED 0x20000 (`onAir`), TX_LATCHED 0x40000, TX_KEY_MINE 0x80000 (THIS session holds the key), TX_REMOTE_ARMED 0x400000 | gating, lamp, PTT/LATCH look, checkbox, key protocol (`latchedShown`, `latchMine`) |
| state.txFlags | FOXAPI_TX_SPLIT 1, MONITOR 2, FAULTED 4, INPUT_OPENING 8, POWER_RANGE 0x10 | SPLIT look/MHz enable, Listen checkbox, lamp FAULT, MIC/TONE disable, "(the board published no range)" |
| state | txMode, txInput, txFrequencyHz, txPowerDb, txPowerMinDb, txPowerMaxDb, txToneHz, txInputPeak (measurement; destructive read in the engine publish), txDeviceName, txError, txUnkeyReason, tunedHz | as in section A |
| engine meaning | txFrequencyHz = where it would transmit (receiver dial when SPLIT off) or, while keyed with a sink, the frequency actually held; txPowerDb = the board's readback once open | ENG:4718-4730 |
| fact `transmit.args` | LOCAL only. The address the engine uses: the one typed, else the seed | address box |
| fact `status.transmitError` | status line (level 3); the classic reads it as `statusLines_.transmitError` | radio line when no radio |
| facts `tx.plugin.armed` ("1"/"0"), `.minutes` (""/n), `.secondsLeft` (""/n), `.name` (first granted loaded TX plugin's display name when armed), `.keyed` (name of the plugin on air or ""), `.refusal` (int of TxArming::Refusal: 0 None,1 NoRadio,2 NoGrant,3 BadMinutes), `.reason` (last close reason unless it was "you pressed OFF"), `.dataFlat` | all LOCAL only | ARM block |
| KeyBindings | `keyBindings_[TransmitPtt]` chord (key, ctrl, shift, alt) | Space rule |
| window-local | `transmitArgsDraft_[128]`, `transmitPeak_`, `transmitKeyNote_`, `transmitKey_` (TxKeyMemory), `transmitPluginArm_` (TxPluginArmMemory), page-open flags | |

Engine facts the page does NOT read: `fact.plugin.<key>.tx` bits (Fitted modules plate only, ETP:428).

---------------------------------------------------------------------------------------------------------------------------------------
## C. Every op sent, who may send it, and the engine's possible answers

Common to ALL ops below (FEA:1670-1685, 1396-1410): grant TRANSMIT needed -> else `DENIED` ("this session does not hold the grant for that"). A REMOTE session may send only `TX_PTT`
(every other TRANSMIT op `DENIED`, "a remote session may only press and release the PTT"); all `FOXAPP_OP_*` are LOCAL-only. The Slint session is LOCAL with TRANSMIT: may send all.
Submit-time statuses (the submit call itself): `OK` (ticket), `NO_CHANGE` (a SAFER command merged into the one of its kind this session already has outstanding: no new ticket, the
ticket's one result answers both), `BUSY` (ordinary command, session already has 256 pending; not taken, no ticket). Every judgement of the command itself arrives as a RESULT
(poll_results); a refused result has FOXAPI_RESULT_REFUSED set exactly when status != OK. SAFER kinds (never BUSY, merge): TX_PTT 0, TX_LATCH 0, RUN 0, TX_CLOSE, TX_REMOTE_ARM 0,
TX_PLUGIN_ARM 0 and -1 (FEA:1353-1369, API:498-518). A non-safer command from a session that closed/detached before apply -> `DETACHED`.

| Op | Args | Sender | Engine answers (status; message; applied) | Cite |
|---|---|---|---|---|
| FOXAPI_OP_TX_OPEN 0x0A01 | text=address | LOCAL+TRANSMIT | `OK` (radio open, QUIET only if saved power is the quiet end - see surprises); `BAD_ARGUMENT` "no transmitter address given" (empty/space-tab only text); `FAILED` "the transmitter did not open" (and `status.transmitError` / `txError` get the driver's sentence). Ends any key ("the transmitter was changed"); that a changed radio also closes the plugins' arming window is pinned by tests/test_engine_api_txplugin.cpp:813 but the code path was not traced here (UNVERIFIED). With no hardware this attempts a real Pluto/iiod connection to the typed address (duration UNVERIFIED). | ENG:8000-8014, 5428-5470 |
| FOXAPI_OP_TX_CLOSE 0x0A02 | - | same | `OK` always (also with none open); submit may answer `NO_CHANGE` (merged). Ends the key ("the transmitter was closed") and closes the plugin window ("the transmit radio was closed"). | ENG:8015-8018, 5471-5480; FEA:1361 |
| FOXAPI_OP_TX_PTT 0x0A03 | ival[0] 1 hold / 0 release | LOCAL, and REMOTE (only op a remote may send) | 1: submit-judged refusal `NO_DEVICE` "there is no transmitter open" (state shows no radio); `DENIED` remote without consent "the local operator has not given consent to remote transmit"; at apply `BUSY` "another interface holds the key"; `OK` applied[0]=1 (hold bought: 2000 ms). 0: always `OK` (safer, merges). Hold = FOXAPI_PTT_HOLD_MS 2000. | FEA:1750-1760, 1865-1892; KR:key() |
| FOXAPI_OP_TX_LATCH 0x0A04 | ival[0] 1/0 | LOCAL only | 1: `DENIED` non-local; `NO_DEVICE` (no radio, submit-judged); apply: `BUSY` "another interface holds the key"; `NO_CHANGE` "already latched; the latch keeps its original deadline" (applied[0]=ms left); `DENIED` re-arm "no latch closes within 1000 ms of the last one ending (<reason>)" (applied[0]=ms left); `DENIED` mark "the latch ended (<reason>): this interface must send LATCH 0 first"; `OK` applied[0]=1. 0: always `OK` (safer). Reasons seen: "released by its owner (LATCH 0)", "released from another interface of the same operator (LATCH 0)", "the latch timed out", "the PTT was not re-asserted within the hold", "the interface holding the key stopped answering", "the interface holding the key closed its session", "the transmitter was changed|closed" etc. | FEA:1779-1790; KR:95-191 |
| FOXAPI_OP_TX_REMOTE_ARM 0x0A0C | ival[0] 1/0 | LOCAL only | `OK` applied[0]=resulting consent (1/0); 0 is safer/merges and ends a remote-held key ("the local operator withdrew consent to remote transmit"); `DENIED` remote. Consent lapses if this session closes ("the local interface that gave consent to remote transmit closed") or stops answering >1 s ("... stopped answering"). | KR:193-206, 207-262 |
| FOXAPI_OP_TX_SET_MODE 0x0A05 | ival[0] 0..4 | LOCAL | `OUT_OF_RANGE` at submit if outside 0..4; `OK` (recorded with or without a radio) | FEA:1744; ENG:8067 |
| FOXAPI_OP_TX_SET_FREQUENCY 0x0A06 | num[0] Hz | LOCAL | submit: `OUT_OF_RANGE` unless 0 < Hz <= 6e9 (NaN -> `BAD_ARGUMENT`); apply: `BUSY` "the transmitter is keyed: its frequency is held until the key is released" (nothing stored); else `OK` (stored as the split frequency; no retune without a radio). The classic does NOT show a BUSY here (see surprises). | FEA:1746; ENG:8074-8083 |
| FOXAPI_OP_TX_SET_SPLIT 0x0A09 | ival[0] 0/1, num[0] Hz | LOCAL | `BUSY` same sentence while keyed; else `OK`. | ENG:8084-8093 |
| FOXAPI_OP_TX_SET_POWER 0x0A07 | num[0] dB | LOCAL | submit: `OUT_OF_RANGE` unless -89.75 <= dB <= 0; `OK` applied[0] = the board's readback (the requested value with no sink). A board with a range wider than the validator's cannot be driven beyond it. | FEA:1748; ENG:8094-8099 |
| FOXAPI_OP_TX_SET_INPUT 0x0A08 | ival[0] 0 MIC / 1 TONE | LOCAL | `OUT_OF_RANGE` unless 0/1 (txInputFromIndex would land on TONE anyway); `BUSY` "the microphone is still opening"; `OK`. Choosing MIC with no radio STILL opens the system default microphone on a worker (bounded) and may set txError "no microphone could be opened" or "audio device busy - still opening". | ENG:8100-8122 |
| FOXAPI_OP_TX_SET_TONE 0x0A0A | num[0] Hz | LOCAL | `OK` applied[0] = clamped readback; no validator range. | ENG:8123-8128 |
| FOXAPI_OP_TX_SET_MONITOR 0x0A0B | ival[0] 0/1 | LOCAL | `OK` | ENG:8129-8131 |
| FOXAPP_OP_SET_TRANSMIT_ARGS 0x8419 | text | LOCAL | `OK`; `FAILED` "the transmitter is open" if a sink is open. | ENG:8310-8317 |
| FOXAPP_OP_SET_TRANSMIT_PAGE_OPEN 0x8408 | ival[0] 0/1 | LOCAL | `OK`; seeds the address when opening (section H). | ENG:8019-8036 |
| FOXAPP_OP_TX_PLUGIN_ARM 0x842B | ival[0] 5/15/60 arm, 0 close, -1 page live; ival[1] 0 "you pressed OFF" / 1 "the TRANSMIT page was closed" | LOCAL only; never applied by applyCommand (needs the sender) | submit: `OUT_OF_RANGE` if ival[0] not in {-1,0,5,15,60} or ival[1] not 0/1. Apply: `DENIED` remote; `DETACHED` for a non-OFF from a closed session. -1: `OK` (stamps the lease) or `NO_CHANGE` if no window is open or this session does not own it. 0: `OK` applied[0]=0 (closes the window if open; clears the published refusal). Arm: `NO_DEVICE` "not armed: no transmitter is open" (applied[0]=1; also when the sink has faulted), `NOT_FOUND` "not armed: no plugin has been granted TRANSMIT" (applied[0]=2), `OUT_OF_RANGE` "not armed: that is not one of the offered times" (applied[0]=3, unreachable from the keys), else `OK` applied[0]=0 (clears any plugin fault; owner = this session). | FEA:1761-1768, 1894-1908; ETP:161-216 |
| FOXAPP_OP_TX_DATA_FLAT 0x842C | ival[0] 0/1 | LOCAL | `OUT_OF_RANGE` unless 0/1; `DENIED` remote; `OK` applied[0]=state. Session-only, never saved. | FEA:1777; ENG:8049-8056 |

Not sent by the page (do not send): FOXAPP_OP_PLUGIN_TX_GRANT (Fitted modules plate), FOXAPI_OP_SET_FREQUENCY (the receiver).
Answer handling in the classic (APP:17796-17838): ONLY `TX_LATCH` and `TX_REMOTE_ARM` results touch the page (`transmitKeyNote_`, rules in D). `TX_PLUGIN_ARM` results are ignored (the refusal comes back as the fact `tx.plugin.refusal`).

---------------------------------------------------------------------------------------------------------------------------------------
## D. The key protocol (PTT, LATCH, remote consent, page-live)

D1. Engine rules the page is subject to (API:619-690, tx_key_rules.hpp, transmitter.hpp):
* PTT 1 is a 2 s HOLD (FOXAPI_PTT_HOLD_MS 2000): the interface re-asserts while the hand is down; silence opens the key ("the PTT was not re-asserted within the hold"). PTT 0 opens it now.
* LATCH: LOCAL sessions only; self-releases 60 s after it closed (FOXAPI_LATCH_TIMEOUT_MS; the Transmitter keeps an independent 60 s deadline `kLatchTimeout`); a LATCH 1 while latched changes nothing
  (NO_CHANGE); after ANY latch ends no latch closes for 1000 ms (FOXAPI_LATCH_REARM_MS); a latch ending by anything but its owner's own LATCH 0 MARKS the operator: LATCH 1 from any local session is
  refused until THAT session sends LATCH 0. A LATCH 0 from a non-owner while latched ends the latch AND marks.
* One key holder at a time (BUSY "another interface holds the key").
* Keep-alive: a local session's heartbeat older than 1000 ms (FOXAPI_KEEPALIVE_MS) loses the key; remote 2000 ms. Only heartbeat() counts; re-asserting the PTT is not a heartbeat. The TX thread also
  watches two stamps (control tick, key holder's heartbeat) and opens the key itself within `kKeyAliveWait` = 1000 ms; frozen window or wedged control thread both release.
* The key never survives a stop (RUN 0), a fault, a transmitter change/close, and is never restored from config.
* Frequency is FROZEN while keyed (SET_FREQUENCY / SET_SPLIT BUSY); the transmitter follows the dial again once the key is up (core/transmitter.hpp:306-325).
* Remote PTT: needs TX_REMOTE_ARM 1 from a LOCAL session; the remote key holds 2 s per press (`kRemotePttHoldMs`), never latches. Consent lapses when withdrawn, the consenting session closes or stops answering.

D2. Per-frame rebuild (the classic's shape; the Slint page must have the same property: a request is REBUILT FROM NOTHING every tick, never remembered):
1. First act of each frame: heartbeat (APP:1595). Then drawUi clears `transmitPttHeld_`, `transmitPageLive_`, `transmitLatchPressed_`, `transmitArmRequest_=-1`, `transmitPluginArmRequest_=-2` (APP:3367-3371).
2. Answers polled, state read (the "look", APP:3372+), `actOnUiAnswers` handles TX_LATCH / TX_REMOTE_ARM results (APP:17799-17804).
3. The page draws. `transmitPageLive_ = true` only AFTER `beginPage` returns true: i.e. page open AND not rolled up/collapsed AND the window is visible AND well >= 8 px (APP:17883-17892, 10662-10672).
   A page that is not open, is rolled up, or is not drawn sets NOTHING.
4. If drawn: `transmitPttHeld_ = true` iff `have && (mouseHeld || keyHeld)`; `transmitLatchPressed_ = true` on a LATCH click; `transmitArmRequest_` = 1/0 on a checkbox click (APP:18182-18228).
5. After all pages: `submitTransmitKey()` (APP:3464) -> ONE batch via `engineApi_->submit(session, batch, n)` (APP:17755-17768), then `submitTransmitPluginArming()` (APP:3467).

D3. `txKeyCommands` (TP:167-211) - exact; inputs `TxKeyFrame{pageLive,pttHeld,latchPressed,armRequest,latchedShown,latchMine}`, memory `TxKeyMemory{pttSent,armedByUs,pageWasLive,latchPending}`,
max 6 commands. `latchedShown` = state LATCHED flag; `latchMine` = latchedShown && KEY_MINE.
* If `latchedShown && latchMine`: `latchPending = false` (it landed).
* pageLive:
  * pttHeld -> `TX_PTT 1` (every frame it is held), `pttSent = true`; else if `pttSent` -> `TX_PTT 0` once, `pttSent = false`.
  * latchPressed -> `TX_LATCH 0`; then if `!latchedShown` -> `TX_LATCH 1`, `latchPending = true`; else (shown latched) `latchPending = false`. (Released-then-pressed rule: a press on a key shown RELEASED sends
    LATCH 0 then LATCH 1 in ONE batch; a press on a key shown LATCHED sends LATCH 0 only. Never level-triggered; the key is never re-sent while held.)
  * armRequest >= 0 -> `TX_REMOTE_ARM armRequest`; `armedByUs = (armRequest == 1)`.
* not pageLive but `pageWasLive` (one-shot, on the first frame it is gone): if `pttSent` -> `TX_PTT 0`; if `latchMine || (latchPending && !othersLatch)` -> `TX_LATCH 0` (`othersLatch = latchedShown && !latchMine`: a
  different window's latch is left alone); if `armedByUs` -> `TX_REMOTE_ARM 0`; then reset pttSent/armedByUs/latchPending.
* `pageWasLive = pageLive` every call. Nothing is sent by a page that was never live.
* Also: `if (!pageLive && memory.pageWasLive) transmitKeyNote_.clear()` (APP:17764).
* Note the PTT is NOT dropped when `have` goes false mid-hold: `pttHeld` just stops being set (it requires `have`), so the next frame sends `TX_PTT 0` once (pttSent).

D4. Remote-arm consent: page-driven checkbox, OFF by default each time the page opens (state comes from the engine flag; `armedByUs` memory starts false), cleared by: unticking, page not live, session stopping answering,
session closing, engine key-rule ends. A REMOTE PTT needs it (`TX_PTT 1` from a remote session is refused DENIED without it).

D5. The note (`transmitKeyNote_`): set to the engine's raw `message` when a `TX_LATCH` or `TX_REMOTE_ARM` result has status not OK and not NO_CHANGE (APP:17799-17801); cleared when a `TX_LATCH` result is OK
(APP:17802-17804) - so the LATCH 0 that every press sends first clears an old note, then a refused LATCH 1 sets the new one - and cleared when the page stops being live. A `NO_CHANGE` neither sets nor clears.
PTT refusals (NO_DEVICE / BUSY) are NOT lettered (surprise 3).

D6. What happens when the page closes / rolls up / loses focus - every interlock the classic enforces:
| Event | Classic effect |
|---|---|
| Page closed (close box / rail row off / `setTransmitPageOpen(false)`) | not drawn -> not live -> next frame batch: PTT 0 (if sent), LATCH 0 (if mine or pending and nobody else's), REMOTE_ARM 0 (if by us); plugin arming: `TX_PLUGIN_ARM 0,1` if a window is shown armed (E). Key note cleared. |
| Page rolled up (cabinet collapse) | `beginPage` false -> same as closed (page still "open" in the flag but not live). |
| Rail bank switched | NO effect (the floating page is independent of banks); the stale comments at APP:16824-16832 and 3360-3363 say it releases the key - they are wrong for the floating page. |
| Radio closes / key ends by engine | `have` false -> PTT not set -> PTT 0 once; all Transmit controls greyed (umbrella). |
| Window loses OS focus | UNVERIFIED in the classic code: nothing in APP handles it; Space requires ImGui window focus (so Space stops being "down" for the page when focus moves); a mouse hold relies on ImGui clearing pointer state on focus loss (UNVERIFIED). Backstops that DO hold: engine PTT hold 2 s, keep-alive 1 s. |
| Space held while a text field has focus | `WantTextInput` -> not a PTT. |
| Window frozen / process stalls | engine opens the key within 1 s (keep-alive) and the remote consent lapses ("... stopped answering"); plugin lease lapses (E). |
| Chord rebound | read live from `keyBindings_` each frame; unbound -> no Space PTT. |
| Mouse held on PTT then dragged off the key | still held (IsItemActive) until release anywhere. |
SLINT NOTE (requirement, not classic behaviour): Slint is event-driven (press/release events), the classic polls physical state every frame. A lost release event (focus loss, page hidden,
modal over it, window minimised, drag-off) would be re-asserted forever by a naive per-tick "held" flag. The held flag MUST be cleared on key-released, pointer-up, pointer-exit-after-up,
focus-out, page hidden/rolled/closed, radio gone, and each tick must re-derive it. Engine backstop only covers a stalled window, not a live tick re-asserting a stale flag.

---------------------------------------------------------------------------------------------------------------------------------------
## E. ARM FOR PLUGINS lease protocol (TP:213-257, TX:142-154, ETP)

`txPluginArmCommands(TxPluginArmFrame{pageLive, pressed(-2 none/0 OFF/5/15/60), armedShown (fact tx.plugin.armed=="1"), nowS}, memory{armSentAtS=-1e9}, out[1])` -> at most ONE command per frame, op
`FOXAPP_OP_TX_PLUGIN_ARM`, `ival[0]=v0`, `ival[1]=v1`:
1. `pressed >= 0`: v0 = pressed (0 = OFF, ival[1] = 0 "you pressed OFF"); a nonzero press sets `armSentAtS = nowS`. Sent ALONE, whether or not the page is live.
2. else if `pageLive`: if `!armedShown && nowS - armSentAtS >= 3.0` (kTxPluginArmHoldS) -> send NOTHING; else send `-1` ("still here") - EVERY FRAME while armed, and for 3 s after an ARM was sent
   (before the fact that it opened has come back).
3. else (page not live): if `armedShown` -> `v0 = 0, v1 = 1` (OFF, "the TRANSMIT page was closed"); else nothing.
`nowS` = ImGui::GetTime() (monotonic seconds). Nothing is sent when no window is open and none was asked for.
Engine lease (ETP:248-259, 161-179): the window belongs to the SESSION that armed it; `-1` stamps `txArmLiveMs_` only from the owner of an open window (else `NO_CHANGE`). The window CLOSES when: no sink
("the transmit radio was closed"), sink faulted ("the radio faulted"), transmitter stopped itself ("the transmitter stopped on its own"), plugin fault (its reason), lease lost ("the window that armed this was closed" /
"the window stopped responding" if the owner's heartbeat is older than 1000 ms / "the TRANSMIT page was closed" if no `-1` or ARM stamp within 1000 ms), control tick stale ("the transmit control stopped responding"),
last grant gone ("no plugin holds the TRANSMIT grant any more"), the keyed plugin gone ("the plugin that held the key stopped (dormant, stopped, unloaded or rebuilt)"), timer ("the arming window ran out"),
OFF ("you pressed OFF"). Arm 5/15/60 min only; session-only, never saved. Plugin key conditions (pluginMayKey): grant + armed + pageLive + tick fresh + plugin active + radio ok + hands off (no PTT/latch/remote) + no fault.
Cap: a plugin's key is never held longer than 60 s per carrier; hand keys take the audio source (one source).
=> The Slint page MUST send the `-1` beat at least once per second while a window is open (lease 1000 ms; the classic sends one per frame) and OFF/1 the moment the page stops being live.

---------------------------------------------------------------------------------------------------------------------------------------
## F. Every user-visible string (exact)

Labels (tr unless marked raw): "RADIO"; "Open"; "Close"; "No transmitter is open."; "MHz" (raw); "SPLIT"; "following the receiver"; mode names "CW" "AM" "NFM" "USB" "LSB" (raw); "TX POWER"; "QUIET" (raw);
"%.1f dB" (raw); "(the board published no range)"; "INPUT"; "MIC"; "TONE"; "Hz" (raw); "AUDIO"; "PTT" (raw); "LATCH"; "Allow remote transmit"; "Listen while transmitting"; "ARM FOR PLUGINS"; "5 MIN"; "15 MIN";
"60 MIN"; "OFF"; "DATA (flat)"; page title "TRANSMIT"; rail row "Transmit"; lamp words "NO RADIO" "READY" "ON AIR" "FAULT" (TP:111-119).
Tooltips: exact text in section A (rows 3, 8, 10, 20, 21, 23, 24, 30, 37, rail-row tooltip in H).
Notes and sentences:
* "Not armed: no plugin can key this transmitter."
* "ARMED: %s may key this transmitter, %s left."   (name, "m:ss")
* "ON AIR for %s."
* txArmRefusalText (TP:278-290): 1 "Not armed: no transmitter is open. Open the radio above first."; 2 "Not armed: no plugin has been granted TRANSMIT. Grant one on its page in Fitted modules first."; 3 "Not armed: that is not one of the offered times."
* `fact.tx.plugin.reason` (raw, amber, English): "the arming window ran out", "the TRANSMIT page was closed", "the transmit radio was closed", "the radio faulted", "the transmitter stopped on its own",
  "the window that armed this was closed", "the window stopped responding", "the transmit control stopped responding", "no plugin holds the TRANSMIT grant any more", "the plugin that held the key stopped (dormant, stopped, unloaded or rebuilt)", plus plugin fault reasons (cap etc.); never "you pressed OFF" (suppressed).
* Engine result messages shown raw in the key note (all English, from KR): "there is no transmitter open", "another interface holds the key", "the local operator has not given consent to remote transmit", "already latched; the latch keeps its original deadline" (NO_CHANGE: not shown), "no latch closes within 1000 ms of the last one ending (<reason>)", "the latch ended (<reason>): this interface must send LATCH 0 first", "the latch is a hands-on control and cannot be closed remotely", "this session does not hold the grant for that", "the session closed before this command was applied".
* Auto-unkey reasons (`txUnkeyReason`, raw): "the PTT was not re-asserted within the hold", "the latch timed out", "the interface holding the key stopped answering", "the interface holding the key closed its session", "the login of the interface holding the key was revoked", "the transmitter was changed", "the transmitter was closed", "the transmitter stopped on its own", "the radio would not key: <err>", faults/stop sentences (engine-owned; full set UNVERIFIED).
* Engine transmit errors (statusLine transmitError / txError): "no microphone could be opened", "audio device busy - still opening" (both pass through tr() on the radio line), a driver's own text (English).
* Licence notice: quoted in A row 39.

---------------------------------------------------------------------------------------------------------------------------------------
## G. Test seams (no transmitter hardware) and unattended-send audit

G1. Answers the engine gives with NO radio open (real product engine, LOCAL session with TRANSMIT):
* `TX_PTT 1`, `TX_LATCH 1`: refused result `NO_DEVICE` "there is no transmitter open" (judged at submit against the published state) - tests/test_engine_api_transmit.cpp:66-78 (`testNoTransmitterNoKey`). PTT 0 / LATCH 0: `OK`.
* `TX_PLUGIN_ARM 5|15|60`: `NO_DEVICE` "not armed: no transmitter is open", applied[0]=1, and the fact `tx.plugin.refusal` becomes "1" (tests/test_engine_api_txplugin.cpp:504-523); with a radio and no grant `NOT_FOUND` applied[0]=2.
* `TX_OPEN ""`: `BAD_ARGUMENT`; `TX_OPEN <unreachable address>`: `FAILED` + driver text on `txError` (a real network connection attempt; not RF).
* `TX_SET_MODE/_FREQUENCY/_SPLIT/_POWER/_TONE/_MONITOR/DATA_FLAT/SET_TRANSMIT_ARGS/SET_TRANSMIT_PAGE_OPEN`: `OK` and recorded; `TX_CLOSE`: `OK`; `TX_SET_INPUT 0`: `OK` and opens the default microphone.
* The state with no radio: no AVAILABLE flag; txFrequencyHz = receiver dial (or split frequency); txPowerDb = saved/default -89.75; range -89.75..0 without POWER_RANGE.
G2. Dummy load: `TX_OPEN text="test:tx:dummy"` works ONLY in an engine created through the table with option `testHooks=1` (FEA:327-377, 537; ENG:5439-5447). Neither the classic window nor the Slint host
sets it (grep: only tests do), so a real-exe test cannot open a radio without hardware; those tests are C++ tests against the table.
G3. Tests pinning the classic behaviour (assertions as far as read; others by header only):
* tests/test_transmit_page.cpp (989 lines): 1 transmit frequency (split/follow), 2 power direction (left=quiet, 0 dB=full), 3 readout ("QUIET", "%.1f dB"), 4 lamp (fault outranks on-air), 5 meter dB scale, 6 the licence sentence, 7 THE KEY: `txKeyCommands` frame by frame
  (PTT re-assert, LATCH edge protocol, consent, page-away release), 8 wiring read FROM THE SOURCE of app_window.cpp/engine.cpp/fox_engine_api.cpp (one writer of the key in the API pass; `submitTransmitKey` called once in drawUi after pages; `transmitPageLive_ = true` only after beginPage; heartbeat first act of run()), 9 plugin sentences, 10 the plugin key wiring order.
* tests/test_tx_audio_page_app.cpp (295 lines; real cascade.exe, `--frames`, FOXSDR_OPEN_TRANSMIT=1, FOXSDR_UI_CENSUS, FOXSDR_INPUT_SCRIPT): DRAW (arm5/15/60, armoff, dataflat:on, notarmed present; no refusal; no "tx: armed" in the log), REFUSE (click 5 MIN with no radio -> census `transmit:refusal1`, still notarmed; control run 3 frames earlier shows no refusal), TICK (DATA (flat) click toggles off, two clicks back on). Nothing transmits; scratch config.
* tests/test_tx_audio_app.cpp (859 lines): the engine behind the page's own commands (`tx_front_end.hpp` FrontEnd + `gui::txPluginArmCommands`): refusals (NoRadio, NoGrant, 0/1/10/30/61/120/-5/65 BadMinutes; 5/15/60 accepted; "tx: armed N min", "tx: disarmed (you pressed OFF)"), quiet-unless-everything-holds, keys/unkeys, every close reason, cap, hand precedence, config (grant saved, window not).
* tests/test_engine_api_transmit.cpp (key rules through the table, dummy load): PTT hold, latch rules/marks/re-arm, safer commands never BUSY, merging, key protocol (`testLatchKeyProtocol`), remote consent, keep-alive, session close.
* tests/test_engine_api_txplugin.cpp (lease, facts, refusals, dead-man), test_tx_key_rules.cpp (R1..R8 on a turned clock), test_transmit_dead_man.cpp / test_transmit_key_race.cpp / test_transmit_liveness.cpp / test_remote_key_release.cpp / test_tx_window_threads.cpp / test_transmitter.cpp / test_engine_api_tx_frequency_hold.cpp (names per headers), test_tx_audio_plate_app.cpp (GRANT TRANSMIT plate).
* Seams in the classic window usable by tests: `uiSubmitTap_` records every submitted command (APP:16956), `uiAnswerTap_` sees every answer (APP:17798), `AppWindowTestAccess`. The Slint test pattern: builder functions returning `Command` with the grant (signal_logic.hpp:428, test_slint_host_signal.cpp:998) over a stub table (`TableReader`).
G4. Can a TX op be sent without an explicit key press? Audit of the classic (and what the Slint page must keep):
* TX_PTT 1: only while a pointer/Space hold is observed this frame AND radio open AND page live. Space auto-repeat is irrelevant (IsKeyDown, not key events). Per-frame while held.
* TX_LATCH 1: only on the frame of a LATCH click on a radio shown open; never sent level-wise, never on page open, never from config.
* TX_REMOTE_ARM 1: only on a checkbox click; consent is never restored (config has no field). TX_REMOTE_ARM 0 / PTT 0 / LATCH 0 / PLUGIN_ARM 0 are sent unprompted on page-away, by design (safer).
* TX_PLUGIN_ARM 5/15/60: only on a key click. `-1` is sent unprompted every frame while a window is open (renews the lease; it opens nothing).
* TX_OPEN: only on the Open button (disabled when the box is blank). FOXSDR_OPEN_TRANSMIT opens the PAGE only.
* TX_SET_INPUT 0 (MIC) opens a real microphone with no radio and no key (surprise 5). TX_SET_SPLIT / TX_SET_FREQUENCY / TX_SET_POWER retune/re-level a radio but do not key it.
* Flag: `FOXAPP_OP_SET_TRANSMIT_PAGE_OPEN` is re-offered every frame until taken (harmless, not keying).
* Flag: a power change on an open radio takes effect on the NEXT key, and a radio opened with a SAVED non-quiet power is not quiet (surprise 4).

---------------------------------------------------------------------------------------------------------------------------------------
## Surprises / UNVERIFIED (also reported to the caller)

1. Stale comments in APP:3360-3363 / 16824-16832 claim bank switching releases the key; the page is a floating window independent of banks. Close and roll-up do release.
2. Slint session polls results in TWO places (main.cpp:1262 and plugin_kit.cpp:339); TX answers must be claimed from both. `PluginKit::submit` is single-command; the key needs a batch.
3. The classic does NOT letter refusals of TX_PTT (NO_DEVICE / BUSY), TX_SET_FREQUENCY/SET_SPLIT BUSY while keyed, TX_SET_INPUT BUSY, TX_OPEN FAILED (only via `txError`), TX_PLUGIN_ARM (only via the refusal fact). Only LATCH / REMOTE_ARM refusals are lettered.
4. Open tooltip says the radio is left at "maximum attenuation"; the engine writes the SAVED power (`transmitPowerDb_`, default -89.75, persisted) before installing the sink (ENG:5455-5461), so a saved louder setting survives. Tooltip text is a tr() key: change it only with the catalogues.
5. TX_SET_INPUT 0 with no radio open opens the system microphone.
6. Tooltips on SPLIT, mode keys and Listen are plain-hover, so they are NOT shown while no radio is open (they sit in the disabled umbrella); PTT, LATCH, Allow remote, Open use AllowWhenDisabled and ARE.
7. Chip words differ at HEAD: main.cpp:609 "NO TX"/Good READY vs classic/logic "NO RADIO"/amber READY; docs row 1356 cites row 129 wrongly.
8. UNVERIFIED: button default heights and the 560x640 layout px in Slint terms; window-focus-loss behaviour of a mouse-held PTT in ImGui; whether the classic frame loop runs while minimised; duration of TX_OPEN against an unreachable address; full set of engine auto-unkey sentences; translations exist for every key quoted (assumed by signal_logic.hpp's header).
9. Untracked `src/slint/transmit_logic.{hpp,cpp}` appeared in this worktree during the read (not in HEAD, not reviewed).
